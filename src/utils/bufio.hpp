#pragma once

#include <stddef.h>
#include <sys/types.h>

#include "utils/utils.hpp"

// A read buffer in front of an IoReader, like Go's bufio.Reader. A parser looks at the
// buffered bytes, consumes what it used, and whatever is left stays for the next message
// on the same stream.
class BufReader : public IoReader {
 public:
    explicit BufReader(IoReader *rd, size_t size = 4096);
    virtual ~BufReader();

    BufReader(const BufReader &) = delete;
    BufReader &operator=(const BufReader &) = delete;

    // Returns buffered bytes first. With nothing buffered, a read of at least the
    // buffer's capacity goes straight to the underlying reader, without a copy.
    int Read(void *buf, size_t size, ssize_t *nread) override;

    const char *Peek() const { return buf_ + r_; }
    size_t Buffered() const { return w_ - r_; }
    void Consume(size_t n);
    // Reads once from the underlying reader and appends to the buffer. A full buffer
    // grows up to max_size; beyond that it fails with ERROR_READER_BUFFER_OVERFLOW.
    int Fill(size_t max_size);

 private:
    IoReader *rd_;
    char *buf_;
    size_t cap_;
    // buf_[r_, w_) is buffered and not consumed.
    size_t r_ = 0;
    size_t w_ = 0;
};
