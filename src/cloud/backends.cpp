#include "backends.hpp"
#if VOICE_ENGINE_HAS_STREAMING_TTS
#include "websocket.hpp"
#endif
#if VOICE_ENGINE_HAS_STREAMING_ASR
#include "asr_stream.hpp"
#endif

#include <nlohmann/json.hpp>
#include <algorithm>
#include <mutex>
#include <chrono>
#include <cmath>
#include <cstring>

namespace voice_engine::cloud {
namespace {
using Json = nlohmann::json;
constexpr std::size_t wav_header_bytes = 44;
constexpr int asr_rate = 16000;
constexpr std::size_t wav_metadata_budget = 64 * 1024;

Json parse_json(std::string_view bytes) {
    try {
        return Json::parse(bytes.begin(), bytes.end(), [](int depth, auto, auto&) {
            if (depth > 32) throw Error("invalid_json_depth");
            return true;
        });
    } catch (const Json::exception&) {
        throw Error("invalid_json");
    }
}

std::string wav_data_uri(std::string_view wav) {
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string uri = "data:audio/wav;base64,";
    uri.reserve(uri.size() + 4 * ((wav.size() + 2) / 3));
    for (std::size_t n = 0; n < wav.size(); n += 3) {
        const auto count = wav.size() - n;
        const auto a = static_cast<unsigned char>(wav[n]);
        const auto b = count > 1 ? static_cast<unsigned char>(wav[n + 1]) : 0;
        const auto c = count > 2 ? static_cast<unsigned char>(wav[n + 2]) : 0;
        uri += alphabet[a >> 2];
        uri += alphabet[((a & 3) << 4) | (b >> 4)];
        uri += count > 1 ? alphabet[((b & 15) << 2) | (c >> 6)] : '=';
        uri += count > 2 ? alphabet[c & 63] : '=';
    }
    return uri;
}

const Json& required_field(const Json& value, const char* name, const char* error) {
    const auto field = value.find(name);
    if (field == value.end()) throw Error(error);
    return *field;
}

const Json& bailian_output(const Json& response, const char* error) {
    if (!response.is_object() || response.contains("error")) throw Error(error);
    const auto code = response.find("code");
    if (code != response.end() && !code->is_null() && *code != "")
        throw Error("bailian_service_error");
    return required_field(response, "output", error);
}

void put_le(std::string& data, std::size_t offset, std::uint32_t value, unsigned width) {
    for (unsigned n = 0; n < width; ++n)
        data[offset + n] = static_cast<char>((value >> (8 * n)) & 0xff);
}

std::uint32_t get_le(const std::string& data, std::size_t offset, unsigned width) {
    std::uint32_t result = 0;
    for (unsigned n = 0; n < width; ++n)
        result |= std::uint32_t(static_cast<unsigned char>(data[offset + n])) << (8 * n);
    return result;
}

class ChatStream {
    const LlmBackend::Callback& emit_;
    std::string line_, data_;
    bool previous_cr_ = false, finished_choice_ = false, done_ = false;
    static constexpr std::size_t event_limit = 64 * 1024;

    void dispatch() {
        if (data_.empty()) return;
        if (done_) throw Error("unexpected_sse_event_after_done");
        data_.pop_back(); // SSE joins multiple data fields with LF.
        if (data_ == "[DONE]") {
            if (!finished_choice_) throw Error("missing_finish_reason");
            done_ = true;
        } else {
            const auto message = parse_json(data_);
            if (!message.is_object()) throw Error("invalid_chat_event");
            if (message.contains("error")) throw Error("service_stream_error");
            const auto choices = message.find("choices");
            if (choices == message.end() || !choices->is_array() || choices->size() > 1)
                throw Error("invalid_chat_event");
            if (!choices->empty()) {
                if (finished_choice_) throw Error("unexpected_chat_choice_after_finish");
                const auto& choice = choices->front();
                if (!choice.is_object() || choice.find("index") == choice.end() || choice["index"] != 0)
                    throw Error("invalid_chat_choice");
                const auto delta = choice.find("delta");
                if (delta == choice.end() || !delta->is_object()) throw Error("invalid_chat_delta");
                for (const auto* name : {"tool_calls", "function_call"}) {
                    const auto tool = delta->find(name);
                    if (tool != delta->end() && !tool->is_null())
                        throw Error("unsupported_chat_tool_call");
                }
                const auto content = delta->find("content");
                if (content != delta->end() && !content->is_null()) {
                    if (!content->is_string()) throw Error("invalid_chat_content");
                    const auto& text = content->get_ref<const std::string&>();
                    if (!text.empty()) {
                        LlmBackendEvent event;
                        event.token_id = 0; // The API supplies deltas, without native token IDs.
                        event.text = text;
                        emit_(event);
                    }
                }
                const auto finish = choice.find("finish_reason");
                if (finish != choice.end() && !finish->is_null()) {
                    if (*finish != "stop" && *finish != "length") throw Error("unsupported_chat_finish_reason");
                    finished_choice_ = true;
                }
            }
        }
        data_.clear();
    }

