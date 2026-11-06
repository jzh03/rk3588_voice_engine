#pragma once

#include "http.hpp"

namespace voice_engine::cloud {
using PcmSink = std::function<bool(const float*, std::size_t)>;
void stream_speech(const CloudTtsConfig&, const std::string& text, int max_audio_ms,
                   Cancellation&, const PcmSink&, const std::string& ca_file = {});
} // namespace voice_engine::cloud
