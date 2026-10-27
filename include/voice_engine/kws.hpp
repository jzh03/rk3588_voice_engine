#pragma once
#include "voice_engine/config.hpp"
#include <istream>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>
namespace voice_engine {
// A single explicit token line prevents a changed label from silently selecting
// another keyword. Tokens must belong to the selected model, never its blank/unk.
struct KeywordSpec {
    std::string word, encoded;
    std::vector<std::string> tokens;
    float score = 1.0f, threshold = 0.25f;
};
KeywordSpec parse_keyword(std::istream&, const std::string& expected_word,
                          const std::set<std::string>& vocabulary);
struct KeywordHit { std::uint64_t input_samples = 0; double token_end_seconds = 0; };
class KeywordSpotter {
public:
    // Caller validates enabled assets first. CPU only; one owner worker.
    explicit KeywordSpotter(const Config&);
    ~KeywordSpotter();
    void reset(); // Recreate stream: clear feature, encoder and decoder state.
    std::optional<KeywordHit> accept(const float*, std::size_t); // <=320 samples
    const KeywordSpec& keyword() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace voice_engine
