#pragma once
#include <stdint.h>

#include <string>
#include <utility>
#include <vector>

// Status codes are http-parser's enum http_status: HTTP_STATUS_OK, HTTP_STATUS_NOT_FOUND...
#include "http-parser/http_parser.h"

// The CONSTS_HTTP_* names predate the http_status enum and are kept for existing code.
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

#define HTTP_CR '\r'
#define HTTP_LF '\n'
#define HTTP_CRLF "\r\n"
#define HTTP_CRLFCRLF "\r\n\r\n"

// the default timeout of each read on a server connection.
#define HTTP_RECV_TIMEOUT_US (60 * 1000 * 1000LL)
// the size of reads into a stack buffer.
#define HTTP_READ_CACHE_BYTES 4096
#define DEFAULT_HTTP_PORT 80

// The reason phrase for code, e.g. "Not Found"; empty when unknown.
const char *HttpStatusText(int code);
// RFC 7230 3.3: 1xx, 204 and 304 responses have no body.
bool HttpBodyAllowedForStatus(int code);
// Like Go's http.DetectContentType: sniffs at most the first 512 bytes and falls back to
// "application/octet-stream".
std::string HttpDetectContentType(const char *data, size_t size);

// Header fields in arrival order. Names compare case-insensitively (RFC 7230 3.2) and are
// sent as given.
class HttpHeader {
 public:
    typedef std::pair<std::string, std::string> Field;
    typedef std::vector<Field>::const_iterator const_iterator;

    // The first value of key, or "" when absent.
    const std::string &Get(const std::string &key) const;
    bool Has(const std::string &key) const;
    std::vector<std::string> Values(const std::string &key) const;
    // Replaces every value of key.
    void Set(const std::string &key, const std::string &value);
    // Appends a value, keeping those key already has.
    void Add(const std::string &key, const std::string &value);
    void Del(const std::string &key);
    // Whether any comma-separated element of key's values equals token, ignoring case,
    // e.g. HasToken("Connection", "upgrade") for "Connection: keep-alive, Upgrade".
    bool HasToken(const std::string &key, const char *token) const;

    size_t Size() const { return fields_.size(); }
    bool Empty() const { return fields_.empty(); }
    const_iterator begin() const { return fields_.begin(); }
    const_iterator end() const { return fields_.end(); }
    void Clear() { fields_.clear(); }

    // Appends "Name: value\r\n" for every field. CR and LF in a value become spaces, so a
    // value cannot inject header lines.
    void WriteTo(std::string *out) const;

 private:
    friend class HttpRequestParser;
    std::vector<Field> fields_;
};

// Query parameters or form values, like Go's url.Values.
class HttpValues {
 public:
    // Parses "a=1&b=2", unescaping keys and values; malformed pairs are skipped.
    static HttpValues Parse(const std::string &query);

    const std::string &Get(const std::string &key) const;
    bool Has(const std::string &key) const;
    std::vector<std::string> Values(const std::string &key) const;
    void Set(const std::string &key, const std::string &value);
    void Add(const std::string &key, const std::string &value);
    void Del(const std::string &key);
    size_t Size() const { return values_.size(); }
    // "a=1&b=2", escaped, in insertion order.
    std::string Encode() const;

 private:
    std::vector<std::pair<std::string, std::string>> values_;
};

// Escapes s for a query component: space becomes '+'.
std::string HttpQueryEscape(const std::string &s);
// Decodes %XX, and '+' as space; false on a malformed escape.
bool HttpQueryUnescape(const std::string &s, std::string *out);
// Decodes %XX only; false on a malformed escape.
bool HttpPathUnescape(const std::string &s, std::string *out);
// Like Go's path.Clean for a URL path, keeping a trailing slash: resolves "." and "..",
// merges repeated slashes and always starts with "/".
std::string HttpCleanPath(const std::string &p);
