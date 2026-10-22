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
#include <iomanip>
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
    "voice_file_demo --config PATH --manifest PATH --output-dir ROOT --run-id ID\n"
    "  [--sample ID ...] [--warmup-rounds 1..20] [--tts-mode stream|batch]\n"
    "  [--validate-input-only]\n"
    "Resident WAV -> Silero/Zipformer -> actual hypothesis -> RKLLM -> Melo -> WAV.\n"
    "One warmup round by default, excluded from measurement; independent session per sample.\n"
    "No devices. Empty recognition never starts LLM/TTS. Content review is external.\n"
    "Run under flock -x /tmp/rk3588_voice_engine_model_validation.lock and process-group timeout.\n"
    "No concurrent builds or large hashes during sampling. Exit: 0 software checks pass,\n"
    "2 CLI/config/assets/lock, 4 model/I/O failure, 5 deadline, 6 resource/flow failure, 128+signal.\n";
struct Options {
    std::string config, manifest, output = "output/runs", id, tts_mode = "stream";
    std::set<std::string> samples;
    int warmup_rounds = 1;
    bool validate = false;
};
Options parse(int argc, char** argv) {
    Options o;
    std::set<std::string> seen;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (key != "--sample" && !seen.insert(key).second) throw ConfigError("duplicate option: " + key);
        if (key == "--validate-input-only") { o.validate = true; continue; }
        if (i + 1 >= argc) throw ConfigError("missing value: " + key);
        const std::string value = argv[++i];
        if (value.empty()) throw ConfigError("empty value: " + key);
        if (key == "--config") o.config = value;
        else if (key == "--manifest") o.manifest = value;
        else if (key == "--output-dir") o.output = value;
        else if (key == "--run-id") o.id = value;
        else if (key == "--tts-mode") o.tts_mode = value;
        else if (key == "--sample") {
            if (!o.samples.insert(value).second) throw ConfigError("duplicate sample: " + value);
        } else if (key == "--warmup-rounds") {
            if (value.find_first_not_of("0123456789") != std::string::npos || value.size() > 2)
                throw ConfigError("invalid warmup-rounds");
            o.warmup_rounds = std::stoi(value);
            if (o.warmup_rounds < 1 || o.warmup_rounds > 20) throw ConfigError("warmup-rounds must be 1..20");
        } else throw ConfigError("unknown option: " + key);
    }
    if (o.config.empty() || o.manifest.empty() || (!o.validate && o.id.empty()))
        throw ConfigError("required --config, --manifest, and --run-id (except validation)");
    if (!o.id.empty() && !std::regex_match(o.id, std::regex("[A-Za-z0-9][A-Za-z0-9_.-]{0,63}")))
        throw ConfigError("invalid run-id");
    if (o.tts_mode != "stream" && o.tts_mode != "batch") throw ConfigError("tts-mode must be stream or batch");
    return o;
}
const char* swap_policy_description(const std::string&) {
    return "observational counters; sustained swap requires independent review";
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
            throw ConfigError("voice_file_demo requires enabled fixed model: " + item.second);
    if (c.models.kws.enabled || c.dialogue.wake_enabled || c.dialogue.barge_in)
        throw ConfigError("voice_file_demo excludes KWS/wake/barge-in");
    if (c.audio.profile != "file-only") throw ConfigError("voice_file_demo requires file-only profile");
    if (c.threads.tts != 4) throw ConfigError("voice_file_demo requires the selected TTS 4-thread configuration");
}
struct Sample {
    std::string id, sha, expected;
    J metadata;
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
            if (!std::regex_match(id, std::regex("[A-Za-z0-9][A-Za-z0-9_.-]{0,63}")) || !ids.insert(id).second) throw ConfigError("empty/duplicate fixture sample id");
            const bool requested = o.samples.count(id) != 0;
            if (!o.samples.empty() && !requested) continue;
            const auto expected = item.at("expected_behavior").get<std::string>();
            if (expected != "speech" && expected != "silence" && expected != "empty" && expected != "no_speech")
                throw ConfigError("unsupported expected_behavior: " + expected);
            if (!item.at("sample_rate").is_number_integer() || item.at("sample_rate") != 16000 ||
                !item.at("channels").is_number_integer() || item.at("channels") != 1 ||
                !item.at("num_samples").is_number_integer())
                throw ConfigError("fixture must declare integer mono 16 kHz PCM sample count");
            Sample sample;
            sample.id = id; sample.expected = expected; sample.metadata = item;
            sample.sha = item.at("sha256").get<std::string>();
            if (!std::regex_match(sample.sha, std::regex("[0-9a-f]{64}")))
                throw ConfigError("invalid fixture sha256");
            sample.frames = item.at("num_samples").get<sf_count_t>();
            if (sample.frames < 0 || sample.frames > 15 * 16000 || (expected == "speech" && sample.frames == 0))
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
        if (samples.empty()) throw ConfigError("no inputs selected");
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
    std::string phase = "warmup", stage = "idle", run_id, sample_id;
    std::uint64_t session_id = 0, generation_id = 0;
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
                   {"round", state.round}, {"run_id", state.run_id}, {"sample_id", state.sample_id},
                   {"session_id", state.session_id}, {"generation_id", state.generation_id},
                   {"measurement_begin_ns", state.measurement_begin_ns}};
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
                error = "nonzero post-warmup swap-in/out";
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

