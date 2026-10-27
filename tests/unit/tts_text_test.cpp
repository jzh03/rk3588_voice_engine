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

void soft_target_tests() {
    TtsConfig c; c.target_chars=12;
    std::string text;
    for(int i=0;i<36;++i)text+="中";
    auto parts=segment_tts_text(text,c);
    require(parts.size()==3 && join(parts)==text,"soft target did not split uniform Chinese");
    TtsSegmenter pending(c);
    require(pending.push("configuratio").empty(),"partial English token emitted");
    require(pending.push("n").empty(),"unfinished English token emitted");
    auto word=pending.push("完成。");
    auto tail=pending.finish(); word.insert(word.end(),tail.begin(),tail.end());
    require(join(word)=="configuration完成。","long soft-target word changed");
    require(word.front()=="configuration","soft target split long English token");
    const std::string mixed="前面说明configuration已经完成。今天是2026年10月26日15时30分，温度-123.456度。";
    for(int chunk: {1,2,5,17,200}) {
        TtsSegmenter stream(c);std::vector<std::string> out;
        for(std::size_t i=0;i<mixed.size();i+=chunk) {
            auto next=stream.push(mixed.substr(i,chunk));out.insert(out.end(),next.begin(),next.end());
            next=stream.poll(300);out.insert(out.end(),next.begin(),next.end());
        }
        auto rest=stream.finish();out.insert(out.end(),rest.begin(),rest.end());
        require(join(out)==mixed,"soft target dropped/reordered UTF8 text");
        for(const std::string token:{"configuration","2026年10月26日15时30分","-123.456"})
            require(std::any_of(out.begin(),out.end(),[&](const auto& s){return s.find(token)!=std::string::npos;}),
                    "soft target split an indivisible token");
    }
    require(segment_tts_text("好。",c)==std::vector<std::string>{"好。"},"soft target lost short EOS");
    rejects([&]{segment_tts_text(std::string(41,'a'),c);});
    c.target_chars=7; rejects([&]{TtsSegmenter invalid(c);});
    c.target_chars=41; rejects([&]{TtsSegmenter invalid(c);});
    c.target_chars=-1; rejects([&]{TtsSegmenter invalid(c);});
}


