#include "voice_engine/logger.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <utility>

namespace voice_engine {
namespace {

std::string timestamp_utc() {
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(now);
    const auto millis =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - seconds).count();
    const auto time = std::chrono::system_clock::to_time_t(seconds);
    std::tm utc{};
#if defined(_WIN32)
    if (gmtime_s(&utc, &time) != 0) {
#else
    if (gmtime_r(&time, &utc) == nullptr) {
#endif
        throw LogError("cannot format the log timestamp");
    }
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.'
           << std::setfill('0') << std::setw(3) << millis << 'Z';
    if (!output) {
        throw LogError("cannot format the log timestamp");
    }
    return output.str();
}

}  // namespace

JsonlLogger::JsonlLogger(const std::filesystem::path& run_dir, std::string run_id,
                         LogLimits limits, std::string mode)
    : run_dir_(run_dir.lexically_normal()), run_id_(std::move(run_id)),
      mode_(std::move(mode)), limits_(limits) {
    if (mode_ != "mock" && mode_ != "audio_probe" && mode_ != "realtime") {
        throw LogError("unsupported JSONL run mode");
    }
    if (limits_.max_file_bytes == 0 || limits_.max_files == 0) {
        throw LogError("log byte and file limits must be greater than zero");
    }
    // A trailing separator must not make parent creation create the run itself.
    if (run_dir_.filename().empty() && run_dir_.has_relative_path()) {
        run_dir_ = run_dir_.parent_path();
    }
    try {
        if (!run_dir_.parent_path().empty()) {
            std::filesystem::create_directories(run_dir_.parent_path());
        }
        if (!std::filesystem::create_directory(run_dir_)) {
            throw LogError("log run directory already exists; use a new run ID");
        }
        open_current();
    } catch (const std::filesystem::filesystem_error& error) {
        throw LogError(std::string("cannot initialize the log directory: ") +
                       error.what());
    }
}

JsonlLogger::~JsonlLogger() noexcept = default;

std::filesystem::path JsonlLogger::log_path(std::size_t index) const {
    if (index == 0) {
        return run_dir_ / "events.jsonl";
    }
    return run_dir_ / ("events." + std::to_string(index) + ".jsonl");
}

void JsonlLogger::open_current() {
    stream_.clear();
    stream_.open(log_path(0), std::ios::out | std::ios::binary | std::ios::trunc);
    if (!stream_) {
        throw LogError("cannot open the current JSONL log file");
    }
    current_bytes_ = 0;
}

void JsonlLogger::rotate() {
    stream_.close();
    if (!stream_) {
        throw LogError("cannot close the JSONL log file during rotation");
    }
    if (limits_.max_files > 1) {
        std::filesystem::remove(log_path(limits_.max_files - 1));
        for (std::size_t index = limits_.max_files - 1; index > 0; --index) {
            const auto source = log_path(index - 1);
            if (std::filesystem::exists(source)) {
                std::filesystem::rename(source, log_path(index));
            }
        }
    }
    open_current();
}

void JsonlLogger::write(std::string_view level, std::string_view event,
                        std::uint64_t session_id, std::uint64_t generation_id,
                        std::string_view state, const nlohmann::json& data) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (failed_) {
        throw LogError("the JSONL logger stopped after an earlier I/O failure");
    }
    if (!data.is_object()) {
        throw LogError("JSONL log data must be an object");
    }
    if (sequence_ == std::numeric_limits<std::uint64_t>::max()) {
        throw LogError("JSONL log sequence limit reached");
    }
    std::string line;
    try {
        const auto monotonic_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now().time_since_epoch())
                                      .count();
        const nlohmann::json record = {
            {"timestamp_utc", timestamp_utc()},
            {"monotonic_ns", monotonic_ns},
            {"run_id", run_id_},
            {"seq", sequence_ + 1},
            {"level", std::string(level)},
            {"event", std::string(event)},
            {"session_id", session_id},
            {"generation_id", generation_id},
            {"state", std::string(state)},
            {"mode", mode_},
            {"data", data},
        };
        line = record.dump();
        line.push_back('\n');
    } catch (const nlohmann::json::exception& error) {
        throw LogError(std::string("cannot serialize the JSONL record: ") +
                       error.what());
    }
    if (line.size() > limits_.max_file_bytes ||
        line.size() > static_cast<std::size_t>(
                          std::numeric_limits<std::streamsize>::max())) {
        throw LogError("a JSONL record exceeds the configured file byte limit");
    }
    try {
        if (line.size() > limits_.max_file_bytes - current_bytes_) {
            rotate();
        }
        stream_.write(line.data(), static_cast<std::streamsize>(line.size()));
        stream_.flush();
        if (!stream_) {
            throw LogError("cannot write and flush the JSONL record");
        }
        current_bytes_ += line.size();
        ++sequence_;
    } catch (const std::filesystem::filesystem_error& error) {
        failed_ = true;
        throw LogError(std::string("cannot rotate the JSONL log files: ") +
                       error.what());
    } catch (const LogError&) {
        failed_ = true;
        throw;
    }
}

}  // namespace voice_engine
