#include "voice_engine/config.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <iterator>
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

double bounded_number(const YAML::Node& node, const std::string& section,
                      const char* key, double fallback, double minimum, double maximum) {
    const auto value = node[key];
    if (!value) return fallback;
    const auto field = section + "." + key;
    double result = 0;
    try {
        if (!value.IsScalar() ||
            (value.Tag() != "?" && value.Tag() != "tag:yaml.org,2002:float" &&
             value.Tag() != "tag:yaml.org,2002:int")) {
            throw ConfigError(field + " must be a finite number");
        }
        result = value.as<double>();
    } catch (const YAML::Exception&) {
        throw ConfigError(field + " must be a finite number");
    }
    if (!std::isfinite(result) || result < minimum || result > maximum) {
        throw ConfigError(field + " must be a finite number between " +
                          std::to_string(minimum) + " and " + std::to_string(maximum));
    }
    return result;
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

bool identifier(const std::string& text, std::string_view punctuation, std::size_t max) {
    return !text.empty() && text.size() <= max && std::all_of(text.begin(), text.end(), [&](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
               punctuation.find(ch) != std::string_view::npos;
    });
}

void parse_cloud_service(const YAML::Node& node, const std::string& section,
                         const char* compatible_protocol, const char* bailian_protocol,
                         CloudService& service, const char* websocket_protocol = nullptr) {
    service.provider = string_value(node, section, "provider");
    service.protocol = string_value(node, section, "protocol");
    const bool bailian = service.provider == "bailian";
    const bool streaming = websocket_protocol && bailian && service.protocol == websocket_protocol;
    if (!streaming && ((service.provider != "openai_compatible" && !bailian) ||
        service.protocol != (bailian ? bailian_protocol : compatible_protocol)))
        throw ConfigError(section + " requires openai_compatible/" + compatible_protocol +
                          " or bailian/" + bailian_protocol + (websocket_protocol ? std::string(" or bailian/") + websocket_protocol : ""));
    if (streaming && service.protocol == "dashscope_asr_ws") service.total_ms = 5000;
    service.allow_http = boolean_or_default(node, section, "allow_http", false);
    service.endpoint = string_value(node, section, "endpoint");
    const std::string_view workspace_prefix = streaming ? "wss://{WorkspaceId}." : "https://{WorkspaceId}.";
    if (bailian && service.endpoint.compare(0, workspace_prefix.size(), workspace_prefix) == 0) {
        const char* value = std::getenv("DASHSCOPE_WORKSPACE_ID");
        const std::string workspace = value ? value : "";
        if (!identifier(workspace, "-", 63) || workspace.front() == '-' || workspace.back() == '-')
            throw ConfigError(section + " requires DASHSCOPE_WORKSPACE_ID for the workspace endpoint");
        service.endpoint.replace(workspace_prefix.find('{'), std::string_view("{WorkspaceId}").size(), workspace);
    }
    const auto& url = service.endpoint;
    const auto https = url.compare(0, 8, "https://") == 0;
    const auto http = url.compare(0, 7, "http://") == 0;
    const auto start = streaming ? 6 : https ? 8 : 7;
    const auto slash = url.find('/', start);
    const bool allowed = streaming ? !service.allow_http && url.compare(0, 6, "wss://") == 0 :
                                     https || (http && service.allow_http);
    if (!allowed || slash == std::string::npos ||
        slash == static_cast<std::size_t>(start) ||
        !identifier(url.substr(start, slash - start), ".-:[]", 260) ||
        !identifier(url.substr(slash), "/._-~", 1024))
        throw ConfigError(section + (streaming ? ".endpoint requires a complete WSS address with a path" :
            ".endpoint requires a complete HTTPS address with a path (HTTP requires allow_http)"));
    service.model = string_value(node, section, "model");
    if (!identifier(service.model, "_-.:/@", 256))
        throw ConfigError(section + ".model must be a model identifier");
    const auto auth = node["auth"] ? string_value(node, section, "auth") : "bearer";
    if (auth != "bearer" && auth != "none")
        throw ConfigError(section + ".auth must be bearer or none");
    service.bearer_auth = auth == "bearer";
    if (bailian && !service.bearer_auth)
        throw ConfigError(section + " requires bearer auth for Bailian");
    if (service.bearer_auth) {
        service.api_key_env = string_value(node, section, "api_key_env");
        if (!identifier(service.api_key_env, "_", 128) ||
            (service.api_key_env.front() >= '0' && service.api_key_env.front() <= '9'))
            throw ConfigError(section + ".api_key_env must be an environment variable name");
        try { cloud_api_key(service); }
        catch (const ConfigError& error) { throw ConfigError(section + ": " + error.what()); }
    } else if (node["api_key_env"]) {
        throw ConfigError(section + ".auth=none requires omitting api_key_env");
    }
    service.total_ms = bounded_integer(node, section, "total_ms", service.total_ms, 1, 120000);
    service.connect_ms = bounded_integer(node, section, "connect_ms", std::min(service.connect_ms, service.total_ms),
                                        1, service.total_ms);
    service.first_response_ms = bounded_integer(node, section, "first_response_ms",
        std::min(service.first_response_ms, service.total_ms), 1, service.total_ms);
    service.idle_ms = bounded_integer(node, section, "idle_ms", std::min(service.idle_ms, service.total_ms),
                                     1, service.total_ms);
    service.max_request_bytes = bounded_integer(node, section, "max_request_bytes", service.max_request_bytes,
                                               1024, 8 * 1024 * 1024);
    service.max_response_bytes = bounded_integer(node, section, "max_response_bytes", service.max_response_bytes,
                                                128, 16 * 1024 * 1024);
}

void parse_backends(const YAML::Node& root, Config& config) {
    const auto backends = root["backends"];
    if (backends) {
        check_fields(backends, "backends", {"asr", "llm", "tts"});
        auto backend = [&](const char* name) {
            if (!backends[name]) return Backend::local;
            const auto value = string_value(backends, "backends", name);
            if (value == "local") return Backend::local;
            if (value == "cloud") return Backend::cloud;
            throw ConfigError(std::string("backends.") + name + " must be local or cloud");
        };
        config.backends = {backend("asr"), backend("llm"), backend("tts")};
    }
    const auto& mode = config.backends;
    if (mode.asr != mode.llm || mode.asr != mode.tts)
        throw ConfigError("backends supports all-local or all-cloud only");
    if (!mode.cloud()) return;

    const auto cloud = root["cloud"];
    check_fields(cloud, "cloud", {"asr", "llm", "tts", "turn_timeout_ms"});
    auto& c = config.cloud;
    c.turn_timeout_ms = bounded_integer(cloud, "cloud", "turn_timeout_ms", c.turn_timeout_ms, 1000, 300000);

    const auto asr = cloud["asr"];
    check_fields(asr, "cloud.asr", {"provider", "protocol", "endpoint", "model", "auth", "api_key_env", "allow_http",
        "connect_ms", "first_response_ms", "idle_ms", "total_ms", "max_request_bytes", "max_response_bytes",
        "language", "wav_bytes", "stream_pre_roll_ms", "stream_queue_ms"});
    parse_cloud_service(asr, "cloud.asr", "audio_transcriptions", "dashscope_asr_json", c.asr, "dashscope_asr_ws");
    if (c.asr.streaming()) {
        if (asr["wav_bytes"]) throw ConfigError("cloud.asr.wav_bytes applies to batch ASR");
        if (c.asr.model != "qwen-audio-3.1-asr-flash-message")
            throw ConfigError("cloud.asr dashscope_asr_ws requires qwen-audio-3.1-asr-flash-message");
        c.asr.stream_pre_roll_ms = bounded_integer(asr, "cloud.asr", "stream_pre_roll_ms",
            c.asr.stream_pre_roll_ms, 0, 1000);
        c.asr.stream_queue_ms = bounded_integer(asr, "cloud.asr", "stream_queue_ms",
            c.asr.stream_queue_ms, c.asr.stream_pre_roll_ms + 32, 5000);
    } else if (asr["stream_pre_roll_ms"] || asr["stream_queue_ms"]) {
        throw ConfigError("cloud.asr stream settings require dashscope_asr_ws");
    }
    if (asr["language"]) c.asr.language = string_value(asr, "cloud.asr", "language", true);
    if (!c.asr.language.empty() && !identifier(c.asr.language, "-", 16))
        throw ConfigError("cloud.asr.language must be a language code");
    c.asr.wav_bytes = bounded_integer(asr, "cloud.asr", "wav_bytes", c.asr.wav_bytes, 4096, 4 * 1024 * 1024);

    const auto llm = cloud["llm"];
    check_fields(llm, "cloud.llm", {"provider", "protocol", "endpoint", "model", "auth", "api_key_env", "allow_http",
        "connect_ms", "first_response_ms", "idle_ms", "total_ms", "max_request_bytes", "max_response_bytes",
        "context_tokens", "max_output_tokens", "margin_tokens"});
    c.llm.total_ms = 30000;
    parse_cloud_service(llm, "cloud.llm", "chat_completions_sse", "chat_completions_sse", c.llm);
    c.llm.context_tokens = bounded_integer(llm, "cloud.llm", "context_tokens", c.llm.context_tokens, 256, 262144);
    c.llm.max_output_tokens = bounded_integer(llm, "cloud.llm", "max_output_tokens", c.llm.max_output_tokens, 1, 8192);
    c.llm.margin_tokens = bounded_integer(llm, "cloud.llm", "margin_tokens", c.llm.margin_tokens, 32, 16384);
    if (c.llm.max_output_tokens + c.llm.margin_tokens >= c.llm.context_tokens)
        throw ConfigError("cloud.llm output and margin must fit the configured model context");

    const auto tts = cloud["tts"];
    check_fields(tts, "cloud.tts", {"provider", "protocol", "endpoint", "model", "auth", "api_key_env", "allow_http",
        "connect_ms", "first_response_ms", "idle_ms", "total_ms", "max_request_bytes", "max_response_bytes",
        "voice", "speed", "sample_rate"});
    c.tts.total_ms = 10000;
    c.tts.max_request_bytes = 16 * 1024;
    c.tts.max_response_bytes = 2 * 1024 * 1024;
    parse_cloud_service(tts, "cloud.tts", "audio_speech_wav", "dashscope_tts_wav_url", c.tts, "dashscope_tts_ws");
    c.tts.voice = string_value(tts, "cloud.tts", "voice");
    if (!identifier(c.tts.voice, "_-.:/@", 256))
        throw ConfigError("cloud.tts.voice must be a voice identifier");
    c.tts.speed = bounded_number(tts, "cloud.tts", "speed", 1.0, 0.25, 4.0);
    c.tts.sample_rate = bounded_integer(tts, "cloud.tts", "sample_rate", c.tts.sample_rate, 8000, 48000);
    if (c.tts.provider == "bailian") {
        constexpr int rates[] = {8000, 12000, 16000, 22050, 24000, 44100, 48000};
        if (std::find(std::begin(rates), std::end(rates), c.tts.sample_rate) == std::end(rates) ||
            (c.tts.streaming() && c.tts.sample_rate == 12000))
            throw ConfigError("cloud.tts.sample_rate is unsupported by the selected Bailian protocol");
        if (c.tts.speed < 0.5 || c.tts.speed > 2.0)
            throw ConfigError("cloud.tts.speed must be within 0.5..2.0 for Bailian");
    }
}

Config parse_config(const std::string& path) {
    const auto documents = YAML::LoadAllFromFile(path);
    if (documents.size() != 1) {
        throw ConfigError("expected exactly one YAML document");
    }
    const auto& root = documents.front();
    check_fields(root, "root", {"audio", "logging", "models", "assets", "vad",
                                 "threads", "llm", "tts", "queues", "timeouts", "dialogue", "backends", "cloud"});
    const auto audio = root["audio"];
    check_fields(audio, "audio", {"source", "sink", "profile", "sample_rate",
                                  "channels", "frame_ms", "playback_lead_in_ms", "playback_keepalive"});

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
    config.audio.playback_keepalive = boolean_or_default(audio, "audio", "playback_keepalive", false);
    config.audio.playback_lead_in_ms = bounded_integer(
        audio, "audio", "playback_lead_in_ms", 0, 0, 2000);
    if (config.audio.playback_lead_in_ms % config.audio.frame_ms != 0)
        throw ConfigError("audio.playback_lead_in_ms must be a multiple of audio.frame_ms");

    const auto logging = root["logging"];
    if (logging) {
        check_fields(logging, "logging", {"max_file_bytes", "max_files"});
        config.logging.max_file_bytes = bounded_integer(
            logging, "logging", "max_file_bytes", config.logging.max_file_bytes,
            1024, 10 * 1024 * 1024);
        config.logging.max_files = bounded_integer(
            logging, "logging", "max_files", config.logging.max_files, 1, 10);
    }

    parse_backends(root, config);

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
    if (llm && !config.backends.cloud()) {
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
        check_fields(tts, "tts", {"min_chars", "max_chars", "flush_ms", "prebuffer_ms", "target_chars", "speed", "boundary_lookahead_chars", "sentence_mode", "join_short_sentences", "first_segment"});
        config.tts.min_chars = bounded_integer(tts, "tts", "min_chars", config.tts.min_chars, 1, 256);
        config.tts.max_chars = bounded_integer(tts, "tts", "max_chars", config.tts.max_chars, 1, 1024);
        config.tts.flush_ms = bounded_integer(tts, "tts", "flush_ms", config.tts.flush_ms, 1, 5000);
        config.tts.prebuffer_ms = bounded_integer(tts, "tts", "prebuffer_ms", 0, 0, 2000);
        config.tts.target_chars = bounded_integer(tts, "tts", "target_chars", 0, 0, 1024);
        config.tts.speed = bounded_number(tts, "tts", "speed", 1.0, 0.5, 2.0);
        config.tts.boundary_lookahead_chars = bounded_integer(
            tts, "tts", "boundary_lookahead_chars", 0, 0, 32);
        config.tts.sentence_mode = boolean_or_default(tts, "tts", "sentence_mode", false);
        config.tts.join_short_sentences = boolean_or_default(tts, "tts", "join_short_sentences", false);
        const auto first = tts["first_segment"];
        if (first && !first.IsNull()) {
            check_fields(first, "tts.first_segment", {"min_chars", "target_chars", "boundary_lookahead_chars"});
            for (const char* key : {"min_chars", "target_chars", "boundary_lookahead_chars"})
                if (!first[key]) throw ConfigError(std::string("tts.first_segment.") + key + " is required");
            TtsSegmentPolicy policy;
            policy.min_chars = bounded_integer(first, "tts.first_segment", "min_chars", 8, 1, 256);
            policy.target_chars = bounded_integer(first, "tts.first_segment", "target_chars", 15, 1, 1024);
            policy.boundary_lookahead_chars = bounded_integer(first, "tts.first_segment", "boundary_lookahead_chars", 2, 0, 32);
            config.tts.first_segment = policy;
        }
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
        if (!config.backends.cloud()) {
            config.timeouts.asr_final_ms = bounded_integer(
                timeouts, "timeouts", "asr_final_ms", config.timeouts.asr_final_ms, 1, 120000);
            config.timeouts.llm_first_token_ms = bounded_integer(
                timeouts, "timeouts", "llm_first_token_ms", config.timeouts.llm_first_token_ms, 1, 120000);
            config.timeouts.llm_total_ms = bounded_integer(
                timeouts, "timeouts", "llm_total_ms", config.timeouts.llm_total_ms, 1, 300000);
            config.timeouts.tts_segment_ms = bounded_integer(
                timeouts, "timeouts", "tts_segment_ms", config.timeouts.tts_segment_ms, 1, 120000);
        }
        config.timeouts.cancel_ms = bounded_integer(
            timeouts, "timeouts", "cancel_ms", config.timeouts.cancel_ms, 1, 10000);
    }

    const auto dialogue = root["dialogue"];
    if (dialogue) {
        check_fields(dialogue, "dialogue", {"wake_enabled", "barge_in", "wake_word",
                                            "window_ms", "barge_in_min_ms", "keywords_file"});
        config.dialogue.wake_enabled = boolean_or_default(
            dialogue, "dialogue", "wake_enabled", config.dialogue.wake_enabled);
        config.dialogue.barge_in = boolean_or_default(
            dialogue, "dialogue", "barge_in", config.dialogue.barge_in);
        if (dialogue["wake_word"]) {
            config.dialogue.wake_word = string_value(dialogue, "dialogue", "wake_word", true);
        }
        if (dialogue["keywords_file"]) {
            config.dialogue.keywords_file = string_value(dialogue, "dialogue", "keywords_file", true);
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
    if (config.tts.first_segment) {
        const auto& first = *config.tts.first_segment;
        if (config.tts.sentence_mode)
            throw ConfigError("tts.first_segment requires sentence_mode=false");
        if (first.min_chars > config.tts.max_chars || first.target_chars < first.min_chars ||
            first.target_chars > config.tts.max_chars ||
            first.boundary_lookahead_chars > config.tts.max_chars - first.target_chars)
            throw ConfigError("tts.first_segment requires min <= target and target + lookahead <= tts.max_chars");
    }
    if (config.tts.sentence_mode && (config.tts.target_chars || config.tts.boundary_lookahead_chars)) {
        throw ConfigError("tts.sentence_mode requires target_chars and boundary_lookahead_chars to be 0");
    }
    if (config.tts.target_chars && (config.tts.target_chars < config.tts.min_chars ||
                                    config.tts.target_chars > config.tts.max_chars)) {
        throw ConfigError("tts.target_chars must be 0 or within tts.min_chars..tts.max_chars");
    }
    if (config.tts.boundary_lookahead_chars && (!config.tts.target_chars ||
        config.tts.boundary_lookahead_chars > config.tts.max_chars - config.tts.target_chars)) {
        throw ConfigError("tts.boundary_lookahead_chars requires a positive target_chars and target + lookahead <= max_chars");
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
        (!config.models.vad.enabled || (!config.backends.cloud() && !config.models.asr.enabled))) {
        throw ConfigError(config.backends.cloud() ? "dialogue.barge_in requires models.vad.enabled" :
                          "dialogue.barge_in requires models.vad.enabled and models.asr.enabled");
    }
    if (config.backends.cloud()) {
        const auto& asr = config.cloud.asr;
        if (asr.streaming()) {
            const auto max_audio_bytes = 32 * (asr.stream_pre_roll_ms + config.vad.max_speech_ms +
                                               config.vad.end_silence_ms + 64);
            if (max_audio_bytes > asr.max_request_bytes)
                throw ConfigError("cloud.asr.max_request_bytes is too small for the streaming PCM budget");
        } else {
            // Include trailing silence for the VAD and two windows of look-ahead.
            const auto samples = 16 * (config.vad.pre_roll_ms + config.vad.max_speech_ms +
                                      config.vad.end_silence_ms) + 1024;
            const int payload = asr.provider == "bailian" ? 4 * ((asr.wav_bytes + 2) / 3) : asr.wav_bytes;
            if (44 + 2 * samples > asr.wav_bytes || payload + 2048 > asr.max_request_bytes)
                throw ConfigError("cloud.asr WAV/request budget is too small for the configured VAD");
        }
        config.timeouts.asr_final_ms = asr.total_ms;
        config.timeouts.llm_first_token_ms = config.cloud.llm.first_response_ms;
        config.timeouts.llm_total_ms = config.cloud.llm.total_ms;
        config.timeouts.tts_segment_ms = config.cloud.tts.total_ms;
        if (config.cloud.tts.streaming() && config.tts.prebuffer_ms != 0)
            throw ConfigError("dashscope_tts_ws requires tts.prebuffer_ms: 0");
    }
    return config;
}

}  // namespace

std::string cloud_api_key(const CloudService& service) {
    if (!service.bearer_auth) return {};
    const char* raw = std::getenv(service.api_key_env.c_str());
    const std::string key = raw ? raw : "";
    if (key.empty() || key.size() > 4096 || std::any_of(key.begin(), key.end(),
        [](unsigned char ch) { return ch <= 32 || ch >= 127; }))
        throw ConfigError("missing or invalid credential in the configured api_key_env");
    return key;
}

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
