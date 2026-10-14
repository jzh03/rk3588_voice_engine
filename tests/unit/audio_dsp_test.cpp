#include "voice_engine/audio_dsp.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Action>
void require_audio_error(Action action, const std::string& message) {
    try {
        action();
    } catch (const voice_engine::AudioError&) {
        return;
    }
    throw std::runtime_error(message);
}

void append(std::vector<float>& result, const std::vector<float>& block) {
    result.insert(result.end(), block.begin(), block.end());
}

std::vector<float> convert_blocks(voice_engine::StreamResampler& converter,
                                  const std::vector<float>& source, int channels,
                                  const std::vector<std::size_t>& block_sizes) {
    std::vector<float> result;
    std::size_t offset = 0;
    std::size_t next_block = 0;
    const auto frames = source.size() / static_cast<std::size_t>(channels);
    while (offset < frames) {
        const auto count = std::min(block_sizes[next_block++ % block_sizes.size()], frames - offset);
        append(result, converter.process(source.data() + offset * channels, count));
        offset += count;
    }
    append(result, converter.finish());
    return result;
}

void check_arbitrary_framing_and_cancel() {
    std::vector<float> input(1003);
    for (std::size_t index = 0; index < input.size(); ++index) {
        input[index] = static_cast<float>(index % 97) / 100.0F;
    }
    voice_engine::MonoFrameAssembler assembler;
    std::vector<float> output;
    std::size_t offset = 0;
    for (const std::size_t count : {1U, 318U, 2U, 401U, 281U}) {
        for (const auto& frame : assembler.push(input.data() + offset, count)) {
            require(frame.size() == 320, "capture frame is not 20 ms");
            output.insert(output.end(), frame.begin(), frame.end());
        }
        offset += count;
        require(assembler.pending_samples() < 320, "assembler retained an unbounded residual");
    }
    require(output.size() == 960 && assembler.pending_samples() == 43,
            "frame boundaries dropped or duplicated samples");
    const auto tail = assembler.finish();
    require(tail && tail->valid_samples == 43, "final partial frame has the wrong valid size");
    output.insert(output.end(), tail->samples.begin(), tail->samples.begin() + tail->valid_samples);
    require(output == input, "arbitrary blocks changed sample order or values");
    require(std::all_of(tail->samples.begin() + tail->valid_samples, tail->samples.end(),
                        [](float value) { return value == 0.0F; }),
            "final frame padding contains stale audio");
    require(!assembler.finish(), "finish returned the tail twice");
    require_audio_error([&] { assembler.push(input.data(), 1); }, "finished assembler accepted input");

    assembler.reset();
    assembler.push(input.data(), 100);
    assembler.reset();  // A cancellation must discard the old generation's partial frame.
    const std::vector<float> silence(320, 0.0F);
    const auto frames = assembler.push(silence.data(), silence.size());
    require(frames.size() == 1 &&
            std::all_of(frames.front().begin(), frames.front().end(),
                        [](float value) { return value == 0.0F; }),
            "cancelled residual leaked into a new frame");
    require(!assembler.finish(), "aligned input unexpectedly produced an extra frame");
}

void check_capture_downmix_and_streaming() {
    std::vector<float> stereo(48000 * 2);
    for (std::size_t frame = 0; frame < 48000; ++frame) {
        const float wave = static_cast<float>(std::sin(2.0 * kPi * 500.0 * frame / 48000.0));
        stereo[frame * 2] = 0.4F * wave;
        stereo[frame * 2 + 1] = 0.2F * wave;
    }
    voice_engine::StreamResampler whole(48000, 16000, 2, 1);
    voice_engine::StreamResampler chunked(48000, 16000, 2, 1);
    const auto reference = convert_blocks(whole, stereo, 2, {48000});
    const auto output = convert_blocks(chunked, stereo, 2, {1, 37, 960, 5, 4000});
    require(output.size() >= 15999 && output.size() <= 16001,
            "48 kHz to 16 kHz conversion changed audio duration or lost its tail");
    require(reference.size() == output.size(), "block boundaries changed resampling duration");
    for (std::size_t index = 0; index < output.size(); ++index) {
        require(std::abs(output[index] - reference[index]) < 0.00001F,
                "resampler did not preserve filter state across input blocks");
    }
    double squared = 0.0;
    for (std::size_t index = 100; index < output.size() - 100; ++index) {
        squared += output[index] * output[index];
    }
    const double rms = std::sqrt(squared / (output.size() - 200));
    require(std::abs(rms - 0.3 / std::sqrt(2.0)) < 0.003,
            "stereo downmix or resampling changed normalized amplitude");

    voice_engine::MonoFrameAssembler assembler;
    std::size_t complete_frames = 0;
    for (std::size_t offset = 0; offset < output.size();) {
        const auto count = std::min<std::size_t>(137, output.size() - offset);
        complete_frames += assembler.push(output.data() + offset, count).size();
        offset += count;
    }
    const auto tail = assembler.finish();
    require(complete_frames * 320 + (tail ? tail->valid_samples : 0) == output.size(),
            "capture framing lost resampler output");

    voice_engine::StreamResampler downmix_only(16000, 16000, 2, 1);
    const std::vector<float> opposing{0.75F, -0.75F, -0.25F, 0.25F};
    require(downmix_only.process(opposing.data(), 2) == std::vector<float>({0.0F, 0.0F}),
            "equal-rate downmix did not average its channels");
}

