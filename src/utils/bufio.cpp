#include "utils/bufio.hpp"

#include <string.h>

#include <algorithm>

#include "common/error.hpp"

BufReader::BufReader(IoReader *rd, size_t size) : rd_(rd), buf_(new char[size]), cap_(size) {}

BufReader::~BufReader() { delete[] buf_; }

void BufReader::Consume(size_t n) {
    r_ += std::min(n, Buffered());
    if (r_ == w_) {
        r_ = w_ = 0;
    }
}

int BufReader::Fill(size_t max_size) {
    if (w_ == cap_ && r_ > 0) {
        memmove(buf_, buf_ + r_, w_ - r_);
        w_ -= r_;
        r_ = 0;
    }
    if (w_ == cap_) {
        if (cap_ >= max_size) {
            return ERROR_READER_BUFFER_OVERFLOW;
        }
        size_t cap = std::min(cap_ * 2, max_size);
        char *buf = new char[cap];
        memcpy(buf, buf_, w_);
        delete[] buf_;
        buf_ = buf;
        cap_ = cap;
    }

    ssize_t n = 0;
    int ret = rd_->Read(buf_ + w_, cap_ - w_, &n);
    if (ret != COCO_SUCCESS) {
        return ret;
    }
    if (n <= 0) {
        return ERROR_SOCKET_READ;
    }
    w_ += (size_t)n;
    return COCO_SUCCESS;
}

int BufReader::Read(void *buf, size_t size, ssize_t *nread) {
    if (Buffered() == 0) {
        if (size >= cap_) {
            return rd_->Read(buf, size, nread);
        }
        int ret = Fill(cap_);
        if (ret != COCO_SUCCESS) {
            if (nread) {
                *nread = 0;
            }
            return ret;
        }
    }
    size_t n = std::min(size, Buffered());
    memcpy(buf, Peek(), n);
    Consume(n);
    if (nread) {
        *nread = (ssize_t)n;
    }
    return COCO_SUCCESS;
}
