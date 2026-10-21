#pragma once

#include "voice_engine/config.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <vector>

namespace voice_engine {

// Transport chunks (including 320-sample engine frames) are assembled into
// Silero's 512 NEW samples. The fixed runtime owns its 64-sample context.
class VadWindowAssembler {
public:
    using Sink = std::function<void(const float*, std::size_t valid)>;

    void accept(const float* pcm, std::size_t count, const Sink& sink) {
        if (finished_) throw std::logic_error("VAD input already finished");
        if (count && !pcm) throw std::invalid_argument("null PCM");
        if (count > 16000) throw std::length_error("VAD accept block exceeds 1 second");
        for (std::size_t i = 0; i < count; ++i) {
            if (!std::isfinite(pcm[i]) || std::abs(pcm[i]) > 1.001f) {
                throw std::invalid_argument("PCM must be finite normalized float32");
            }
        }
        while (count) {
            const auto take = std::min(count, 512 - used_);
            std::copy_n(pcm, take, buffer_.begin() + used_);
            used_ += take;
            pcm += take;
            count -= take;
            if (used_ == 512) {
                sink(buffer_.data(), 512);
                used_ = 0;
            }
        }
    }

    void finish(const Sink& sink) {
        if (finished_) return;
        if (used_) {
            std::fill(buffer_.begin() + used_, buffer_.end(), 0);
            sink(buffer_.data(), used_);
            used_ = 0;
        }
        finished_ = true;
    }

    void reset() {
        used_ = 0;
        finished_ = false;
        buffer_.fill(0);
    }

    std::size_t buffered() const { return used_; }

private:
    std::array<float, 512> buffer_{};
    std::size_t used_ = 0;
    bool finished_ = false;
};

struct VadSegment {
    std::uint64_t start_sample = 0;
    std::uint64_t end_sample = 0;
    std::uint64_t pre_roll_start_sample = 0;
};

struct VadStats {
    std::uint64_t input_samples = 0;
    std::uint64_t windows = 0;
    std::uint64_t tail_valid_samples = 0;
    std::uint64_t max_duration_endpoints = 0;
    bool speech_detected = false;
    bool start_timed_out = false;
};

class SileroVad {
public:
    SileroVad(const std::filesystem::path& model, const VadConfig&, int threads);
    ~SileroVad();
    SileroVad(const SileroVad&) = delete;
    SileroVad& operator=(const SileroVad&) = delete;
    void accept(const float*, std::size_t);
    void finish();
    void reset();
    VadStats stats() const;
    std::string runtime_info_json() const;
    const std::vector<VadSegment>& segments() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace voice_engine