void check_playback_tail_and_reset() {
    std::vector<float> input(8000);
    for (std::size_t index = 0; index < input.size(); ++index) {
        input[index] = 0.25F * static_cast<float>(std::sin(2.0 * kPi * 1000.0 * index / 16000.0));
    }
    input.back() = 0.5F;
    voice_engine::StreamResampler playback(16000, 48000, 1, 2);
    std::vector<float> output;
    for (std::size_t offset = 0; offset < input.size(); offset += 320) {
        append(output, playback.process(input.data() + offset, std::min<std::size_t>(320, input.size() - offset)));
    }
    const auto tail = playback.finish();
    require(!tail.empty(), "normal playback finish discarded the delayed filter tail");
    append(output, tail);
    require(output.size() % 2 == 0 && output.size() / 2 >= 23999 && output.size() / 2 <= 24001,
            "playback conversion has the wrong output duration or channel format");
    for (std::size_t index = 0; index < output.size(); index += 2) {
        require(output[index] == output[index + 1], "mono playback was not duplicated into device channels");
        require(std::isfinite(output[index]) && std::abs(output[index]) <= 1.0F,
                "playback output is outside normalized float range");
    }
    require(*std::max_element(tail.begin(), tail.end()) > 0.1F,
            "the last input impulse was lost instead of appearing in the final tail");
    require(playback.finish().empty(), "playback tail was emitted twice");
    require_audio_error([&] { playback.process(input.data(), 1); }, "finished resampler accepted input");

    playback.reset();
    std::vector<float> old_audio(320, 0.8F);
    playback.process(old_audio.data(), old_audio.size());
    playback.reset();  // Cancel without draining; old filter history must disappear.
    const std::vector<float> silence(3200, 0.0F);
    const auto fresh = convert_blocks(playback, silence, 1, {17, 320, 7});
    require(!fresh.empty() && std::all_of(fresh.begin(), fresh.end(),
                                        [](float value) { return value == 0.0F; }),
            "reset leaked cancelled playback into the next generation");

    voice_engine::StreamResampler clipping(16000, 48000);
    const std::vector<float> steps(320, 1.0F);
    auto bounded = clipping.process(steps.data(), steps.size());
    append(bounded, clipping.finish());
    require(std::all_of(bounded.begin(), bounded.end(),
                        [](float value) { return std::isfinite(value) && value >= -1.0F && value <= 1.0F; }),
            "sinc overshoot escaped normalized output range");
}

void check_validation_and_size_bounds() {
    require_audio_error([] { voice_engine::StreamResampler invalid(0, 16000); }, "zero rate accepted");
    require_audio_error([] { voice_engine::StreamResampler invalid(16000, 192001); }, "excessive rate accepted");
    require_audio_error([] { voice_engine::StreamResampler invalid(16000, 16000, 0); }, "zero channels accepted");
    require_audio_error([] { voice_engine::StreamResampler invalid(16000, 16000, 1, 9); }, "excessive channels accepted");
    voice_engine::StreamResampler converter(16000, 16000);
    require(converter.process(nullptr, 0).empty(), "empty input generated audio");
    require_audio_error([&] { converter.process(nullptr, 1); }, "null input accepted");
    require_audio_error([&] { converter.process(nullptr, 16001); }, "oversize input accepted");
    voice_engine::MonoFrameAssembler assembler;
    require_audio_error([&] { assembler.push(nullptr, 32001); }, "unbounded assembler input accepted");
    for (float invalid : {std::numeric_limits<float>::quiet_NaN(),
                          std::numeric_limits<float>::infinity(), 1.01F, -1.01F}) {
        require_audio_error([&] { converter.process(&invalid, 1); }, "invalid resampler sample accepted");
        require_audio_error([&] { assembler.push(&invalid, 1); }, "invalid assembler sample accepted");
    }
    const std::vector<float> endpoints{-1.0F, 0.0F, 1.0F};
    require(converter.process(endpoints.data(), endpoints.size()) == endpoints,
            "identity conversion changed valid normalized endpoints");
}

}  // namespace

int main() {
    try {
        check_arbitrary_framing_and_cancel();
        check_capture_downmix_and_streaming();
        check_playback_tail_and_reset();
        check_validation_and_size_bounds();
        std::cout << "AUDIO_DSP_OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "audio DSP test failed: " << error.what() << '\n';
        return 1;
    }
}
