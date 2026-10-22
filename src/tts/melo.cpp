#include "onnxruntime_c_api.h"
#include "sherpa-onnx/c-api/c-api.h"
#include "voice_engine/assets.hpp"
#include "voice_engine/probe_support.hpp"
#include "voice_engine/tts.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <stdexcept>

namespace voice_engine {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
bool under(const fs::path &p, const fs::path &root) {
    auto a = p.begin(), b = root.begin();
    for (; b != root.end(); ++a, ++b)
        if (a == p.end() || *a != *b)
            return false;
    return true;
}
fs::path scoped(const fs::path &root, const std::string &path) {
    if (fs::path(path).is_absolute())
        throw ConfigError("TTS: absolute asset path rejected");
    auto result = fs::canonical(root / path);
    if (!under(result, root) || !fs::is_regular_file(result))
        throw ConfigError("TTS: asset outside manifest directory or not regular");
    return result;
}
Json read_json(const fs::path &p) {
    std::ifstream f(p);
    if (!f)
        throw ConfigError("TTS: cannot read " + p.string());
    return Json::parse(f);
}
Config tts_only(Config c) {
    if (!c.models.tts.enabled || c.models.tts.id.empty())
        throw ConfigError("TTS: models.tts must be enabled with an id");
    c.models.vad.enabled = c.models.asr.enabled = c.models.llm.enabled = c.models.kws.enabled =
        false;
    return c;
}
void check_runtime(const Config &c) {
    const auto deps = fs::canonical(c.source_path.parent_path() / c.assets.deps_manifest);
    const auto d = read_json(deps);
    std::map<std::string, std::set<fs::path>> expected;
    for (const auto &dep : d.at("dependencies"))
        if (dep.at("id") == "sherpa-onnx-cpu" || dep.at("id") == "onnxruntime-cpu") {
            for (const auto &f : dep.at("files")) {
                const auto n = fs::path(f.at("path").get<std::string>()).filename().string();
                if (n == "libonnxruntime.so" || n == "libsherpa-onnx-c-api.so" ||
                    n == "libsherpa-onnx-cxx-api.so")
                    expected[n].insert(scoped(deps.parent_path(), f.at("path")));
            }
        }
    std::ifstream maps("/proc/self/maps");
    std::string line;
    std::set<std::string> seen;
    while (std::getline(maps, line)) {
        const auto pos = line.find('/');
        if (pos == std::string::npos)
            continue;
        const fs::path path = line.substr(pos);
        const auto n = path.filename().string();
        if (n.find("libonnxruntime.so") != 0 && n.find("libsherpa-onnx-c-") != 0 &&
            n.find("libsherpa-onnx-cxx-") != 0)
            continue;
        if (!expected.count(n) || !expected.at(n).count(fs::canonical(path)))
            throw ConfigError("TTS: loaded runtime outside validated dependency files: " +
                              path.string());
        seen.insert(n);
    }
    for (const auto *n : {"libonnxruntime.so", "libsherpa-onnx-c-api.so"})
        if (!seen.count(n))
            throw ConfigError(std::string("TTS: missing runtime mapping: ") + n);
    if (std::string(SherpaOnnxGetVersionStr()) != "1.12.14" ||
        std::string(OrtGetApiBase()->GetVersionString()) != "1.17.1")
        throw ConfigError("TTS: runtime version differs from fixed sherpa 1.12.14 / ORT 1.17.1");
}
} // namespace
MeloTtsAssets resolve_melo_tts_assets(const Config &original) {
    const auto c = tts_only(original);
    validate_assets(c);
    const auto manifest = fs::canonical(c.source_path.parent_path() / c.assets.models_manifest);
    const auto data = read_json(manifest);
    const Json *model = nullptr;
    for (const auto &m : data.at("models"))
        if (m.at("id") == c.models.tts.id)
            model = &m;
    if (!model)
        throw ConfigError("TTS: model id not found");
    if (model->at("quantization") != "FP32" ||
        model->at("onnx_metadata").at("model_type") != "melo-vits")
        throw ConfigError("TTS: requires the validated FP32 Melo model");
    std::map<std::string, std::vector<fs::path>> roles;
    for (const auto &f : model->at("files"))
        if (f.contains("role"))
            roles[f.at("role")].push_back(scoped(manifest.parent_path(), f.at("path")));
    const auto one = [&](const std::string &role) {
        if (roles[role].size() != 1)
            throw ConfigError("TTS: exactly one asset required for role " + role);
        return roles[role][0];
    };
    MeloTtsAssets a;
    a.model_id = c.models.tts.id;
    a.model = one("active_fp32_weight");
    a.tokens = one("tts_tokens");
    a.lexicon = one("tts_lexicon");
    if (a.model != scoped(manifest.parent_path(), model->at("model_path")))
        throw ConfigError("TTS: model_path differs from active_fp32_weight");
    for (const auto *r :
         {"tts_rule_date", "tts_rule_number", "tts_rule_phone", "tts_rule_heteronym"})
        a.rule_fsts.push_back(one(r));
    const auto root = a.model.parent_path();
    a.dictionary = root / "dict";
    for (const auto &f : {a.tokens, a.lexicon})
        if (!under(f, root))
            throw ConfigError("TTS: language asset outside model directory");
    for (const auto &f : a.rule_fsts)
        if (!under(f, root))
            throw ConfigError("TTS: FST outside model directory");
    const auto dictionary = fs::canonical(a.dictionary);
    if (!under(dictionary, root))
        throw ConfigError("TTS: dictionary escapes model directory");
    std::set<fs::path> recorded(roles["tts_dictionary"].begin(), roles["tts_dictionary"].end());
    if (recorded.empty())
        throw ConfigError("TTS: no dictionary assets");
    for (const auto &f : recorded)
        if (!under(f, dictionary))
            throw ConfigError("TTS: dictionary role outside dict directory");
    for (const auto &e : fs::recursive_directory_iterator(dictionary))
        if (e.is_regular_file() && !recorded.count(fs::canonical(e.path())))
            throw ConfigError("TTS: unvalidated dictionary file: " + e.path().string());
    check_runtime(c);
    return a;
}
struct MeloTts::Impl {
    MeloTtsAssets assets;
    std::unique_ptr<const SherpaOnnxOfflineTts, decltype(&SherpaOnnxDestroyOfflineTts)> model{
        nullptr, SherpaOnnxDestroyOfflineTts};
    std::set<std::string> lexicon;
    double initialization_seconds = 0;
    TtsConfig text_config;
    std::size_t max_samples;
    Impl(const Config &c, int threads, bool debug_frontend,
         const fs::path &pronunciation_rules, const fs::path &derived_lexicon)
        : assets(resolve_melo_tts_assets(c)), text_config(c.tts),
          max_samples(static_cast<std::size_t>(44100) * c.queues.max_tts_audio_ms / 1000) {
        probe::require_model_validation_lock();
        if (threads != 2 && threads != 4)
            throw ConfigError("TTS: threads must be 2 or 4");
        assets.effective_lexicon = assets.lexicon;
        if (!pronunciation_rules.empty()) {
            if (derived_lexicon.empty())
                throw ConfigError("TTS: pronunciation supplement requires a new derived lexicon path");
            assets.pronunciation_rule_count = prepare_tts_pronunciation_lexicon(
                assets.lexicon, assets.tokens, pronunciation_rules, derived_lexicon);
            assets.effective_lexicon = fs::canonical(derived_lexicon);
            assets.pronunciation_rules = fs::canonical(pronunciation_rules);
        } else if (!derived_lexicon.empty()) {
            throw ConfigError("TTS: derived lexicon path without pronunciation rules");
        }
        std::ifstream words(assets.effective_lexicon);
        std::string line;
        while (std::getline(words, line)) {
            std::istringstream in(line);
            std::string word;
            in >> word;
            lexicon.insert(word);
        }
        SherpaOnnxOfflineTtsConfig t{};
        t.model.vits.model = assets.model.c_str();
        t.model.vits.tokens = assets.tokens.c_str();
        t.model.vits.lexicon = assets.effective_lexicon.c_str();
        t.model.vits.dict_dir = assets.dictionary.c_str();
        t.model.vits.noise_scale = 0.667F;
        t.model.vits.noise_scale_w = 0.8F;
        t.model.vits.length_scale = 1.0F;
        t.model.num_threads = threads;
        t.model.debug = debug_frontend ? 1 : 0;
        t.model.provider = "cpu";
        t.silence_scale = 1;
        // Batching separately AddBlank-padded sentences flips phone/blank
        // parity in every second sentence, conflicting with Melo language IDs.
        // melo_frontend_text preserves context across Chinese commas instead.
        t.max_num_sentences = 1;
        std::string rules;
        for (const auto &f : assets.rule_fsts) {
            if (!rules.empty())
                rules += ',';
            rules += f.string();
        }
        t.rule_fsts = rules.c_str();
        const auto start = std::chrono::steady_clock::now();
        model.reset(SherpaOnnxCreateOfflineTts(&t));
        initialization_seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (!model || SherpaOnnxOfflineTtsSampleRate(model.get()) != 44100)
            throw std::runtime_error(
                "TTS: model initialization failed or native sample rate is not 44100 Hz");
    }
};
MeloTts::MeloTts(const Config &c, int threads, bool debug_frontend,
                 const fs::path &pronunciation_rules, const fs::path &derived_lexicon) {
    probe::require_model_validation_lock();
    impl_ = std::make_unique<Impl>(c, threads, debug_frontend, pronunciation_rules, derived_lexicon);
}
MeloTts::~MeloTts() = default;
TtsNativeAudio MeloTts::generate(const std::string &text) {
    probe::require_model_validation_lock();
    const auto segments = segment_tts_text(text, impl_->text_config);
    if (segments.size() != 1 || segments[0] != text)
        throw std::invalid_argument(
            "TTS: generate requires one presegmented bounded plain-text segment");
    const auto frontend_text = melo_frontend_text(text);
    const auto generate_start = std::chrono::steady_clock::now();
    const std::unique_ptr<const SherpaOnnxGeneratedAudio,
                          decltype(&SherpaOnnxDestroyOfflineTtsGeneratedAudio)>
        a(SherpaOnnxOfflineTtsGenerate(impl_->model.get(), frontend_text.c_str(), 0, 1.0F),
          SherpaOnnxDestroyOfflineTtsGeneratedAudio);
    const double generate_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - generate_start).count();
    if (!a || a->sample_rate != 44100 || a->n <= 0 || !a->samples)
        throw std::runtime_error("TTS: empty or unexpected-rate audio");
    if (static_cast<std::size_t>(a->n) > impl_->max_samples)
        throw std::runtime_error("TTS: generated segment exceeds max_tts_audio_ms (checked after "
                                 "upstream whole-segment allocation)");
    for (int i = 0; i < a->n; ++i)
        if (!std::isfinite(a->samples[i]) || a->samples[i] < -1 || a->samples[i] > 1)
            throw std::runtime_error("TTS: invalid native float PCM");
    return {{a->samples, a->samples + a->n}, a->sample_rate, generate_seconds};
}
std::vector<std::string> MeloTts::unknown_english_words(const std::string &text) const {
    std::set<std::string> unknown;
    std::string word;
    const auto flush = [&] {
        if (!word.empty() && !impl_->lexicon.count(word))
            unknown.insert(word);
        word.clear();
    };
    for (unsigned char c : text) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
            word += static_cast<char>(std::tolower(c));
        else
            flush();
    }
    flush();
    return {unknown.begin(), unknown.end()};
}
const MeloTtsAssets &MeloTts::assets() const noexcept { return impl_->assets; }
double MeloTts::model_initialization_seconds() const noexcept {
    return impl_->initialization_seconds;
}
std::string MeloTts::runtime_version() {
    return std::string("sherpa=") + SherpaOnnxGetVersionStr() +
           " ort=" + OrtGetApiBase()->GetVersionString();
}
} // namespace voice_engine
