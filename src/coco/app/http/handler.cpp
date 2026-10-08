#include "coco/app/http/handler.hpp"

namespace coco {

void HttpError(HttpResponseWriter &w, const std::string &error, int code) {
    HttpHeader &h = w.Header();
    h.Del(HttpHeaderContentLength);
    h.Set(HttpHeaderContentType, HttpContentTypeText);
    h.Set("X-Content-Type-Options", "nosniff");
    w.WriteHeader(code);
    w.Write(error + "\n");
}

void HttpNotFound(HttpResponseWriter &w, HttpRequest &r) {
    HttpError(w, "404 page not found", HttpStatusNotFound);
}

static std::string HtmlEscape(const std::string &s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '&': out += "&amp;"; break;
            case '"': out += "&#34;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c;
        }
    }
    return out;
}

void HttpRedirect(HttpResponseWriter &w, HttpRequest &r, const std::string &url, int code) {
    std::string loc = url;
    if (loc.find("://") == std::string::npos && (loc.empty() || loc[0] != '/')) {
        size_t q = loc.find('?');
        std::string query = q == std::string::npos ? "" : loc.substr(q);
        std::string rel = loc.substr(0, q);
        std::string dir = r.path.substr(0, r.path.rfind('/') + 1);
        loc = HttpCleanPath(dir + rel) + query;
    }

    HttpHeader &h = w.Header();
    h.Set(HttpHeaderLocation, loc);
    bool get_or_head = r.method == HttpMethodGet || r.method == HttpMethodHead;
    if (get_or_head && !h.Has(HttpHeaderContentType)) {
        h.Set(HttpHeaderContentType, HttpContentTypeHtml);
    }
    w.WriteHeader(code);
    if (get_or_head) {
        w.Write("<a href=\"" + HtmlEscape(loc) + "\">" + HttpStatusText(code) + "</a>.\n");
    }
}

}  // namespace coco
