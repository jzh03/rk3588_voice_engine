#pragma once

#include "voice_engine/config.hpp"
#include "voice_engine/audio.hpp"

#include <filesystem>
#include <stdexcept>
#include <string>

namespace voice_engine {

class ControlError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

int run_stdin_control(const Config& config, const std::string& run_id,
                      const std::filesystem::path& output_root,
                      AudioBackend* audio = nullptr);

}  // namespace voice_engine
