#include "voice_engine/barge_in.hpp"
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
using namespace voice_engine;
namespace {
void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
template<class F> void until(F condition) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= end) throw std::runtime_error("monitor test deadline");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
AudioFrame frame(AudioContext id) {
    AudioFrame f; f.context = id; f.samples.assign(320, .2f); f.valid_samples = 320; f.seq = 7; return f;
}
}
int main() {
    try {
        std::mutex gate; std::condition_variable cv;
        bool entered = false, release = false, should_throw = false;
        BargeInMonitor monitor([] {}, [&](const float*, std::size_t) {
            std::unique_lock<std::mutex> lock(gate);
            entered = true; cv.notify_all();
            if (!cv.wait_for(lock, std::chrono::seconds(2), [&] { return release; }))
                throw std::runtime_error("test gate timeout");
            if (should_throw) throw std::runtime_error("injected detector fault");
            return true;
        }, 1000);
        const AudioContext old{3, 4}, next{3, 5};
        monitor.begin(old); until([&] { return monitor.can_accept(); });
        require(!monitor.submit(frame(next)), "foreign capture accepted");
        require(monitor.submit(frame(old)), "first frame rejected");
        {
            std::unique_lock<std::mutex> lock(gate);
            require(cv.wait_for(lock, std::chrono::seconds(2), [&] { return entered; }), "detector not entered");
        }
        require(!monitor.can_accept() && !monitor.submit(frame(old)), "worker input not bounded");
        monitor.stop();
        require(!monitor.idle() && !monitor.poll(), "stop pretended detector call ended");
        bool rejected = false; try { monitor.begin(next); } catch (const std::logic_error&) { rejected = true; }
        require(rejected, "new generation overlapped detector");
        { std::lock_guard<std::mutex> lock(gate); release = true; cv.notify_all(); }
        until([&] { return monitor.idle(); });
        require(!monitor.poll(), "cancelled detector result leaked");
        monitor.begin(next); until([&] { return monitor.can_accept(); });
        require(monitor.submit(frame(next)), "next generation rejected");
        until([&] { return monitor.idle(); });
        auto hit = monitor.poll();
        require(hit && hit->context.generation_id == 5 && hit->last_frame_seq == 7 &&
            hit->input_samples == 320 && hit->error.empty(), "wrong detector event");
        require(!monitor.can_accept() && !monitor.poll(), "duplicate hit or admission after detection");
        monitor.stop();
        { std::lock_guard<std::mutex> lock(gate); should_throw = true; }
        monitor.begin({3, 6}); until([&] { return monitor.can_accept(); });
        monitor.submit(frame({3, 6})); until([&] { return monitor.idle(); });
        auto error = monitor.poll();
        require(error && error->error == "injected detector fault", "worker failure was hidden");
        std::cout << "BARGE_IN_MONITOR_OK: bounded input, late result invalidation, idle barrier, errors\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
