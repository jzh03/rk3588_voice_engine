#include "voice_engine/kws.hpp"
#include <cmath>
#include <sstream>
namespace voice_engine {
KeywordSpec parse_keyword(std::istream& input, const std::string& expected,
                          const std::set<std::string>& vocabulary) {
    KeywordSpec result;
    std::string line, selected;
    std::size_t bytes = 0;
    while (std::getline(input, line)) {
        bytes += line.size() + 1;
        if (bytes > 4096) throw ConfigError("keyword file exceeds 4096 bytes");
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
        if (!selected.empty()) throw ConfigError("keyword file must contain exactly one nonempty line");
        selected = line;
    }
    bool score = false, threshold = false, label = false, metadata = false;
    std::istringstream fields(selected); std::string field;
    auto number = [](const std::string& value, float low, float high) {
        std::size_t consumed = 0;
        float n;
        try { n = std::stof(value, &consumed); }
        catch (...) { throw ConfigError("invalid keyword score/threshold"); }
        if (consumed != value.size() || !std::isfinite(n) || n <= low || n > high)
            throw ConfigError("invalid keyword score/threshold");
        return n;
    };
    while (fields >> field) {
        if (label) throw ConfigError("keyword label must be the final item");
        if (field[0] == ':') {
            if (score) throw ConfigError("duplicate keyword score");
            result.score = number(field.substr(1), 0, 10); score = metadata = true;
        } else if (field[0] == '#') {
            if (threshold) throw ConfigError("duplicate keyword threshold");
            result.threshold = number(field.substr(1), 0, 1); threshold = metadata = true;
        } else if (field[0] == '@') {
            result.word = field.substr(1); label = metadata = true;
        } else {
            if (metadata || !vocabulary.count(field) || field[0] == '<' || result.tokens.size() >= 64)
                throw ConfigError("invalid keyword token or token order");
            result.tokens.push_back(field);
        }
    }
    if (result.tokens.empty() || !label || result.word.empty() || result.word != expected)
        throw ConfigError("keyword @label must match dialogue.wake_word and contain valid tokens");
    std::ostringstream encoded;
    for (const auto& token : result.tokens) encoded << token << ' ';
    encoded << ':' << result.score << " #" << result.threshold << " @" << result.word << '\n';
    result.encoded = encoded.str();
    return result;
}
} // namespace voice_engine
