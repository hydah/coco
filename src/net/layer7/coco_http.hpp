#pragma once

#include "net/layer4/coco_tcp.hpp"
#include "thirdparty/http-parser/http_parser.h"
#include "utils/utils.hpp"
#include <sstream>

class HttpServerConn;
class HttpMessage;
class HttpMuxEntry;

// state of message
enum HttpParseState {
  HttpParseStateInit = 0,
  HttpParseStateStart,
  HttpParseStateHeaderComplete,
  HttpParseStateBodyStart,
  HttpParseStateMessageComplete
};

// Lines are terminated by either a single LF character or a CR
// character followed by an LF character.
// CR             = <US-ASCII CR, carriage return (13)>
#define CONSTS_CR '\r' // 0x0D
// LF             = <US-ASCII LF, linefeed (10)>
#define CONSTS_LF '\n' // 0x0A

///////////////////////////////////////////////////////////
// HTTP consts values
///////////////////////////////////////////////////////////
// linux path seprator
#define CONSTS_HTTP_PATH_SEP '/'
// query string seprator
#define CONSTS_HTTP_QUERY_SEP '?'

// the default recv timeout.
#define HTTP_RECV_TIMEOUT_US 60 * 1000 * 1000

// 6.1.1 Status Code and Reason Phrase
#define CONSTS_HTTP_Continue 100
#define CONSTS_HTTP_SwitchingProtocols 101
#define CONSTS_HTTP_OK 200
#define CONSTS_HTTP_Created 201
#define CONSTS_HTTP_Accepted 202
#define CONSTS_HTTP_NonAuthoritativeInformation 203
#define CONSTS_HTTP_NoContent 204
#define CONSTS_HTTP_ResetContent 205
#define CONSTS_HTTP_PartialContent 206
#define CONSTS_HTTP_MultipleChoices 300
#define CONSTS_HTTP_MovedPermanently 301
#define CONSTS_HTTP_Found 302
#define CONSTS_HTTP_SeeOther 303
#define CONSTS_HTTP_NotModified 304
#define CONSTS_HTTP_UseProxy 305
#define CONSTS_HTTP_TemporaryRedirect 307
#define CONSTS_HTTP_BadRequest 400
#define CONSTS_HTTP_Unauthorized 401
#define CONSTS_HTTP_PaymentRequired 402
#define CONSTS_HTTP_Forbidden 403
#define CONSTS_HTTP_NotFound 404
#define CONSTS_HTTP_MethodNotAllowed 405
#define CONSTS_HTTP_NotAcceptable 406
#define CONSTS_HTTP_ProxyAuthenticationRequired 407
#define CONSTS_HTTP_RequestTimeout 408
#define CONSTS_HTTP_Conflict 409
#define CONSTS_HTTP_Gone 410
#define CONSTS_HTTP_LengthRequired 411
#define CONSTS_HTTP_PreconditionFailed 412
#define CONSTS_HTTP_RequestEntityTooLarge 413
#define CONSTS_HTTP_RequestURITooLarge 414
#define CONSTS_HTTP_UnsupportedMediaType 415
#define CONSTS_HTTP_RequestedRangeNotSatisfiable 416
#define CONSTS_HTTP_ExpectationFailed 417
#define CONSTS_HTTP_InternalServerError 500
#define CONSTS_HTTP_NotImplemented 501
#define CONSTS_HTTP_BadGateway 502
#define CONSTS_HTTP_ServiceUnavailable 503
#define CONSTS_HTTP_GatewayTimeout 504
#define CONSTS_HTTP_HTTPVersionNotSupported 505

#define CONSTS_HTTP_Continue_str "Continue"
#define CONSTS_HTTP_SwitchingProtocols_str "Switching Protocols"
#define CONSTS_HTTP_OK_str "OK"
#define CONSTS_HTTP_Created_str "Created"
#define CONSTS_HTTP_Accepted_str "Accepted"
#define CONSTS_HTTP_NonAuthoritativeInformation_str                            \
  "Non Authoritative Information"
