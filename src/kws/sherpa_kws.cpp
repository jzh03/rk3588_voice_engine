#include "voice_engine/kws.hpp"
#include "voice_engine/probe_support.hpp"
#include <sherpa-onnx/c-api/c-api.h>
#include <nlohmann/json.hpp>
#include <cmath>
#include <fstream>
#include <sstream>
namespace voice_engine {
struct KeywordSpotter::Impl {
    const SherpaOnnxKeywordSpotter* model = nullptr;
    const SherpaOnnxOnlineStream* stream = nullptr;
    KeywordSpec spec;
    std::uint64_t samples = 0;
    explicit Impl(const Config& c) {
        probe::require_model_validation_lock();
        if (!c.models.kws.enabled) throw ConfigError("KWS model must be enabled");
        const auto manifest_path = std::filesystem::canonical(c.source_path.parent_path() / c.assets.models_manifest);
        std::ifstream manifest_input(manifest_path);
        const auto manifest = nlohmann::json::parse(manifest_input);
        nlohmann::json selected;
        for (const auto& m : manifest.at("models")) if (m.at("id") == c.models.kws.id) selected = m;
        if (selected.is_null()) throw ConfigError("KWS model absent from manifest");
        auto role = [&](const char* key) {
            const auto name = selected.at("roles").at(key).get<std::string>();
            bool declared = false;
            for (const auto& file : selected.at("files")) if (file.at("path") == name) declared = true;
            const auto root = manifest_path.parent_path();
            const auto path = std::filesystem::canonical(root / name);
            const auto relative = path.lexically_relative(root);
            if (!declared || relative.empty() || relative.is_absolute() || *relative.begin() == "..")
                throw ConfigError("KWS role outside validated files");
            return path.string();
        };
        const auto encoder = role("encoder"), decoder = role("decoder"), joiner = role("joiner"), tokens = role("tokens");
        std::ifstream token_input(tokens); std::string line;
        std::set<std::string> vocabulary;
        while (std::getline(token_input, line)) {
            std::istringstream fields(line); std::string token; int id;
            if (!(fields >> token >> id) || id < 0 || !vocabulary.insert(token).second)
                throw ConfigError("invalid KWS token vocabulary");
        }
        const auto keyword_path = c.source_path.parent_path() / c.dialogue.keywords_file;
        if (c.dialogue.keywords_file.empty() || std::filesystem::file_size(keyword_path) > 4096)
            throw ConfigError("keyword file missing or too large");
        std::ifstream keywords(keyword_path);
        if (!keywords) throw ConfigError("cannot read dialogue.keywords_file");
        spec = parse_keyword(keywords, c.dialogue.wake_word, vocabulary);
        SherpaOnnxKeywordSpotterConfig cfg{};
        cfg.feat_config.sample_rate = 16000; cfg.feat_config.feature_dim = 80;
        cfg.model_config.transducer.encoder = encoder.c_str();
        cfg.model_config.transducer.decoder = decoder.c_str();
        cfg.model_config.transducer.joiner = joiner.c_str();
        cfg.model_config.tokens = tokens.c_str(); cfg.model_config.provider = "cpu";
        cfg.model_config.num_threads = c.threads.kws;
        cfg.max_active_paths = 4; cfg.num_trailing_blanks = 1;
        cfg.keywords_score = spec.score; cfg.keywords_threshold = spec.threshold;
        cfg.keywords_buf = spec.encoded.c_str(); cfg.keywords_buf_size = static_cast<int>(spec.encoded.size());
        model = SherpaOnnxCreateKeywordSpotter(&cfg);
        if (!model) throw std::runtime_error("cannot create CPU KWS");
        try { reset(); }
        catch (...) { SherpaOnnxDestroyKeywordSpotter(model); model = nullptr; throw; }
    }
    ~Impl() {
        if (stream) SherpaOnnxDestroyOnlineStream(stream);
        if (model) SherpaOnnxDestroyKeywordSpotter(model);
    }
    void reset() {
        const auto next = SherpaOnnxCreateKeywordStream(model);
        if (!next) throw std::runtime_error("cannot create KWS stream");
        if (stream) SherpaOnnxDestroyOnlineStream(stream);
        stream = next; samples = 0;
    }
    std::optional<KeywordHit> accept(const float* pcm, std::size_t count) {
        if (!pcm || !count || count > 320) throw std::invalid_argument("KWS expects 1..320 samples");
        for (std::size_t n = 0; n < count; ++n)
            if (!std::isfinite(pcm[n]) || std::abs(pcm[n]) > 1.001f) throw std::invalid_argument("invalid KWS PCM");
        SherpaOnnxOnlineStreamAcceptWaveform(stream, 16000, pcm, static_cast<int>(count));
        samples += count;
        while (SherpaOnnxIsKeywordStreamReady(model, stream)) {
            SherpaOnnxDecodeKeywordStream(model, stream);
            const auto result = SherpaOnnxGetKeywordResult(model, stream);
            if (!result) throw std::runtime_error("missing KWS result");
            const bool hit = result->keyword && spec.word == result->keyword;
            const auto end = result->count && result->timestamps ?
                result->start_time + result->timestamps[result->count - 1] : 0.0;
            SherpaOnnxDestroyKeywordResult(result);
            if (hit) return KeywordHit{samples, end};
        }
        return {};
    }
};
KeywordSpotter::KeywordSpotter(const Config& c) : impl_(std::make_unique<Impl>(c)) {}
KeywordSpotter::~KeywordSpotter() = default;
void KeywordSpotter::reset() { impl_->reset(); }
std::optional<KeywordHit> KeywordSpotter::accept(const float* p, std::size_t n) { return impl_->accept(p,n); }
const KeywordSpec& KeywordSpotter::keyword() const { return impl_->spec; }
} // namespace voice_engine
