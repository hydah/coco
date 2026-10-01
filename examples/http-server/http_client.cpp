#include <iostream>
#include <string>

#include "coco/coco.h"

using namespace coco;

static void Print(int ret, HttpResponse *resp) {
    std::string body;
    if (ret == COCO_SUCCESS && (ret = resp->body.ReadAll(&body)) == COCO_SUCCESS) {
        std::cout << resp->status << ": " << body << std::endl;
    } else {
        std::cerr << "request failed: " << ret << std::endl;
    }
}

// Talks to http_server: GET, then POST on the same pooled connection.
int main(int argc, char **argv) {
    std::string base = argc > 1 ? argv[1] : "https://127.0.0.1:9082";
    return CocoRun([&base]() {
        HttpClient client;
        client.SetTlsDialer(TlsDialer());
        std::unique_ptr<HttpResponse> resp;

        Print(client.Get(base + "/hello/coco", &resp), resp.get());
        Print(client.Post(base + "/echo", HttpContentTypeText, "ping", &resp), resp.get());
        return 0;
    });
}