#define CONSTS_HTTP_NoContent_str "No Content"
#define CONSTS_HTTP_ResetContent_str "Reset Content"
#define CONSTS_HTTP_PartialContent_str "Partial Content"
#define CONSTS_HTTP_MultipleChoices_str "Multiple Choices"
#define CONSTS_HTTP_MovedPermanently_str "Moved Permanently"
#define CONSTS_HTTP_Found_str "Found"
#define CONSTS_HTTP_SeeOther_str "See Other"
#define CONSTS_HTTP_NotModified_str "Not Modified"
#define CONSTS_HTTP_UseProxy_str "Use Proxy"
#define CONSTS_HTTP_TemporaryRedirect_str "Temporary Redirect"
#define CONSTS_HTTP_BadRequest_str "Bad Request"
#define CONSTS_HTTP_Unauthorized_str "Unauthorized"
#define CONSTS_HTTP_PaymentRequired_str "Payment Required"
#define CONSTS_HTTP_Forbidden_str "Forbidden"
#define CONSTS_HTTP_NotFound_str "Not Found"
#define CONSTS_HTTP_MethodNotAllowed_str "Method Not Allowed"
#define CONSTS_HTTP_NotAcceptable_str "Not Acceptable"
#define CONSTS_HTTP_ProxyAuthenticationRequired_str                            \
  "Proxy Authentication Required"
#define CONSTS_HTTP_RequestTimeout_str "Request Timeout"
#define CONSTS_HTTP_Conflict_str "Conflict"
#define CONSTS_HTTP_Gone_str "Gone"
#define CONSTS_HTTP_LengthRequired_str "Length Required"
#define CONSTS_HTTP_PreconditionFailed_str "Precondition Failed"
#define CONSTS_HTTP_RequestEntityTooLarge_str "Request Entity Too Large"
#define CONSTS_HTTP_RequestURITooLarge_str "Request URI Too Large"
#define CONSTS_HTTP_UnsupportedMediaType_str "Unsupported Media Type"
#define CONSTS_HTTP_RequestedRangeNotSatisfiable_str                           \
  "Requested Range Not Satisfiable"
#define CONSTS_HTTP_ExpectationFailed_str "Expectation Failed"
#define CONSTS_HTTP_InternalServerError_str "Internal Server Error"
#define CONSTS_HTTP_NotImplemented_str "Not Implemented"
#define CONSTS_HTTP_BadGateway_str "Bad Gateway"
#define CONSTS_HTTP_ServiceUnavailable_str "Service Unavailable"
#define CONSTS_HTTP_GatewayTimeout_str "Gateway Timeout"
#define CONSTS_HTTP_HTTPVersionNotSupported_str "HTTP Version Not Supported"

// http specification
// CR             = <US-ASCII CR, carriage return (13)>
#define HTTP_CR CONSTS_CR // 0x0D
// LF             = <US-ASCII LF, linefeed (10)>
#define HTTP_LF CONSTS_LF // 0x0A
// SP             = <US-ASCII SP, space (32)>
#define HTTP_SP ' ' // 0x20
// HT             = <US-ASCII HT, horizontal-tab (9)>
#define HTTP_HT '\x09' // 0x09

// HTTP/1.1 defines the sequence CR LF as the end-of-line marker for all
// protocol elements except the entity-body (see appendix 19.3 for
// tolerant applications).
#define HTTP_CRLF "\r\n"         // 0x0D0A
#define HTTP_CRLFCRLF "\r\n\r\n" // 0x0D0A0D0A

// for read all of http body, read each time.
#define HTTP_READ_CACHE_BYTES 4096

// default http listen port.
#define DEFAULT_HTTP_PORT 80

class HttpHeader {
private:
  std::map<std::string, std::string> headers;
  int64_t header_length;

public:
  HttpHeader();
  virtual ~HttpHeader();

public:
  // Add adds the key, value pair to the header.
  // It appends to any existing values associated with key.
  virtual void set(std::string key, std::string value);
  // Get gets the first value associated with the given key.
  // If there are no values associated with the key, Get returns "".
  // To access multiple values of a key, access the map directly
  // with CanonicalHeaderKey.
  virtual std::string get(std::string key);

public:
  /**
   * get the content length. -1 if not set.
   */
  virtual int64_t content_length();
  /**
   * set the content length by header "Content-Length"
   */
  virtual void set_content_length(int64_t size);

  virtual int64_t get_header_length();
  virtual void set_header_length(int64_t size);

public:
  /**
   * get the content type. empty string if not set.
   */
  virtual std::string content_type();
  /**
   * set the content type by header "Content-Type"
   */
  virtual void set_content_type(std::string ct);

public:
  /**
   * write all headers to string stream.
   */
  virtual void write(std::stringstream &ss);
};

