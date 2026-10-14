#include "voice_engine/config.hpp"
#include "voice_engine/assets.hpp"
#include "voice_engine/audio.hpp"
#include "voice_engine/control.hpp"
#include "voice_engine/logger.hpp"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>

#include <unistd.h>

namespace {

void print_usage(std::ostream& output) {
    output << "Usage: voice_engine --config <path.yaml> --check-config\n"
              "       voice_engine --config <path.yaml> --mock --control stdin\n"
              "                    [--run-id <id>] [--output-dir <root>]\n"
              "       voice_engine --help\n\n"
              "Checks configuration and enabled model/dependency assets, or runs mock controls.\n"
              "Reads one complete file; default/local files are not merged.\n"
              "Relative manifest paths are based on the configuration file directory.\n"
              "Commands: start stop cancel new-session status help quit.\n"
              "Mock mode skips model assets and does not open devices or run models.\n"
              "Logs: <root>/<run-id>/events.jsonl; root defaults to output/runs.\n"
              "The run directory must be new; an ID is generated if omitted.\n"
              "Run IDs: 1-64 ASCII letters/digits/_.-, starting with letter/digit.\n"
              "Exit codes: 0 = normal/help, 1 = runtime I/O error, 2 = config/CLI error.\n"
              "Signal exits: 130 = SIGINT, 143 = SIGTERM, after flushing shutdown.\n";
}

int usage_error(const std::string& message) {
    std::cerr << "ARGUMENT_ERROR: " << message << '\n';
    print_usage(std::cerr);
    return 2;
}

bool valid_run_id(std::string_view value) {
    auto alphanumeric = [](char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9');
    };
    if (value.empty() || value.size() > 64 || !alphanumeric(value.front())) {
        return false;
    }
    for (const auto character : value) {
        if (!alphanumeric(character) && character != '_' && character != '.' &&
            character != '-') {
            return false;
        }
    }
    return true;
}

std::string generate_run_id() {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc {};
    if (gmtime_r(&now, &utc) == nullptr) {
        throw voice_engine::ControlError("cannot generate run ID from system time");
    }
    std::ostringstream id;
    id << "run-" << std::put_time(&utc, "%Y%m%dT%H%M%SZ") << '-' << getpid() << '-'
       << std::hex << std::chrono::steady_clock::now().time_since_epoch().count();
    return id.str();
}

}  // namespace

int main(int argc, char* argv[]) {
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) == "--help") {
            print_usage(std::cout);
            return 0;
        }
    }

    std::string config_path;
    std::string control;
    std::string run_id;
    std::filesystem::path output_root = "output/runs";
    bool check_config = false;
    bool mock = false;
    std::set<std::string_view> options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (!options.insert(argument).second) {
            return usage_error(std::string(argument) + " can only be specified once");
        }
        if (argument == "--check-config") {
            check_config = true;
        } else if (argument == "--mock") {
            mock = true;
        } else if (argument == "--config" || argument == "--control" ||
                   argument == "--run-id" || argument == "--output-dir") {
            if (index + 1 >= argc || std::string_view(argv[index + 1]).empty() ||
                std::string_view(argv[index + 1]).front() == '-') {
                return usage_error(std::string(argument) + " requires a value");
            }
            const std::string value(argv[++index]);
            if (argument == "--config") {
                config_path = value;
            } else if (argument == "--control") {
                control = value;
            } else if (argument == "--run-id") {
                run_id = value;
            } else {
                output_root = value;
            }
        } else {
            return usage_error("unknown argument: " + std::string(argument));
        }
    }
    if (config_path.empty()) {
        return usage_error("--config <path.yaml> is required");
    }
    if (check_config) {
        if (mock || !control.empty() || options.count("--run-id") != 0 ||
            options.count("--output-dir") != 0) {
            return usage_error("--check-config cannot be combined with control/log output options");
        }
    } else if (!mock || control != "stdin") {
        return usage_error("terminal control requires --mock --control stdin; real engine is pending");
    }
    if (!run_id.empty() && !valid_run_id(run_id)) {
        return usage_error("invalid --run-id; use 1-64 ASCII letters/digits/_.- starting with letter/digit");
    }

    try {
        const auto config = voice_engine::load_config(config_path);
        if (!check_config) {
#if VOICE_ENGINE_HAS_AUDIO
            auto audio = voice_engine::make_mock_audio(
                static_cast<std::size_t>(config.queues.playback_ms / config.audio.frame_ms));
            return voice_engine::run_stdin_control(
                config, run_id.empty() ? generate_run_id() : run_id, output_root, audio.get());
#else
            return voice_engine::run_stdin_control(
                config, run_id.empty() ? generate_run_id() : run_id, output_root);
#endif
        }
        const auto assets = voice_engine::validate_assets(config);
        std::cout << "CONFIG_OK\n"
                  << "ASSETS_OK models=" << assets.models
                  << " dependencies=" << assets.dependencies
                  << " files=" << assets.files << '\n'
                  << "audio.source: " << config.audio.source << '\n'
                  << "audio.sink: " << config.audio.sink << '\n'
                  << "audio.profile: " << config.audio.profile << '\n'
                  << "audio.sample_rate: " << config.audio.sample_rate << " Hz\n"
                  << "audio.channels: " << config.audio.channels << '\n'
                  << "audio.frame_ms: " << config.audio.frame_ms << " ms\n"
                  << "logging.max_file_bytes: " << config.logging.max_file_bytes << '\n'
                  << "logging.max_files: " << config.logging.max_files << '\n';
        return 0;
    } catch (const voice_engine::ConfigError& error) {
        std::cerr << "CONFIG_ERROR: " << error.what() << '\n';
        return 2;
    } catch (const voice_engine::LogError& error) {
        std::cerr << "LOG_ERROR: " << error.what() << '\n';
        return 1;
    } catch (const voice_engine::ControlError& error) {
        std::cerr << "CONTROL_ERROR: " << error.what() << '\n';
        return 1;
    } catch (const voice_engine::AudioError& error) {
        std::cerr << "AUDIO_ERROR: " << error.what() << '\n';
        return 1;
    }
}
