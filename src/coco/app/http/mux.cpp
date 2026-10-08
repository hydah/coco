#include "coco/app/http/mux.hpp"

#include <string.h>

#include <algorithm>
#include <set>

#include "coco/common/error.hpp"
#include "coco/log/log.hpp"

namespace coco {

struct HttpServeMux::Route {
    // empty for any method.
    std::string method;
    std::string pattern;
    // The {name} wildcards from the root down, then the {name...} one if named.
    std::vector<std::string> names;
    bool rest_named = false;
    std::shared_ptr<HttpHandler> handler;
};

struct HttpServeMux::Node {
    std::vector<std::pair<std::string, std::unique_ptr<Node>>> literals;
    std::unique_ptr<Node> wildcard;
    // Patterns whose path ends at this node.
    std::vector<Route> exact;
    // Patterns that match whatever follows this node: "/a/" or "/a/{rest...}".
    std::vector<Route> subtree;

    Node *Literal(const std::string &seg) {
        for (auto &kv : literals) {
            if (kv.first == seg) {
                return kv.second.get();
            }
        }
        literals.push_back(std::make_pair(seg, std::unique_ptr<Node>(new Node())));
        return literals.back().second.get();
    }
    Node *Wildcard() {
        if (!wildcard) {
            wildcard.reset(new Node());
        }
        return wildcard.get();
    }
};

struct HttpServeMux::Match {
    const std::string *path = nullptr;
    const std::string *method = nullptr;
    // (offset, length) of each segment a {name} captured, along the current branch.
    std::vector<std::pair<size_t, size_t>> caps;
    const Route *route = nullptr;
    // Where the subtree match started; npos when it matched nothing.
    size_t rest = std::string::npos;
    bool subtree = false;
    // Methods of patterns whose path matched but whose method did not.
    std::set<std::string> allow;
};

HttpServeMux::HttpServeMux() : root_(new Node()) {}

HttpServeMux::~HttpServeMux() = default;

static bool IsWildcard(const std::string &seg, std::string *name, bool *rest) {
    if (seg.size() < 3 || seg.front() != '{' || seg.back() != '}') {
        return false;
    }
    *name = seg.substr(1, seg.size() - 2);
    *rest = name->size() > 3 && name->compare(name->size() - 3, 3, "...") == 0;
    if (*rest) {
        name->resize(name->size() - 3);
    }
    return !name->empty() && name->find_first_of("{}/") == std::string::npos;
}

int HttpServeMux::Handle(const std::string &pattern, std::shared_ptr<HttpHandler> handler) {
    if (!handler) {
        return ERROR_HTTP_HANDLER_INVALID;
    }

    std::string rest = pattern;
    std::string method;
    size_t sp = pattern.find_first_of(" \t");
    if (sp != std::string::npos) {
        method = pattern.substr(0, sp);
        size_t start = pattern.find_first_not_of(" \t", sp);
        rest = start == std::string::npos ? "" : pattern.substr(start);
    }
    size_t slash = rest.find('/');
    if (slash == std::string::npos || method.find('/') != std::string::npos) {
        coco_error("http: invalid pattern \"%s\"", pattern.c_str());
        return ERROR_HTTP_PATTERN_EMPTY;
    }
    std::string host = rest.substr(0, slash);
    std::string path = rest.substr(slash);

    Node *n = root_.get();
    if (!host.empty()) {
        std::unique_ptr<Node> &h = hosts_[host];
        if (!h) {
            h.reset(new Node());
        }
        n = h.get();
    }

    Route route;
    route.method = method;
    route.pattern = pattern;
    route.handler = handler;
    std::vector<Route> *list = nullptr;

    // "/a/b/" splits into "a", "b", "".
    size_t pos = 1;
    while (list == nullptr) {
        size_t end = path.find('/', pos);
        bool last = end == std::string::npos;
        std::string seg = path.substr(pos, last ? std::string::npos : end - pos);
        std::string name;
        bool is_rest = false;

        if (seg.empty()) {
            if (!last) {
                coco_error("http: empty segment in pattern \"%s\"", pattern.c_str());
                return ERROR_HTTP_PATTERN_EMPTY;
            }
            list = &n->subtree;
        } else if (seg == "{$}") {
            if (!last) {
                return ERROR_HTTP_PATTERN_EMPTY;
            }
            list = &n->Literal("")->exact;
        } else if (IsWildcard(seg, &name, &is_rest)) {
            route.names.push_back(name);
            if (is_rest) {
                if (!last) {
                    coco_error("http: {%s...} must be last in \"%s\"", name.c_str(),
                               pattern.c_str());
                    return ERROR_HTTP_PATTERN_EMPTY;
                }
                route.rest_named = true;
                list = &n->subtree;
            } else {
                n = n->Wildcard();
            }
        } else if (seg.find_first_of("{}") != std::string::npos) {
            coco_error("http: bad wildcard in pattern \"%s\"", pattern.c_str());
            return ERROR_HTTP_PATTERN_EMPTY;
        } else {
            n = n->Literal(seg);
        }

        if (list == nullptr && last) {
            list = &n->exact;
        }
        pos = end + 1;
    }

    for (const Route &r : *list) {
        if (r.method == method) {
            coco_error("http: \"%s\" conflicts with \"%s\"", pattern.c_str(), r.pattern.c_str());
            return ERROR_HTTP_PATTERN_DUPLICATED;
        }
    }
    list->push_back(route);
    return COCO_SUCCESS;
}

int HttpServeMux::Handle(const std::string &pattern, HttpHandler *handler) {
    return Handle(pattern, std::shared_ptr<HttpHandler>(handler));
}

int HttpServeMux::HandleFunc(const std::string &pattern, HttpHandlerFunc f) {
    return Handle(pattern, std::make_shared<HttpFuncHandler>(std::move(f)));
}

bool HttpServeMux::TryRoutes(const std::vector<Route> &routes, size_t rest, bool subtree,
                             Match *m) const {
    if (routes.empty()) {
        return false;
    }
    const std::string &method = *m->method;
    const Route *exact = nullptr, *head = nullptr, *any = nullptr;
    for (const Route &r : routes) {
        if (r.method == method) {
            exact = &r;
        } else if (r.method == HttpMethodGet && method == HttpMethodHead) {
            head = &r;
        } else if (r.method.empty()) {
            any = &r;
        }
    }
    const Route *found = exact ? exact : head ? head : any;
    if (found == nullptr) {
        for (const Route &r : routes) {
            m->allow.insert(r.method);
            if (r.method == HttpMethodGet) {
                m->allow.insert(HttpMethodHead);
            }
        }
        return false;
    }
    m->route = found;
    m->rest = rest;
    m->subtree = subtree;
    return true;
}

// seg is where the current segment starts, just after its '/', or npos past the end.
bool HttpServeMux::Walk(const Node *n, size_t seg, Match *m) const {
    if (seg == std::string::npos) {
        return TryRoutes(n->exact, std::string::npos, false, m);
    }

    const std::string &path = *m->path;
    size_t end = path.find('/', seg);
    if (end == std::string::npos) {
        end = path.size();
    }
    size_t len = end - seg;
    size_t next = end < path.size() ? end + 1 : std::string::npos;

    for (auto &kv : n->literals) {
        if (kv.first.size() == len && memcmp(kv.first.data(), path.data() + seg, len) == 0 &&
            Walk(kv.second.get(), next, m)) {
            return true;
        }
    }
    if (n->wildcard && len > 0) {
        m->caps.push_back(std::make_pair(seg, len));
        if (Walk(n->wildcard.get(), next, m)) {
            return true;
        }
        m->caps.pop_back();
    }
    return TryRoutes(n->subtree, seg, true, m);
}

static std::string HostName(const std::string &host) {
    if (!host.empty() && host[0] == '[') {
        size_t end = host.find(']');
        return end == std::string::npos ? host : host.substr(0, end + 1);
    }
    size_t colon = host.find(':');
    return colon == std::string::npos ? host : host.substr(0, colon);
}

bool HttpServeMux::Find(const std::string &host, const std::string &path, Match *m) const {
    if (path.empty() || path[0] != '/') {
        return false;
    }
    m->path = &path;
    if (!hosts_.empty()) {
        auto it = hosts_.find(HostName(host));
        if (it != hosts_.end()) {
            m->caps.clear();
            if (Walk(it->second.get(), 1, m)) {
                return true;
            }
        }
    }
    m->caps.clear();
    return Walk(root_.get(), 1, m);
}

void HttpServeMux::ServeHTTP(HttpResponseWriter &w, HttpRequest &r) {
    if (r.url == "*") {
        if (r.ProtoAtLeast(1, 1)) {
            w.Header().Set(HttpHeaderConnection, "close");
        }
        w.WriteHeader(HttpStatusBadRequest);
        return;
    }

    std::string query = r.raw_query.empty() ? "" : "?" + r.raw_query;
    if (r.method != HttpMethodConnect) {
        // The escaped path as sent, so a redirect keeps its escapes.
        std::string raw = !r.url.empty() && r.url[0] == '/' ? r.url.substr(0, r.url.find('?'))
                                                              : r.path;
        std::string clean = HttpCleanPath(raw);
        if (clean != raw) {
            HttpRedirect(w, r, clean + query, HttpStatusMovedPermanently);
            return;
        }
    }

    Match m;
    m.method = &r.method;
    bool found = Find(r.host, r.path, &m);

    // Like Go: when "/tree/" is registered, "/tree" redirects to it unless some pattern
    // matches "/tree" exactly; a broader subtree such as "/" does not count.
    if ((!found || m.subtree) && r.method != HttpMethodConnect && !r.path.empty() &&
        r.path.back() != '/') {
        std::string with_slash = r.path + "/";
        Match m2;
        m2.method = &r.method;
        if (Find(r.host, with_slash, &m2) && m2.subtree && m2.rest == with_slash.size()) {
            std::string raw = !r.url.empty() && r.url[0] == '/' ? r.url.substr(0, r.url.find('?'))
                                                                  : r.path;
            HttpRedirect(w, r, raw + "/" + query, HttpStatusMovedPermanently);
            return;
        }
    }

    if (found) {
        const Route *route = m.route;
        size_t i = 0;
        for (; i < m.caps.size() && i < route->names.size(); ++i) {
            r.SetPathValue(route->names[i], r.path.substr(m.caps[i].first, m.caps[i].second));
        }
        if (route->rest_named) {
            r.SetPathValue(route->names.back(),
                           m.rest == std::string::npos ? "" : r.path.substr(m.rest));
        }
        route->handler->ServeHTTP(w, r);
        return;
    }

    if (!m.allow.empty()) {
        std::string allow;
        for (const std::string &method : m.allow) {
            if (method.empty()) {
                continue;
            }
            allow += allow.empty() ? method : ", " + method;
        }
        w.Header().Set(HttpHeaderAllow, allow);
        HttpError(w, "Method Not Allowed", HttpStatusMethodNotAllowed);
        return;
    }

    HttpNotFound(w, r);
}

}  // namespace coco