// the http chunked header size,
// for writev, there always one chunk to send it.
#define HTTP_HEADER_CACHE_SIZE 64
class HttpResponseWriter {
private:
  CocoSocket *skt;
  HttpHeader *hdr;

private:
  char header_cache[HTTP_HEADER_CACHE_SIZE];
  iovec *iovss_cache;
  int nb_iovss_cache;

private:
  // reply header has been (logically) written
  bool header_wrote;
  // status code passed to WriteHeader
  int status;

private:
  // explicitly-declared Content-Length; or -1
  int64_t content_length;
  // number of bytes written in body
  int64_t written;

private:
  // wroteHeader tells whether the header's been written to "the
  // wire" (or rather: w.conn.buf). this is unlike
  // (*response).wroteHeader, which tells only whether it was
  // logically written.
  bool header_sent;

public:
  HttpResponseWriter(CocoSocket *io);
  virtual ~HttpResponseWriter();

public:
  virtual int final_request();
  virtual HttpHeader *header();
  virtual int write(char *data, int size);
  virtual int writev(iovec *iov, int iovcnt, ssize_t *pnwrite);
  virtual void write_header(int code);
  virtual int send_header(char *data, int size);
  virtual CocoSocket *st_socket();
};

/**
 * response reader use st socket.
 */
class HttpResponseReader {
private:
  CocoSocket *skt;
  HttpMessage *owner;
  FastBuffer *buffer;
  bool is_eof;
  // the left bytes in chunk.
  int nb_left_chunk;
  // the number of bytes of current chunk.
  int nb_chunk;
  // already read total bytes.
  int64_t nb_total_read;

public:
  HttpResponseReader(HttpMessage *msg, CocoSocket *io);
  virtual ~HttpResponseReader();

public:
  /**
   * initialize the response reader with buffer.
   */
  virtual int initialize(FastBuffer *buffer);

public:
  virtual bool eof();
  virtual int read(char *data, int nb_data, int *nb_read);
  virtual int read_full(char *data, int nb_data, int *nb_read);

private:
  virtual int read_chunked(char *data, int nb_data, int *nb_read);
  virtual int read_specified(char *data, int nb_data, int *nb_read);
};

// get the status text of code.
extern std::string generate_http_status_text(int status);
// Error replies to the request with the specified error message and HTTP code.
// The error message should be plain text.
extern int go_http_error(HttpResponseWriter *w, int code);
extern int go_http_error(HttpResponseWriter *w, int code, std::string error);

// bodyAllowedForStatus reports whether a given response status code
// permits a body.  See RFC2616, section 4.4.
extern bool go_http_body_allowd(int status);

// DetectContentType implements the algorithm described
// at http://mimesniff.spec.whatwg.org/ to determine the
// Content-Type of the given data.  It considers at most the
// first 512 bytes of data.  DetectContentType always returns
// a valid MIME type: if it cannot determine a more specific one, it
// returns "application/octet-stream".
extern std::string go_http_detect(char *data, int size);

/**
 * used to resolve the http uri.
 */
class HttpUri {
private:
  std::string url;
  std::string schema;
  std::string host;
  int port;
  std::string path;
  std::string raw_path;
  std::string query;

public:
  HttpUri();
  virtual ~HttpUri();

public:
  /**
   * initialize the http uri.
   */
  virtual int initialize(std::string _url);

public:
  virtual const char *get_url() { return url.data(); };
  virtual const char *get_schema() { return schema.data(); };
  virtual const char *get_host() { return host.data(); };
  virtual int get_port() { return port; };
  virtual const char *get_path() { return path.data(); };
  virtual const char *get_query() { return query.data(); };
  virtual const char *get_raw_path() { return raw_path.data(); };
  virtual bool update_path(const std::string new_path) {
    path = new_path;
    return true;
  };
  virtual bool update_host(const std::string new_host) {
    host = new_host;
    return true;
  };

private:
  /**
   * get the parsed url field.
   * @return return empty string if not set.
   */
  virtual std::string get_uri_field(std::string uri, http_parser_url *hp_u,
                                    http_parser_url_fields field);
};

// for http header.
typedef std::pair<std::string, std::string> HttpHeaderField;
/**
 * wrapper for http-parser,
 * provides HTTP message originted service.
 */
