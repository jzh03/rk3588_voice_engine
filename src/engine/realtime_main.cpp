#include "voice_engine/realtime.hpp"
#include "voice_engine/assets.hpp"
#include "voice_engine/control.hpp"
#include "voice_engine/logger.hpp"
#include "voice_engine/probe_support.hpp"
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <unistd.h>
#include <thread>
namespace voice_engine {
namespace {
volatile sig_atomic_t signal_requested = 0;
void handle_signal(int n) { signal_requested = n; }
std::int64_t now_ns() { return std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count(); }
std::int64_t memory_available() {
    std::ifstream input("/proc/meminfo"); std::string line;
    while(std::getline(input, line)) {
        std::istringstream fields(line); std::string key; std::int64_t value;
        if ((fields >> key >> value) && key == "MemAvailable:") return value;
    }
    return -1;
}
std::int64_t proc_counter(const char* path, const std::string& wanted) {
    std::ifstream input(path); std::string line;
    while (std::getline(input, line)) {
        std::istringstream fields(line); std::string key; std::int64_t value;
        if ((fields >> key >> value) && key == wanted) return value;
    }
    return -1;
}
// Installed after model/audio construction, before any turn can start. Every
// exceptional exit (including logger failure) must invalidate and wait boundedly
// without relying on logging before model destructors can join synchronous SDKs.
class BoundedCleanup {
    RealtimePipeline& pipeline_;
    LiveAudioBackend& audio_;
    int timeout_ms_;
    bool armed_ = true;
public:
    BoundedCleanup(RealtimePipeline& pipeline, LiveAudioBackend& audio, int timeout)
        : pipeline_(pipeline), audio_(audio), timeout_ms_(timeout) {}
    void dismiss() noexcept { armed_ = false; }
    ~BoundedCleanup() noexcept {
        if (!armed_) return;
        try {
            pipeline_.cancel(); audio_.cancel();
            const auto deadline = now_ns() + std::int64_t(timeout_ms_) * 1000000;
            while (true) {
                const auto status = audio_.status();
                if (pipeline_.idle() && (!status.reset_pending || status.state == LiveAudioState::device_wait)) return;
                if (now_ns() >= deadline) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        } catch (...) {}
        std::cerr << "RESTART_REQUIRED: exceptional cleanup did not confirm idle" << std::endl;
        std::_Exit(5);
    }
};
class Signals {
    struct sigaction old_int_{}, old_term_{};
public:
    Signals() {
        signal_requested = 0;
        struct sigaction action{}; action.sa_handler = handle_signal; sigemptyset(&action.sa_mask);
        if(sigaction(SIGINT, &action, &old_int_)) throw ControlError("cannot install SIGINT");
        if(sigaction(SIGTERM, &action, &old_term_)) {
            sigaction(SIGINT, &old_int_, nullptr); throw ControlError("cannot install SIGTERM");
        }
    }
    ~Signals() { sigaction(SIGINT, &old_int_, nullptr); sigaction(SIGTERM, &old_term_, nullptr); }
};
}
int run_realtime_control(const Config& config, const std::string& run_id,
                         const std::filesystem::path& output_root) {
    probe::require_model_validation_lock();
    const auto budget = realtime_queue_budget(config);
    validate_realtime_config(config);
    const Signals signals;
    const auto root = output_root / run_id;
    JsonlLogger logger(root, run_id, {static_cast<std::size_t>(config.logging.max_file_bytes),
                                    static_cast<std::size_t>(config.logging.max_files)}, "realtime");
    const auto initial_memory = memory_available();
    if (initial_memory < 1572864) {
        logger.write("error", "startup_low_memory", 1, 0, "ERROR", {{"mem_available_kib", initial_memory}});
        return 4;
    }
    logger.write("info", "startup", 1, 0, "STARTING",
        {{"backend", "pulse"}, {"models", config.dialogue.wake_enabled ?
             nlohmann::json({"vad", "asr", "llm", "tts", "kws"}) : nlohmann::json({"vad", "asr", "llm", "tts"})},
         {"wake_enabled", config.dialogue.wake_enabled}, {"dialogue_window_ms", config.dialogue.window_ms},
         {"keywords_sha256", config.dialogue.wake_enabled ?
             file_sha256(config.source_path.parent_path() / config.dialogue.keywords_file) : ""},
         {"config_sha256", file_sha256(config.source_path)},
         {"measurement_method", "software_observation"}, {"private_transcripts_logged", false},
         {"queue_budget", {{"capture_audio_frames", budget.capture_audio_frames},
                          {"capture_model_frames", budget.capture_model_frames},
                          {"playback_audio_frames", budget.playback_audio_frames},
                          {"playback_tts_frames", budget.playback_tts_frames},
                          {"playback_retained_frames", budget.playback_retained_frames}}}});
    std::cout << "ENGINE_STARTING run_id=" << run_id << std::endl;
    const auto start = now_ns();
    LiveAudioOptions options;
    options.source = config.audio.source; options.sink = config.audio.sink; options.profile = config.audio.profile;
    options.playback_lead_in_ms = config.audio.playback_lead_in_ms;
    options.playback_keepalive = config.audio.playback_keepalive;
    options.capture_capacity_frames = budget.capture_audio_frames;
    options.playback_capacity_frames = budget.playback_audio_frames;
    // Open the silent keepalive before model initialization so the configured
    // output can resume while models load, instead of at the first answer.
    auto audio = make_pulse_audio(options);
    auto pipeline = make_realtime_pipeline(config);
    BoundedCleanup cleanup(*pipeline, *audio, config.timeouts.cancel_ms);
    logger.write("info", "models_ready", 1, 0, "STARTING", {{"initialization_ms", (now_ns() - start) / 1e6}});
    RealtimeController controller(config, *audio, *pipeline,
        [&](std::string_view event, AudioContext id, RealtimeState state, const nlohmann::json& data) {
            logger.write("info", event, id.session_id, id.generation_id, realtime_state_name(state), data);
            if (event == "wake_listening") std::cout << "WAKE_LISTENING" << std::endl;
            if (event == "wake_detected") std::cout << "WAKE_DETECTED" << std::endl;
            if (event == "state_changed")
                std::cout << "STATE " << realtime_state_name(state) << " session_id=" << id.session_id
                          << " generation_id=" << id.generation_id << std::endl;
        });
    controller.tick(now_ns(), memory_available());
    std::cout << "ENGINE_READY run_id=" << run_id << " state="
              << realtime_state_name(controller.snapshot().state) << " log_dir=" << root.string() << std::endl;
    std::string line; bool oversized = false, input_closed = false;
    std::int64_t mem_sample_ns = 0, resource_sample_ns = 0, available = initial_memory;
    int exit_code = 0;
    auto finish_line = [&] {
        bool accepted = false;
        if (!oversized) accepted = controller.command(line, now_ns());
        const auto status = controller.snapshot();
        if (line == "help") std::cout << "COMMANDS start stop cancel new-session status help quit\n";
        if (line == "status") std::cout << "STATUS state=" << realtime_state_name(status.state)
            << " session_id=" << status.context.session_id << " generation_id=" << status.context.generation_id
            << " wake_listening=" << status.wake_listening << '\n';
        // Never echo unrecognized input, which may be private speech or text.
        std::cout << (accepted ? "OK command" : oversized ? "ERROR line_too_long" : "ERROR command_rejected")
                  << " state=" << realtime_state_name(status.state) << std::endl;
        line.clear(); oversized = false;
    };
    while (!controller.snapshot().stopping) {
        const auto now = now_ns();
        if (signal_requested) {
            exit_code = 128 + signal_requested; input_closed = true; controller.shutdown(now);
        }
        if (now - mem_sample_ns >= 100000000) { available = memory_available(); mem_sample_ns = now; }
        try { controller.tick(now, available); }
        catch (const std::exception& e) {
            logger.write("error", "runtime_error", 1, 0, "ERROR", {{"detail", e.what()}});
            exit_code = 4; input_closed = true; controller.shutdown(now_ns());
        }
        if (controller.snapshot().stopping) break;
        if (now - resource_sample_ns >= 1000000000) {
            resource_sample_ns = now;
            const auto status = audio->status(); const auto snap = controller.snapshot();
            logger.write("info", "resource_sample", snap.context.session_id, snap.context.generation_id,
                realtime_state_name(snap.state), {{"mem_available_kib", available},
                    {"rss_kib", proc_counter("/proc/self/status", "VmRSS:")},
                    {"swap_in_pages", proc_counter("/proc/vmstat", "pswpin")},
                    {"swap_out_pages", proc_counter("/proc/vmstat", "pswpout")},
                    {"capture_queued_frames", status.capture_queued_frames},
                    {"capture_peak_frames", status.capture_queue_peak_frames},
                    {"playback_queued_frames", status.playback_queued_frames},
                    {"playback_peak_frames", status.playback_queue_peak_frames},
                    {"capture_overflows", status.capture_overflows},
                    {"capture_holes", status.capture_holes},
                    {"playback_overflows", status.playback_overflows},
                    {"playback_underruns", status.playback_underruns},
                    {"output_service_max_gap_ns", status.output_service_max_gap_ns},
                    {"capture_service_max_work_ns", status.capture_service_max_work_ns},
                    {"playback_keepalive_active", status.playback_keepalive_active},
                    {"playback_keepalive_native_frames", status.playback_keepalive_native_frames},
                    {"playback_keepalive_underruns", status.playback_keepalive_underruns},
                    {"dropped_events", status.dropped_events},
                    {"models_idle", pipeline->idle()}, {"pipeline", pipeline->diagnostics()}});
        }
        struct pollfd input{input_closed ? -1 : STDIN_FILENO, POLLIN, 0};
        const auto ready = ::poll(&input, 1, 5);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0 || (input.revents & (POLLERR | POLLNVAL))) {
            exit_code = 1; input_closed = true; controller.shutdown(now_ns()); continue;
        }
        if (ready == 0) continue;
        std::array<char, 256> buffer{};
        const auto count = ::read(STDIN_FILENO, buffer.data(), buffer.size());
        if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (count <= 0) {
            if (count == 0 && (!line.empty() || oversized)) finish_line();
            if (count < 0) exit_code = 1;
            input_closed = true; controller.shutdown(now_ns()); continue;
        }
        for (ssize_t n = 0; n < count; ++n) {
            if (buffer[n] == '\n') finish_line();
            else if (!oversized) {
                if (line.size() == 128) { line.clear(); oversized = true; }
                else line.push_back(buffer[n]);
            }
        }
    }
    const auto snap = controller.snapshot();
    if (snap.exit_code) exit_code = snap.exit_code;
    logger.write(exit_code ? "warn" : "info", "shutdown", snap.context.session_id,
                 snap.context.generation_id, realtime_state_name(snap.state),
                 {{"exit_code", exit_code}, {"restart_required", snap.exit_code == 5}});
    std::cout << "BYE exit_code=" << exit_code << std::endl;
    // A timed out synchronous ASR/TTS SDK call cannot safely be destroyed or
    // joined. Terminate this process after flushed evidence; restart explicitly.
    if (snap.exit_code == 5) std::_Exit(5);
    cleanup.dismiss();
    return exit_code;
}
} // namespace voice_engine