    void line() {
        if (line_.empty()) {
            dispatch();
        } else {
            const auto colon = line_.find(':');
            if (line_.compare(0, colon, "data") == 0) {
                std::string_view value(line_);
                value = colon == std::string::npos ? std::string_view() : value.substr(colon + 1);
                if (!value.empty() && value.front() == ' ') value.remove_prefix(1);
                if (value.size() + 1 > event_limit - data_.size()) throw Error("sse_event_too_large");
                data_.append(value);
                data_ += '\n';
            }
        }
        line_.clear();
    }
public:
    explicit ChatStream(const LlmBackend::Callback& emit) : emit_(emit) {}
    void feed(std::string_view bytes) {
        for (const char ch : bytes) {
            if (ch == '\r' || (ch == '\n' && !previous_cr_)) line();
            else if (ch != '\n') {
                if (line_.size() == event_limit) throw Error("sse_line_too_large");
                line_ += ch;
            }
            previous_cr_ = ch == '\r';
        }
    }
    void finish() {
        if (!done_ || !line_.empty() || !data_.empty()) throw Error("truncated_chat_stream");
        LlmBackendEvent event;
        event.kind = LlmBackendEvent::Kind::finish;
        emit_(event);
    }
};

class ChatBackend final : public LlmBackend {
    CloudLlmConfig config_;
    std::mutex mutex_;
    std::shared_ptr<Cancellation> active_;
public:
    explicit ChatBackend(const CloudLlmConfig& config) : config_(config) {}
    bool uses_role_messages() const noexcept override { return true; }
    void initialize() override { validate_http_runtime(config_); }
    int run_request(const LlmRequest& request, Callback emit) override {
        auto token = std::make_shared<Cancellation>();
        struct Active {
            ChatBackend& owner;
            Active(ChatBackend& backend, const std::shared_ptr<Cancellation>& token) : owner(backend) {
                std::lock_guard<std::mutex> lock(owner.mutex_);
                owner.active_ = token;
            }
            ~Active() {
                std::lock_guard<std::mutex> lock(owner.mutex_);
                owner.active_.reset();
            }
        } active(*this, token);
        try {
            auto messages = Json::array();
            for (const auto& m : request.messages) messages.push_back({{"role", m.role}, {"content", m.content}});
            Json body = {{"model", config_.model}, {"messages", std::move(messages)}, {"stream", true}};
            if (config_.provider == "bailian") {
                body["max_completion_tokens"] = request.max_tokens;
                body["enable_thinking"] = false;
                body["preserve_thinking"] = false;
            } else {
                body["max_tokens"] = request.max_tokens;
            }
            ChatStream stream(emit);
            post(config_, body.dump(), "text/event-stream", config_.max_response_bytes,
                 *token, [&](auto bytes) { stream.feed(bytes); });
            stream.finish();
            return 0;
        } catch (const Error& e) {
            LlmBackendEvent event;
            event.kind = LlmBackendEvent::Kind::error;
            event.error_detail = e.what();
            emit(event);
        } catch (...) {
            LlmBackendEvent event;
            event.kind = LlmBackendEvent::Kind::error;
            event.error_detail = "chat_request_failed";
            emit(event);
        }
        return -1;
    }
    int abort() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_) active_->cancel();
        return 0;
    }
    int running() override {
        std::lock_guard<std::mutex> lock(mutex_);
        return active_ ? 1 : 0;
    }
    int clear() override { return 0; }
    int shutdown() override { return 0; }
};

void finalize_bailian_wav_header(std::string& wav) {
    // Bailian's downloaded WAV can retain its streaming length placeholders.
    // Accept only that canonical header after the HTTP layer has verified the
    // complete OSS object. All format and duration checks still run below.
    constexpr std::uint32_t streaming_riff_size = 0x7fffffbf;
    constexpr std::uint32_t streaming_data_size = streaming_riff_size - 36;
    if (wav.size() < wav_header_bytes || wav.compare(0, 4, "RIFF") != 0 ||
        wav.compare(8, 8, "WAVEfmt ") != 0 || get_le(wav, 16, 4) != 16 ||
        wav.compare(36, 4, "data") != 0 || get_le(wav, 4, 4) != streaming_riff_size ||
        get_le(wav, 40, 4) != streaming_data_size)
        return;
    put_le(wav, 4, static_cast<std::uint32_t>(wav.size() - 8), 4);
    put_le(wav, 40, static_cast<std::uint32_t>(wav.size() - wav_header_bytes), 4);
}

