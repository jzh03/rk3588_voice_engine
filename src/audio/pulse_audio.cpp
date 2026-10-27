#include "voice_engine/live_audio.hpp"
#include "voice_engine/audio_dsp.hpp"

#include <pulse/pulseaudio.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

namespace voice_engine {
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now().time_since_epoch()).count();
}
bool same_context(AudioContext a, AudioContext b) {
    return a.session_id == b.session_id && a.generation_id == b.generation_id;
}
std::string safe(const char* value) { return value ? value : ""; }
void release(pa_operation* operation) { if (operation) pa_operation_unref(operation); }

std::string identity(pa_proplist* properties) {
    std::string result;
    for (const char* key : {"device.string", "api.bluez5.address", "device.bus_path",
                            "device.serial", "device.vendor.id", "device.product.id"}) {
        const char* value = properties ? pa_proplist_gets(properties, key) : nullptr;
        if (value) result += std::string(key) + "=" + value + "\n";
    }
    return result;
}
LiveAudioBuffer buffer_info(const pa_buffer_attr* value) {
    if (!value) return {};
    return {value->maxlength, value->tlength, value->prebuf, value->minreq, value->fragsize};
}

struct Device {
    bool found = false;
    std::string name;
    std::string identity;
    std::uint32_t index = PA_INVALID_INDEX;
    std::uint32_t card = PA_INVALID_INDEX;
    pa_sample_spec spec{};
};

class PulseAudio final : public LiveAudioBackend {
public:
    explicit PulseAudio(LiveAudioOptions options) : options_(std::move(options)) {
        if (options_.source.empty() || options_.sink.empty() || options_.profile.empty() ||
            options_.source.find('\0') != std::string::npos ||
            options_.sink.find('\0') != std::string::npos ||
            options_.profile.find('\0') != std::string::npos ||
            options_.source.size() >= 256 || options_.sink.size() >= 256 ||
            options_.profile.size() >= 256)
            throw AudioError("explicit source, sink and profile names are required (1..255 bytes)");
        if (options_.source.size() >= 8 &&
            options_.source.compare(options_.source.size() - 8, 8, ".monitor") == 0)
            throw AudioError("monitor source is forbidden");
        if ((!options_.capture_enabled && !options_.playback_enabled) ||
            options_.capture_capacity_frames < 1 || options_.capture_capacity_frames > 500 ||
            options_.playback_capacity_frames < 1 || options_.playback_capacity_frames > 500)
            throw AudioError("audio directions or queue capacities are invalid");
        if (options_.playback_lead_in_ms < 0 || options_.playback_lead_in_ms > 2000 ||
            options_.playback_lead_in_ms % 20 != 0)
            throw AudioError("playback lead-in must be 0..2000 ms and a multiple of 20");
        status_.source = options_.source;
        status_.sink = options_.sink;
        status_.profile = options_.profile;
        worker_ = std::thread([this] { run(); });
    }
    ~PulseAudio() override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            wake_worker();
        }
        if (worker_.joinable()) worker_.join();
    }
    bool start_session(AudioContext context, bool capture) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.state == LiveAudioState::device_wait || fault_pending_) return false;
        if (capture && !options_.capture_enabled)
            throw AudioError("capture is disabled for this backend");
        if (last_context_valid_ &&
            (context.session_id < last_context_.session_id ||
             (context.session_id == last_context_.session_id &&
              context.generation_id <= last_context_.generation_id))) {
            ++status_.stale_rejections;
            return false;
        }
        last_context_ = context;
        last_context_valid_ = true;
        status_.context = context;
        status_.session_active = true;
        desired_capture_ = capture;
        invalidate_queues();
        ++epoch_;
        status_.reset_pending = true;
        emit("session_start_requested", capture ? "capture and playback session" : "playback session");
        wake_worker();
        return true;
    }
    bool interrupt_playback(AudioContext next) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.state == LiveAudioState::device_wait || fault_pending_ ||
            !status_.session_active || !status_.capturing || status_.capture_stopping ||
            !desired_capture_ || status_.reset_pending ||
            next.session_id != status_.context.session_id ||
            next.generation_id <= status_.context.generation_id) return false;
        last_context_ = status_.context = next; last_context_valid_ = true;
        for (auto& frame : capture_) frame.context = next;
        playback_.clear(); native_playback_.clear(); native_offset_ = 0;
        drain_requested_ = tail_finished_ = playback_started_ = playback_transport_started_ = false;
        lead_in_remaining_frames_ = 0; status_.drained = true;
        capture_preserved_reset_ = true; status_.reset_pending = true; ++epoch_;
        emit("playback_interrupt_requested", "old output invalidated; continuous capture preserved");
        wake_worker(); return true;
    }
    void stop_capture() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!desired_capture_ && !status_.capturing) return;
        desired_capture_ = false;
        status_.capturing = false;
        status_.capture_stopping = true;
        stop_capture_pending_ = true;
        emit("capture_stop_requested", "capture cork requested; buffered input and filter tail will finish normally");
        wake_worker();
    }
    void cancel() override {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.session_active = false;
        desired_capture_ = false;
        invalidate_queues();
        ++epoch_;
        status_.reset_pending = status_.state != LiveAudioState::device_wait;
        emit("cancel_requested", "application queues invalidated; server flush requested");
        wake_worker();
    }
    bool try_capture_frame(AudioFrame& frame) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (capture_.empty()) return false;
        frame = std::move(capture_.front());
        capture_.pop_front();
        return true;
    }
    bool try_submit_playback(const AudioFrame& frame) override {
        if (frame.sample_rate != 16000 || frame.channels != 1 || frame.samples.size() != 320 ||
            frame.valid_samples < 1 || frame.valid_samples > frame.samples.size() ||
            !std::all_of(frame.samples.begin(), frame.samples.end(), [](float value) {
                return std::isfinite(value) && value >= -1.0F && value <= 1.0F;
            }) || !std::all_of(frame.samples.begin() + frame.valid_samples, frame.samples.end(),
                               [](float value) { return value == 0.0F; }))
            throw AudioError("playback requires 320 normalized finite mono float32 samples at 16 kHz with 1..320 valid samples and zero padding");
        std::lock_guard<std::mutex> lock(mutex_);
        if (!options_.playback_enabled || !status_.session_active ||
            status_.state == LiveAudioState::device_wait || drain_requested_)
            return false;
        if (!same_context(frame.context, status_.context)) {
            ++status_.stale_rejections;
            return false;
        }
        const std::size_t pending = playback_.size() + (native_playback_.empty() ? 0 : 1);
        if (pending >= options_.playback_capacity_frames) return false;
        playback_.push_back(frame);
        status_.playback_queue_peak_frames = std::max(status_.playback_queue_peak_frames,
            playback_.size() + (native_playback_.empty() ? 0 : 1));
        status_.drained = false;
        wake_worker();
        return true;
    }
    void request_drain() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.session_active && options_.playback_enabled && !drain_requested_) {
            drain_requested_ = true;
            status_.drained = false;
            drain_deadline_ = Clock::now() + std::chrono::milliseconds(
                options_.playback_capacity_frames * 20 + options_.playback_lead_in_ms + 5000);
            wake_worker();
        }
    }
    bool playback_drained() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return status_.drained;
    }
    bool commit_if_drained(AudioContext context, const std::function<bool()>& commit) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (fault_pending_ || !status_.session_active || !same_context(context, status_.context) ||
            status_.state == LiveAudioState::device_wait || status_.reset_pending ||
            !drain_requested_ || !status_.drained) return false;
        return commit();
    }
    LiveAudioStatus status() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto result = status_;
        result.capture_queued_frames = capture_.size();
        result.playback_queued_frames = playback_.size() + (native_playback_.empty() ? 0 : 1);
        return result;
    }
    std::vector<LiveAudioEvent> poll_events() override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<LiveAudioEvent> result;
        result.reserve(events_.size());
        while (!events_.empty()) {
            result.push_back(std::move(events_.front()));
            events_.pop_front();
        }
        return result;
    }

