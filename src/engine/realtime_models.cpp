#include "voice_engine/realtime.hpp"
#include "voice_engine/asr.hpp"
#include "voice_engine/kws.hpp"
#include "voice_engine/assets.hpp"
#include "voice_engine/llm.hpp"
#include "voice_engine/probe_support.hpp"
#include "voice_engine/tts_stream.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <thread>
#include <type_traits>
namespace voice_engine {
namespace {
using Clock = std::chrono::steady_clock;
std::int64_t now_ns() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }
bool same(AudioContext a, AudioContext b) {
    return a.session_id == b.session_id && a.generation_id == b.generation_id;
}
class ModelPipeline final : public RealtimePipeline {
    Config config_;
    RealtimeQueueBudget budget_;
    std::unique_ptr<KeywordSpotter> kws_;
    bool wake_mode_ = false;
    std::atomic<std::int64_t> kws_call_ns_{0};
    std::unique_ptr<BargeInDetector> barge_detector_;
    std::unique_ptr<BargeInMonitor> barge_;
    std::unique_ptr<ZipformerAsr> asr_;
    std::unique_ptr<SileroVad> vad_;
    std::unique_ptr<MeloTts> model_;
    std::unique_ptr<LlmAdapter> llm_;
    std::unique_ptr<ListeningAsr> listening_;
    std::unique_ptr<TtsStreamWorker> tts_;
    mutable std::mutex gate_;
    std::condition_variable cv_;
    std::thread worker_;
    bool stop_ = false, busy_ = false, valid_ = false, pending_ = false;
    bool input_end_ = false, accepting_ = false, responding_ = false, have_llm_ = false;
    bool completed_ = false, watchdog_reported_ = false;
    AudioContext context_;
    std::int64_t listening_deadline_ns_ = 0;
    LlmTurn turn_;
    std::deque<AudioFrame> capture_;
    std::deque<RealtimeEvent> events_;
    std::size_t capture_peak_ = 0;
    std::atomic<std::int64_t> asr_call_ns_{0}, tts_call_ns_{0};
    bool current(AudioContext id) const { return valid_ && same(id, context_) && !stop_; }
    TtsStreamId tts_id() const { return {context_.session_id, context_.generation_id}; }
    template<class F> auto asr_call(F&& function) -> decltype(function()) {
        asr_call_ns_ = now_ns();
        try {
            if constexpr (std::is_void_v<decltype(function())>) {
                function(); asr_call_ns_ = 0;
            } else {
                auto result = function(); asr_call_ns_ = 0; return result;
            }
        } catch (...) { asr_call_ns_ = 0; throw; }
    }
    bool alive(AudioContext id) const { std::lock_guard<std::mutex> lock(gate_); return current(id); }
    void event(AudioContext id, RealtimeEvent::Kind kind, const std::string& why,
               nlohmann::json data = nlohmann::json::object()) {
        std::lock_guard<std::mutex> lock(gate_);
        if (!current(id)) return;
        if (events_.size() >= 8) throw std::runtime_error("bounded pipeline event queue overflow");
        RealtimeEvent e; e.context = id; e.kind = kind; e.reason = why; e.data = std::move(data);
        events_.push_back(std::move(e));
    }
    void cancel_locked() {
        if (barge_) barge_->stop();
        valid_ = false; accepting_ = false; responding_ = false;
        capture_.clear(); events_.clear(); input_end_ = true;
        llm_->cancel(); tts_->cancel(tts_id()); cv_.notify_all();
    }
    void wake_turn(AudioContext id) {
        kws_->reset();
        while (alive(id)) {
            AudioFrame frame;
            {
                std::unique_lock<std::mutex> lock(gate_);
                cv_.wait_for(lock, std::chrono::milliseconds(5), [&] {
                    return !current(id) || !capture_.empty();
                });
                if (!current(id)) return;
                if (capture_.empty()) continue;
                frame = std::move(capture_.front()); capture_.pop_front();
            }
            kws_call_ns_ = now_ns();
            std::optional<KeywordHit> hit;
            try { hit = kws_->accept(frame.samples.data(), frame.valid_samples); }
            catch (...) { kws_call_ns_ = 0; throw; }
            kws_call_ns_ = 0;
            if (hit) {
                // Stop KWS admission before publishing. Already queued post-hit
                // PCM stays in capture_ and is handed to ASR under the same ID.
                { std::lock_guard<std::mutex> lock(gate_); accepting_ = false; }
                event(id, RealtimeEvent::Kind::wake, "keyword",
                    {{"input_samples", hit->input_samples}, {"trigger_ns", now_ns()},
                     {"last_token_seconds", hit->token_end_seconds},
                     {"timing_method", "software_observation_token_time_not_acoustic_end"}});
                return;
            }
        }
    }
    void turn(AudioContext id) {
        if (!llm_->idle() || !tts_->wait_idle(0)) throw std::logic_error("NPU/producer not idle before ASR");
        auto opening_deadline = listening_deadline_ns_;
        asr_call([&] { listening_->begin(nullptr, 0, opening_deadline); });
        while (alive(id)) {
            AudioFrame frame;
            bool have = false, end = false;
            {
                std::unique_lock<std::mutex> lock(gate_);
                cv_.wait_for(lock, std::chrono::milliseconds(5), [&] {
                    return !current(id) || !capture_.empty() || input_end_;
                });
                if (!current(id)) break;
                if (!capture_.empty()) {
                    frame = std::move(capture_.front()); capture_.pop_front(); have = true;
                } else end = input_end_;
            }
            if (have) asr_call([&] { listening_->accept(frame.samples.data(), frame.valid_samples); });
            if (end) {
                event(id, RealtimeEvent::Kind::endpoint, "manual_stop", {{"endpoint_ns", now_ns()}, {"end_reason", "manual_stop"}});
                asr_call([&] { listening_->finish(); });
            }
            if (opening_deadline && now_ns() >= opening_deadline) {
                asr_call([&] { listening_->poll_start_timeout(now_ns()); });
                opening_deadline = 0;
            }
            if (listening_->state() == ListeningState::final) break;
        }
        if (!alive(id)) { asr_call([&] { listening_->cancel(); }); return; }
        const auto result = listening_->result();
        {
            std::lock_guard<std::mutex> lock(gate_);
            if (!current(id)) return;
            accepting_ = false; capture_.clear();
        }
        // ListeningAsr may synchronously finalize during accept(). The independent
        // asr_call watchdog covers that path as well as explicit finish().
        if (result.reason != ListeningEnd::input_finished)
            event(id, RealtimeEvent::Kind::endpoint, listening_end_name(result.reason),
                  {{"end_reason", listening_end_name(result.reason)},
                   {"endpoint_ns", now_ns() - static_cast<std::int64_t>(result.endpoint_to_final_sec * 1e9)},
                   {"endpoint_time_method", "asr_completion_minus_finish_duration_estimate"},
                   {"endpoint_sample", result.endpoint_sample}});
        event(id, RealtimeEvent::Kind::asr_final, result.hypothesis.empty() ? "empty" : "recognized",
              {{"end_reason", listening_end_name(result.reason)}, {"asr_final_ns", now_ns()},
               {"text_bytes", result.hypothesis.size()}, {"input_samples", result.input_samples},
               {"endpoint_to_final_sec", result.endpoint_to_final_sec}});
        if (result.hypothesis.empty()) {
            event(id, RealtimeEvent::Kind::complete, "empty"); return;
        }
        {
            std::lock_guard<std::mutex> lock(gate_);
            if (!current(id)) return;
            // No ASR call is running here; a single turn owns NPU admission.
            turn_ = llm_->start(result.hypothesis);
            have_llm_ = true;
            tts_->begin(tts_id()); responding_ = true;
        }
        bool llm_done = false;
        LlmResult completed_llm;
        while (alive(id)) {
            if (!llm_done) {
                // Drain token packets first. wait() is legal only after SDK idle
                // and an empty packet queue, so it cannot swallow trailing text.
                for (int n = 0; n < 64; ++n) {
                    auto packet = llm_->poll(turn_, 0);
                    if (!packet) break;
                    if (packet->text.empty()) continue;
                    bool accepted = false;
                    {
                        std::lock_guard<std::mutex> lock(gate_);
                        if (!current(id)) break;
                        llm_->commit_if_current(turn_, [&] { accepted = tts_->submit(tts_id(), packet->text); });
                    }
                    if (!accepted && alive(id)) throw std::runtime_error("TTS text queue rejected input");
                }
                if (llm_->restart_required()) throw std::runtime_error("LLM restart_required");
                if (llm_->idle()) {
                    // poll once more after the idle barrier. This also flushes
                    // the adapter's final UTF-8/control-filter text atomically.
                    auto tail = llm_->poll(turn_, 0);
                    if (tail) {
                        if (!tail->text.empty()) {
                            bool accepted = false;
                            std::lock_guard<std::mutex> lock(gate_);
                            if (current(id)) llm_->commit_if_current(turn_, [&] {
                                accepted = tts_->submit(tts_id(), tail->text);
                            });
                            if (!accepted && current(id)) throw std::runtime_error("TTS tail rejected");
                        }
                        continue;
                    }
                    const auto result_llm = llm_->wait(turn_, 0);
                    if (!result_llm) throw std::runtime_error("LLM idle without completion");
                    if (result_llm->status != "completed")
                        throw std::runtime_error("LLM " + result_llm->status);
                    if (result_llm->text.empty()) throw std::runtime_error("LLM empty response");
                    completed_llm = *result_llm;
                    if (!tts_->finish({id.session_id, id.generation_id}) && alive(id))
                        throw std::runtime_error("TTS finish rejected");
                    llm_done = true;
                }
            }
            const bool tts_idle = tts_->wait_idle(0);
            const auto stats = tts_->stats();
            if (stats.status == "failed") throw std::runtime_error("TTS " + stats.error);
            if (llm_done && tts_idle && stats.current_pcm_frames == 0) {
                if (stats.status != "completed" || stats.frames == 0)
                    throw std::runtime_error("TTS incomplete or empty output");
                {
                    std::lock_guard<std::mutex> lock(gate_);
                    if (!current(id)) return;
                    completed_ = true;
                }
                nlohmann::json segment_timings=nlohmann::json::array();
                for(const auto& t:tts_->segment_timings({id.session_id,id.generation_id})) {
                    segment_timings.push_back({
                        {"segment_seq",t.segment_seq},{"text_bytes",t.text_bytes},
                        {"text_chars",t.text_chars},{"min_chars",t.min_chars},
                        {"target_chars",t.target_chars},{"boundary_lookahead_chars",t.boundary_lookahead_chars},
                        {"text_selected_ns",t.text_selected_ns},
                        {"native_wait_begin_ns",t.native_wait_begin_ns},
                        {"native_slot_acquired_ns",t.native_slot_acquired_ns},
                        {"generate_begin_ns",t.generate_begin_ns},{"generate_end_ns",t.generate_end_ns},
                        {"sdk_generate_seconds",t.sdk_generate_seconds},
                        {"native_samples",t.native_samples},{"native_sample_rate",t.native_sample_rate},
                        {"native_enqueued_ns",t.native_enqueued_ns},
                        {"input_queued_bytes",t.input_queued_bytes},
                        {"pcm_frames_before_generate",t.pcm_frames_before_generate},
                        {"native_samples_before_generate",t.native_samples_before_generate},
                        {"publish_begin_ns",t.publish_begin_ns},{"resample_end_ns",t.resample_end_ns},
                        {"publish_end_ns",t.publish_end_ns},
                        {"first_pcm_enqueued_ns",t.first_pcm_enqueued_ns},
                        {"last_pcm_enqueued_ns",t.last_pcm_enqueued_ns},
                        {"pcm_queue_wait_ns",t.pcm_queue_wait_ns},
                        {"first_pcm_seq",t.first_pcm_seq},{"last_pcm_seq",t.last_pcm_seq},
                        {"pcm_frames",t.pcm_frames}});
                }
                event(id, RealtimeEvent::Kind::complete, "completed",
                      {{"tts_frames", stats.frames}, {"tts_segments", stats.segments},
                        {"tts_segment_timing_schema",1},
                        {"tts_segment_timings",std::move(segment_timings)},
                        {"tts_segment_timings_dropped",stats.segment_timings_dropped},
                        {"tts_prebuffer_ms", config_.tts.prebuffer_ms},
                        {"tts_target_chars", config_.tts.target_chars},
                         {"tts_first_segment_override",config_.tts.first_segment.has_value()},
                         {"tts_first_min_chars",config_.tts.first_segment ? config_.tts.first_segment->min_chars : config_.tts.min_chars},
                         {"tts_first_target_chars",config_.tts.first_segment ? config_.tts.first_segment->target_chars : config_.tts.target_chars},
                         {"tts_first_boundary_lookahead_chars",config_.tts.first_segment ? config_.tts.first_segment->boundary_lookahead_chars : config_.tts.boundary_lookahead_chars},
                        {"tts_speed", config_.tts.speed},
                         {"tts_sentence_mode", config_.tts.sentence_mode},
                          {"tts_join_short_sentences", config_.tts.join_short_sentences},
                         {"tts_boundary_lookahead_chars", config_.tts.boundary_lookahead_chars},
                        {"tts_first_generated_audio_ns", stats.first_generated_audio_ns},
                        {"tts_playback_release_ns", stats.playback_release_ns},
                        {"tts_peak_native_slots", stats.peak_native_slots},
                        {"tts_peak_native_samples", stats.peak_native_samples},
                       {"tts_first_pcm_ns", stats.first_pcm_ns}, {"tts_text_peak_bytes", stats.peak_input_bytes},
                       {"tts_pcm_peak_frames", stats.peak_pcm_frames},
                       {"capture_peak_frames", capture_peak_},
                       {"llm_submit_ns", completed_llm.submit_ns},
                       {"llm_first_token_ns", completed_llm.first_token_ns},
                       {"llm_first_text_ns", completed_llm.first_text_ns},
                       {"speakable_text_ns", stats.first_segment_ns},
                       {"llm_idle_ns", completed_llm.idle_ns},
                       {"llm_token_callbacks", completed_llm.token_callbacks},
                       {"llm_queue_peak_bytes", completed_llm.queue_peak_bytes},
                       {"tts_begin_ns", stats.begin_ns},
                       {"tts_first_segment_ns", stats.first_segment_ns},
                       {"tts_generate_seconds", stats.generate_seconds},
                       {"tts_peak_segments", stats.peak_segments},
                       {"measurement_method", "software_observation"}});
                return;
            }
            std::unique_lock<std::mutex> lock(gate_);
            cv_.wait_for(lock, std::chrono::milliseconds(2), [&] { return !current(id); });
        }
        asr_call([&] { listening_->cancel(); });
    }
    void loop() {
        while (true) {
            AudioContext id;
            {
                std::unique_lock<std::mutex> lock(gate_);
                cv_.wait(lock, [&] { return stop_ || pending_; });
                if (stop_) return;
                pending_ = false; id = context_;
            }
            try { if (wake_mode_) wake_turn(id); else turn(id); }
            catch (const std::exception& error) {
                event(id, RealtimeEvent::Kind::error, "model_failure", {{"detail", error.what()}});
                // The main controller owns invalidation and publishes the error.
            }
            {
                std::lock_guard<std::mutex> lock(gate_); busy_ = false; cv_.notify_all();
            }
        }
    }
public:
    explicit ModelPipeline(const Config& c) : config_(c), budget_(realtime_queue_budget(c)) {
        probe::require_model_validation_lock();
        validate_realtime_config(c);
        validate_assets(c);
        const auto manifest_path = std::filesystem::canonical(c.source_path.parent_path() / c.assets.models_manifest);
        std::ifstream input(manifest_path);
        const auto manifest = nlohmann::json::parse(input);
        auto selected = [&](const std::string& id) {
            for (const auto& model : manifest.at("models")) if (model.at("id") == id) return model;
            throw ConfigError("model absent from validated manifest: " + id);
        };
        const auto asr = selected(c.models.asr.id), vad = selected(c.models.vad.id);
        auto path = [&](const nlohmann::json& model, const std::string& name) {
            bool declared = false;
            for (const auto& file : model.at("files")) if (file.at("path") == name) declared = true;
            const auto root = manifest_path.parent_path();
            const auto result = std::filesystem::canonical(root / name);
            auto relative = result.lexically_relative(root);
            if (!declared || relative.empty() || relative.is_absolute() || *relative.begin() == "..")
                throw ConfigError("model role outside validated files");
            return result;
        };
        auto role = [&](const std::string& key) { return path(asr, asr.at("roles").at(key).get<std::string>()); };
        asr_ = std::make_unique<ZipformerAsr>(ZipformerFiles{role("encoder"), role("decoder"), role("joiner"),
                                                            role("vocabulary"), role("feature_config")});
        auto listening_config = c.vad;
        if (c.dialogue.wake_enabled) listening_config.start_timeout_ms = c.dialogue.window_ms;
        vad_ = std::make_unique<SileroVad>(path(vad, vad.at("model_path").get<std::string>()), listening_config, c.threads.vad);
        if (c.dialogue.wake_enabled) kws_ = std::make_unique<KeywordSpotter>(c);
        if (c.dialogue.barge_in) {
            barge_detector_ = std::make_unique<BargeInDetector>(
                path(vad, vad.at("model_path").get<std::string>()), c.dialogue.barge_in_min_ms, c.threads.vad);
            barge_ = std::make_unique<BargeInMonitor>(
                [this] { barge_detector_->reset(); },
                [this](const float* p, std::size_t n) { return barge_detector_->accept(p, n); },
                c.timeouts.asr_final_ms);
        }
        model_ = std::make_unique<MeloTts>(c, c.threads.tts);
        auto options = llm_options_from_config(c);
        llm_ = std::make_unique<LlmAdapter>(make_rkllm_backend(resolve_rkllm_assets(c), options), options);
        listening_ = std::make_unique<ListeningAsr>(*asr_, *vad_, listening_config);
        auto tts_queues = c.queues;
        tts_queues.playback_ms = static_cast<int>(budget_.playback_tts_frames) * 20;
        tts_ = std::make_unique<TtsStreamWorker>([this](const std::string& text,
                const std::shared_ptr<TtsCancellation>& cancellation) {
            tts_call_ns_ = now_ns();
            try { auto pcm = model_->generate(text, cancellation); tts_call_ns_ = 0; return pcm; }
            catch (...) { tts_call_ns_ = 0; throw; }
        }, c.tts, tts_queues, c.timeouts.tts_segment_ms);
        worker_ = std::thread([this] { loop(); });
    }
    ~ModelPipeline() override {
        { std::lock_guard<std::mutex> lock(gate_); cancel_locked(); stop_ = true; cv_.notify_all(); }
        if (worker_.joinable()) worker_.join();
        llm_->close();
    }
    void begin(AudioContext id) override { begin_mode(id, false); }
    void begin_wake(AudioContext id) override {
        if (!kws_) throw ConfigError("KWS unavailable");
        begin_mode(id, true);
    }
    void begin_mode(AudioContext id, bool wake) {
        std::lock_guard<std::mutex> lock(gate_);
        if (busy_ || (barge_ && !barge_->idle()) || !llm_->idle() || !tts_->wait_idle(0))
            throw std::logic_error("begin requires all model workers idle");
        const bool handoff = wake_mode_ && !wake && current(id);
        if (!handoff) { capture_.clear(); capture_peak_ = 0; }
        context_ = id; wake_mode_ = wake; events_.clear();
        listening_deadline_ns_ = !wake && config_.dialogue.wake_enabled ?
            now_ns() + std::int64_t(config_.dialogue.window_ms) * 1000000 : 0;
        valid_ = busy_ = pending_ = accepting_ = true;
        input_end_ = responding_ = have_llm_ = completed_ = watchdog_reported_ = false;
        cv_.notify_all();
    }
    void begin_barge_in(AudioContext id) override {
        std::lock_guard<std::mutex> lock(gate_);
        if (!barge_ || !current(id)) throw std::logic_error("invalid barge-in begin");
        barge_->begin(id);
    }
    void end_barge_in() override { if (barge_) barge_->stop(); }
    bool can_accept_barge_in() const override { return barge_ && barge_->can_accept(); }
    bool submit_barge_in(const AudioFrame& f) override { return barge_ && barge_->submit(f); }
    std::optional<BargeInSignal> poll_barge_in() override { return barge_ ? barge_->poll() : std::nullopt; }
    bool can_accept_capture() const override {
        std::lock_guard<std::mutex> lock(gate_);
        return valid_ && (accepting_ ? capture_.size() < budget_.capture_model_frames : !wake_mode_);
    }
    bool submit(const AudioFrame& frame) override {
        std::lock_guard<std::mutex> lock(gate_);
        if (!current(frame.context)) return false;
        if (!accepting_) return true; // PCM following an automatic endpoint is intentionally discarded.
        if (frame.valid_samples < 1 || frame.valid_samples > frame.samples.size() ||
            frame.samples.size() != 320 || frame.sample_rate != 16000 || frame.channels != 1)
            throw AudioError("invalid capture frame");
        if (capture_.size() >= budget_.capture_model_frames) return false;
        capture_.push_back(frame); capture_peak_ = std::max(capture_peak_, capture_.size()); cv_.notify_all(); return true;
    }
    void finish_input() override { std::lock_guard<std::mutex> lock(gate_); input_end_ = true; cv_.notify_all(); }
    void cancel() override { std::lock_guard<std::mutex> lock(gate_); cancel_locked(); }
    void new_session() override { std::lock_guard<std::mutex> lock(gate_); llm_->new_session(); }
    bool idle() const override {
        std::lock_guard<std::mutex> lock(gate_);
        return !busy_ && (!barge_ || barge_->idle()) && llm_->idle() && tts_->wait_idle(0) && !llm_->restart_required();
    }
    nlohmann::json diagnostics() const override {
        std::lock_guard<std::mutex> lock(gate_);
        const auto now = now_ns(), asr_started = asr_call_ns_.load(), tts_started = tts_call_ns_.load();
        const auto tts = tts_->stats();
        nlohmann::json result = {{"barge_in_enabled", config_.dialogue.barge_in}, {"barge_in_idle", !barge_ || barge_->idle()}, {"wake_mode", wake_mode_}, {"listening_deadline_ns", listening_deadline_ns_}, {"kws_call_started_ns", kws_call_ns_.load()}, {"worker_busy", busy_}, {"generation_valid", valid_},
            {"accepting_audio", accepting_}, {"responding", responding_},
            {"capture_queued_frames", capture_.size()}, {"capture_peak_frames", capture_peak_},
            {"llm_idle", llm_->idle()}, {"llm_restart_required", llm_->restart_required()},
            {"tts_idle", tts_->wait_idle(0)}, {"tts_status", tts.status},
            {"tts_queued_frames", tts.current_pcm_frames}, {"tts_peak_frames", tts.peak_pcm_frames},
            {"tts_prebuffer_ms", config_.tts.prebuffer_ms},
                        {"tts_target_chars", config_.tts.target_chars},
                         {"tts_first_segment_override",config_.tts.first_segment.has_value()},
                         {"tts_first_min_chars",config_.tts.first_segment ? config_.tts.first_segment->min_chars : config_.tts.min_chars},
                         {"tts_first_target_chars",config_.tts.first_segment ? config_.tts.first_segment->target_chars : config_.tts.target_chars},
                         {"tts_first_boundary_lookahead_chars",config_.tts.first_segment ? config_.tts.first_segment->boundary_lookahead_chars : config_.tts.boundary_lookahead_chars},
                        {"tts_speed", config_.tts.speed},
                         {"tts_sentence_mode", config_.tts.sentence_mode},
                          {"tts_join_short_sentences", config_.tts.join_short_sentences},
                         {"tts_boundary_lookahead_chars", config_.tts.boundary_lookahead_chars},
            {"tts_native_run_started_ns", tts.native_run_started_ns},
            {"tts_native_runs", tts.native_runs},
            {"tts_native_buffered_segments", tts.native_buffered_segments},
            {"tts_native_buffered_samples", tts.native_buffered_samples},
            {"tts_peak_native_slots", tts.peak_native_slots},
            {"tts_peak_native_samples", tts.peak_native_samples},
            {"tts_first_generated_audio_ns", tts.first_generated_audio_ns},
            {"tts_playback_release_ns", tts.playback_release_ns},
            {"asr_call_started_ns", asr_started}, {"tts_call_started_ns", tts_started},
            {"asr_call_elapsed_ms", asr_started ? (now - asr_started) / 1e6 : 0.0},
            {"tts_call_elapsed_ms", tts_started ? (now - tts_started) / 1e6 : 0.0}};
        if (have_llm_) {
            const auto llm = llm_->snapshot(turn_);
            result["llm_status"] = llm.status;
            result["llm_queued_packets"] = llm.queue_current_packets;
            result["llm_queued_bytes"] = llm.queue_current_bytes;
            result["llm_queue_peak_bytes"] = llm.queue_peak_bytes;
            result["llm_cancel_requested_ns"] = llm.cancel_requested_ns;
            result["llm_idle_ns"] = llm.idle_ns;
        }
        return result;
    }
    std::optional<RealtimeEvent> poll() override {
        std::lock_guard<std::mutex> lock(gate_);
        if (!valid_) return {};
        const auto now = now_ns(), asr_started = asr_call_ns_.load(), tts_started = tts_call_ns_.load();
        const auto kws_started = kws_call_ns_.load();
        if (!watchdog_reported_ && ((kws_started && now - kws_started >
            std::int64_t(config_.timeouts.asr_final_ms) * 1000000) || (asr_started && now - asr_started >
            std::int64_t(config_.timeouts.asr_final_ms) * 1000000) ||
            (tts_started && now - tts_started > std::int64_t(config_.timeouts.tts_segment_ms) * 1000000))) {
            watchdog_reported_ = true;
            RealtimeEvent e; e.context = context_; e.kind = RealtimeEvent::Kind::error;
            e.reason = kws_started ? "kws_call_timeout" : asr_started ? "asr_call_timeout" : "tts_segment_timeout"; return e;
        }
        if (!events_.empty()) { auto e = std::move(events_.front()); events_.pop_front(); return e; }
        if (responding_) if (auto pcm = tts_->poll(tts_id(), 0)) {
            RealtimeEvent e; e.context = context_; e.kind = RealtimeEvent::Kind::pcm;
            e.frame.context = context_; e.frame.seq = pcm->seq; e.frame.monotonic_ns = now;
            e.frame.samples.assign(pcm->audio.samples.begin(), pcm->audio.samples.end());
            e.frame.valid_samples = pcm->audio.valid_samples;
            return e;
        }
        return {};
    }
    bool commit_if_current(AudioContext id, const std::function<void()>& sink) override {
        std::lock_guard<std::mutex> lock(gate_);
        if (!current(id) || !have_llm_) return false;
        bool accepted = false;
        const bool current_llm = llm_->commit_if_current(turn_, [&] {
            accepted = tts_->commit_if_current(tts_id(), sink);
        });
        return current_llm && accepted;
    }
    bool commit_history(AudioContext id) override {
        std::lock_guard<std::mutex> lock(gate_);
        return current(id) && completed_ && have_llm_ && !busy_ && llm_->commit_history(turn_);
    }
};
}
void validate_realtime_config(const Config& c) {
    realtime_queue_budget(c);
    if (!c.models.vad.enabled || !c.models.asr.enabled || !c.models.llm.enabled || !c.models.tts.enabled)
        throw ConfigError("realtime requires VAD/ASR/LLM/TTS enabled");
    if (c.models.kws.enabled != c.dialogue.wake_enabled)
        throw ConfigError("realtime KWS and wake_enabled must be enabled together");
    if (c.dialogue.wake_enabled && c.dialogue.keywords_file.empty())
        throw ConfigError("wake mode requires dialogue.keywords_file");
    if (c.audio.profile == "file-only") throw ConfigError("realtime requires real audio source/sink/profile");
}
std::unique_ptr<RealtimePipeline> make_realtime_pipeline(const Config& config) {
    return std::make_unique<ModelPipeline>(config);
}
} // namespace voice_engine