void write_wav(const fs::path& path, const std::vector<short>& samples) {
    SF_INFO info{};
    info.samplerate = 16000; info.channels = 1; info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    SNDFILE* file = sf_open(path.c_str(), SFM_WRITE, &info);
    if (!file) throw Failure(4, "cannot create output WAV");
    const auto written = sf_write_short(file, samples.data(), static_cast<sf_count_t>(samples.size()));
    const int rc = sf_close(file);
    if (written != static_cast<sf_count_t>(samples.size()) || rc)
        throw Failure(4, "output WAV write/close failed");
    SF_INFO check{};
    std::unique_ptr<SNDFILE, decltype(&sf_close)> read(sf_open(path.c_str(), SFM_READ, &check), sf_close);
    if (!read || check.frames != static_cast<sf_count_t>(samples.size()) ||
        check.samplerate != 16000 || check.channels != 1)
        throw Failure(4, "output WAV verification failed");
}
int failure_code(const std::exception& e) {
    const auto* failure = dynamic_cast<const Failure*>(&e);
    return failure ? failure->code : dynamic_cast<const ConfigError*>(&e) ? 2 : 4;
}
} // namespace

int main(int argc, char** argv) {
    fs::path root;
    J record = J::object();
    std::string stage = "input_validation";
    try {
        if (argc == 2 && std::string(argv[1]) == "--help") { std::cout << help; return 0; }
        const auto o = parse(argc, argv);
        const auto config = load_config(o.config);
        config_contract(config);
        auto inputs = read_samples(o);
        if (o.validate) {
            std::cout << "VOICE_FILE_INPUT_OK\n" << J{{"selected_samples", inputs.size()},
                {"warmup_rounds", o.warmup_rounds}, {"tts_mode", o.tts_mode}, {"A07", "not_run"}}.dump() << '\n';
            return 0;
        }
        probe::require_model_validation_lock();
        const auto requested = fs::absolute(fs::path(o.output) / o.id);
        // create_directory is exclusive even if another process wins the race.
        fs::create_directories(requested.parent_path());
        if (!fs::create_directory(requested)) throw ConfigError("run directory must be new");
        root = fs::canonical(requested);
        std::signal(SIGINT, signal_handler); std::signal(SIGTERM, signal_handler);
        JsonLines events(root / "events.jsonl"), rounds(root / "rounds.jsonl");
        auto event = [&](const std::string& name, J fields = J::object()) {
            fields["event"] = name; fields["run_id"] = o.id; fields["pid"] = getpid();
            if (!fields.contains("monotonic_ns")) fields["monotonic_ns"] = now_ns();
            events.write(fields);
        };
        const auto manifest_input = read_json(o.manifest);
        record = {{"schema_version", 1}, {"run_id", o.id}, {"mode", "file"}, {"status", "initializing"},
            {"A07", "not_run"}, {"content_review", "not_run"}, {"tts_mode", o.tts_mode},
            {"selected_samples", inputs.size()}, {"warmup_rounds", o.warmup_rounds},
            {"completed_rounds", 0}, {"skipped_rounds", 0}, {"failed_rounds", 0},
            {"completed_warmup_rounds", 0}, {"clean_shutdown", false},
            {"input_policy", "manifest order; ASR hypothesis verbatim to LLM; one independent session per input"},
            {"scope", "P3 file chain; no devices; A03/A04 content and A07 acceptance require independent review"},
            {"config_path", fs::canonical(o.config).string()}, {"config_sha256", file_sha256(o.config)},
            {"manifest_path", fs::canonical(o.manifest).string()}, {"manifest_sha256", file_sha256(o.manifest)},
            {"executable_sha256", file_sha256(fs::canonical("/proc/self/exe"))},
            {"lock_path", probe::kModelValidationLockPath}, {"lock_owner_verified", true},
            {"dataset_id", manifest_input.value("dataset_id", "")},
            {"dataset_kind", manifest_input.value("dataset_kind", "")},
            {"llm_max_output_tokens", config.llm.max_output_tokens},
            {"llm_system_template", llm_system_template()},
            {"tts_threads", config.threads.tts}, {"tts_min_chars", config.tts.min_chars},
            {"tts_max_chars", config.tts.max_chars}, {"tts_flush_ms", config.tts.flush_ms},
            {"npu_memory", "unavailable separately; RSS is not all NPU memory"},
            {"inputs", J::array()}, {"selected_sample_ids", J::array()},
            {"pcm_fingerprint", "fnv1a64 over emitted little-endian PCM16 bytes; noncryptographic correspondence check"},
            {"results", J::array()}, {"output_artifacts", J::array()}};
        for (const auto& input : inputs) record["selected_sample_ids"].push_back(input.id);
        for (const auto& input : inputs)
            record["inputs"].push_back({{"sample_id", input.id}, {"path", input.path.string()},
                {"sha256", input.sha}, {"samples", input.frames}, {"expected_behavior", input.expected}});
        save(root / "run.json", record);
        stage = "asset_validation";
        event("validation_begin");
        load_pcm(inputs);
        auto speech_config = config;
        speech_config.models.llm.enabled = speech_config.models.tts.enabled = speech_config.models.kws.enabled = false;
        const auto speech_assets = validate_assets(speech_config);
        const auto models_path = fs::canonical(config.source_path.parent_path() / config.assets.models_manifest);
        const auto models = read_json(models_path);
        const auto asr_model = selected_model(models, config.models.asr.id);
        const auto vad_model = selected_model(models, config.models.vad.id);
        const auto role = [&](const std::string& key) {
            return model_path(models_path.parent_path(), asr_model, asr_model.at("roles").at(key).get<std::string>());
        };
        const ZipformerFiles files{role("encoder"), role("decoder"), role("joiner"), role("vocabulary"), role("feature_config")};
        const auto vad_file = model_path(models_path.parent_path(), vad_model, vad_model.at("model_path").get<std::string>());
        const auto llm_options = llm_options_from_config(config);
        const auto llm_assets = resolve_rkllm_assets(config);
        record["speech_validated_assets"] = {{"models", speech_assets.models}, {"files", speech_assets.files}};
        event("validation_end"); check_signal();
        stage = "model_initialization";
        const auto init = now_ns();
        auto asr = std::make_unique<ZipformerAsr>(files);
        auto vad = std::make_unique<SileroVad>(vad_file, config.vad, config.threads.vad);
        auto model = std::make_unique<MeloTts>(config, config.threads.tts);
        auto llm = std::make_unique<LlmAdapter>(make_rkllm_backend(llm_assets, llm_options), llm_options);
        auto listening = std::make_unique<ListeningAsr>(*asr, *vad, config.vad);
        record["initialization_ms"] = (now_ns() - init) / 1e6;
        record["libraries"] = mapped_libraries(config); // All large hashes precede sampling.
        record["resident_models"] = J::array({"vad", "asr", "llm", "tts"});
        record["all_models_resident_ns"] = now_ns();
        event("all_models_resident", {{"models", record["resident_models"]}});
        SharedState state; state.run_id = o.id;
        std::mutex generated_mutex;
        std::vector<J> generated;
        auto worker = std::make_unique<TtsStreamWorker>([&](const std::string& text) {
            J fields;
            {
                std::lock_guard<std::mutex> guard(state.mutex);
                fields = {{"phase", state.phase}, {"round", state.round}, {"sample_id", state.sample_id},
                    {"session_id", state.session_id}, {"generation_id", state.generation_id}};
            }
            const auto begin = now_ns();
            event("tts_generate_begin", fields);
            const auto audio = model->generate(text);
            fields["text"] = text; fields["frontend_text"] = melo_frontend_text(text);
            fields["begin_ns"] = begin; fields["end_ns"] = now_ns();
            fields["native_samples"] = audio.samples.size(); fields["generate_s"] = audio.generate_seconds;
            event("tts_generate_end", fields);
            std::lock_guard<std::mutex> guard(generated_mutex); generated.push_back(fields);
            return audio;
        }, config.tts, config.queues, config.timeouts.tts_segment_ms);
        ResourceSampler sampler(root, state, *llm, *worker, config, "review");
        auto round = [&](int number, bool warmup, const Sample& input) {
            const std::string phase = warmup ? "warmup" : "measurement";
            const auto relative = fs::path(warmup ? "warmup" : "samples") /
                (warmup ? std::to_string(number) + "-" + input.id : input.id);
            const auto dir = root / relative;
            fs::create_directories(dir);
            J r{{"schema_version", 1}, {"run_id", o.id}, {"round", number}, {"phase", phase},
                {"warmup", warmup}, {"sample_id", input.id}, {"expected_behavior", input.expected},
                {"input_path", input.path.string()}, {"input_sha256", input.sha}, {"input_samples", input.frames},
                {"session_id", nullptr}, {"generation_id", nullptr}, {"status", "running"},
                {"begin_ns", now_ns()}, {"llm", nullptr}, {"tts", nullptr}, {"output_wav", nullptr},
                {"output_samples", 0}, {"first_pcm_ns", nullptr}, {"first_pcm_s", nullptr},
                {"result_path", (relative / "result.json").string()},
                {"packets_path", (relative / "packets.jsonl").string()},
                {"pcm_frames_path", (relative / "pcm_frames.jsonl").string()}};
            for (const auto* key : {"reference_text", "keywords", "category", "origin_case_id"})
                if (input.metadata.contains(key)) r[key] = input.metadata.at(key);
            auto turn_event = [&](const std::string& name) {
                event(name, {{"phase", phase}, {"round", number}, {"sample_id", input.id},
                    {"session_id", r["session_id"]}, {"generation_id", r["generation_id"]}, {"stage", stage}});
            };
            auto persist = [&] {
                r["end_ns"] = now_ns(); r["complete_s"] = (r["end_ns"].get<std::int64_t>() - r["begin_ns"].get<std::int64_t>()) / 1e9;
                save(dir / "result.json", r); rounds.write(r);
                record["results"].push_back({{"sample_id", input.id}, {"round", number}, {"phase", phase},
                    {"status", r["status"]}, {"result_path", r["result_path"]}});
            };
            bool active_llm = false, active_tts = false;
            LlmTurn turn; TtsStreamId id;
            try {
                stage = "round_boundary"; sampler.check();
                if (!llm->idle() || !worker->wait_idle(0) || llm->restart_required())
                    throw Failure(4, "models not idle at sample boundary; restart required");
                llm->new_session();
                if (llm->history_pairs()) throw Failure(6, "new session retained history");
                {
                    std::lock_guard<std::mutex> guard(state.mutex);
                    state.round = number; state.phase = phase; state.stage = "asr"; state.sample_id = input.id;
                    state.have_turn = false; state.session_id = state.generation_id = 0;
                    state.asr_buffered_samples = 0;
                }
                { std::lock_guard<std::mutex> guard(generated_mutex); generated.clear(); }
                turn_event("round_begin");
                stage = "asr";
                r["asr_begin_ns"] = now_ns(); turn_event("asr_begin"); listening->begin();
                std::size_t consumed = 0;
                while (consumed < input.pcm.size() && listening->state() == ListeningState::listening) {
                    sampler.check();
                    const auto accepted = listening->accept(input.pcm.data() + consumed,
                        std::min<std::size_t>(320, input.pcm.size() - consumed));
                    consumed += accepted;
                    { std::lock_guard<std::mutex> guard(state.mutex); state.asr_buffered_samples = listening->buffered_samples(); }
                    if (!accepted && listening->state() == ListeningState::listening) throw Failure(4, "ASR made no progress");
                }
                const auto result = listening->finish();
                r["asr_end_ns"] = now_ns();
                r["asr"] = {{"hypothesis", result.hypothesis}, {"unfiltered_hypothesis", result.unfiltered_hypothesis},
                    {"reason", listening_end_name(result.reason)}, {"input_samples", result.input_samples},
                    {"consumed_fresh_samples", consumed}, {"endpoint_sample", result.endpoint_sample},
                    {"backends_reset", result.backends_reset}, {"asr_finish_called", result.asr_finish_called},
                    {"speech_detected", result.vad.speech_detected}, {"encoder_chunks", result.asr.encoder_chunks},
                    {"vad_windows", result.vad.windows}, {"endpoint_to_final_sec", result.endpoint_to_final_sec},
                    {"processing_s", (r["asr_end_ns"].get<std::int64_t>() - r["asr_begin_ns"].get<std::int64_t>()) / 1e9}};
                r["asr"]["rtf"] = input.frames ? J(r["asr"]["processing_s"].get<double>() / (input.frames / 16000.0)) : J();
                turn_event("asr_end");
                if (!result.backends_reset || !asr->partial().empty() || asr->stats().input_samples || vad->stats().input_samples)
                    throw Failure(6, "ASR/VAD reset failed");
                if (result.hypothesis.empty() || result.hypothesis.find_first_not_of(" \t\r\n") == std::string::npos) {
                    r["status"] = "skipped_empty"; r["skip_reason"] = "empty ASR hypothesis; no LLM or TTS submission";
                    r["expected_empty"] = input.expected != "speech";
                    if (!warmup) record["skipped_rounds"] = record["skipped_rounds"].get<int>() + 1;
                    turn_event("empty_input_skipped"); sampler.sample(); persist(); return r;
                }
                if (input.expected != "speech") throw Failure(6, "non-speech input produced nonempty hypothesis");
                if (!llm->idle()) throw Failure(6, "ASR overlapped LLM NPU inference");
                stage = "llm_start";
                r["llm_begin_ns"] = now_ns();
                // Never pass reference text, keywords, or an independent fixed prompt.
                turn = llm->start(result.hypothesis); active_llm = true;
                id = {turn.session_id, turn.generation_id};
                r["session_id"] = id.session; r["generation_id"] = id.generation;
                r["llm"] = {{"input_text", result.hypothesis}, {"text", ""}, {"status", "running"}};
                turn_event("llm_begin"); stage = "tts_begin"; worker->begin(id); active_tts = true;
                {
                    std::lock_guard<std::mutex> guard(state.mutex);
                    state.turn = turn; state.have_turn = true; state.stage = "llm_tts";
                    state.session_id = id.session; state.generation_id = id.generation; state.asr_buffered_samples = 0;
                }
                turn_event("tts_begin");
                JsonLines packets(dir / "packets.jsonl"), pcm_frames(dir / "pcm_frames.jsonl");
                std::vector<short> audio; audio.reserve(120 * 16000);
                std::string text;
                bool input_end = false;
                std::int64_t first_pcm = 0;
                std::uint64_t frame_count = 0;
                double energy = 0;
                LlmResult completed;
                const auto consume = [&](const TtsStreamFrame& frame) {
                    stage = "pcm_consume";
                    if (frame.id.session != id.session || frame.id.generation != id.generation ||
                        frame.audio.valid_samples > frame.audio.samples.size() ||
                        audio.size() + frame.audio.valid_samples > 120 * 16000)
                        throw Failure(4, "invalid PCM identity/size");
                    for (std::size_t i = 0; i < frame.audio.valid_samples; ++i)
                        if (!std::isfinite(frame.audio.samples[i])) throw Failure(4, "non-finite PCM");
                    static_assert(sizeof(short) == 2, "PCM16 requires 16-bit short");
                    std::vector<short> quantized;
                    quantized.reserve(frame.audio.valid_samples);
                    std::uint64_t fingerprint = 14695981039346656037ULL;
                    for (std::size_t i = 0; i < frame.audio.valid_samples; ++i) {
                        const double scaled = std::max(-32768.0, std::min(32767.0,
                            static_cast<double>(frame.audio.samples[i]) * 32768.0));
                        const auto value = static_cast<short>(std::lround(scaled));
                        quantized.push_back(value);
                        const auto bits = static_cast<std::uint16_t>(value);
                        fingerprint = (fingerprint ^ (bits & 255U)) * 1099511628211ULL;
                        fingerprint = (fingerprint ^ (bits >> 8U)) * 1099511628211ULL;
                    }
                    std::ostringstream fingerprint_text;
                    fingerprint_text << std::hex << std::setfill('0') << std::setw(16) << fingerprint;
                    if (!worker->commit_if_current(id, [&] {
                        if (!first_pcm) first_pcm = now_ns();
                        audio.insert(audio.end(), quantized.begin(), quantized.end());
                        ++frame_count;
                    })) throw Failure(4, "stale PCM commit");
                    for (std::size_t i = 0; i < frame.audio.valid_samples; ++i)
                        energy += static_cast<double>(frame.audio.samples[i]) * frame.audio.samples[i];
                    pcm_frames.write({{"run_id", o.id}, {"sample_id", input.id}, {"phase", phase}, {"round", number},
                        {"session_id", id.session}, {"generation_id", id.generation}, {"seq", frame.seq},
                        {"valid_samples", frame.audio.valid_samples}, {"monotonic_ns", now_ns()},
                        {"pcm16_fnv1a64", fingerprint_text.str()}});
                };
                while (true) {
                    stage = "llm_tts"; sampler.check();
                    if (now_ns() - r["begin_ns"].get<std::int64_t>() > 120000000000LL)
                        throw Failure(5, "sample deadline exceeded 120 seconds");
                    if (llm->restart_required()) throw Failure(4, "LLM idle unconfirmed; restart required");
                    if (!input_end) {
                        stage = "llm_poll";
                        auto packet = llm->poll(turn);
                        const bool idle = llm->idle();
                        if (!packet && idle) packet = llm->poll(turn);
                        if (packet) {
                            if (packet->session_id != id.session || packet->generation_id != id.generation ||
                                text.size() + packet->text.size() > 8192) throw Failure(6, "LLM packet identity/bound failed");
                            stage = "tts_submit";
                            if (!llm->commit_if_current(turn, [&] {
                                if (o.tts_mode == "stream" && !packet->text.empty() && !worker->submit(id, packet->text))
                                    throw Failure(4, "TTS rejected current text packet");
                            })) throw Failure(4, "LLM generation invalidated");
                            text += packet->text;
                            const auto stamp = now_ns();
                            packets.write({{"run_id", o.id}, {"sample_id", input.id}, {"phase", phase}, {"round", number},
                                {"session_id", id.session}, {"generation_id", id.generation}, {"seq", packet->seq},
                                {"text", packet->text}, {"token_id", packet->token_id}, {"callback_ns", packet->monotonic_ns},
                                {"monotonic_ns", stamp}, {"offset_ns", stamp - llm->snapshot(turn).submit_ns}});
                            r["llm"]["text"] = text;
                        } else if (idle) {
                            stage = "llm_completion";
                            const auto done = llm->wait(turn, 1);
                            if (!done) throw Failure(4, "LLM idle without result");
                            completed = *done;
                            r["llm"] = {{"input_text", result.hypothesis}, {"text", completed.text}, {"status", completed.status},
                                {"run_rc", completed.run_rc}, {"clear_rc", completed.clear_rc}, {"idle_ns", completed.idle_ns},
                                {"submit_ns", completed.submit_ns}, {"first_token_ns", completed.first_token_ns},
                                {"first_text_ns", completed.first_text_ns}, {"finish_ns", completed.finish_ns},
                                {"run_return_ns", completed.run_return_ns}, {"token_callbacks", completed.token_callbacks},
                                {"output_budget", completed.output_budget}, {"queue_peak_bytes", completed.queue_peak_bytes},
                                {"history_pairs", llm->history_pairs()}, {"history_committed", false},
                                {"overflow", completed.overflow}, {"invalid_utf8", completed.invalid_utf8},
                                {"restart_required", completed.restart_required}};
                            if (completed.status != "completed" || completed.run_rc || completed.clear_rc ||
                                completed.overflow || completed.invalid_utf8 || completed.restart_required || text.empty() || text != completed.text)
                                throw Failure(4, "LLM completion failed: " + completed.status);
                            r["llm_end_ns"] = now_ns(); turn_event("llm_end");
                            stage = "tts_finish";
                            if (o.tts_mode == "batch" && !worker->submit(id, text)) throw Failure(4, "TTS batch rejected");
                            if (!worker->finish(id)) throw Failure(4, "TTS finish rejected");
                            input_end = true;
                            { std::lock_guard<std::mutex> guard(state.mutex); state.stage = "tts_drain"; }
                        }
                    }
                    while (auto frame = worker->poll(id)) consume(*frame);
                    if (worker->wait_idle(0)) {
                        while (auto frame = worker->poll(id)) consume(*frame);
                        stage = "tts_completion";
                        const auto ts = worker->stats();
                        if (ts.status == "failed") throw Failure(4, ts.error);
                        if (input_end && llm->idle()) break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                stage = "completion_checks";
                const auto t = worker->stats(); const auto l = llm->snapshot(turn);
                r["tts"] = {{"status", t.status}, {"segments", t.segments}, {"frames", t.frames},
                    {"consumed_frames", t.consumed_frames}, {"pcm_samples", audio.size()}, {"pcm_energy", energy},
                    {"first_segment_ns", t.first_segment_ns}, {"first_pcm_ns", t.first_pcm_ns},
                    {"producer_done_ns", t.producer_done_ns}, {"generate_s", t.generate_seconds},
                    {"input_bytes", t.input_bytes}, {"peak_input_bytes", t.peak_input_bytes},
                    {"peak_segments", t.peak_segments}, {"peak_pcm_frames", t.peak_pcm_frames},
                    {"current_input_bytes", t.current_input_bytes}, {"current_segments", t.current_segments},
                    {"current_pcm_frames", t.current_pcm_frames}, {"segmenter_pending_bytes", t.segmenter_pending_bytes},
                    {"discarded_generation_results", t.discarded_generation_results}, {"rejected_stale_inputs", t.rejected_stale_inputs}};
                {
                    std::lock_guard<std::mutex> guard(generated_mutex); r["tts_generations"] = generated;
                }
                if (t.status != "completed" || audio.empty() || energy <= 1e-12 || !t.segments ||
                    t.current_input_bytes || t.current_segments || t.current_pcm_frames || t.segmenter_pending_bytes ||
                    l.queue_current_bytes || l.queue_current_packets || t.frames != t.consumed_frames ||
                    frame_count != t.frames || t.discarded_generation_results || t.rejected_stale_inputs)
                    throw Failure(6, "incomplete audio/queues/identity");
                sampler.check(); sampler.sample();
                stage = "output_write"; turn_event("output_write_begin");
                // I/O occurs outside generation-commit mutexes; the vector has a hard 120-second bound.
                write_wav(dir / "output-16k.wav", audio);
                r["output_wav"] = (relative / "output-16k.wav").string(); r["output_samples"] = audio.size();
                r["first_pcm_ns"] = first_pcm ? J(first_pcm) : J();
                r["first_pcm_s"] = first_pcm ? J((first_pcm - r["begin_ns"].get<std::int64_t>()) / 1e9) : J();
                r["llm_to_first_pcm_s"] = first_pcm ? J((first_pcm - completed.submit_ns) / 1e9) : J();
                r["npu_serial"] = r["asr_end_ns"].get<std::int64_t>() <= completed.submit_ns;
                r["reset_verified"] = true;
                check_signal(); // A written WAV does not acknowledge an interrupted turn.
                if (!llm->commit_history(turn)) throw Failure(4, "output history acknowledgement failed");
                r["llm"]["history_committed"] = true;
                r["status"] = "completed"; turn_event("round_complete");
                if (!warmup) record["completed_rounds"] = record["completed_rounds"].get<int>() + 1;
                persist(); return r;
            } catch (const std::exception& e) {
                r["status"] = "failed"; r["failure_stage"] = stage; r["error"] = e.what();
                r["exit_code"] = failure_code(e);
                if (!warmup) record["failed_rounds"] = record["failed_rounds"].get<int>() + 1;
                // Persist the original failure before bounded cleanup; no later sample is admitted.
                try { turn_event("round_failed"); persist(); save(root / "run.json", record); } catch (...) {}
                if (active_tts) worker->cancel(id);
                if (active_llm) llm->cancel();
                if (listening->state() == ListeningState::listening) listening->cancel();
                throw;
            }
        };
        record["status"] = "warmup"; save(root / "run.json", record);
        for (int i = 0; i < o.warmup_rounds; ++i) {
            stage = "warmup"; round(i, true, inputs[static_cast<std::size_t>(i) % inputs.size()]);
            record["completed_warmup_rounds"] = i + 1;
        }
        const auto measurement_begin = now_ns();
        {
            std::lock_guard<std::mutex> guard(state.mutex);
            state.phase = "measurement"; state.measurement_begin_ns = measurement_begin;
            // The completed warmup identities and peaks cannot enter measured samples.
            // ResourceSampler zeroes both LLM and TTS peaks when have_turn is false;
            // the first measured worker.begin() resets TTS statistics before it is set true.
            state.have_turn = false; state.turn = LlmTurn{};
            state.round = -1; state.sample_id.clear(); state.stage = "between_rounds";
            state.session_id = state.generation_id = 0; state.asr_buffered_samples = 0;
        }
        record["measurement_begin_ns"] = measurement_begin;
        record["status"] = "measurement"; save(root / "run.json", record);
        event("measurement_begin");
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            round(static_cast<int>(i), false, inputs[i]); save(root / "run.json", record);
        }
        sampler.sample(); sampler.check(); sampler.finish();
        record["measurement_end_ns"] = now_ns();
        record["resources"] = sampler.summary();
        event("measurement_end");
        stage = "shutdown";
        check_signal();
        if (!llm->idle() || !worker->wait_idle(0) || !llm->close()) throw Failure(4, "clean shutdown not confirmed");
        worker.reset(); listening.reset(); llm.reset(); vad.reset(); asr.reset(); model.reset();
        check_signal();
        record["clean_shutdown"] = true; event("shutdown_complete");
        stage = "artifact_hashes";
        // Hash small outputs only after sampler and every model have stopped.
        for (const auto& item : record["results"]) {
            check_signal();
            const auto r = read_json(root / item.at("result_path").get<std::string>());
            if (r["output_wav"].is_string()) {
                const auto path = r["output_wav"].get<std::string>();
                record["output_artifacts"].push_back({{"sample_id", r["sample_id"]}, {"phase", r["phase"]},
                    {"round", r["round"]}, {"path", path}, {"sha256", file_sha256(root / path)}});
            }
            check_signal();
        }
        bool expected_skips = true;
        for (const auto& item : record["results"]) if (item["phase"] == "measurement" && item["status"] == "skipped_empty") {
            const auto r = read_json(root / item.at("result_path").get<std::string>());
            if (!r.value("expected_empty", false)) expected_skips = false;
        }
        const bool flow_ok = record["failed_rounds"] == 0 && expected_skips;
        record["status"] = flow_ok ? "completed" : "failed";
        record["software_checks"] = flow_ok ? "passed" : "failed";
        record["flow_20_of_20"] = inputs.size() == 20 && record["completed_rounds"] == 20;
        record["A07"] = record["flow_20_of_20"].get<bool>() ? "pending_external_review" : "not_run";
        record["content_review"] = "not_run";
        record["exit_code"] = flow_ok ? 0 : 6;
        check_signal();
        save(root / "run.json", record);
        check_signal();
        save(root / "summary.json", record);
        check_signal();
        std::cout << (flow_ok ? "VOICE_FILE_OK " : "VOICE_FILE_FAILED ") << J{{"run_id", o.id},
            {"completed_rounds", record["completed_rounds"]}, {"skipped_rounds", record["skipped_rounds"]},
            {"A07", record["A07"]}}.dump() << '\n';
        check_signal();
        return flow_ok ? 0 : 6;
    } catch (const std::exception& e) {
        const int code = failure_code(e);
        std::cerr << "VOICE_FILE_FAILED " << stage << ": " << e.what() << '\n';
        if (!root.empty()) try {
            record["status"] = "failed"; record["failure_stage"] = stage; record["error"] = e.what();
            record["exit_code"] = code; record["A07"] = "not_run"; record["clean_shutdown"] = false;
            record["failure_ns"] = now_ns();
            save(root / "failure.json", record);
            save(root / "run.json", record); save(root / "summary.json", record);
        } catch (...) {}
        return code;
    }
}
