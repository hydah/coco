#pragma once
#include <map>
#include <sstream>

#include "protocol/http/http_parser.h"
#include "utils/utils.hpp"
class HttpResponseReader;

class HttpMessage {
public:
  HttpMessage();
  virtual ~HttpMessage();

public:
  virtual int Initialize(enum http_parser_type type);
  virtual int Parse(IoReaderWriter *io, void *c);

  virtual int update_buffer(FastBuffer *body);
  virtual HttpResponseReader *get_http_response_reader();
  virtual void *GetObserver() { return observer_; };
  virtual u_int8_t method();
  virtual u_int16_t status_code() { return (u_int16_t)header_->status_code; };
  /**
   * method helpers.
   */
  virtual std::string method_str();
  virtual bool is_http_get() { return method() == HTTP_GET; };
  virtual bool is_http_put() { return method() == HTTP_PUT; };
  virtual bool is_http_post() { return method() == HTTP_POST; };
  virtual bool is_http_delete() { return method() == HTTP_DELETE; };
  virtual bool is_http_options() { return header_->method == HTTP_OPTIONS; };
  /**
   * whether body is chunked encoding, for reader only.
   */
  virtual bool is_chunked() { return chunked; };
  /**
   * whether body is infinite chunked encoding.
   * @remark set by enter_infinite_chunked.
   */
  virtual bool is_infinite_chunked() { return infinite_chunked; };
  /**
   * whether should keep the connection alive.
   */
  virtual bool is_keep_alive() { return keep_alive; };
  /**
   * the uri contains the host and path.
   */
  virtual std::string uri();
  /**
   * the url maybe the path.
   */
  virtual std::string url() { return _uri->get_url(); };
  virtual std::string host() { return _uri->get_host(); };
  virtual std::string get_host() { return host(); };
  virtual std::string path() { return _uri->get_path(); };
  virtual std::string query() { return _uri->get_query(); };
  virtual std::string ext() { return _ext; };
  /**
   * get the RESTful matched id.
   */
  virtual int parse_rest_id(std::string pattern);

  virtual int enter_infinite_chunked();
  /**
   * read body to string.
   * @remark for small http body.
   */
  virtual int body_read_all(std::string &body);
  /**
   * get the body reader, to read one by one.
   * @remark when body is very large, or chunked, use this.
   */
  virtual HttpResponseReader *body_reader();
  /**
   * the content length, -1 for chunked or not set.
   */
  virtual int64_t content_length();
  virtual void set_content_length(int64_t len);
  /**
   * get the param in query string,
   * for instance, query is "start=100&end=200",
   * then query_get("start") is "100", and query_get("end") is "200"
   */
  virtual std::string query_get(std::string key);
  /**
   * get the headers.
   */
  virtual int request_header_count();
  virtual std::string request_header_key_at(int index);
  virtual std::string request_header_value_at(int index);
  virtual std::string get_request_header(std::string name);
  virtual bool is_jsonp();

private:
  // the transport connection, can be NULL.
  void *observer_;
  IoReaderWriter *io_;
  // parsed http header.
  http_parser *header_;
  std::vector<HttpHeaderField> *headers_;
  HttpParser *parser_ = nullptr;
  /**
   * uri parser
   */
  HttpUri *_uri = nullptr;
  /**
   * body object, reader object.
   * @remark, user can get body in string by get_body().
   */
  HttpResponseReader *_body = nullptr;

  // parsed url.
  std::string _url;
  // the extension of file, for example, .flv
  std::string _ext;
  /**
   * whether the body is chunked.
   */
  bool chunked;
  /**
   * whether the body is infinite chunked.
   */
  bool infinite_chunked;
  // whether the request indicates should keep alive for the http connection.
  bool keep_alive;
  // whether request is jsonp.
  bool jsonp;
  // the method in QueryString will override the HTTP method.
  std::string jsonp_method;
  // http headers
  // the query map
  std::map<std::string, std::string> _query;
};