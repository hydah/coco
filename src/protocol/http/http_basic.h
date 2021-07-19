#pragma once
#include <map>
#include <sstream>
#include <string>

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

// the http chunked header size,
// for writev, there always one chunk to send it.
#define HTTP_HEADER_CACHE_SIZE 64

class HttpHeader {
public:
  HttpHeader() = default;
  virtual ~HttpHeader() = default;

  // Add adds the key, value pair to the header.
  // It appends to any existing values associated with key.
  virtual void set(std::string key, std::string value);
  // Get gets the first value associated with the given key.
  // If there are no values associated with the key, Get returns "".
  // To access multiple values of a key, access the map directly
  // with CanonicalHeaderKey.
  virtual std::string get(std::string key);

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

  /**
   * get the content type. empty string if not set.
   */
  virtual std::string content_type();
  /**
   * set the content type by header "Content-Type"
   */
  virtual void set_content_type(std::string ct);

  /**
   * write all headers to string stream.
   */
  virtual void write(std::stringstream &ss);

private:
  std::map<std::string, std::string> headers;
  int64_t header_length = 0;
};