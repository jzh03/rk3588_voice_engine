#include "asr_stream.hpp"
#include <array>
#include <chrono>
#include <iostream>
#include <string_view>
#include <thread>

using namespace voice_engine;
using namespace voice_engine::cloud;
using namespace std::chrono_literals;

int main(int argc, char** argv) {
    if (argc != 4) return 2;
    const std::string_view scenario(argv[3]);
    CloudAsrConfig config;
    config.protocol = "dashscope_asr_ws";
    config.provider = "bailian";
    config.model = "qwen-audio-3.1-asr-flash-message";
    config.endpoint = argv[1];
    config.api_key_env = "STREAM_ASR_TEST_KEY";
    config.total_ms = 2000;
    if (scenario == "timeout") config.first_response_ms = 150;
    if (scenario == "overflow") config.stream_queue_ms = 332;
    try {
        Cancellation cancellation;
        StreamingAsr stream(config, 100, cancellation, argv[2]);
        std::array<float, 512> samples;
        samples.fill(-0.5f);
        for (int n = 0; n < 12; ++n) stream.append(samples.data(), samples.size(), false);
        samples.fill(0.25f);
        stream.append(samples.data(), samples.size(), false);
        if (scenario == "silence") {
            std::this_thread::sleep_for(250ms);
            return stream.finish().audio_bytes == 0 ? 0 : 1;
        }
        samples.fill(0.5f);
        stream.append(samples.data(), samples.size(), true);
        stream.append(samples.data(), samples.size(), true);
        if (scenario == "cancel") {
            std::this_thread::sleep_for(250ms);
            cancellation.cancel();
        }
        const auto result = stream.finish();
        const auto expected = std::size_t(300 * 32 + 2 * samples.size() * 2);
        if (scenario == "speech" && result.text == "first second" && result.audio_bytes == expected &&
            result.task_ready_ns <= result.first_audio_sent_ns &&
            result.first_audio_sent_ns < result.finish_sent_ns)
            return 0;
        std::cerr << "unexpected_stream_result\n";
    } catch (const Error& error) {
        const std::string_view reason(error.what());
        if ((scenario == "cancel" && reason == "cancelled") ||
            (scenario == "timeout" && reason == "streaming_asr_task_start_timeout") ||
            (scenario == "overflow" && reason == "streaming_asr_queue_full"))
            return 0;
        std::cerr << reason << '\n';
    }
    return 1;
}
