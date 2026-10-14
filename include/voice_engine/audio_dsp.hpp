#pragma once

#include "voice_engine/audio.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace voice_engine {

// Stateful, single-caller conversion of normalized float PCM. Input channels
// are averaged to mono; mono output is duplicated into each output channel.
// Rates are 8000..192000 Hz and channel counts are 1..8. Each process() call
// accepts at most one second of input; its second argument counts FRAMES,
// not interleaved samples. A nonempty input requires frames * input_channels
// readable samples, each finite and in [-1, 1]. Filter overshoot is clipped
// to that same range, without scaling quiet signals or changing their gain.
//
// process() preserves filter history across calls. finish() emits the delayed
// filter tail and seals the stream; repeated finish() calls return no samples.
// reset() drops all history, reopens a finished stream, and is the operation
// to use for cancellation, device reconnects, and generation changes.
class StreamResampler {
public:
    StreamResampler(int input_rate, int output_rate,
                    int input_channels = 1, int output_channels = 1);
    ~StreamResampler();
    StreamResampler(const StreamResampler&) = delete;
    StreamResampler& operator=(const StreamResampler&) = delete;

    std::vector<float> process(const float* interleaved, std::size_t input_frames);
    std::vector<float> finish();
    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

using MonoFrame = std::array<float, 320>;

struct PaddedAudioFrame {
    MonoFrame samples{};
    std::size_t valid_samples = 0;
};

// Split 16 kHz mono float PCM into 20 ms frames, retaining at most 319 samples
// between calls. A push is limited to 32000 samples (two seconds); samples
// have the same finite [-1,1] contract as StreamResampler. finish() optionally
// returns the zero-padded final frame with its real sample count, and seals
// the assembler. Cancellation uses reset() to discard the unfinished frame.
class MonoFrameAssembler {
public:
    std::vector<MonoFrame> push(const float* mono, std::size_t sample_count);
    std::optional<PaddedAudioFrame> finish();
    void reset() noexcept;
    std::size_t pending_samples() const noexcept;

private:
    MonoFrame pending_{};
    std::size_t pending_count_ = 0;
    bool finished_ = false;
};

}  // namespace voice_engine
