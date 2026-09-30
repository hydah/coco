#include <iostream>
#include <string>

#include "coco_api.h"
#include "common/error.hpp"
#include "net/layer7/ws/coco_ws.hpp"
#include "server/coco_http_server.hpp"

// Echo server. Try it with: websocat ws://127.0.0.1:9083/echo
int main() {
    CocoInit();

    HttpServeMux mux;
    mux.handle("/echo", new WebSocketHandler([](WebSocketConn *ws) {
        std::string data;
        WebSocketHeader::Type type;
        while (ws->ReadMessage(&data, &type) == COCO_SUCCESS) {
            std::cout << ws->GetRemoteAddr() << ": " << data << std::endl;
            ws->Send(data, type);
        }
    }));

    // true for wss, with ./server.key and ./server.crt.
    HttpServer server(false);
    if (server.ListenAndServe("0.0.0.0", 9083, &mux) != COCO_SUCCESS) {
        return -1;
    }
    CocoLoopMs(1000);
    return 0;
}
