#include "http.hpp"

#include <curl/curl.h>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <arpa/inet.h>
#include <sys/socket.h>

namespace voice_engine::cloud {
namespace {
using Clock = std::chrono::steady_clock;
using Milliseconds = std::chrono::milliseconds;

void require(CURLcode result) {
    if (result != CURLE_OK) throw Error("http_setup_failed");
}

void initialize() {
    static const CURLcode result = curl_global_init(CURL_GLOBAL_DEFAULT);
    require(result);
    const auto* version = curl_version_info(CURLVERSION_NOW);
    if (!(version->features & CURL_VERSION_ASYNCHDNS) || !(version->features & CURL_VERSION_SSL))
        throw ConfigError("cloud requires libcurl with asynchronous DNS and TLS");
}

struct Request {
    CURL* easy;
    Cancellation& cancellation;
    const ResponseSink& sink;
    std::string_view expected_type;
    std::size_t limit;
    Clock::time_point start = Clock::now(), last_progress = start;
    curl_off_t uploaded = 0, downloaded = 0;
    std::size_t received = 0, header_bytes = 0;
    bool responded = false;
    std::string error_body{};
    std::exception_ptr write_error{};

    static int progress(void* data, curl_off_t, curl_off_t down, curl_off_t, curl_off_t up) noexcept {
        auto& r = *static_cast<Request*>(data);
        if (up != r.uploaded || down != r.downloaded) {
            r.last_progress = Clock::now();
            r.uploaded = up;
            r.downloaded = down;
        }
        return r.cancellation.requested() ? 1 : 0;
    }

    static std::size_t header(char*, std::size_t size, std::size_t count, void* data) noexcept {
        auto& r = *static_cast<Request*>(data);
        const auto bytes = size * count;
        r.header_bytes += bytes;
        if (r.header_bytes > 64 * 1024) return 0;
        r.last_progress = Clock::now();
        return bytes;
    }

