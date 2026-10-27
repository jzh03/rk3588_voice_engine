#include "voice_engine/realtime.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <utility>
#include <deque>
namespace voice_engine {
namespace {
bool same(AudioContext a, AudioContext b) {
    return a.session_id == b.session_id && a.generation_id == b.generation_id;
}
constexpr std::int64_t memory_floor_kib = 1572864;
}
RealtimeQueueBudget realtime_queue_budget(const Config& config) {
    if (config.audio.frame_ms != 20 || config.queues.capture_ms < 40 ||
        config.queues.playback_ms < 60 || config.queues.capture_ms % 20 ||
        config.queues.playback_ms % 20)
        throw ConfigError("realtime requires 20 ms frames, capture_ms >= 40 and playback_ms >= 60");
    RealtimeQueueBudget budget;
    if (config.dialogue.barge_in) {
        // 128 ms covers the fixed detector's window/context lookback. The
        // configured pre-roll and confirmation interval remain real PCM.
        budget.capture_preroll_frames = (config.vad.pre_roll_ms + config.dialogue.barge_in_min_ms + 128 + 19) / 20;
        if (std::size_t(config.queues.capture_ms / 20) < budget.capture_preroll_frames + 2)
            throw ConfigError("barge-in capture_ms must cover pre-roll, confirmation and two transport slots");
    }
    budget.capture_audio_frames = config.queues.capture_ms / 20 - 1 - budget.capture_preroll_frames;
    const auto playback = static_cast<std::size_t>(config.queues.playback_ms / 20);
    budget.playback_tts_frames = (playback - 1) / 2;
    budget.playback_audio_frames = playback - 1 - budget.playback_tts_frames;
    return budget;
}
const char* realtime_state_name(RealtimeState s) noexcept {
    switch(s) {
    case RealtimeState::starting: return "STARTING";
    case RealtimeState::idle: return "IDLE";
    case RealtimeState::listening: return "LISTENING";
    case RealtimeState::finalizing: return "FINALIZING";
    case RealtimeState::responding: return "RESPONDING";
    case RealtimeState::cancelling: return "CANCELLING";
    case RealtimeState::device_wait: return "DEVICE_WAIT";
    case RealtimeState::error: return "ERROR";
    case RealtimeState::stopping: return "STOPPING";
    }
    return "ERROR";
}
struct RealtimeController::Impl {
    Config config;
    RealtimeQueueBudget budget;
    std::deque<AudioFrame> preroll;
    bool barge_listening = false, awaiting_barge_playback = false;
    LiveAudioBackend& audio;
    RealtimePipeline& pipeline;
    Log logger;
    RealtimeSnapshot snap;
    bool shutting_down = false, low_memory = false, input_finished = false;
    bool draining = false, empty_turn = false, buffered_start = false, pending_stop = false;
    bool wake_triggered = false, dialogue_open = false, wake_fault = false;
    std::int64_t deadline = 0, listen_started = 0, memory_kib = -1, low_since = 0, playback_blocked_since = 0;
    std::uint64_t capture_overflows = 0, capture_holes = 0, dropped_events = 0;
    std::optional<RealtimeEvent> pending_pcm;
    Impl(const Config& c, LiveAudioBackend& a, RealtimePipeline& p, Log l)
        : config(c), budget(realtime_queue_budget(c)), audio(a), pipeline(p), logger(std::move(l)) {}
    void log(std::string_view name, const nlohmann::json& data = nlohmann::json::object()) {
        if (logger) logger(name, snap.context, snap.state, data);
    }
    void state(RealtimeState next) {
        if (snap.state == next) return;
        auto old = snap.state; snap.state = next;
        log("state_changed", {{"from", realtime_state_name(old)}, {"to", realtime_state_name(next)}});
    }
    bool active() const {
        return snap.state == RealtimeState::listening || snap.state == RealtimeState::finalizing ||
               snap.state == RealtimeState::responding;
    }
    void cancel(std::int64_t now, const std::string& why) {
        const auto dispatch_start = std::chrono::steady_clock::now();
        const bool already_cancelling = snap.state == RealtimeState::cancelling;
        auto old = snap.context;
        ++snap.context.generation_id;
        pending_pcm.reset(); playback_blocked_since = 0; draining = false; empty_turn = false;
        buffered_start = false; pending_stop = false;
        barge_listening = awaiting_barge_playback = false; preroll.clear();
        snap.wake_listening = false; wake_triggered = false; dialogue_open = false;
        // The controller is single-threaded; invalidation and downstream flush
        // finish before another playback submission can be attempted.
        pipeline.cancel(); audio.cancel();
        const auto stopped_at = std::chrono::steady_clock::now();
        const auto dispatch_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            stopped_at - dispatch_start).count();
        const auto stopped_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            stopped_at.time_since_epoch()).count();
        log("cancel_requested", {{"reason", why}, {"cancelled_generation", old.generation_id},
                                 {"cancel_requested_ns", now}, {"old_pcm_stopped_ns", stopped_ns},
                                 {"cancel_dispatch_ns", dispatch_ns},
                                 {"measurement_method", "software_observation"},
                                 {"pipeline", pipeline.diagnostics()}});
        if (!already_cancelling) deadline = now + std::int64_t(config.timeouts.cancel_ms) * 1000000;
        state(RealtimeState::cancelling);
    }
    void fail(std::int64_t now, const std::string& why) {
        snap.pending_start = false;
        log("turn_failed", {{"reason", why}});
        cancel(now, why);
    }
    void buffer_pending(std::int64_t now) {
        preroll.clear();
        const auto status = audio.status();
        ++snap.context.generation_id;
        if (!audio.start_session(snap.context, true)) {
            fail(now, "device_start_failed"); return;
        }
        capture_overflows = status.capture_overflows;
        capture_holes = status.capture_holes; dropped_events = status.dropped_events;
        buffered_start = true; listen_started = now;
        log("pending_capture_started", {{"capacity_ms", config.queues.capture_ms},
                                       {"model_admission", "waiting_for_idle"}});
    }
    bool preserve_capture(std::int64_t now) {
        if (!audio.interrupt_playback(snap.context)) {
            fail(now, "barge_capture_handoff_failed"); return false;
        }
        for (auto& frame : preroll) frame.context = snap.context;
        buffered_start = true; listen_started = now;
        return true;
    }
    void barge_in(std::int64_t now, const BargeInSignal& signal) {
        const auto old = snap.context;
        log("barge_in_detected", {{"detected_ns", signal.detected_ns},
            {"detector_last_frame_seq", signal.last_frame_seq}, {"input_samples", signal.input_samples},
            {"preroll_frames", preroll.size()}, {"minimum_speech_ms", config.dialogue.barge_in_min_ms},
            {"first_preserved_seq", preroll.empty() ? 0 : preroll.front().seq},
            {"measurement_method", "software_observation"}});
        ++snap.context.generation_id;
        pending_pcm.reset(); playback_blocked_since = 0; draining = empty_turn = false;
        pending_stop = false; barge_listening = awaiting_barge_playback = false;
        // Invalidate all model output first. Only playback is flushed; capture
        // remains continuous and is admitted to ASR after both idle barriers.
        pipeline.cancel();
        if (!preserve_capture(now)) return;
        snap.pending_start = true;
        deadline = now + std::int64_t(config.timeouts.cancel_ms) * 1000000;
        const auto stopped = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        log("cancel_requested", {{"reason", "barge_in"}, {"cancelled_generation", old.generation_id},
            {"cancel_requested_ns", now}, {"old_pcm_stopped_ns", stopped},
            {"measurement_method", "software_observation"}, {"capture_preserved", true}});
        state(RealtimeState::cancelling);
    }
    void service_barge_in(std::int64_t now) {
        if (awaiting_barge_playback) {
            // Capture stays live, but pre-output speech/noise must not cancel
            // the answer before it can start. Drain these frames within the
            // existing bound; do not accumulate a second utterance queue.
            AudioFrame discarded;
            for (int n = 0; n < config.queues.capture_ms / config.audio.frame_ms &&
                 audio.try_capture_frame(discarded); ++n) {}
            return;
        }
        if (!barge_listening) return;
        if (auto signal = pipeline.poll_barge_in()) {
            if (!same(signal->context, snap.context)) log("stale_barge_in_event");
            else if (!signal->error.empty()) { fail(now, signal->error); return; }
            else { barge_in(now, *signal); return; }
        }
        AudioFrame frame;
        const auto limit = config.queues.capture_ms / config.audio.frame_ms;
        for (int n = 0; n < limit && pipeline.can_accept_barge_in() && audio.try_capture_frame(frame); ++n) {
            if (!same(frame.context, snap.context)) continue;
            if (!pipeline.submit_barge_in(frame)) { fail(now, "barge_capture_queue_rejected"); return; }
            preroll.push_back(std::move(frame));
            if (preroll.size() > budget.capture_preroll_frames) preroll.pop_front();
        }
    }
    void arm_wake(std::int64_t now) {
        if (!config.dialogue.wake_enabled || wake_fault || snap.wake_listening || !pipeline.idle() ||
            audio.status().reset_pending) return;
        ++snap.context.session_id; ++snap.context.generation_id;
        pipeline.new_session();
        log("session_started", {{"reason", "waiting_for_new_wake"}});
        if (!audio.start_session(snap.context, true)) { fail(now, "device_start_failed"); return; }
        const auto status = audio.status();
        capture_overflows = status.capture_overflows; capture_holes = status.capture_holes;
        dropped_events = status.dropped_events;
        pipeline.begin_wake(snap.context); snap.wake_listening = true; wake_triggered = false;
        log("wake_listening", {{"cpu_only", true}});
    }
    void service_wake(std::int64_t now) {
        const auto status = audio.status();
        if (status.capture_overflows > capture_overflows || status.capture_holes > capture_holes ||
            status.dropped_events > dropped_events) { fail(now, "wake_capture_discontinuity"); return; }
        AudioFrame frame;
        const auto limit = std::max(1, config.queues.capture_ms / config.audio.frame_ms);
        for (int n = 0; !wake_triggered && n < limit && pipeline.can_accept_capture() &&
             audio.try_capture_frame(frame); ++n)
            if (same(frame.context, snap.context) && !pipeline.submit(frame)) {
                fail(now, "wake_capture_queue_overflow"); return;
            }
        for (int n = 0; n < 8; ++n) {
            const auto event = pipeline.poll();
            if (!event) break;
            if (!same(event->context, snap.context)) { log("stale_pipeline_event"); continue; }
            if (event->kind == RealtimeEvent::Kind::error) {
                // A failed detector must not spin through automatic rearming.
                // A manual start/new-session is required after cancellation.
                wake_fault = true;
                log("pipeline_error", event->data); fail(now, event->reason); return;
            }
            if (event->kind != RealtimeEvent::Kind::wake) { fail(now, "invalid_wake_event"); return; }
            wake_triggered = true; log("wake_detected", event->data);
        }
        if (wake_triggered && pipeline.idle()) {
            // Keep the live audio session/ring: no flush of post-trigger speech.
            snap.wake_listening = false; wake_triggered = false;
            buffered_start = true; listen_started = now; snap.pending_start = true;
            start(now);
        }
    }
    void start(std::int64_t now) {
        const auto status = audio.status();
        if (status.state == LiveAudioState::device_wait || status.reset_pending || !pipeline.idle()) return;
        if (memory_kib < memory_floor_kib) { snap.pending_start = false; low_memory = true; state(RealtimeState::error); return; }
        if (!buffered_start) buffer_pending(now);
        if (!buffered_start) return;
        pipeline.begin(snap.context);
        dialogue_open = config.dialogue.wake_enabled;
        // Reuse the pending audio session and its bounded capture ring. Starting
        // it again here would flush the beginning of the user's new utterance.
        buffered_start = false;
        input_finished = false; draining = false; empty_turn = false;
        snap.pending_start = false;
        state(pending_stop ? RealtimeState::finalizing : RealtimeState::listening);
        if (pending_stop) deadline = now + std::int64_t(config.timeouts.asr_final_ms) * 1000000;
        pending_stop = false;
        log("turn_started");
        if (dialogue_open) log("dialogue_window_opened",
            {{"window_ms", config.dialogue.window_ms}, {"opened_ns", now}});
    }
    void tick(std::int64_t now, std::int64_t available) {
        if (snap.stopping) return;
        memory_kib = available;
        for (const auto& e : audio.poll_events())
            log("audio_event", {{"type", e.type}, {"detail", e.detail}, {"monotonic_ns", e.monotonic_ns},
                                {"audio_session_id", e.context.session_id},
                                {"audio_generation_id", e.context.generation_id}});
        auto status = audio.status();
        if (buffered_start && (status.capture_overflows > capture_overflows ||
            status.capture_holes > capture_holes || status.dropped_events > dropped_events)) {
            fail(now, "pending_capture_overflow");
            log("repeat_required", {{"reason", "pending_capture_overflow"}});
            status = audio.status();
        }
        if (status.state == LiveAudioState::device_wait && (active() || buffered_start || snap.wake_listening)) {
            snap.pending_start = false; cancel(now, "device_unavailable");
        }
        if (available < memory_floor_kib) {
            if (!low_memory) log("low_memory", {{"mem_available_kib", available}, {"floor_kib", memory_floor_kib}});
            if (!low_memory) low_since = now;
            low_memory = true;
            if ((active() && (available < 0 || now - low_since >= 3000000000LL)) || buffered_start || snap.wake_listening) {
                snap.pending_start = false; cancel(now, "low_memory");
            }
        } else { low_memory = false; low_since = 0; }
        status = audio.status();
        if (buffered_start && (status.capture_overflows > capture_overflows ||
            status.capture_holes > capture_holes || status.dropped_events > dropped_events)) {
            fail(now, "pending_capture_overflow");
            log("repeat_required", {{"reason", "pending_capture_overflow"}});
            status = audio.status();
        }
        if (snap.state == RealtimeState::cancelling) {
            if (pipeline.idle() && (!status.reset_pending || status.state == LiveAudioState::device_wait)) {
                const auto observed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                log("cancel_completed", {{"model_stopped_ns", observed_ns},
                    {"flush_ack_ns", status.state == LiveAudioState::device_wait ? 0 : observed_ns},
                    {"completion_time_method", "poll_observation"}});
                if (shutting_down) { state(RealtimeState::stopping); snap.stopping = true; return; }
                state(status.state == LiveAudioState::device_wait ? RealtimeState::device_wait :
                      (low_memory || wake_fault) ? RealtimeState::error : RealtimeState::idle);
            } else if (now >= deadline) {
                log("restart_required", {{"reason", "cancel_timeout"}, {"deadline_ns", deadline},
                                         {"pipeline", pipeline.diagnostics()}});
                state(RealtimeState::error); snap.exit_code = 5; snap.stopping = true; return;
            } else return;
        }
        if (snap.state == RealtimeState::starting || snap.state == RealtimeState::device_wait ||
            snap.state == RealtimeState::idle || snap.state == RealtimeState::error) {
            state(status.state == LiveAudioState::device_wait ? RealtimeState::device_wait :
                  (low_memory || wake_fault) ? RealtimeState::error : RealtimeState::idle);
            if (snap.pending_start && snap.state == RealtimeState::idle) start(now);
        }
        if (snap.wake_listening) service_wake(now);
        if (!active()) {
            if (snap.state == RealtimeState::idle && !snap.pending_start) arm_wake(now);
            return;
        }
        if (status.capture_overflows > capture_overflows || status.capture_holes > capture_holes ||
            status.dropped_events > dropped_events) { fail(now, "audio_capture_discontinuity"); return; }
        // Independent priority path: a full playback queue must never hide an
        // insertion behind the retained PCM event or the normal model queue.
        if (snap.state == RealtimeState::responding) {
            service_barge_in(now);
            if (snap.state != RealtimeState::responding) return;
        }
        if (snap.state == RealtimeState::listening || (snap.state == RealtimeState::finalizing && !input_finished)) {
            AudioFrame frame;
            // At most the configured bounded queue per tick; never block capture.
            const auto limit = std::max(1, config.queues.capture_ms / config.audio.frame_ms);
            for (int n = 0; n < limit && pipeline.can_accept_capture(); ++n) {
                if (!preroll.empty()) { frame = std::move(preroll.front()); preroll.pop_front(); }
                else if (!audio.try_capture_frame(frame)) break;
                if (same(frame.context, snap.context) && !pipeline.submit(frame)) {
                    fail(now, "capture_queue_overflow"); return;
                }
            }
            status = audio.status();
            if (snap.state == RealtimeState::finalizing && !status.capture_stopping &&
                status.capture_queued_frames == 0 && preroll.empty()) {
                pipeline.finish_input(); input_finished = true;
                deadline = now + std::int64_t(config.timeouts.asr_final_ms) * 1000000;
            }
            const auto listen_limit = std::int64_t((dialogue_open ? config.dialogue.window_ms : config.vad.start_timeout_ms) + config.vad.max_speech_ms +
                                                   config.timeouts.asr_final_ms) * 1000000;
            if (snap.state == RealtimeState::listening && now - listen_started > listen_limit) {
                fail(now, "capture_stalled"); return;
            }
        }
        if (snap.state == RealtimeState::finalizing && now >= deadline) {
            fail(now, "asr_final_timeout"); return;
        }
        // One retained PCM slot provides bounded backpressure to the TTS queue.
        for (int n = 0; n < 64; ++n) {
            auto e = pending_pcm ? std::move(pending_pcm) : pipeline.poll();
            pending_pcm.reset();
            if (!e) break;
            if (!same(e->context, snap.context)) { log("stale_pipeline_event"); continue; }
            switch(e->kind) {
            case RealtimeEvent::Kind::wake:
                fail(now, "wake_outside_idle"); return;
            case RealtimeEvent::Kind::endpoint:
                if (snap.state != RealtimeState::listening && snap.state != RealtimeState::finalizing) {
                    fail(now, "invalid_pipeline_order"); return;
                }
                if (!config.dialogue.barge_in) audio.stop_capture();
                input_finished = true;
                state(RealtimeState::finalizing);
                deadline = now + std::int64_t(config.timeouts.asr_final_ms) * 1000000;
                log("endpoint", e->data); break;
            case RealtimeEvent::Kind::asr_final:
                if (snap.state != RealtimeState::finalizing) { fail(now, "invalid_pipeline_order"); return; }
                if (!config.dialogue.barge_in) audio.stop_capture();
                // Discard ASR tail before arming the fresh response detector.
                { AudioFrame discarded; while(audio.try_capture_frame(discarded)) {} }
                preroll.clear();
                empty_turn = e->reason == "empty";
                state(RealtimeState::responding); log("asr_final", e->data);
                awaiting_barge_playback = config.dialogue.barge_in && !empty_turn;
                if (awaiting_barge_playback)
                    log("barge_in_waiting_for_playback", {{"policy", "first_pcm_accepted"}});
                break;
            case RealtimeEvent::Kind::pcm: {
                if (snap.state != RealtimeState::responding || draining || empty_turn) {
                    fail(now, "invalid_pipeline_order"); return;
                }
                bool accepted = false;
                const bool current = pipeline.commit_if_current(snap.context, [&] {
                    accepted = audio.try_submit_playback(e->frame);
                });
                if (!current) { fail(now, "stale_pcm_commit"); return; }
                if (!accepted) {
                    if (!playback_blocked_since) playback_blocked_since = now;
                    if (now - playback_blocked_since >
                        std::int64_t(config.queues.playback_ms + 5000) * 1000000) {
                        fail(now, "playback_submit_timeout"); return;
                    }
                    pending_pcm = std::move(e); return;
                }
                playback_blocked_since = 0;
                log("playback_submit", {{"seq", e->frame.seq}, {"valid_samples", e->frame.valid_samples},
                                       {"playback_submit_ns", now}});
                if (awaiting_barge_playback) {
                    // The producer has released actual response PCM. Throw
                    // away the final pre-output capture tail before reset, so
                    // it cannot be replayed as a fresh insertion.
                    AudioFrame discarded; while (audio.try_capture_frame(discarded)) {}
                    preroll.clear();
                    pipeline.begin_barge_in(snap.context);
                    barge_listening = true; awaiting_barge_playback = false;
                    log("barge_in_armed", {{"minimum_speech_ms", config.dialogue.barge_in_min_ms},
                        {"preroll_capacity_frames", budget.capture_preroll_frames},
                        {"capture_audio_capacity_frames", budget.capture_audio_frames},
                        {"policy", "first_pcm_accepted"}});
                }
                break;
            }
            case RealtimeEvent::Kind::complete:
                if (snap.state != RealtimeState::responding) { fail(now, "invalid_pipeline_order"); return; }
                if (!draining) {
                    draining = true; audio.request_drain();
                    deadline = now + std::int64_t(config.queues.max_tts_audio_ms +
                                                  config.queues.playback_ms + 5000) * 1000000;
                    log("playback_drain_requested", e->data);
                }
                break;
            case RealtimeEvent::Kind::error:
                log("pipeline_error", e->data); fail(now, e->reason); return;
            }
        }
        if (draining) {
            status = audio.status();
            if (!status.session_active || !same(status.context, snap.context) ||
                status.state == LiveAudioState::device_wait) { fail(now, "drain_session_lost"); return; }
            if (!status.reset_pending && status.drained && barge_listening) {
                pipeline.end_barge_in(); barge_listening = false;
            }
            if (!status.reset_pending && status.drained && pipeline.idle()) {
                bool committed = false, callback_entered = false;
                const bool drained_current = audio.commit_if_drained(snap.context, [&] {
                    callback_entered = true;
                    if (empty_turn) return true;
                    committed = pipeline.commit_history(snap.context); return committed;
                });
                if (!drained_current) { fail(now, callback_entered ? "history_commit_rejected" : "drain_session_lost"); return; }
                log("turn_completed", {{"history_committed", committed}, {"empty_input", empty_turn},
                                       {"playback_drained_ns", now}});
                draining = false; state(RealtimeState::idle);
                if (dialogue_open && !empty_turn) {
                    if (config.dialogue.barge_in) {
                        ++snap.context.generation_id;
                        if (!preserve_capture(now)) return;
                    }
                    snap.pending_start = true; start(now);
                } else {
                    preroll.clear();
                    if (config.dialogue.barge_in) audio.stop_capture();
                    if (dialogue_open) {
                        dialogue_open = false;
                        log("dialogue_window_closed", {{"reason", "empty_input"}});
                    }
                }
            } else if (now >= deadline) { fail(now, "playback_drain_timeout"); return; }
        }
    }
};
RealtimeController::RealtimeController(const Config& c, LiveAudioBackend& a, RealtimePipeline& p, Log log)
    : impl_(std::make_unique<Impl>(c, a, p, std::move(log))) {}
