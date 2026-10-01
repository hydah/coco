#include <iostream>
#include <string>

#include "coco_api.h"
#include "common/error.hpp"
#include "net/layer7/http/coco_http.hpp"
#include "net/tls/coco_tls.hpp"

// Talks to http_server: GET, then POST on the same pooled connection.
int main(int argc, char **argv) {
    CocoInit();

    std::string base = argc > 1 ? argv[1] : "https://127.0.0.1:9082";
    HttpClient client;
    client.SetTlsDialer(TlsDialer());

    std::unique_ptr<HttpResponse> resp;
    int ret = client.Get(base + "/hello/coco", &resp);
    if (ret != COCO_SUCCESS) {
        std::cerr << "GET failed: " << ret << std::endl;
        return 1;
    }
    std::string body;
    resp->body.ReadAll(&body);
    std::cout << resp->status << ": " << body;

    ret = client.Post(base + "/echo", "text/plain", "ping", &resp);
    if (ret != COCO_SUCCESS) {
        std::cerr << "POST failed: " << ret << std::endl;
        return 1;
    }
    body.clear();
    resp->body.ReadAll(&body);
    std::cout << resp->status << ": " << body << std::endl;
    return 0;
}
