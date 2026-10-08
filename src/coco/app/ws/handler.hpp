#pragma once
#include <functional>

#include "coco/app/http/handler.hpp"
#include "coco/app/ws/conn.hpp"

namespace coco {

// Serves WebSocket on the patterns it is registered for in an HttpServeMux, which owns it.
// A request that is not a valid version 13 upgrade gets 400. After the handshake serve
// runs in the HTTP connection's own coroutine; when it returns the connection is closed
// (with CLOSE 1000 if still open) and the conn is freed, so other coroutines may Send()
// on it only until then.
//
//   mux.Handle("/echo", new WebSocketHandler([](WebSocketConn *ws) {
//       std::string data;
//       WebSocketHeader::Type type;
//       while (ws->ReadMessage(&data, &type) == COCO_SUCCESS) {
//           ws->Send(data, type);
//       }
//   }));
class WebSocketHandler : public HttpHandler {
 public:
    typedef std::function<void(WebSocketConn *)> ServeFunc;

    explicit WebSocketHandler(ServeFunc serve) : serve_(serve) {}
    virtual ~WebSocketHandler() = default;

    void ServeHTTP(HttpResponseWriter &w, HttpRequest &r) override;

 private:
    ServeFunc serve_;
};

}  // namespace coco
