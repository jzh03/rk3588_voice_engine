#pragma once
#include "voice_engine/audio.hpp"
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace voice_engine {
struct BargeInSignal {
    AudioContext context;
    std::int64_t detected_ns = 0;
    std::uint64_t last_frame_seq = 0, input_samples = 0;
    std::string error;
};
// One CPU detector owner. Its single input slot is included in the shared
// capture budget. Control calls never execute inference or wait for it.
// stop() invalidates a late result immediately; idle() also waits for the
// active detector call to return. Callbacks never run on the control thread.
class BargeInMonitor {
public:
    BargeInMonitor(std::function<void()> reset,
                   std::function<bool(const float*, std::size_t)> accept,
                   int call_timeout_ms);
    ~BargeInMonitor();
    void begin(AudioContext);
    void stop();
    bool can_accept() const;
    bool submit(const AudioFrame&);
    bool idle() const;
    std::optional<BargeInSignal> poll();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace voice_engine