class HttpParser {
private:
  http_parser_settings settings;
  http_parser parser;
  // the global parse buffer.
  FastBuffer *buffer;
  const char *p_header_tail;
  char *p_body_start;

private:
  // http parse data, reset before parse message.
  bool expect_field_name;
  std::string field_name;
  std::string field_value;
  HttpParseState state;
  http_parser header;
  std::string url;
  std::vector<HttpHeaderField> headers;
  int header_parsed;

public:
  HttpParser();
  virtual ~HttpParser();

public:
  /**
   * initialize the http parser with specified type,
   * one parser can only parse request or response messages.
   */
  virtual int initialize(enum http_parser_type type);
  /**
   * always parse a http message,
   * that is, the *ppmsg always NOT-NULL when return success.
   * or error and *ppmsg must be NULL.
   * @remark, if success, *ppmsg always NOT-NULL, *ppmsg always is_complete().
   */
  virtual int parse_message(CocoSocket *skt, HttpServerConn *conn,
                            HttpMessage **ppmsg);

private:
  /**
   * parse the HTTP message to member field: msg.
   */
  virtual int parse_message_imp(CocoSocket *skt);

private:
  static int on_message_begin(http_parser *parser);
  static int on_headers_complete(http_parser *parser);
  static int on_message_complete(http_parser *parser);
  static int on_url(http_parser *parser, const char *at, size_t length);
  static int on_header_field(http_parser *parser, const char *at,
                             size_t length);
  static int on_header_value(http_parser *parser, const char *at,
                             size_t length);
  static int on_body(http_parser *parser, const char *at, size_t length);
};

class HttpMessage {
private:
  // parsed url.
  std::string _url;
  // the extension of file, for example, .flv
  std::string _ext;
  // parsed http header.
  http_parser _header;
  /**
   * body object, reader object.
   * @remark, user can get body in string by get_body().
   */
  HttpResponseReader *_body;
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
  /**
   * uri parser
   */
  HttpUri *_uri;

  // http headers
  std::vector<HttpHeaderField> _headers;
  // the query map
  std::map<std::string, std::string> _query;
  // the transport connection, can be NULL.
  HttpServerConn *conn;
  // whether request is jsonp.
  bool jsonp;
  // the method in QueryString will override the HTTP method.
  std::string jsonp_method;

public:
  HttpMessage(CocoSocket *io, HttpServerConn *c);
  virtual ~HttpMessage();

public:
  virtual int update_buffer(FastBuffer *body);
  /**
   * set the original messages, then update the message.
   */
  virtual int update(std::string url, http_parser *header, FastBuffer *body,
                     std::vector<HttpHeaderField> &headers);
  virtual HttpResponseReader *get_http_response_reader();

public:
  virtual HttpServerConn *connection();

public:
  virtual u_int8_t method();
  virtual u_int16_t status_code() { return (u_int16_t)_header.status_code; };
  /**
   * method helpers.
   */
  virtual std::string method_str();
  virtual bool is_http_get() { return method() == HTTP_GET; };
  virtual bool is_http_put() { return method() == HTTP_PUT; };
  virtual bool is_http_post() { return method() == HTTP_POST; };
  virtual bool is_http_delete() { return method() == HTTP_DELETE; };
  virtual bool is_http_options() { return _header.method == HTTP_OPTIONS; };
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

public:
  virtual int enter_infinite_chunked();

public:
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

public:
  virtual bool is_jsonp();
};

// Objects implementing the Handler interface can be
// registered to serve a particular path or subtree
// in the HTTP server.
//
// ServeHTTP should write reply headers and data to the ResponseWriter
// and then return.  Returning signals that the request is finished
// and that the HTTP server can move on to the next request on
// the connection.
class IHttpHandler {
public:
  HttpMuxEntry *entry;

public:
  IHttpHandler();
  virtual ~IHttpHandler();

public:
  virtual bool is_not_found();
  virtual int serve_http(HttpResponseWriter *w, HttpMessage *r) = 0;
};

// Redirect to a fixed URL
class HttpRedirectHandler : public IHttpHandler {
private:
  std::string url;
  int code;

public:
  HttpRedirectHandler(std::string u, int c);
  virtual ~HttpRedirectHandler();

public:
  virtual int serve_http(HttpResponseWriter *w, HttpMessage *r);
};

// NotFound replies to the request with an HTTP 404 not found error.
class HttpNotFoundHandler : public IHttpHandler {
public:
  HttpNotFoundHandler();
  virtual ~HttpNotFoundHandler();

public:
  virtual bool is_not_found();
  virtual int serve_http(HttpResponseWriter *w, HttpMessage *r);
};

// the mux entry for server mux.
// the matcher info, for example, the pattern and handler.
class HttpMuxEntry {
public:
  bool explicit_match;
  IHttpHandler *handler;
  std::string pattern;
  bool enabled;

public:
  HttpMuxEntry();
  virtual ~HttpMuxEntry();
};

