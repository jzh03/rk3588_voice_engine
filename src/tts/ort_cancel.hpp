#pragma once
#include "voice_engine/tts_cancellation.hpp"
#include <memory>

namespace voice_engine {
// Application-owned bridge for the frozen sherpa 1.12.14 / ORT 1.17.1 ABI.
// Only Run() on this thread within a Melo generate scope is cancellable.
// Other callers (including Silero) receive the original API behavior.
class TtsOrtCancellationScope {
public:
    explicit TtsOrtCancellationScope(std::shared_ptr<TtsCancellation>);
    ~TtsOrtCancellationScope();
    TtsOrtCancellationScope(const TtsOrtCancellationScope&) = delete;
    TtsOrtCancellationScope& operator=(const TtsOrtCancellationScope&) = delete;
    void verify_observed() const;
    std::shared_ptr<TtsCancellation> token;
    unsigned runs = 0;
private:
    TtsOrtCancellationScope* previous_;
};
} // namespace voice_engine
