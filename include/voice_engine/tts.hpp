#pragma once
#include "voice_engine/audio_dsp.hpp"
#include "voice_engine/config.hpp"
#include "voice_engine/tts_cancellation.hpp"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace voice_engine {
// Fail closed: only plain text is accepted. Tags, special-token delimiters,
// Markdown controls and invalid UTF-8 throw; rejected text is never sanitized.
// push() accepts partial UTF-8 tokens. finish() flushes a short final sentence;
// poll() flushes >= min_chars after the configured delay. Character limits
// count Unicode scalar values; indivisible numeric/date or English tokens
// exceeding max_chars are rejected rather than split. Optional target_chars is
// a soft preference: a longer indivisible token may extend up to max_chars.
// Optional boundary lookahead seeks nearby clause punctuation after the target;
// its bounded grace starts at the first poll after reaching the target. This is
// not general Chinese word segmentation. Zero preserves the legacy behavior.
// sentence_mode instead emits an early first clause and preserves later full
// sentences. Its timer may release an existing clause/word boundary, never an
// arbitrary short character prefix; EOS and max_chars remain bounded exits.
class TtsSegmenter {
  public:
    explicit TtsSegmenter(TtsConfig config = {});
    std::vector<std::string> push(const std::string &chunk);
    std::vector<std::string> poll(int elapsed_ms);
    std::vector<std::string> finish();
    void reset() noexcept;
    std::size_t pending_bytes() const noexcept { return pending_.size(); }

  private:
    std::vector<std::string> drain(bool final, bool timed);
    TtsConfig config_;
    std::string pending_;
    bool finished_ = false;
    bool first_fragment_emitted_ = false;
    bool waiting_for_boundary_ = false;
    int boundary_wait_from_ms_ = -1;
};
std::vector<std::string> segment_tts_text(const std::string &, TtsConfig = {});
// Validate an already selected segment independently of first/later policy.
// Reject invalid plain text, empty/edge-whitespace text and >max_chars scalars.
void validate_tts_segment(const std::string &, int max_chars);

// Fixed Melo frontend input: validates strict plain-text UTF-8, then maps only
// U+FF0C to U+3001, the same comma token without a per-comma acoustic split in
// the frozen frontend. Original text/segments remain the caller's record;
// With join_short_sentences, internal Chinese sentence punctuation becomes the
// same non-splitting comma token: short sentences already grouped by the bounded
// segmenter share one acoustic sequence. Trailing punctuation/closing quotes are
// preserved. No new text wait, cross-segment merge, audio splice or larger limit.
// ASCII punctuation (including numeric grouping/decimal points) is unchanged.
std::string melo_frontend_text(const std::string &plain_text,
                               bool join_short_sentences = false);

// Each stream retains resampler filter state across native blocks and segments.
// Only finish() pads the last 20 ms frame; valid_samples excludes zero padding.
class TtsPcmConverter {
  public:
    explicit TtsPcmConverter(int native_sample_rate = 44100);
    std::vector<PaddedAudioFrame> push(const float *, std::size_t);
    std::vector<PaddedAudioFrame> finish();
    void reset();

  private:
    std::vector<PaddedAudioFrame> frame(const std::vector<float> &);
    StreamResampler resampler_;
    MonoFrameAssembler assembler_;
    std::size_t max_input_;
    bool finished_ = false;
};
struct TtsNativeAudio {
    std::vector<float> samples;
    int sample_rate = 44100;
    double generate_seconds = 0; // Exact blocking sherpa Generate call, excludes validation/copy.
};
struct MeloTtsAssets {
    std::string model_id;
    std::filesystem::path model, tokens, lexicon, dictionary;
    std::filesystem::path effective_lexicon, pronunciation_rules;
    std::size_t pronunciation_rule_count = 0;
    std::vector<std::filesystem::path> rule_fsts;
};
// Validates a copy with only TTS enabled, then resolves named manifest roles.
// The other models, including LLM, are neither loaded nor validated here.
MeloTtsAssets resolve_melo_tts_assets(const Config &);
// Optional word-level pronunciation supplement, never text substitution.
// Validates phones against the frozen token inventory; writes a new exclusive
// derived lexicon and leaves the original model package untouched.
std::size_t prepare_tts_pronunciation_lexicon(const std::filesystem::path &base,
    const std::filesystem::path &tokens, const std::filesystem::path &rules,
    const std::filesystem::path &destination);

class MeloTts {
  public:
    MeloTts(const Config &, int threads, bool debug_frontend = false,
            const std::filesystem::path &pronunciation_rules = {},
            const std::filesystem::path &derived_lexicon = {});
    ~MeloTts();
    MeloTts(const MeloTts &) = delete;
    MeloTts &operator=(const MeloTts &) = delete;
    // One resident model; CPU, sid=0, configured speed (default 1), frozen FSTs.
    // Requires the common validation lock for construction and generation.
    // Optional per-generation token interrupts native ONNX Run. Cancellation
    // is cooperative between operators; wait for the caller to actually return.
    TtsNativeAudio generate(const std::string &plain_segment,
                            std::shared_ptr<TtsCancellation> cancellation = {});
    std::vector<std::string> unknown_english_words(const std::string &) const;
    const MeloTtsAssets &assets() const noexcept;
    static std::string runtime_version();
    double model_initialization_seconds() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace voice_engine
