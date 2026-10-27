#pragma once
#include <atomic>
#include <functional>
#include <cstdint>
#include <mutex>
#include <stdexcept>

namespace voice_engine {
// One token per generation. A binding may outlive a cancel request, never its
// token. The short interrupt must not reenter this token or the stream worker.
// Binding teardown serializes with cancel(), protecting native RunOptions.
class TtsCancellation {
public:
    // Native adapter observations. Nonzero only while the synchronous Run is
    // owned by this generation; they do not claim acoustic or kernel timing.
    std::atomic<std::int64_t> native_run_started_ns{0};
    std::atomic<std::uint64_t> native_runs{0};
    bool requested() const noexcept { return requested_.load(); }
    void cancel() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (requested_.exchange(true)) return;
        if (interrupt_) interrupt_();
    }
    class Binding {
    public:
        Binding(TtsCancellation& token, std::function<void()> interrupt) : token_(token) {
            std::lock_guard<std::mutex> lock(token_.mutex_);
            if (!interrupt || token_.interrupt_)
                throw std::logic_error("invalid or overlapping TTS cancellation binding");
            token_.interrupt_ = std::move(interrupt);
            try { if (token_.requested_) token_.interrupt_(); }
            catch (...) { token_.interrupt_ = {}; throw; }
        }
        ~Binding() {
            std::lock_guard<std::mutex> lock(token_.mutex_);
            token_.interrupt_ = {};
        }
        Binding(const Binding&) = delete;
        Binding& operator=(const Binding&) = delete;
    private:
        TtsCancellation& token_;
    };
private:
    std::atomic<bool> requested_{false};
    std::mutex mutex_;
    std::function<void()> interrupt_;
};
} // namespace voice_engine