void boundary_lookahead_tests(int lookahead) {
    TtsConfig c; c.min_chars=12; c.target_chars=12; c.boundary_lookahead_chars=lookahead;
    const std::string text="我不知道你所在的具体位置，无法提供附近图书馆的坐标或名称。请告诉我你的城市或区域，我可以尝试帮你查找。";
    for(int chunk: {1,2,5,17,200}) {
        TtsSegmenter stream(c); std::vector<std::string> out; int elapsed=0;
        for(std::size_t i=0;i<text.size();i+=chunk) {
            auto next=stream.push(text.substr(i,chunk));out.insert(out.end(),next.begin(),next.end());
            next=stream.poll(elapsed+=10);out.insert(out.end(),next.begin(),next.end());
        }
        auto rest=stream.finish();out.insert(out.end(),rest.begin(),rest.end());
        require(join(out)==text,"lookahead changed UTF8 text");
        require(out.front()=="我不知道你所在的具体位置，","lookahead missed nearby comma");
        for(const std::string word: {"坐标","城市"})
            require(std::any_of(out.begin(),out.end(),[&](const auto& s){return s.find(word)!=std::string::npos;}),
                    "lookahead split frozen Chinese word");
    }
    const std::string first="我不知道你所在的具体位置";
    TtsSegmenter grace(c);
    require(grace.push(first).empty(),"lookahead emitted at target without grace");
    require(grace.poll(1200).empty(),"lookahead inherited expired pending timer");
    require(grace.poll(1499).empty(),"lookahead grace ended early");
    require(grace.poll(1500)==std::vector<std::string>{first},"lookahead grace unbounded");
    grace.reset();
    require(grace.push(first).empty(),"reset lost lookahead");
    grace.poll(0);
    require(grace.push("，")==std::vector<std::string>{first+"，"},"punctuation did not release grace");
    grace.reset();
    require(grace.push(first+"还有更多文字").front()==first,"lookahead character bound failed");
    const std::string mixed="前面说明configuration已经完成。今天是2026年10月26日15时30分，温度-123.456度。";
    for(int chunk: {1,2,5,17,200}) {
        TtsSegmenter stream(c);std::vector<std::string> out; int elapsed=0;
        for(std::size_t i=0;i<mixed.size();i+=chunk) {
            auto next=stream.push(mixed.substr(i,chunk));out.insert(out.end(),next.begin(),next.end());
            next=stream.poll(elapsed+=50);out.insert(out.end(),next.begin(),next.end());
        }
        auto rest=stream.finish();out.insert(out.end(),rest.begin(),rest.end());
        require(join(out)==mixed,"lookahead dropped mixed text");
        for(const std::string token: {"configuration","2026年10月26日15时30分","-123.456"})
            require(std::any_of(out.begin(),out.end(),[&](const auto& s){return s.find(token)!=std::string::npos;}),
                    "lookahead split protected token");
    }
    require(segment_tts_text("好。",c)==std::vector<std::string>{"好。"},"lookahead lost short EOS");
    rejects([&]{segment_tts_text(std::string(41,'a'),c);});
    c.target_chars=0;rejects([&]{TtsSegmenter invalid(c);});
    c.target_chars=39;rejects([&]{TtsSegmenter invalid(c);});
    c.target_chars=12;c.boundary_lookahead_chars=-1;rejects([&]{TtsSegmenter invalid(c);});
    c.boundary_lookahead_chars=33;rejects([&]{TtsSegmenter invalid(c);});
}


void sentence_mode_tests() {
    TtsConfig c;c.min_chars=12;c.sentence_mode=true;
    const std::vector<std::string> expected{
        "我不知道你所在的具体位置，",
        "无法提供附近图书馆的坐标或名称。",
        "请告诉我你的城市或区域，我可以尝试帮你查找。"};
    const auto text=join(expected);
    for(int chunk:{1,2,5,17,200}) {
        TtsSegmenter s(c);std::vector<std::string> out;
        for(std::size_t i=0;i<text.size();i+=chunk) {
            auto part=s.push(text.substr(i,chunk));out.insert(out.end(),part.begin(),part.end());
        }
        auto tail=s.finish();out.insert(out.end(),tail.begin(),tail.end());
        require(out==expected,"first/later sentence policy split frozen text incorrectly");
    }
    TtsSegmenter live(c);
    require(live.push(expected[0])==std::vector<std::string>{expected[0]},"first clause was not released early");
    const std::string later="请告诉我你的城市或区域，";
    require(live.push(later).empty(),"later comma fragmented a sentence");
    require(live.poll(299).empty(),"later clause flushed before deadline");
    require(live.push("我可以尝试帮你查找。")==std::vector<std::string>{expected[2]},
            "later complete sentence not preserved");
    live.reset();
    require(live.push(later)==std::vector<std::string>{later},"reset did not restore first-clause policy");
    require(live.push(later).empty(),"second clause emitted early after reset");
    require(live.poll(300)==std::vector<std::string>{later},"timed existing clause did not release");
    const std::string unpunctuated="我不知道你所在的具体位置";
    require(live.push(unpunctuated).empty(),"sentence policy forced a short character cut");
    require(live.poll(5000).empty(),"timer forced a mid-word cut without boundary");
    require(live.finish()==std::vector<std::string>{unpunctuated},"EOS tail was lost");
    require(segment_tts_text("好。",c)==std::vector<std::string>{"好。"},"short EOS was not flushed");
    std::string long_text;
    for(int i=0;i<81;++i)long_text+="中";
    auto parts=segment_tts_text(long_text,c);
    require(parts.size()==3 && join(parts)==long_text,"sentence mode lost hard-bound fallback");
    const std::string clause="这里是一段超过最短长度的说明，";
    parts=segment_tts_text(clause+long_text,c);
    require(parts.front()==clause && join(parts)==clause+long_text,"hard limit ignored available clause boundary");
    const std::string mixed="请检查configuration是否正确，今天是2026年10月26日15时30分，温度-123.456度。";
    parts=segment_tts_text(mixed,c);
    require(join(parts)==mixed,"sentence policy changed mixed text");
    for(const std::string token:{"configuration","2026年10月26日15时30分","-123.456"})
        require(std::any_of(parts.begin(),parts.end(),[&](const auto& s){return s.find(token)!=std::string::npos;}),
                "sentence policy split protected token");
    rejects([&]{segment_tts_text(std::string(41,'a'),c);});
    c.target_chars=12;rejects([&]{TtsSegmenter invalid(c);});
    c.target_chars=0;c.boundary_lookahead_chars=2;rejects([&]{TtsSegmenter invalid(c);});
    // A later sentence may contain an early comma; adapter validation must not
    // reapply the first-clause policy to already selected model input.
    validate_tts_segment(expected[2],40);
    validate_tts_segment("好。",40);
    for(const std::string bad:{""," "," 边界","边界 ","<think>隐藏","**内容**"})
        rejects([&]{validate_tts_segment(bad,40);});
    rejects([&]{validate_tts_segment(std::string(41,'a'),40);});
    rejects([&]{validate_tts_segment(std::string("\xe4\xb8",2),40);});
    rejects([&]{validate_tts_segment("正常",0);});
}


