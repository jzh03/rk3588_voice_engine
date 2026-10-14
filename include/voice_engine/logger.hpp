#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace voice_engine {

struct LogLimits {
    std::size_t max_file_bytes = 10 * 1024 * 1024;
    std::size_t max_files = 10;
};

class LogError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Each logger owns a new run directory; an existing run is never reused.
class JsonlLogger {
public:
    JsonlLogger(const std::filesystem::path& run_dir, std::string run_id,
                LogLimits limits = {});
    ~JsonlLogger() noexcept;

    JsonlLogger(const JsonlLogger&) = delete;
    JsonlLogger& operator=(const JsonlLogger&) = delete;

    void write(std::string_view level, std::string_view event,
               std::uint64_t session_id, std::uint64_t generation_id,
               std::string_view state,
               const nlohmann::json& data = nlohmann::json::object());

private:
    std::filesystem::path log_path(std::size_t index) const;
    void open_current();
    void rotate();

    std::filesystem::path run_dir_;
    std::string run_id_;
    LogLimits limits_;
    std::ofstream stream_;
    std::size_t current_bytes_ = 0;
    std::uint64_t sequence_ = 0;
    bool failed_ = false;
    std::mutex mutex_;
};

}  // namespace voice_engine
