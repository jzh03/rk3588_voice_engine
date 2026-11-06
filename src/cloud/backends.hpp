#pragma once

#include "http.hpp"
#include "voice_engine/asr.hpp"
#include "voice_engine/llm.hpp"
#include "voice_engine/tts.hpp"

namespace voice_engine::cloud {
void validate_services(const CloudConfig&);

class Utterance {
public:
    explicit Utterance(std::size_t wav_bytes);
    void reset();
    void append(const float*, std::size_t);
    // Uses absolute VAD coordinates. Earlier silence can age out; an
    // incomplete eligible speech span is rejected instead of uploaded.
    std::string finish(const ListeningResult&, const VadConfig&);
private:
    std::string wav_;
    std::size_t capacity_;
    std::uint64_t received_ = 0;
};

std::string transcribe(const CloudAsrConfig&, const std::string& wav,
                       std::size_t text_limit, Cancellation&);
std::unique_ptr<LlmBackend> make_llm(const CloudLlmConfig&);
TtsNativeAudio synthesize(const CloudTtsConfig&, const std::string& text,
                          int max_audio_ms, Cancellation&);
}
