#include "voice_engine/audio_dsp.hpp"

#include <samplerate.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace voice_engine {
namespace {

constexpr int kMinRate = 8000;
constexpr int kMaxRate = 192000;
constexpr int kMaxChannels = 8;
constexpr std::size_t kTailCapacity = 8192;
constexpr std::size_t kMaxAssemblerInput = 32000;

void check_samples(const float* samples, std::size_t count) {
    if (count != 0 && samples == nullptr) {
        throw AudioError("audio DSP: nonempty input has a null sample pointer");
    }
    for (std::size_t index = 0; index < count; ++index) {
        if (!std::isfinite(samples[index]) || samples[index] < -1.0F || samples[index] > 1.0F) {
            throw AudioError("audio DSP: input samples must be finite normalized floats in [-1,1]");
        }
    }
}

void check_src(int error, const char* operation) {
    if (error != 0) {
        throw AudioError(std::string("audio DSP: ") + operation + ": " + src_strerror(error));
    }
}

}  // namespace

struct StreamResampler::Impl {
    int input_rate;
    int output_rate;
    int input_channels;
    int output_channels;
    double ratio;
    SRC_STATE* state = nullptr;
    bool finished = false;

    Impl(int input, int output, int input_ch, int output_ch)
        : input_rate(input), output_rate(output), input_channels(input_ch),
          output_channels(output_ch), ratio(1.0) {
        if (input < kMinRate || input > kMaxRate || output < kMinRate || output > kMaxRate) {
            throw AudioError("audio DSP: sample rates must be within 8000..192000 Hz");
        }
        if (input_ch < 1 || input_ch > kMaxChannels || output_ch < 1 || output_ch > kMaxChannels) {
            throw AudioError("audio DSP: channel counts must be within 1..8");
        }
        ratio = static_cast<double>(output) / input;
        if (input != output) {
            int error = 0;
            state = src_new(SRC_SINC_FASTEST, 1, &error);
            if (state == nullptr) {
                check_src(error, "create converter");
                throw AudioError("audio DSP: converter allocation failed");
            }
            // Fix the ratio immediately instead of ramping between block ratios.
            error = src_set_ratio(state, ratio);
            if (error != 0) {
                state = src_delete(state);
                check_src(error, "set conversion ratio");
            }
        }
    }

    ~Impl() {
        if (state != nullptr) {
            src_delete(state);
        }
    }

    std::vector<float> interleave(const float* mono, std::size_t count) const {
        std::vector<float> output(count * static_cast<std::size_t>(output_channels));
        for (std::size_t index = 0; index < count; ++index) {
            if (!std::isfinite(mono[index])) {
                throw AudioError("audio DSP: resampler produced a non-finite sample");
            }
            const float normalized = std::clamp(mono[index], -1.0F, 1.0F);
            std::fill_n(output.data() + index * static_cast<std::size_t>(output_channels),
                        output_channels, normalized);
        }
        return output;
    }

    std::vector<float> convert(const float* mono, std::size_t count, bool final) {
        if (state == nullptr) {
            return interleave(mono, count);
        }
        // One second plus a generous fixed filter-tail allowance. The allowed
        // rate ratio is at most 24; SRC_SINC_FASTEST has a much shorter tail.
        // The checked capacity also prevents accidental unbounded accumulation.
        const std::size_t capacity = static_cast<std::size_t>(std::ceil(count * ratio)) + kTailCapacity;
        std::vector<float> converted(capacity);
        std::size_t consumed = 0;
        std::size_t produced = 0;
        float empty_input = 0.0F;
        do {
            SRC_DATA data{};
            data.data_in = count == 0 ? &empty_input : mono + consumed;
            data.input_frames = static_cast<long>(count - consumed);
            data.data_out = converted.data() + produced;
            data.output_frames = static_cast<long>(capacity - produced);
            data.src_ratio = ratio;
            data.end_of_input = final ? 1 : 0;
            check_src(src_process(state, &data), "convert stream");
            if (data.input_frames_used < 0 || data.output_frames_gen < 0 ||
                static_cast<std::size_t>(data.input_frames_used) > count - consumed ||
                static_cast<std::size_t>(data.output_frames_gen) > capacity - produced) {
                throw AudioError("audio DSP: converter returned invalid buffer counts");
            }
            consumed += static_cast<std::size_t>(data.input_frames_used);
            produced += static_cast<std::size_t>(data.output_frames_gen);
            if (produced == capacity) {
                throw AudioError("audio DSP: converter exceeded the bounded output buffer");
            }
            if (consumed == count && (!final || data.output_frames_gen == 0)) {
                break;
            }
            if (data.input_frames_used == 0 && data.output_frames_gen == 0) {
                throw AudioError("audio DSP: converter did not consume its input");
            }
        } while (true);
        return interleave(converted.data(), produced);
    }
};

