#include "protocol/http/http_mux.h"

#include <assert.h>
#include <string.h>

#include "common/error.hpp"
#include "log/log.hpp"
#include "protocol/http/http_io.h"
#include "protocol/http/http_message.h"

IHttpHandler::IHttpHandler() { entry = nullptr; }

IHttpHandler::~IHttpHandler() {}

bool IHttpHandler::is_not_found() { return false; }

HttpRedirectHandler::HttpRedirectHandler(std::string u, int c) {
  url = u;
  code = c;
}

HttpRedirectHandler::~HttpRedirectHandler() {}

int HttpRedirectHandler::serve_http(HttpResponseWriter *w, HttpMessage *r) {
  int ret = COCO_SUCCESS;

  std::string location = url;
  if (!r->query().empty()) {
    location += "?" + r->query();
  }

  std::string msg = "Redirect to" + location;

  w->header()->set_content_type("text/plain; charset=utf-8");
  w->header()->set_content_length(msg.length());
  w->header()->set("Location", location);
  w->WriteHeader(code);

  w->Write((char *)msg.data(), (int)msg.length());
  w->final_request();

  coco_info("redirect to %s.", location.c_str());
  return ret;
}

HttpNotFoundHandler::HttpNotFoundHandler() {}

HttpNotFoundHandler::~HttpNotFoundHandler() {}

bool HttpNotFoundHandler::is_not_found() { return true; }

int HttpNotFoundHandler::serve_http(HttpResponseWriter *w, HttpMessage *r) {
  return go_http_error(w, CONSTS_HTTP_NotFound);
}

HttpMuxEntry::HttpMuxEntry() {
  enabled = true;
  explicit_match = false;
  handler = nullptr;
}

HttpMuxEntry::~HttpMuxEntry() { coco_freep(handler); }

HttpServeMux::HttpServeMux() {}

HttpServeMux::~HttpServeMux() {
  std::map<std::string, HttpMuxEntry *>::iterator it;
  for (it = entries.begin(); it != entries.end(); ++it) {
    HttpMuxEntry *entry = it->second;
    coco_freep(entry);
  }
  entries.clear();

  vhosts.clear();
}

int HttpServeMux::initialize() {
  int ret = COCO_SUCCESS;
  // TODO: FIXME: implements it.
  return ret;
}

void HttpServeMux::remove_entry(std::string pattern) {
  if (entries.find(pattern) == entries.end()) {
    return;
  }
  coco_trace("remove inactive mux patten %s", pattern.c_str());
  HttpMuxEntry *exists = entries[pattern];
  entries.erase(pattern);
  coco_freep(exists);
}

int HttpServeMux::handle(std::string pattern, IHttpHandler *handler) {
  int ret = COCO_SUCCESS;

  assert(handler);

  if (pattern.empty()) {
    ret = ERROR_HTTP_PATTERN_EMPTY;
    coco_error("http: empty pattern. ret=%d", ret);
    return ret;
  }

  if (entries.find(pattern) != entries.end()) {
    HttpMuxEntry *exists = entries[pattern];
    if (exists->explicit_match) {
      ret = ERROR_HTTP_PATTERN_DUPLICATED;
      coco_error("http: multiple registrations for %s. ret=%d", pattern.c_str(),
                 ret);
      return ret;
    }
  }

  std::string vhost = pattern;
  if (pattern.at(0) != '/') {
    if (pattern.find("/") != std::string::npos) {
      vhost = pattern.substr(0, pattern.find("/"));
    }
    vhosts[vhost] = handler;
  }

  if (true) {
    HttpMuxEntry *entry = new HttpMuxEntry();
    entry->explicit_match = true;
    entry->handler = handler;
    entry->pattern = pattern;
    entry->handler->entry = entry;

    if (entries.find(pattern) != entries.end()) {
      HttpMuxEntry *exists = entries[pattern];
      coco_freep(exists);
    }
    entries[pattern] = entry;
  }

  // Helpful behavior:
  // If pattern is /tree/, insert an implicit permanent redirect for /tree.
  // It can be overridden by an explicit registration.
  if (pattern != "/" && !pattern.empty() &&
      pattern.at(pattern.length() - 1) == '/') {
    std::string rpattern = pattern.substr(0, pattern.length() - 1);
    HttpMuxEntry *entry = nullptr;

    // free the exists not explicit entry
    if (entries.find(rpattern) != entries.end()) {
      HttpMuxEntry *exists = entries[rpattern];
      if (!exists->explicit_match) {
        entry = exists;
      }
    }

    // create implicit redirect.
    if (!entry || entry->explicit_match) {
      coco_freep(entry);

      entry = new HttpMuxEntry();
      entry->explicit_match = false;
      entry->handler = new HttpRedirectHandler(pattern, CONSTS_HTTP_Found);
      entry->pattern = pattern;
      entry->handler->entry = entry;

      entries[rpattern] = entry;
    }
  }

  return ret;
}

