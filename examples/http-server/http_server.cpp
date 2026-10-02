#include <string>

#include "coco/coco.h"

using namespace coco;

// HTTPS on 9082, run from examples/http-server so ./server.crt and ./server.key are found.
// Try: curl -k https://127.0.0.1:9082/hello/coco. Ctrl-C stops it gracefully.
int main() {
    return CocoRun([]() {
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

        // The handlers share nothing, so connections may run on four worker threads.
        HttpServeOptions opt;
        opt.threads = 4;
        HttpServer server(&mux, opt);
        // Serves until SIGINT or SIGTERM, then closes every connection and returns.
        if (server.ListenAndServeTLS("0.0.0.0", 9082, "./server.crt", "./server.key") !=
            COCO_SUCCESS) {
            coco_error("listen failed");
            return -1;
        }
        return 0;
    });
}
