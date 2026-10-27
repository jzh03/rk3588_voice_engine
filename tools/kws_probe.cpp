#include "voice_engine/kws.hpp"
#include "voice_engine/assets.hpp"
#include "voice_engine/probe_support.hpp"
#include <nlohmann/json.hpp>
#include <sndfile.h>
#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <sys/resource.h>
using namespace voice_engine;
int main(int argc, char** argv) {
    try {
        std::string config_path, output;
        std::vector<std::string> wavs;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--help") {
                std::cout << "kws_probe --config YAML --wav PCM16_WAV [--wav WAV] --output NEW_JSON\n";
                return 0;
            }
            if (i + 1 == argc) throw ConfigError("missing option value");
            if (arg == "--config") config_path = argv[++i];
            else if (arg == "--wav") wavs.emplace_back(argv[++i]);
            else if (arg == "--output") output = argv[++i];
            else throw ConfigError("unknown option");
        }
        if (config_path.empty() || output.empty() || wavs.empty() || std::filesystem::exists(output))
            throw ConfigError("config, WAV and a new output path are required");
        probe::require_model_validation_lock();
        auto c = load_config(config_path);
        c.models.vad.enabled = c.models.asr.enabled = c.models.llm.enabled = c.models.tts.enabled = false;
        validate_assets(c);
        KeywordSpotter kws(c);
        nlohmann::json report = {{"status", "completed"}, {"mode", "file_diagnostic_not_A11"},
            {"model_id", c.models.kws.id}, {"provider", "cpu"}, {"threads", c.threads.kws},
            {"keywords", kws.keyword().encoded}, {"tail_padding_ms", 1000},
            {"files", nlohmann::json::array()}};
        for (const auto& path : wavs) {
            SF_INFO info{};
            std::unique_ptr<SNDFILE, decltype(&sf_close)> file(sf_open(path.c_str(), SFM_READ, &info), sf_close);
            if (!file || info.channels != 1 || info.samplerate != 16000 || info.frames <= 0 ||
                (info.format & SF_FORMAT_SUBMASK) != SF_FORMAT_PCM_16 || info.frames > 16000LL * 1800)
                throw ConfigError("WAV requires mono 16 kHz PCM16, 0..1800 seconds");
            kws.reset();
            std::array<float,320> pcm{};
            std::uint64_t samples = 0;
            nlohmann::json hits = nlohmann::json::array();
            auto accept = [&](std::size_t n) {
                samples += n;
                if (const auto hit = kws.accept(pcm.data(), n)) {
                    hits.push_back({{"trigger_input_sample", samples},
                        {"last_token_seconds_since_reset", hit->token_end_seconds}});
                    kws.reset();
                }
            };
            const auto start = std::chrono::steady_clock::now();
            sf_count_t n;
            while ((n = sf_readf_float(file.get(), pcm.data(), pcm.size())) > 0) accept(n);
            pcm.fill(0);
            for (int i = 0; i < 50; ++i) accept(pcm.size());
            const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            report["files"].push_back({{"path", path}, {"sha256", file_sha256(path)},
                {"original_samples", info.frames}, {"hits", hits}, {"decode_seconds", seconds},
                {"rtf_including_tail", seconds / (samples / 16000.0)}});
        }
        struct rusage usage{}; getrusage(RUSAGE_SELF, &usage); report["max_rss_kib"] = usage.ru_maxrss;
        std::ofstream out(output); if (!out) throw std::runtime_error("cannot create report");
        out << report.dump(2) << '\n'; out.close();
        if (!out) throw std::runtime_error("report write failed");
        std::cout << "KWS_PROBE_OK files=" << wavs.size() << " output=" << output << '\n';
        return 0;
    } catch (const std::exception& error) { std::cerr << "KWS_ERROR: " << error.what() << '\n'; return 2; }
}
