#include "voice_engine/audio.hpp"

#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

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

void check_capture_and_context() {
    auto audio = voice_engine::make_mock_audio();
    require(!audio->capturing(), "new backend must be idle");
    require_audio_error([&] { audio->capture_frame(); }, "idle capture was accepted");
    voice_engine::AudioFrame unstarted;
    unstarted.samples.assign(320, 0.0F);
    require_audio_error([&] { audio->submit_playback(unstarted); },
                        "playback before start was accepted");

    audio->start_capture({42, 7});
    const auto first = audio->capture_frame();
    const auto second = audio->capture_frame();
    require(audio->capturing(), "capture did not start");
    require(first.context.session_id == 42 && first.context.generation_id == 7,
            "frame lost its session/generation context");
    require(first.seq == 1 && second.seq == 2, "frame sequence did not advance");
    require(first.monotonic_ns > 0 && second.monotonic_ns >= first.monotonic_ns,
            "frame timestamp is not monotonic");
    require(first.sample_rate == 16000 && first.channels == 1 && first.samples.size() == 320 &&
            first.valid_samples == 320,
            "capture frame has the wrong format or duration");
    require(std::all_of(first.samples.begin(), first.samples.end(),
                        [](float sample) { return sample == 0.0F; }),
            "mock capture must generate silence");

    audio->stop_capture();
    require(!audio->capturing(), "capture did not stop");
    require_audio_error([&] { audio->capture_frame(); }, "stopped capture was accepted");
    audio->submit_playback(first);  // Playback remains valid after input ends.
    audio->cancel();
    require_audio_error([&] { audio->submit_playback(first); },
                        "cancelled generation was still playable");
    audio->cancel();  // Repeated stop/cancel calls must remain harmless.
    audio->stop_capture();
    audio->start_capture({42, 8});
    const auto next = audio->capture_frame();
    require(next.seq == 1 && next.context.generation_id == 8,
            "new generation did not reset capture state");
    require_audio_error([&] { audio->submit_playback(first); },
                        "old generation reached new playback");
    audio->start_capture({43, 8});
    require_audio_error([&] { audio->submit_playback(next); },
                        "old session reached new playback");
}

void check_playback_validation_and_bounds() {
    auto audio = voice_engine::make_mock_audio();
    audio->start_capture({1, 1});
    const auto valid = audio->capture_frame();

    auto invalid = valid;
    invalid.sample_rate = 48000;
    require_audio_error([&] { audio->submit_playback(invalid); }, "wrong rate accepted");
    invalid = valid;
    invalid.channels = 2;
    require_audio_error([&] { audio->submit_playback(invalid); }, "stereo accepted");
    invalid = valid;
    invalid.samples.pop_back();
    require_audio_error([&] { audio->submit_playback(invalid); }, "short frame accepted");
    invalid = valid;
    invalid.samples.push_back(0.0F);
    require_audio_error([&] { audio->submit_playback(invalid); }, "long frame accepted");
    invalid = valid;
    invalid.samples[10] = std::numeric_limits<float>::quiet_NaN();
    require_audio_error([&] { audio->submit_playback(invalid); }, "NaN sample accepted");
    invalid.samples[10] = std::numeric_limits<float>::infinity();
    require_audio_error([&] { audio->submit_playback(invalid); }, "infinite sample accepted");
    invalid.samples[10] = 1.01F;
    require_audio_error([&] { audio->submit_playback(invalid); }, "sample above normalized range accepted");
    invalid.samples[10] = -1.01F;
    require_audio_error([&] { audio->submit_playback(invalid); }, "sample below normalized range accepted");
    invalid = valid;
    invalid.valid_samples = 0;
    require_audio_error([&] { audio->submit_playback(invalid); }, "empty valid region accepted");
    invalid.valid_samples = 321;
    require_audio_error([&] { audio->submit_playback(invalid); }, "oversized valid region accepted");
    invalid = valid;
    invalid.valid_samples = 17;
    invalid.samples[20] = 0.5F;
    require_audio_error([&] { audio->submit_playback(invalid); }, "nonzero padding accepted");
    invalid.samples[20] = 0.0F;
    audio->submit_playback(invalid);
    audio->flush_playback();

    for (int index = 0; index < 100; ++index) {
        audio->submit_playback(valid);
    }
    require_audio_error([&] { audio->submit_playback(valid); }, "2-second bound not enforced");
    audio->flush_playback();
    for (int index = 0; index < 100; ++index) {
        audio->submit_playback(valid);
    }
    audio->start_capture({1, 2});
    audio->submit_playback(audio->capture_frame());  // Start removed old pending frames.
    audio->cancel();
    audio->start_capture({1, 3});
    for (int index = 0; index < 100; ++index) {
        audio->submit_playback(audio->capture_frame());
    }

    require_audio_error([] { voice_engine::make_mock_audio(0); }, "zero capacity accepted");
    require_audio_error([] { voice_engine::make_mock_audio(501); }, "unbounded capacity accepted");
    auto small = voice_engine::make_mock_audio(1);
    small->start_capture({1, 1});
    const auto small_frame = small->capture_frame();
    small->submit_playback(small_frame);
    require_audio_error([&] { small->submit_playback(small_frame); }, "custom bound not enforced");
    small->flush_playback();
    small->submit_playback(small_frame);
    auto largest = voice_engine::make_mock_audio(500);
    largest->start_capture({1, 1});
    for (int index = 0; index < 500; ++index) {
        largest->submit_playback(largest->capture_frame());
    }
    require_audio_error([&] { largest->submit_playback(largest->capture_frame()); },
                        "maximum capacity not enforced");
}

}  // namespace

int main() {
    try {
        check_capture_and_context();
        check_playback_validation_and_bounds();
        std::cout << "MOCK_AUDIO_OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "mock audio test failed: " << error.what() << '\n';
        return 1;
    }
}
