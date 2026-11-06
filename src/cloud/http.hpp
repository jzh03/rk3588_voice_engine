#pragma once

#include "voice_engine/config.hpp"
#include "voice_engine/cancellation.hpp"
#include <chrono>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace voice_engine::cloud {
using Deadline = std::chrono::steady_clock::time_point;
using ResponseSink = std::function<void(std::string_view)>;
class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

void validate_http_runtime(const CloudService&);
// data is the serialized JSON or the WAV. The latter is referenced by
// curl_mime_data_cb for the duration of the request, without a full copy.
void post(const CloudService&, const std::string& data, std::string_view expected_type,
          std::size_t response_limit, Cancellation&, const ResponseSink&,
          const CloudAsrConfig* multipart = nullptr, Deadline deadline = Deadline::max());
// DashScope TTS returns a signed OSS object URL. The GET shares the synthesis
// deadline and token, and does not send the API bearer credential.
void download_bailian_wav(const CloudTtsConfig&, const std::string& url,
                          std::size_t response_limit, Cancellation&, const ResponseSink&, Deadline);
}
