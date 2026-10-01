#pragma once

#include <stddef.h>
#include <sys/types.h>
#include <sys/uio.h>

namespace coco {

class IoReader {
 public:
    IoReader() = default;
    virtual ~IoReader() = default;

    virtual int Read(void *buf, size_t size, ssize_t *nread) = 0;
};

class IoWriter {
 public:
    IoWriter() = default;
    virtual ~IoWriter() = default;

    virtual int Write(void *buf, size_t size, ssize_t *nwrite) = 0;
    virtual int Writev(const iovec *iov, int iov_size, ssize_t *nwrite) = 0;
};

class IoReaderWriter : public IoReader, public IoWriter {
 public:
    IoReaderWriter() = default;
    virtual ~IoReaderWriter() = default;
};

}  // namespace coco
