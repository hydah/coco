#include <iostream>
#include <string>

#include "coco_api.h"
#include "common/error.hpp"
#include "net/layer7/ws/coco_ws.hpp"

// Talks to ws_server: sends a few messages and prints the echoes.
int main(int argc, char **argv) {
    CocoInit();

    WebSocketClient ws;
    if (ws.Dial(argc > 1 ? argv[1] : "ws://127.0.0.1:9083/echo") != COCO_SUCCESS) {
        return -1;
    }
    for (int i = 0; i < 3; ++i) {
        ws.Send("hello " + std::to_string(i));
        std::string data;
        if (ws.ReadMessage(&data) != COCO_SUCCESS) {
            break;
        }
        std::cout << "got " << data << std::endl;
    }
    return 0;
}
