#include "coco/app/ws/codec/handshake.hpp"

#include <string.h>
#include <strings.h>

#include <random>

#include "http-parser/http_parser.h"

#include "coco/common/error.hpp"
#include "coco/log/log.hpp"
#include "coco/utils/base64.hpp"
#include "coco/utils/sha1.hpp"

namespace coco {

int ParseWebSocketUrl(const std::string &url, WebSocketUrl *u) {
    http_parser_url pu;
    memset(&pu, 0, sizeof(pu));
    if (http_parser_parse_url(url.data(), url.size(), 0, &pu) != 0) {
        coco_error("websocket: bad url %s", url.c_str());
        return ERROR_HTTP_PARSE_URI;
    }
    auto field = [&](http_parser_url_fields f) {
        if ((pu.field_set & (1 << f)) == 0) {
            return std::string();
        }
        return url.substr(pu.field_data[f].off, pu.field_data[f].len);
    };

    std::string schema = field(UF_SCHEMA);
    bool is_wss = strcasecmp(schema.c_str(), "wss") == 0;
    std::string host = field(UF_HOST);
    if ((!is_wss && strcasecmp(schema.c_str(), "ws") != 0) || host.empty()) {
        coco_error("websocket: url must be ws://host or wss://host, got %s", url.c_str());
        return ERROR_HTTP_PARSE_URI;
    }
    uint16_t port = pu.port != 0 ? pu.port : (is_wss ? 443 : 80);
    std::string path = field(UF_PATH);
    if (path.empty()) {
        path = "/";
    }
    if (pu.field_set & (1 << UF_QUERY)) {
        path += "?" + field(UF_QUERY);
    }
    u->tls = is_wss;
    u->host = host;
    u->port = port;
    u->path = path;
    return COCO_SUCCESS;
}

std::string WebSocketNewKey() {
    static thread_local std::random_device rd;
    unsigned char nonce[16];
    for (int i = 0; i < 16; i += 4) {
        uint32_t v = rd();
        memcpy(nonce + i, &v, 4);
    }
    return base64::Encode(nonce, sizeof(nonce));
}

bool WebSocketKeyValid(const std::string &key) { return base64::decode(key).size() == 16; }

std::string WebSocketAcceptKey(const std::string &key) {
    unsigned char sha[20] = {0};
    std::string src = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    sha1::calc(src.data(), src.size(), sha);
    return base64::Encode(sha, sizeof(sha));
}

}  // namespace coco
