#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
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
} // namespace voice_engine