StreamResampler::StreamResampler(int input_rate, int output_rate,
                               int input_channels, int output_channels)
    : impl_(std::make_unique<Impl>(input_rate, output_rate, input_channels, output_channels)) {}

StreamResampler::~StreamResampler() = default;

std::vector<float> StreamResampler::process(const float* interleaved, std::size_t input_frames) {
    if (impl_->finished) {
        throw AudioError("audio DSP: process after finish requires reset");
    }
    if (input_frames > static_cast<std::size_t>(impl_->input_rate)) {
        throw AudioError("audio DSP: each input block is limited to one second");
    }
    check_samples(interleaved, input_frames * static_cast<std::size_t>(impl_->input_channels));
    if (input_frames == 0) {
        return {};
    }
    std::vector<float> mono(input_frames);
    for (std::size_t frame = 0; frame < input_frames; ++frame) {
        double sum = 0.0;
        for (int channel = 0; channel < impl_->input_channels; ++channel) {
            sum += interleaved[frame * static_cast<std::size_t>(impl_->input_channels) + channel];
        }
        mono[frame] = static_cast<float>(sum / impl_->input_channels);
    }
    return impl_->convert(mono.data(), mono.size(), false);
}

std::vector<float> StreamResampler::finish() {
    if (impl_->finished) {
        return {};
    }
    auto tail = impl_->convert(nullptr, 0, true);
    impl_->finished = true;
    return tail;
}

void StreamResampler::reset() {
    if (impl_->state != nullptr) {
        check_src(src_reset(impl_->state), "reset converter");
        check_src(src_set_ratio(impl_->state, impl_->ratio), "reset conversion ratio");
    }
    impl_->finished = false;
}

std::vector<MonoFrame> MonoFrameAssembler::push(const float* mono, std::size_t sample_count) {
    if (finished_) {
        throw AudioError("audio DSP: push after finish requires reset");
    }
    if (sample_count > kMaxAssemblerInput) {
        throw AudioError("audio DSP: frame assembler input is limited to two seconds");
    }
    check_samples(mono, sample_count);
    std::vector<MonoFrame> frames;
    frames.reserve((pending_count_ + sample_count) / pending_.size());
    std::size_t consumed = 0;
    while (consumed < sample_count) {
        const auto count = std::min(pending_.size() - pending_count_, sample_count - consumed);
        std::copy_n(mono + consumed, count, pending_.data() + pending_count_);
        consumed += count;
        pending_count_ += count;
        if (pending_count_ == pending_.size()) {
            frames.push_back(pending_);
            pending_count_ = 0;
        }
    }
    return frames;
}

std::optional<PaddedAudioFrame> MonoFrameAssembler::finish() {
    if (finished_) {
        return std::nullopt;
    }
    finished_ = true;
    if (pending_count_ == 0) {
        return std::nullopt;
    }
    PaddedAudioFrame tail;
    tail.valid_samples = pending_count_;
    std::copy_n(pending_.data(), pending_count_, tail.samples.data());
    pending_count_ = 0;
    return tail;
}

void MonoFrameAssembler::reset() noexcept {
    pending_.fill(0.0F);
    pending_count_ = 0;
    finished_ = false;
}

std::size_t MonoFrameAssembler::pending_samples() const noexcept {
    return pending_count_;
}

}  // namespace voice_engine
