#include <iostream>
#include <string>

#include "coco/coco.h"

using namespace coco;

// Echo server. Try it with: websocat ws://127.0.0.1:9083/echo
int main() {
    HttpServeMux mux;
    mux.Handle("/echo", new WebSocketHandler([](WebSocketConn *ws) {
        std::string data;
        WebSocketHeader::Type type;
        while (ws->ReadMessage(&data, &type) == COCO_SUCCESS) {
            std::cout << ws->GetRemoteAddr() << ": " << data << std::endl;
            ws->Send(data, type);
        }
    }));

    // ListenAndServeTLS for wss.
    HttpServer server(&mux);
    return server.ListenAndServe("0.0.0.0", 9083) == COCO_SUCCESS ? 0 : -1;
}
