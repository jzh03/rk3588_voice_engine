#include "voice_engine/assets.hpp"
#include "voice_engine/audio_dsp.hpp"
#include "voice_engine/config.hpp"
#include "voice_engine/live_audio.hpp"
#include "voice_engine/logger.hpp"

#include <sndfile.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <poll.h>
#include <set>
#include <string>
#include <thread>
#include <unistd.h>

namespace {
namespace ve = voice_engine;
namespace fs = std::filesystem;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
volatile std::sig_atomic_t received_signal = 0;

extern "C" void handle_signal(int number) { received_signal = number; }

struct Arguments {
    std::string config;
    std::string mode;
    fs::path output = "output/runs";
    fs::path play_wav;
    std::string run_id;
    int duration_sec = 10;
    int wait_device_sec = 10;
    bool control = false;
    bool record_wav = false;
};

void usage(std::ostream& output) {
    output << "Usage: audio_probe --config FILE (--duplex|--capture-only|--playback-only)\n"
              "                   [--duration-sec N] [--wait-device-sec N]\n"
              "                   [--control stdin] [--record-wav] [--play-wav FILE]\n"
              "                   [--run-id ID] [--output-dir ROOT]\n"
              "       audio_probe --help\n\n"
              "Explicitly binds the configured source/sink and checks the active profile.\n"
              "No model is loaded and no device/profile/default is changed.\n"
              "Playback emits a quiet 440 Hz beep each five seconds, or reads --play-wav.\n"
              "Capture stays in memory unless --record-wav saves run/capture.wav.\n"
              "Duration: 1..3600 s (default 10); initial device wait: 1..60 s (default 10).\n"
              "With --control stdin: starts idle; commands start stop cancel status help quit.\n"
              "After a device reconnect, an explicit start is required.\n"
              "Reports: ROOT/ID/{run.json,events.jsonl,summary.json}; ID must be new.\n"
              "Exit: 0 normal, 1 I/O/runtime, 2 CLI/config/WAV, 3 device unavailable,\n"
              "      5 drain timeout, 6 software audio errors, 130 SIGINT, 143 SIGTERM.\n"
              "A01/A02 require physical-device and human acceptance; reports keep them not_run.\n";
}

bool valid_id(const std::string& id) {
    const auto alnum = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9');
    };
    return !id.empty() && id.size() <= 64 && alnum(id.front()) &&
           std::all_of(id.begin(), id.end(), [&](char c) {
               return alnum(c) || c == '-' || c == '_' || c == '.';
           });
}

int integer(const std::string& text, int maximum, const std::string& name) {
    int value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        value < 1 || value > maximum) {
        throw ve::ConfigError(name + " must be an integer in 1.." + std::to_string(maximum));
    }
    return value;
}

Arguments parse(int argc, char** argv) {
    Arguments result;
    std::set<std::string> seen;
    for (int i = 1; i < argc; ++i) {
        const std::string option = argv[i];
        if (!seen.insert(option).second) throw ve::ConfigError("duplicate option: " + option);
        if (option == "--duplex" || option == "--capture-only" || option == "--playback-only") {
            if (!result.mode.empty()) throw ve::ConfigError("choose exactly one audio mode");
            result.mode = option.substr(2);
        } else if (option == "--record-wav") {
            result.record_wav = true;
        } else if (option == "--config" || option == "--duration-sec" ||
                   option == "--wait-device-sec" || option == "--control" ||
                   option == "--run-id" || option == "--output-dir" || option == "--play-wav") {
            if (++i >= argc || !*argv[i] || std::string(argv[i]).rfind("--", 0) == 0)
                throw ve::ConfigError(option + " needs a value");
            const std::string value = argv[i];
            if (option == "--config") result.config = value;
            else if (option == "--duration-sec") result.duration_sec = integer(value, 3600, option);
            else if (option == "--wait-device-sec") result.wait_device_sec = integer(value, 60, option);
            else if (option == "--run-id") result.run_id = value;
            else if (option == "--output-dir") result.output = value;
            else if (option == "--play-wav") result.play_wav = value;
            else {
                if (value != "stdin") throw ve::ConfigError("--control must be stdin");
                result.control = true;
            }
        } else throw ve::ConfigError("unknown option: " + option);
    }
    if (result.config.empty() || result.mode.empty())
        throw ve::ConfigError("--config and one audio mode are required");
    if (result.record_wav && result.mode == "playback-only")
        throw ve::ConfigError("--record-wav requires capture");
    if (!result.play_wav.empty() && result.mode == "capture-only")
        throw ve::ConfigError("--play-wav requires playback");
    if (result.run_id.empty()) {
        const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        result.run_id = "audio-" + std::to_string(stamp) + "-" + std::to_string(getpid());
    }
    if (!valid_id(result.run_id)) throw ve::ConfigError("invalid --run-id");
    return result;
}

