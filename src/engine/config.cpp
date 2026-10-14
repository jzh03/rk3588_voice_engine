#include "voice_engine/config.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <initializer_list>
#include <set>
#include <string_view>

#include <yaml-cpp/yaml.h>

namespace voice_engine {
namespace {

void check_fields(const YAML::Node& node, const std::string& section,
                  std::initializer_list<std::string_view> allowed) {
    if (!node.IsMap()) {
        throw ConfigError(section + " must be a YAML mapping");
    }

    std::set<std::string> seen;
    for (const auto& entry : node) {
        if (!entry.first.IsScalar()) {
            throw ConfigError(section + " keys must be strings");
        }
        const auto key = entry.first.Scalar();
        const auto field = section == "root" ? key : section + "." + key;
        if (!seen.insert(key).second) {
            throw ConfigError(field + ": duplicate field");
        }
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
            throw ConfigError(field + ": unknown field");
        }
    }
}

bool is_numeric_scalar(const YAML::Node& value) {
    long long integer_value = 0;
    double numeric_value = 0;
    if (YAML::convert<long long>::decode(value, integer_value) ||
        YAML::convert<double>::decode(value, numeric_value)) {
        return true;
    }

    // Conversion failure can mean numeric overflow, rather than a string.
    const auto& text = value.Scalar();
    char* end = nullptr;
    const auto previous_errno = errno;
    errno = 0;
    std::strtod(text.c_str(), &end);
    const bool range_error = errno == ERANGE;
    errno = previous_errno;
    return range_error && end != text.c_str() &&
           end == text.c_str() + text.size();
}

bool non_blank(const std::string& text) {
    return std::any_of(text.begin(), text.end(), [](unsigned char character) {
        return !std::isspace(character);
    });
}

std::string string_value(const YAML::Node& node, const std::string& section,
                         const char* key, bool allow_empty = false) {
    const auto value = node[key];
    const auto field = section + "." + key;
    const auto requirement = allow_empty ? " must be a string" : " must be a non-empty string";
    if (!value || !value.IsScalar()) {
        throw ConfigError(field + requirement);
    }

    // yaml-cpp exposes all scalar values as text, including unquoted numbers.
    // Preserve quoted/explicit strings, and reject implicitly typed scalars.
    const auto tag = value.Tag();
    bool boolean_value = false;
    const bool implicit_non_string =
        tag == "?" &&
        (YAML::convert<bool>::decode(value, boolean_value) ||
         is_numeric_scalar(value));
    if ((tag != "?" && tag != "!" && tag != "tag:yaml.org,2002:str") ||
        implicit_non_string) {
        throw ConfigError(field + requirement);
    }

    const auto text = value.Scalar();
    if (text.find('\0') != std::string::npos) {
        throw ConfigError(field + " must not contain NUL");
    }
    if (!allow_empty && !non_blank(text)) {
        throw ConfigError(field + requirement);
    }
    return text;
}

bool boolean_or_default(const YAML::Node& node, const std::string& section,
                        const char* key, bool fallback) {
    const auto value = node[key];
    if (!value) {
        return fallback;
    }
    bool result = false;
    if (!value.IsScalar() ||
        (value.Tag() != "?" && value.Tag() != "tag:yaml.org,2002:bool") ||
        !YAML::convert<bool>::decode(value, result)) {
        throw ConfigError(section + "." + key + " must be a boolean");
    }
    return result;
}

int integer_or_default(const YAML::Node& node, const std::string& section,
                       const char* key, int fallback) {
    const auto value = node[key];
    if (!value) {
        return fallback;
    }

    const auto field = section + "." + key;
    try {
        if (!value.IsScalar() ||
            (value.Tag() != "?" && value.Tag() != "tag:yaml.org,2002:int")) {
            throw ConfigError(field + " must be an integer");
        }
        return value.as<int>();
    } catch (const YAML::Exception&) {
        throw ConfigError(field + " must be an integer");
    }
}

int bounded_integer(const YAML::Node& node, const std::string& section,
                    const char* key, int fallback, int minimum, int maximum) {
    const auto value = integer_or_default(node, section, key, fallback);
    if (value < minimum || value > maximum) {
        throw ConfigError(section + "." + key + " must be between " +
                          std::to_string(minimum) + " and " + std::to_string(maximum));
    }
    return value;
}

int fixed_integer(const YAML::Node& audio, const char* key, int expected) {
    const auto actual = integer_or_default(audio, "audio", key, expected);
    if (actual != expected) {
        const auto field = std::string("audio.") + key;
        throw ConfigError(field + " must be " + std::to_string(expected) +
                          " in the current audio pipeline");
    }
    return actual;
}

Config parse_config(const std::string& path) {
    const auto documents = YAML::LoadAllFromFile(path);
    if (documents.size() != 1) {
        throw ConfigError("expected exactly one YAML document");
    }
    const auto& root = documents.front();
    check_fields(root, "root", {"audio", "logging", "models", "assets", "vad",
                                 "threads", "llm", "tts", "queues", "timeouts", "dialogue"});
    const auto audio = root["audio"];
    check_fields(audio, "audio", {"source", "sink", "profile", "sample_rate",
                                  "channels", "frame_ms"});

    Config config;
    config.source_path = std::filesystem::absolute(path).lexically_normal();
    config.audio.source = string_value(audio, "audio", "source");
    config.audio.sink = string_value(audio, "audio", "sink");
    config.audio.profile = string_value(audio, "audio", "profile");

    constexpr std::string_view monitor_suffix = ".monitor";
    const auto& source = config.audio.source;
    if (source.size() >= monitor_suffix.size() &&
        source.compare(source.size() - monitor_suffix.size(),
                       monitor_suffix.size(), monitor_suffix) == 0) {
        throw ConfigError("audio.source must be a microphone, not a .monitor source");
    }

    config.audio.sample_rate = fixed_integer(audio, "sample_rate", 16000);
    config.audio.channels = fixed_integer(audio, "channels", 1);
    config.audio.frame_ms = fixed_integer(audio, "frame_ms", 20);

    const auto logging = root["logging"];
    if (logging) {
        check_fields(logging, "logging", {"max_file_bytes", "max_files"});
        config.logging.max_file_bytes = bounded_integer(
            logging, "logging", "max_file_bytes", config.logging.max_file_bytes,
            1024, 10 * 1024 * 1024);
        config.logging.max_files = bounded_integer(
            logging, "logging", "max_files", config.logging.max_files, 1, 10);
    }

    const auto models = root["models"];
    if (models) {
        check_fields(models, "models", {"vad", "asr", "llm", "tts", "kws"});
        auto parse_model = [&](const char* key, ModelRef& ref) {
            const auto model = models[key];
            if (!model) {
                return;
            }
            const auto section = std::string("models.") + key;
            check_fields(model, section, {"enabled", "id"});
            ref.enabled = boolean_or_default(model, section, "enabled", ref.enabled);
            if (model["id"]) {
                ref.id = string_value(model, section, "id", true);
            }
            if (ref.enabled && !non_blank(ref.id)) {
                throw ConfigError(section + ".id must be non-empty when enabled");
            }
        };
        parse_model("vad", config.models.vad);
        parse_model("asr", config.models.asr);
        parse_model("llm", config.models.llm);
        parse_model("tts", config.models.tts);
        parse_model("kws", config.models.kws);
    }

    const auto assets = root["assets"];
    if (assets) {
        check_fields(assets, "assets", {"models_manifest", "deps_manifest"});
        if (assets["models_manifest"]) {
            config.assets.models_manifest = string_value(assets, "assets", "models_manifest");
        }
        if (assets["deps_manifest"]) {
            config.assets.deps_manifest = string_value(assets, "assets", "deps_manifest");
        }
    }

    const auto vad = root["vad"];
    if (vad) {
        check_fields(vad, "vad", {"pre_roll_ms", "min_speech_ms", "end_silence_ms",
                                  "start_timeout_ms", "max_speech_ms"});
        config.vad.pre_roll_ms = bounded_integer(
            vad, "vad", "pre_roll_ms", config.vad.pre_roll_ms, 0, 10000);
        config.vad.min_speech_ms = bounded_integer(
            vad, "vad", "min_speech_ms", config.vad.min_speech_ms, 20, 30000);
        config.vad.end_silence_ms = bounded_integer(
            vad, "vad", "end_silence_ms", config.vad.end_silence_ms, 20, 10000);
        config.vad.start_timeout_ms = bounded_integer(
            vad, "vad", "start_timeout_ms", config.vad.start_timeout_ms, 20, 60000);
        config.vad.max_speech_ms = bounded_integer(
            vad, "vad", "max_speech_ms", config.vad.max_speech_ms, 20, 60000);
    }

    const auto threads = root["threads"];
    if (threads) {
        check_fields(threads, "threads", {"vad", "asr", "llm", "tts", "kws"});
        config.threads.vad = bounded_integer(threads, "threads", "vad", config.threads.vad, 1, 16);
        config.threads.asr = bounded_integer(threads, "threads", "asr", config.threads.asr, 1, 16);
        config.threads.llm = bounded_integer(threads, "threads", "llm", config.threads.llm, 1, 16);
        config.threads.tts = bounded_integer(threads, "threads", "tts", config.threads.tts, 1, 16);
        config.threads.kws = bounded_integer(threads, "threads", "kws", config.threads.kws, 1, 16);
    }

    const auto llm = root["llm"];
    if (llm) {
        check_fields(llm, "llm", {"context_tokens", "max_output_tokens", "margin_tokens"});
        config.llm.context_tokens = bounded_integer(
            llm, "llm", "context_tokens", config.llm.context_tokens, 128, 2048);
        config.llm.max_output_tokens = bounded_integer(
            llm, "llm", "max_output_tokens", config.llm.max_output_tokens, 1, 128);
        config.llm.margin_tokens = bounded_integer(
            llm, "llm", "margin_tokens", config.llm.margin_tokens, 0, 1024);
    }

    const auto tts = root["tts"];
    if (tts) {
        check_fields(tts, "tts", {"min_chars", "max_chars", "flush_ms"});
        config.tts.min_chars = bounded_integer(tts, "tts", "min_chars", config.tts.min_chars, 1, 256);
        config.tts.max_chars = bounded_integer(tts, "tts", "max_chars", config.tts.max_chars, 1, 1024);
        config.tts.flush_ms = bounded_integer(tts, "tts", "flush_ms", config.tts.flush_ms, 1, 5000);
    }

    const auto queues = root["queues"];
    if (queues) {
        check_fields(queues, "queues", {"capture_ms", "pending_turns", "llm_text_bytes",
                                        "tts_segments", "playback_ms", "max_tts_audio_ms"});
        config.queues.capture_ms = bounded_integer(
            queues, "queues", "capture_ms", config.queues.capture_ms, 20, 10000);
        config.queues.pending_turns = bounded_integer(
            queues, "queues", "pending_turns", config.queues.pending_turns, 1, 1);
        config.queues.llm_text_bytes = bounded_integer(
            queues, "queues", "llm_text_bytes", config.queues.llm_text_bytes, 1, 1024 * 1024);
        config.queues.tts_segments = bounded_integer(
            queues, "queues", "tts_segments", config.queues.tts_segments, 1, 64);
        config.queues.playback_ms = bounded_integer(
            queues, "queues", "playback_ms", config.queues.playback_ms, 20, 10000);
        config.queues.max_tts_audio_ms = bounded_integer(
            queues, "queues", "max_tts_audio_ms", config.queues.max_tts_audio_ms, 20, 60000);
    }

    const auto timeouts = root["timeouts"];
    if (timeouts) {
        check_fields(timeouts, "timeouts", {"asr_final_ms", "llm_first_token_ms",
                                            "llm_total_ms", "tts_segment_ms", "cancel_ms"});
        config.timeouts.asr_final_ms = bounded_integer(
            timeouts, "timeouts", "asr_final_ms", config.timeouts.asr_final_ms, 1, 120000);
        config.timeouts.llm_first_token_ms = bounded_integer(
            timeouts, "timeouts", "llm_first_token_ms", config.timeouts.llm_first_token_ms, 1, 120000);
        config.timeouts.llm_total_ms = bounded_integer(
            timeouts, "timeouts", "llm_total_ms", config.timeouts.llm_total_ms, 1, 300000);
        config.timeouts.tts_segment_ms = bounded_integer(
            timeouts, "timeouts", "tts_segment_ms", config.timeouts.tts_segment_ms, 1, 120000);
        config.timeouts.cancel_ms = bounded_integer(
            timeouts, "timeouts", "cancel_ms", config.timeouts.cancel_ms, 1, 10000);
    }

    const auto dialogue = root["dialogue"];
    if (dialogue) {
        check_fields(dialogue, "dialogue", {"wake_enabled", "barge_in", "wake_word",
                                            "window_ms", "barge_in_min_ms"});
        config.dialogue.wake_enabled = boolean_or_default(
            dialogue, "dialogue", "wake_enabled", config.dialogue.wake_enabled);
        config.dialogue.barge_in = boolean_or_default(
            dialogue, "dialogue", "barge_in", config.dialogue.barge_in);
        if (dialogue["wake_word"]) {
            config.dialogue.wake_word = string_value(dialogue, "dialogue", "wake_word", true);
        }
        config.dialogue.window_ms = bounded_integer(
            dialogue, "dialogue", "window_ms", config.dialogue.window_ms, 20, 60000);
        config.dialogue.barge_in_min_ms = bounded_integer(
            dialogue, "dialogue", "barge_in_min_ms", config.dialogue.barge_in_min_ms, 20, 5000);
    }

    if (config.queues.capture_ms % config.audio.frame_ms != 0) {
        throw ConfigError("queues.capture_ms must be a multiple of audio.frame_ms");
    }
    if (config.queues.playback_ms % config.audio.frame_ms != 0) {
        throw ConfigError("queues.playback_ms must be a multiple of audio.frame_ms");
    }
    if (config.vad.pre_roll_ms > config.queues.capture_ms) {
        throw ConfigError("vad.pre_roll_ms must not exceed queues.capture_ms");
    }
    if (config.vad.min_speech_ms > config.vad.max_speech_ms) {
        throw ConfigError("vad.min_speech_ms must not exceed vad.max_speech_ms");
    }
    if (config.tts.min_chars > config.tts.max_chars) {
        throw ConfigError("tts.min_chars must not exceed tts.max_chars");
    }
    if (config.llm.max_output_tokens + config.llm.margin_tokens >= config.llm.context_tokens) {
        throw ConfigError("llm.max_output_tokens + llm.margin_tokens must be less than llm.context_tokens");
    }
    if (config.timeouts.llm_first_token_ms > config.timeouts.llm_total_ms) {
        throw ConfigError("timeouts.llm_first_token_ms must not exceed timeouts.llm_total_ms");
    }
    if (config.dialogue.wake_enabled && !config.models.kws.enabled) {
        throw ConfigError("dialogue.wake_enabled requires models.kws.enabled");
    }
    if (config.dialogue.wake_enabled && !non_blank(config.dialogue.wake_word)) {
        throw ConfigError("dialogue.wake_word must be non-empty when dialogue.wake_enabled");
    }
    if (config.dialogue.barge_in &&
        (!config.models.vad.enabled || !config.models.asr.enabled)) {
        throw ConfigError("dialogue.barge_in requires models.vad.enabled and models.asr.enabled");
    }
    return config;
}

}  // namespace

Config load_config(const std::string& path) {
    try {
        return parse_config(path);
    } catch (const YAML::Exception& error) {
        throw ConfigError(path + ": " + error.what());
    } catch (const ConfigError& error) {
        throw ConfigError(path + ": " + error.what());
    } catch (const std::filesystem::filesystem_error& error) {
        throw ConfigError(path + ": " + error.what());
    }
}

}  // namespace voice_engine
