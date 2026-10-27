#pragma once
#include "voice_engine/config.hpp"
#include "voice_engine/barge_in.hpp"
#include "voice_engine/live_audio.hpp"
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <nlohmann/json.hpp>
namespace voice_engine {
enum class RealtimeState { starting, idle, listening, finalizing, responding, cancelling, device_wait, error, stopping };
const char* realtime_state_name(RealtimeState) noexcept;
struct RealtimeEvent {
    enum class Kind { wake, endpoint, asr_final, pcm, complete, error };
    Kind kind = Kind::error;
    AudioContext context;
    AudioFrame frame;
    std::string reason;
    nlohmann::json data = nlohmann::json::object();
};
// Calls are nonblocking except construction/destruction; one control caller.
// cancel immediately invalidates output. idle confirms model calls and resets
// finished. One polled PCM frame can be retained under downstream backpressure.
class RealtimePipeline {
public:
    virtual ~RealtimePipeline() = default;
    virtual void begin(AudioContext) = 0;
    virtual void begin_wake(AudioContext) { throw ConfigError("KWS unavailable in this pipeline"); }
    virtual void begin_barge_in(AudioContext) { throw ConfigError("barge-in unavailable"); }
    virtual void end_barge_in() {}
    virtual bool can_accept_barge_in() const { return false; }
    virtual bool submit_barge_in(const AudioFrame&) { return false; }
    virtual std::optional<BargeInSignal> poll_barge_in() { return {}; }
    virtual bool can_accept_capture() const = 0;
    virtual bool submit(const AudioFrame&) = 0;
    virtual void finish_input() = 0;
    virtual void cancel() = 0;
    virtual void new_session() = 0;
    virtual bool idle() const = 0;
    virtual nlohmann::json diagnostics() const { return nlohmann::json::object(); }
    virtual std::optional<RealtimeEvent> poll() = 0;
    // Atomic generation validation + quick nonblocking downstream enqueue.
    virtual bool commit_if_current(AudioContext, const std::function<void()>&) = 0;
    virtual bool commit_history(AudioContext) = 0;
};
struct RealtimeQueueBudget {
    std::size_t capture_audio_frames = 0, capture_model_frames = 1, capture_preroll_frames = 0;
    std::size_t playback_audio_frames = 0, playback_tts_frames = 0, playback_retained_frames = 1;
};
// Shared application queue limits, not per-stage limits. PCM temporarily owned
// by a synchronous TTS Generate call has its separate max_tts_audio_ms bound.
RealtimeQueueBudget realtime_queue_budget(const Config&);
struct RealtimeSnapshot {
    RealtimeState state = RealtimeState::starting;
    AudioContext context;
    bool pending_start = false, stopping = false, wake_listening = false;
    int exit_code = 0;
};
class RealtimeController {
public:
    using Log = std::function<void(std::string_view, AudioContext, RealtimeState, const nlohmann::json&)>;
    RealtimeController(const Config&, LiveAudioBackend&, RealtimePipeline&, Log = {});
    ~RealtimeController();
    // now_ns is monotonic and injectable; memory_kib < 0 fails closed.
    void tick(std::int64_t now_ns, std::int64_t memory_kib);
    bool command(std::string_view command, std::int64_t now_ns);
    void shutdown(std::int64_t now_ns);
    RealtimeSnapshot snapshot() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Structural real-mode preflight; no files, model initialization, or device I/O.
void validate_realtime_config(const Config&);
std::unique_ptr<RealtimePipeline> make_realtime_pipeline(const Config&);
int run_realtime_control(const Config&, const std::string& run_id,
                         const std::filesystem::path& output_root);
} // namespace voice_engine