RealtimeController::~RealtimeController() = default;
void RealtimeController::tick(std::int64_t now, std::int64_t available) { impl_->tick(now, available); }
RealtimeSnapshot RealtimeController::snapshot() const { return impl_->snap; }
bool RealtimeController::command(std::string_view command, std::int64_t now) {
    auto& i = *impl_;
    while (!command.empty() && std::isspace(static_cast<unsigned char>(command.front()))) command.remove_prefix(1);
    while (!command.empty() && std::isspace(static_cast<unsigned char>(command.back()))) command.remove_suffix(1);
    if (command.empty()) return true;
    if (i.snap.stopping || i.shutting_down) return false;
    if (command != "start" && command != "stop" && command != "cancel" &&
        command != "new-session" && command != "quit" && command != "help" && command != "status") {
        i.log("command_rejected", {{"reason", "unknown_command"}}); return false;
    }
    i.log("command_received", {{"command", command}});
    if (command == "start") {
        if (i.low_memory || i.audio.status().state == LiveAudioState::device_wait) {
            i.log("command_rejected", {{"reason", "start_unavailable"}}); return false;
        }
        i.wake_fault = false;
        i.snap.pending_start = true;
        if (i.active() || i.snap.state == RealtimeState::cancelling || i.snap.wake_listening || !i.pipeline.idle()) {
            i.cancel(now, "replaced_by_start");
            i.buffer_pending(now);
        } else i.start(now);
    } else if (command == "stop") {
        if (i.buffered_start && i.snap.pending_start) {
            if (!i.config.dialogue.barge_in) i.audio.stop_capture();
            i.pending_stop = true; return true;
        }
        if (i.snap.state != RealtimeState::listening) return false;
        if (!i.config.dialogue.barge_in) i.audio.stop_capture();
        i.state(RealtimeState::finalizing);
        i.deadline = now + std::int64_t(i.config.timeouts.asr_final_ms) * 1000000;
    } else if (command == "cancel") {
        i.snap.pending_start = false; i.cancel(now, "command");
    } else if (command == "new-session") {
        i.wake_fault = false;
        i.snap.pending_start = false; i.cancel(now, "new_session");
        ++i.snap.context.session_id; i.pipeline.new_session(); i.log("session_started");
    } else if (command == "quit") shutdown(now);
    else if (command != "help" && command != "status") return false;
    return true;
}
void RealtimeController::shutdown(std::int64_t now) {
    auto& i = *impl_;
    if (i.snap.stopping || i.shutting_down) return;
    i.shutting_down = true; i.snap.pending_start = false; i.cancel(now, "shutdown");
}
} // namespace voice_engine
