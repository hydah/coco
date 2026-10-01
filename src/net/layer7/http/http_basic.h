#pragma once
#include <stdint.h>

#include <string>
#include <utility>
#include <vector>

#include "http-parser/http_parser.h"

// Status codes, named as in Go's net/http.
enum HttpStatus {
    HttpStatusContinue = 100,
    HttpStatusSwitchingProtocols = 101,

    HttpStatusOK = 200,
    HttpStatusCreated = 201,
    HttpStatusAccepted = 202,
    HttpStatusNonAuthoritativeInfo = 203,
    HttpStatusNoContent = 204,
    HttpStatusResetContent = 205,
    HttpStatusPartialContent = 206,

    HttpStatusMultipleChoices = 300,
    HttpStatusMovedPermanently = 301,
    HttpStatusFound = 302,
    HttpStatusSeeOther = 303,
    HttpStatusNotModified = 304,
    HttpStatusUseProxy = 305,
    HttpStatusTemporaryRedirect = 307,
    HttpStatusPermanentRedirect = 308,

    HttpStatusBadRequest = 400,
    HttpStatusUnauthorized = 401,
    HttpStatusPaymentRequired = 402,
    HttpStatusForbidden = 403,
    HttpStatusNotFound = 404,
    HttpStatusMethodNotAllowed = 405,
    HttpStatusNotAcceptable = 406,
    HttpStatusProxyAuthRequired = 407,
    HttpStatusRequestTimeout = 408,
    HttpStatusConflict = 409,
    HttpStatusGone = 410,
    HttpStatusLengthRequired = 411,
    HttpStatusPreconditionFailed = 412,
    HttpStatusRequestEntityTooLarge = 413,
    HttpStatusRequestURITooLong = 414,
    HttpStatusUnsupportedMediaType = 415,
    HttpStatusRequestedRangeNotSatisfiable = 416,
    HttpStatusExpectationFailed = 417,
    HttpStatusUnprocessableEntity = 422,
    HttpStatusTooManyRequests = 429,
    HttpStatusRequestHeaderFieldsTooLarge = 431,

    HttpStatusInternalServerError = 500,
    HttpStatusNotImplemented = 501,
    HttpStatusBadGateway = 502,
    HttpStatusServiceUnavailable = 503,
    HttpStatusGatewayTimeout = 504,
    HttpStatusHTTPVersionNotSupported = 505,
};

// Methods, as in Go's net/http. HttpRequest::method holds one of these.
constexpr char HttpMethodGet[] = "GET";
constexpr char HttpMethodHead[] = "HEAD";
constexpr char HttpMethodPost[] = "POST";
constexpr char HttpMethodPut[] = "PUT";
constexpr char HttpMethodPatch[] = "PATCH";
constexpr char HttpMethodDelete[] = "DELETE";
constexpr char HttpMethodConnect[] = "CONNECT";
constexpr char HttpMethodOptions[] = "OPTIONS";
constexpr char HttpMethodTrace[] = "TRACE";

// Common Content-Type values.
constexpr char HttpContentTypeText[] = "text/plain; charset=utf-8";
constexpr char HttpContentTypeHtml[] = "text/html; charset=utf-8";
constexpr char HttpContentTypeJson[] = "application/json";
constexpr char HttpContentTypeForm[] = "application/x-www-form-urlencoded";
constexpr char HttpContentTypeOctetStream[] = "application/octet-stream";

// Header field names. Lookups ignore case, so these are only to avoid typos.
constexpr char HttpHeaderAllow[] = "Allow";
constexpr char HttpHeaderAuthorization[] = "Authorization";
constexpr char HttpHeaderConnection[] = "Connection";
constexpr char HttpHeaderContentLength[] = "Content-Length";
constexpr char HttpHeaderContentType[] = "Content-Type";
constexpr char HttpHeaderCookie[] = "Cookie";
constexpr char HttpHeaderDate[] = "Date";
constexpr char HttpHeaderExpect[] = "Expect";
constexpr char HttpHeaderHost[] = "Host";
constexpr char HttpHeaderLocation[] = "Location";
constexpr char HttpHeaderSetCookie[] = "Set-Cookie";
constexpr char HttpHeaderTransferEncoding[] = "Transfer-Encoding";
constexpr char HttpHeaderUpgrade[] = "Upgrade";
constexpr char HttpHeaderUserAgent[] = "User-Agent";
constexpr char HttpHeaderWWWAuthenticate[] = "WWW-Authenticate";

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
