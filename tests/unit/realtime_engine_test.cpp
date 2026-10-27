#include "voice_engine/realtime.hpp"
#include <algorithm>
#include <chrono>
#include <deque>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
using namespace voice_engine;
namespace {
void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
bool same(AudioContext a, AudioContext b) { return a.session_id == b.session_id && a.generation_id == b.generation_id; }
AudioFrame frame(AudioContext context, std::uint64_t seq = 1, std::size_t valid = 320) {
    AudioFrame f; f.context = context; f.seq = seq; f.monotonic_ns = 1000000000;
    f.samples.assign(320, 0.25f); std::fill(f.samples.begin() + valid, f.samples.end(), 0.0f);
    f.valid_samples = valid; return f;
}
// Model-free transport doubles only: assertions exercise the production
// controller, never claim real VAD/ASR/NPU timing or acoustic acceptance.
struct Audio final : LiveAudioBackend {
    LiveAudioStatus value;
    std::deque<AudioFrame> capture;
    std::vector<AudioFrame> playback;
    std::vector<AudioContext> starts;
    bool full = false, hold_stop = false, hold_reset = false, reject_start = false;
    bool drain_requested = false, disconnect_at_drain_commit = false;
    int interrupts = 0;
    int stops = 0, cancels = 0, drains = 0, attempts = 0, reads = 0;
    Audio() { value.state = LiveAudioState::ready; }
    bool start_session(AudioContext c, bool recording) override {
        if (reject_start) return false;
        starts.push_back(c); capture.clear(); value.context = c;
        value.session_active = true; value.capturing = recording;
        value.capture_stopping = false; value.state = LiveAudioState::capturing;
        value.drained = true; drain_requested = false; return true;
    }
    bool interrupt_playback(AudioContext next) override {
        if (reject_start || !value.capturing || value.reset_pending ||
            !same({next.session_id, value.context.generation_id}, value.context) ||
            next.generation_id <= value.context.generation_id) return false;
        ++interrupts; value.context = next;
        for (auto& f : capture) f.context = next;
        value.reset_pending = hold_reset; value.drained = true; drain_requested = false;
        return true;
    }
    void stop_capture() override {
        ++stops; value.capturing = false; value.capture_stopping = hold_stop;
        if (value.state != LiveAudioState::device_wait) value.state = LiveAudioState::ready;
    }
    void cancel() override {
        ++cancels; capture.clear(); value.session_active = false;
        value.capturing = value.capture_stopping = false; value.drained = true;
        value.reset_pending = hold_reset; drain_requested = false;
        if (value.state != LiveAudioState::device_wait) value.state = LiveAudioState::ready;
    }
    bool try_capture_frame(AudioFrame& f) override {
        ++reads; if (capture.empty()) return false;
        f = capture.front(); capture.pop_front(); return true;
    }
    bool try_submit_playback(const AudioFrame& f) override {
        ++attempts;
        if (full || !value.session_active || !same(f.context, value.context)) return false;
        playback.push_back(f); value.drained = false; return true;
    }
    void request_drain() override { ++drains; drain_requested = true; value.drained = false; }
    bool commit_if_drained(AudioContext c, const std::function<bool()>& commit) override {
        // Simulate loss after the controller observes drained but before the
        // backend's atomic guard. A flush also reports drained=true.
        if (disconnect_at_drain_commit) { value.state = LiveAudioState::device_wait; cancel(); }
        if (!drain_requested || !value.drained || value.reset_pending || !value.session_active ||
            value.state == LiveAudioState::device_wait || !same(c, value.context)) return false;
        return commit();
    }
    bool playback_drained() const override { return value.drained; }
    LiveAudioStatus status() const override {
        auto s = value; s.capture_queued_frames = capture.size(); return s;
    }
    std::vector<LiveAudioEvent> poll_events() override { return {}; }
};
struct Pipeline final : RealtimePipeline {
    AudioContext current;
    bool busy = false, valid = false, reject_submit = false, acknowledge_cancel = false;
    bool invalidate_at_commit = false, reject_history = false;
    bool capture_admission = true, close_admission_after_submit = false;
    int wake_begins = 0, barge_begins = 0;
    bool barge_active = false;
    std::deque<BargeInSignal> barge_events;
    std::vector<AudioFrame> barge_frames;
    void begin_barge_in(AudioContext c) override {
        require(same(c, current), "foreign barge-in context"); barge_active = true; ++barge_begins;
    }
    void end_barge_in() override { barge_active = false; }
    bool can_accept_barge_in() const override { return barge_active; }
    bool submit_barge_in(const AudioFrame& f) override { barge_frames.push_back(f); return barge_active; }
    std::optional<BargeInSignal> poll_barge_in() override {
        if (barge_events.empty()) return {};
        auto e = barge_events.front(); barge_events.pop_front(); return e;
    }
    void hit(AudioContext c) { barge_events.push_back({c, 10, barge_frames.empty() ? 0 : barge_frames.back().seq, 640, ""}); }
    int begins = 0, overlapping_begins = 0, finishes = 0, cancels = 0, sessions = 0;
    int history = 0, history_attempts = 0, polls = 0, atomic_commits = 0;
    std::vector<AudioFrame> accepted;
    std::deque<RealtimeEvent> events;
    void begin(AudioContext c) override {
        if (busy) ++overlapping_begins;
        current = c; busy = valid = true; ++begins;
    }
    void begin_wake(AudioContext c) override {
        require(!busy, "KWS overlapped active model work");
        current = c; busy = valid = true; ++wake_begins;
    }
    bool can_accept_capture() const override { return capture_admission; }
    bool submit(const AudioFrame& f) override {
        if (reject_submit) return false;
        require(same(f.context, current), "controller delivered foreign capture context");
        accepted.push_back(f);
        if (close_admission_after_submit) capture_admission = false;
        return true;
    }
    void finish_input() override { ++finishes; }
    void cancel() override { ++cancels; valid = false; barge_active = false; if (acknowledge_cancel) busy = false; }
    void new_session() override { ++sessions; history = 0; }
    bool idle() const override { return !busy; }
    std::optional<RealtimeEvent> poll() override {
        ++polls; if (events.empty()) return std::nullopt;
        auto e = events.front(); events.pop_front(); return e;
    }
    bool commit_if_current(AudioContext c, const std::function<void()>& commit) override {
        if (invalidate_at_commit) valid = false;
        if (!valid || !same(current, c)) return false;
        ++atomic_commits; commit(); return true;
    }
    bool commit_history(AudioContext c) override {
        ++history_attempts;
        if (reject_history || !valid || busy || !same(current, c)) return false;
        ++history; return true;
    }
    void emit(RealtimeEvent::Kind kind, AudioContext c, const std::string& reason = "") {
        RealtimeEvent e; e.kind = kind; e.context = c; e.reason = reason;
        e.data = {{"end_reason", reason}}; e.frame = frame(c, events.size() + 1);
        events.push_back(e);
    }
};
struct Rig {
    Config config;
    Audio audio;
    Pipeline pipeline;
    struct Log { std::string name; nlohmann::json data; };
    std::vector<Log> logs;
    RealtimeController controller;
    std::int64_t now = 1000000000, memory = 4 * 1024 * 1024;
    static Config settings(bool wake, bool barge) { Config c; c.dialogue.wake_enabled = wake; c.dialogue.barge_in = barge; return c; }
    explicit Rig(bool wake = false, bool barge = false) : config(settings(wake, barge)), controller(config, audio, pipeline, [this](std::string_view name, AudioContext, RealtimeState, const nlohmann::json& data) {
        logs.push_back({std::string(name), data});
    }) { tick(); }
    void tick(std::int64_t milliseconds = 1) { now += milliseconds * 1000000; controller.tick(now, memory); }
    bool command(const std::string& c) { return controller.command(c, now); }
    AudioContext start() {
        require(command("start"), "start rejected"); tick();
        require(controller.snapshot().state == RealtimeState::listening, "start did not reach listening");
        return controller.snapshot().context;
    }
    bool reason(const std::string& why) const {
        return std::any_of(logs.begin(), logs.end(), [&](const Log& row) { return row.data.value("reason", "") == why; });
    }
    void settle_cancel() { pipeline.busy = false; audio.value.reset_pending = false; tick(); }
    void response(AudioContext c, bool empty = false) {
        if (controller.snapshot().state == RealtimeState::listening)
            pipeline.emit(RealtimeEvent::Kind::endpoint, c, "vad_silence");
        pipeline.emit(RealtimeEvent::Kind::asr_final, c, empty ? "empty" : "recognized");
        if (!empty) pipeline.emit(RealtimeEvent::Kind::pcm, c);
        pipeline.emit(RealtimeEvent::Kind::complete, c); pipeline.busy = false; tick();
    }
};
void barge_interrupt_under_backpressure() {
    Rig r(false, true); const auto old = r.start();
    r.pipeline.emit(RealtimeEvent::Kind::endpoint, old);
    r.pipeline.emit(RealtimeEvent::Kind::asr_final, old, "recognized");
    r.pipeline.emit(RealtimeEvent::Kind::pcm, old); r.tick();
    require(r.audio.value.capturing && r.pipeline.barge_begins == 1, "response stopped insertion capture");
    r.audio.playback.clear(); r.audio.full = true;
    r.pipeline.emit(RealtimeEvent::Kind::pcm, old); r.tick();
    r.audio.capture.push_back(frame(old, 101)); r.audio.capture.push_back(frame(old, 102, 111)); r.tick();
    r.audio.capture.push_back(frame(old, 103));
    r.audio.hold_reset = true; r.pipeline.hit(old); r.tick();
    const auto next = r.controller.snapshot().context;
    require(r.reason("barge_in") && r.audio.interrupts == 1 && r.audio.cancels == 0,
            "blocked PCM hid insertion or insertion flushed capture");
    require(next.session_id == old.session_id && next.generation_id > old.generation_id, "wrong insertion context");
    require(r.pipeline.begins == 1 && r.pipeline.history == 0, "new ASR overlapped old NPU or committed cancelled history");
    r.pipeline.busy = false; r.tick();
    require(r.pipeline.begins == 1, "ASR skipped playback flush barrier");
    r.pipeline.emit(RealtimeEvent::Kind::pcm, old);
    r.pipeline.emit(RealtimeEvent::Kind::complete, old);
    r.audio.value.reset_pending = false; r.tick();
    require(r.pipeline.begins == 2 && r.pipeline.overlapping_begins == 0, "barge did not recover");
    require(r.pipeline.accepted.size() == 3, "insertion prefix or pending continuation lost");
    for (std::size_t n = 0; n < 3; ++n) {
        const auto& f = r.pipeline.accepted[n];
        require(f.seq == 101 + n && same(f.context, next), "prefix reordered or not retagged");
    }
    require(r.pipeline.accepted[1].valid_samples == 111 && r.audio.playback.empty(),
            "valid prefix tail changed or stale PCM was played");
}
void barge_prefix_bound_and_overflow() {
    Rig r(false, true); const auto c = r.start();
    r.pipeline.emit(RealtimeEvent::Kind::endpoint, c);
    r.pipeline.emit(RealtimeEvent::Kind::asr_final, c, "recognized");
    r.pipeline.emit(RealtimeEvent::Kind::pcm, c); r.tick();
    for (int batch = 0; batch < 3; ++batch) {
        for (int n = 0; n < 20; ++n) r.audio.capture.push_back(frame(c, batch * 20 + n + 1));
        r.tick();
    }
    const auto b = realtime_queue_budget(r.config);
    require(b.capture_preroll_frames == 44 && b.capture_audio_frames == 55 &&
        b.capture_preroll_frames + b.capture_audio_frames + b.capture_model_frames == 100,
        "barge increased total capture budget");
    r.pipeline.hit(c); r.tick();
    ++r.audio.value.capture_overflows; r.tick();
    require(r.reason("pending_capture_overflow") && !r.controller.snapshot().pending_start &&
        r.pipeline.begins == 1, "overflow silently admitted a truncated insertion");
    bool rejected = false;
    auto small = r.config; small.queues.capture_ms = 880;
    try { realtime_queue_budget(small); } catch (const ConfigError&) { rejected = true; }
    require(rejected, "pre-roll consumed all transport capacity");
}
void barge_stale_failure_and_deadline() {
    Rig r(false, true); const auto c = r.start();
    r.pipeline.emit(RealtimeEvent::Kind::endpoint, c);
    r.pipeline.emit(RealtimeEvent::Kind::asr_final, c, "recognized");
    r.pipeline.emit(RealtimeEvent::Kind::pcm, c); r.tick();
    r.pipeline.hit({c.session_id, c.generation_id - 1}); r.tick();
    require(r.audio.interrupts == 0, "stale detector cancelled new response");
    r.audio.capture.push_back(frame(c)); r.tick();
    r.pipeline.hit(c); r.tick(); r.tick(2000);
    require(r.controller.snapshot().exit_code == 5 && r.pipeline.begins == 1,
        "barge cancellation timeout admitted new NPU");
    Rig failed(false, true); const auto f = failed.start();
    failed.pipeline.emit(RealtimeEvent::Kind::endpoint, f);
    failed.pipeline.emit(RealtimeEvent::Kind::asr_final, f, "recognized");
    failed.pipeline.emit(RealtimeEvent::Kind::pcm, f); failed.tick();
    failed.pipeline.barge_events.push_back({f, 0, 0, 0, "barge_vad_call_timeout"}); failed.tick();
    require(failed.reason("barge_vad_call_timeout"), "detector error ignored");
}
void barge_waits_for_accepted_playback() {
    Rig r(false, true); const auto c = r.start();
    r.pipeline.emit(RealtimeEvent::Kind::endpoint, c);
    r.pipeline.emit(RealtimeEvent::Kind::asr_final, c, "recognized"); r.tick();
    for (int batch = 0; batch < 3; ++batch) {
        for (int n = 0; n < 20; ++n) r.audio.capture.push_back(frame(c, batch * 20 + n));
        r.tick();
    }
    require(r.pipeline.barge_begins == 0 && r.pipeline.barge_frames.empty() &&
        r.audio.capture.empty() && r.pipeline.cancels == 0, "pre-playback noise reached detector or accumulated");
    r.audio.full = true; r.pipeline.emit(RealtimeEvent::Kind::pcm, c); r.tick();
    require(r.pipeline.barge_begins == 0, "unaccepted PCM enabled interruption");
    r.audio.full = false; r.tick();
    require(r.pipeline.barge_begins == 1, "accepted output did not arm interruption");
    r.audio.capture.push_back(frame(c, 80)); r.tick();
    require(r.pipeline.barge_frames.size() == 1 && r.pipeline.barge_frames.back().seq == 80,
        "pre-output capture leaked into new detector");
    r.pipeline.hit(c); r.tick();
    require(r.reason("barge_in"), "playback insertion disabled by pre-output guard");
}
void barge_normal_drain_and_manual_stop() {
    Rig r(true, true);
    const auto c = r.controller.snapshot().context;
    r.pipeline.emit(RealtimeEvent::Kind::wake, c); r.pipeline.busy = false; r.tick();
    r.response(c);
    r.audio.capture.push_back(frame(c, 61)); r.tick();
    r.audio.value.drained = true; r.tick(); r.tick();
    require(r.pipeline.history == 1 && r.pipeline.begins == 2 && r.audio.interrupts == 1,
        "normal drain failed to commit/reopen with preserved input");
    require(r.pipeline.accepted.back().seq == 61 &&
        r.controller.snapshot().context.session_id == c.session_id,
        "response-boundary input or history lost");
    const auto next = r.controller.snapshot().context;
    r.audio.capture.push_back(frame(next, 62));
    r.pipeline.capture_admission = false;
    r.command("stop"); r.tick();
    require(r.pipeline.finishes == 0, "manual endpoint discarded queued input");
    r.pipeline.capture_admission = true; r.tick();
    require(r.pipeline.accepted.back().seq == 62, "manual endpoint lost capture tail");
    require(r.audio.value.capturing && r.pipeline.finishes > 0, "manual endpoint disabled future insertion capture");
}
void wake_handoff_window_and_session() {
    Rig r(true);
    require(r.controller.snapshot().wake_listening && r.pipeline.begins == 0, "idle must run only KWS");
    const auto c = r.controller.snapshot().context;
    const auto starts = r.audio.starts.size();
    r.pipeline.capture_admission = false; // recognizer closed admission after hit
    r.audio.capture.push_back(frame(c, 77));
    r.pipeline.emit(RealtimeEvent::Kind::wake, c);
    r.tick();
    require(r.pipeline.begins == 0, "wake must wait for KWS idle");
    r.pipeline.busy = false; r.tick();
    require(r.controller.snapshot().state == RealtimeState::listening, "wake did not begin question");
    require(r.audio.starts.size() == starts && r.audio.capture.size() == 1, "wake handoff flushed capture");
    r.pipeline.capture_admission = true; r.tick();
    require(r.pipeline.accepted.back().seq == 77, "post-trigger first frame lost");
    r.response(c); r.audio.value.drained = true; r.tick();
    require(r.pipeline.history == 1 && r.pipeline.begins == 2, "drain did not reopen dialogue");
    require(r.controller.snapshot().context.session_id == c.session_id, "continuation lost session");
    const auto next = r.controller.snapshot().context;
    require(next.generation_id > c.generation_id, "continuation reused old generation");
    require(r.pipeline.wake_begins == 1, "KWS ran during dialogue/response");
    r.response(next, true); r.audio.value.drained = true; r.tick(); r.tick();
    require(r.controller.snapshot().wake_listening && r.pipeline.history == 0, "silence did not return to fresh wake session");
    require(r.controller.snapshot().context.session_id > c.session_id, "new wake reused previous session");
}
void wake_cancel_device_memory_and_stale() {
    Rig r(true); const auto old = r.controller.snapshot().context;
    r.command("cancel"); r.pipeline.emit(RealtimeEvent::Kind::wake, old); r.settle_cancel();
    require(r.controller.snapshot().wake_listening && r.pipeline.begins == 0, "late wake started ASR");
    r.tick(); require(r.pipeline.begins == 0, "stale wake crossed generation");
    r.audio.value.state = LiveAudioState::device_wait; r.tick(); r.settle_cancel();
    require(!r.controller.snapshot().wake_listening &&
        r.controller.snapshot().state == RealtimeState::device_wait, "disconnected KWS kept running");
    r.audio.value.state = LiveAudioState::ready; r.tick();
    require(r.controller.snapshot().wake_listening, "reconnected KWS did not reset");
    r.memory = -1; r.tick(); r.settle_cancel();
    require(!r.controller.snapshot().wake_listening && r.controller.snapshot().state == RealtimeState::error,
        "KWS ignored unavailable memory");
    r.memory = 4 * 1024 * 1024; r.tick();
    require(r.controller.snapshot().wake_listening, "KWS memory recovery");
    r.command("quit"); r.settle_cancel();
    require(r.controller.snapshot().stopping && !r.controller.snapshot().wake_listening, "quit rearmed KWS");
}
void wake_fault_requires_explicit_recovery() {
    Rig r(true); const auto id = r.controller.snapshot().context;
    r.pipeline.emit(RealtimeEvent::Kind::error, id, "kws_call_timeout");
    r.tick(); r.settle_cancel();
    const auto attempts = r.pipeline.wake_begins;
    for (int n=0; n<5; ++n) r.tick(1000);
    require(r.controller.snapshot().state == RealtimeState::error &&
            r.pipeline.wake_begins == attempts, "failed detector automatically restarted");
    r.command("new-session"); r.settle_cancel();
    require(r.controller.snapshot().wake_listening &&
            r.pipeline.wake_begins == attempts + 1, "explicit detector recovery failed");
}
void manual_start_and_overflow_during_wake() {
    Rig r(true);
    require(r.command("start"), "manual start rejected while monitoring");
    require(r.controller.snapshot().state == RealtimeState::cancelling && r.pipeline.begins == 0,
        "manual start skipped KWS cancellation");
    r.settle_cancel();
    require(r.controller.snapshot().state == RealtimeState::listening && r.pipeline.overlapping_begins == 0,
        "manual start overlapped KWS");
    Rig overflow(true); ++overflow.audio.value.capture_overflows; overflow.tick();
    require(overflow.reason("wake_capture_discontinuity"), "KWS silently lost capture");
}
void shared_queue_budget_boundaries() {
    for (const int capture_ms : {40, 60, 80, 2000, 10000}) {
        for (const int playback_ms : {60, 80, 100, 2000, 10000}) {
            Config c; c.vad.pre_roll_ms = 0;
            c.queues.capture_ms = capture_ms; c.queues.playback_ms = playback_ms;
            const auto b = realtime_queue_budget(c);
            require(b.capture_audio_frames > 0 && b.capture_model_frames == 1,
                    "capture budget lost producer capacity or allowed multiple model slots");
            require((b.capture_audio_frames + b.capture_model_frames) * 20 == static_cast<std::size_t>(capture_ms),
                    "capture stage queue capacities exceed or lose the configured total budget");
            require(b.playback_audio_frames > 0 && b.playback_tts_frames > 0 && b.playback_retained_frames == 1,
                    "playback budget omitted a live stage or retained more than one controller frame");
            require((b.playback_audio_frames + b.playback_tts_frames + b.playback_retained_frames) * 20 ==
                    static_cast<std::size_t>(playback_ms), "playback stages each allocated the full total budget");
        }
    }
    const auto minimum = [] {
        Config c; c.vad.pre_roll_ms = 0; c.queues.capture_ms = 40; c.queues.playback_ms = 60;
        return realtime_queue_budget(c);
    }();
    require(minimum.capture_audio_frames == 1 && minimum.playback_audio_frames == 1 &&
            minimum.playback_tts_frames == 1, "smallest realtime budgets did not leave one frame per stage");
    for (const auto& values : std::vector<std::pair<int, int>>{{20, 60}, {0, 60}, {-20, 60}, {41, 60},
                                                           {40, 20}, {40, 40}, {40, 0}, {40, -20}, {40, 61}}) {
        Config c; c.vad.pre_roll_ms = 0; c.queues.capture_ms = values.first; c.queues.playback_ms = values.second;
        bool rejected = false; try { (void)realtime_queue_budget(c); } catch (const ConfigError&) { rejected = true; }
        require(rejected, "undersized or non-frame-aligned realtime queue budget accepted");
    }
    for (const int frame_ms : {0, 10, 40}) {
        Config c; c.audio.frame_ms = frame_ms;
        bool rejected = false; try { (void)realtime_queue_budget(c); } catch (const ConfigError&) { rejected = true; }
        require(rejected, "unsupported transport quantum accepted by queue allocator");
    }
}
void capture_admission_preserves_audio_queue() {
    Rig r; const auto c = r.start(); r.pipeline.capture_admission = false;
    r.audio.capture.push_back(frame(c, 71)); r.audio.capture.push_back(frame(c, 72, 137));
    const auto reads = r.audio.reads;
    r.tick(); r.command("stop"); r.tick();
    require(r.audio.reads == reads && r.audio.capture.size() == 2 && r.pipeline.accepted.empty() &&
            r.pipeline.cancels == 0 && r.pipeline.finishes == 0,
            "saturated model queue consumed/dropped audio or finalized before remaining tail");
    r.pipeline.capture_admission = true; r.pipeline.close_admission_after_submit = true; r.tick();
    require(r.pipeline.accepted.size() == 1 && r.audio.capture.size() == 1 && r.pipeline.finishes == 0,
            "controller ignored admission changing after one nonblocking submit");
    r.pipeline.capture_admission = true; r.pipeline.close_admission_after_submit = false; r.tick();
    require(r.pipeline.accepted.size() == 2 && r.pipeline.accepted[0].seq == 71 &&
            r.pipeline.accepted[1].seq == 72 && r.pipeline.accepted[1].valid_samples == 137 &&
            r.pipeline.finishes == 1, "capture backpressure lost/reordered samples or EOF");
    Rig overflow; const auto oc = overflow.start(); overflow.pipeline.capture_admission = false;
    overflow.audio.capture.push_back(frame(oc)); ++overflow.audio.value.capture_overflows; overflow.tick();
    require(overflow.reason("audio_capture_discontinuity") && overflow.pipeline.accepted.empty() &&
            overflow.audio.capture.empty(), "audio-side overflow under model backpressure was silently accepted");
}
void normal_and_stop_tail() {
    Rig r; const auto c = r.start();
    r.audio.capture.push_back(frame(c)); r.tick();
    r.audio.hold_stop = true; require(r.command("stop"), "manual stop rejected"); r.tick();
    require(r.pipeline.finishes == 0, "ASR finalized before capture cork/tail acknowledgement");
    r.audio.capture.push_back(frame(c, 2, 137)); r.audio.value.capture_stopping = false; r.tick();
    require(r.pipeline.finishes == 1 && r.pipeline.accepted.size() == 2 &&
            r.pipeline.accepted.back().valid_samples == 137, "manual stop lost or padded valid tail");
    r.response(c);
    require(r.audio.playback.size() == 1 && r.audio.drains == 1 && r.pipeline.history == 0,
            "response did not wait for actual normal drain");
    r.tick(); require(r.pipeline.history == 0, "history committed while playback pending");
    r.audio.value.drained = true; r.audio.value.reset_pending = true; r.tick();
    require(r.pipeline.history == 0, "history committed before outstanding flush acknowledgement");
    r.audio.value.reset_pending = false; r.tick();
    require(r.pipeline.history == 1 && r.controller.snapshot().state == RealtimeState::idle,
            "completed playback did not commit exactly one history pair");
    r.tick(); require(r.pipeline.history == 1, "idle duplicated history commit");
    require(r.command("quit"), "quit rejected"); r.tick();
    require(r.controller.snapshot().stopping && r.controller.snapshot().exit_code == 0,
            "quit did not wait for and finish cancellation");
}
void endpoint_reasons_and_empty_input() {
    for (const std::string reason : {"vad_silence", "start_timeout", "max_speech"}) {
        Rig r; const auto c = r.start();
        if (reason == "max_speech") {
            // Exactly 15 s of 20 ms transport frames; the model double emits
            // its endpoint. Real VAD duration detection is tested separately.
            for (int block = 0; block < 15; ++block) {
                for (int i = 0; i < 50; ++i) r.audio.capture.push_back(frame(c, block * 50 + i));
                r.tick(1000);
            }
            require(r.pipeline.accepted.size() == 750, "15 second transport lost frames");
        }
        r.pipeline.emit(RealtimeEvent::Kind::endpoint, c, reason); r.tick();
        require(r.controller.snapshot().state == RealtimeState::finalizing && r.audio.stops == 1,
                "automatic endpoint did not stop capture");
        const bool empty = reason == "start_timeout";
        r.response(c, empty); r.audio.value.drained = true; r.tick();
        require(r.controller.snapshot().state == RealtimeState::idle && r.pipeline.history == (empty ? 0 : 1),
                "empty/speech endpoint history policy incorrect");
        require(r.audio.playback.size() == (empty ? 0u : 1u), "silent turn emitted PCM");
    }
}
void replacement_waits_for_both_barriers() {
    Rig r; const auto old = r.start(); r.audio.hold_reset = true;
    require(r.command("start") && r.command("start"), "replacement start rejected");
    require(r.controller.snapshot().pending_start, "replacement did not retain one pending start");
    r.pipeline.emit(RealtimeEvent::Kind::pcm, old);
    r.pipeline.emit(RealtimeEvent::Kind::complete, old);
    r.tick(); require(r.pipeline.begins == 1, "replacement overlapped active model");
    r.pipeline.busy = false; r.tick();
    require(r.pipeline.begins == 1, "replacement began before audio flush acknowledgement");
    r.audio.value.reset_pending = false; r.tick();
    const auto fresh = r.controller.snapshot().context;
    require(r.pipeline.begins == 2 && r.pipeline.overlapping_begins == 0 && !same(old, fresh),
            "replacement did not begin once with a fresh generation");
    require(r.audio.playback.empty() && r.audio.drains == 0 && r.pipeline.history_attempts == 0,
            "late cancelled events submitted PCM/drain/history");
    r.audio.capture.push_back(frame(old)); r.audio.capture.push_back(frame(fresh, 2)); r.tick();
    require(r.pipeline.accepted.size() == 1 && same(r.pipeline.accepted[0].context, fresh),
            "stale capture crossed into replacement generation");
}
void replacement_preserves_waiting_capture_ring() {
    Rig r; const auto old = r.start(); r.audio.hold_reset = true;
    r.command("start"); const auto pending = r.controller.snapshot().context;
    require(r.audio.starts.size() == 2 && r.audio.value.capturing && !same(old, pending),
            "replacement did not start a fresh capture ring while old model was cancelling");
    r.audio.capture.push_back(frame(pending, 41)); r.audio.capture.push_back(frame(pending, 42, 137));
    r.tick(); require(r.pipeline.accepted.empty() && r.pipeline.begins == 1,
                      "waiting capture reached ASR before old model released NPU");
    r.pipeline.busy = false; r.tick();
    require(r.pipeline.begins == 1 && r.audio.capture.size() == 2, "pending ring was consumed before flush acknowledgement");
    r.audio.value.reset_pending = false; r.tick();
    require(r.audio.starts.size() == 2 && r.pipeline.begins == 2 && r.pipeline.accepted.size() == 2 &&
            r.pipeline.accepted.front().seq == 41 && r.pipeline.accepted.back().seq == 42 &&
            r.pipeline.accepted.back().valid_samples == 137 && same(r.controller.snapshot().context, pending),
            "worker admission restarted capture or lost waiting ring first/tail samples");
    Rig latest; latest.start(); latest.command("start");
    const auto replaced = latest.controller.snapshot().context;
    latest.audio.capture.push_back(frame(replaced, 9)); latest.command("start");
    const auto newest = latest.controller.snapshot().context;
    require(!same(newest, replaced) && latest.audio.capture.empty(), "repeated start retained superseded waiting ring");
    latest.audio.capture.push_back(frame(newest, 10)); latest.settle_cancel();
    require(latest.pipeline.accepted.size() == 1 && latest.pipeline.accepted.front().seq == 10 &&
            latest.audio.starts.size() == 3 && latest.pipeline.overlapping_begins == 0,
            "repeated start did not preserve only newest prebuffer");
}
void pending_stop_and_cancel_deadline() {
    Rig r; r.start(); r.command("start"); const auto pending = r.controller.snapshot().context;
    r.audio.hold_stop = true; r.audio.capture.push_back(frame(pending, 7));
    require(r.command("stop"), "stop was rejected while replacement audio buffered");
    r.pipeline.busy = false; r.tick();
    require(r.controller.snapshot().state == RealtimeState::finalizing && r.pipeline.finishes == 0 &&
            r.pipeline.accepted.size() == 1, "pending stop lost buffered PCM or finalized before final capture tail");
    r.audio.capture.push_back(frame(pending, 8, 137)); r.audio.value.capture_stopping = false; r.tick();
    require(r.pipeline.finishes == 1 && r.pipeline.accepted.size() == 2 && r.audio.starts.size() == 2,
            "pending stop did not finish the same capture session exactly once");
    r.response(pending); r.audio.value.drained = true; r.tick();
    require(r.pipeline.history == 1, "stopped buffered replacement did not complete normally");
    Rig bounded; bounded.start(); bounded.command("start"); bounded.tick(1500);
    bounded.command("start"); bounded.tick(501);
    require(bounded.controller.snapshot().stopping && bounded.controller.snapshot().exit_code == 5 &&
            bounded.pipeline.begins == 1, "repeated start extended an unresponsive model cancellation deadline");
}
void pending_capture_overflow_requires_repeat() {
    Rig r; r.start(); r.command("start"); const auto pending = r.controller.snapshot().context;
    r.audio.capture.push_back(frame(pending)); ++r.audio.value.capture_overflows; r.tick();
    require(r.reason("pending_capture_overflow") && !r.controller.snapshot().pending_start &&
            r.pipeline.begins == 1 && r.audio.capture.empty(),
            "capture overflow while cancelling silently admitted an incomplete new utterance");
    r.settle_cancel(); require(r.pipeline.begins == 1, "overflowed pending start automatically resumed");
    r.start(); require(r.pipeline.begins == 2, "explicit repeat failed after pending-ring overflow");
    Rig simultaneous; simultaneous.start(); simultaneous.command("start");
    ++simultaneous.audio.value.capture_overflows;
    simultaneous.audio.value.state = LiveAudioState::device_wait; simultaneous.tick();
    const auto repeat = std::any_of(simultaneous.logs.begin(), simultaneous.logs.end(), [](const Rig::Log& row) {
        return row.name == "repeat_required" && row.data.value("reason", "") == "pending_capture_overflow";
    });
    require(repeat && !simultaneous.controller.snapshot().pending_start && simultaneous.pipeline.begins == 1,
            "simultaneous device loss masked overflow and lost explicit repeat-required evidence");
}
void cancel_new_session_and_late_output() {
    Rig r; const auto old = r.start(); r.response(old);
    require(r.pipeline.history == 0, "history committed before cancellation test");
    require(r.command("cancel"), "cancel rejected");
    require(r.audio.value.drained, "double did not reproduce flush reporting drained");
    r.tick(); require(r.pipeline.history_attempts == 0, "cancelled drain committed history");
    require(r.command("new-session"), "new session rejected"); r.tick();
    require(r.pipeline.sessions == 1 && r.controller.snapshot().context.session_id > old.session_id,
            "new session failed to clear history/advance identity");
    const auto fresh = r.start();
    r.pipeline.emit(RealtimeEvent::Kind::asr_final, old);
    r.pipeline.emit(RealtimeEvent::Kind::pcm, old);
    r.pipeline.emit(RealtimeEvent::Kind::complete, old);
    r.pipeline.emit(RealtimeEvent::Kind::error, old, "old_model_error"); r.tick();
    require(r.controller.snapshot().state == RealtimeState::listening && r.audio.playback.size() == 1 &&
            r.audio.drains == 1 && r.pipeline.history_attempts == 0, "old session callback affected new turn");
    r.response(fresh); r.audio.value.drained = true; r.tick();
    require(r.pipeline.history == 1, "new session did not recover after cancelled drain");
    Rig immediate; const auto previous = immediate.start();
    immediate.command("cancel"); immediate.command("new-session"); immediate.command("start"); immediate.tick();
    require(immediate.pipeline.begins == 1 && immediate.controller.snapshot().pending_start,
            "cancel/new-session/start admitted a new model call before old worker stopped");
    immediate.settle_cancel();
    require(immediate.pipeline.begins == 2 && immediate.pipeline.overlapping_begins == 0 &&
            immediate.controller.snapshot().context.session_id > previous.session_id,
            "cancel/new-session/start did not resume with fresh history identity");
}
void backpressure_and_atomic_invalidation() {
    Rig r; const auto c = r.start(); r.audio.full = true;
    r.pipeline.emit(RealtimeEvent::Kind::endpoint, c, "vad_silence");
    r.pipeline.emit(RealtimeEvent::Kind::asr_final, c, "recognized");
    r.pipeline.emit(RealtimeEvent::Kind::pcm, c);
    r.pipeline.emit(RealtimeEvent::Kind::complete, c); r.pipeline.busy = false;
    const auto begin = std::chrono::steady_clock::now(); r.tick();
    require(std::chrono::steady_clock::now() - begin < std::chrono::milliseconds(100),
            "full playback queue blocked control caller");
    require(r.pipeline.events.size() == 1 && r.audio.drains == 0 && r.audio.attempts == 1,
            "full playback queue consumed completion ahead of pending PCM");
    r.tick(); require(r.pipeline.events.size() == 1 && r.audio.attempts == 2,
                      "backpressure retained more than one PCM or busy-looped");
    r.command("cancel"); r.tick(); r.audio.full = false; r.tick();
    require(r.audio.playback.empty() && r.pipeline.history_attempts == 0,
            "pending PCM survived cancellation");
    const auto fresh = r.start(); r.pipeline.invalidate_at_commit = true;
    r.pipeline.emit(RealtimeEvent::Kind::endpoint, fresh, "vad_silence");
    r.pipeline.emit(RealtimeEvent::Kind::asr_final, fresh, "recognized");
    r.pipeline.emit(RealtimeEvent::Kind::pcm, fresh); r.tick();
    require(r.audio.playback.empty() && r.reason("stale_pcm_commit") && r.pipeline.history_attempts == 0,
            "generation changed at commit boundary but PCM escaped");
    Rig blocked; const auto bc = blocked.start(); blocked.audio.full = true;
    blocked.response(bc);
    blocked.tick(blocked.config.queues.playback_ms + 5001);
    require(blocked.reason("playback_submit_timeout") && blocked.audio.playback.empty() &&
            blocked.pipeline.history_attempts == 0,
            "full device queue retained pending PCM forever without controlled cancellation");
}
void capture_overflow_and_discontinuity() {
    Rig r; const auto c = r.start(); r.pipeline.reject_submit = true;
    r.audio.capture.push_back(frame(c));
    const auto begin = std::chrono::steady_clock::now(); r.tick();
    require(std::chrono::steady_clock::now() - begin < std::chrono::milliseconds(100),
            "full model input queue blocked capture/control");
    require(r.reason("capture_queue_overflow") && r.pipeline.cancels == 1 && r.audio.capture.empty(),
            "input overflow was silently dropped instead of cancelling/resetting turn");
    r.settle_cancel(); r.pipeline.reject_submit = false; r.start();
    ++r.audio.value.capture_holes; r.tick();
    require(r.reason("audio_capture_discontinuity") && r.pipeline.history_attempts == 0,
            "capture hole was accepted as a complete utterance");
    Rig bounded; const auto bc = bounded.start();
    for (int i = 0; i < 101; ++i) bounded.audio.capture.push_back(frame(bc, i));
    bounded.tick(); require(bounded.pipeline.accepted.size() == 100 && bounded.audio.capture.size() == 1,
                            "one tick exceeded configured capture queue work bound");
}
void disconnect_recovery_and_drain_identity() {
    Rig r; const auto old = r.start(); r.response(old);
    r.audio.value.state = LiveAudioState::device_wait; r.audio.value.drained = true; r.tick();
    require(r.pipeline.history_attempts == 0 && r.controller.snapshot().state == RealtimeState::device_wait,
            "device loss during drain committed history or failed to wait");
    require(!r.command("start"), "start accepted without configured device");
    r.audio.value.state = LiveAudioState::ready; r.tick();
    require(r.controller.snapshot().state == RealtimeState::idle && r.pipeline.begins == 1,
            "device recovery automatically resumed stale session");
    const auto fresh = r.start(); r.audio.capture.push_back(frame(old));
    r.audio.capture.push_back(frame(fresh)); r.tick();
    require(r.pipeline.accepted.size() == 1 && same(r.pipeline.accepted.back().context, fresh),
            "reconnected capture retained old session audio");
    r.response(fresh); ++r.audio.value.context.generation_id; r.audio.value.drained = true; r.tick();
    require(r.reason("drain_session_lost") && r.pipeline.history_attempts == 0,
            "foreign drained audio session committed current history");
}
void drain_disconnect_race() {
    Rig r; const auto c = r.start(); r.response(c);
    r.audio.value.drained = true; r.audio.disconnect_at_drain_commit = true; r.tick();
    require(r.pipeline.history_attempts == 0 && r.pipeline.history == 0 && r.reason("drain_session_lost"),
            "device loss between drained observation and atomic history commit escaped guard");
}
void invalid_event_order_and_control_privacy() {
    Rig early; const auto c = early.start();
    early.pipeline.emit(RealtimeEvent::Kind::complete, c); early.pipeline.busy = false; early.tick();
    early.audio.value.drained = true; early.tick();
    require(early.pipeline.history_attempts == 0 && early.audio.drains == 0,
            "completion before ASR response was accepted");
    Rig hidden; const std::string secret = "private-rejected-input-47";
    require(!hidden.command(secret), "unknown command accepted");
    for (const auto& row : hidden.logs)
        require(row.data.dump().find(secret) == std::string::npos, "rejected control input leaked to persistent logger");
}
void finalization_deadlines() {
    for (const bool automatic : {false, true}) {
        Rig r; const auto c = r.start();
        if (automatic) { r.pipeline.emit(RealtimeEvent::Kind::endpoint, c, "vad_silence"); r.tick(); }
        else { r.command("stop"); r.tick(); require(r.pipeline.finishes == 1, "manual finalization never began"); }
        r.tick(r.config.timeouts.asr_final_ms + 1);
        require(r.reason("asr_final_timeout") && r.controller.snapshot().state == RealtimeState::cancelling,
                automatic ? "automatic endpoint bypassed ASR final deadline" : "manual finish bypassed ASR final deadline");
        r.settle_cancel(); r.start();
        require(r.pipeline.overlapping_begins == 0, "ASR timeout recovery overlapped old model");
    }
}
void cancellation_timeout_and_quit() {
    Rig r; r.start(); r.command("start");
    r.tick(r.config.timeouts.cancel_ms + 1);
    require(r.controller.snapshot().state == RealtimeState::error && r.controller.snapshot().stopping &&
            r.controller.snapshot().exit_code == 5 && r.pipeline.begins == 1,
            "unresponsive model cancel did not stop with explicit timeout");
    require(!r.command("start"), "restart accepted after fatal cancellation timeout");
    Rig flush; flush.start(); flush.audio.hold_reset = true; flush.pipeline.acknowledge_cancel = true;
    flush.command("quit"); flush.tick();
    require(!flush.controller.snapshot().stopping, "quit skipped audio flush acknowledgement");
    flush.audio.value.reset_pending = false; flush.tick();
    require(flush.controller.snapshot().stopping && flush.controller.snapshot().exit_code == 0,
            "quit failed to complete after both shutdown barriers");
    Rig lost_flush; lost_flush.start(); lost_flush.audio.hold_reset = true;
    lost_flush.pipeline.acknowledge_cancel = true; lost_flush.command("cancel");
    lost_flush.tick(lost_flush.config.timeouts.cancel_ms + 1);
    require(lost_flush.controller.snapshot().stopping && lost_flush.controller.snapshot().exit_code == 5,
            "unacknowledged audio flush did not honor cancellation deadline");
}
void playback_drain_timeout() {
    Rig r; const auto c = r.start(); r.response(c);
    r.tick(r.config.queues.max_tts_audio_ms + r.config.queues.playback_ms + 5001);
    require(r.reason("playback_drain_timeout") && r.pipeline.history_attempts == 0,
            "undrained device waited forever or committed incomplete playback");
    r.settle_cancel(); r.start();
}
void sustained_low_memory_only() {
    Rig r; r.start();
    r.memory = 1572863; r.tick();
    require(!r.command("start") && r.pipeline.cancels == 0,
            "low memory should refuse admission immediately but preserve transient active work");
    r.tick(2999);
    require(r.pipeline.cancels == 0 && r.controller.snapshot().state == RealtimeState::listening,
            "transient low memory cancelled before continuous three second window");
    r.memory = 4 * 1024 * 1024; r.tick();
    r.memory = 1572863; r.tick(); r.tick(2999);
    require(r.pipeline.cancels == 0, "low memory timer was not reset by recovery");
    r.tick(2);
    require(r.pipeline.cancels == 1 && r.controller.snapshot().state == RealtimeState::cancelling,
            "continuous low memory did not cancel at three seconds");
    r.memory = 4 * 1024 * 1024; r.settle_cancel(); r.start();
    require(r.pipeline.overlapping_begins == 0, "low memory recovery reused active worker");
}
void failures_and_memory_recovery() {
    Rig r; auto c = r.start();
    r.pipeline.emit(RealtimeEvent::Kind::error, c, "llm_first_token_timeout"); r.tick();
    require(r.reason("llm_first_token_timeout") && r.pipeline.cancels == 1,
            "pipeline timeout did not initiate controlled cancellation");
    r.settle_cancel(); c = r.start(); r.response(c); r.pipeline.reject_history = true;
    r.audio.value.drained = true; r.tick();
    require(r.reason("history_commit_rejected") && r.pipeline.history == 0,
            "history rejection reported completed success");
    r.settle_cancel(); r.pipeline.reject_history = false; r.memory = -1; r.tick();
    require(r.controller.snapshot().state == RealtimeState::error && !r.command("start"),
            "unknown memory availability did not fail closed");
    r.memory = 4 * 1024 * 1024; r.tick(); r.start();
    require(r.pipeline.overlapping_begins == 0, "memory recovery overlapped old work");
    require(!r.command("unknown-command") && r.command("status") && r.command("help"),
            "control command acceptance invalid");
}
}
int main() {
    int failed = 0;
    const std::vector<std::pair<const char*, void(*)()>> cases = {
        {"barge_pre_output_guard", barge_waits_for_accepted_playback},
        {"barge_backpressure_prefix_barriers", barge_interrupt_under_backpressure},
        {"barge_prefix_bound_overflow", barge_prefix_bound_and_overflow},
        {"barge_stale_failure_deadline", barge_stale_failure_and_deadline},
        {"barge_normal_drain_manual_stop", barge_normal_drain_and_manual_stop},
        {"wake_window_session_handoff", wake_handoff_window_and_session},
        {"wake_cancel_device_memory_stale", wake_cancel_device_memory_and_stale},
        {"wake_manual_start_overflow", manual_start_and_overflow_during_wake},
        {"wake_fault_latch_recovery", wake_fault_requires_explicit_recovery},
        {"shared_queue_budgets", shared_queue_budget_boundaries},
        {"capture_admission_backpressure", capture_admission_preserves_audio_queue}, {"normal_stop_tail", normal_and_stop_tail}, {"endpoints_empty_15s_transport", endpoint_reasons_and_empty_input},
        {"replacement_barriers", replacement_waits_for_both_barriers},
        {"replacement_capture_ring", replacement_preserves_waiting_capture_ring},
        {"pending_stop_cancel_deadline", pending_stop_and_cancel_deadline}, {"pending_capture_overflow", pending_capture_overflow_requires_repeat},
        {"cancel_new_session_late_output", cancel_new_session_and_late_output},
        {"backpressure_atomic_invalidation", backpressure_and_atomic_invalidation}, {"capture_overflow_discontinuity", capture_overflow_and_discontinuity},
        {"device_recovery_drain_identity", disconnect_recovery_and_drain_identity},
        {"atomic_drain_disconnect", drain_disconnect_race}, {"invalid_events_control_privacy", invalid_event_order_and_control_privacy},
        {"asr_final_deadlines", finalization_deadlines},
        {"cancel_timeout_quit", cancellation_timeout_and_quit}, {"playback_drain_timeout", playback_drain_timeout},
        {"sustained_low_memory", sustained_low_memory_only}, {"failures_memory_recovery", failures_and_memory_recovery}
    };
    for (const auto& test : cases) {
        try { test.second(); std::cout << "PASS " << test.first << '\n'; }
        catch (const std::exception& error) { ++failed; std::cerr << "FAIL " << test.first << ": " << error.what() << '\n'; }
    }
    if (failed) return 1;
    std::cout << "REALTIME_ENGINE_OK: model-free production controller; hardware/model acceptance not run\n";
    return 0;
}