TtsNativeAudio decode_wav(const std::string& wav, int expected_rate, int max_ms) {
    if (wav.size() < wav_header_bytes || wav.compare(0, 4, "RIFF") != 0 ||
        wav.compare(8, 4, "WAVE") != 0 || get_le(wav, 4, 4) != wav.size() - 8)
        throw Error("invalid_wav_container");
    std::size_t pcm_offset = 0, pcm_bytes = 0;
    int sample_rate = 0;
    for (std::size_t p = 12; p < wav.size();) {
        if (wav.size() - p < 8) throw Error("truncated_wav_chunk");
        const auto size = std::size_t(get_le(wav, p + 4, 4));
        const auto padded = size + (size % 2);
        if (padded > wav.size() - p - 8) throw Error("truncated_wav_chunk");
        const auto content = p + 8;
        if (wav.compare(p, 4, "fmt ") == 0) {
            if (sample_rate || size < 16 || get_le(wav, content, 2) != 1 ||
                get_le(wav, content + 2, 2) != 1 || get_le(wav, content + 12, 2) != 2 ||
                get_le(wav, content + 14, 2) != 16)
                throw Error("wav_requires_mono_pcm16");
            const auto rate = get_le(wav, content + 4, 4);
            if (rate != static_cast<unsigned>(expected_rate) || get_le(wav, content + 8, 4) != rate * 2)
                throw Error("invalid_wav_sample_rate");
            sample_rate = static_cast<int>(rate);
        } else if (wav.compare(p, 4, "data") == 0) {
            if (pcm_offset || !size || size % 2) throw Error("invalid_wav_pcm_length");
            pcm_offset = content;
            pcm_bytes = size;
        }
        p += 8 + padded;
    }
    if (!sample_rate || !pcm_bytes || wav.size() - pcm_bytes > wav_metadata_budget)
        throw Error("invalid_wav_format");
    if (pcm_bytes / 2 > std::uint64_t(max_ms) * sample_rate / 1000)
        throw Error("wav_audio_too_long");
    TtsNativeAudio result;
    result.sample_rate = sample_rate;
    result.samples.resize(pcm_bytes / 2);
    for (std::size_t n = 0; n < result.samples.size(); ++n) {
        const auto value = int(get_le(wav, pcm_offset + 2 * n, 2));
        result.samples[n] = (value < 32768 ? value : value - 65536) / 32768.0f;
    }
    return result;
}
}

void validate_services(const CloudConfig& config) {
    if (config.asr.streaming()) {
#if VOICE_ENGINE_HAS_STREAMING_ASR
        validate_streaming_asr(config.asr);
#else
        throw ConfigError("dashscope_asr_ws requires VOICE_ENGINE_BUILD_STREAMING_ASR=ON");
#endif
    } else {
        validate_http_runtime(config.asr);
    }
    validate_http_runtime(config.llm);
    if (config.tts.streaming()) {
#if VOICE_ENGINE_HAS_STREAMING_TTS
        parse_websocket_endpoint(config.tts.endpoint);
#else
        throw ConfigError("dashscope_tts_ws requires VOICE_ENGINE_BUILD_STREAMING_TTS=ON");
#endif
    } else {
        validate_http_runtime(config.tts);
    }
}

Utterance::Utterance(std::size_t wav_bytes) : capacity_((wav_bytes - wav_header_bytes) / 2) {}

void Utterance::reset() {
    received_ = 0;
    wav_.resize(wav_header_bytes + 2 * capacity_);
}

void Utterance::append(const float* samples, std::size_t count) {
    for (std::size_t n = 0; n < count; ++n, ++received_) {
        const auto value = std::lround(std::clamp(samples[n], -1.0f, 1.0f) * 32767.0f);
        put_le(wav_, wav_header_bytes + 2 * (received_ % capacity_), static_cast<std::uint16_t>(value), 2);
    }
}

std::string Utterance::finish(const ListeningResult& result, const VadConfig& config) {
    if (!result.asr_finish_called) return {};
    const auto minimum = std::uint64_t(config.min_speech_ms) * 16;
    const auto segment = std::find_if(result.segments.begin(), result.segments.end(),
        [&](const VadSegment& s) { return s.end_sample >= s.start_sample + minimum; });
    if (segment == result.segments.end()) return {};
    const auto begin = segment->pre_roll_start_sample;
    const auto end = std::min(segment->end_sample, segment->start_sample + std::uint64_t(config.max_speech_ms) * 16);
    if (end > received_ || end <= begin || received_ - begin > capacity_ ||
        received_ != result.endpoint_sample)
        throw Error("incomplete_asr_recording");
    const auto length = 2 * static_cast<std::size_t>(end - begin);
    const auto offset = 2 * static_cast<std::size_t>(begin % capacity_);
    auto* pcm = wav_.data() + wav_header_bytes;
    if (offset + length <= 2 * capacity_) std::memmove(pcm, pcm + offset, length);
    else std::rotate(pcm, pcm + offset, pcm + 2 * capacity_);
    wav_.resize(wav_header_bytes + length);
    wav_.replace(0, 4, "RIFF");
    put_le(wav_, 4, static_cast<std::uint32_t>(wav_.size() - 8), 4);
    wav_.replace(8, 8, "WAVEfmt ");
    put_le(wav_, 16, 16, 4);
    put_le(wav_, 20, 1, 2);
    put_le(wav_, 22, 1, 2);
    put_le(wav_, 24, asr_rate, 4);
    put_le(wav_, 28, asr_rate * 2, 4);
    put_le(wav_, 32, 2, 2);
    put_le(wav_, 34, 16, 2);
    wav_.replace(36, 4, "data");
    put_le(wav_, 40, static_cast<std::uint32_t>(length), 4);
    return std::move(wav_);
}

