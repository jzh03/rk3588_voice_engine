#include "asr_stream.hpp"
#include "websocket.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace voice_engine::cloud {
namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;
constexpr std::size_t bytes_per_ms = 32;
constexpr std::size_t max_audio_frame = 100 * bytes_per_ms;

std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

std::string to_pcm16(const float* pcm, std::size_t count) {
    std::string bytes;
    bytes.reserve(2 * count);
    for (std::size_t n = 0; n < count; ++n) {
        const auto value = static_cast<std::uint16_t>(std::lround(std::clamp(pcm[n], -1.0f, 1.0f) * 32767.0f));
        bytes += static_cast<char>(value & 255);
        bytes += static_cast<char>(value >> 8);
    }
    return bytes;
}
}

void validate_streaming_asr(const CloudAsrConfig& config) {
    parse_websocket_endpoint(config.endpoint);
}

struct StreamingAsr::Impl {
    const CloudAsrConfig config;
    const WebSocketEndpoint endpoint;
    const std::size_t text_limit, pre_roll_limit, queue_limit;
    const std::string ca_file;
    Cancellation& cancellation;
    std::mutex mutex;
    std::condition_variable changed;
    std::thread worker;
    lws_context* context = nullptr; // Published under mutex for cross-thread wakeup.
    bool stopped = false, done = false, speech = false, end = false;
    Clock::time_point queued_at{}, finish_deadline{};
    std::string pre_roll, error;
    std::deque<std::string> audio;
    std::size_t queued_bytes = 0, accepted_bytes = 0;
    StreamingAsrResult result;

    // Only accessed on the network thread.
    lws* socket = nullptr;
    bool connected = false, run_sent = false, ready = false, finish_sent = false;
    Clock::time_point connect_deadline{}, ready_deadline{};
    std::string task_id, message;
    std::size_t response_bytes = 0, final_bytes = 0;
    std::map<int, std::string> sentences;

    Impl(const CloudAsrConfig& c, std::size_t text_limit, Cancellation& token, const std::string& ca_file)
        : config(c), endpoint(parse_websocket_endpoint(c.endpoint)), text_limit(text_limit),
          pre_roll_limit(bytes_per_ms * c.stream_pre_roll_ms), queue_limit(bytes_per_ms * c.stream_queue_ms),
          ca_file(ca_file), cancellation(token) {
        worker = std::thread([this] { service(); });
    }

