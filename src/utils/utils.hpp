#pragma once

#include <string>
#include <sys/uio.h>
#include <vector>

#include <arpa/inet.h>


extern bool is_ipv6(std::string ip);
// get local ip, fill to @param ips
extern std::vector<std::string> &get_local_ipv4_ips();

// get local public ip, empty string if no public internet address found.
extern std::string get_public_internet_address();

// compare
#define coco_min(a, b) (((a) < (b)) ? (a) : (b))
#define coco_max(a, b) (((a) < (b)) ? (b) : (a))

// free the p and set to NULL.
// p must be a T*.
#define coco_freep(p)                                                          \
  if (p) {                                                                     \
    delete p;                                                                  \
    p = NULL;                                                                  \
  }                                                                            \
  (void)0
// please use the freepa(T[]) to free an array,
// or the behavior is undefined.
#define coco_freepa(pa)                                                        \
  if (pa) {                                                                    \
    delete[] pa;                                                               \
    pa = NULL;                                                                 \
  }                                                                            \
  (void)0

/**
 * auto free the instance in the current scope, for instance, MyClass* ptr,
 * which is a ptr and this class will:
 *       1. free the ptr.
 *       2. set ptr to NULL.
 *
 * Usage:
 *       MyClass* po = new MyClass();
 *       // ...... use po
 *       SrsAutoFree(MyClass, po);
 *
 * Usage for array:
 *      MyClass** pa = new MyClass*[size];
 *      // ....... use pa
 *      CocoAutoFreeA(MyClass*, pa);
 *
 * @remark the MyClass can be basic type, for instance, CocoAutoFreeA(char,
 * pstr), where the char* pstr = new char[size].
 */
#define CocoAutoFree(className, instance)                                      \
  impl__CocoAutoFree<className> _auto_free_##instance(&instance, false)
#define CocoAutoFreeA(className, instance)                                     \
  impl__CocoAutoFree<className> _auto_free_array_##instance(&instance, true)
template <class T> class impl__CocoAutoFree {
private:
  T **ptr;
  bool is_array;

public:
  /**
   * auto delete the ptr.
   */
  impl__CocoAutoFree(T **p, bool array) {
    ptr = p;
    is_array = array;
  }

  virtual ~impl__CocoAutoFree() {
    if (ptr == NULL || *ptr == NULL) {
      return;
    }

    if (is_array) {
      delete[] * ptr;
    } else {
      delete *ptr;
    }

    *ptr = NULL;
  }
};

class IBufferReader {
public:
  IBufferReader();
  virtual ~IBufferReader();

public:
  virtual int read(void *buf, size_t size, ssize_t *nread) = 0;
};

/**
 * the writer for the buffer to write to whatever channel.
 */
class IBufferWriter {
public:
  IBufferWriter();
  virtual ~IBufferWriter();

public:
  /**
   * write bytes over writer.
   * @nwrite the actual written bytes. NULL to ignore.
   */
  virtual int write(void *buf, size_t size, ssize_t *nwrite) = 0;
  /**
   * write iov over writer.
   * @nwrite the actual written bytes. NULL to ignore.
   */
  virtual int writev(const iovec *iov, int iov_size, ssize_t *nwrite) = 0;
};

/**
 * the buffer provices bytes cache for protocol. generally,
 * protocol recv data from socket, put into buffer, decode to RTMP message.
 * Usage:
 *       IBufferReader* r = ......;
 *       FastBuffer* fb = ......;
 *       fb->grow(r, 1024);
 *       char* header = fb->read_slice(100);
 *       char* payload = fb->read_payload(924);
 */
// TODO: FIXME: add utest for it.
class FastBuffer {
private:
  // the user-space buffer to fill by reader,
  // which use fast index and reset when chunk body read ok.
  // @see https://github.com/ossrs/srs/issues/248
  // ptr to the current read position.
  char *p;
  // ptr to the content end.
  char *end;
  // ptr to the buffer.
  //      buffer <= p <= end <= buffer+nb_buffer
  char *buffer;
  // the size of buffer.
  int nb_buffer;

public:
  FastBuffer();
  virtual ~FastBuffer();

public:
  /**
   * get the size of current bytes in buffer.
   */
  virtual int size();
  /**
   * get the current bytes in buffer.
   * @remark user should use read_slice() if possible,
   *       the bytes() is used to test bytes, for example, to detect the bytes
   * schema.
   */
  virtual char *bytes();
  /**
   * create buffer with specifeid size.
   * @param buffer the size of buffer. ignore when smaller than
   * SRS_MAX_SOCKET_BUFFER.
   * @remark when MR(SRS_PERF_MERGED_READ) disabled, always set to 8K.
   * @remark when buffer changed, the previous ptr maybe invalid.
   * @see https://github.com/ossrs/srs/issues/241
   */
  virtual void set_buffer(int buffer_size);

public:
  /**
   * read 1byte from buffer, move to next bytes.
   * @remark assert buffer already grow(1).
   */
  virtual char read_1byte();
  /**
   * read a slice in size bytes, move to next bytes.
   * user can use this char* ptr directly, and should never free it.
   * @remark user can use the returned ptr util grow(size),
   *       for the ptr returned maybe invalid after grow(x).
   */
  virtual char *read_slice(int size);
  /**
   * skip some bytes in buffer.
   * @param size the bytes to skip. positive to next; negative to previous.
   * @remark assert buffer already grow(size).
   * @remark always use read_slice to consume bytes, which will reset for EOF.
   *       while skip never consume bytes.
   */
  virtual void skip(int size);
  virtual int update(char *data, int required_size);

public:
  /**
   * grow buffer to the required size, loop to read from skt to fill.
   * @param reader, read more bytes from reader to fill the buffer to required
   * size.
   * @param required_size, loop to fill to ensure buffer size to required.
   * @return an int error code, error if required_size negative.
   * @remark, we actually maybe read more than required_size, maybe 4k for
   * example.
   */
  virtual int grow(IBufferReader *reader, int required_size);
  virtual int realloc_buffer(int size);
};

extern int write_large_iovs(IBufferWriter *skt, iovec *iovs, int size,
                            ssize_t *pnwrite);
std::string coco_get_peer_ip(int fd);
int coco_get_peer_port(int fd);
std::string GetRemoteAddr(sockaddr_in &in);