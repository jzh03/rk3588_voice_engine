#pragma once

#include "http.hpp"
#include <curl/curl.h>
#include <libwebsockets.h>
#include <array>
#include <charconv>
#include <memory>

namespace voice_engine::cloud {
struct WebSocketEndpoint {
    std::string host, path;
    int port = 443;
};

inline WebSocketEndpoint parse_websocket_endpoint(const std::string& value) {
    std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> url(curl_url(), curl_url_cleanup);
    if (!url || value.compare(0, 6, "wss://") != 0 ||
        curl_url_set(url.get(), CURLUPART_URL, value.c_str(), CURLU_NON_SUPPORT_SCHEME) != CURLUE_OK)
        throw ConfigError("invalid cloud WebSocket endpoint");
    auto part = [&](CURLUPart name) {
        char* raw = nullptr;
        curl_url_get(url.get(), name, &raw, 0);
        const std::unique_ptr<char, decltype(&curl_free)> text(raw, curl_free);
        return raw ? std::string(raw) : std::string();
    };
    WebSocketEndpoint endpoint{part(CURLUPART_HOST), part(CURLUPART_PATH)};
    const auto port = part(CURLUPART_PORT);
    if (endpoint.host.empty() || !part(CURLUPART_USER).empty() || !part(CURLUPART_PASSWORD).empty() ||
        !part(CURLUPART_QUERY).empty() || !part(CURLUPART_FRAGMENT).empty())
        throw ConfigError("invalid cloud WebSocket endpoint");
    if (!port.empty()) {
        const auto result = std::from_chars(port.data(), port.data() + port.size(), endpoint.port);
        if (result.ec != std::errc{} || result.ptr != port.data() + port.size() ||
            endpoint.port < 1 || endpoint.port > 65535)
            throw ConfigError("invalid cloud WebSocket endpoint port");
    }
    return endpoint;
}

inline std::string websocket_task_id(lws_context* context) {
    std::array<unsigned char, 16> bytes{};
    if (lws_get_random(context, bytes.data(), bytes.size()) != bytes.size())
        throw Error("cloud_websocket_random_failed");
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;
    constexpr char hex[] = "0123456789abcdef";
    std::string id;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) id += '-';
        id += hex[bytes[i] >> 4];
        id += hex[bytes[i] & 15];
    }
    return id;
}

} // namespace voice_engine::cloud
