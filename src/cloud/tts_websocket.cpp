#include "tts_websocket.hpp"
#include "websocket.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <vector>

namespace voice_engine::cloud {
namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;

class Synthesis {
public:
    Synthesis(const CloudTtsConfig& config, const std::string& text, int max_audio_ms,
              Cancellation& token, const PcmSink& sink)
        : config_(config), text_(text), token_(token), sink_(sink),
          endpoint_(parse_websocket_endpoint(config.endpoint)),
          audio_limit_(2 * std::size_t(config.sample_rate) * max_audio_ms / 1000) {}

    void run(const std::string& ca_file) {
        static const lws_protocols protocols[] = {
            {"bailian-tts", &callback, 0, 4096, 0, nullptr, 0}, {}
        };
        static const bool quiet = [] { lws_set_log_level(0, nullptr); return true; }();
        (void)quiet;
        if (token_.requested()) throw Error("cancelled");
        lws_context_creation_info options{};
        options.port = CONTEXT_PORT_NO_LISTEN;
        options.protocols = protocols;
        options.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
        options.gid = -1;
        options.uid = -1;
        options.user = this;
        options.http_proxy_address = "";
        options.socks_proxy_address = "";
        options.client_ssl_ca_filepath = ca_file.empty() ? nullptr : ca_file.c_str();
        std::unique_ptr<lws_context, decltype(&lws_context_destroy)> context(
            lws_create_context(&options), lws_context_destroy);
        if (!context) throw Error("streaming_tts_context_failed");
        Cancellation::Binding wake(token_, [&] { lws_cancel_service(context.get()); });
        task_id_ = websocket_task_id(context.get());
        lws_client_connect_info connect{};
        connect.context = context.get();
        connect.address = endpoint_.host.c_str();
        connect.host = endpoint_.host.c_str();
        connect.port = endpoint_.port;
        connect.path = endpoint_.path.c_str();
        connect.ssl_connection = LCCSCF_USE_SSL | LCCSCF_HTTP_NO_FOLLOW_REDIRECT;
        connect.local_protocol_name = protocols[0].name;
        connect.pwsi = &socket_;
        const auto start = Clock::now();
        if (!lws_client_connect_via_info(&connect))
            throw Error(error_.empty() ? "streaming_tts_connect_failed" : error_);
        lws_sorted_usec_list_t tick{};
        while (stage_ != Stage::done && error_.empty()) {
            if (token_.requested()) throw Error("cancelled");
            const auto now = Clock::now();
            if (now - start >= Ms(config_.total_ms)) throw Error("streaming_tts_total_timeout");
            if (stage_ == Stage::connecting && now - start >= Ms(config_.connect_ms))
                throw Error("streaming_tts_connect_timeout");
            if (stage_ == Stage::starting && now - sent_ >= Ms(config_.first_response_ms))
                throw Error("streaming_tts_start_timeout");
            if (stage_ == Stage::receiving) {
                if (!audio_bytes_ && now - sent_ >= Ms(config_.first_response_ms))
                    throw Error("streaming_tts_first_audio_timeout");
                if (now - received_ >= Ms(config_.idle_ms)) throw Error("streaming_tts_idle_timeout");
            }
            if (stage_ == Stage::run || stage_ == Stage::text || stage_ == Stage::finish)
                lws_callback_on_writable(socket_);
            lws_sul_schedule(context.get(), 0, &tick, [](lws_sorted_usec_list_t*) {}, 50000);
            const int rc = lws_service(context.get(), 0);
            lws_sul_schedule(context.get(), 0, &tick, nullptr, LWS_SET_TIMER_USEC_CANCEL);
            if (rc < 0) throw Error("streaming_tts_service_failed");
        }
        if (token_.requested()) throw Error("cancelled");
        if (!error_.empty()) throw Error(error_);
    }

private:
    enum class Stage { connecting, run, starting, text, finish, receiving, done };
    const CloudTtsConfig& config_;
    const std::string& text_;
    Cancellation& token_;
    const PcmSink& sink_;
    const WebSocketEndpoint endpoint_;
    const std::size_t audio_limit_;
    Stage stage_ = Stage::connecting;
    lws* socket_ = nullptr;
    Clock::time_point sent_{}, received_{};
    std::string task_id_, message_, error_;
    std::size_t request_bytes_ = 0, response_bytes_ = 0, audio_bytes_ = 0;
    int low_byte_ = -1;

    Json command(const char* action) const {
        return {{"header", {{"action", action}, {"task_id", task_id_}, {"streaming", "duplex"}}},
                {"payload", {{"input", Json::object()}}}};
    }

    void write(lws* socket) {
        Json request;
        Stage next;
        switch (stage_) {
        case Stage::run:
            request = command("run-task");
            request["payload"].update({
                {"task_group", "audio"}, {"task", "tts"}, {"function", "SpeechSynthesizer"},
                {"model", config_.model}, {"parameters", {
                    {"text_type", "PlainText"}, {"voice", config_.voice}, {"format", "pcm"},
                    {"sample_rate", config_.sample_rate}, {"rate", config_.speed}
                }}
            });
            next = Stage::starting;
            break;
        case Stage::text:
            request = command("continue-task");
            request["payload"]["input"]["text"] = text_;
            next = Stage::finish;
            break;
        case Stage::finish:
            request = command("finish-task");
            next = Stage::receiving;
            break;
        default: return;
        }
        const auto bytes = request.dump();
        if (bytes.size() > std::size_t(config_.max_request_bytes) - request_bytes_)
            throw Error("streaming_tts_request_limit");
        request_bytes_ += bytes.size();
        std::vector<unsigned char> buffer(LWS_PRE + bytes.size());
        std::copy(bytes.begin(), bytes.end(), buffer.begin() + LWS_PRE);
        if (lws_write(socket, buffer.data() + LWS_PRE, bytes.size(), LWS_WRITE_TEXT) != int(bytes.size()))
            throw Error("streaming_tts_write_failed");
        stage_ = next;
        if (next == Stage::starting || next == Stage::finish) sent_ = Clock::now();
        if (next == Stage::receiving) received_ = Clock::now();
    }

