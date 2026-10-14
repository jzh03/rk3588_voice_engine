#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

namespace voice_engine {

struct AudioContext {
    std::uint64_t session_id = 1;
    std::uint64_t generation_id = 0;
};

// Internal frames use 16 kHz, mono float32: 320 samples are 20 ms.
struct AudioFrame {
    AudioContext context;
    std::uint64_t seq = 0;
    std::int64_t monotonic_ns = 0;
    int sample_rate = 16000;
    int channels = 1;
    std::vector<float> samples;
};

class AudioError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class AudioBackend {
public:
    virtual ~AudioBackend() = default;

    // Starting a generation clears all previous capture/playback state.
    virtual void start_capture(AudioContext context) = 0;
    // Finishing capture keeps this context available for playback.
    virtual void stop_capture() = 0;
    // Cancellation invalidates the context and drops pending playback.
    virtual void cancel() = 0;
    virtual bool capturing() const = 0;
    virtual AudioFrame capture_frame() = 0;
    virtual void submit_playback(const AudioFrame& frame) = 0;
    virtual void flush_playback() = 0;
};

// Single-caller, in-memory mock: generates silence and never opens devices.
// Playback capacity defaults to 2 s; valid capacities are 1..500 frames (10 s).
// A full queue, stale context or malformed frame produces AudioError.
std::unique_ptr<AudioBackend> make_mock_audio(std::size_t playback_capacity_frames = 100);

}  // namespace voice_engine
