#include "voice_engine/asr.hpp"
#include "voice_engine/assets.hpp"
#include "voice_engine/config.hpp"
#include "voice_engine/llm.hpp"
#include "voice_engine/probe_support.hpp"
#include "voice_engine/tts_stream.hpp"
#include <nlohmann/json.hpp>
#include <sndfile.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

namespace {
namespace fs = std::filesystem;
using namespace voice_engine;
using J = nlohmann::json;
using Clock = std::chrono::steady_clock;
constexpr std::int64_t memory_floor_kib = 1572864;
volatile std::sig_atomic_t interrupted = 0;
void signal_handler(int value) { interrupted = value; }
std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now().time_since_epoch()).count();
}
struct Failure : std::runtime_error {
    int code;
    Failure(int value, const std::string& message) : std::runtime_error(message), code(value) {}
};
void check_signal() {
    if (interrupted) throw Failure(128 + interrupted, "interrupted by signal");
}
const char* help =
    "coexist_probe --config PATH --manifest FIXTURES_JSON --output-dir ROOT --run-id ID\n"
    "  [--sample ID ...] [--duration-sec 300] [--min-rounds 5] [--warmup-rounds 5] [--smoke]\n"
    "  [--swap-policy review|zero]\n"
    "  [--validate-input-only]\n"
    "One resident process: Silero VAD/Zipformer ASR, RKLLM, CPU Melo TTS.\n"
    "Finite 16 kHz mono PCM16 speech WAVs; fixed independent LLM questions; no audio devices.\n"
    "ASR/LLM NPU work is serial; streamed CPU TTS may overlap LLM. Not P3/A07.\n"
    "Formal mode requires duration >=300 seconds AND >=5 complete measured rounds.\n"
    "Smoke permits duration >=1 and min-rounds >=1, and always records A06=not_run.\n"
    "Five complete warmup rounds by default (1..20), cycling the same inputs/five prompts, are excluded.\n"
    "Active measured rounds continue until BOTH duration and min-rounds limits are met.\n"
    "validate-input-only checks config/manifest/WAV headers without hashes, locks or models.\n"
    "Run whole process under flock -x /tmp/rk3588_voice_engine_model_validation.lock\n"
    "and an external process-group timeout (include asset validation/init/warmup/shutdown).\n"
    "No builds or large hashes during measurement. Exit: 0 software checks pass,\n"
    "2 CLI/config/assets/lock, 4 model/runtime, 5 timeout, 6 resource/acceptance failure,\n"
    "128+signal interrupted. A06 additionally requires independent supervisor evidence.\n"
    "Swap policy review (default) records every sampled page delta for independent sustained-swap review;\n"
    "zero is the stricter diagnostic that fails on any measured swap-in/out increment.\n";