private:
    // All callers hold mutex_; the only cross-thread Pulse call is wakeup(),
    // documented to interrupt poll(). Creation, callbacks and teardown stay on
    // the owner thread, and no app mutex is held during the blocking poll.
    void wake_worker() {
        if (loop_) pa_mainloop_wakeup(loop_);
        wake_.notify_one();
    }
    void wait_events(std::unique_lock<std::mutex>& lock) {
        if (!loop_) { wake_.wait_for(lock, 5ms); return; }
        // Pulse specifies microseconds. Preserve the existing 5 ms upper bound,
        // but wake immediately for server I/O, timers, deferred work or commands.
        if (pa_mainloop_prepare(loop_, raw_.empty() ? 5000 : 0) < 0) {
            fault("audio event prepare failed"); return;
        }
        lock.unlock();
        const int polled = pa_mainloop_poll(loop_);
        lock.lock();
        if (polled < 0 || pa_mainloop_dispatch(loop_) < 0)
            fault("audio event poll/dispatch failed");
    }
    void service_output() {
        const auto now = now_ns();
        if (last_output_service_ns_)
            status_.output_service_max_gap_ns = std::max(status_.output_service_max_gap_ns,
                                                         now - last_output_service_ns_);
        last_output_service_ns_ = now;
        process_keepalive();
        process_playback();
    }
    std::string service_detail() const {
        return "output_service_gap_ns=" + std::to_string(last_output_service_ns_ ?
            now_ns() - last_output_service_ns_ : 0) +
            " playback_queued_frames=" + std::to_string(playback_.size()) +
            " raw_capture_samples=" + std::to_string(raw_samples_);
    }
    void emit(std::string type, std::string detail) {
        if (events_.size() == 256) {
            events_.pop_front();
            ++status_.dropped_events;
        }
        events_.push_back({std::move(type), std::move(detail), now_ns(), status_.context});
    }
    void fault(std::string reason) {
        if (!fault_pending_) {
            fault_pending_ = true;
            fault_reason_ = std::move(reason);
        }
    }
    void invalidate_queues() {
        capture_preserved_reset_ = false;
        status_.capturing = false;
        status_.capture_stopping = false;
        stop_capture_pending_ = false; stop_cork_done_ = false;
        capture_.clear(); playback_.clear(); raw_.clear(); raw_samples_ = 0; raw_offset_ = 0;
        native_playback_.clear(); native_offset_ = 0;
        drain_requested_ = false; tail_finished_ = false;
        playback_started_ = false; playback_transport_started_ = false;
        lead_in_remaining_frames_ = 0; status_.drained = true;
    }
    void clear_operation(pa_operation*& operation) {
        if (operation) {
            pa_operation_cancel(operation);
            pa_operation_unref(operation);
            operation = nullptr;
        }
    }
    void request_cork(pa_stream* stream, bool cork) {
        auto& operation = stream == record_ ? capture_cork_ : playback_cork_;
        clear_operation(operation);
        operation = pa_stream_cork(stream, cork ? 1 : 0, cork_done, this);
        if (!operation) fault("audio stream cork/uncork request failed");
    }
    void destroy_stream(pa_stream*& stream) {
        if (!stream) return;
        pa_stream_set_state_callback(stream, nullptr, nullptr);
        pa_stream_set_read_callback(stream, nullptr, nullptr);
        pa_stream_set_underflow_callback(stream, nullptr, nullptr);
        pa_stream_set_overflow_callback(stream, nullptr, nullptr);
        pa_stream_set_moved_callback(stream, nullptr, nullptr);
        pa_stream_set_suspended_callback(stream, nullptr, nullptr);
        pa_stream_disconnect(stream);
        pa_stream_unref(stream);
        stream = nullptr;
    }
    void teardown() {
        clear_operation(capture_flush_); clear_operation(playback_flush_);
        clear_operation(drain_operation_); clear_operation(stop_cork_);
        clear_operation(capture_cork_); clear_operation(playback_cork_);
        clear_operation(keepalive_cork_);
        destroy_stream(keepalive_); destroy_stream(record_); destroy_stream(play_);
        keepalive_started_ = false; keepalive_uncorked_ = false;
        status_.playback_keepalive_active = false;
        status_.playback_keepalive_buffer = {};
        if (context_) {
            pa_context_set_state_callback(context_, nullptr, nullptr);
            pa_context_set_subscribe_callback(context_, nullptr, nullptr);
            pa_context_disconnect(context_); pa_context_unref(context_); context_ = nullptr;
        }
        if (loop_) { pa_mainloop_free(loop_); loop_ = nullptr; }
        capture_dsp_.reset(); playback_dsp_.reset(); assembler_.reset();
        discovering_ = false; source_done_ = false; sink_done_ = false;
        streams_ready_ = false; bound_source_ = {}; bound_sink_ = {};
        last_output_service_ns_ = 0;
        current_card_.clear();
    }
    void enter_wait() {
        const bool was_ready = status_.state != LiveAudioState::device_wait;
        status_.session_active = false; desired_capture_ = false;
        invalidate_queues(); ++epoch_;
        status_.reset_pending = false;
        status_.state = LiveAudioState::device_wait;
        if (was_ready) {
            if (status_.context.generation_id != std::numeric_limits<std::uint64_t>::max())
                ++status_.context.generation_id;
        }
        emit("device_wait", fault_reason_);
        teardown();
        const int delays[] = {1, 2, 5};
        const int delay = delays[std::min<std::size_t>(retry_attempt_, 2)];
        ++retry_attempt_;
        retry_at_ = Clock::now() + std::chrono::seconds(delay);
        emit("retry_scheduled", std::to_string(delay) + " seconds");
        fault_pending_ = false;
    }
    void connect_server() {
        loop_ = pa_mainloop_new();
        if (!loop_) throw AudioError("pa_mainloop_new failed");
        context_ = pa_context_new(pa_mainloop_get_api(loop_), "rk3588-voice-audio-probe");
        if (!context_) throw AudioError("pa_context_new failed");
        pa_context_set_state_callback(context_, context_state, this);
        if (pa_context_connect(context_, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0)
            fault("audio service connection failed");
        deadline_ = Clock::now() + 5s;
    }
    void discover() {
        if (discovering_ || !context_ || pa_context_get_state(context_) != PA_CONTEXT_READY) return;
        discovering_ = true; source_done_ = false; sink_done_ = false;
        candidate_source_ = {}; candidate_sink_ = {};
        deadline_ = Clock::now() + 3s;
        pa_operation* first = pa_context_get_source_info_by_name(context_, options_.source.c_str(), source_info, this);
        pa_operation* second = pa_context_get_sink_info_by_name(context_, options_.sink.c_str(), sink_info, this);
        if (!first || !second) fault("explicit device lookup could not be issued");
        release(first); release(second);
    }
    void check_discovery() {
        if (!source_done_ || !sink_done_) return;
        if (!candidate_source_.found || !candidate_sink_.found) {
            fault("configured source or sink is unavailable"); return;
        }
        if (candidate_source_.card != candidate_sink_.card) {
            fault("configured source and sink do not belong to the same card"); return;
        }
        if (candidate_source_.card == PA_INVALID_INDEX) {
            if (options_.profile != "none") {
                fault("cardless devices require the explicit virtual profile 'none'"); return;
            }
            finish_discovery("virtual-cardless", "none", "");
            return;
        }
        pa_operation* operation = pa_context_get_card_info_by_index(
            context_, candidate_source_.card, card_info, this);
        if (!operation) fault("card profile lookup could not be issued");
        release(operation);
    }
    void finish_discovery(const std::string& card, const std::string& profile,
                          const std::string& card_identity) {
        if (profile != options_.profile || card.empty()) {
            fault("configured card active profile does not match audio.profile"); return;
        }
        for (const auto* device : {&candidate_source_, &candidate_sink_}) {
            if (device->spec.rate < 8000 || device->spec.rate > 192000 ||
                device->spec.channels < 1 || device->spec.channels > 8) {
                fault("device sample rate/channel count outside supported DSP range"); return;
            }
        }
        const auto source_identity = card + "\n" + card_identity + candidate_source_.identity;
        const auto sink_identity = card + "\n" + card_identity + candidate_sink_.identity;
        if (identity_bound_ && (source_identity != expected_source_identity_ ||
                                sink_identity != expected_sink_identity_)) {
            fault("configured device name now resolves to a different device identity"); return;
        }
        if (!identity_bound_) {
            identity_bound_ = true;
            expected_source_identity_ = source_identity;
            expected_sink_identity_ = sink_identity;
        }
        if (record_ || play_) {
            if (candidate_source_.index != bound_source_.index ||
                candidate_sink_.index != bound_sink_.index || card != current_card_ ||
                candidate_source_.spec.rate != bound_source_.spec.rate ||
                candidate_source_.spec.channels != bound_source_.spec.channels ||
                candidate_sink_.spec.rate != bound_sink_.spec.rate ||
                candidate_sink_.spec.channels != bound_sink_.spec.channels) {
                fault("device index, format or card changed; old generation cancelled"); return;
            }
            // Native sample representation can change during PipeWire's
            // negotiation without changing rate, channels or device identity.
            // The application stream remains explicitly float32 in either case.
            bound_source_.spec.format = candidate_source_.spec.format;
            bound_sink_.spec.format = candidate_sink_.spec.format;
            status_.capture_device_format = safe(pa_sample_format_to_string(bound_source_.spec.format));
            status_.playback_device_format = safe(pa_sample_format_to_string(bound_sink_.spec.format));
        } else {
            bound_source_ = candidate_source_; bound_sink_ = candidate_sink_;
            current_card_ = card;
            status_.card = card; status_.profile = profile;
            status_.source_identity = source_identity; status_.sink_identity = sink_identity;
            status_.capture_rate = static_cast<int>(bound_source_.spec.rate);
            status_.capture_channels = bound_source_.spec.channels;
            status_.playback_rate = static_cast<int>(bound_sink_.spec.rate);
            status_.playback_channels = bound_sink_.spec.channels;
            status_.capture_device_format = safe(pa_sample_format_to_string(bound_source_.spec.format));
            status_.playback_device_format = safe(pa_sample_format_to_string(bound_sink_.spec.format));
            status_.device_available_ns = now_ns();
            emit("device_available", "source, sink, identity and active profile match configuration");
            make_streams();
        }
        discovering_ = false;
        next_discovery_ = Clock::now() + 2s;
    }
    void make_streams() {
        const auto flags = static_cast<pa_stream_flags_t>(PA_STREAM_START_CORKED |
            PA_STREAM_DONT_MOVE | PA_STREAM_AUTO_TIMING_UPDATE | PA_STREAM_ADJUST_LATENCY);
        if (options_.capture_enabled) {
            pa_sample_spec spec{PA_SAMPLE_FLOAT32NE, bound_source_.spec.rate, bound_source_.spec.channels};
            record_ = pa_stream_new(context_, "explicit microphone", &spec, nullptr);
            if (!record_) { fault("record stream allocation failed"); return; }
            pa_stream_set_state_callback(record_, stream_state, this);
            pa_stream_set_read_callback(record_, read_ready, this);
            pa_stream_set_overflow_callback(record_, overflow, this);
            pa_stream_set_moved_callback(record_, moved, this);
            pa_stream_set_suspended_callback(record_, suspended, this);
            const std::uint32_t frame_bytes = spec.rate / 50 * spec.channels * sizeof(float);
            pa_buffer_attr attr{frame_bytes * 5, 0, 0, 0, frame_bytes};
            if (pa_stream_connect_record(record_, options_.source.c_str(), &attr, flags) < 0)
                fault("record stream connection to explicit source failed");
            capture_dsp_ = std::make_unique<StreamResampler>(spec.rate, 16000, spec.channels, 1);
        }
        if (options_.playback_enabled) {
            pa_sample_spec spec{PA_SAMPLE_FLOAT32NE, bound_sink_.spec.rate, bound_sink_.spec.channels};
            play_ = pa_stream_new(context_, "explicit earphone output", &spec, nullptr);
            if (!play_) { fault("playback stream allocation failed"); return; }
            pa_stream_set_state_callback(play_, stream_state, this);
            pa_stream_set_underflow_callback(play_, underflow, this);
            pa_stream_set_overflow_callback(play_, overflow, this);
            pa_stream_set_moved_callback(play_, moved, this);
            pa_stream_set_suspended_callback(play_, suspended, this);
            const std::uint32_t frame_bytes = spec.rate / 50 * spec.channels * sizeof(float);
            pa_buffer_attr attr{frame_bytes * 5, frame_bytes * 3, frame_bytes * 2,
                                frame_bytes, 0};
            if (pa_stream_connect_playback(play_, options_.sink.c_str(), &attr, flags, nullptr, nullptr) < 0)
                fault("playback stream connection to explicit sink failed");
            playback_dsp_ = std::make_unique<StreamResampler>(16000, spec.rate, 1, spec.channels);
            if (options_.playback_keepalive) {
                keepalive_ = pa_stream_new(context_, "explicit earphone keepalive", &spec, nullptr);
                if (!keepalive_) { fault("keepalive stream allocation failed"); return; }
                pa_stream_set_state_callback(keepalive_, stream_state, this);
                pa_stream_set_underflow_callback(keepalive_, underflow, this);
                pa_stream_set_overflow_callback(keepalive_, overflow, this);
                pa_stream_set_moved_callback(keepalive_, moved, this);
                pa_stream_set_suspended_callback(keepalive_, suspended, this);
                if (pa_stream_connect_playback(keepalive_, options_.sink.c_str(), &attr,
                                               flags, nullptr, nullptr) < 0)
                    fault("keepalive stream connection to explicit sink failed");
            }
        }
        stream_deadline_ = Clock::now() + 3s;
    }
    void check_streams() {
        if (streams_ready_ || fault_pending_) return;
        if ((record_ && pa_stream_get_state(record_) != PA_STREAM_READY) ||
            (play_ && pa_stream_get_state(play_) != PA_STREAM_READY) ||
            (keepalive_ && pa_stream_get_state(keepalive_) != PA_STREAM_READY)) return;
        if (!record_ && !play_) return;
        for (const auto& binding : {std::make_pair(record_, &bound_source_),
                                    std::make_pair(play_, &bound_sink_),
                                    std::make_pair(keepalive_, &bound_sink_)}) {
            if (!binding.first) continue;
            const auto* spec = pa_stream_get_sample_spec(binding.first);
            if (safe(pa_stream_get_device_name(binding.first)) != binding.second->name ||
                pa_stream_get_device_index(binding.first) != binding.second->index || !spec ||
                spec->rate != binding.second->spec.rate || spec->channels != binding.second->spec.channels ||
                spec->format != PA_SAMPLE_FLOAT32NE) {
                fault("server stream binding or native-rate float format differs from request"); return;
            }
        }
        status_.capture_buffer = buffer_info(record_ ? pa_stream_get_buffer_attr(record_) : nullptr);
        status_.playback_buffer = buffer_info(play_ ? pa_stream_get_buffer_attr(play_) : nullptr);
        status_.playback_keepalive_buffer = buffer_info(keepalive_ ? pa_stream_get_buffer_attr(keepalive_) : nullptr);
        streams_ready_ = true; retry_attempt_ = 0;
        if (ready_once_) ++status_.reconnects;
        ready_once_ = true;
        status_.state = LiveAudioState::ready;
        status_.ready_ns = now_ns();
        applied_epoch_ = epoch_;
        emit("device_ready", "explicit devices and active profile verified; waiting for a new start");
    }
    void apply_reset() {
        if (!streams_ready_ || applied_epoch_ == epoch_) return;
        clear_operation(playback_flush_); clear_operation(drain_operation_);
        clear_operation(playback_cork_);
        capture_flush_done_ = capture_preserved_reset_ || !record_;
        playback_flush_done_ = !play_;
        if (playback_dsp_) playback_dsp_->reset();
        if (!capture_preserved_reset_) {
            clear_operation(capture_flush_); clear_operation(stop_cork_); stop_cork_done_ = false;
            clear_operation(capture_cork_);
            if (capture_dsp_) capture_dsp_->reset();
            assembler_.reset(); capture_seq_ = 0;
            if (record_) {
                request_cork(record_, true);
                capture_flush_ = pa_stream_flush(record_, flush_done, this);
                if (!capture_flush_) fault("capture flush request failed");
            }
        }
        if (play_) {
            request_cork(play_, true);
            playback_flush_ = pa_stream_flush(play_, flush_done, this);
            if (!playback_flush_) fault("playback flush request failed");
        }
        applied_epoch_ = epoch_;
        reset_deadline_ = Clock::now() + 3s;
    }
    void complete_reset() {
        if (fault_pending_ || !status_.reset_pending || applied_epoch_ != epoch_ ||
            !capture_flush_done_ || !playback_flush_done_) return;
        clear_operation(capture_flush_); clear_operation(playback_flush_);
        status_.reset_pending = false;
        status_.capturing = status_.session_active && desired_capture_;
        status_.state = status_.capturing ? LiveAudioState::capturing : LiveAudioState::ready;
        if (status_.capturing && record_ && !capture_preserved_reset_) {
            capture_started_at_ = Clock::now();
            request_cork(record_, false);
        }
        if (capture_preserved_reset_)
            emit("playback_interrupt_flushed", "playback flush acknowledged; capture and DSP uninterrupted");
        else emit(status_.session_active ? "session_started" : "cancel_flushed",
             "server capture/playback flush acknowledged; old DSP state cleared");
        capture_preserved_reset_ = false;
    }
    bool enqueue_capture(const MonoFrame& samples, std::size_t valid_samples = 320) {
        if (capture_.size() == options_.capture_capacity_frames) {
            ++status_.capture_overflows;
            fault("bounded capture queue overflow; generation cancelled"); return false;
        }
        AudioFrame frame;
        frame.context = status_.context; frame.seq = ++capture_seq_;
        frame.monotonic_ns = now_ns(); frame.valid_samples = valid_samples;
        frame.samples.assign(samples.begin(), samples.end());
        capture_.push_back(std::move(frame)); ++status_.capture_frames;
        status_.capture_queue_peak_frames = std::max(status_.capture_queue_peak_frames, capture_.size());
        return true;
    }
    void process_capture() {
        if ((!status_.capturing && !status_.capture_stopping) ||
            (status_.reset_pending && !capture_preserved_reset_) ||
            fault_pending_ || raw_.empty()) return;
        const auto begin = now_ns();
        // One 20 ms native slice per service pass. Retain the rest in the same
        // bounded queue; an accumulated capture fragment must not monopolize
        // the worker while speech and keepalive wait for transport service.
        auto& input = raw_.front();
        const std::size_t channels = bound_source_.spec.channels;
        const auto count = std::min<std::size_t>(
            bound_source_.spec.rate / 50 * channels, input.size() - raw_offset_);
        for (std::size_t i = raw_offset_; i < raw_offset_ + count; ++i) {
            if (!std::isfinite(input[i])) {
                fault("capture contains a non-finite sample"); return;
            }
            input[i] = std::clamp(input[i], -1.0F, 1.0F);
        }
        auto mono = capture_dsp_->process(input.data() + raw_offset_, count / channels);
        raw_offset_ += count; raw_samples_ -= count;
        if (raw_offset_ == input.size()) { raw_.pop_front(); raw_offset_ = 0; }
        for (const auto& samples : assembler_.push(mono.data(), mono.size())) {
            if (!enqueue_capture(samples)) return;
        }
        status_.capture_service_max_work_ns = std::max(
            status_.capture_service_max_work_ns, now_ns() - begin);
    }
    void finish_capture() {
        if (!status_.capture_stopping || status_.reset_pending || fault_pending_) return;
        if (stop_capture_pending_) {
            stop_capture_pending_ = false; stop_cork_done_ = !record_;
            stop_epoch_ = epoch_;
            stop_deadline_ = Clock::now() + 3s;
            if (record_) {
                stop_cork_ = pa_stream_cork(record_, 1, capture_cork_done, this);
                if (!stop_cork_) fault("capture cork request failed");
            }
        }
        if (!stop_cork_done_) {
            if (Clock::now() > stop_deadline_) fault("capture cork acknowledgement timeout");
            return;
        }
        clear_operation(stop_cork_);
        // Cork stops production; any input already delivered to the client is
        // still part of normal input. Pull bounded fragments outside callbacks
        // and finish conversion before publishing the final padded frame.
        if (record_) {
            const auto readable = pa_stream_readable_size(record_);
            if (readable == static_cast<std::size_t>(-1)) {
                fault("capture readable-size query failed while stopping"); return;
            }
            if (readable) read_ready(record_, readable, this);
        }
        process_capture();
        if (fault_pending_) return;
        // Normal stop still preserves every buffered sample and the DSP tail;
        // finish across service passes rather than draining a backlog at once.
        if (!raw_.empty() || (record_ && pa_stream_readable_size(record_) != 0)) return;
        if (capture_dsp_) {
            auto tail = capture_dsp_->finish();
            for (const auto& samples : assembler_.push(tail.data(), tail.size())) {
                if (!enqueue_capture(samples)) return;
            }
            if (auto final_frame = assembler_.finish()) {
                if (!enqueue_capture(final_frame->samples, final_frame->valid_samples)) return;
            }
        }
        status_.capture_stopping = false;
        status_.state = LiveAudioState::ready;
        emit("capture_stopped", "capture cork acknowledged; buffered input and DSP tail queued; context remains playable");
    }
    void start_playback_transport() {
        if (playback_transport_started_) return;
        playback_transport_started_ = true;
        playback_started_at_ = Clock::now();
        request_cork(play_, false);
    }
    void process_playback() {
        if (!play_ || !status_.session_active || status_.reset_pending || fault_pending_) return;
        std::size_t writable = pa_stream_writable_size(play_);
        if (writable == static_cast<std::size_t>(-1)) { fault("playback writable-size query failed"); return; }
        const std::size_t frame_bytes = bound_sink_.spec.channels * sizeof(float);
        writable -= writable % frame_bytes;
        // Do not wake or add silence for an empty turn. Native silence goes
        // straight to Pulse's existing bounded server buffer; the constant
        // 1 KiB scratch below is not a queue of additional speech frames.
        if (!playback_transport_started_ && !playback_.empty())
            lead_in_remaining_frames_ = static_cast<std::size_t>(bound_sink_.spec.rate) *
                                        options_.playback_lead_in_ms / 1000;
        static constexpr std::array<float, 256> silence{};
        const auto channels = static_cast<std::size_t>(bound_sink_.spec.channels);
        for (std::size_t work = 0; lead_in_remaining_frames_ &&
                work < options_.playback_capacity_frames + 2; ++work) {
            const auto frames = std::min({lead_in_remaining_frames_, writable / frame_bytes,
                                          silence.size() / channels});
            if (!frames) return;
            const auto bytes = frames * frame_bytes;
            if (pa_stream_write(play_, silence.data(), bytes, nullptr, 0, PA_SEEK_RELATIVE) < 0) {
                fault("playback lead-in write failed"); return;
            }
            lead_in_remaining_frames_ -= frames; writable -= bytes;
            if (!playback_transport_started_) {
                start_playback_transport();
                emit("playback_lead_in_started", std::to_string(options_.playback_lead_in_ms) +
                     " ms native-rate silence before speech; software submission only");
            }
            if (!lead_in_remaining_frames_)
                emit("playback_lead_in_queued", "all configured silence submitted; not an acoustic readiness claim");
        }
        if (lead_in_remaining_frames_) return;
        for (std::size_t work = 0; work < options_.playback_capacity_frames + 2; ++work) {
            if (native_playback_.empty()) {
                if (!playback_.empty()) {
                    auto frame = std::move(playback_.front()); playback_.pop_front();
                    native_playback_ = playback_dsp_->process(frame.samples.data(), frame.valid_samples);
                    native_offset_ = 0; ++status_.playback_frames;
                } else if (drain_requested_ && !tail_finished_) {
                    native_playback_ = playback_dsp_->finish();
                    native_offset_ = 0; tail_finished_ = true;
                } else break;
            }
            if (native_playback_.empty()) continue;
            const std::size_t bytes = std::min(writable,
                (native_playback_.size() - native_offset_) * sizeof(float));
            if (!bytes) break;
            if (pa_stream_write(play_, native_playback_.data() + native_offset_, bytes,
                                nullptr, 0, PA_SEEK_RELATIVE) < 0) {
                fault("playback write failed"); return;
            }
            native_offset_ += bytes / sizeof(float); writable -= bytes;
            if (native_offset_ == native_playback_.size()) { native_playback_.clear(); native_offset_ = 0; }
            if (!playback_started_) {
                playback_started_ = true;
                start_playback_transport();
                emit("playback_started", "first native-rate speech samples submitted after any lead-in; not acoustic onset");
            }
        }
        if (drain_requested_ && tail_finished_ && playback_.empty() && native_playback_.empty() &&
            !drain_operation_ && !status_.drained) {
            if (!playback_started_) { status_.drained = true; emit("playback_drained", "empty playback"); return; }
            release(pa_stream_trigger(play_, nullptr, nullptr));
            drain_operation_ = pa_stream_drain(play_, drain_done, this);
            if (!drain_operation_) fault("playback drain request failed");
        }
    }
    void process_keepalive() {
        // Deliberately independent of session/reset/drain: no user PCM or
        // generation data ever enters this stream. It cannot delay speech drain.
        if (!keepalive_ || fault_pending_) return;
        const std::size_t channels = bound_sink_.spec.channels;
        const std::size_t stride = channels * sizeof(float);
        std::size_t writable = pa_stream_writable_size(keepalive_);
        if (writable == static_cast<std::size_t>(-1)) {
            fault("keepalive writable size failed"); return;
        }
        const std::array<float, 1024> silence{};
        const std::size_t chunk_bytes = silence.size() / channels * stride;
        // Bound each worker pass even if the server grants an oversized buffer.
        for (std::size_t work = 0; writable >= stride && work < 32; ++work) {
            const auto bytes = std::min(writable / stride * stride, chunk_bytes);
            if (pa_stream_write(keepalive_, silence.data(), bytes, nullptr, 0, PA_SEEK_RELATIVE) < 0) {
                fault("keepalive silence write failed"); return;
            }
            status_.playback_keepalive_native_frames += bytes / stride;
            writable -= bytes;
            if (!keepalive_started_) {
                keepalive_started_ = true;
                keepalive_started_at_ = Clock::now();
                keepalive_cork_ = pa_stream_cork(keepalive_, 0, keepalive_cork_done, this);
                if (!keepalive_cork_) { fault("keepalive uncork request failed"); return; }
                emit("playback_keepalive_started", "independent zero-PCM stream; no per-turn prefix");
            }
        }
    }
    void check_active_streams() {
        // A corked device can legitimately suspend while idle. Give an
        // uncorked stream time to resume, then fail instead of feeding an
        // indefinitely suspended device or carrying its generation forward.
        if (fault_pending_) return;
        if (keepalive_ && keepalive_started_) {
            const bool active = keepalive_uncorked_ && pa_stream_is_suspended(keepalive_) == 0;
            if (active != status_.playback_keepalive_active) {
                status_.playback_keepalive_active = active;
                emit(active ? "playback_keepalive_active" : "playback_keepalive_inactive",
                     "server transport state only; not acoustic readiness");
            }
            if (Clock::now() > keepalive_started_at_ + 3s && !active)
                fault("keepalive stream did not remain uncorked and available");
        }
        if (status_.reset_pending || fault_pending_) return;
        if (record_ && status_.capturing && Clock::now() > capture_started_at_ + 1s &&
            pa_stream_is_suspended(record_) != 0)
            fault("active capture device is suspended or unavailable");
        if (play_ && playback_transport_started_ && !status_.drained &&
            Clock::now() > playback_started_at_ + 1s && pa_stream_is_suspended(play_) != 0)
            fault("active playback device is suspended or unavailable");
    }
    void pump_events() {
        if (!loop_) return;
        // Pulse transports headers, payloads and deferred work in separate
        // iterations. Sleeping after each dispatch throttles request/PCM
        // traffic and can starve playback despite an already filled queue.
        // Drain currently runnable work without blocking; the cap also bounds
        // callback work so commands and DSP retain scheduling opportunities.
        for (std::size_t work = 0; work < 32 && !fault_pending_; ++work) {
            int result = 0;
            const int dispatched = pa_mainloop_iterate(loop_, 0, &result);
            if (dispatched < 0) { fault("audio event loop failed"); return; }
            if (dispatched == 0) break;
        }
    }
    void run() noexcept {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!stopping_) {
            try {
                if (fault_pending_) enter_wait();
                if (!context_ && Clock::now() >= retry_at_) connect_server();
                pump_events();
                if (context_ && pa_context_get_state(context_) == PA_CONTEXT_READY &&
                    !discovering_ && Clock::now() >= next_discovery_) discover();
                if (context_ && (pa_context_get_state(context_) != PA_CONTEXT_READY || discovering_) &&
                    Clock::now() > deadline_)
                    fault("audio service/device discovery or stream-ready timeout");
                if ((record_ || play_) && !streams_ready_ && Clock::now() > stream_deadline_)
                    fault("audio stream-ready timeout");
                if (streams_ready_) {
                    apply_reset();
                    complete_reset();
                    if (status_.reset_pending && Clock::now() > reset_deadline_)
                        fault("audio server flush acknowledgement timeout");
                    service_output();
                    process_capture(); finish_capture();
                    service_output();
                    check_active_streams();
                    if (drain_requested_ && !status_.drained && Clock::now() > drain_deadline_)
                        fault("playback drain timeout");
                }
                pump_events();
                wait_events(lock);
            } catch (const std::exception& error) { fault(std::string("audio worker error: ") + error.what()); }
            catch (...) { fault("audio worker encountered an unknown error"); }
        }
        teardown();
    }
    static void context_state(pa_context* context, void* userdata) noexcept {
        auto& self = *static_cast<PulseAudio*>(userdata);
        try {
            const auto state = pa_context_get_state(context);
            if (state == PA_CONTEXT_READY) {
                pa_context_set_subscribe_callback(context, subscription, &self);
                release(pa_context_subscribe(context, static_cast<pa_subscription_mask_t>(
                    PA_SUBSCRIPTION_MASK_SOURCE | PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_CARD |
                    PA_SUBSCRIPTION_MASK_SERVER), nullptr, nullptr));
                self.next_discovery_ = Clock::now();
                self.discover();
            } else if (state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED)
                self.fault("audio service disconnected");
        } catch (...) { self.fault("context callback failed"); }
    }
    static void subscription(pa_context*, pa_subscription_event_type_t type, std::uint32_t index,
                             void* userdata) noexcept {
        auto& self = *static_cast<PulseAudio*>(userdata);
        try {
            const auto facility = type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
            const auto change = type & PA_SUBSCRIPTION_EVENT_TYPE_MASK;
            const bool target = (facility == PA_SUBSCRIPTION_EVENT_SOURCE && index == self.bound_source_.index) ||
                (facility == PA_SUBSCRIPTION_EVENT_SINK && index == self.bound_sink_.index) ||
                (facility == PA_SUBSCRIPTION_EVENT_CARD && index == self.bound_source_.card);
            if (target && change == PA_SUBSCRIPTION_EVENT_REMOVE)
                self.fault("configured device or card was removed");
            self.next_discovery_ = Clock::now();
        } catch (...) { self.fault("device subscription callback failed"); }
    }
    static void source_info(pa_context*, const pa_source_info* info, int eol, void* userdata) noexcept {
        auto& self = *static_cast<PulseAudio*>(userdata);
        try {
            if (info && !eol) {
                if (info->monitor_of_sink != PA_INVALID_INDEX || safe(info->name) != self.options_.source) {
                    self.fault("configured source is a monitor or has an unexpected name"); return;
                }
                if (info->active_port && info->active_port->available == PA_PORT_AVAILABLE_NO) {
                    self.fault("configured source active port is unavailable"); return;
                }
                self.candidate_source_ = {true, safe(info->name), identity(info->proplist),
                                          info->index, info->card, info->sample_spec};
            }
            if (eol) { self.source_done_ = true; self.check_discovery(); }
        } catch (...) { self.fault("source metadata callback failed"); }
    }
    static void sink_info(pa_context*, const pa_sink_info* info, int eol, void* userdata) noexcept {
        auto& self = *static_cast<PulseAudio*>(userdata);
        try {
            if (info && !eol) {
                if (safe(info->name) != self.options_.sink) {
                    self.fault("configured sink has an unexpected name"); return;
                }
                if (info->active_port && info->active_port->available == PA_PORT_AVAILABLE_NO) {
                    self.fault("configured sink active port is unavailable"); return;
                }
                self.candidate_sink_ = {true, safe(info->name), identity(info->proplist),
                                        info->index, info->card, info->sample_spec};
            }
            if (eol) { self.sink_done_ = true; self.check_discovery(); }
        } catch (...) { self.fault("sink metadata callback failed"); }
    }
    static void card_info(pa_context*, const pa_card_info* info, int eol, void* userdata) noexcept {
        auto& self = *static_cast<PulseAudio*>(userdata);
        try {
            if (info && !eol) self.finish_discovery(safe(info->name),
                info->active_profile ? safe(info->active_profile->name) : "", identity(info->proplist));
            if (eol < 0) self.fault("configured card profile is unavailable");
        } catch (...) { self.fault("card metadata callback failed"); }
    }
    static void stream_state(pa_stream* stream, void* userdata) noexcept {
        auto& self = *static_cast<PulseAudio*>(userdata);
        try {
            const auto state = pa_stream_get_state(stream);
            if (state == PA_STREAM_FAILED || state == PA_STREAM_TERMINATED)
                self.fault("explicit audio stream disconnected");
            else if (state == PA_STREAM_READY) self.check_streams();
        } catch (...) { self.fault("stream state callback failed"); }
    }
    static void moved(pa_stream*, void* userdata) noexcept {
        static_cast<PulseAudio*>(userdata)->fault("audio stream moved; refusing automatic fallback");
    }
    static void suspended(pa_stream*, void* userdata) noexcept {
        auto& self = *static_cast<PulseAudio*>(userdata);
        try { self.check_active_streams(); }
        catch (...) { self.fault("suspend state callback failed"); }
    }
    static void overflow(pa_stream* stream, void* userdata) noexcept {
        auto& self = *static_cast<PulseAudio*>(userdata);
        if (stream == self.keepalive_) {
            self.fault("keepalive server stream overflow"); return;
        }
        if (stream == self.record_) ++self.status_.capture_overflows;
        else ++self.status_.playback_overflows;
        self.fault("audio server stream overflow; generation cancelled");
    }
    static void underflow(pa_stream* stream, void* userdata) noexcept {
        auto& self = *static_cast<PulseAudio*>(userdata);
        if (stream == self.keepalive_) {
            if (self.keepalive_uncorked_) {
                ++self.status_.playback_keepalive_underruns;
                try { self.emit("playback_keepalive_underrun", "silent transport buffer underrun; " + self.service_detail()); }
                catch (...) { self.fault("keepalive underflow callback failed"); }
            }
            return;
        }
        if (self.status_.session_active && self.playback_transport_started_ && !self.drain_requested_ &&
            !self.status_.reset_pending) {
            ++self.status_.playback_underruns;
            try { self.emit("playback_underrun", "server playback buffer underrun after transport started; " + self.service_detail()); }
            catch (...) { self.fault("underflow event callback failed"); }
        }
    }
    static void read_ready(pa_stream* stream, std::size_t, void* userdata) noexcept {
        auto& self = *static_cast<PulseAudio*>(userdata);
        try {
            const void* data = nullptr; std::size_t bytes = 0;
            if (pa_stream_peek(stream, &data, &bytes) < 0) { self.fault("capture peek failed"); return; }
            if (!bytes) return;
            if (!data) {
                ++self.status_.capture_holes;
                if (self.status_.capturing || self.status_.capture_stopping)
                    self.fault("capture hole; affected generation cancelled");
            }
            if ((self.status_.capturing || self.status_.capture_stopping) &&
                (!self.status_.reset_pending || self.capture_preserved_reset_) && !self.fault_pending_) {
                const auto channels = self.bound_source_.spec.channels;
                const std::size_t stride = channels * sizeof(float);
                const std::size_t samples = bytes / sizeof(float);
                const double queued_seconds = static_cast<double>(self.capture_.size()) / 50.0 +
                    static_cast<double>(self.assembler_.pending_samples()) / 16000.0 +
                    static_cast<double>(self.raw_samples_ + samples) / channels / self.bound_source_.spec.rate;
                if (bytes % stride != 0 || queued_seconds > self.options_.capture_capacity_frames / 50.0 + 1e-6) {
                    ++self.status_.capture_overflows;
                    self.fault("bounded capture input queue overflow or misaligned fragment");
                } else {
                    // The callback copies at most the configured queue budget.
                    // Conversion, resampling and framing run outside callbacks.
                    std::vector<float> chunk(samples, 0.0F);
                    if (data) std::copy_n(static_cast<const float*>(data), samples, chunk.begin());
                    self.raw_samples_ += samples; self.raw_.push_back(std::move(chunk));
                    self.status_.capture_native_frames += samples / channels;
                    self.status_.raw_capture_peak_bytes = std::max(self.status_.raw_capture_peak_bytes,
                        self.raw_samples_ * sizeof(float));
                }
            }
            if (pa_stream_drop(stream) < 0) self.fault("capture drop failed");
        } catch (...) { self.fault("capture callback failed"); }
    }
    static void flush_done(pa_stream* stream, int success, void* userdata) noexcept {
        auto& self = *static_cast<PulseAudio*>(userdata);
        if (self.applied_epoch_ != self.epoch_) return;
        if (!success) { self.fault("server rejected audio flush"); return; }
        if (stream == self.record_) self.capture_flush_done_ = true;
        if (stream == self.play_) self.playback_flush_done_ = true;
    }
    static void capture_cork_done(pa_stream*, int success, void* userdata) noexcept {
        auto& self = *static_cast<PulseAudio*>(userdata);
        if (self.stop_epoch_ != self.epoch_ || !self.status_.capture_stopping) return;
        if (!success) { self.fault("server rejected capture cork"); return; }
        self.stop_cork_done_ = true;
    }
    static void cork_done(pa_stream*, int success, void* userdata) noexcept {
        auto& self = *static_cast<PulseAudio*>(userdata);
        // Operations from an old epoch are cancelled in apply_reset. Until
        // then a late acknowledgement must not alter the new generation.
        if (self.applied_epoch_ != self.epoch_) return;
        if (!success) self.fault("server rejected audio stream cork/uncork");
    }
    static void keepalive_cork_done(pa_stream* stream, int success, void* userdata) noexcept {
        auto& self = *static_cast<PulseAudio*>(userdata);
        // This operation is backend-scoped, not tied to a speech epoch.
        if (stream != self.keepalive_) return;
        if (!success) { self.fault("server rejected keepalive uncork"); return; }
        self.keepalive_uncorked_ = true;
    }
    static void drain_done(pa_stream*, int success, void* userdata) noexcept {
        auto& self = *static_cast<PulseAudio*>(userdata);
        if (self.applied_epoch_ != self.epoch_) return;
        if (!success) { self.fault("server rejected playback drain"); return; }
        self.status_.drained = true;
        self.playback_started_ = false;
        self.playback_transport_started_ = false;
        self.lead_in_remaining_frames_ = 0;
        try { self.emit("playback_drained", "server playback drain acknowledged"); }
        catch (...) { self.fault("drain event callback failed"); }
    }

    LiveAudioOptions options_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread worker_;
    bool stopping_ = false;
    LiveAudioStatus status_;
    std::deque<LiveAudioEvent> events_;
    std::deque<AudioFrame> capture_, playback_;
    std::deque<std::vector<float>> raw_;
    std::size_t raw_samples_ = 0, raw_offset_ = 0;
    std::int64_t last_output_service_ns_ = 0;
    std::vector<float> native_playback_;
    std::size_t native_offset_ = 0, lead_in_remaining_frames_ = 0;
    std::unique_ptr<StreamResampler> capture_dsp_, playback_dsp_;
    MonoFrameAssembler assembler_;
    pa_mainloop* loop_ = nullptr;
    pa_context* context_ = nullptr;
    pa_stream* record_ = nullptr;
    pa_stream* play_ = nullptr;
    pa_stream* keepalive_ = nullptr;
    pa_operation* keepalive_cork_ = nullptr;
    bool keepalive_started_ = false, keepalive_uncorked_ = false;
    pa_operation* capture_flush_ = nullptr;
    pa_operation* playback_flush_ = nullptr;
    pa_operation* drain_operation_ = nullptr;
    pa_operation* stop_cork_ = nullptr;
    pa_operation* capture_cork_ = nullptr;
    pa_operation* playback_cork_ = nullptr;
    Device candidate_source_, candidate_sink_, bound_source_, bound_sink_;
    bool discovering_ = false, source_done_ = false, sink_done_ = false;
    bool streams_ready_ = false, identity_bound_ = false;
    bool ready_once_ = false;
    bool capture_preserved_reset_ = false;
    bool desired_capture_ = false, stop_capture_pending_ = false;
    bool stop_cork_done_ = false;
    bool capture_flush_done_ = false, playback_flush_done_ = false;
    bool drain_requested_ = false, tail_finished_ = false, playback_started_ = false;
    bool playback_transport_started_ = false;
    bool last_context_valid_ = false, fault_pending_ = false;
    AudioContext last_context_;
    std::string current_card_, expected_source_identity_, expected_sink_identity_, fault_reason_;
    std::uint64_t epoch_ = 0, applied_epoch_ = 0, capture_seq_ = 0, stop_epoch_ = 0;
    std::size_t retry_attempt_ = 0;
    Clock::time_point retry_at_{}, deadline_{}, next_discovery_{}, reset_deadline_{}, stream_deadline_{},
                      stop_deadline_{}, drain_deadline_{}, capture_started_at_{}, playback_started_at_{}, keepalive_started_at_{};
};
}  // namespace

const char* live_audio_state_name(LiveAudioState state) noexcept {
    switch (state) {
        case LiveAudioState::device_wait: return "DEVICE_WAIT";
        case LiveAudioState::ready: return "READY";
        case LiveAudioState::capturing: return "CAPTURING";
    }
    return "DEVICE_WAIT";
}

std::unique_ptr<LiveAudioBackend> make_pulse_audio(const LiveAudioOptions& options) {
    return std::make_unique<PulseAudio>(options);
}
}  // namespace voice_engine