void write_json(const fs::path& path, const Json& value) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << value.dump(2) << '\n';
    output.close();
    if (!output) throw ve::AudioError("cannot write report: " + path.string());
}

struct SndfileCloser { void operator()(SNDFILE* file) const { if (file) sf_close(file); } };
using SoundFile = std::unique_ptr<SNDFILE, SndfileCloser>;

class WavPlayer {
public:
    explicit WavPlayer(const fs::path& path) {
        std::error_code error;
        if (!fs::is_regular_file(path, error) || error || fs::file_size(path) > 500U * 1024U * 1024U)
            throw ve::ConfigError("playback WAV must be a regular file no larger than 500 MiB");
        file_.reset(sf_open(path.c_str(), SFM_READ, &info_));
        if (!file_ || (info_.format & SF_FORMAT_TYPEMASK) != SF_FORMAT_WAV || info_.frames <= 0 ||
            info_.samplerate < 8000 || info_.samplerate > 192000 || info_.channels < 1 || info_.channels > 8)
            throw ve::ConfigError("playback needs a nonempty WAV, 8..192 kHz, 1..8 channels");
        converter_ = std::make_unique<ve::StreamResampler>(info_.samplerate, 16000, info_.channels, 1);
    }

    void reset() {
        if (sf_seek(file_.get(), 0, SEEK_SET) != 0) throw ve::AudioError("cannot rewind playback WAV");
        converter_->reset();
        assembler_.reset();
        pending_.clear();
        ended_ = false;
    }

    std::optional<ve::PaddedAudioFrame> next() {
        while (pending_.empty() && !ended_) {
            const std::size_t frames = static_cast<std::size_t>(info_.samplerate / 50);
            std::vector<float> input(frames * static_cast<std::size_t>(info_.channels));
            const auto count = sf_readf_float(file_.get(), input.data(), static_cast<sf_count_t>(frames));
            if (sf_error(file_.get()) != SF_ERR_NO_ERROR) throw ve::AudioError("playback WAV read failed");
            if (count > 0) append(converter_->process(input.data(), static_cast<std::size_t>(count)));
            else {
                append(converter_->finish());
                if (auto tail = assembler_.finish()) pending_.push_back(*tail);
                ended_ = true;
            }
        }
        if (pending_.empty()) return std::nullopt;
        auto frame = pending_.front();
        pending_.pop_front();
        return frame;
    }

private:
    void append(const std::vector<float>& samples) {
        for (const auto& frame : assembler_.push(samples.data(), samples.size()))
            pending_.push_back({frame,320});
    }
    SF_INFO info_{};
    SoundFile file_;
    std::unique_ptr<ve::StreamResampler> converter_;
    ve::MonoFrameAssembler assembler_;
    std::deque<ve::PaddedAudioFrame> pending_;
    bool ended_ = false;
};

Json buffer_json(const ve::LiveAudioBuffer& value) {
    return {{"maxlength",value.maxlength},{"tlength",value.tlength},{"prebuf",value.prebuf},
            {"minreq",value.minreq},{"fragsize",value.fragsize}};
}

