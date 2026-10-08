#pragma once
#include <stdint.h>

#include <string>

namespace coco {

// ws://host[:port][/path][?query] or wss://, split for the opening handshake.
struct WebSocketUrl {
    bool tls = false;
    std::string host;
    uint16_t port = 80;
    // path and query, "/" when the URL has none.
    std::string path;
};

// ERROR_HTTP_PARSE_URI unless url is ws://host... or wss://host...
int ParseWebSocketUrl(const std::string &url, WebSocketUrl *u);

// RFC 6455 4.1: a random 16-byte nonce, base64 encoded, chosen anew for each connection.
std::string WebSocketNewKey();
// Whether a client's Sec-WebSocket-Key is a base64 encoded 16-byte nonce.
bool WebSocketKeyValid(const std::string &key);
// RFC 6455 4.2.2: the Sec-WebSocket-Accept value for a Sec-WebSocket-Key.
std::string WebSocketAcceptKey(const std::string &key);

}  // namespace coco