    void receive_audio(const unsigned char* bytes, std::size_t size) {
        if (stage_ != Stage::finish && stage_ != Stage::receiving)
            throw Error("streaming_tts_unexpected_audio");
        if (size > audio_limit_ - audio_bytes_) throw Error("streaming_tts_audio_limit");
        audio_bytes_ += size;
        std::array<float, 1024> samples{};
        std::size_t count = 0;
        for (std::size_t i = 0; i < size; ++i) {
            if (low_byte_ < 0) {
                low_byte_ = bytes[i];
                continue;
            }
            const int value = low_byte_ | (int(bytes[i]) << 8);
            low_byte_ = -1;
            samples[count++] = (value < 32768 ? value : value - 65536) / 32768.0f;
            if (count == samples.size()) {
                if (!sink_(samples.data(), count)) throw Error("cancelled");
                count = 0;
            }
        }
        if (count && !sink_(samples.data(), count)) throw Error("cancelled");
    }

    void receive_event() {
        const auto event = Json::parse(message_, [](int depth, auto, auto&) {
            if (depth > 32) throw Error("streaming_tts_json_depth");
            return true;
        });
        const auto& header = event.at("header");
        if (header.at("task_id") != task_id_) return;
        const auto name = header.at("event").get<std::string>();
        if (name == "task-failed") throw Error("streaming_tts_task_failed");
        if (name == "task-started" && stage_ == Stage::starting) {
            stage_ = Stage::text;
        } else if (name == "task-finished" && stage_ == Stage::receiving) {
            if (!audio_bytes_ || low_byte_ >= 0) throw Error("streaming_tts_invalid_pcm");
            stage_ = Stage::done;
        } else if (name != "result-generated" || (stage_ != Stage::finish && stage_ != Stage::receiving)) {
            throw Error("streaming_tts_unexpected_event");
        }
    }

    static int callback(lws* socket, lws_callback_reasons reason, void*, void* in, std::size_t len) {
        if (!socket) return 0;
        auto* self = static_cast<Synthesis*>(lws_context_user(lws_get_context(socket)));
        if (!self || self->stage_ == Stage::done || !self->error_.empty()) return 0;
        try {
            switch (reason) {
            case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER: {
                if (self->token_.requested()) throw Error("cancelled");
                auto** p = static_cast<unsigned char**>(in);
                const auto auth = "Bearer " + cloud_api_key(self->config_);
                if (lws_add_http_header_by_token(socket, WSI_TOKEN_HTTP_AUTHORIZATION,
                        reinterpret_cast<const unsigned char*>(auth.data()), int(auth.size()),
                        p, *p + len) != 0)
                    throw Error("streaming_tts_auth_header_failed");
                break;
            }
            case LWS_CALLBACK_CLIENT_ESTABLISHED:
                self->stage_ = Stage::run;
                break;
            case LWS_CALLBACK_CLIENT_WRITEABLE:
                if (self->token_.requested()) throw Error("cancelled");
                self->write(socket);
                break;
            case LWS_CALLBACK_CLIENT_RECEIVE:
                if (self->token_.requested()) throw Error("cancelled");
                if (len > std::size_t(self->config_.max_response_bytes) - self->response_bytes_)
                    throw Error("streaming_tts_response_limit");
                self->response_bytes_ += len;
                self->received_ = Clock::now();
                if (lws_frame_is_binary(socket)) {
                    self->receive_audio(static_cast<const unsigned char*>(in), len);
                } else {
                    self->message_.append(static_cast<const char*>(in), len);
                    if (lws_is_final_fragment(socket) && !lws_remaining_packet_payload(socket)) {
                        self->receive_event();
                        self->message_.clear();
                    }
                }
                break;
            case LWS_CALLBACK_CLIENT_CONNECTION_ERROR: {
                const auto status = lws_http_client_http_response(socket);
                throw Error(status == 401 || status == 403 ? "streaming_tts_auth_failed" :
                            status == 429 ? "streaming_tts_rate_limited" : "streaming_tts_connect_failed");
            }
            case LWS_CALLBACK_CLIENT_CLOSED:
                throw Error("streaming_tts_connection_closed");
            default: break;
            }
            return 0;
        } catch (const Error& error) {
            self->error_ = error.what();
        } catch (...) {
            self->error_ = "streaming_tts_protocol_error";
        }
        // Leave teardown to the service loop, including cancellation wakeups
        // and callbacks emitted by lws_context_destroy.
        return 0;
    }
};
} // namespace

void stream_speech(const CloudTtsConfig& config, const std::string& text, int max_audio_ms,
                   Cancellation& token, const PcmSink& sink, const std::string& ca_file) {
    Synthesis(config, text, max_audio_ms, token, sink).run(ca_file);
}
} // namespace voice_engine::cloud
