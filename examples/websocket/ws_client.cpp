#include <iostream>
#include <string>

#include "coco_api.h"
#include "common/error.hpp"
#include "log/log.hpp"
#include "net/coco_socket.hpp"
#include "net/layer7/coco_ws.hpp"

using namespace std;

string server_ip = "127.0.0.1";
int port = 3000;
int OnMessage(WebSocketConn* conn, std::unique_ptr<WebSocektMessage> msg) {
    std::cout << "get " << msg->data_ << endl;
}

int main() {
    int ret = COCO_SUCCESS;

    CocoInit();

    WebSocketClient ws_client;
    ws_client.SetMessageHandler(OnMessage);
    ws_client.Start(false, server_ip, port, "/");

    std::string msg = "hello ws";
    std::cout << "send hello ws" << std::endl;
    ws_client.Send((uint8_t*)msg.data(), msg.size());

    CocoLoopMs(1000);

    return 0;
}