bool HttpServeMux::can_serve(HttpMessage *r) {
  int ret = COCO_SUCCESS;

  IHttpHandler *h = nullptr;
  if ((ret = find_handler(r, &h)) != COCO_SUCCESS) {
    return false;
  }

  assert(h);
  return !h->is_not_found();
}

int HttpServeMux::serve_http(HttpResponseWriter *w, HttpMessage *r) {
  int ret = COCO_SUCCESS;

  IHttpHandler *handler = nullptr;
  if ((ret = find_handler(r, &handler)) != COCO_SUCCESS) {
    coco_error("find handler failed. ret=%d", ret);
    return ret;
  }

  assert(handler);
  if ((ret = handler->serve_http(w, r)) != COCO_SUCCESS) {
    if (!coco_is_client_gracefully_close(ret)) {
      coco_error("handler serve http failed. ret=%d", ret);
    }
    return ret;
  }

  return ret;
}

int HttpServeMux::find_handler(HttpMessage *r, IHttpHandler **ph) {
  int ret = COCO_SUCCESS;

  // TODO: FIXME: support the path . and ..
  if (r->url().find("..") != std::string::npos) {
    ret = ERROR_HTTP_URL_NOT_CLEAN;
    coco_error("http url not canonical, url=%s. ret=%d", r->url().c_str(), ret);
    return ret;
  }

  if ((ret = match(r, ph)) != COCO_SUCCESS) {
    coco_error("http match handler failed. ret=%d", ret);
    return ret;
  }

  static IHttpHandler *h404 = new HttpNotFoundHandler();
  if (*ph == nullptr) {
    *ph = h404;
  }

  return ret;
}

int HttpServeMux::match(HttpMessage *r, IHttpHandler **ph) {
  int ret = COCO_SUCCESS;

  std::string path = r->path();

  // Host-specific pattern takes precedence over generic ones
  if (!vhosts.empty() && vhosts.find(r->get_host()) != vhosts.end()) {
    path = r->get_host() + path;
  }

  int nb_matched = 0;
  IHttpHandler *h = nullptr;

  std::map<std::string, HttpMuxEntry *>::iterator it;
  for (it = entries.begin(); it != entries.end(); ++it) {
    std::string pattern = it->first;
    HttpMuxEntry *entry = it->second;

    if (!entry->enabled) {
      continue;
    }

    if (!path_match(pattern, path)) {
      continue;
    }

    if (!h || (int)pattern.length() > nb_matched) {
      nb_matched = (int)pattern.length();
      h = entry->handler;
    }
  }

  *ph = h;

  return ret;
}

bool HttpServeMux::path_match(std::string pattern, std::string path) {
  if (pattern.empty()) {
    return false;
  }

  int n = (int)pattern.length();

  // not endswith '/', exactly match.
  if (pattern.at(n - 1) != '/') {
    return pattern == path;
  }

  // endswith '/', match any,
  // for example, '/api/' match '/api/[N]'
  if ((int)path.length() >= n) {
    if (memcmp(pattern.data(), path.data(), n) == 0) {
      return true;
    }
  }

  return false;
}