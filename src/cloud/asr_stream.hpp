#pragma once

#include "http.hpp"
#include <cstdint>
#include <memory>

namespace voice_engine::cloud {

struct StreamingAsrResult {
    std::string text;
    std::size_t audio_bytes = 0;
    std::int64_t task_ready_ns = 0, first_audio_sent_ns = 0, finish_sent_ns = 0;
};

void validate_streaming_asr(const CloudAsrConfig&);

// One connection and one server task per listening generation. PCM arrives
// from the VAD worker. Connection, send and receive run on a network thread.
class StreamingAsr {
public:
    StreamingAsr(const CloudAsrConfig&, std::size_t text_limit, Cancellation&,
                 const std::string& ca_file = {});
    ~StreamingAsr();
    StreamingAsr(const StreamingAsr&) = delete;
    StreamingAsr& operator=(const StreamingAsr&) = delete;
    void append(const float* pcm, std::size_t count, bool speech_confirmed);
    StreamingAsrResult finish();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace voice_engine::cloud
