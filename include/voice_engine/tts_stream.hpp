#pragma once
#include "voice_engine/tts.hpp"
#include <functional>
#include <optional>
namespace voice_engine {
struct TtsStreamId {
    std::uint64_t session = 0, generation = 0;
};
// Monotonic software observations, no private text or PCM. A fixed per-turn
// history avoids I/O and unbounded allocation on synthesis/publication paths.
inline constexpr std::size_t kTtsSegmentTimingCapacity = 128;
struct TtsSegmentTiming {
    std::uint64_t segment_seq = 0;
    std::size_t text_bytes = 0, text_chars = 0, native_samples = 0;
    int min_chars = 0, target_chars = 0, boundary_lookahead_chars = 0;
    int native_sample_rate = 0;
    std::int64_t text_selected_ns = 0, native_wait_begin_ns = 0, native_slot_acquired_ns = 0;
    std::int64_t generate_begin_ns = 0, generate_end_ns = 0, native_enqueued_ns = 0;
    double sdk_generate_seconds = 0;
    std::size_t input_queued_bytes = 0, pcm_frames_before_generate = 0;
    std::size_t native_samples_before_generate = 0;
    std::int64_t publish_begin_ns = 0, resample_end_ns = 0, publish_end_ns = 0;
    std::int64_t first_pcm_enqueued_ns = 0, last_pcm_enqueued_ns = 0;
    std::int64_t pcm_queue_wait_ns = 0;
    std::uint64_t first_pcm_seq = 0, last_pcm_seq = 0;
    std::size_t pcm_frames = 0;
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
    std::int64_t native_run_started_ns = 0;
    std::uint64_t native_runs = 0;
    std::size_t segment_timings_dropped = 0;
    // Native prefetch owns at most two segments, including publisher/inference
    // reservations. Each validated segment obeys max_tts_audio_ms.
    std::size_t native_buffered_segments = 0, native_buffered_samples = 0;
    std::size_t peak_native_slots = 0, peak_native_samples = 0;
    std::int64_t first_generated_audio_ns = 0, playback_release_ns = 0;
};
struct TtsStreamFrame {
    TtsStreamId id;
    std::uint64_t seq = 0;
    PaddedAudioFrame audio;
};
// One resident synthesis worker; optional independent PCM publisher for native
// lookahead when text.prebuffer_ms > 0. submit/finish never wait for generation.
// PCM backpressure blocks delivery, while one following segment can synthesize.
// Startup releases at the real-audio reserve, two occupied native slots, or EOS.
// Cancel invalidates both queues; wait_idle includes both workers and inference.
// CancellableGenerate receives a distinct token per generation, including when
// cancellation races with entry into generate(). Idle still means inference has
// actually returned. Legacy Generate callbacks only discard their late result;
// the unchanged controller deadline supervises an unresponsive native call.
class TtsStreamWorker {
public:
    using Generate = std::function<TtsNativeAudio(const std::string&)>;
    using CancellableGenerate = std::function<TtsNativeAudio(
        const std::string&, const std::shared_ptr<TtsCancellation>&)>;
    TtsStreamWorker(CancellableGenerate generate, TtsConfig text = {}, QueuesConfig queues = {},
                    int segment_timeout_ms = 10000);
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
    // Copies at most 128 entries on explicit request, not during stats()/poll().
    // In-progress entries have zero completion fields. Stale/cancelled ids yield
    // no records. PCM ranges cover converter.push; EOS flush is not attributed.
    std::vector<TtsSegmentTiming> segment_timings(TtsStreamId id) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace voice_engine
