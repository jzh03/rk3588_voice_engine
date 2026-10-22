#include "voice_engine/tts.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace voice_engine;
namespace {
void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
template <class F> void rejects(F f) {
    try {
        f();
    } catch (const std::exception &) {
        return;
    }
    throw std::runtime_error("expected rejection");
}
std::string join(const std::vector<std::string> &v) {
    std::string s;
    for (const auto &x : v)
        s += x;
    return s;
}
void text_tests() {
    const std::string text = "你好！准备好了吗？我们现在开始。";
    require(join(segment_tts_text(text)) == text, "sentence split changed text");
    require(segment_tts_text("好。").size() == 1, "short EOS not flushed");
    TtsSegmenter s({2, 20, 300});
    require(s.push("你").empty(), "short fragment emitted");
    require(s.poll(500).empty(), "below-min timeout emitted");
    require(s.push("好").empty(), "unpunctuated text emitted early");
    require(s.poll(299).empty(), "early timer emitted");
    require(s.poll(300) == std::vector<std::string>{"你好"}, "timer failed");
    s.push("旧");
    s.reset();
    require(join(s.push("新。")) + join(s.finish()) == "新。", "reset leaked prior text");
    require(s.finish().empty(), "finish duplicated text");
    rejects([&] { s.push("后"); });
    const std::string date = "今天是2026年10月20日，温度23.5度。";
    TtsSegmenter chunks({2, 18, 300});
    std::vector<std::string> output;
    for (char c : date) {
        auto v = chunks.push(std::string(1, c));
        output.insert(output.end(), v.begin(), v.end());
    }
    auto tail = chunks.finish();
    output.insert(output.end(), tail.begin(), tail.end());
    require(join(output) == date, "cross-byte UTF8/date dropped data");
    require(
        std::any_of(output.begin(), output.end(),
                    [](const auto &x) { return x.find("2026年10月20日") != std::string::npos; }),
        "date split");
    require(std::any_of(output.begin(), output.end(),
                        [](const auto &x) { return x.find("23.5") != std::string::npos; }),
            "decimal split");
    for (const std::string token : {"-123.45", "+123.45", "2026-10-20", "2026/10/20"}) {
        rejects([&] { segment_tts_text(token, {1, 5, 300}); });
        const auto parts = segment_tts_text("前面" + token + "后面", {1, 12, 300});
        require(std::any_of(parts.begin(), parts.end(),
                            [&](const auto &x) { return x.find(token) != std::string::npos; }),
                "signed/date token split");
    }
    std::string long_text;
    for (int i = 0; i < 101; ++i)
        long_text += "中";
    auto parts = segment_tts_text(long_text);
    require(parts.size() == 3 && join(parts) == long_text, "long sentence split failed");
    for (const std::string bad : {"<think>秘密</think>回答", "<|im_start|>", "[INST]", "**粗体**",
                                  "# 标题", "`code`", "未闭合<", "", "   "})
        rejects([&] { segment_tts_text(bad); });
    rejects([&] { segment_tts_text(std::string("\xc0\xaf", 2)); });
    rejects([&] { segment_tts_text(std::string("\xed\xa0\x80", 3)); });
    rejects([&] { segment_tts_text(std::string("\xe4\xb8", 2)); });
    rejects([&] { segment_tts_text(std::string("a\0b", 3)); });
    s.reset();
    s.push("安全文本");
    rejects([&] { s.push("<thi"); });
}
void melo_frontend_tests() {
    const std::string original = "请打开浏览器，搜索 hello world。";
    require(melo_frontend_text(original) == "请打开浏览器、搜索 hello world。",
            "Melo comma context normalization failed");
    require(original == "请打开浏览器，搜索 hello world。" &&
                segment_tts_text(original) == std::vector<std::string>{original},
            "Melo normalization changed original text or application segmentation");
    require(melo_frontend_text("日期2026年10月20日，温度-23.5度，数量1,234。") ==
                "日期2026年10月20日、温度-23.5度、数量1,234。",
            "Melo normalization changed date, signed decimal or ASCII grouping");
    const std::string unchanged = "Hello, world! 1,234.50; 中文、英文？\n\t";
    require(melo_frontend_text(unchanged) == unchanged,
            "Melo normalization changed existing punctuation or whitespace");
    require(melo_frontend_text("你好，世界，，再见，") == "你好、世界、、再见、",
            "Melo normalization dropped adjacent or trailing commas");
    require(melo_frontend_text("没有逗号。") == "没有逗号。",
            "Melo normalization changed text without commas");
    const auto normalized = melo_frontend_text(original);
    require(melo_frontend_text(normalized) == normalized,
            "Melo normalization is not idempotent");
    for (const std::string bad : {"正常，<think>秘密</think>", "<|im_start|>", "[INST]",
                                  "**粗体**", "# 标题", "未闭合<"})
        rejects([&] { melo_frontend_text(bad); });
    rejects([&] { melo_frontend_text(std::string("\xc0\xaf", 2)); });
    rejects([&] { melo_frontend_text(std::string("\xed\xa0\x80", 3)); });
    rejects([&] { melo_frontend_text(std::string("正常，") + std::string("\xe4\xb8", 2)); });
    rejects([&] { melo_frontend_text(std::string("a\0b", 3)); });
    rejects([&] { melo_frontend_text(std::string("正常，") + "\xe2\x80\x8b"); });
}
std::vector<float> flatten(const std::vector<PaddedAudioFrame> &frames) {
    std::vector<float> out;
    for (const auto &f : frames) {
        require(f.valid_samples > 0 && f.valid_samples <= 320, "invalid frame count");
        out.insert(out.end(), f.samples.begin(), f.samples.begin() + f.valid_samples);
        for (std::size_t i = f.valid_samples; i < 320; ++i)
            require(f.samples[i] == 0, "tail padding nonzero");
    }
    return out;
}
void pcm_tests() {
    std::vector<float> input(44100 + 337);
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = .2F * std::sin(i * .1);
    TtsPcmConverter whole, chunked;
    auto a = whole.push(input.data(), input.size());
    auto t = whole.finish();
    a.insert(a.end(), t.begin(), t.end());
    auto reference = flatten(a);
    require(std::abs(static_cast<long>(reference.size()) -
                     static_cast<long>(std::llround(input.size() * 16000.0 / 44100))) <= 1,
            "44.1k conversion duration/tail loss");
    std::vector<PaddedAudioFrame> b;
    for (std::size_t i = 0; i < input.size();) {
        const auto n = std::min<std::size_t>(37, input.size() - i);
        auto f = chunked.push(input.data() + i, n);
        b.insert(b.end(), f.begin(), f.end());
        i += n;
    }
    t = chunked.finish();
    b.insert(b.end(), t.begin(), t.end());
    const auto actual = flatten(b);
    require(actual.size() == reference.size(), "chunked conversion length mismatch");
    for (std::size_t i = 0; i < actual.size(); ++i)
        require(std::abs(actual[i] - reference[i]) < 1e-5, "filter state lost across chunks");
    require(b.back().valid_samples < 320, "test did not exercise partial final frame");
    require(chunked.finish().empty(), "tail duplicated");
    rejects([&] { chunked.push(input.data(), 1); });
    chunked.reset();
    chunked.push(input.data(), 111);
    chunked.reset();
    std::vector<float> zero(44100, 0);
    auto z = chunked.push(zero.data(), zero.size());
    t = chunked.finish();
    z.insert(z.end(), t.begin(), t.end());
    const auto silence = flatten(z);
    require(std::all_of(silence.begin(), silence.end(), [](float x) { return x == 0; }),
            "reset leaked filter history");
}
} // namespace
int main() {
    try {
        text_tests();
        melo_frontend_tests();
        pcm_tests();
        std::cout << "TTS_TEXT_PCM_OK\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
