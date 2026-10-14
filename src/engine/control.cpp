#include "voice_engine/control.hpp"
#include "voice_engine/logger.hpp"

#include <array>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <iostream>
#include <string_view>

#include <poll.h>
#include <signal.h>
#include <unistd.h>

namespace voice_engine {
namespace {

volatile sig_atomic_t requested_signal = 0;

void request_shutdown(int signal_number) {
    requested_signal = signal_number;
}

class SignalScope {
public:
    SignalScope() {
        requested_signal = 0;
        struct sigaction action {};
        action.sa_handler = request_shutdown;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGINT, &action, &previous_int_) != 0) {
            throw ControlError("cannot install SIGINT handler");
        }
        if (sigaction(SIGTERM, &action, &previous_term_) != 0) {
            sigaction(SIGINT, &previous_int_, nullptr);
            throw ControlError("cannot install SIGTERM handler");
        }
    }

    ~SignalScope() {
        sigaction(SIGINT, &previous_int_, nullptr);
        sigaction(SIGTERM, &previous_term_, nullptr);
    }

    SignalScope(const SignalScope&) = delete;
    SignalScope& operator=(const SignalScope&) = delete;

private:
    struct sigaction previous_int_ {};
    struct sigaction previous_term_ {};
};

enum class State { Starting, Idle, Listening, Finalizing, Cancelling, Stopping };

std::string_view state_name(State state) {
    switch (state) {
        case State::Starting: return "STARTING";
        case State::Idle: return "IDLE";
        case State::Listening: return "LISTENING";
        case State::Finalizing: return "FINALIZING";
        case State::Cancelling: return "CANCELLING";
        case State::Stopping: return "STOPPING";
    }
    return "ERROR";
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return text;
}

class MockController {
public:
    MockController(JsonlLogger& logger, AudioBackend* audio)
        : logger_(logger), audio_(audio) {
        log("startup", {{"backend", "mock"},
                        {"audio_backend", audio_ != nullptr ? "mock" : "none"},
                        {"asset_checks", "skipped_mock"}});
        transition(State::Idle);
    }

    bool stopped() const { return state_ == State::Stopping; }

    void reject(std::string_view reason) {
        log("command_rejected", {{"reason", reason}}, "warn");
        std::cout << "ERROR " << reason << std::endl;
    }

    void handle(std::string_view input) {
        const auto command = trim(input);
        if (command.empty()) {
            return;
        }
        if (command != "start" && command != "stop" && command != "cancel" &&
            command != "new-session" && command != "quit" && command != "help" &&
            command != "status") {
            reject("unknown_command");
            return;
        }

        log("command_received", {{"command", command}});
        if (command == "start") {
            invalidate_and_cancel();
            if (audio_ != nullptr) {
                audio_->start_capture({session_id_, generation_id_});
            }
            transition(State::Listening);
            if (audio_ != nullptr) {
                const auto frame = audio_->capture_frame();
                log("mock_audio_frame", {{"samples", frame.samples.size()},
                                         {"sample_rate", frame.sample_rate},
                                         {"channels", frame.channels},
                                         {"frame_seq", frame.seq},
                                         {"synthetic", true}});
            }
        } else if (command == "stop") {
            if (state_ != State::Listening) {
                reject("stop_requires_listening");
                return;
            }
            transition(State::Finalizing);
            if (audio_ != nullptr) {
                audio_->stop_capture();
            }
            log("mock_input_finished", {{"inference", "not_run"}});
            transition(State::Idle);
        } else if (command == "cancel") {
            invalidate_and_cancel();
        } else if (command == "new-session") {
            invalidate_and_cancel();
            ++session_id_;
            log("session_started");
        } else if (command == "quit") {
            shutdown("quit");
            return;
        } else if (command == "help") {
            std::cout << "COMMANDS start stop cancel new-session status help quit\n";
        } else if (command == "status") {
            std::cout << "STATUS state=" << state_name(state_)
                      << " session_id=" << session_id_
                      << " generation_id=" << generation_id_ << '\n';
        }

        log("command_completed", {{"command", command}});
        std::cout << "OK " << command << " state=" << state_name(state_)
                  << " session_id=" << session_id_
                  << " generation_id=" << generation_id_ << std::endl;
    }

