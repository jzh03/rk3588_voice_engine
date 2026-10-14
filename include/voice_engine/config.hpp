#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>

namespace voice_engine {

struct AudioConfig {
    std::string source;
    std::string sink;
    std::string profile;
    int sample_rate = 16000;  // Hz
    int channels = 1;
    int frame_ms = 20;
};

struct LoggingConfig {
    int max_file_bytes = 10 * 1024 * 1024;
    int max_files = 10;  // Includes the current file.
};

struct ModelRef {
    bool enabled = false;
    std::string id;
};

struct ModelsConfig {
    ModelRef vad;
    ModelRef asr;
    ModelRef llm;
    ModelRef tts;
    ModelRef kws;
};

struct AssetsConfig {
    // Relative paths are resolved from the configuration file's directory.
    std::string models_manifest = "../models/manifest.json";
    std::string deps_manifest = "../deps/manifest.json";
};

struct VadConfig {
    int pre_roll_ms = 500;
    int min_speech_ms = 200;
    int end_silence_ms = 600;
    int start_timeout_ms = 5000;
    int max_speech_ms = 15000;
};

struct ThreadsConfig {
    int vad = 1;
    int asr = 1;
    int llm = 1;
    int tts = 2;
    int kws = 1;
};

struct LlmConfig {
    int context_tokens = 2048;
    int max_output_tokens = 128;
    int margin_tokens = 64;
};

struct TtsConfig {
    int min_chars = 8;
    int max_chars = 40;
    int flush_ms = 300;
};

struct QueuesConfig {
    int capture_ms = 2000;
    int pending_turns = 1;
    int llm_text_bytes = 8192;
    int tts_segments = 4;
    int playback_ms = 2000;
    int max_tts_audio_ms = 20000;
};

struct TimeoutsConfig {
    int asr_final_ms = 3000;
    int llm_first_token_ms = 10000;
    int llm_total_ms = 30000;
    int tts_segment_ms = 10000;
    int cancel_ms = 2000;
};

struct DialogueConfig {
    bool wake_enabled = false;
    bool barge_in = false;
    std::string wake_word = "你好小派";
    int window_ms = 15000;
    int barge_in_min_ms = 250;
};

struct Config {
    AudioConfig audio;
    LoggingConfig logging;
    ModelsConfig models;
    AssetsConfig assets;
    VadConfig vad;
    ThreadsConfig threads;
    LlmConfig llm;
    TtsConfig tts;
    QueuesConfig queues;
    TimeoutsConfig timeouts;
    DialogueConfig dialogue;
    std::filesystem::path source_path;
};

class ConfigError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

Config load_config(const std::string& path);

}  // namespace voice_engine
