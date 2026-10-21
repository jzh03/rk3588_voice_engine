#include "voice_engine/assets.hpp"
#include "voice_engine/probe_support.hpp"
#include "voice_engine/tts.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <numeric>
#include <random>
#include <regex>
#include <set>
#include <sndfile.h>
#include <sstream>
#include <sys/resource.h>
#include <thread>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using namespace voice_engine;
struct Options {
    std::string config, cases, output, run_id, only_case;
    int threads = 0, repeat = 3, warmup = 1;
    bool text_only = false;
};
const char *usage =
    "tts_probe --config PATH --cases PATH [--output-dir ROOT] --run-id ID [--threads 2|4] "
    "[--repeat 1..100] [--warmup 0..10] [--case-id ID] [--validate-text-only]\nOutputs native "
    "44100 Hz and resampled 16000 Hz mono WAV; never opens an audio device.\nModel execution "
    "requires: flock -x /tmp/rk3588_voice_engine_model_validation.lock ...\n";
bool safe_id(const std::string &s) {
    return std::regex_match(s, std::regex("[A-Za-z0-9][A-Za-z0-9_-]{0,95}"));
}
int number(const std::string &s, int lo, int hi) {
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument("invalid numeric argument");
    const auto n = std::stoll(s);
    if (n < lo || n > hi)
        throw std::invalid_argument("numeric argument out of range");
    return static_cast<int>(n);
}
Options parse(int argc, char **argv) {
    Options o;
    std::set<std::string> seen;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (!seen.insert(a).second)
            throw std::invalid_argument("duplicate argument: " + a);
        if (a == "--validate-text-only") {
            o.text_only = true;
            continue;
        }
        if (i + 1 >= argc)
            throw std::invalid_argument("missing value: " + a);
        const std::string v = argv[++i];
        if (a == "--config")
            o.config = v;
        else if (a == "--cases")
            o.cases = v;
        else if (a == "--output-dir")
            o.output = v;
        else if (a == "--run-id")
            o.run_id = v;
        else if (a == "--threads") {
            o.threads = number(v, 2, 4);
            if (o.threads == 3)
                throw std::invalid_argument("threads must be 2 or 4");
        } else if (a == "--repeat")
            o.repeat = number(v, 1, 100);
        else if (a == "--warmup")
            o.warmup = number(v, 0, 10);
        else if (a == "--case-id")
            o.only_case = v;
        else
            throw std::invalid_argument("unknown argument: " + a);
    }
    if (o.cases.empty() || (!o.text_only && o.config.empty()) || !safe_id(o.run_id))
        throw std::invalid_argument(
            "--cases, --config (model mode), and safe --run-id are required");
    if (o.output.empty())
        o.output = "output/runs";
    return o;
}
Json read_cases(const Options &o) {
    if (fs::file_size(o.cases) > 1024 * 1024)
        throw std::invalid_argument("cases file exceeds 1 MiB");
    std::ifstream f(o.cases);
    const auto d = Json::parse(f);
    if (d.at("schema_version") != 1 || !d.at("cases").is_array() || d.at("cases").empty() ||
        d.at("cases").size() > 1000)
        throw std::invalid_argument("invalid case schema");
    Json cases = Json::array();
    std::set<std::string> ids;
    for (const auto &c : d.at("cases")) {
        const auto id = c.at("id").get<std::string>();
        const auto text = c.at("text").get<std::string>();
        if (!safe_id(id) || !ids.insert(id).second || text.size() > 8192 ||
            !c.at("category").is_string())
            throw std::invalid_argument("invalid/duplicate case id, text, or category");
        if (o.only_case.empty() || o.only_case == id)
            cases.push_back(c);
    }
    if (cases.empty())
        throw std::invalid_argument("case filter matched nothing");
    return cases;
}
void json_file(const fs::path &p, const Json &j) {
    std::ofstream f(p);
    f << j.dump(2) << '\n';
    if (!f)
        throw std::runtime_error("cannot write " + p.string());
}
long mem_available() {
    std::ifstream f("/proc/meminfo");
    std::string line;
    while (std::getline(f, line))
        if (line.rfind("MemAvailable:", 0) == 0) {
            std::istringstream s(line.substr(13));
            long n;
            s >> n;
            return n;
        }
    throw std::runtime_error("cannot read MemAvailable");
}
long rss() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line))
        if (line.rfind("VmRSS:", 0) == 0) {
            std::istringstream s(line.substr(6));
            long n;
            s >> n;
            return n;
        }
    return 0;
}
void check_memory() {
    if (mem_available() < 1572864)
        throw std::runtime_error("MemAvailable below 1.5 GiB safety floor");
}
struct Monitor {
    std::atomic<bool> stop{false}, low_memory{false}, failed{false};
    std::atomic<long> peak_rss{0}, min_available{LONG_MAX};
    std::mutex mutex;
    std::string failure;
    std::thread worker;
    explicit Monitor(const fs::path &path) {
        worker = std::thread([this, path] {
            try {
                std::ofstream f(path);
                if (!f)
                    throw std::runtime_error("cannot open resource log");
                const auto begin = Clock::now();
                while (!stop) {
                    const long r = rss(), m = mem_available();
                    peak_rss = std::max(peak_rss.load(), r);
                    min_available = std::min(min_available.load(), m);
                    if (m < 1572864)
                        low_memory = true;
                    f << Json{{"elapsed_s",
                               std::chrono::duration<double>(Clock::now() - begin).count()},
                              {"rss_kib", r},
                              {"mem_available_kib", m}}
                             .dump()
                      << '\n';
                    f.flush();
                    if (!f)
                        throw std::runtime_error("cannot flush resource log");
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            } catch (const std::exception &e) {
                std::lock_guard<std::mutex> lock(mutex);
                failure = e.what();
                failed = true;
            }
        });
    }
    std::string error() {
        std::lock_guard<std::mutex> lock(mutex);
        return failure;
    }
    void finish() {
        stop = true;
        if (worker.joinable())
            worker.join();
    }
    ~Monitor() { finish(); }
};
Json libraries() {
    std::ifstream f("/proc/self/maps");
    std::string line;
    std::set<std::string> paths;
    while (std::getline(f, line)) {
        const auto p = line.find('/');
        if (p != std::string::npos && line.find(".so", p) != std::string::npos)
            paths.insert(line.substr(p));
    }
    return paths;
}
void wav(const fs::path &p, const std::vector<float> &samples, int rate) {
    // O_EXCL also prevents accidental overwrite if a path appears after run creation.
    const int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        throw std::runtime_error("WAV output already exists or cannot be created: " + p.string());
    SF_INFO info{};
    info.samplerate = rate;
    info.channels = 1;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    SNDFILE *f = sf_open_fd(fd, SFM_WRITE, &info, SF_TRUE);
    if (!f) {
        ::close(fd);
        throw std::runtime_error("cannot create WAV");
    }
    const auto count = sf_writef_float(f, samples.data(), static_cast<sf_count_t>(samples.size()));
    const int err = sf_close(f);
    if (count != static_cast<sf_count_t>(samples.size()) || err)
        throw std::runtime_error("short/failed WAV write");
}
Json stats(std::vector<double> x) {
    if (x.empty())
        return nullptr;
    std::sort(x.begin(), x.end());
    const auto rank = [&](double p) {
        return x[static_cast<std::size_t>(std::ceil(p * x.size())) - 1];
    };
    return Json{{"n", x.size()},
                {"p50", rank(.50)},
                {"p95", rank(.95)},
                {"max", x.back()},
                {"method", "nearest-rank"}};
}
double seconds(Clock::time_point p) {
    return std::chrono::duration<double>(Clock::now() - p).count();
}
} // namespace
int main(int argc, char **argv) {
    if (argc == 2 && std::string(argv[1]) == "--help") {
        std::cout << usage;
        return 0;
    }
    fs::path output;
    bool model_phase = false;
    try {
        const auto o = parse(argc, argv);
        const auto cases = read_cases(o);
        Config config;
        if (!o.config.empty())
            config = load_config(o.config);
        const int threads = o.threads ? o.threads : config.threads.tts;
        if (!o.text_only && threads != 2 && threads != 4)
            throw std::invalid_argument("TTS thread count must be 2 or 4");
        const fs::path requested = fs::absolute(fs::path(o.output) / o.run_id);
        fs::create_directories(requested.parent_path());
        if (!fs::create_directory(requested))
            throw std::invalid_argument(
                "output directory already exists; never overwrite evidence");
        output = fs::canonical(requested);
        Json report = {{"schema_version", 1},
                       {"run_id", o.run_id},
                       {"cases_sha256", file_sha256(o.cases)},
                       {"threads", threads},
                       {"repeat", o.repeat},
                       {"warmup", o.warmup},
                       {"status", "running"},
                       {"mode", o.text_only ? "text-only" : "MeloTTS CPU file probe"},
                       {"subjective_audio_quality", "not listened to or user-confirmed"}};
        Json argv_json = Json::array();
        for (int i = 0; i < argc; ++i)
            argv_json.push_back(argv[i]);
        report["command"] = argv_json;
        if (!o.text_only) {
            report["config_sha256"] = file_sha256(o.config);
            report["binary_sha256"] = file_sha256("/proc/self/exe");
            report["models_manifest_sha256"] =
                file_sha256(config.source_path.parent_path() / config.assets.models_manifest);
            report["deps_manifest_sha256"] =
                file_sha256(config.source_path.parent_path() / config.assets.deps_manifest);
        }
        json_file(output / "run.json", report);
        if (o.text_only) {
            Json rows = Json::array();
            std::size_t failures = 0;
            for (const auto &c : cases) {
                Json r = c;
                try {
                    r["segments"] = segment_tts_text(c.at("text"), config.tts);
                    r["success"] = true;
                } catch (const std::exception &e) {
                    r["success"] = false;
                    r["error"] = e.what();
                    ++failures;
                }
                rows.push_back(r);
            }
            report["results"] = rows;
            report["failed"] = failures;
            report["status"] = failures ? "failed" : "ok";
            json_file(output / "summary.json", report);
            std::cout << report.dump() << '\n';
            return failures ? 2 : 0;
        }
        probe::require_model_validation_lock();
        check_memory();
        Monitor monitor(output / "resources.jsonl");
        const auto loading = Clock::now();
        model_phase = true;
        MeloTts model(config, threads);
        report["adapter_construction_s"] = seconds(loading);
        report["model_initialization_s"] = model.model_initialization_seconds();
        report["runtime_version"] = MeloTts::runtime_version();
        report["loaded_libraries"] = libraries();
        report["model_id"] = model.assets().model_id;
        report["resolved_assets"] = {{"model", model.assets().model.string()},
                                     {"tokens", model.assets().tokens.string()},
                                     {"lexicon", model.assets().lexicon.string()},
                                     {"dictionary", model.assets().dictionary.string()}};
        for (const auto &rule : model.assets().rule_fsts)
            report["resolved_assets"]["rule_fsts"].push_back(rule.string());
        report["case_order_seed"] = 20261020;
        report["timeout_contract"] =
            "config timeouts.tts_segment_ms is checked after blocking Generate returns; hard "
            "process bound requires external timeout, which exits 124 and may leave partial JSONL "
            "without summary. No A08 cancellation claim.";
        report["normalization"] =
            "validated FST order: date, number, phone, new_heteronym; the fixed C API does not "
            "expose normalized text; spoken pronunciation needs listening";
        report["measurement"] =
            "Generate excludes conversion; first_segment_s includes segmentation, first Generate "
            "and conversion; complete_s includes all segments and final resampler/frame flush, "
            "excludes file I/O. First segment is not first sample streaming latency. Stats are "
            "successful measured attempts only; failures are retained and fail the gate.";
        report["fixed_options"] = {{"sid", 0},
                                   {"speed", 1.0},
                                   {"silence_scale", 1.0},
                                   {"provider", "cpu"},
                                   {"noise_scale", 0.667},
                                   {"noise_scale_w", 0.8},
                                   {"deterministic", false}};
        json_file(output / "run.json", report);
        std::ofstream rows(output / "results.jsonl");
        Json results = Json::array();
        std::size_t failed = 0, warmup_failed = 0;
        std::vector<double> gen_rtf, complete_rtf, gen_times, first_times, complete_times;
        std::mt19937 order_rng(20261020);
        bool timed_out = false, input_failed = false;
        for (int rep = -o.warmup; rep < o.repeat; ++rep) {
            // Warmup uses the first case, same resident model, with explicit records.
            const std::size_t count = rep < 0 ? 1 : cases.size();
            std::vector<std::size_t> order(count);
            std::iota(order.begin(), order.end(), 0);
            if (rep >= 0)
                std::shuffle(order.begin(), order.end(), order_rng);
            for (std::size_t ci : order) {
                const auto &c = cases[ci];
                const bool warm = rep < 0;
                const auto id = c.at("id").get<std::string>();
                Json row = {{"id", id},
                            {"text", c.at("text")},
                            {"category", c.at("category")},
                            {"warmup", warm},
                            {"repeat_index", warm ? rep + o.warmup : rep},
                            {"success", false},
                            {"threads", threads}};
                double generate_s = 0;
                const auto begin = Clock::now();
                try {
                    if (monitor.failed)
                        throw std::runtime_error("resource monitoring failed: " + monitor.error());
                    if (monitor.low_memory)
                        throw std::runtime_error(
                            "resource monitor observed MemAvailable below 1.5 GiB");
                    check_memory();
                    const auto text = c.at("text").get<std::string>();
                    const auto parts = segment_tts_text(text, config.tts);
                    row["segments"] = parts;
                    row["unknown_english_words"] = model.unknown_english_words(text);
                    std::vector<float> native, internal;
                    std::vector<std::size_t> valid;
                    TtsPcmConverter converter;
                    const auto accept = [&](const std::vector<PaddedAudioFrame> &frames) {
                        for (const auto &f : frames) {
                            valid.push_back(f.valid_samples);
                            internal.insert(internal.end(), f.samples.begin(),
                                            f.samples.begin() +
                                                static_cast<std::ptrdiff_t>(f.valid_samples));
                        }
                    };
                    Json segments = Json::array();
                    for (std::size_t si = 0; si < parts.size(); ++si) {
                        check_memory();
                        const auto start = Clock::now();
                        auto audio = model.generate(parts[si]);
                        const double segment_wall_s = seconds(start);
                        const double elapsed = audio.generate_seconds;
                        generate_s += elapsed;
                        if (segment_wall_s * 1000 > config.timeouts.tts_segment_ms) {
                            timed_out = true;
                            row["failure_code"] = 5;
                            throw std::runtime_error("blocking Generate exceeded configured "
                                                     "tts_segment_ms; detected on return");
                        }
                        if (audio.samples.size() >
                            static_cast<std::size_t>(44100) * config.queues.max_tts_audio_ms / 1000)
                            throw std::runtime_error(
                                "native segment exceeds configured max_tts_audio_ms");
                        accept(converter.push(audio.samples.data(), audio.samples.size()));
                        native.insert(native.end(), audio.samples.begin(), audio.samples.end());
                        segments.push_back({{"text", parts[si]},
                                            {"generate_s", elapsed},
                                            {"adapter_s", segment_wall_s},
                                            {"native_samples", audio.samples.size()}});
                        if (si == 0)
                            row["first_segment_s"] = seconds(begin);
                    }
                    accept(converter.finish());
                    const double complete_s = seconds(begin);
                    if (native.empty() || internal.empty())
                        throw std::runtime_error("empty generated/converted PCM");
                    if (monitor.failed)
                        throw std::runtime_error("resource monitoring failed: " + monitor.error());
                    if (monitor.low_memory)
                        throw std::runtime_error(
                            "resource monitor observed MemAvailable below 1.5 GiB");
                    double native_energy = 0, internal_energy = 0;
                    for (float x : native)
                        native_energy += static_cast<double>(x) * x;
                    for (float x : internal)
                        internal_energy += static_cast<double>(x) * x;
                    if (native_energy == 0 || internal_energy == 0)
                        throw std::runtime_error("generated or resampled audio is all-zero");
                    const auto expected = std::llround(native.size() * 16000.0 / 44100);
                    if (std::abs(static_cast<long long>(internal.size()) - expected) > 1)
                        throw std::runtime_error("resampling lost duration or final filter tail");
                    row["native_rms"] = std::sqrt(native_energy / native.size());
                    row["internal_rms"] = std::sqrt(internal_energy / internal.size());
                    const double duration = static_cast<double>(native.size()) / 44100;
                    const auto stem = (warm ? "warmup-" : "") + id + "-r" +
                                      std::to_string(warm ? rep + o.warmup : rep);
                    wav(output / (stem + "-native.wav"), native, 44100);
                    wav(output / (stem + "-16k.wav"), internal, 16000);
                    row["success"] = true;
                    row["generate_s"] = generate_s;
                    row["complete_s"] = complete_s;
                    row["generate_rtf"] = generate_s / duration;
                    row["complete_rtf"] = complete_s / duration;
                    row["duration_s"] = duration;
                    row["native_sample_rate"] = 44100;
                    row["internal_sample_rate"] = 16000;
                    row["channels"] = 1;
                    row["native_samples"] = native.size();
                    row["internal_samples"] = internal.size();
                    row["frame_ms"] = 20;
                    row["frame_count"] = valid.size();
                    row["last_frame_valid_samples"] = valid.back();
                    row["segment_metrics"] = segments;
                    row["native_wav"] = stem + "-native.wav";
                    row["internal_wav"] = stem + "-16k.wav";
                    row["native_sha256"] = file_sha256(output / (stem + "-native.wav"));
                    row["internal_sha256"] = file_sha256(output / (stem + "-16k.wav"));
                    if (!warm) {
                        gen_rtf.push_back(generate_s / duration);
                        complete_rtf.push_back(complete_s / duration);
                        gen_times.push_back(generate_s);
                        first_times.push_back(row.at("first_segment_s"));
                        complete_times.push_back(complete_s);
                    }
                } catch (const std::exception &e) {
                    row["success"] = false;
                    if (dynamic_cast<const std::invalid_argument *>(&e)) {
                        input_failed = true;
                        row["failure_code"] = 2;
                    } else if (!row.contains("failure_code"))
                        row["failure_code"] = 4;
                    row["error"] = e.what();
                    row["elapsed_until_failure_s"] = seconds(begin);
                    row["generate_before_failure_s"] = generate_s;
                    if (warm)
                        ++warmup_failed;
                    else
                        ++failed;
                }
                row["rss_kib"] = rss();
                row["mem_available_kib"] = mem_available();
                rows << row.dump() << '\n';
                rows.flush();
                if (!rows) throw std::runtime_error("cannot persist result JSONL");
                results.push_back(row);
                std::cout << (warm ? "WARMUP " : "CASE ") << id << " repeat=" << rep
                          << " success=" << row.at("success") << std::endl;
            }
        }
        monitor.finish();
        report["resource_monitor_error"] = monitor.error();
        report["low_memory_observed"] = monitor.low_memory.load();
        struct rusage ru {};
        getrusage(RUSAGE_SELF, &ru);
        report["results"] = results;
        report["attempts"] = cases.size() * o.repeat;
        report["successful"] = gen_rtf.size();
        report["failed"] = failed;
        report["warmup_failed"] = warmup_failed;
        report["generate_rtf"] = stats(gen_rtf);
        report["complete_rtf"] = stats(complete_rtf);
        report["generate_s"] = stats(gen_times);
        report["first_segment_s"] = stats(first_times);
        report["complete_s"] = stats(complete_times);
        report["peak_rss_kib"] = ru.ru_maxrss;
        report["sampled_peak_rss_kib"] = monitor.peak_rss.load();
        report["min_mem_available_kib"] = monitor.min_available.load();
        report["performance_gate_pass"] = failed == 0 && warmup_failed == 0 && !monitor.failed &&
                                          !monitor.low_memory && !gen_rtf.empty() &&
                                          report["generate_rtf"]["p95"].get<double>() < 1;
        report["a05_pass"] = false;
        report["a05_pending"] = "user listening/number-date pronunciation confirmation and any "
                                "unmet performance condition";
        report["status"] = failed || warmup_failed ? "failed" : "completed";
        json_file(output / "summary.json", report);
        std::cout << "TTS_PROBE_COMPLETE output=" << output << " failed=" << failed
                  << " RTF=" << report["generate_rtf"].dump() << std::endl;
        if (input_failed)
            return 2;
        if (timed_out)
            return 5;
        if (failed || warmup_failed || monitor.failed || monitor.low_memory)
            return 4;
        return report["performance_gate_pass"].get<bool>() ? 0 : 6;
    } catch (const std::exception &e) {
        if (!output.empty()) {
            try {
                json_file(output / "failure.json", Json{{"error", e.what()}, {"status", "failed"}});
            } catch (...) {
            }
        }
        std::cerr << "TTS_PROBE_ERROR: " << e.what() << '\n';
        return dynamic_cast<const ConfigError *>(&e) ||
                       dynamic_cast<const std::invalid_argument *>(&e) || !model_phase
                   ? 2
                   : 4;
    }
}
