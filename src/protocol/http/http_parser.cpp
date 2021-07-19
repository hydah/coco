#include "protocol/http/http_parser.h"

#include <assert.h>
#include <string.h>

#include "common/error.hpp"
#include "log/log.hpp"
#include "protocol/http/http_io.h"



HttpUri::HttpUri() { port = DEFAULT_HTTP_PORT; }

HttpUri::~HttpUri() {}

int HttpUri::initialize(std::string _url) {
  int ret = COCO_SUCCESS;

  url = _url;
  const char *purl = url.c_str();

  http_parser_url hp_u;
  if ((ret = http_parser_parse_url(purl, url.length(), 0, &hp_u)) != 0) {
    int code = ret;
    ret = ERROR_HTTP_PARSE_URI;

    coco_error("parse url %s failed, code=%d, ret=%d", purl, code, ret);
    return ret;
  }

  std::string field = get_uri_field(url, &hp_u, UF_SCHEMA);
  if (!field.empty()) {
    schema = field;
  }

  host = get_uri_field(url, &hp_u, UF_HOST);

  field = get_uri_field(url, &hp_u, UF_PORT);
  if (!field.empty()) {
    port = atoi(field.c_str());
  }

  path = get_uri_field(url, &hp_u, UF_PATH);
  coco_info("parse path %s success", path.c_str());

  query = get_uri_field(url, &hp_u, UF_QUERY);
  coco_info("parse query %s success", query.c_str());

  raw_path = path;
  if (!query.empty()) {
    raw_path += "?" + query;
  }

  return ret;
}

std::string HttpUri::get_uri_field(std::string uri, http_parser_url *hp_u,
                                   http_parser_url_fields field) {
  if ((hp_u->field_set & (1 << field)) == 0) {
    return "";
  }

  coco_dbg("uri field matched, off=%d, len=%d, value=%.*s",
           hp_u->field_data[field].off, hp_u->field_data[field].len,
           hp_u->field_data[field].len,
           uri.c_str() + hp_u->field_data[field].off);

  int offset = hp_u->field_data[field].off;
  int len = hp_u->field_data[field].len;

  return uri.substr(offset, len);
}

HttpParser::HttpParser() {
  buffer_ = new FastBuffer();
  expect_field_name = false;
  header_parsed = 0;
}

HttpParser::~HttpParser() { coco_freep(buffer_); }

int HttpParser::initialize(enum http_parser_type type) {
  int ret = COCO_SUCCESS;

  memset(&settings, 0, sizeof(settings));
  settings.on_message_begin = on_message_begin;
  settings.on_url = on_url;
  settings.on_header_field = on_header_field;
  settings.on_header_value = on_header_value;
  settings.on_headers_complete = on_headers_complete;
  settings.on_body = on_body;
  settings.on_message_complete = on_message_complete;

  http_parser_init(&parser, type);
  // callback object ptr.
  parser.data = (void *)this;
  p_body_start = nullptr;
  p_header_tail = nullptr;

  return ret;
}

int HttpParser::ParseMessage(IoReaderWriter *io) {
  int ret = COCO_SUCCESS;

  // reset request data.
  field_name = "";
  field_value = "";
  expect_field_name = true;
  state = HttpParseStateInit;
  url = "";
  header_parsed = 0;

  header_ = http_parser();
  headers_.clear();

  // do parse
  if ((ret = parse_message_imp(io)) != COCO_SUCCESS) {
    if (!coco_is_client_gracefully_close(ret)) {
      coco_error("parse http msg failed. ret=%d", ret);
    }
  }

  return ret;
}

