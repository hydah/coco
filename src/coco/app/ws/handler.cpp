#include "coco/app/ws/handler.hpp"

#include "coco/common/error.hpp"
#include "coco/app/ws/codec/handshake.hpp"

namespace coco {

void WebSocketHandler::ServeHTTP(HttpResponseWriter &w, HttpRequest &r) {
    // RFC 6455 4.2.1. IsUpgrade() covers the Connection: upgrade token.
    const std::string &key = r.header.Get("Sec-WebSocket-Key");
    if (r.method != HttpMethodGet || !r.IsUpgrade() ||
        !r.header.HasToken(HttpHeaderUpgrade, "websocket") || !WebSocketKeyValid(key)) {
        HttpError(w, HttpStatusText(HttpStatusBadRequest), HttpStatusBadRequest);
        return;
    }
    if (r.header.Get("Sec-WebSocket-Version") != "13") {
        w.Header().Set("Sec-WebSocket-Version", "13");
        HttpError(w, HttpStatusText(HttpStatusBadRequest), HttpStatusBadRequest);
        return;
    }

    StreamConn *conn = nullptr;
    BufReader *br = nullptr;
    if (w.Hijack(&conn, &br) != COCO_SUCCESS) {
        return;
    }
    std::string rsp =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " +
        WebSocketAcceptKey(key) + "\r\n\r\n";
    if (conn->Write((void *)rsp.data(), rsp.size(), nullptr) != COCO_SUCCESS) {
        return;
    }

    WebSocketConn ws(conn, br, false);
    serve_(&ws);
    // Read errors were logged, and the connection ends here either way.
    ws.Finish();
}

}  // namespace coco