struct Options {
    std::string config, manifest, output = "output/runs", id, swap_policy = "review";
    std::set<std::string> samples;
    int duration = 300, min_rounds = 5, warmup_rounds = 5;
    bool smoke = false, validate = false;
};
int integer(const std::string& value, int low, int high, const std::string& key) {
    if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
        throw ConfigError("invalid integer: " + key);
    try {
        const auto n = std::stoll(value);
        if (n < low || n > high) throw ConfigError("out of range: " + key);
        return static_cast<int>(n);
    } catch (const ConfigError&) { throw; }
    catch (...) { throw ConfigError("invalid integer: " + key); }
}
Options parse(int argc, char** argv) {
    Options o;
    std::set<std::string> seen;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (key != "--sample" && !seen.insert(key).second)
            throw ConfigError("duplicate option: " + key);
        if (key == "--smoke") { o.smoke = true; continue; }
        if (key == "--validate-input-only") { o.validate = true; continue; }
        if (i + 1 >= argc) throw ConfigError("missing value: " + key);
        const std::string value = argv[++i];
        if (value.empty()) throw ConfigError("empty value: " + key);
        if (key == "--config") o.config = value;
        else if (key == "--manifest") o.manifest = value;
        else if (key == "--output-dir") o.output = value;
        else if (key == "--run-id") o.id = value;
        else if (key == "--sample") {
            if (!o.samples.insert(value).second) throw ConfigError("duplicate sample: " + value);
        } else if (key == "--duration-sec") o.duration = integer(value, 1, 3600, key);
        else if (key == "--min-rounds") o.min_rounds = integer(value, 1, 1000, key);
        else if (key == "--warmup-rounds") o.warmup_rounds = integer(value, 1, 20, key);
        else if (key == "--swap-policy") o.swap_policy = value;
        else throw ConfigError("unknown option: " + key);
    }
    if (o.config.empty() || o.manifest.empty() || (!o.validate && o.id.empty()))
        throw ConfigError("required --config, --manifest, and --run-id (except validation)");
    if (!o.id.empty() && !std::regex_match(o.id, std::regex("[A-Za-z0-9][A-Za-z0-9_.-]{0,63}")))
        throw ConfigError("invalid run-id");
    if (!o.smoke && (o.duration < 300 || o.min_rounds < 5))
        throw ConfigError("formal A06 requires duration >=300 and min-rounds >=5; use --smoke for shorter checks");
    if (o.swap_policy != "review" && o.swap_policy != "zero")
        throw ConfigError("swap-policy must be review or zero");
    return o;
}
const char* swap_policy_description(const std::string& policy) {
    return policy == "zero" ?
        "diagnostic: zero page delta after warmup; any change fails conservatively" :
        "original A06: record every sampled post-warmup delta; independent review must assess sustained swap";
}
J read_json(const fs::path& path) {
    try {
        if (!fs::is_regular_file(path) || fs::file_size(path) > 4 * 1024 * 1024)
            throw ConfigError("JSON must be a regular file <=4 MiB: " + path.string());
        std::ifstream input(path);
        if (!input) throw ConfigError("cannot open JSON: " + path.string());
        return J::parse(input);
    } catch (const ConfigError&) { throw; }
    catch (const std::exception& e) { throw ConfigError("JSON input: " + std::string(e.what())); }
}
fs::path confined(const fs::path& base, const std::string& name) {
    try {
        const fs::path p(name);
        if (p.empty() || p.is_absolute() || name.find('\0') != std::string::npos)
            throw ConfigError("asset path must be relative");
        for (const auto& part : p) if (part == "..") throw ConfigError("asset traversal");
        const auto root = fs::canonical(base), resolved = fs::canonical(base / p);
        auto a = root.begin(), b = resolved.begin();
        for (; a != root.end(); ++a, ++b)
            if (b == resolved.end() || *a != *b) throw ConfigError("asset symlink escape");
        if (!fs::is_regular_file(resolved)) throw ConfigError("asset must be regular");
        return resolved;
    } catch (const ConfigError&) { throw; }
    catch (const std::exception& e) { throw ConfigError("asset path: " + std::string(e.what())); }
}
J selected_model(const J& manifest, const std::string& id) {
    for (const auto& model : manifest.at("models")) if (model.at("id") == id) return model;
    throw ConfigError("selected model missing: " + id);
}
fs::path model_path(const fs::path& base, const J& model, const std::string& name) {
    for (const auto& file : model.at("files"))
        if (file.at("path") == name) return confined(base, name);
    throw ConfigError("model role is not declared in files: " + name);
}
void config_contract(const Config& c) {
    for (const auto& item : std::vector<std::pair<ModelRef, std::string>>{
        {c.models.vad, "silero-vad-v5.0"},
        {c.models.asr, "zipformer-bilingual-zh-en-rk3588-fp16"},
        {c.models.llm, "qwen3.5-2b-rk3588-w8a8"},
        {c.models.tts, "vits-melo-tts-zh-en-fp32"}})
        if (!item.first.enabled || item.first.id != item.second)
            throw ConfigError("coexist_probe requires enabled fixed model: " + item.second);
    if (c.models.kws.enabled || c.dialogue.wake_enabled || c.dialogue.barge_in)
        throw ConfigError("coexist_probe excludes KWS/wake/barge-in");
    if (c.audio.profile != "file-only") throw ConfigError("coexist_probe requires file-only profile");
    if (c.threads.tts != 4) throw ConfigError("coexist_probe requires the selected TTS 4-thread configuration");
}
struct Sample {
    std::string id, sha;
    fs::path path;
    sf_count_t frames = 0;
    std::vector<float> pcm;
};
std::vector<Sample> read_samples(const Options& o) {
    try {
        const auto manifest = read_json(o.manifest);
        if (!manifest.at("schema_version").is_number_integer() || manifest.at("schema_version") != 1 ||
            !manifest.at("samples").is_array() || manifest.at("samples").empty() ||
            manifest.at("samples").size() > 256)
            throw ConfigError("invalid fixture schema/samples");
        std::vector<Sample> samples;
        std::set<std::string> ids, found;
        for (const auto& item : manifest.at("samples")) {
            const auto id = item.at("sample_id").get<std::string>();
            if (id.empty() || !ids.insert(id).second) throw ConfigError("empty/duplicate fixture sample id");
            const bool requested = o.samples.count(id) != 0;
            if (!o.samples.empty() && !requested) continue;
            if (item.at("expected_behavior") != "speech") {
                if (requested) throw ConfigError("selected sample must have expected_behavior=speech");
                continue;
            }
            if (!item.at("sample_rate").is_number_integer() || item.at("sample_rate") != 16000 ||
                !item.at("channels").is_number_integer() || item.at("channels") != 1 ||
                !item.at("num_samples").is_number_integer())
                throw ConfigError("fixture must declare integer mono 16 kHz PCM sample count");
            Sample sample;
            sample.id = id;
            sample.sha = item.at("sha256").get<std::string>();
            if (!std::regex_match(sample.sha, std::regex("[0-9a-f]{64}")))
                throw ConfigError("invalid fixture sha256");
            sample.frames = item.at("num_samples").get<sf_count_t>();
            if (sample.frames < 1 || sample.frames > 15 * 16000)
                throw ConfigError("speech fixture length must be 1 sample..15 seconds");
            if (!item.at("duration_sec").is_number() ||
                !std::isfinite(item.at("duration_sec").get<double>()) ||
                std::abs(item.at("duration_sec").get<double>() - sample.frames / 16000.0) > 1e-6)
                throw ConfigError("fixture duration_sec must match PCM frame count");
            if (item.contains("sample_width_bytes") &&
                (!item.at("sample_width_bytes").is_number_integer() || item.at("sample_width_bytes") != 2))
                throw ConfigError("fixture sample_width_bytes must be 2");
            sample.path = confined(fs::path(o.manifest).parent_path().empty() ? fs::path(".") :
                                    fs::path(o.manifest).parent_path(), item.at("path").get<std::string>());
            SF_INFO info{};
            std::unique_ptr<SNDFILE, decltype(&sf_close)> wav(sf_open(sample.path.c_str(), SFM_READ, &info), sf_close);
            if (!wav || info.samplerate != 16000 || info.channels != 1 || info.frames != sample.frames ||
                (info.format & SF_FORMAT_SUBMASK) != SF_FORMAT_PCM_16 ||
                (info.format & SF_FORMAT_TYPEMASK) != SF_FORMAT_WAV)
                throw ConfigError("fixture must be exact PCM16 mono 16 kHz RIFF WAV: " + id);
            samples.push_back(std::move(sample));
            found.insert(id);
        }
        if (samples.empty()) throw ConfigError("no speech inputs selected");
        for (const auto& id : o.samples) if (!found.count(id)) throw ConfigError("sample not found: " + id);
        return samples;
    } catch (const ConfigError&) { throw; }
    catch (const std::exception& e) { throw ConfigError("invalid fixture: " + std::string(e.what())); }
}
void load_pcm(std::vector<Sample>& samples) {
    // All file hashes and input loads happen before initialization/warmup/measurement.
    for (auto& sample : samples) {
        if (file_sha256(sample.path) != sample.sha) throw ConfigError("fixture SHA-256 mismatch: " + sample.id);
        SF_INFO info{};
        std::unique_ptr<SNDFILE, decltype(&sf_close)> wav(sf_open(sample.path.c_str(), SFM_READ, &info), sf_close);
        if (!wav || info.frames != sample.frames || info.samplerate != 16000 || info.channels != 1 ||
            (info.format & SF_FORMAT_SUBMASK) != SF_FORMAT_PCM_16 ||
            (info.format & SF_FORMAT_TYPEMASK) != SF_FORMAT_WAV)
            throw ConfigError("fixture changed after header validation: " + sample.id);
        sample.pcm.resize(static_cast<std::size_t>(sample.frames));
        if (sf_read_float(wav.get(), sample.pcm.data(), sample.frames) != sample.frames)
            throw ConfigError("short fixture read: " + sample.id);
    }
}
void save(const fs::path& path, const J& value) {
    std::ofstream out(path);
    out << value.dump(2) << '\n';
    out.flush();
    if (!out) throw Failure(4, "report write failed: " + path.string());
}
class JsonLines {
    std::ofstream stream_;
    std::mutex mutex_;
public:
    explicit JsonLines(const fs::path& path) : stream_(path) {
        if (!stream_) throw Failure(4, "cannot create " + path.string());
    }
    void write(const J& value) {
        std::lock_guard<std::mutex> guard(mutex_);
        stream_ << value.dump() << '\n';
        stream_.flush();
        if (!stream_) throw Failure(4, "JSONL report write failed");
    }
};
std::int64_t proc_counter(const char* path, const std::string& key) {
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream row(line);
        std::string name;
        std::int64_t value = -1;
        if ((row >> name >> value) && name == key) return value;
    }
    return -1;
}
J mapped_libraries(const Config& c) {
    const auto deps_path = fs::canonical(c.source_path.parent_path() / c.assets.deps_manifest);
    const auto deps = read_json(deps_path);
    std::map<std::string, J> allowed;
    for (const auto& dep : deps.at("dependencies"))
        for (const auto& file : dep.at("files")) {
            const auto name = file.at("path").get<std::string>();
            if (name.find(".so") == std::string::npos) continue;
            const auto path = confined(deps_path.parent_path(), name);
            allowed[path.string()] = {{"dependency", dep.at("id")}, {"version", dep.at("version")},
                                     {"sha256", file.at("sha256")}};
        }
    std::ifstream maps("/proc/self/maps");
    std::set<std::string> paths, families;
    std::string line;
    while (std::getline(maps, line)) {
        const auto pos = line.find('/');
        if (pos == std::string::npos) continue;
        const auto path = line.substr(pos);
        for (const auto* family : {"librknnrt.so", "librkllmrt.so", "libsherpa-onnx", "libonnxruntime.so"})
            if (path.find(family) != std::string::npos) {
                paths.insert(fs::canonical(path).string()); families.insert(family);
            }
    }
    if (families.size() != 4) throw ConfigError("missing model runtime mapping");
    J result = J::array();
    for (const auto& path : paths) {
        const auto expected = allowed.find(path);
        if (expected == allowed.end()) throw ConfigError("loaded library outside frozen manifest: " + path);
        auto row = expected->second;
        if (file_sha256(path) != row.at("sha256")) throw ConfigError("loaded library SHA-256 mismatch: " + path);
        row["path"] = path;
        result.push_back(row);
    }
    return result;
}
struct SharedState {
    std::mutex mutex;
    std::string phase = "warmup", stage = "idle";
    std::int64_t measurement_begin_ns = 0;
    int round = 0;
    bool have_turn = false;
    LlmTurn turn;
    std::size_t asr_buffered_samples = 0;
};
J queue_snapshot(const TtsStreamStats& t, const LlmResult& l, std::size_t asr_buffered_samples) {
    return {{"llm_text_bytes", l.queue_current_bytes}, {"llm_packets", l.queue_current_packets},
            {"llm_text_peak_bytes", l.queue_peak_bytes},
            {"tts_input_bytes", t.current_input_bytes}, {"tts_input_peak_bytes", t.peak_input_bytes},
            {"tts_segments", t.current_segments}, {"tts_segments_peak", t.peak_segments},
            {"tts_pcm_frames", t.current_pcm_frames}, {"tts_pcm_frames_peak", t.peak_pcm_frames},
            {"tts_segmenter_pending_bytes", t.segmenter_pending_bytes},
            {"asr_pending_samples", asr_buffered_samples}, {"pending_turns", 0}};
}
struct ResourceSampler {
    JsonLines log;
    SharedState& state;
    LlmAdapter& llm;
    TtsStreamWorker& tts;
    const Config& config;
    const std::string swap_policy;
    std::mutex sample_mutex, wait_mutex;
    std::condition_variable cv;
    std::atomic<bool> stop{false};
    std::thread thread;
    std::string error;
    std::int64_t minimum_available = std::numeric_limits<std::int64_t>::max();
    std::int64_t initial_swap_in = -1, initial_swap_out = -1, final_swap_in = -1, final_swap_out = -1;
    std::int64_t first_sample_ns = 0, last_sample_ns = 0, maximum_gap_ns = 0;
    std::size_t measured_samples = 0;
    J peaks = {{"llm_text_bytes", 0}, {"tts_input_bytes", 0}, {"tts_segments", 0}, {"tts_pcm_frames", 0}};
    ResourceSampler(const fs::path& root, SharedState& s, LlmAdapter& l, TtsStreamWorker& t,
                    const Config& c, std::string policy)
        : log(root / "resources.jsonl"), state(s), llm(l), tts(t), config(c), swap_policy(std::move(policy)) {
        thread = std::thread([this] {
            auto due = Clock::now();
            while (!stop) {
                try { sample(); }
                catch (const std::exception& e) {
                    std::lock_guard<std::mutex> guard(sample_mutex);
                    if (error.empty()) error = e.what();
                }
                due += std::chrono::seconds(1);
                std::unique_lock<std::mutex> guard(wait_mutex);
                cv.wait_until(guard, due, [&] { return stop.load(); });
            }
        });
    }
    ~ResourceSampler() { finish(); }
    void finish() { stop = true; cv.notify_all(); if (thread.joinable()) thread.join(); }
    void sample() {
        std::lock_guard<std::mutex> guard(sample_mutex);
        J row;
        LlmResult l;
        LlmTurn observed_turn;
        bool have_turn = false;
        std::size_t asr_buffered_samples = 0;
        {
            // Copy the coordinator state, then release it before model snapshots.
            std::lock_guard<std::mutex> state_guard(state.mutex);
            observed_turn = state.turn; have_turn = state.have_turn;
            asr_buffered_samples = state.asr_buffered_samples;
            row = {{"monotonic_ns", now_ns()}, {"phase", state.phase}, {"stage", state.stage},
                   {"round", state.round}, {"measurement_begin_ns", state.measurement_begin_ns}};
        }
        if (have_turn) l = llm.snapshot(observed_turn);
        auto t = tts.stats();
        if (!have_turn) {
            // The previous producer is drained; its peaks belong to its old round,
            // particularly the excluded warmup, rather than the current ASR phase.
            t.peak_input_bytes = 0; t.peak_segments = 0; t.peak_pcm_frames = 0;
        }
        row["queues"] = queue_snapshot(t, l, asr_buffered_samples);
        row["llm_idle"] = llm.idle(); row["tts_idle"] = tts.wait_idle(0);
        if (l.queue_peak_bytes > static_cast<std::size_t>(config.queues.llm_text_bytes) ||
            t.peak_input_bytes > static_cast<std::size_t>(config.queues.llm_text_bytes) ||
            t.peak_segments > static_cast<std::size_t>(config.queues.tts_segments) ||
            t.peak_pcm_frames > static_cast<std::size_t>(config.queues.playback_ms / 20))
            error = "observed queue bound exceeded";
        const auto available = proc_counter("/proc/meminfo", "MemAvailable:");
        const auto swap_in = proc_counter("/proc/vmstat", "pswpin");
        const auto swap_out = proc_counter("/proc/vmstat", "pswpout");
        row["mem_available_kib"] = available;
        row["rss_kib"] = proc_counter("/proc/self/status", "VmRSS:");
        row["pswpin"] = swap_in; row["pswpout"] = swap_out;
        row["swap_policy"] = swap_policy;
        row["swap_in_pages_delta"] = nullptr; row["swap_out_pages_delta"] = nullptr;
        row["swap_in_pages_step"] = nullptr; row["swap_out_pages_step"] = nullptr;
        if (available < 0 || swap_in < 0 || swap_out < 0) error = "required /proc counters unavailable";
        if (row["phase"] == "measurement") {
            ++measured_samples;
            minimum_available = std::min(minimum_available, available);
            if (initial_swap_in < 0) { initial_swap_in = swap_in; initial_swap_out = swap_out; }
            row["swap_in_pages_delta"] = swap_in - initial_swap_in;
            row["swap_out_pages_delta"] = swap_out - initial_swap_out;
            row["swap_in_pages_step"] = final_swap_in < 0 ? 0 : swap_in - final_swap_in;
            row["swap_out_pages_step"] = final_swap_out < 0 ? 0 : swap_out - final_swap_out;
            if ((final_swap_in >= 0 && swap_in < final_swap_in) ||
                (final_swap_out >= 0 && swap_out < final_swap_out))
                error = "system swap counters moved backwards";
            final_swap_in = swap_in; final_swap_out = swap_out;
            const auto time = row["monotonic_ns"].get<std::int64_t>();
            if (!first_sample_ns) first_sample_ns = time;
            if (last_sample_ns) maximum_gap_ns = std::max(maximum_gap_ns, time - last_sample_ns);
            last_sample_ns = time;
            if (available < memory_floor_kib) error = "MemAvailable below 1.5 GiB during measurement";
            if (swap_policy == "zero" && (swap_in != initial_swap_in || swap_out != initial_swap_out))
                error = "nonzero post-warmup swap-in/out (conservative A06 gate)";
            const auto& q = row["queues"];
            for (const auto& pair : std::vector<std::pair<std::string,std::string>>{
                {"llm_text_bytes","llm_text_peak_bytes"}, {"tts_input_bytes","tts_input_peak_bytes"},
                {"tts_segments","tts_segments_peak"}, {"tts_pcm_frames","tts_pcm_frames_peak"}})
                peaks[pair.first] = std::max(peaks[pair.first].get<std::size_t>(), q[pair.second].get<std::size_t>());
        }
        log.write(row);
    }
    void check() {
        check_signal();
        std::lock_guard<std::mutex> guard(sample_mutex);
        if (!error.empty()) throw Failure(6, error);
    }
    J summary() {
        std::lock_guard<std::mutex> guard(sample_mutex);
        return {{"samples", measured_samples}, {"minimum_mem_available_kib", minimum_available},
                {"swap_in_pages_delta", final_swap_in - initial_swap_in},
                {"swap_out_pages_delta", final_swap_out - initial_swap_out},
                {"first_sample_ns", first_sample_ns}, {"last_sample_ns", last_sample_ns},
                {"maximum_sample_gap_ns", maximum_gap_ns}, {"queue_peaks", peaks}, {"error", error},
                {"initial_swap_in_pages", initial_swap_in}, {"initial_swap_out_pages", initial_swap_out},
                {"final_swap_in_pages", final_swap_in}, {"final_swap_out_pages", final_swap_out},
                {"swap_policy", swap_policy}, {"swap_policy_description", swap_policy_description(swap_policy)}};
    }
};
} // namespace

