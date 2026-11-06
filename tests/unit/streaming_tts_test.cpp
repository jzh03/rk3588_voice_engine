#include "tts_websocket.hpp"
#include "voice_engine/tts_stream.hpp"
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace voice_engine;
using Clock = std::chrono::steady_clock;
namespace {
void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
}

int main(int argc, char** argv) {
    try {
        const bool live = argc == 3 && std::string(argv[1]) == "--live";
        require(live || argc == 4, "expected --live CONFIG or ENDPOINT CA CASE");
        CloudTtsConfig config;
        const std::string scenario = live ? "live" : argv[3];
        if (live) {
            config = load_config(argv[2]).cloud.tts;
            require(config.streaming(), "expected the streaming TTS profile");
        } else {
            config.endpoint = argv[1];
            config.api_key_env = "STREAM_ASR_TEST_KEY";
            config.voice = "fixture-voice";
            config.model = "qwen-audio-3.1-tts-flash";
            config.sample_rate = 16000;
            config.total_ms = 4000;
        }
        TtsConfig text;
        text.prebuffer_ms = 0;
        QueuesConfig queues;
        queues.playback_ms = 40;
        const auto start = Clock::now();
        TtsStreamWorker worker(
            [&](const std::string& part, const std::shared_ptr<TtsCancellation>& token,
                const TtsStreamWorker::PcmSink& sink) {
                cloud::stream_speech(config, part, queues.max_tts_audio_ms, *token, sink,
                                     live ? "" : argv[2]);
            }, text, queues, config.total_ms, config.sample_rate);
        const TtsStreamId id{1, 1};
        worker.begin(id);
        require(worker.submit(id, "你好，欢迎回来。") && worker.finish(id), "cannot feed test text");
        const auto first = worker.poll(id, config.total_ms + 500);
        if (!first) {
            const auto error = worker.stats().error;
            throw std::runtime_error(error.empty() ? "first PCM was not available" : error);
        }
        const auto first_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-start).count();
        std::cout << "FIRST_PCM " << first_ms << std::endl;
        if (scenario == "cancel") {
            worker.cancel(id);
            require(worker.wait_idle(1000) && !worker.poll(id, 0), "cancel did not release the PCM producer");
            return 0;
        }
        std::size_t samples = 0;
        const auto consume = [&](const TtsStreamFrame& frame) {
            if (!live) {
                for (std::size_t i = 0; i < frame.audio.valid_samples; ++i)
                    require(std::abs(frame.audio.samples[i] + 0.25f) < 0.001f, "PCM sample misaligned");
            }
            samples += frame.audio.valid_samples;
        };
        consume(*first);
        const auto deadline = start + std::chrono::milliseconds(config.total_ms + 1000);
        do {
            if (auto frame = worker.poll(id, 20)) consume(*frame);
            if (worker.wait_idle(0) && worker.stats().current_pcm_frames == 0) break;
        } while (Clock::now() < deadline);
        const auto stats = worker.stats();
        if (scenario == "invalid") {
            require(stats.status == "failed" && stats.error == "streaming_tts_invalid_pcm",
                    "truncated PCM did not fail");
        } else {
            if (stats.status != "completed") throw std::runtime_error(stats.error);
            require(live ? samples > 320 : samples == 1600, "unexpected PCM sample count");
            require(stats.peak_pcm_frames <= 2, "PCM queue exceeded its configured bound");
            std::cout << "COMPLETE " << samples << " "
                      << std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-start).count()
                      << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