    ~Impl() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopped = true;
            wake_locked();
        }
        worker.join();
    }

    void wake_locked() {
        if (context) lws_cancel_service(context);
        changed.notify_all();
    }

    void terminate(const char* reason = nullptr) {
        std::lock_guard<std::mutex> lock(mutex);
        if (done) return;
        if (reason) error = reason;
        done = true;
        audio.clear();
        queued_bytes = 0;
        wake_locked();
    }

    void enqueue(const std::string& bytes) {
        for (std::size_t offset = 0; offset < bytes.size(); offset += max_audio_frame)
            audio.push_back(bytes.substr(offset, max_audio_frame));
        if (!queued_bytes && !bytes.empty()) queued_at = Clock::now();
        queued_bytes += bytes.size();
        accepted_bytes += bytes.size();
    }

    void append(const float* pcm, std::size_t count, bool confirmed) {
        if (count > 16000 || (count && !pcm)) throw Error("invalid_streaming_asr_pcm");
        auto bytes = to_pcm16(pcm, count);
        std::lock_guard<std::mutex> lock(mutex);
        if (cancellation.requested()) throw Error("cancelled");
        if (!speech && !confirmed) {
            pre_roll += bytes;
            if (pre_roll.size() > pre_roll_limit) pre_roll.erase(0, pre_roll.size() - pre_roll_limit);
            return;
        }
        if (done) throw Error(error.empty() ? "streaming_asr_closed" : error);
        if (end) throw Error("streaming_asr_input_closed");
        const auto new_bytes = bytes.size() + pre_roll.size();
        if (new_bytes > queue_limit - queued_bytes) throw Error("streaming_asr_queue_full");
        if (new_bytes > static_cast<std::size_t>(config.max_request_bytes) - accepted_bytes)
            throw Error("streaming_asr_audio_limit");
        if (!speech) {
            speech = true;
            enqueue(pre_roll);
            pre_roll.clear();
        }
        enqueue(bytes);
        wake_locked();
    }

    StreamingAsrResult finish() {
        std::unique_lock<std::mutex> lock(mutex);
        if (cancellation.requested()) throw Error("cancelled");
        if (!speech || !accepted_bytes) return {};
        end = true;
        finish_deadline = Clock::now() + Ms(config.total_ms);
        wake_locked();
        if (!changed.wait_until(lock, finish_deadline, [&] { return done || cancellation.requested(); }))
            throw Error("streaming_asr_final_timeout");
        if (cancellation.requested()) throw Error("cancelled");
        if (!error.empty()) throw Error(error);
        return result;
    }

    Json command(const char* action) const {
        return {{"header", {{"action", action}, {"task_id", task_id}, {"streaming", "duplex"}}},
                {"payload", {{"input", Json::object()}}}};
    }

    std::string run_task() const {
        auto request = command("run-task");
        Json parameters = {{"format", "pcm"}, {"sample_rate", 16000}, {"vad_model", "near_meeting_16k"}};
        if (!config.language.empty()) parameters["language_hints"] = Json::array({config.language});
        request["payload"].update({{"task_group", "audio"}, {"task", "asr"}, {"function", "recognition"},
                                  {"model", config.model}, {"parameters", std::move(parameters)}});
        return request.dump();
    }

    void receive(const std::string& bytes) {
        const auto reply = Json::parse(bytes, [](int depth, auto, auto&) {
            if (depth > 32) throw Error("streaming_asr_json_depth");
            return true;
        });
        const auto& header = reply.at("header");
        if (header.at("task_id") != task_id) return;
        const auto event = header.at("event").get<std::string>();
        if (event == "task-failed") throw Error("streaming_asr_task_failed");
        if (event == "task-started") {
            if (!run_sent || ready) throw Error("streaming_asr_invalid_task_state");
            ready = true;
            result.task_ready_ns = now_ns();
            return;
        }
        if (!ready) throw Error("streaming_asr_invalid_task_state");
        if (event == "result-generated") {
            const auto& sentence = reply.at("payload").at("output").at("sentence");
            if (sentence.value("heartbeat", false) || !sentence.value("sentence_end", false)) return;
            const auto id = sentence.at("sentence_id").get<int>();
            auto text = sentence.at("text").get<std::string>();
            if (id < 1 || text.find('\0') != std::string::npos) throw Error("streaming_asr_invalid_sentence");
            auto& slot = sentences[id];
            final_bytes -= slot.size();
            if (text.size() > text_limit - final_bytes) throw Error("streaming_asr_text_limit");
            final_bytes += text.size();
            slot = std::move(text);
        } else if (event == "task-finished") {
            if (!finish_sent) throw Error("streaming_asr_premature_finish");
            for (const auto& [id, text] : sentences) {
                (void)id;
                result.text += text;
            }
            terminate();
        } else {
            throw Error("streaming_asr_unexpected_event");
        }
    }

    void write_frame(lws* wsi) {
        std::string bytes;
        lws_write_protocol type = LWS_WRITE_TEXT;
        bool final = false;
        if (!run_sent) {
            bytes = run_task();
        } else {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopped || done || !ready) return;
            if (!audio.empty()) {
                bytes = std::move(audio.front());
                audio.pop_front();
                queued_bytes -= bytes.size();
                type = LWS_WRITE_BINARY;
            } else if (end && !finish_sent) {
                bytes = command("finish-task").dump();
                final = true;
            } else return;
        }
        if (cancellation.requested()) throw Error("cancelled");
        std::vector<unsigned char> buffer(LWS_PRE + bytes.size());
        std::copy(bytes.begin(), bytes.end(), buffer.begin() + LWS_PRE);
        if (lws_write(wsi, buffer.data() + LWS_PRE, bytes.size(), type) != static_cast<int>(bytes.size()))
            throw Error("streaming_asr_write_failed");
        if (type == LWS_WRITE_BINARY) {
            if (!result.audio_bytes) result.first_audio_sent_ns = now_ns();
            result.audio_bytes += bytes.size();
            std::lock_guard<std::mutex> lock(mutex);
            queued_at = Clock::now();
        } else if (final) {
            finish_sent = true;
            result.finish_sent_ns = now_ns();
        } else {
            run_sent = true;
            ready_deadline = Clock::now() + Ms(config.first_response_ms);
        }
    }

    static int callback(lws* wsi, lws_callback_reasons reason, void*, void* in, std::size_t len) {
        if (!wsi) return 0;
        auto& self = *static_cast<Impl*>(lws_context_user(lws_get_context(wsi)));
        try {
            switch (reason) {
            case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER: {
                auto** p = static_cast<unsigned char**>(in);
                const auto key = "Bearer " + cloud_api_key(self.config);
                if (lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_AUTHORIZATION,
                        reinterpret_cast<const unsigned char*>(key.data()), static_cast<int>(key.size()),
                        p, *p + len) != 0) throw Error("streaming_asr_auth_header_failed");
                break;
            }
            case LWS_CALLBACK_CLIENT_ESTABLISHED:
                self.connected = true;
                break;
            case LWS_CALLBACK_CLIENT_WRITEABLE:
                self.write_frame(wsi);
                break;
            case LWS_CALLBACK_CLIENT_RECEIVE:
                if (self.cancellation.requested()) throw Error("cancelled");
                if (lws_frame_is_binary(wsi)) throw Error("streaming_asr_unexpected_binary");
                if (len > static_cast<std::size_t>(self.config.max_response_bytes) - self.response_bytes)
                    throw Error("streaming_asr_response_limit");
                self.response_bytes += len;
                self.message.append(static_cast<const char*>(in), len);
                if (lws_is_final_fragment(wsi) && !lws_remaining_packet_payload(wsi)) {
                    self.receive(self.message);
                    self.message.clear();
                }
                break;
            case LWS_CALLBACK_CLIENT_CONNECTION_ERROR: {
                const auto status = lws_http_client_http_response(wsi);
                self.terminate(status == 401 || status == 403 ? "streaming_asr_auth_failed" :
                               status == 429 ? "streaming_asr_rate_limited" : "streaming_asr_connect_failed");
                break;
            }
            case LWS_CALLBACK_CLIENT_CLOSED:
                self.terminate("streaming_asr_connection_closed");
                break;
            default: break;
            }
            return 0;
        } catch (const Error& e) {
            self.terminate(e.what());
        } catch (...) {
            self.terminate("streaming_asr_protocol_error");
        }
        return -1;
    }

    void run(lws_context* ctx) {
        if (cancellation.requested()) throw Error("cancelled");
        task_id = websocket_task_id(ctx);
        lws_client_connect_info info{};
        info.context = ctx;
        info.address = endpoint.host.c_str();
        info.host = endpoint.host.c_str();
        info.port = endpoint.port;
        info.path = endpoint.path.c_str();
        info.ssl_connection = LCCSCF_USE_SSL | LCCSCF_HTTP_NO_FOLLOW_REDIRECT;
        info.local_protocol_name = "bailian-asr";
        info.pwsi = &socket;
        connect_deadline = Clock::now() + Ms(config.connect_ms);
        if (!lws_client_connect_via_info(&info)) throw Error("streaming_asr_connect_failed");

        lws_sorted_usec_list_t tick{};
        for (;;) {
            bool writable;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (done || stopped || cancellation.requested()) break;
                const auto now = Clock::now();
                if (!connected && now >= connect_deadline) throw Error("streaming_asr_connect_timeout");
                if (run_sent && !ready && now >= ready_deadline) throw Error("streaming_asr_task_start_timeout");
                if (end && now >= finish_deadline) throw Error("streaming_asr_final_timeout");
                if (queued_bytes && now - queued_at >= Ms(config.idle_ms))
                    throw Error("streaming_asr_send_timeout");
                writable = connected && (!run_sent || (ready && (queued_bytes || (end && !finish_sent))));
            }
            if (writable) lws_callback_on_writable(socket);
            // LWS 4.x uses scheduled timers, rather than lws_service's timeout argument.
            lws_sul_schedule(ctx, 0, &tick, [](lws_sorted_usec_list_t*) {}, 50000);
            const int rc = lws_service(ctx, 0);
            lws_sul_schedule(ctx, 0, &tick, nullptr, LWS_SET_TIMER_USEC_CANCEL);
            if (rc < 0) throw Error("streaming_asr_service_failed");
        }
        terminate(cancellation.requested() ? "cancelled" : "streaming_asr_closed");
    }

    void service() noexcept {
        static const lws_protocols protocols[] = {
            {"bailian-asr", &callback, 0, 4096, 0, nullptr, 0}, {}
        };
        static const bool quiet = [] { lws_set_log_level(0, nullptr); return true; }();
        (void)quiet;
        lws_context_creation_info info{};
        info.port = CONTEXT_PORT_NO_LISTEN;
        info.protocols = protocols;
        info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
        info.gid = -1;
        info.uid = -1;
        info.user = this;
        // Match the HTTP backends' direct connection policy, independent of
        // proxy variables inherited from the interactive shell.
        info.http_proxy_address = "";
        info.socks_proxy_address = "";
        info.client_ssl_ca_filepath = ca_file.empty() ? nullptr : ca_file.c_str();
        std::unique_ptr<lws_context, decltype(&lws_context_destroy)> ctx(lws_create_context(&info), lws_context_destroy);
        if (!ctx) {
            terminate("streaming_asr_context_failed");
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            context = ctx.get();
        }
        try {
            Cancellation::Binding interrupt(cancellation, [this] {
                std::lock_guard<std::mutex> lock(mutex);
                wake_locked();
            });
            run(ctx.get());
        } catch (const Error& e) {
            terminate(e.what());
        } catch (...) {
            terminate("streaming_asr_setup_failed");
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            context = nullptr;
        }
    }
};

StreamingAsr::StreamingAsr(const CloudAsrConfig& config, std::size_t text_limit,
                           Cancellation& cancellation, const std::string& ca_file)
    : impl_(std::make_unique<Impl>(config, text_limit, cancellation, ca_file)) {}
StreamingAsr::~StreamingAsr() = default;

void StreamingAsr::append(const float* pcm, std::size_t count, bool confirmed) {
    impl_->append(pcm, count, confirmed);
}
StreamingAsrResult StreamingAsr::finish() { return impl_->finish(); }

} // namespace voice_engine::cloud
