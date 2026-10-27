#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <array>
#include <chrono>
#include <functional>
#include <vector>
#include "voice_engine/vad.hpp"
#include <memory>
#include <string>
namespace voice_engine {
struct ZipformerFiles {
    std::filesystem::path encoder, decoder, joiner, vocabulary, feature_config;
};
struct AsrStats {
    std::uint64_t input_samples=0, encoder_chunks=0, tail_padding_samples=0;
    std::size_t peak_feature_frames=0;
};
// Single-worker, 16 kHz mono normalized PCM. Accept consumes bounded subblocks
// immediately; it does not retain utterance PCM. Finish is idempotent. Reset
// clears fbank history, all 35 encoder states, decoder tokens and output text.
class ZipformerAsr {
public:
    explicit ZipformerAsr(const ZipformerFiles&, std::size_t max_text_bytes=8192);
    ~ZipformerAsr();
    ZipformerAsr(const ZipformerAsr&)=delete;
    ZipformerAsr& operator=(const ZipformerAsr&)=delete;
    void accept(const float* pcm, std::size_t count);
    std::string finish();
    void reset();
    const std::string& partial() const;
    AsrStats stats() const;
    std::string runtime_info_json() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

enum class ListeningState { idle, listening, finalizing, final, cancelled, error };
enum class ListeningEnd { none, vad_silence, start_timeout, max_speech, input_finished, cancelled, error };
const char* listening_end_name(ListeningEnd);

// Callback seams keep the single-worker turn logic independent of model SDKs.
// The production constructor below binds the existing VAD/Zipformer adapters.
struct ListeningBackends {
    std::function<void(std::size_t pre_roll_samples)> reset;
    std::function<void(const float*, std::size_t)> accept_asr, accept_vad;
    std::function<void()> finish_vad;
    std::function<std::string()> finish_asr, partial_asr;
    std::function<AsrStats()> asr_stats;
    std::function<VadStats()> vad_stats;
    std::function<std::vector<VadSegment>()> vad_segments;
};
struct ListeningResult {
    ListeningEnd reason=ListeningEnd::none;
    std::string hypothesis, unfiltered_hypothesis;
    // Sample zero is the first pre-roll sample. Fresh input begins at pre_roll_samples.
    std::uint64_t pre_roll_samples=0, input_samples=0, endpoint_sample=0;
    std::uint64_t chunks_before_finish=0;
    AsrStats asr;
    VadStats vad;
    std::vector<VadSegment> segments;
    double endpoint_to_final_sec=0;
    bool asr_finish_called=false, backends_reset=false;
};

// A reusable one-turn LISTENING worker, not the application state machine.
// Both adapters receive the same original PCM, in 512-sample endpoint quanta.
// begin() may receive actual pre-roll PCM once (up to config.pre_roll_ms).
// start_timeout excludes that prefix; all VAD/ASR offsets include it.
// accept() returns the exact fresh samples consumed; any remainder belongs to
// the caller and is never silently submitted after an endpoint. EOF/manual stop
// must call finish() to consume the last <512 valid samples (no ASR fake padding).
// finish() is idempotent, snapshots final then resets both adapters. cancel()
// discards pending/text state and resets without final decoding. Calls are
// synchronous on one worker; cancel cannot preempt an in-flight SDK call.
class ListeningAsr {
public:
    ListeningAsr(ZipformerAsr&, SileroVad&, const VadConfig&);
    ListeningAsr(ListeningBackends, const VadConfig&);
    // Optional monotonic deadline applies only until VAD confirms speech.
    // Zero preserves sample-only timing for file probes and the button mode.
    void begin(const float* pre_roll=nullptr, std::size_t count=0,
               std::int64_t start_deadline_ns=0);
    bool poll_start_timeout(std::int64_t now_ns);
    std::size_t accept(const float*, std::size_t);
    const ListeningResult& finish();
    void cancel();
    ListeningState state() const { return state_; }
    const ListeningResult& result() const { return result_; }
    std::string partial() const;
    std::size_t buffered_samples() const { return used_; }
private:
    void consume(const float*, std::size_t, bool check_endpoint);
    bool endpoint();
    void complete(ListeningEnd);
    void fail() noexcept;
    void reset_backends(std::size_t);
    ListeningBackends backends_;
    VadConfig config_;
    ListeningState state_=ListeningState::idle;
    ListeningResult result_;
    std::array<float,512> pending_{};
    std::size_t used_=0;
    std::int64_t start_deadline_ns_=0;
    std::chrono::steady_clock::time_point endpoint_time_;
};
} // namespace voice_engine
