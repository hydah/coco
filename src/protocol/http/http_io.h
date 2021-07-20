#pragma once
#include <sstream>

#include "http_parser.h"

#include "protocol/http/http_basic.h"
#include "protocol/http/http_message.h"
#include "utils/utils.hpp"

class HttpResponseWriter {
 private:
    IoReaderWriter *io_;
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
    HttpResponseWriter(IoReaderWriter *io);
    virtual ~HttpResponseWriter();

 public:
    virtual int final_request();
    virtual HttpHeader *header();
    virtual int Write(char *data, int size);
    virtual int Writev(iovec *iov, int iovcnt, ssize_t *pnwrite);
    virtual void WriteHeader(int code);
    virtual int SendHeader(char *data, int size);
};

/**
 * response reader use st socket.
 */
class HttpResponseReader {
 private:
    IoReaderWriter *io_;
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
    HttpResponseReader(HttpMessage *msg, IoReaderWriter *io);
    virtual ~HttpResponseReader();

 public:
    /**
     * initialize the response reader with buffer.
     */
    virtual int initialize(FastBuffer *buffer);

 public:
    virtual bool eof();
    virtual int Read(char *data, int nb_data, int *nb_read);
    virtual int ReadFull(char *data, int nb_data, int *nb_read);

 private:
    virtual int ReadChunked(char *data, int nb_data, int *nb_read);
    virtual int ReadSpecified(char *data, int nb_data, int *nb_read);
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