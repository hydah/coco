#include "protocol/http/http_message.h"

#include <assert.h>

#include "common/error.hpp"
#include "log/log.hpp"
#include "protocol/http/http_io.h"
HttpMessage::HttpMessage() {
  chunked = false;
  infinite_chunked = false;
  keep_alive = true;
  jsonp = false;

  _uri = new HttpUri();
  parser_ = new HttpParser();
}

HttpMessage::~HttpMessage() {
  coco_freep(_body);
  coco_freep(_uri);
}

int HttpMessage::Initialize(enum http_parser_type type) {
  return parser_->initialize(type);
}

int HttpMessage::Parse(IoReaderWriter *io, void *c) {
  int ret = COCO_SUCCESS;
  observer_ = c;
  io_ = io;
  coco_freep(_body);
  _body = new HttpResponseReader(this, io_);

  do {
    if ((ret = parser_->ParseMessage(io_)) != COCO_SUCCESS) {
      coco_error("parse message failed, ret = %d", ret);
      break;
    }
    _url = parser_->GetUrl();
    header_ = parser_->GetHeader();
    headers_ = parser_->GetHeaderField();

    // whether chunked.
    std::string transfer_encoding = get_request_header("Transfer-Encoding");
    chunked = (transfer_encoding == "chunked");
    // whether keep alive.
    keep_alive = http_should_keep_alive(header_);

    // set the buffer.
    if ((ret = _body->initialize(parser_->GetBuffer())) != COCO_SUCCESS) {
      break;
    }

    // parse uri from url.
    std::string _host = get_request_header("Host");

    // use server public ip when no host specified.
    // to make telnet happy.
    if (_host.empty()) {
      _host = get_public_internet_address();
    }

    // parse uri to schema/server:port/path?query
    std::string uri_ = "http://" + _host + _url;
    if ((ret = _uri->initialize(uri_)) != COCO_SUCCESS) {
      break;
    }

    // must format as key=value&...&keyN=valueN
    std::string q = _uri->get_query();
    size_t pos = std::string::npos;
    while (!q.empty()) {
      std::string k = q;
      if ((pos = q.find("=")) != std::string::npos) {
        k = q.substr(0, pos);
        q = q.substr(pos + 1);
      } else {
        q = "";
      }

      std::string v = q;
      if ((pos = q.find("&")) != std::string::npos) {
        v = q.substr(0, pos);
        q = q.substr(pos + 1);
      } else {
        q = "";
      }

      _query[k] = v;
    }

    // parse ext.
    _ext = _uri->get_path();
    if ((pos = _ext.rfind(".")) != std::string::npos) {
      _ext = _ext.substr(pos);
    } else {
      _ext = "";
    }

    // parse jsonp request message.
    if (!query_get("callback").empty()) {
      jsonp = true;
    }
    if (jsonp) {
      jsonp_method = query_get("method");
    }
  } while (0);

  return ret;
}

int HttpMessage::update_buffer(FastBuffer *body) {
  int ret = COCO_SUCCESS;
  if ((ret = _body->initialize(body)) != COCO_SUCCESS) {
    return ret;
  }
  return ret;
}

HttpResponseReader *HttpMessage::get_http_response_reader() { return _body; }

uint8_t HttpMessage::method() {
  if (jsonp && !jsonp_method.empty()) {
    if (jsonp_method == "GET") {
      return HTTP_GET;
    } else if (jsonp_method == "PUT") {
      return HTTP_PUT;
    } else if (jsonp_method == "POST") {
      return HTTP_POST;
    } else if (jsonp_method == "DELETE") {
      return HTTP_DELETE;
    }
  }

  return (uint8_t)header_->method;
}

std::string HttpMessage::method_str() {
  if (jsonp && !jsonp_method.empty()) {
    return jsonp_method;
  }

  if (is_http_get()) {
    return "GET";
  }
  if (is_http_put()) {
    return "PUT";
  }
  if (is_http_post()) {
    return "POST";
  }
  if (is_http_delete()) {
    return "DELETE";
  }
  if (is_http_options()) {
    return "OPTIONS";
  }

  return "OTHER";
}

std::string HttpMessage::uri() {
  std::string uri_ = _uri->get_schema();
  if (uri_.empty()) {
    uri_ += "http";
  }
  uri_ += "://";

  uri_ += host();
  uri_ += path();

  return uri_;
}

int HttpMessage::parse_rest_id(std::string pattern) {
  std::string p = _uri->get_path();
  if (p.length() <= pattern.length()) {
    return -1;
  }

  std::string id = p.substr((int)pattern.length());
  if (!id.empty()) {
    return ::atoi(id.c_str());
  }

  return -1;
}

int HttpMessage::enter_infinite_chunked() {
  int ret = COCO_SUCCESS;

  if (infinite_chunked) {
    return ret;
  }

  if (is_chunked() || content_length() != -1) {
    ret = ERROR_HTTP_DATA_INVALID;
    coco_error("infinite chunkted not supported in specified codec. ret=%d",
               ret);
    return ret;
  }

  infinite_chunked = true;

  return ret;
}

int HttpMessage::body_read_all(std::string &body) {
  int ret = COCO_SUCCESS;

  // cache to read.
  char *buf = new char[HTTP_READ_CACHE_BYTES];
  CocoAutoFreeA(char, buf);

  // whatever, read util EOF.
  while (!_body->eof()) {
    int nb_read = 0;
    if ((ret = _body->Read(buf, HTTP_READ_CACHE_BYTES, &nb_read)) !=
        COCO_SUCCESS) {
      return ret;
    }

    if (nb_read > 0) {
      body.append(buf, nb_read);
    }
  }

  return ret;
}

HttpResponseReader *HttpMessage::body_reader() { return _body; }

void HttpMessage::set_content_length(int64_t len) {
  header_->content_length = len;
}

int64_t HttpMessage::content_length() { return header_->content_length; }

std::string HttpMessage::query_get(std::string key) {
  std::string v;

  if (_query.find(key) != _query.end()) {
    v = _query[key];
  }

  return v;
}

int HttpMessage::request_header_count() { return (int)headers_->size(); }

std::string HttpMessage::request_header_key_at(int index) {
  assert(index < request_header_count());
  HttpHeaderField item = (*headers_)[index];
  return item.first;
}

std::string HttpMessage::request_header_value_at(int index) {
  assert(index < request_header_count());
  HttpHeaderField item = (*headers_)[index];
  return item.second;
}

std::string HttpMessage::get_request_header(std::string name) {
  std::vector<HttpHeaderField>::iterator it;

  for (it = headers_->begin(); it != headers_->end(); ++it) {
    HttpHeaderField &elem = *it;
    std::string key = elem.first;
    std::string value = elem.second;
    if (key == name) {
      return value;
    }
  }

  return "";
}

bool HttpMessage::is_jsonp() { return jsonp; }
