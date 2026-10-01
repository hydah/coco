#include <string>

#include "coco_api.h"
#include "common/error.hpp"
#include "log/log.hpp"
#include "server/coco_http_server.hpp"

// HTTPS on 9082, run from examples/http-server so ./server.crt and ./server.key are found.
// Try: curl -k https://127.0.0.1:9082/hello/coco
int main() {
    CocoInit();

    HttpServeMux mux;
    mux.HandleFunc("GET /hello/{name}", [](HttpResponseWriter &w, HttpRequest &r) {
        w.Write("hello " + r.PathValue("name") + "\n");
    });
    mux.HandleFunc("POST /echo", [](HttpResponseWriter &w, HttpRequest &r) {
        std::string body;
        if (r.body.ReadAll(&body) != COCO_SUCCESS) {
            HttpError(w, "bad body", HttpStatusBadRequest);
            return;
        }
        w.Header().Set(HttpHeaderContentType, r.header.Get(HttpHeaderContentType));
        w.Write(body);
    });
    mux.HandleFunc("/", [](HttpResponseWriter &w, HttpRequest &r) {
        w.Header().Set(HttpHeaderContentType, HttpContentTypeJson);
        w.Write("{\"path\":\"" + r.path + "\",\"q\":\"" + r.Query().Get("q") + "\"}\n");
    });

    HttpServer server(&mux);
    if (server.ListenAndServeTLS("0.0.0.0", 9082, "./server.crt", "./server.key") !=
        COCO_SUCCESS) {
        coco_error("listen failed");
        return -1;
    }

    CocoLoopMs(1000);
    return 0;
}
