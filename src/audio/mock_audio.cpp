#include "voice_engine/audio.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>

namespace voice_engine {
namespace {

class MockAudio final : public AudioBackend {
public:
    explicit MockAudio(std::size_t playback_capacity_frames)
        : playback_capacity_frames_(playback_capacity_frames) {
        if (playback_capacity_frames_ == 0 || playback_capacity_frames_ > 500) {
            throw AudioError("mock playback capacity must be 1..500 frames");
        }
    }

    void start_capture(AudioContext context) override {
        playback_.clear();
        context_ = context;
        context_valid_ = true;
        capturing_ = true;
        seq_ = 0;
    }

    void stop_capture() override { capturing_ = false; }

    void cancel() override {
        capturing_ = false;
        context_valid_ = false;
        seq_ = 0;
        playback_.clear();
    }

    bool capturing() const override { return capturing_; }

    AudioFrame capture_frame() override {
        if (!capturing_) {
            throw AudioError("mock capture is not running");
        }
        AudioFrame frame;
        frame.context = context_;
        frame.seq = ++seq_;
        frame.monotonic_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        frame.samples.assign(320, 0.0F);
        return frame;
    }

    void submit_playback(const AudioFrame& frame) override {
        if (!context_valid_ || frame.context.session_id != context_.session_id ||
            frame.context.generation_id != context_.generation_id) {
            throw AudioError("mock playback frame has an inactive session or generation");
        }
        if (frame.sample_rate != 16000 || frame.channels != 1 ||
            frame.samples.size() != 320 || frame.valid_samples == 0 ||
            frame.valid_samples > frame.samples.size()) {
            throw AudioError("mock playback requires 320 mono float32 samples at 16000 Hz");
        }
        if (std::any_of(frame.samples.begin(), frame.samples.end(),
                        [](float sample) {
                            return !std::isfinite(sample) || sample < -1.0F || sample > 1.0F;
                        })) {
            throw AudioError("mock playback frame samples must be normalized finite floats in [-1,1]");
        }
        if (std::any_of(frame.samples.begin() + frame.valid_samples, frame.samples.end(),
                        [](float sample) { return sample != 0.0F; })) {
            throw AudioError("mock playback padding must contain zero samples");
        }
        if (playback_.size() >= playback_capacity_frames_) {
            throw AudioError("mock playback queue is full");
        }
        playback_.push_back(frame);
    }

    void flush_playback() override { playback_.clear(); }

private:
    std::size_t playback_capacity_frames_;
    AudioContext context_;
    std::uint64_t seq_ = 0;
    bool context_valid_ = false;
    bool capturing_ = false;
    std::deque<AudioFrame> playback_;
};

}  // namespace

std::unique_ptr<AudioBackend> make_mock_audio(std::size_t playback_capacity_frames) {
    return std::make_unique<MockAudio>(playback_capacity_frames);
}

}  // namespace voice_engine