Json status_json(const ve::LiveAudioStatus& s) {
    return {{"state",ve::live_audio_state_name(s.state)},{"session_active",s.session_active},
        {"capturing",s.capturing},{"capture_stopping",s.capture_stopping},
        {"reset_pending",s.reset_pending},{"drained",s.drained},
        {"source",s.source},{"sink",s.sink},{"card",s.card},{"profile",s.profile},
        {"source_identity",s.source_identity},{"sink_identity",s.sink_identity},
        {"capture_device_format",s.capture_device_format},{"playback_device_format",s.playback_device_format},
        {"capture_rate",s.capture_rate},{"capture_channels",s.capture_channels},
        {"playback_rate",s.playback_rate},{"playback_channels",s.playback_channels},
        {"capture_buffer",buffer_json(s.capture_buffer)},{"playback_buffer",buffer_json(s.playback_buffer)},
        {"capture_queued_frames",s.capture_queued_frames},{"playback_queued_frames",s.playback_queued_frames},
        {"capture_queue_peak_frames",s.capture_queue_peak_frames},
        {"playback_queue_peak_frames",s.playback_queue_peak_frames},
        {"raw_capture_peak_bytes",s.raw_capture_peak_bytes},{"capture_native_frames",s.capture_native_frames},
        {"device_available_ns",s.device_available_ns},{"ready_ns",s.ready_ns},
        {"capture_frames",s.capture_frames},{"playback_frames",s.playback_frames},
        {"capture_overflows",s.capture_overflows},{"capture_holes",s.capture_holes},
        {"playback_overflows",s.playback_overflows},{"playback_underruns",s.playback_underruns},
        {"output_service_max_gap_ns",s.output_service_max_gap_ns},
        {"capture_service_max_work_ns",s.capture_service_max_work_ns},
        {"playback_keepalive_buffer",buffer_json(s.playback_keepalive_buffer)},
        {"playback_keepalive_active",s.playback_keepalive_active},
        {"playback_keepalive_native_frames",s.playback_keepalive_native_frames},
        {"playback_keepalive_underruns",s.playback_keepalive_underruns},
        {"stale_rejections",s.stale_rejections},{"reconnects",s.reconnects},{"dropped_events",s.dropped_events}};
}

