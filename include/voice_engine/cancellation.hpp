#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace voice_engine {
// The short interrupt can wake a worker or signal a native call. It must
// not reenter the token. Binding teardown serializes with cancellation.
class Cancellation {
public:
    bool requested() const noexcept { return requested_.load(); }
    void cancel() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (requested_.exchange(true)) return;
        if (interrupt_) interrupt_();
    }
    class Binding {
    public:
        Binding(Cancellation& token, std::function<void()> interrupt) : token_(token) {
            std::lock_guard<std::mutex> lock(token_.mutex_);
            if (!interrupt || token_.interrupt_)
                throw std::logic_error("invalid or overlapping cancellation binding");
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
        Cancellation& token_;
    };
private:
    std::atomic<bool> requested_{false};
    std::mutex mutex_;
    std::function<void()> interrupt_;
};
} // namespace voice_engine
