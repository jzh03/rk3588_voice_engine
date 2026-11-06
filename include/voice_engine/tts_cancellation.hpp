#pragma once
#include "voice_engine/cancellation.hpp"
#include <cstdint>

namespace voice_engine {
// One token per TTS generation.
class TtsCancellation : public Cancellation {
public:
    // Native adapter observations. Nonzero only while the synchronous Run is
    // owned by this generation; they do not claim acoustic or kernel timing.
    std::atomic<std::int64_t> native_run_started_ns{0};
    std::atomic<std::uint64_t> native_runs{0};
};
} // namespace voice_engine
