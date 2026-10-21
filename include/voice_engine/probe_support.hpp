#pragma once

#include <filesystem>

namespace voice_engine::probe {

inline constexpr const char* kModelValidationLockPath =
    "/tmp/rk3588_voice_engine_model_validation.lock";

// Throws ConfigError unless the current process or a live flock ancestor owns
// an exclusive flock on this exact device/inode. Another validation process's
// lock does not authorize this process. No model is loaded by this check.
void require_model_validation_lock();

// Path-parameterized check for the diagnostic's isolated lock regression tests.
// Model adapters must call require_model_validation_lock(), not this overload.
void require_owned_flock(const std::filesystem::path& path);

}  // namespace voice_engine::probe