int HttpParser::parse_message_imp(IoReaderWriter *io_) {
  int ret = COCO_SUCCESS;

  // while (true) {
  //     ssize_t nparsed = 0;

  //     // when got entire http header, parse it.
  //     char* start = buffer->bytes();
  //     char* end = start + buffer->size();
  //     for (char* p = start; p <= end - 4; p++) {
  //         // SRS_HTTP_CRLFCRLF "\r\n\r\n" // 0x0D0A0D0A
  //         if (p[0] == CONSTS_CR && p[1] == CONSTS_LF && p[2] == CONSTS_CR &&
  //         p[3] == CONSTS_LF) {
  //             nparsed = http_parser_execute(&parser, &settings,
  //             buffer->bytes(), buffer->size()); coco_info("buffer=%d,
  //             nparsed=%d, header=%d", buffer->size(), (int)nparsed,
  //             header_parsed);
  //             // break;
  //         }
  //     }

  //     // consume the parsed bytes.
  //     if (nparsed && header_parsed) {
  //         buffer->read_slice(header_parsed);
  //         coco_info("temp: size=%d,  %.*s", buffer->size(), buffer->size(),
  //         buffer->bytes());
  //     }

  //     // ok atleast header completed,
  //     // never wait for body completed, for maybe chunked.
  //     if (state == HttpParseStateHeaderComplete || state ==
  //     HttpParseStateMessageComplete) {
  //         break;
  //     }

  //     // when nothing parsed, read more to parse.
  //     if (nparsed == 0) {
  //         // when requires more, only grow 1bytes, but the buffer will cache
  //         more. if ((ret = buffer->grow(io_, buffer->size() + 1)) !=
  //         COCO_SUCCESS) {
  //             if (!coco_is_client_gracefully_close(ret)) {
  //                 coco_error("read body from server failed. ret=%d", ret);
  //             }
  //             return ret;
  //         }
  //     }
  // }

  while (true) {
    int32_t consumed = 0;
    if (buffer_->size() > 0) {
      ssize_t nparsed = http_parser_execute(&parser, &settings,
                                            buffer_->bytes(), buffer_->size());

      // When buffer consumed these bytes, it's dropped so the new ptr is
      // actually the HTTP body. But http-parser doesn't indicate the specific
      // sizeof header, so we must finger it out.
      // @remark We shouldn't use on_body, because it only works for normal
      // case, and losts the chunk header and length.
      if (p_header_tail && buffer_->bytes() < p_body_start) {
        for (const char *p = p_header_tail; p <= p_body_start - 4; p++) {
          if (p[0] == CONSTS_CR && p[1] == CONSTS_LF && p[2] == CONSTS_CR &&
              p[3] == CONSTS_LF) {
            consumed = int32_t(p + 4 - buffer_->bytes());
            break;
          }
        }
      }
      coco_info("size=%d, nparsed=%d, consumed=%d", buffer_->size(),
                (int)nparsed, consumed);

      // Only consume the header bytes.
      buffer_->read_slice(consumed);

      // Done when header completed, never wait for body completed, because it
      // maybe chunked.
      if (state >= HttpParseStateHeaderComplete) {
        break;
      }
    }

    // when requires more, only grow 1bytes, but the buffer will cache more.
    if ((ret = buffer_->grow(io_, buffer_->size() + 1)) != COCO_SUCCESS) {
      if (!coco_is_client_gracefully_close(ret)) {
        coco_error("read body from server failed. ret=%d", ret);
      }
      return ret;
    }
  }

  // parse last header.
  if (!field_name.empty() && !field_value.empty()) {
    headers_.push_back(std::make_pair(field_name, field_value));
  }

  return ret;
}

int HttpParser::on_message_begin(http_parser *parser) {
  HttpParser *obj = (HttpParser *)parser->data;
  assert(obj);

  obj->state = HttpParseStateStart;

  coco_info("***MESSAGE BEGIN***");

  return 0;
}

int HttpParser::on_headers_complete(http_parser *parser) {
  HttpParser *obj = (HttpParser *)parser->data;
  assert(obj);

  obj->header_ = *parser;
  // save the parser when header parse completed.
  obj->state = HttpParseStateHeaderComplete;
  obj->header_parsed = (int)parser->nread;

  coco_info("***HEADERS COMPLETE***");

  // see http_parser.c:1570, return 1 to skip body.
  return 0;
}

int HttpParser::on_message_complete(http_parser *parser) {
  HttpParser *obj = (HttpParser *)parser->data;
  assert(obj);

  // save the parser when body parse completed.
  obj->state = HttpParseStateMessageComplete;
  obj->p_body_start = obj->buffer_->bytes() + obj->buffer_->size();

  coco_info("***MESSAGE COMPLETE***\n");

  return 0;
}

int HttpParser::on_url(http_parser *parser, const char *at, size_t length) {
  HttpParser *obj = (HttpParser *)parser->data;
  assert(obj);

  if (length > 0) {
    obj->url.append(at, (int)length);
  }
  obj->p_header_tail = at;

  coco_info("Method: %d, Url: %.*s", parser->method, (int)length, at);

  return 0;
}

int HttpParser::on_header_field(http_parser *parser, const char *at,
                                size_t length) {
  HttpParser *obj = (HttpParser *)parser->data;
  assert(obj);

  // field value=>name, reap the field.
  if (!obj->expect_field_name) {
    obj->headers_.push_back(std::make_pair(obj->field_name, obj->field_value));

    // reset the field name when parsed.
    obj->field_name = "";
    obj->field_value = "";
  }
  obj->expect_field_name = true;

  if (length > 0) {
    obj->field_name.append(at, (int)length);
  }
  obj->p_header_tail = at;
  coco_info("Header field(%d bytes): %.*s", (int)length, (int)length, at);
  return 0;
}

int HttpParser::on_header_value(http_parser *parser, const char *at,
                                size_t length) {
  HttpParser *obj = (HttpParser *)parser->data;
  assert(obj);

  if (length > 0) {
    obj->field_value.append(at, (int)length);
  }
  obj->expect_field_name = false;
  obj->p_header_tail = at;
  coco_info("Header value(%d bytes): %.*s", (int)length, (int)length, at);
  return 0;
}

int HttpParser::on_body(http_parser *parser, const char *at, size_t length) {
  // HttpParser* obj = (HttpParser*)parser->data;
  // assert(obj);

  coco_info("Body:len:%d,  %.*s", (int)length, (int)length, at);

  return 0;
}