std::string transcribe(const CloudAsrConfig& config, const std::string& wav,
                       std::size_t text_limit, Cancellation& cancellation) {
    std::string body;
    const auto append = [&](auto bytes) { body.append(bytes); };
    const bool bailian = config.provider == "bailian";
    if (bailian) {
        Json parameters = {{"format", "wav"}, {"sample_rate", "16000"}};
        if (!config.language.empty()) parameters["language_hints"] = Json::array({config.language});
        Json audio = {{"type", "input_audio"}, {"input_audio", {{"data", wav_data_uri(wav)}}}};
        Json message = {{"role", "user"}, {"content", Json::array({std::move(audio)})}};
        const auto request = Json{{"model", config.model}, {"parameters", std::move(parameters)},
                                  {"input", {{"messages", Json::array({std::move(message)})}}}}.dump();
        post(config, request, "", config.max_response_bytes, cancellation, append);
    } else {
        post(config, wav, "", config.max_response_bytes, cancellation, append, &config);
    }
    const auto response = parse_json(body);
    const auto* fields = bailian ? &bailian_output(response, "invalid_transcription") : &response;
    if (bailian && !fields->contains("text"))
        fields = &required_field(*fields, "sentence", "invalid_transcription");
    if (fields->contains("error")) throw Error("invalid_transcription");
    const auto& value = required_field(*fields, "text", "invalid_transcription");
    if (!value.is_string()) throw Error("invalid_transcription");
    const auto& text = value.get_ref<const std::string&>();
    if (text.size() > text_limit || text.find('\0') != std::string::npos)
        throw Error("invalid_transcription_text");
    const auto begin = text.find_first_not_of(" \t\r\n");
    return begin == std::string::npos ? std::string() :
        text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
}

std::unique_ptr<LlmBackend> make_llm(const CloudLlmConfig& config) {
    return std::make_unique<ChatBackend>(config);
}

TtsNativeAudio synthesize(const CloudTtsConfig& config, const std::string& text,
                          int max_audio_ms, Cancellation& cancellation) {
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::milliseconds(config.total_ms);
    const auto limit = wav_metadata_budget + std::size_t(max_audio_ms) * config.sample_rate * 2 / 1000;
    std::string wav;
    const auto append = [&](auto bytes) { wav.append(bytes); };
    if (config.provider == "bailian") {
        const auto request = Json{{"model", config.model}, {"input",
            {{"text", text}, {"voice", config.voice}, {"format", "wav"},
             {"sample_rate", config.sample_rate}, {"rate", config.speed}}}}.dump();
        std::string body;
        post(config, request, "", 64 * 1024, cancellation,
             [&](auto bytes) { body.append(bytes); }, nullptr, deadline);
        const auto response = parse_json(body);
        const auto& output = bailian_output(response, "invalid_bailian_tts_response");
        if (required_field(output, "finish_reason", "invalid_bailian_tts_response") != "stop")
            throw Error("incomplete_bailian_synthesis");
        const auto& audio = required_field(output, "audio", "invalid_bailian_tts_response");
        const auto& url = required_field(audio, "url", "invalid_bailian_tts_response");
        if (!url.is_string()) throw Error("invalid_bailian_audio_url");
        const auto remaining_bytes = static_cast<std::size_t>(config.max_response_bytes) - body.size();
        download_bailian_wav(config, url.get_ref<const std::string&>(), std::min(limit, remaining_bytes),
                             cancellation, append, deadline);
        finalize_bailian_wav_header(wav);
    } else {
        const auto request = Json{{"model", config.model}, {"input", text}, {"voice", config.voice},
                                  {"response_format", "wav"}, {"speed", config.speed}}.dump();
        post(config, request, "", limit, cancellation, append, nullptr, deadline);
    }
    auto audio = decode_wav(wav, config.sample_rate, max_audio_ms);
    audio.generate_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return audio;
}
}