void dynamic_segment_tests() {
    TtsConfig c;c.min_chars=10;c.target_chars=24;c.boundary_lookahead_chars=4;
    c.first_segment=TtsSegmentPolicy{8,15,2};
    const std::string first="你好这是开头啊，";
    const std::string later="这是后续的一段说明文字，";
    const auto repeated=[](int n) {std::string s;while(n--)s+="中";return s;};
    const auto whole=first+later+repeated(30);
    const std::vector<std::string> expected{first,later,repeated(24),repeated(6)};
    for(int bytes:{1,3,8,4096}) {
        TtsSegmenter segmenter(c);std::vector<std::string> got;
        for(std::size_t i=0;i<whole.size();i+=bytes) {
            auto out=segmenter.push(whole.substr(i,bytes));got.insert(got.end(),out.begin(),out.end());
        }
        auto tail=segmenter.finish();got.insert(got.end(),tail.begin(),tail.end());
        require(got==expected,"dynamic policy depends on input chunk boundaries or missed early comma");
    }
    TtsSegmenter s(c);
    require(s.push(first)==std::vector<std::string>{first},"first comma did not release at8");
    require(s.push("一二三四五六七八，").empty(),"later policy kept first minimum8");
    require(s.push("九十，")==std::vector<std::string>{"一二三四五六七八，九十，"},
            "later comma did not release after minimum10");
    s.reset();
    require(s.push(repeated(15)).empty(),"first target ignored lookahead2");
    require(s.push(repeated(2))==std::vector<std::string>{repeated(15)},
            "first target failed bounded punctuation-free fallback");
    require(s.push(repeated(22)).empty(),"later target ignored lookahead4");
    require(s.push(repeated(4))==std::vector<std::string>{repeated(24)},
            "later policy did not switch to24/4");
    require(s.finish()==std::vector<std::string>{repeated(4)},"dynamic EOS tail lost");
    s.reset();
    require(s.push(first)==std::vector<std::string>{first},"reset did not restore first8/15/2");
    s.reset();s.push(repeated(15));s.poll(0);
    require(s.poll(299).empty(),"dynamic first lookahead timer fired early");
    require(s.poll(300)==std::vector<std::string>{repeated(15)},
            "dynamic first timed fallback required sentence end");
    s.push(repeated(24));s.poll(0);
    require(s.poll(300)==std::vector<std::string>{repeated(24)},
            "dynamic later timed fallback required sentence end");
    require(segment_tts_text("你好。",c)==std::vector<std::string>{"你好。"},
            "dynamic short EOS failed");
    const auto mixed=first+"请检查configuration和2026年10月27日15时30分以及-123.456是否正确。";
    const auto parts=segment_tts_text(mixed,c);
    require(join(parts)==mixed,"dynamic segmentation changed mixed text");
    for(const std::string token:{"configuration","2026年10月27日15时30分","-123.456"})
        require(std::any_of(parts.begin(),parts.end(),[&](const auto& p){return p.find(token)!=std::string::npos;}),
                "dynamic policy split a protected token");
    rejects([&]{segment_tts_text(first+std::string(41,'a'),c);});
    auto bad=c;bad.first_segment->min_chars=16;
    rejects([&]{TtsSegmenter invalid(bad);});
    bad=c;bad.first_segment->target_chars=39;
    rejects([&]{TtsSegmenter invalid(bad);});
    bad=c;bad.first_segment->target_chars=0;
    rejects([&]{TtsSegmenter invalid(bad);});
    bad=c;bad.sentence_mode=true;bad.target_chars=0;bad.boundary_lookahead_chars=0;
    rejects([&]{TtsSegmenter invalid(bad);});
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

void joined_melo_tests() {
    const std::string original = "好的。没问题。现在开始。";
    TtsConfig c;c.min_chars=12;c.target_chars=12;c.join_short_sentences=true;
    auto parts=segment_tts_text(original,c);
    require(parts==std::vector<std::string>{original},"bounded short-sentence grouping changed");
    require(melo_frontend_text(parts[0],true)=="好的、没问题、现在开始。",
            "short sentences were not joined before acoustic inference");
    require(melo_frontend_text(original,false)==original,"disabled joining changed baseline");
    const std::string twice=original+"可以。请稍等。马上就好。";
    parts=segment_tts_text(twice,c);
    require(parts.size()==2 && join(parts)==twice,"joining enlarged or changed bounded segments");
    require(melo_frontend_text("你好。再见。",true)=="你好、再见。","short EOS joining failed");
    require(melo_frontend_text("你好！准备好了吗？可以；现在开始！",true)==
            "你好、准备好了吗、可以、现在开始！","internal punctuation or final exclamation changed");
    require(melo_frontend_text("你好。准备好了吗？",true)=="你好、准备好了吗？",
            "final question intonation marker lost");
    require(melo_frontend_text("你好。再见？！ \n",true)=="你好、再见？！ \n",
            "trailing punctuation/whitespace changed");
    require(melo_frontend_text("好的。他说：“再见。”",true)=="好的、他说：“再见。”",
            "final quoted sentence marker lost");
    require(melo_frontend_text("好。温度-23.5度，数量1,234。",true)==
            "好、温度-23.5度、数量1,234。","joining changed decimal or numeric grouping");
    require(melo_frontend_text("Hello! Yes? 12.5; 1,234.",true)=="Hello! Yes? 12.5; 1,234.",
            "joining rewrote ASCII punctuation");
    const auto normalized=melo_frontend_text(original,true);
    require(melo_frontend_text(normalized,true)==normalized,"joining is not idempotent");
    for (const std::string& bad : std::vector<std::string>{"正常。<think>秘密</think>", "**内容**",
                                  std::string("\xe4\xb8",2), std::string("a\0b",3)})
        rejects([&]{melo_frontend_text(bad,true);});
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
        dynamic_segment_tests();
        soft_target_tests();
        boundary_lookahead_tests(2);
        boundary_lookahead_tests(6);
        sentence_mode_tests();
        melo_frontend_tests();
        joined_melo_tests();
        pcm_tests();
        std::cout << "TTS_TEXT_PCM_OK\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
