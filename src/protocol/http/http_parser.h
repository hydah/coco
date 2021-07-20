#pragma once
#include <map>

#include "http-parser/http_parser.h"

#include "protocol/http/http_basic.h"
#include "utils/utils.hpp"

/**
 * used to resolve the http uri.
 */
class HttpUri {
 public:
    HttpUri();
    virtual ~HttpUri();
    /**
     * initialize the http uri.
     */
    virtual int initialize(std::string _url);

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
    std::string url;
    std::string schema;
    std::string host;
    int port;
    std::string path;
    std::string raw_path;
    std::string query;
};

// for http header.
typedef std::pair<std::string, std::string> HttpHeaderField;
/**
 * wrapper for http-parser,
 * provides HTTP message originted service.
 */
class HttpParser {
 public:
    HttpParser();
    virtual ~HttpParser();

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
    virtual int ParseMessage(IoReaderWriter *io);
    std::string &GetUrl() { return url; };
    http_parser *GetHeader() { return &header_; };
    std::vector<HttpHeaderField> *GetHeaderField() { return &headers_; };
    FastBuffer *GetBuffer() { return buffer_; };

 private:
    /**
     * parse the HTTP message to member field: msg.
     */
    virtual int parse_message_imp(IoReaderWriter *io);

    static int on_message_begin(http_parser *parser);
    static int on_headers_complete(http_parser *parser);
    static int on_message_complete(http_parser *parser);
    static int on_url(http_parser *parser, const char *at, size_t length);
    static int on_header_field(http_parser *parser, const char *at, size_t length);
    static int on_header_value(http_parser *parser, const char *at, size_t length);
    static int on_body(http_parser *parser, const char *at, size_t length);

    http_parser_settings settings;
    http_parser parser;
    // the global parse buffer.
    FastBuffer *buffer_;
    const char *p_header_tail;
    char *p_body_start;
    // http parse data, reset before parse message.
    bool expect_field_name;
    std::string field_name;
    std::string field_value;
    HttpParseState state;
    std::string url;
    int header_parsed;

    std::vector<HttpHeaderField> headers_;
    http_parser header_;
};