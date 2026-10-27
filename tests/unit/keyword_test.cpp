#include "voice_engine/kws.hpp"
#include <iostream>
#include <sstream>
#include <stdexcept>
using namespace voice_engine;
int main() {
    const std::set<std::string> tokens = {"<blk>", "<unk>", "n", "ǐ", "h", "ǎo", "x", "iǎo", "p", "ài"};
    auto parse = [&](const std::string& s) { std::istringstream in(s); return parse_keyword(in, "你好小派", tokens); };
    try {
        const auto k = parse("n ǐ h ǎo x iǎo p ài :1 #0.25 @你好小派\n");
        if (k.tokens.size() != 8 || k.threshold != .25f || k.score != 1) throw std::runtime_error("valid encoding");
        for (const auto& bad : {"", "n @别的词", "UNKNOWN @你好小派", "<blk> @你好小派",
             "n #nan @你好小派", "n #0 @你好小派", "n #1.01 @你好小派",
             "n :inf @你好小派", "n :1 :2 @你好小派", "n #.2 #.3 @你好小派",
             "n @你好小派 extra", "n #.2 h @你好小派", "n\nn @你好小派", "n @你好小派\nn @你好小派"}) {
            bool rejected = false; try { parse(bad); } catch (const ConfigError&) { rejected = true; }
            if (!rejected) throw std::runtime_error(std::string("accepted invalid keyword: ") + bad);
        }
        std::cout << "KEYWORD_CONFIG_OK\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
