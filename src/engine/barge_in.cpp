#include "voice_engine/barge_in.hpp"
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace voice_engine {
namespace {
std::int64_t clock_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
bool same(AudioContext a, AudioContext b) {
    return a.session_id == b.session_id && a.generation_id == b.generation_id;
}
}
struct BargeInMonitor::Impl {
    std::function<void()> reset;
    std::function<bool(const float*, std::size_t)> accept;
    const std::int64_t timeout_ns;
    mutable std::mutex gate;
    std::condition_variable cv;
    std::thread worker;
    bool stopping = false, valid = false, resetting = false, busy = false, detected = false;
    AudioContext context;
    std::optional<AudioFrame> input;
    std::optional<BargeInSignal> result;
    std::int64_t call_started_ns = 0;
    std::uint64_t samples = 0;
    Impl(std::function<void()> r, std::function<bool(const float*, std::size_t)> a, int timeout)
        : reset(std::move(r)), accept(std::move(a)), timeout_ns(std::int64_t(timeout) * 1000000) {
        if (!reset || !accept || timeout <= 0) throw std::invalid_argument("invalid barge-in monitor");
        worker = std::thread([this] { run(); });
    }
    ~Impl() {
        { std::lock_guard<std::mutex> lock(gate); stopping = true; valid = false; cv.notify_all(); }
        worker.join();
    }
    void run() {
        std::unique_lock<std::mutex> lock(gate);
        while (true) {
            cv.wait(lock, [&] { return stopping || resetting || input.has_value(); });
            if (stopping) return;
            const auto id = context;
            const bool do_reset = resetting;
            resetting = false;
            AudioFrame frame;
            if (!do_reset) { frame = std::move(*input); input.reset(); }
            busy = true; call_started_ns = clock_ns();
            lock.unlock();
            bool hit = false;
            std::string error;
            try {
                if (do_reset) reset();
                else hit = accept(frame.samples.data(), frame.valid_samples);
            } catch (const std::exception& e) { error = e.what(); }
            catch (...) { error = "unknown CPU VAD failure"; }
            lock.lock();
            if (valid && same(id, context) && !detected) {
                if (!do_reset) samples += frame.valid_samples;
                if (hit || !error.empty()) {
                    detected = true;
                    result = BargeInSignal{id, clock_ns(), do_reset ? 0 : frame.seq, samples, error};
                }
            }
            busy = false; call_started_ns = 0; cv.notify_all();
        }
    }
};
BargeInMonitor::BargeInMonitor(std::function<void()> r,
        std::function<bool(const float*, std::size_t)> a, int timeout)
    : impl_(std::make_unique<Impl>(std::move(r), std::move(a), timeout)) {}
BargeInMonitor::~BargeInMonitor() = default;
void BargeInMonitor::begin(AudioContext c) {
    auto& i = *impl_; std::lock_guard<std::mutex> lock(i.gate);
    if (i.busy || i.resetting || i.input) throw std::logic_error("barge-in begin requires idle detector");
    i.context = c; i.valid = i.resetting = true; i.detected = false;
    i.samples = 0; i.result.reset(); i.cv.notify_all();
}
void BargeInMonitor::stop() {
    auto& i = *impl_; std::lock_guard<std::mutex> lock(i.gate);
    i.valid = i.resetting = false; i.input.reset(); i.result.reset(); i.cv.notify_all();
}
bool BargeInMonitor::can_accept() const {
    auto& i = *impl_; std::lock_guard<std::mutex> lock(i.gate);
    return i.valid && !i.detected && !i.busy && !i.resetting && !i.input;
}
bool BargeInMonitor::submit(const AudioFrame& f) {
    auto& i = *impl_; std::lock_guard<std::mutex> lock(i.gate);
    if (!i.valid || i.detected || i.busy || i.resetting || i.input || !same(f.context, i.context)) return false;
    if (f.samples.size() != 320 || f.valid_samples == 0 || f.valid_samples > 320 ||
        f.sample_rate != 16000 || f.channels != 1) throw std::invalid_argument("invalid barge-in frame");
    i.input = f; i.cv.notify_all(); return true;
}
bool BargeInMonitor::idle() const {
    auto& i = *impl_; std::lock_guard<std::mutex> lock(i.gate);
    return !i.busy && !i.resetting && !i.input;
}
std::optional<BargeInSignal> BargeInMonitor::poll() {
    auto& i = *impl_; std::lock_guard<std::mutex> lock(i.gate);
    if (i.valid && !i.detected && i.call_started_ns && clock_ns() - i.call_started_ns > i.timeout_ns) {
        i.detected = true;
        i.result = BargeInSignal{i.context, clock_ns(), 0, i.samples, "barge_vad_call_timeout"};
    }
    auto result = std::move(i.result); i.result.reset(); return result;
}
} // namespace voice_engine
