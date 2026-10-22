#include "voice_engine/tts.hpp"
#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace voice_engine {
namespace {
struct Rune {
    std::uint32_t c;
    std::size_t end;
};
std::vector<Rune> decode(const std::string &s, bool final) {
    std::vector<Rune> out;
    for (std::size_t i = 0; i < s.size();) {
        const auto b = static_cast<unsigned char>(s[i]);
        const std::size_t n =
            b < 0x80 ? 1
                     : (b >= 0xc2 && b <= 0xdf
                            ? 2
                            : (b >= 0xe0 && b <= 0xef ? 3 : (b >= 0xf0 && b <= 0xf4 ? 4 : 0)));
        if (!n)
            throw std::invalid_argument("TTS: invalid UTF-8 lead byte");
        if (i + n > s.size()) {
            if (final)
                throw std::invalid_argument("TTS: incomplete UTF-8");
            break;
        }
        std::uint32_t c = b & (n == 1 ? 0x7f : (n == 2 ? 0x1f : (n == 3 ? 0x0f : 0x07)));
        for (std::size_t j = 1; j < n; ++j) {
            const auto x = static_cast<unsigned char>(s[i + j]);
            if ((x & 0xc0) != 0x80)
                throw std::invalid_argument("TTS: invalid UTF-8 continuation");
            c = (c << 6) | (x & 0x3f);
        }
        if ((n == 2 && c < 0x80) || (n == 3 && c < 0x800) || (n == 4 && c < 0x10000) ||
            (c >= 0xd800 && c <= 0xdfff) || c > 0x10ffff)
            throw std::invalid_argument("TTS: noncanonical UTF-8");
        if ((c < 32 && c != '\n' && c != '\t' && c != '\r') || c == 127 ||
            (c >= 0x80 && c <= 0x9f) || c == 0x200b || (c >= 0x202a && c <= 0x202e) ||
            (c >= 0x2066 && c <= 0x2069))
            throw std::invalid_argument("TTS: hidden/control character rejected");
        if (c < 128 && std::string("<>[]{}|`*_#\\").find(static_cast<char>(c)) != std::string::npos)
            throw std::invalid_argument(
                "TTS: tags, LLM tokens and Markdown controls are rejected, not cleaned");
        i += n;
        out.push_back({c, i});
    }
    return out;
}
bool digit(std::uint32_t c) { return c >= '0' && c <= '9'; }
bool letter(std::uint32_t c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
bool numeric_join(std::uint32_t c) {
    return digit(c) || c == '.' || c == '+' || c == '-' || c == '/' || c == ':' || c == 0x5e74 ||
           c == 0x6708 || c == 0x65e5 || c == 0x70b9 || c == 0x5206 || c == 0x65f6 || c == '%';
}
bool whitespace(std::uint32_t c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0x3000;
}
bool terminal(std::uint32_t c) {
    return c == 0x3002 || c == 0xff01 || c == 0xff1f || c == '!' || c == '?' || c == ';' ||
           c == 0xff1b || c == '\n';
}
// No cut inside ASCII words or numeric expressions (including Chinese dates).
bool safe_cut(const std::vector<Rune> &r, std::size_t n, bool final) {
    if (n == 0)
        return false;
    const auto a = r[n - 1].c;
    if (n == r.size())
        return final || !(letter(a) || numeric_join(a));
    const auto b = r[n].c;
    if (letter(a) && letter(b))
        return false;
    if (numeric_join(a) && numeric_join(b)) {
        std::size_t j = n;
        while (j > 0 && numeric_join(r[j - 1].c))
            --j;
        if (j < n && (digit(r[j].c) || r[j].c == '-' || r[j].c == '+'))
            return false;
    }
    return true;
}
std::string trim(std::string s) {
    const auto r = decode(s, true);
    std::size_t first = 0, last = r.size();
    while (first < last && whitespace(r[first].c))
        ++first;
    while (last > first && whitespace(r[last - 1].c))
        --last;
    const auto begin = first ? r[first - 1].end : 0;
    const auto end = last ? r[last - 1].end : 0;
    return s.substr(begin, end - begin);
}
} // namespace
TtsSegmenter::TtsSegmenter(TtsConfig c) : config_(c) {
    if (c.min_chars < 1 || c.max_chars < c.min_chars || c.flush_ms < 1)
        throw std::invalid_argument("TTS: invalid segmentation limits");
}
std::vector<std::string> TtsSegmenter::push(const std::string &chunk) {
    if (finished_)
        throw std::logic_error("TTS: push after finish");
    if (pending_.size() + chunk.size() > 8192)
        throw std::invalid_argument("TTS: pending text exceeds 8192 bytes");
    pending_ += chunk;
    return drain(false, false);
}
std::vector<std::string> TtsSegmenter::poll(int elapsed_ms) {
    if (finished_)
        return {};
    return drain(false, elapsed_ms >= config_.flush_ms);
}
std::vector<std::string> TtsSegmenter::finish() {
    if (finished_)
        return {};
    auto result = drain(true, true);
    finished_ = true;
    return result;
}
void TtsSegmenter::reset() noexcept {
    pending_.clear();
    finished_ = false;
}
std::vector<std::string> TtsSegmenter::drain(bool final, bool timed) {
    std::vector<std::string> result;
    while (!pending_.empty()) {
        const auto r = decode(pending_, final);
        std::size_t cut = 0;
        const auto limit = std::min(r.size(), static_cast<std::size_t>(config_.max_chars));
        for (std::size_t i = 0; i < limit; ++i) {
            bool end = terminal(r[i].c);
            if (r[i].c == '.')
                end = (i + 1 < r.size() ? !digit(r[i + 1].c) : final) &&
                      !(i > 0 && digit(r[i - 1].c));
            if (end && i + 1 >= static_cast<std::size_t>(config_.min_chars) &&
                safe_cut(r, i + 1, final)) {
                cut = i + 1;
                break;
            }
        }
        if (!cut && r.size() >= static_cast<std::size_t>(config_.max_chars)) {
            cut = limit;
            while (cut && !safe_cut(r, cut, final))
                --cut;
            if (!cut && (r.size() > limit || final))
                throw std::invalid_argument(
                    "TTS: indivisible numeric/date/word token exceeds max_chars");
        }
        if (!cut && ((final && !r.empty()) ||
                     (timed && r.size() >= static_cast<std::size_t>(config_.min_chars)))) {
            cut = r.size();
            while (cut && !safe_cut(r, cut, final))
                --cut;
        }
        if (!cut)
            break;
        auto part = trim(pending_.substr(0, r[cut - 1].end));
        pending_.erase(0, r[cut - 1].end);
        if (!part.empty())
            result.push_back(std::move(part));
    }
    return result;
}
std::vector<std::string> segment_tts_text(const std::string &text, TtsConfig c) {
    // Validate the entire input before any caller may synthesize an early segment.
    decode(text, true);
    TtsSegmenter s(c);
    auto out = s.push(text);
    auto tail = s.finish();
    out.insert(out.end(), tail.begin(), tail.end());
    if (out.empty())
        throw std::invalid_argument("TTS: empty/whitespace-only text");
    return out;
}
std::string melo_frontend_text(const std::string &plain_text) {
    // Validate the complete input before normalization; never clean rejected text.
    const auto runes = decode(plain_text, true);
    std::string result;
    result.reserve(plain_text.size());
    std::size_t begin = 0;
    for (const auto &r : runes) {
        // sherpa 1.12.14 maps both runes to the same comma token, but U+3001
        // keeps acoustic context across the comma and receives AddBlank once.
        // This preserves words/tones rather than overriding any pronunciation.
        if (r.c == 0xff0c)
            result += "\xe3\x80\x81";
        else
            result.append(plain_text, begin, r.end - begin);
        begin = r.end;
    }
    return result;
}
TtsPcmConverter::TtsPcmConverter(int rate) : resampler_(rate, 16000), max_input_(rate) {}
std::vector<PaddedAudioFrame> TtsPcmConverter::frame(const std::vector<float> &samples) {
    std::vector<PaddedAudioFrame> result;
    for (std::size_t i = 0; i < samples.size();) {
        const auto n = std::min<std::size_t>(32000, samples.size() - i);
        for (const auto &x : assembler_.push(samples.data() + i, n))
            result.push_back({x, 320});
        i += n;
    }
    return result;
}
std::vector<PaddedAudioFrame> TtsPcmConverter::push(const float *p, std::size_t n) {
    if (finished_)
        throw std::logic_error("TTS PCM: push after finish");
    if (n && p == nullptr)
        throw std::invalid_argument("TTS PCM: null data");
    std::vector<PaddedAudioFrame> result;
    for (std::size_t i = 0; i < n;) {
        const auto count = std::min(max_input_, n - i);
        auto x = frame(resampler_.process(p + i, count));
        result.insert(result.end(), x.begin(), x.end());
        i += count;
    }
    return result;
}
std::vector<PaddedAudioFrame> TtsPcmConverter::finish() {
    if (finished_)
        return {};
    auto out = frame(resampler_.finish());
    const auto tail = assembler_.finish();
    if (tail)
        out.push_back(*tail);
    finished_ = true;
    return out;
}
void TtsPcmConverter::reset() {
    resampler_.reset();
    assembler_.reset();
    finished_ = false;
}
} // namespace voice_engine
