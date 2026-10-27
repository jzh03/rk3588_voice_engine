#pragma once

#include "voice_engine/audio.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <functional>
#include <string>
#include <vector>

namespace voice_engine {

enum class LiveAudioState { device_wait, ready, capturing };
const char* live_audio_state_name(LiveAudioState state) noexcept;

struct LiveAudioOptions {
    std::string source;
    std::string sink;
    std::string profile;
    std::size_t capture_capacity_frames = 100;
    std::size_t playback_capacity_frames = 100;
    bool capture_enabled = true;
    bool playback_enabled = true;
    // Independent zero-PCM stream, including idle/cancel/drain; playback only.
    bool playback_keepalive = false;
    // Native-rate silence before each non-empty session; lazily generated
    // without delaying control calls or adding queued speech frames.
    int playback_lead_in_ms = 0;  // 0..2000 ms, multiple of 20
};

struct LiveAudioBuffer {
    std::uint32_t maxlength = 0;
    std::uint32_t tlength = 0;
    std::uint32_t prebuf = 0;
    std::uint32_t minreq = 0;
    std::uint32_t fragsize = 0;
};

struct LiveAudioStatus {
    LiveAudioState state = LiveAudioState::device_wait;
    bool session_active = false;
    bool capturing = false;
    bool capture_stopping = false;
    bool reset_pending = false;
    bool drained = true;
    AudioContext context;
    std::string source;
    std::string sink;
    std::string card;
    std::string profile;
    std::string source_identity;
    std::string sink_identity;
    std::string capture_device_format;
    std::string playback_device_format;
    int capture_rate = 0;
    int capture_channels = 0;
    int playback_rate = 0;
    int playback_channels = 0;
    LiveAudioBuffer capture_buffer;
    LiveAudioBuffer playback_buffer;
    LiveAudioBuffer playback_keepalive_buffer;
    // Server uncork acknowledgement and non-suspended transport, not acoustic readiness.
    bool playback_keepalive_active = false;
    std::uint64_t playback_keepalive_native_frames = 0;
    std::uint64_t playback_keepalive_underruns = 0;
    std::size_t capture_queued_frames = 0;
    std::size_t playback_queued_frames = 0;
    std::size_t capture_queue_peak_frames = 0;
    std::size_t playback_queue_peak_frames = 0;
    std::size_t raw_capture_peak_bytes = 0;
    std::uint64_t capture_native_frames = 0;
    std::int64_t device_available_ns = 0;
    std::int64_t ready_ns = 0;
    std::uint64_t capture_frames = 0;
    std::uint64_t playback_frames = 0;
    std::uint64_t capture_overflows = 0;
    std::uint64_t capture_holes = 0;
    std::uint64_t playback_overflows = 0;
    std::uint64_t playback_underruns = 0;
    // Software worker observations; excludes DEVICE_WAIT and is not acoustic timing.
    std::int64_t output_service_max_gap_ns = 0;
    std::int64_t capture_service_max_work_ns = 0;
    std::uint64_t stale_rejections = 0;
    std::uint64_t reconnects = 0;
    std::uint64_t dropped_events = 0;
};

struct LiveAudioEvent {
    std::string type;
    std::string detail;
    std::int64_t monotonic_ns = 0;
    AudioContext context;
};

// One background worker owns libpulse. These calls never wait for server I/O.
// Server events and application commands wake its fd poll immediately; capture
// DSP is bounded per pass and interleaved with speech/keepalive transport work.
// A successful start clears the old generation and asynchronously flushes both
// speech/capture streams; reset_pending stays true until both flushes are acknowledged.
// Optional silent keepalive is independent and survives generation changes.
// Destruction or DEVICE_WAIT releases every stream, including keepalive.
// DEVICE_WAIT cancels the session. Recovery returns READY and requires a new
// explicit start_session; it never resumes old recording or PCM playback.
class LiveAudioBackend {
public:
    virtual ~LiveAudioBackend() = default;
    virtual bool start_session(AudioContext context, bool capture = true) = 0;
    // Normal stop acknowledges corking, consumes buffered input and emits the
    // resampler/assembler tail. Poll until capture_stopping is false, consuming
    // queued frames in the meantime. cancel() instead discards all input.
    virtual void stop_capture() = 0;
    virtual void cancel() = 0;
    virtual bool try_capture_frame(AudioFrame& frame) = 0;
    // Invalid frames throw AudioError; a full queue or inactive session returns
    // false. Frames belonging to another context are counted and rejected.
    virtual bool try_submit_playback(const AudioFrame& frame) = 0;
    // After the producer submits its last frame, finish the resampler tail and
    // drain normally. cancel() instead flushes and invalidates pending PCM.
    virtual void request_drain() = 0;
    virtual bool playback_drained() const = 0;
    // Atomically validate a normal drain and commit application history while
    // device/session state is locked. Callback must not wait or reenter audio.
    virtual bool commit_if_drained(AudioContext, const std::function<bool()>&) = 0;
    virtual LiveAudioStatus status() const = 0;
    virtual std::vector<LiveAudioEvent> poll_events() = 0;
};

// Explicit names only: no default device or automatic fallback. A real card
// must match profile. "none" is reserved for a pair of cardless virtual devices.
std::unique_ptr<LiveAudioBackend> make_pulse_audio(const LiveAudioOptions& options);

}  // namespace voice_engine