int run(const Arguments& args) {
    const auto config = ve::load_config(args.config);
    const bool capture = args.mode != "playback-only";
    const bool playback = args.mode != "capture-only";
    std::unique_ptr<WavPlayer> player;
    if (!args.play_wav.empty()) player = std::make_unique<WavPlayer>(args.play_wav);
    const auto run_dir = args.output / args.run_id;
    ve::JsonlLogger logger(run_dir, args.run_id,
        {static_cast<std::size_t>(config.logging.max_file_bytes),static_cast<std::size_t>(config.logging.max_files)},
        "audio_probe");
    const auto binary = fs::read_symlink("/proc/self/exe");
    Json metadata = {{"run_id",args.run_id},{"mode",args.mode},
        {"config_path",config.source_path.string()},{"config_sha256",ve::file_sha256(config.source_path)},
        {"binary_path",binary.string()},{"binary_sha256",ve::file_sha256(binary)},
        {"build_date",__DATE__},{"build_time",__TIME__},{"compiler",__VERSION__},
        {"duration_sec",args.duration_sec},{"wait_device_sec",args.wait_device_sec},
        {"control_stdin",args.control},{"record_wav",args.record_wav},
        {"play_wav",args.play_wav.empty()?Json(nullptr):Json(args.play_wav.string())},
        {"source",config.audio.source},{"sink",config.audio.sink},{"profile",config.audio.profile},
        {"playback_lead_in_ms",config.audio.playback_lead_in_ms},
        {"playback_keepalive",config.audio.playback_keepalive},
        {"internal_format",{{"rate",16000},{"channels",1},{"samples_per_frame",320},{"sample_type","float32_normalized"}}},
        {"models_loaded",false}};
    if (player) metadata["play_wav_sha256"] = ve::file_sha256(args.play_wav);
    write_json(run_dir / "run.json", metadata);
    SoundFile recording;
    if (args.record_wav) {
        SF_INFO info{};
        info.samplerate = 16000; info.channels = 1; info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
        recording.reset(sf_open((run_dir / "capture.wav").c_str(), SFM_WRITE, &info));
        if (!recording) throw ve::AudioError("cannot create capture.wav");
    }
    ve::LiveAudioOptions options;
    options.source = config.audio.source; options.sink = config.audio.sink; options.profile = config.audio.profile;
    options.playback_lead_in_ms = config.audio.playback_lead_in_ms;
    options.playback_keepalive = config.audio.playback_keepalive;
    options.capture_enabled = capture; options.playback_enabled = playback;
    options.capture_capacity_frames = static_cast<std::size_t>(config.queues.capture_ms / config.audio.frame_ms);
    options.playback_capacity_frames = static_cast<std::size_t>(config.queues.playback_ms / config.audio.frame_ms);
    auto audio = ve::make_pulse_audio(options);
    ve::AudioContext context{1,0};
    auto log = [&](const std::string& event, const Json& data = Json::object()) {
        logger.write("info",event,context.session_id,context.generation_id,
                     ve::live_audio_state_name(audio->status().state),data);
    };
    log("startup",{{"audio_backend","pulse"},{"probe_mode",args.mode},{"record_wav",args.record_wav}});
    std::cout << "AUDIO_WAIT run_id=" << args.run_id << " output=" << run_dir.string() << std::endl;
    const auto entered = Clock::now();
    auto previous = entered;
    auto next_metrics = entered;
    std::optional<Clock::time_point> ready_at;
    std::optional<Clock::time_point> produce_at;
    std::optional<Clock::time_point> drain_at;
    bool previous_capturing = false;
    bool previous_ready = false;
    bool previous_active = false;
    bool producing = false;
    bool stopping = false;
    bool quitting = false;
    bool initial_started = false;
    bool device_lost = false;
    bool eof = false;
    bool oversized_input = false;
    std::string command;
    std::optional<ve::AudioFrame> pending_playback;
    std::uint64_t playback_sequence = 0;
    std::uint64_t capture_frames = 0;
    std::uint64_t capture_samples = 0;
    std::uint64_t invalid_capture_frames = 0;
    std::uint64_t discarded_playback_frames = 0;
    std::uint64_t source_frame_index = 0;
    std::uint64_t last_capture_seq = 0;
    std::int64_t last_capture_time = 0;
    double active_capture_seconds = 0;
    double squares = 0;
    double peak = 0;
    int exit_code = 0;
    std::string reason = "duration_complete";
    ve::LiveAudioStatus final_status;

    const auto consume_capture = [&] {
        ve::AudioFrame frame;
        while (audio->try_capture_frame(frame)) {
            if (frame.context.session_id != context.session_id || frame.context.generation_id != context.generation_id ||
                frame.sample_rate != 16000 || frame.channels != 1 || frame.samples.size() != 320 ||
                frame.valid_samples < 1 || frame.valid_samples > 320 ||
                frame.seq <= last_capture_seq || frame.monotonic_ns < last_capture_time) {
                ++invalid_capture_frames;
                throw ve::AudioError("invalid or stale capture frame");
            }
            last_capture_seq=frame.seq; last_capture_time=frame.monotonic_ns;
            ++capture_frames; capture_samples += frame.valid_samples;
            for (std::size_t i=0; i<frame.valid_samples; ++i) {
                const auto sample=frame.samples[i];
                if (!std::isfinite(sample) || std::abs(sample) > 1.0F)
                    throw ve::AudioError("invalid capture amplitude");
                squares += static_cast<double>(sample)*sample;
                peak = std::max(peak,std::abs(static_cast<double>(sample)));
            }
            if (recording && sf_writef_float(recording.get(),frame.samples.data(),
                    static_cast<sf_count_t>(frame.valid_samples)) != static_cast<sf_count_t>(frame.valid_samples))
                throw ve::AudioError("capture.wav write failed");
        }
    };

    const auto start = [&] {
        const auto before = audio->status();
        if (before.state == ve::LiveAudioState::device_wait) {
            log("command_rejected",{{"reason","device_not_ready"}});
            return;
        }
        const ve::AudioContext proposed{context.session_id,context.generation_id+1};
        if (!audio->start_session(proposed,capture)) {
            log("command_rejected",{{"reason","session_not_ready"}});
            return;
        }
        context = proposed;
        previous_active = false;
        if (player) player->reset();
        source_frame_index = 0; playback_sequence = 0; last_capture_seq = 0; last_capture_time = 0;
        pending_playback.reset(); produce_at.reset(); drain_at.reset();
        producing = playback; stopping = false; initial_started = true;
        log("session_start",{{"capture",capture},{"playback",playback}});
    };
    const auto cancel = [&] {
        audio->cancel();
        previous_active = false;
        ++context.generation_id;
        producing = false; stopping = false; pending_playback.reset(); produce_at.reset(); drain_at.reset();
        log("session_cancel");
    };
    const auto process_command = [&](const std::string& text, bool too_long) {
        if (too_long) { log("command_rejected",{{"reason","input_too_long"}}); return; }
        if (text == "start") start();
        else if (text == "stop") {
            audio->stop_capture(); producing = false; pending_playback.reset();
            audio->request_drain(); drain_at = Clock::now(); stopping = true;
            log("session_stop");
        } else if (text == "cancel") cancel();
        else if (text == "status") {
            const auto status = status_json(audio->status()); log("status",status);
            std::cout << "AUDIO_STATUS " << status.dump() << std::endl;
        } else if (text == "help") std::cout << "Commands: start stop cancel status help quit" << std::endl;
        else if (text == "quit") { reason="quit"; quitting=true; }
        else if (!text.empty()) log("command_rejected",{{"reason","unknown_command"}});
    };

    while (!quitting) {
        const auto now = Clock::now();
        if (previous_capturing) active_capture_seconds += std::chrono::duration<double>(now-previous).count();
        previous = now;
        for (const auto& event : audio->poll_events()) {
            logger.write("info",event.type,event.context.session_id,event.context.generation_id,
                ve::live_audio_state_name(audio->status().state),
                {{"detail",event.detail},{"backend_monotonic_ns",event.monotonic_ns}});
        }
        auto status = audio->status();
        const bool available = status.state != ve::LiveAudioState::device_wait;
        if (available && !previous_ready) {
            if (!ready_at) ready_at = now;
            log("probe_ready",status_json(status));
            std::cout << "AUDIO_READY run_id=" << args.run_id << " state="
                      << ve::live_audio_state_name(status.state) << std::endl;
            if (!args.control && !initial_started) start();
        }
        if (!available && previous_ready) {
            device_lost = true;
            ++context.generation_id;
            producing = false; stopping = false; pending_playback.reset(); produce_at.reset(); drain_at.reset();
            log("probe_device_wait");
            std::cout << "AUDIO_DEVICE_WAIT reconnect_then_start_required" << std::endl;
        }
        previous_ready = available;
        status = audio->status();
        if (status.session_active && !status.reset_pending && !previous_active) {
            log("session_ready");
            std::cout << "AUDIO_SESSION_STARTED generation=" << context.generation_id << std::endl;
        }
        previous_active = status.session_active && !status.reset_pending;
        previous_capturing = status.capturing && !status.reset_pending;
        consume_capture();
        if (producing && status.session_active && !status.reset_pending) {
            if (!produce_at) produce_at = now;
            const auto elapsed = std::chrono::duration<double>(now-*produce_at).count();
            const auto due = static_cast<std::uint64_t>(elapsed*50.0)+6;
            while (source_frame_index < due && source_frame_index < static_cast<std::uint64_t>(args.duration_sec)*50) {
                if (!pending_playback) {
                    ve::AudioFrame output;
                    output.context=context; output.seq=++playback_sequence;
                    output.monotonic_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
                    if (player) {
                        auto next = player->next();
                        if (!next) {
                            producing=false; stopping=true;
                            if (capture) audio->stop_capture();
                            audio->request_drain(); drain_at=now;
                            log("playback_file_end"); break;
                        }
                        output.samples.assign(next->samples.begin(),next->samples.end());
                        output.valid_samples=next->valid_samples;
                    } else {
                        output.samples.resize(320);
                        for (std::size_t i=0;i<320;++i) {
                            const auto sample_index=source_frame_index*320+i;
                            const auto phase=sample_index%80000;
                            // 300 ms beep with a 10 ms fade at each edge.
                            const double envelope=phase<4800 ? std::min({1.0,phase/160.0,(4800-phase)/160.0}) : 0.0;
                            output.samples[i]=static_cast<float>(0.04*envelope*std::sin(2.0*3.14159265358979323846*440.0*sample_index/16000.0));
                        }
                    }
                    pending_playback=std::move(output);
                }
                if (!audio->try_submit_playback(*pending_playback)) break;
                pending_playback.reset(); ++source_frame_index;
            }
        }
        if (stopping && audio->playback_drained() && !audio->status().capture_stopping) {
            consume_capture();
            log("playback_drain_complete"); audio->cancel(); stopping=false; drain_at.reset();
            if (player && !args.control) { reason="playback_file_complete"; quitting=true; }
        }
        if (drain_at && now-*drain_at>std::chrono::seconds(3)) {
            reason="drain_timeout"; exit_code=5; break;
        }
        if (!ready_at && now-entered>=std::chrono::seconds(args.wait_device_sec)) {
            reason="device_unavailable"; exit_code=3; break;
        }
        if (ready_at && now-*ready_at>=std::chrono::seconds(args.duration_sec)) break;
        if (received_signal) {
            exit_code=received_signal==SIGINT?130:143; reason="signal"; break;
        }
        if (args.control && !eof) {
            pollfd input{STDIN_FILENO,POLLIN,0};
            const auto result=poll(&input,1,0);
            if (result<0 && errno!=EINTR) throw ve::AudioError("stdin poll failed");
            if (result>0 && (input.revents&(POLLIN|POLLHUP|POLLERR|POLLNVAL))) {
                if (input.revents&(POLLERR|POLLNVAL)) throw ve::AudioError("stdin became unavailable");
                std::array<char,256> buffer{};
                const auto count=read(STDIN_FILENO,buffer.data(),buffer.size());
                if (count<0 && errno!=EINTR && errno!=EAGAIN) throw ve::AudioError("stdin read failed");
                if (count==0) { eof=true; reason="eof"; quitting=true; }
                for (ssize_t i=0;i<std::max<ssize_t>(count,0);++i) {
                    if (buffer[static_cast<std::size_t>(i)]=='\n') {
                        if (!command.empty() && command.back()=='\r') command.pop_back();
                        process_command(command,oversized_input); command.clear(); oversized_input=false;
                    } else if (command.size()<128) command.push_back(buffer[static_cast<std::size_t>(i)]);
                    else oversized_input=true;
                    if (quitting) break;
                }
            }
        }
        if (now>=next_metrics) {
            log("audio_metrics",status_json(audio->status()));
            next_metrics=now+std::chrono::seconds(1);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!exit_code && audio->status().session_active) {
        if (capture) audio->stop_capture();
        if (playback) audio->request_drain();
        const auto deadline=Clock::now()+std::chrono::seconds(3);
        while ((!audio->playback_drained() || audio->status().capture_stopping) &&
                Clock::now()<deadline && !received_signal) {
            consume_capture();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        consume_capture();
        if (!audio->playback_drained() || audio->status().capture_stopping) {
            exit_code=5; reason="drain_timeout";
        }
    }
    final_status=audio->status();
    discarded_playback_frames=final_status.playback_queued_frames+(pending_playback?1:0);
    audio->cancel();
    const auto cancel_deadline=Clock::now()+std::chrono::seconds(2);
    while (audio->status().reset_pending && Clock::now()<cancel_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (audio->status().reset_pending && !exit_code) { exit_code=5; reason="cancel_flush_timeout"; }
    for (const auto& event : audio->poll_events())
        logger.write("info",event.type,event.context.session_id,event.context.generation_id,
            ve::live_audio_state_name(audio->status().state),{{"detail",event.detail},{"backend_monotonic_ns",event.monotonic_ns}});
    if (recording) {
        auto* file=recording.release();
        if (sf_close(file)!=0) throw ve::AudioError("capture.wav finalization failed");
    }
    const double duration_error=active_capture_seconds>0 ?
        std::abs(static_cast<double>(capture_samples)/16000-active_capture_seconds)/active_capture_seconds : 0;
    const double requested_duration_error=capture ?
        std::abs(static_cast<double>(capture_samples)/16000-args.duration_sec)/args.duration_sec : 0;
    const bool counters_ok=!final_status.capture_overflows && !final_status.capture_holes &&
        !final_status.playback_overflows && !final_status.playback_underruns && !final_status.dropped_events &&
        !final_status.playback_keepalive_underruns &&
        !final_status.stale_rejections && !invalid_capture_frames;
    if (!exit_code && !args.control && initial_started &&
        (device_lost || final_status.state == ve::LiveAudioState::device_wait)) {
        exit_code=3; reason="device_disconnected";
    }
    if (!exit_code && (!counters_ok || (!args.control && capture && !capture_samples) ||
        (!args.control && playback && !final_status.playback_frames) ||
        (!args.control && capture && !player && args.duration_sec>=600 &&
            (duration_error>0.01 || requested_duration_error>0.01)))) {
        exit_code=6; reason="software_audio_check_failed";
    }
    if (received_signal) { exit_code=received_signal==SIGINT?130:143; reason="signal"; }
    Json summary={{"run_id",args.run_id},{"mode",args.mode},{"exit_code",exit_code},{"reason",reason},
        {"capture_frames_consumed",capture_frames},{"capture_samples",capture_samples},
        {"active_capture_seconds",active_capture_seconds},{"capture_duration_relative_error",duration_error},
        {"requested_capture_duration_relative_error",requested_duration_error},{"device_lost",device_lost},
        {"rms",capture_samples?std::sqrt(squares/capture_samples):0.0},{"peak",peak},
        {"invalid_capture_frames",invalid_capture_frames},{"discarded_playback_frames",discarded_playback_frames},
        {"status",status_json(final_status)},{"software_counters_ok",counters_ok},
        {"record_wav",args.record_wav},{"recording_concatenates_sessions",args.control && args.record_wav},
        {"acceptance",{{"A01","not_run"},{"A02","not_run"},{"requires_physical_device_review",true}}}};
    log("shutdown",{{"reason",reason},{"exit_code",exit_code}});
    write_json(run_dir/"summary.json",summary);
    std::cout << "AUDIO_PROBE_DONE exit_code=" << exit_code << " reason=" << reason
              << " summary=" << (run_dir/"summary.json").string() << std::endl;
    return exit_code;
}
}  // namespace

int main(int argc,char** argv) {
    for (int i=1;i<argc;++i) if (std::string(argv[i])=="--help") { usage(std::cout); return 0; }
    std::signal(SIGINT,handle_signal); std::signal(SIGTERM,handle_signal);
    try { return run(parse(argc,argv)); }
    catch (const ve::ConfigError& error) { std::cerr << "CONFIG_ERROR: " << error.what() << '\n'; return 2; }
    catch (const std::exception& error) { std::cerr << "AUDIO_ERROR: " << error.what() << '\n'; return 1; }
}