// ServeMux is an HTTP request multiplexer.
// It matches the URL of each incoming request against a list of registered
// patterns and calls the handler for the pattern that
// most closely matches the URL.
//
// Patterns name fixed, rooted paths, like "/favicon.ico",
// or rooted subtrees, like "/images/" (note the trailing slash).
// Longer patterns take precedence over shorter ones, so that
// if there are handlers registered for both "/images/"
// and "/images/thumbnails/", the latter handler will be
// called for paths beginning "/images/thumbnails/" and the
// former will receive requests for any other paths in the
// "/images/" subtree.
//
// Note that since a pattern ending in a slash names a rooted subtree,
// the pattern "/" matches all paths not matched by other registered
// patterns, not just the URL with Path == "/".
//
// Patterns may optionally begin with a host name, restricting matches to
// URLs on that host only.  Host-specific patterns take precedence over
// general patterns, so that a handler might register for the two patterns
// "/codesearch" and "codesearch.google.com/" without also taking over
// requests for "http://www.google.com/".
//
// ServeMux also takes care of sanitizing the URL request path,
// redirecting any request containing . or .. elements to an
// equivalent .- and ..-free URL.
class HttpServeMux {
private:
  // the pattern handler, to handle the http request.
  std::map<std::string, HttpMuxEntry *> entries;
  // the vhost handler.
  // when find the handler to process the request,
  // append the matched vhost when pattern not starts with /,
  // for example, for pattern /live/livestream.flv of vhost ossrs.net,
  // the path will rewrite to ossrs.net/live/livestream.flv
  std::map<std::string, IHttpHandler *> vhosts;
  void *connection_;

public:
  HttpServeMux();
  virtual ~HttpServeMux();

public:
  /**
   * initialize the http serve mux.
   */
  virtual int initialize();
  void SetConnection(void *connection) { this->connection_ = connection; };

public:
  // Handle registers the handler for the given pattern.
  // If a handler already exists for pattern, Handle panics.
  virtual int handle(std::string pattern, IHttpHandler *handler);
  virtual void remove_entry(std::string pattern);
  // whether the http muxer can serve the specified message,
  // if not, user can try next muxer.
  virtual bool can_serve(HttpMessage *r);

public:
  virtual int serve_http(HttpResponseWriter *w, HttpMessage *r);

private:
  virtual int find_handler(HttpMessage *r, IHttpHandler **ph);
  virtual int match(HttpMessage *r, IHttpHandler **ph);
  virtual bool path_match(std::string pattern, std::string path);
};

class HttpServerConn : public ConnRoutine {
private:
  TcpConn *_conn;
  HttpServeMux *_mux;
  HttpParser *_parser;

public:
  HttpServerConn(ConnManager *manager, TcpConn *conn, HttpServeMux *mux);
  virtual ~HttpServerConn();
  virtual std::string GetRemoteAddr() { return _conn->RemoteAddr(); };

public:
  virtual int DoCycle();
  int ProcessRequest(HttpResponseWriter *w, HttpMessage *r);
};

class HttpServer : public ListenRoutine {
private:
  TcpListener *_l;
  HttpServeMux *_mux;
  ConnManager *manager;

public:
  HttpServer();
  virtual ~HttpServer();

public:
  virtual int ListenAndServe(std::string local_ip, int local_port,
                             HttpServeMux *mux);
  virtual int Serve(TcpListener *l, HttpServeMux *mux);

public:
  virtual int Cycle();
};

// the default timeout for http client. 1s
#define HTTP_CLIENT_TIMEOUT_US (int64_t)(1 * 1000 * 1000LL)

/**
 * http client to GET/POST/PUT/DELETE uri
 */
class HttpClient {
private:
  bool connected;
  bool is_https;
  st_netfd_t stfd;
  CocoSocket *skt;
  TcpConn *conn;
  HttpParser *parser;

private:
  int64_t timeout_us;
  // host name or ip.
  std::string host;
  int port;

public:
  HttpClient();
  virtual ~HttpClient();

public:
  /**
   * initialize the client, connect to host and port.
   */
  virtual int initialize(std::string h, int p,
                         int64_t t_us = HTTP_CLIENT_TIMEOUT_US,
                         std::string url = "");

public:
  /**
   * to post data to the uri.
   * @param the path to request on.
   * @param req the data post to uri. empty string to ignore.
   * @param ppmsg output the http message to read the response.
   * @param request_id request id, used for trace log.
   */
  virtual int post(std::string path, std::string req, HttpMessage **ppmsg,
                   std::string request_id = "");
  /**
   * to get data from the uri.
   * @param the path to request on.
   * @param req the data post to uri. empty string to ignore.
   * @param ppmsg output the http message to read the response.
   * @param request_id request id, used for trace log.
   */
  virtual int get(std::string path, std::string req, HttpMessage **ppmsg,
                  std::string request_id = "");

  /**
   * to set https mode flag
   * @param flag to be set to bHttps
   */
  virtual void set_https_flag(bool b_flag);

private:
  virtual void disconnect();
  virtual int connect();
};