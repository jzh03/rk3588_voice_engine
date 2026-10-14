#pragma once

#include <cstddef>

#include "voice_engine/config.hpp"

namespace voice_engine {

struct AssetCheckResult {
    std::size_t models = 0;
    std::size_t dependencies = 0;
    std::size_t files = 0;
};

// Checks only enabled model references and their declared dependencies. Manifest
// paths are relative to the configuration file; asset paths are relative to their
// manifest directory. No model is executed and no network access is performed.
// Throws ConfigError for invalid metadata or missing/mismatched local assets.
AssetCheckResult validate_assets(const Config& config);

}  // namespace voice_engine
