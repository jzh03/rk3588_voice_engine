#pragma once
#include "voice_engine/tts.hpp"
#include <functional>
#include <optional>
namespace voice_engine {
struct TtsStreamId {
    std::uint64_t session = 0, generation = 0;
};
struct TtsStreamStats {
    TtsStreamId id;
    std::string status = "idle", error;
    std::size_t input_bytes = 0, segments = 0, frames = 0, consumed_frames = 0;
    std::size_t peak_input_bytes = 0, peak_segments = 0, peak_pcm_frames = 0;
    // Instantaneous queues; excludes text/audio currently owned by generate().
    std::size_t current_input_bytes = 0, current_segments = 0, current_pcm_frames = 0;
    std::size_t segmenter_pending_bytes = 0;
    std::size_t rejected_stale_inputs = 0, discarded_generation_results = 0;
    std::int64_t begin_ns = 0, first_segment_ns = 0, first_pcm_ns = 0;
    std::int64_t cancel_ns = 0, producer_done_ns = 0;
    double generate_seconds = 0;
};
struct TtsStreamFrame {
    TtsStreamId id;
    std::uint64_t seq = 0;
    PaddedAudioFrame audio;
};
// One consumer and one resident synthesis worker. submit/finish never wait for
// generation or PCM capacity. A full text queue fails and invalidates the turn;
// PCM backpressure blocks only the worker and is interrupted by cancel.
// generate() is synchronous; cancellation discards its result but cannot abort
// the upstream CPU model. A supervised process supplies the hard shutdown bound.
class TtsStreamWorker {
public:
    using Generate = std::function<TtsNativeAudio(const std::string&)>;
    TtsStreamWorker(Generate generate, TtsConfig text = {}, QueuesConfig queues = {},
                    int segment_timeout_ms = 10000);
    ~TtsStreamWorker();
    TtsStreamWorker(const TtsStreamWorker&) = delete;
    TtsStreamWorker& operator=(const TtsStreamWorker&) = delete;
    // Requires idle producer and a strictly newer (session,generation) pair.
    // Identities cannot be reused; clears completed old PCM.
    void begin(TtsStreamId id);
    bool submit(TtsStreamId id, const std::string& utf8_chunk);
    bool finish(TtsStreamId id);
    void cancel(TtsStreamId id);
    std::optional<TtsStreamFrame> poll(TtsStreamId id, int wait_ms = 0);
    // Atomic current-generation check plus a quick downstream commit.
    // No I/O, waiting, inference, or reentry into this worker inside sink.
    bool commit_if_current(TtsStreamId id, const std::function<void()>& sink);
    bool wait_idle(int timeout_ms);
    TtsStreamStats stats() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace voice_engine
