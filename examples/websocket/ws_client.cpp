#include <iostream>
#include <string>

#include "coco/coco.h"

using namespace coco;

// Talks to ws_server: sends a few messages and prints the echoes.
int main(int argc, char **argv) {
    std::string url = argc > 1 ? argv[1] : "ws://127.0.0.1:9083/echo";
    return CocoRun([&url]() {
        WebSocketClient ws;
        ws.SetTlsDialer(TlsDialer());
        if (ws.Dial(url) != COCO_SUCCESS) {
            return -1;
        }
        for (int i = 0; i < 3 && !CocoShouldStop(); ++i) {
            ws.Send("hello " + std::to_string(i));
            std::string data;
            if (ws.ReadMessage(&data) != COCO_SUCCESS) {
                break;
            }
            std::cout << "got " << data << std::endl;
        }
        return 0;
    });
}