int main(int argc, char** argv) {
    fs::path root;
    J record;
    bool smoke = false;
    try {
        if (argc == 2 && std::string(argv[1]) == "--help") { std::cout << help; return 0; }
        const auto o = parse(argc, argv);
        smoke = o.smoke;
        const auto config = load_config(o.config);
        config_contract(config);
        auto inputs = read_samples(o);
        if (o.validate) {
            std::cout << "COEXIST_INPUT_OK\n" << J{{"formal_eligible", !o.smoke}, {"A06", "not_run"},
                {"selected_samples", inputs.size()}, {"duration_sec", o.duration},
                {"min_rounds", o.min_rounds}, {"warmup_rounds", o.warmup_rounds},
                {"swap_policy", o.swap_policy},
                {"mode", o.smoke ? "smoke" : "formal"}}.dump() << '\n';
            return 0;
        }
        probe::require_model_validation_lock();
        const auto requested = fs::absolute(fs::path(o.output) / o.id);
        if (fs::exists(requested) || !fs::create_directories(requested))
            throw ConfigError("run directory must be new");
        root = fs::canonical(requested);
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);
        JsonLines events(root / "events.jsonl"), rounds(root / "rounds.jsonl");
        auto event = [&](const std::string& name, J fields = J::object()) {
            fields["event"] = name;
            if (!fields.contains("monotonic_ns")) fields["monotonic_ns"] = now_ns();
            fields["pid"] = getpid();
            events.write(fields);
        };
        const std::vector<std::string> prompts = {
            "请用一句简短完整的话说明喝水有什么作用。",
            "请用一句简短完整的话说明为什么需要睡觉。",
            "请用一句简短完整的话说明植物为什么需要阳光。",
            "请用一句简短完整的话说明下雨时为什么要带伞。",
            "请用一句简短完整的话说明饭前为什么要洗手。"
        };
        record = {{"schema_version", 1}, {"run_id", o.id}, {"pid", getpid()},
            {"status", "initializing"}, {"A06", "not_run"}, {"mode", o.smoke ? "smoke" : "formal"},
            {"formal_eligible", !o.smoke}, {"duration_sec", o.duration}, {"min_rounds", o.min_rounds},
            {"scope", "P2-04 four-model single-process coexistence; no P3/A07 or real audio certification"},
            {"input_policy", "cycle finite speech WAVs in manifest order; fixed independent prompts; no ASR-to-LLM content claim"},
            {"prompt_policy", "cycle five fixed short questions independently of ASR hypothesis; reset session every round"},
            {"prompts", prompts}, {"warmup_rounds", o.warmup_rounds},
            {"completed_warmup_rounds", 0},
            {"warmup_policy", "complete four-model rounds cycle the same finite inputs and five fixed prompts; all excluded from measured gates"},
            {"warmup_prompt_coverage", 0},
            {"warmup_input_coverage", 0},
            {"asr_chunk_samples", 320},
            {"config_path", fs::canonical(o.config).string()}, {"config_sha256", file_sha256(o.config)},
            {"manifest_path", fs::canonical(o.manifest).string()}, {"manifest_sha256", file_sha256(o.manifest)},
            {"executable_path", fs::canonical("/proc/self/exe").string()},
            {"executable_sha256", file_sha256(fs::canonical("/proc/self/exe"))},
            {"lock_path", probe::kModelValidationLockPath}, {"lock_owner_verified", true},
            {"thread_limits", {{"vad", config.threads.vad}, {"asr", config.threads.asr},
                               {"llm", config.threads.llm}, {"tts", config.threads.tts}}},
            {"queue_capacities", {{"llm_text_bytes", config.queues.llm_text_bytes},
                {"tts_input_bytes", config.queues.llm_text_bytes}, {"tts_segments", config.queues.tts_segments},
                {"tts_pcm_frames", config.queues.playback_ms / 20}, {"tts_pcm_ms", config.queues.playback_ms},
                {"asr_pending_samples", 512}, {"pending_turns", config.queues.pending_turns},
                {"tts_native_segment_ms", config.queues.max_tts_audio_ms}}},
            {"queue_scope", "current snapshots exclude actively synthesizing segment/native temporary audio; capture/playback device queues absent"},
            {"mem_available_floor_kib", memory_floor_kib},
            {"swap_policy", o.swap_policy}, {"swap_policy_description", swap_policy_description(o.swap_policy)},
            {"npu_memory", "unavailable separately; RSS is not total NPU memory"},
            {"supervisor_required", true}};
        save(root / "run.json", record);
        event("validation_begin", {{"phase", "initialization"}});
        load_pcm(inputs);
        record["inputs"] = J::array();
        for (const auto& input : inputs)
            record["inputs"].push_back({{"sample_id", input.id}, {"path", input.path.string()},
                {"sha256", input.sha}, {"samples", input.frames}});
        // Each adapter validates its own selected assets. Avoid a second full LLM
        // hash pass; ASR/VAD are the only assets resolved directly by this probe.
        auto speech_config = config;
        speech_config.models.llm.enabled = speech_config.models.tts.enabled = speech_config.models.kws.enabled = false;
        const auto speech_assets = validate_assets(speech_config);
        const auto manifest_path = fs::canonical(config.source_path.parent_path() / config.assets.models_manifest);
        const auto manifest = read_json(manifest_path);
        const auto asr_model = selected_model(manifest, config.models.asr.id);
        const auto vad_model = selected_model(manifest, config.models.vad.id);
        const auto role = [&](const std::string& key) {
            return model_path(manifest_path.parent_path(), asr_model, asr_model.at("roles").at(key).get<std::string>());
        };
        const ZipformerFiles files{role("encoder"), role("decoder"), role("joiner"), role("vocabulary"), role("feature_config")};
        const auto vad_file = model_path(manifest_path.parent_path(), vad_model, vad_model.at("model_path").get<std::string>());
        const auto llm_options = llm_options_from_config(config);
        const auto llm_assets = resolve_rkllm_assets(config);
        record["speech_validated_assets"] = {{"models", speech_assets.models},
            {"dependencies", speech_assets.dependencies}, {"files", speech_assets.files}};
        event("validation_end", {{"phase", "initialization"}});
        check_signal();
        const auto initialization_begin = now_ns();
        auto asr = std::make_unique<ZipformerAsr>(files);
        event("model_ready", {{"phase", "initialization"}, {"model", "asr"}});
        auto vad = std::make_unique<SileroVad>(vad_file, config.vad, config.threads.vad);
        event("model_ready", {{"phase", "initialization"}, {"model", "vad"}});
        auto model = std::make_unique<MeloTts>(config, config.threads.tts);
        event("model_ready", {{"phase", "initialization"}, {"model", "tts"}});
        auto llm = std::make_unique<LlmAdapter>(make_rkllm_backend(llm_assets, llm_options), llm_options);
        event("model_ready", {{"phase", "initialization"}, {"model", "llm"}});
        auto listening = std::make_unique<ListeningAsr>(*asr, *vad, config.vad);
        record["initialization_ms"] = (now_ns() - initialization_begin) / 1e6;
        record["llm_initialization_ms"] = llm->initialization_ms();
        record["tts_initialization_s"] = model->model_initialization_seconds();
        record["runtime"] = {{"asr", J::parse(asr->runtime_info_json())},
            {"vad", J::parse(vad->runtime_info_json())}, {"tts", MeloTts::runtime_version()},
            {"llm", {{"sdk", "1.3.0"}, {"version_source", "validated dependency manifest/hash and SDK stdout"},
                     {"model_id", llm_assets.model_id}}}};
        record["libraries"] = mapped_libraries(config);
        record["all_models_resident_ns"] = now_ns();
        record["resident_models"] = J::array({"vad", "asr", "llm", "tts"});
        record["status"] = "warmup";
        save(root / "run.json", record);
        save(root / "libraries.json", record["libraries"]);
        event("all_models_resident", {{"phase", "warmup"}, {"models", record["resident_models"]},
            {"monotonic_ns", record["all_models_resident_ns"]}, {"libraries", record["libraries"]}});
        SharedState state;
        std::mutex generated_mutex;
        std::vector<J> generated;
        auto worker = std::make_unique<TtsStreamWorker>([&](const std::string& text) {
            int round;
            std::string phase;
            {
                std::lock_guard<std::mutex> guard(state.mutex);
                round = state.round; phase = state.phase;
            }
            const auto begin = now_ns();
            event("tts_generate_begin", {{"phase", phase}, {"round", round}, {"monotonic_ns", begin}});
            auto audio = model->generate(text);
            const auto end = now_ns();
            event("tts_generate_end", {{"phase", phase}, {"round", round}, {"monotonic_ns", end},
                {"native_samples", audio.samples.size()}, {"generate_s", audio.generate_seconds}});
            std::lock_guard<std::mutex> guard(generated_mutex);
            generated.push_back({{"text", text}, {"frontend_text", melo_frontend_text(text)},
                {"begin_ns", begin}, {"end_ns", end}, {"generate_s", audio.generate_seconds},
                {"native_samples", audio.samples.size()}});
            return audio;
        }, config.tts, config.queues, config.timeouts.tts_segment_ms);
        ResourceSampler sampler(root, state, *llm, *worker, config, o.swap_policy);
        auto round = [&](int number, bool warmup, const Sample& input, const std::string& prompt) {
            sampler.check();
            if (!llm->idle() || !worker->wait_idle(0) || llm->restart_required())
                throw Failure(4, "models were not idle at round boundary");
            const auto old_queues = worker->stats();
            if (old_queues.current_input_bytes || old_queues.current_segments || old_queues.current_pcm_frames)
                throw Failure(6, "TTS queues not empty at round boundary");
            llm->new_session();
            if (llm->history_pairs()) throw Failure(6, "history not reset");
            {
                std::lock_guard<std::mutex> guard(state.mutex);
                state.round = number; state.have_turn = false; state.stage = "asr";
                state.asr_buffered_samples = 0;
            }
            {
                std::lock_guard<std::mutex> guard(generated_mutex);
                generated.clear();
            }
            const auto phase = warmup ? "warmup" : "measurement";
            J r{{"round", number}, {"warmup", warmup}, {"phase", phase}, {"sample_id", input.id},
                {"prompt", prompt}, {"status", "running"}, {"begin_ns", now_ns()}};
            auto round_event = [&](const std::string& name, std::int64_t timestamp) {
                event(name, {{"phase", phase}, {"round", number}, {"sample_id", input.id},
                    {"monotonic_ns", timestamp}});
            };
            round_event("round_begin", r["begin_ns"].get<std::int64_t>());
            r["asr_begin_ns"] = now_ns();
            round_event("asr_begin", r["asr_begin_ns"].get<std::int64_t>());
            listening->begin();
            std::size_t consumed = 0;
            while (consumed < input.pcm.size() && listening->state() == ListeningState::listening) {
                sampler.check();
                const auto count = std::min<std::size_t>(320, input.pcm.size() - consumed);
                const auto accepted = listening->accept(input.pcm.data() + consumed, count);
                consumed += accepted;
                {
                    std::lock_guard<std::mutex> guard(state.mutex);
                    state.asr_buffered_samples = listening->buffered_samples();
                }
                if (accepted == 0 && listening->state() == ListeningState::listening)
                    throw Failure(4, "LISTENING made no progress");
            }
            const auto result = listening->finish();
            r["asr_end_ns"] = now_ns();
            round_event("asr_end", r["asr_end_ns"].get<std::int64_t>());
            if (!result.backends_reset || !result.asr_finish_called || result.hypothesis.empty() ||
                !result.vad.speech_detected || result.vad.windows == 0 || result.asr.encoder_chunks == 0)
                throw Failure(4, "ASR/VAD round did not complete speech inference/reset");
            if (!asr->partial().empty() || asr->stats().input_samples || vad->stats().input_samples)
                throw Failure(6, "ASR/VAD state leaked after reset");
            if (!llm->idle()) throw Failure(6, "ASR ended while LLM was active");
            r["asr"] = {{"hypothesis", result.hypothesis}, {"reason", listening_end_name(result.reason)},
                {"input_samples", result.input_samples}, {"file_samples", input.pcm.size()},
                {"consumed_fresh_samples", consumed}, {"endpoint_sample", result.endpoint_sample},
                {"encoder_chunks", result.asr.encoder_chunks}, {"vad_windows", result.vad.windows},
                {"speech_detected", result.vad.speech_detected},
                {"peak_feature_frames", result.asr.peak_feature_frames},
                {"backends_reset", result.backends_reset}, {"asr_finish_called", result.asr_finish_called}};
            r["llm_begin_ns"] = now_ns();
            round_event("llm_begin", r["llm_begin_ns"].get<std::int64_t>());
            const auto turn = llm->start(prompt);
            const TtsStreamId id{turn.session_id, turn.generation_id};
            worker->begin(id);
            {
                std::lock_guard<std::mutex> guard(state.mutex);
                state.turn = turn; state.have_turn = true; state.stage = "llm_tts"; state.asr_buffered_samples = 0;
            }
            bool input_end = false;
            std::string text;
            std::uint64_t pcm_samples = 0, pcm_frames = 0;
            double pcm_energy = 0, pcm_abs_peak = 0;
            std::int64_t first_pcm_ns = 0;
            LlmResult llm_result;
            const auto consume = [&](const TtsStreamFrame& frame) {
                if (frame.audio.valid_samples > frame.audio.samples.size() ||
                    pcm_samples + frame.audio.valid_samples > 120 * 16000)
                    throw Failure(4, "invalid/oversized PCM output");
                double energy = 0, peak = 0;
                for (std::size_t i = 0; i < frame.audio.valid_samples; ++i) {
                    const auto value = frame.audio.samples[i];
                    if (!std::isfinite(value)) throw Failure(4, "non-finite PCM output");
                    energy += value * value; peak = std::max(peak, std::abs(static_cast<double>(value)));
                }
                if (!worker->commit_if_current(id, [&] {
                    // Bounded discard sink, not a playback device or WAV accumulator.
                    if (!first_pcm_ns) first_pcm_ns = now_ns();
                    pcm_samples += frame.audio.valid_samples; ++pcm_frames;
                    pcm_energy += energy; pcm_abs_peak = std::max(pcm_abs_peak, peak);
                })) throw Failure(4, "stale PCM commit");
            };
            while (true) {
                sampler.check();
                if (now_ns() - r["begin_ns"].get<std::int64_t>() > 120000000000LL)
                    throw Failure(5, "round exceeded 120-second deadline");
                if (llm->restart_required()) {
                    worker->cancel(id);
                    throw Failure(4, "LLM idle not confirmed; supervised process restart required");
                }
                if (!input_end) {
                    auto packet = llm->poll(turn);
                    const bool observed_idle = llm->idle();
                    if (!packet && observed_idle) packet = llm->poll(turn);
                    if (packet) {
                        if (text.size() + packet->text.size() > 8192)
                            throw Failure(6, "round output text bound exceeded");
                        if (!llm->commit_if_current(turn, [&] {
                            if (!packet->text.empty() && !worker->submit(id, packet->text))
                                throw Failure(4, "TTS rejected current LLM output");
                        })) throw Failure(4, "LLM generation invalidated");
                        text += packet->text;
                    } else if (observed_idle) {
                        const auto completed = llm->wait(turn, 1);
                        if (!completed) throw Failure(4, "LLM idle without completed result");
                        llm_result = *completed;
                        if (llm_result.status != "completed" || llm_result.run_rc != 0 ||
                            llm_result.clear_rc != 0 || llm_result.overflow || llm_result.invalid_utf8 ||
                            llm_result.restart_required || text.empty() || text != llm_result.text)
                            throw Failure(4, "LLM completion/stream/KV reset failed: " + llm_result.status);
                        if (!llm->idle()) throw Failure(6, "LLM completion was not SDK idle");
                        r["llm_end_ns"] = now_ns();
                        round_event("llm_end", r["llm_end_ns"].get<std::int64_t>());
                        if (!worker->finish(id)) throw Failure(4, "TTS finish rejected");
                        input_end = true;
                        std::lock_guard<std::mutex> guard(state.mutex);
                        state.stage = "tts_drain";
                    }
                }
                while (auto frame = worker->poll(id)) consume(*frame);
                if (worker->wait_idle(0)) {
                    while (auto frame = worker->poll(id)) consume(*frame);
                    const auto status = worker->stats();
                    if (status.status == "failed") throw Failure(4, status.error);
                    if (input_end && llm->idle()) break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            sampler.check();
            const auto t = worker->stats();
            const auto l = llm->snapshot(turn);
            if (t.status != "completed" || !pcm_samples || pcm_energy <= 1e-12 || t.segments == 0 ||
                t.current_input_bytes || t.current_segments || t.current_pcm_frames || t.segmenter_pending_bytes ||
                l.queue_current_bytes || l.queue_current_packets || t.frames != t.consumed_frames ||
                t.discarded_generation_results || t.rejected_stale_inputs || llm->history_pairs())
                throw Failure(6, "round completion/empty queue/reset checks failed");
            if (l.queue_peak_bytes > static_cast<std::size_t>(config.queues.llm_text_bytes) ||
                t.peak_input_bytes > static_cast<std::size_t>(config.queues.llm_text_bytes) ||
                t.peak_segments > static_cast<std::size_t>(config.queues.tts_segments) ||
                t.peak_pcm_frames > static_cast<std::size_t>(config.queues.playback_ms / 20))
                throw Failure(6, "round queue peak exceeded capacity");
            r["llm"] = {{"status", llm_result.status}, {"text", text}, {"run_rc", llm_result.run_rc},
                {"clear_rc", llm_result.clear_rc}, {"idle_ns", llm_result.idle_ns},
                {"submit_ns", llm_result.submit_ns}, {"first_token_ns", llm_result.first_token_ns},
                {"first_text_ns", llm_result.first_text_ns}, {"finish_ns", llm_result.finish_ns},
                {"run_return_ns", llm_result.run_return_ns}, {"tokens", llm_result.token_callbacks},
                {"queue_peak_bytes", llm_result.queue_peak_bytes}, {"queue_current_bytes", l.queue_current_bytes},
                {"queue_current_packets", l.queue_current_packets}, {"history_pairs", llm->history_pairs()}};
            r["tts"] = {{"status", t.status}, {"segments", t.segments}, {"frames", t.frames},
                {"consumed_frames", t.consumed_frames}, {"pcm_samples", pcm_samples},
                {"pcm_energy", pcm_energy}, {"pcm_abs_peak", pcm_abs_peak},
                {"first_pcm_ns", first_pcm_ns}, {"first_segment_ns", t.first_segment_ns},
                {"producer_done_ns", t.producer_done_ns}, {"generate_s", t.generate_seconds},
                {"peak_input_bytes", t.peak_input_bytes}, {"peak_segments", t.peak_segments},
                {"peak_pcm_frames", t.peak_pcm_frames}, {"current_input_bytes", t.current_input_bytes},
                {"current_segments", t.current_segments}, {"current_pcm_frames", t.current_pcm_frames},
                {"segmenter_pending_bytes", t.segmenter_pending_bytes}, {"sink", "bounded frame discard, no audio playback"}};
            std::int64_t overlap_ns = 0;
            {
                std::lock_guard<std::mutex> guard(generated_mutex);
                r["tts_generations"] = generated;
                for (const auto& part : generated) {
                    const auto a = std::max(part.at("begin_ns").get<std::int64_t>(), llm_result.submit_ns);
                    const auto b = std::min(part.at("end_ns").get<std::int64_t>(), llm_result.run_return_ns);
                    overlap_ns += std::max<std::int64_t>(0, b - a);
                }
            }
            r["tts_llm_overlap_ns"] = overlap_ns;
            r["npu_serial"] = r["asr_end_ns"].get<std::int64_t>() <= r["llm_begin_ns"].get<std::int64_t>();
            r["reset_verified"] = true;
            r["end_ns"] = now_ns();
            r["status"] = "completed";
            rounds.write(r);
            round_event("round_complete", r["end_ns"].get<std::int64_t>());
            // Snapshot every round in addition to one-second samples so all peak
            // counters survive a fast round ending between periodic samples.
            sampler.sample();
            return r;
        };
        const auto warmup_begin = now_ns();
        record["warmup_begin_ns"] = warmup_begin;
        record["warmup_round_results"] = J::array();
        event("warmup_begin", {{"phase", "warmup"}, {"monotonic_ns", warmup_begin},
            {"requested_rounds", o.warmup_rounds}});
        // Exercise all five fixed prompt lengths before measurement by default.
        // These are ordinary complete rounds, never idle padding. Initial
        // allocation/reclaim belongs to preparation; the selected measured
        // swap policy is reported independently from the unchanged hard gates.
        for (int number = 0; number < o.warmup_rounds; ++number) {
            const auto index = static_cast<std::size_t>(number);
            record["warmup_round_results"].push_back(
                round(number, true, inputs[index % inputs.size()], prompts[index % prompts.size()]));
            record["completed_warmup_rounds"] = number + 1;
            record["warmup_prompt_coverage"] = std::min<std::size_t>(number + 1, prompts.size());
            record["warmup_input_coverage"] = std::min<std::size_t>(number + 1, inputs.size());
        }
        const auto warmup_end = now_ns();
        event("warmup_end", {{"phase", "warmup"}, {"monotonic_ns", warmup_end},
            {"completed_rounds", o.warmup_rounds}});
        record["warmup_end_ns"] = warmup_end;
        {
            std::lock_guard<std::mutex> guard(state.mutex);
            state.have_turn = false; state.stage = "round_boundary";
        }
        const auto measurement_begin = now_ns();
        {
            std::lock_guard<std::mutex> guard(state.mutex);
            state.phase = "measurement"; state.measurement_begin_ns = measurement_begin;
        }
        sampler.sample();
        sampler.check();
        record["measurement_begin_ns"] = measurement_begin;
        record["status"] = "measuring";
        save(root / "run.json", record);
        event("measurement_begin", {{"phase", "measurement"}, {"monotonic_ns", measurement_begin},
            {"required_duration_sec", o.duration}, {"required_rounds", o.min_rounds}});
        int completed_rounds = 0;
        std::int64_t total_overlap_ns = 0;
        // Never stop active work after five rounds and pad the window with idle.
        while (completed_rounds < o.min_rounds ||
               now_ns() - measurement_begin < static_cast<std::int64_t>(o.duration) * 1000000000LL) {
            const auto index = static_cast<std::size_t>(completed_rounds);
            const auto r = round(completed_rounds + 1, false, inputs[index % inputs.size()], prompts[index % prompts.size()]);
            total_overlap_ns += r["tts_llm_overlap_ns"].get<std::int64_t>();
            ++completed_rounds;
        }
        sampler.sample();
        sampler.check();
        const auto measurement_end = now_ns();
        {
            std::lock_guard<std::mutex> guard(state.mutex);
            state.phase = "shutdown"; state.stage = "idle";
        }
        event("measurement_end", {{"phase", "measurement"}, {"monotonic_ns", measurement_end},
            {"completed_rounds", completed_rounds}});
        sampler.finish();
        // A periodic sample may have completed after the last foreground check.
        // Joining closes that race; accept no resources before its error is checked.
        sampler.check();
        const auto resources = sampler.summary();
        const double elapsed = (measurement_end - measurement_begin) / 1e9;
        if (elapsed < o.duration || completed_rounds < o.min_rounds ||
            resources["samples"].get<std::size_t>() < static_cast<std::size_t>(o.duration) ||
            resources["maximum_sample_gap_ns"].get<std::int64_t>() > 2500000000LL)
            throw Failure(6, "measurement duration/round count/sample coverage failed");
        record["measurement_end_ns"] = measurement_end;
        record["measured_duration_sec"] = elapsed;
        record["completed_rounds"] = completed_rounds;
        record["resources"] = resources;
        record["tts_llm_overlap_ns"] = total_overlap_ns;
        record["npu_serial_verified"] = true;
        record["queue_bounds_verified"] = true;
        record["round_resets_verified"] = true;
        record["status"] = "shutting_down";
        // Read paths only after measurement. No second large hash pass can
        // perturb the measured resource window.
        std::set<std::string> initial_paths, final_paths;
        for (const auto& library : record["libraries"])
            initial_paths.insert(library.at("path").get<std::string>());
        std::ifstream final_maps("/proc/self/maps");
        std::string mapping;
        while (std::getline(final_maps, mapping)) {
            const auto pos = mapping.find('/');
            if (pos == std::string::npos) continue;
            const auto path = mapping.substr(pos);
            for (const auto* family : {"librknnrt.so", "librkllmrt.so", "libsherpa-onnx", "libonnxruntime.so"})
                if (path.find(family) != std::string::npos)
                    final_paths.insert(fs::canonical(path).string());
        }
        J final_libraries{{"monotonic_ns", now_ns()}, {"phase", "shutdown"},
            {"paths", final_paths}, {"matches_initial", initial_paths == final_paths},
            {"method", "path-only /proc/self/maps after measurement; initial hashes already verified"}};
        save(root / "libraries-final.json", final_libraries);
        record["final_libraries"] = final_libraries;
        if (initial_paths != final_paths) throw Failure(6, "loaded model library paths changed during run");
        save(root / "run.json", record);
        event("shutdown_begin", {{"phase", "shutdown"}});
        if (!llm->idle() || !worker->wait_idle(0) || !llm->close())
            throw Failure(4, "clean model shutdown failed");
        worker.reset();
        listening.reset();
        llm.reset();
        vad.reset();
        asr.reset();
        model.reset();
        const auto shutdown_end = now_ns();
        event("shutdown_complete", {{"phase", "shutdown"}, {"monotonic_ns", shutdown_end},
            {"destroyed_models", J::array({"vad", "asr", "llm", "tts"})}});
        record["shutdown_complete_ns"] = shutdown_end;
        record["clean_shutdown"] = true;
        record["status"] = "completed";
        // The process reports software gates only. The supervisor independently
        // reviews OS samples and clean child termination before A06 is accepted.
        record["software_checks"] = "passed";
        record["A06"] = o.smoke ? "not_run" : "pending_external_review";
        save(root / "summary.json", record);
        std::cout << "COEXIST_PROBE_OK " << J{{"run_id", o.id}, {"completed_rounds", completed_rounds},
            {"measured_duration_sec", elapsed}, {"A06", record["A06"]},
            {"clean_shutdown", true}}.dump() << '\n';
        return 0;
    } catch (const std::exception& e) {
        const auto* failure = dynamic_cast<const Failure*>(&e);
        const int code = failure ? failure->code : dynamic_cast<const ConfigError*>(&e) ? 2 : 4;
        std::cerr << "COEXIST_PROBE_FAILED " << e.what() << '\n';
        if (!root.empty()) {
            try {
                record["status"] = "failed"; record["error"] = e.what(); record["exit_code"] = code;
                record["A06"] = smoke ? "not_run" : "failed"; record["clean_shutdown"] = false;
                record["failure_ns"] = now_ns();
                save(root / "failure.json", record);
                save(root / "summary.json", record);
            } catch (...) {}
        }
        return code;
    }
}