    static std::size_t write(char* data, std::size_t size, std::size_t count, void* user) noexcept {
        auto& r = *static_cast<Request*>(user);
        const auto bytes = size * count;
        try {
            if (r.cancellation.requested()) throw Error("cancelled");
            if (bytes > r.limit - r.received) throw Error("response_too_large");
            r.received += bytes;
            long status = 0;
            curl_easy_getinfo(r.easy, CURLINFO_RESPONSE_CODE, &status);
            if (status >= 200 && status < 300) {
                if (!r.responded && !r.expected_type.empty()) {
                    char* mime = nullptr;
                    curl_easy_getinfo(r.easy, CURLINFO_CONTENT_TYPE, &mime);
                    auto type = mime ? std::string_view(mime) : std::string_view();
                    type = type.substr(0, type.find(';'));
                    while (!type.empty() && type.back() == ' ') type.remove_suffix(1);
                    const auto equal_ascii = [](unsigned char a, unsigned char b) {
                        return std::tolower(a) == std::tolower(b);
                    };
                    if (!std::equal(type.begin(), type.end(), r.expected_type.begin(),
                                    r.expected_type.end(), equal_ascii))
                        throw Error("unexpected_content_type");
                }
                r.sink({data, bytes});
            } else {
                // Only used for classifying a small error envelope.
                r.error_body.append(data, std::min(bytes, 8192 - r.error_body.size()));
            }
            r.responded = true;
            r.last_progress = Clock::now();
            return bytes;
        } catch (...) {
            r.write_error = std::current_exception();
            return 0;
        }
    }
};

struct Upload {
    const std::string& bytes;
    std::size_t offset = 0;
    static std::size_t read(char* dest, std::size_t size, std::size_t count, void* data) noexcept {
        auto& u = *static_cast<Upload*>(data);
        const auto take = std::min(size * count, u.bytes.size() - u.offset);
        std::memcpy(dest, u.bytes.data() + u.offset, take);
        u.offset += take;
        return take;
    }
};

bool quota_error(const std::string& body) {
    // A single fixed code determines the diagnostic. Provider text is private.
    constexpr std::string_view code = "\"insufficient_quota\"";
    return body.find(code) != std::string::npos;
}

void check_status(long status, const std::string& body) {
    if (status >= 200 && status < 300) return;
    if (status == 401 || status == 403) throw Error("authentication_failed");
    if (status == 402 || (status == 429 && quota_error(body))) throw Error("quota_exhausted");
    if (status == 429) throw Error("rate_limited");
    if (status >= 500) throw Error("service_unavailable");
    if (status >= 300 && status < 400) throw Error("redirect_rejected");
    if (status >= 400) throw Error("request_rejected");
    throw Error("invalid_http_status");
}

void check_transfer(CURLcode result) {
    switch (result) {
    case CURLE_OK: return;
    case CURLE_OPERATION_TIMEDOUT: throw Error("request_timeout");
    case CURLE_COULDNT_RESOLVE_HOST: throw Error("dns_failure");
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CACERT_BADFILE: throw Error("tls_failure");
    case CURLE_PARTIAL_FILE: throw Error("truncated_response");
    case CURLE_COULDNT_CONNECT: throw Error("connection_failed");
    default: throw Error("transport_failure");
    }
}

void multipart_field(curl_mime* form, const char* name, const std::string& value) {
    auto* part = curl_mime_addpart(form);
    if (!part) throw Error("http_setup_failed");
    require(curl_mime_name(part, name));
    require(curl_mime_data(part, value.c_str(), value.size()));
}

bool local_ipv4(std::uint32_t address) {
    return (address >> 24) == 127 || (address >> 24) == 10 ||
           (address >> 20) == 0xac1 || (address >> 16) == 0xc0a8 ||
           (address >> 16) == 0xa9fe || (address >> 22) == 0x191;
}

curl_socket_t local_http_socket(void*, curlsocktype purpose, struct curl_sockaddr* target) noexcept {
    if (purpose != CURLSOCKTYPE_IPCXN) return CURL_SOCKET_BAD;
    bool local = false;
    if (target->family == AF_INET && target->addrlen >= sizeof(sockaddr_in)) {
        sockaddr_in address{};
        std::memcpy(&address, &target->addr, sizeof(address));
        local = local_ipv4(ntohl(address.sin_addr.s_addr));
    } else if (target->family == AF_INET6 && target->addrlen >= sizeof(sockaddr_in6)) {
        sockaddr_in6 address{};
        std::memcpy(&address, &target->addr, sizeof(address));
        const auto& ip = address.sin6_addr;
        local = IN6_IS_ADDR_LOOPBACK(&ip) || IN6_IS_ADDR_LINKLOCAL(&ip) ||
                (ip.s6_addr[0] & 0xfe) == 0xfc;
        if (IN6_IS_ADDR_V4MAPPED(&ip)) {
            std::uint32_t v4;
            std::memcpy(&v4, ip.s6_addr + 12, sizeof(v4));
            local = local_ipv4(ntohl(v4));
        }
    }
    return local ? ::socket(target->family, target->socktype | SOCK_CLOEXEC, target->protocol) : CURL_SOCKET_BAD;
}

struct Transfer {
    CURLM* multi;
    CURL* easy;
    Transfer(CURLM* m, CURL* e) : multi(m), easy(e) {
        if (curl_multi_add_handle(multi, easy) != CURLM_OK) throw Error("http_setup_failed");
    }
    ~Transfer() { curl_multi_remove_handle(multi, easy); }
};
}

void validate_http_runtime(const CloudService& service) {
    initialize();
    std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> url(curl_url(), curl_url_cleanup);
    if (!url || curl_url_set(url.get(), CURLUPART_URL, service.endpoint.c_str(), 0) != CURLUE_OK)
        throw ConfigError("invalid cloud endpoint URL");
}

namespace {
void request(const CloudService& service, const std::string& url, const std::string* data,
             std::string_view expected_type, std::size_t response_limit, Cancellation& cancellation,
             const ResponseSink& sink, const CloudAsrConfig* multipart, Deadline deadline) {
    if (cancellation.requested()) throw Error("cancelled");
    const auto request_limit = static_cast<std::size_t>(service.max_request_bytes);
    // The named fields, filename, media type, and multipart boundaries are
    // fixed; the variable model/language names have validated length bounds.
    const auto overhead = multipart ? 2048u : 0u;
    if (data && data->size() + overhead > request_limit) throw Error("request_too_large");
    const auto start = Clock::now();
    const auto stop = std::min(start + Milliseconds(service.total_ms), deadline);
    const auto remaining_ms = std::chrono::duration_cast<Milliseconds>(stop - start).count();
    if (remaining_ms <= 0) throw Error("request_timeout");

    initialize();
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> easy(curl_easy_init(), curl_easy_cleanup);
    std::unique_ptr<CURLM, decltype(&curl_multi_cleanup)> multi(curl_multi_init(), curl_multi_cleanup);
    if (!easy || !multi) throw Error("http_setup_failed");
    auto set = [&](CURLoption option, auto value) { require(curl_easy_setopt(easy.get(), option, value)); };

    Request request{easy.get(), cancellation, sink, expected_type,
                    std::min(response_limit, static_cast<std::size_t>(service.max_response_bytes))};
    set(CURLOPT_URL, url.c_str());
    const bool allow_http = data && service.allow_http;
#if LIBCURL_VERSION_NUM >= 0x075500
    set(CURLOPT_PROTOCOLS_STR, allow_http ? "https,http" : "https");
#else
    set(CURLOPT_PROTOCOLS, allow_http ? long(CURLPROTO_HTTPS | CURLPROTO_HTTP) : long(CURLPROTO_HTTPS));
#endif
    set(CURLOPT_FOLLOWLOCATION, 0L);
    set(CURLOPT_PROXY, "");
    // Check the actual resolved peer for plaintext HTTP, including DNS names.
    if (allow_http && url.compare(0, 7, "http://") == 0)
        set(CURLOPT_OPENSOCKETFUNCTION, &local_http_socket);
    if (!data) set(CURLOPT_PATH_AS_IS, 1L);
    set(CURLOPT_SSL_VERIFYPEER, 1L);
    set(CURLOPT_SSL_VERIFYHOST, 2L);
    set(CURLOPT_NOSIGNAL, 1L);
    set(CURLOPT_CONNECTTIMEOUT_MS, long(service.connect_ms));
    set(CURLOPT_TIMEOUT_MS, long(remaining_ms));
    set(CURLOPT_WRITEFUNCTION, &Request::write);
    set(CURLOPT_WRITEDATA, &request);
    set(CURLOPT_HEADERFUNCTION, &Request::header);
    set(CURLOPT_HEADERDATA, &request);
    set(CURLOPT_XFERINFOFUNCTION, &Request::progress);
    set(CURLOPT_XFERINFODATA, &request);
    set(CURLOPT_NOPROGRESS, 0L);

    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(nullptr, curl_slist_free_all);
    auto header = [&](const std::string& text) {
        auto* list = curl_slist_append(headers.get(), text.c_str());
        if (!list) throw Error("http_setup_failed");
        headers.release();
        headers.reset(list);
    };
    if (data && service.bearer_auth) header("Authorization: Bearer " + cloud_api_key(service));
    if (data) header("Expect:");
    if (data && service.protocol == "dashscope_asr_json") header("X-DashScope-SSE: disable");
    const std::string empty;
    Upload upload{data ? *data : empty};
    std::unique_ptr<curl_mime, decltype(&curl_mime_free)> form(nullptr, curl_mime_free);
    if (multipart) {
        form.reset(curl_mime_init(easy.get()));
        if (!form) throw Error("http_setup_failed");
        multipart_field(form.get(), "model", service.model);
        multipart_field(form.get(), "response_format", "json");
        if (!multipart->language.empty()) multipart_field(form.get(), "language", multipart->language);
        auto* part = curl_mime_addpart(form.get());
        if (!part) throw Error("http_setup_failed");
        require(curl_mime_name(part, "file"));
        require(curl_mime_filename(part, "utterance.wav"));
        require(curl_mime_type(part, "audio/wav"));
        require(curl_mime_data_cb(part, static_cast<curl_off_t>(data->size()), &Upload::read,
                                 nullptr, nullptr, &upload));
        set(CURLOPT_MIMEPOST, form.get());
    } else if (data) {
        header("Content-Type: application/json");
        set(CURLOPT_POSTFIELDS, data->c_str());
        set(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(data->size()));
    }
    set(CURLOPT_HTTPHEADER, headers.get());

    Transfer transfer(multi.get(), easy.get());
    Cancellation::Binding interrupt(cancellation, [&] { curl_multi_wakeup(multi.get()); });
    int active = 0;
    do {
        if (cancellation.requested()) throw Error("cancelled");
        if (curl_multi_perform(multi.get(), &active) != CURLM_OK) throw Error("transport_failure");
        if (request.header_bytes > 64 * 1024) throw Error("response_headers_too_large");
        if (request.write_error) std::rethrow_exception(request.write_error);
        const auto now = Clock::now();
        if (now >= stop) throw Error("request_timeout");
        if (!request.responded && now - request.start >= Milliseconds(service.first_response_ms))
            throw Error("first_response_timeout");
        if (now - request.last_progress >= Milliseconds(service.idle_ms)) throw Error("network_idle_timeout");
        // Wake immediately on cancel and check deadlines even on an idle socket.
        if (active && curl_multi_poll(multi.get(), nullptr, 0, 50, nullptr) != CURLM_OK)
            throw Error("transport_failure");
    } while (active);

    if (cancellation.requested()) throw Error("cancelled");
    int remaining = 0;
    const auto* done = curl_multi_info_read(multi.get(), &remaining);
    if (!done || done->msg != CURLMSG_DONE) throw Error("transport_failure");
    long status = 0;
    curl_easy_getinfo(easy.get(), CURLINFO_RESPONSE_CODE, &status);
    if (status >= 300) check_status(status, request.error_body);
    check_transfer(done->data.result);
    check_status(status, request.error_body);
    if (!data) {
        // The signed URL must return the whole OSS object. In particular, a
        // synthesized WAV can have placeholder lengths in its internal header.
        curl_off_t length = -1;
        curl_easy_getinfo(easy.get(), CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &length);
        if (status != 200 || length < 0 || static_cast<std::uint64_t>(length) != request.received)
            throw Error("incomplete_bailian_audio_download");
    }
}
}

void post(const CloudService& service, const std::string& data, std::string_view expected_type,
          std::size_t response_limit, Cancellation& cancellation, const ResponseSink& sink,
          const CloudAsrConfig* multipart, Deadline deadline) {
    request(service, service.endpoint, &data, expected_type, response_limit, cancellation, sink, multipart, deadline);
}

void download_bailian_wav(const CloudTtsConfig& service, const std::string& url,
                          std::size_t response_limit, Cancellation& cancellation,
                          const ResponseSink& sink, Deadline deadline) {
    const bool https = url.compare(0, 8, "https://") == 0;
    const bool http = url.compare(0, 7, "http://") == 0;
    const std::size_t host_begin = https ? 8 : 7;
    const auto path_begin = url.find('/', host_begin);
    constexpr std::string_view beijing_bucket = "dashscope-result-bj.oss-cn-beijing.aliyuncs.com";
    if ((!https && !http) || url.size() > 8192 || path_begin == std::string::npos ||
        std::string_view(url).substr(host_begin, path_begin - host_begin) != beijing_bucket ||
        std::any_of(url.begin(), url.end(), [](unsigned char c) {
            return c <= 32 || c >= 127 || c == '\\' || c == '#';
        }))
        throw Error("unexpected_bailian_audio_url");

    // The OSS signature covers the object and query, and the bucket supports
    // HTTPS even when DashScope returns an http:// spelling.
    const auto secure_url = https ? url : "https://" + url.substr(host_begin);
    request(service, secure_url, nullptr, "", response_limit, cancellation, sink, nullptr, deadline);
}
}