    void shutdown(std::string_view reason) {
        if (stopped()) {
            return;
        }
        if (state_ != State::Idle) {
            invalidate_and_cancel();
        } else if (audio_ != nullptr) {
            audio_->cancel();
        }
        transition(State::Stopping);
        log("shutdown", {{"reason", reason}});
        std::cout << "BYE reason=" << reason << std::endl;
    }

private:
    void log(std::string_view event,
             const nlohmann::json& data = nlohmann::json::object(),
             std::string_view level = "info") {
        logger_.write(level, event, session_id_, generation_id_, state_name(state_), data);
    }

    void transition(State next) {
        const auto previous = state_;
        state_ = next;
        log("state_changed", {{"from", state_name(previous)}, {"to", state_name(next)}});
    }

    void invalidate_and_cancel() {
        ++generation_id_;
        if (audio_ != nullptr) {
            audio_->cancel();
        }
        if (state_ != State::Idle) {
            transition(State::Cancelling);
            transition(State::Idle);
        }
    }

    JsonlLogger& logger_;
    AudioBackend* audio_;
    State state_ = State::Starting;
    std::uint64_t session_id_ = 1;
    std::uint64_t generation_id_ = 0;
};

}  // namespace

int run_stdin_control(const Config& config, const std::string& run_id,
                      const std::filesystem::path& output_root,
                      AudioBackend* audio) {
    const SignalScope signal_scope;
    const auto run_directory = output_root / run_id;
    JsonlLogger logger(run_directory, run_id,
                       {static_cast<std::size_t>(config.logging.max_file_bytes),
                        static_cast<std::size_t>(config.logging.max_files)});
    MockController controller(logger, audio);
    std::cout << "MOCK_READY run_id=" << run_id << " log_dir="
              << run_directory.string() << "\n"
              << "audio_backend=" << (audio != nullptr ? "mock" : "none")
              << " asset_checks=skipped_mock\n"
              << "No audio or model inference. Type help for commands." << std::endl;

    std::string line;
    line.reserve(128);
    bool oversized_line = false;
    std::array<char, 256> buffer {};
    auto finish_line = [&] {
        if (oversized_line) {
            controller.reject("line_too_long");
        } else {
            controller.handle(line);
        }
        line.clear();
        oversized_line = false;
    };

    try {
        while (!controller.stopped()) {
            if (requested_signal != 0) {
                const auto signal_number = requested_signal;
                controller.shutdown(signal_number == SIGINT ? "sigint" : "sigterm");
                return 128 + signal_number;
            }
            struct pollfd input {STDIN_FILENO, POLLIN, 0};
            const auto ready = poll(&input, 1, 100);
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw ControlError("cannot poll terminal input");
            }
            if (ready == 0) {
                continue;
            }
            if ((input.revents & (POLLERR | POLLNVAL)) != 0) {
                throw ControlError("terminal input is unavailable");
            }
            const auto count = read(STDIN_FILENO, buffer.data(), buffer.size());
            if (count < 0) {
                if (errno == EINTR || errno == EAGAIN) {
                    continue;
                }
                throw ControlError("cannot read terminal input");
            }
            if (count == 0) {
                if (!line.empty() || oversized_line) {
                    finish_line();
                }
                controller.shutdown("eof");
                return 0;
            }
            for (ssize_t index = 0; index < count && !controller.stopped(); ++index) {
                const auto character = buffer[static_cast<std::size_t>(index)];
                if (character == '\n') {
                    finish_line();
                } else if (!oversized_line) {
                    if (line.size() == 128) {
                        line.clear();
                        oversized_line = true;
                    } else {
                        line.push_back(character);
                    }
                }
            }
        }
    } catch (const ControlError&) {
        controller.shutdown("input_error");
        throw;
    } catch (const AudioError&) {
        controller.shutdown("audio_error");
        throw;
    }
    return 0;
}

}  // namespace voice_engine
