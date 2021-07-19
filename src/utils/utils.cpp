#include "utils/utils.hpp"

#include <arpa/inet.h>
#include <assert.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <cstdlib>

#include <map>

#include "common/error.hpp"
#include "log/log.hpp"

bool is_ipv6(std::string ip) { return false; }

static std::map<std::string, bool> _device_ifs;
bool net_device_is_internet(in_addr_t addr) {
    u_int32_t addr_h = ntohl(addr);

    // lo, 127.0.0.0-127.0.0.1
    if (addr_h >= 0x7f000000 && addr_h <= 0x7f000001) {
        return false;
    }

    // Class A 10.0.0.0-10.255.255.255
    if (addr_h >= 0x0a000000 && addr_h <= 0x0affffff) {
        return false;
    }

    // Class B 172.16.0.0-172.31.255.255
    if (addr_h >= 0xac100000 && addr_h <= 0xac1fffff) {
        return false;
    }

    // Class C 192.168.0.0-192.168.255.255
    if (addr_h >= 0xc0a80000 && addr_h <= 0xc0a8ffff) {
        return false;
    }

    return true;
}

#define CONSTS_LOCALHOST "127.0.0.1"
std::vector<std::string> _system_ipv4_ips;
void retrieve_local_ipv4_ips() {
    std::vector<std::string> &ips = _system_ipv4_ips;

    ips.clear();

    ifaddrs *ifap;
    if (getifaddrs(&ifap) == -1) {
        coco_warn("retrieve local ips, ini ifaddrs failed.");
        return;
    }

    ifaddrs *p = ifap;
    while (p != NULL) {
        ifaddrs *cur = p;
        sockaddr *addr = cur->ifa_addr;
        p = p->ifa_next;

        // retrieve ipv4 addr
        // ignore the tun0 network device,
        // which addr is NULL.
        // @see: https://github.com/ossrs/srs/issues/141
        if (addr && addr->sa_family == AF_INET) {
            in_addr *inaddr = &((sockaddr_in *)addr)->sin_addr;

            char buf[16];
            memset(buf, 0, sizeof(buf));

            if ((inet_ntop(addr->sa_family, inaddr, buf, sizeof(buf))) == NULL) {
                coco_warn("convert local ip failed");
                break;
            }

            std::string ip = buf;
            if (ip != CONSTS_LOCALHOST) {
                coco_trace("retrieve local ipv4 ip=%s, index=%d", ip.c_str(), (int)ips.size());
                ips.push_back(ip);
            }

            // set the device internet status.
            if (!net_device_is_internet(inaddr->s_addr)) {
                coco_trace("detect intranet address: %s, ifname=%s", ip.c_str(), cur->ifa_name);
                _device_ifs[cur->ifa_name] = false;
            } else {
                coco_trace("detect internet address: %s, ifname=%s", ip.c_str(), cur->ifa_name);
                _device_ifs[cur->ifa_name] = true;
            }
        }
    }

    freeifaddrs(ifap);
}

std::vector<std::string> &get_local_ipv4_ips() {
    if (_system_ipv4_ips.empty()) {
        retrieve_local_ipv4_ips();
    }

    return _system_ipv4_ips;
}

std::string _public_internet_address;
std::string get_public_internet_address() {
    if (!_public_internet_address.empty()) {
        return _public_internet_address;
    }

    std::vector<std::string> &ips = get_local_ipv4_ips();

    // find the best match public address.
    for (int i = 0; i < (int)ips.size(); i++) {
        std::string ip = ips[i];
        in_addr_t addr = inet_addr(ip.c_str());
        u_int32_t addr_h = ntohl(addr);
        // lo, 127.0.0.0-127.0.0.1
        if (addr_h >= 0x7f000000 && addr_h <= 0x7f000001) {
            coco_trace("ignore private address: %s", ip.c_str());
            continue;
        }
        // Class A 10.0.0.0-10.255.255.255
        if (addr_h >= 0x0a000000 && addr_h <= 0x0affffff) {
            coco_trace("ignore private address: %s", ip.c_str());
            continue;
        }
        // Class B 172.16.0.0-172.31.255.255
        if (addr_h >= 0xac100000 && addr_h <= 0xac1fffff) {
            coco_trace("ignore private address: %s", ip.c_str());
            continue;
        }
        // Class C 192.168.0.0-192.168.255.255
        if (addr_h >= 0xc0a80000 && addr_h <= 0xc0a8ffff) {
            coco_trace("ignore private address: %s", ip.c_str());
            continue;
        }
        coco_warn("use public address as ip: %s", ip.c_str());

        _public_internet_address = ip;
        return ip;
    }

    // no public address, use private address.
    for (int i = 0; i < (int)ips.size(); i++) {
        std::string ip = ips[i];
        in_addr_t addr = inet_addr(ip.c_str());
        u_int32_t addr_h = ntohl(addr);
        // lo, 127.0.0.0-127.0.0.1
        if (addr_h >= 0x7f000000 && addr_h <= 0x7f000001) {
            coco_trace("ignore private address: %s", ip.c_str());
            continue;
        }
        coco_warn("use private address as ip: %s", ip.c_str());

        _public_internet_address = ip;
        return ip;
    }

    return "";
}

// the default recv buffer size, 128KB.
#define DEFAULT_RECV_BUFFER_SIZE (128 * 1024)
// limit user-space buffer to 256KB, for 3Mbps stream delivery.
//      800*2000/8=200000B(about 195KB).
// @remark it's ok for higher stream, the buffer is ok for one chunk is 256KB.
#define MAX_SOCKET_BUFFER (10 * 1024 * 1024)

FastBuffer::FastBuffer() {
    nb_buffer = DEFAULT_RECV_BUFFER_SIZE;
    buffer = (char *)malloc(nb_buffer);
    p = end = buffer;
}

FastBuffer::~FastBuffer() {
    free(buffer);
    buffer = NULL;
}

int FastBuffer::size() { return (int)(end - p); }

char *FastBuffer::bytes() { return p; }

int FastBuffer::update(char *data, int required_size) {
    int ret = COCO_SUCCESS;

    // must be positive.
    assert(required_size > 0);

    // the free space of buffer,
    //      buffer = consumed_bytes + exists_bytes + free_space.
    int nb_free_space = (int)(buffer + nb_buffer - end);

    // the bytes already in buffer
    int nb_exists_bytes = (int)(end - p);
    assert(nb_exists_bytes >= 0);

    // resize the space when no left space.
    if (nb_free_space < required_size) {
        coco_dbg("move fast buffer %d bytes", nb_exists_bytes);

        // reset or move to get more space.
        if (!nb_exists_bytes) {
            // reset when buffer is empty.
            p = end = buffer;
            coco_dbg("all consumed, reset fast buffer");
        } else if (nb_exists_bytes < nb_buffer && p > buffer) {
            // move the left bytes to start of buffer.
            // @remark Only move memory when space is enough, or failed at next check.
            // @see https://github.com/ossrs/srs/issues/848
            buffer = (char *)memmove(buffer, p, nb_exists_bytes);
            p = buffer;
            end = p + nb_exists_bytes;
        }

        // check whether enough free space in buffer.
        nb_free_space = (int)(buffer + nb_buffer - end);
        if (nb_free_space < required_size) {
            ret = ERROR_READER_BUFFER_OVERFLOW;
            coco_error("buffer overflow, required=%d, max=%d, left=%d, ret=%d", required_size,
                       nb_buffer, nb_free_space, ret);
            return ret;
        }
    }

    memcpy(end, data, required_size);
    end += required_size;
    nb_free_space -= required_size;

    return ret;
}

void FastBuffer::set_buffer(int buffer_size) {
    // never exceed the max size.
    if (buffer_size > MAX_SOCKET_BUFFER) {
        coco_warn("limit the user-space buffer from %d to %d", buffer_size, MAX_SOCKET_BUFFER);
    }

    // the user-space buffer size limit to a max value.
    int nb_resize_buf = coco_min(buffer_size, MAX_SOCKET_BUFFER);

    // only realloc when buffer changed bigger
    if (nb_resize_buf <= nb_buffer) {
        return;
    }

    // realloc for buffer change bigger.
    int start = (int)(p - buffer);
    int nb_bytes = (int)(end - p);

    buffer = (char *)realloc(buffer, nb_resize_buf);
    nb_buffer = nb_resize_buf;
    p = buffer + start;
    end = p + nb_bytes;
}

char FastBuffer::read_1byte() {
    assert(end - p >= 1);
    return *p++;
}

char *FastBuffer::read_slice(int _size) {
    assert(_size >= 0);
    assert(end - p >= _size);
    assert(p + _size >= buffer);

    char *ptr = p;
    p += _size;

    return ptr;
}

void FastBuffer::skip(int _size) {
    assert(end - p >= _size);
    assert(p + _size >= buffer);
    p += _size;
}

int FastBuffer::grow(IoReader *reader, int required_size) {
    int ret = COCO_SUCCESS;

    // already got required size of bytes.
    if (end - p >= required_size) {
        return ret;
    }

    // must be positive.
    assert(required_size > 0);

    // the free space of buffer,
    //      buffer = consumed_bytes + exists_bytes + free_space.
    int nb_free_space = (int)(buffer + nb_buffer - end);

    // the bytes already in buffer
    int nb_exists_bytes = (int)(end - p);
    assert(nb_exists_bytes >= 0);

    // resize the space when no left space.
    if (nb_free_space < required_size - nb_exists_bytes) {
        coco_dbg("move fast buffer %d bytes", nb_exists_bytes);

        // reset or move to get more space.
        if (!nb_exists_bytes) {
            // reset when buffer is empty.
            p = end = buffer;
            coco_dbg("all consumed, reset fast buffer");
        } else if (nb_exists_bytes < nb_buffer && p > buffer) {
            // move the left bytes to start of buffer.
            // @remark Only move memory when space is enough, or failed at next check.
            // @see https://github.com/ossrs/srs/issues/848
            buffer = (char *)memmove(buffer, p, nb_exists_bytes);
            p = buffer;
            end = p + nb_exists_bytes;
        }

        // check whether enough free space in buffer.
        nb_free_space = (int)(buffer + nb_buffer - end);
        if (nb_free_space < required_size - nb_exists_bytes) {
            ret = realloc_buffer(required_size);
            if (ret != COCO_SUCCESS) {
                return ret;
            }
            nb_free_space = (int)(buffer + nb_buffer - end);
        }
    }

    // buffer is ok, read required size of bytes.
    while (end - p < required_size) {
        ssize_t nread;
        if ((ret = reader->Read(end, nb_free_space, &nread)) != COCO_SUCCESS) {
            return ret;
        }
        // we just move the ptr to next.
        assert((int)nread > 0);
        end += nread;
        nb_free_space -= (int)nread;
    }

    return ret;
}

int FastBuffer::realloc_buffer(int _size) {
    int ret = COCO_SUCCESS;
    int before_free_space = (int)(buffer + nb_buffer - end);
    int before_buffer_size = nb_buffer;
    int nb_exists_bytes = (int)(end - p);
    int nb_already_read_bytes = (int)(p - buffer);
    int realloc_size = nb_already_read_bytes + _size;
    int n = realloc_size / DEFAULT_RECV_BUFFER_SIZE + 1;
    realloc_size = n * DEFAULT_RECV_BUFFER_SIZE;
    realloc_size = coco_max(realloc_size, nb_buffer) + 2 * DEFAULT_RECV_BUFFER_SIZE;
    if (realloc_size > MAX_SOCKET_BUFFER) {
        ret = ERROR_READER_BUFFER_OVERFLOW;
        coco_error(
            "buffer overflow, required=%d, buffer size=%d, left=%d, realloc "
            "size=%d, max realloc size=%d",
            _size, nb_buffer, (int)(buffer + nb_buffer - end), realloc_size, MAX_SOCKET_BUFFER);
        return ret;
    }
    buffer = (char *)realloc(buffer, realloc_size);
    assert(buffer != NULL);
    p = buffer + nb_already_read_bytes;
    end = p + nb_exists_bytes;
    nb_buffer = realloc_size;
    int nb_free_space = (int)(buffer + nb_buffer - end);
    coco_warn(
        "buffer realloc, required=%d, before: max=%d, left=%d, now: "
        "max=%d, left=%d",
        _size, before_buffer_size, before_free_space, nb_buffer, nb_free_space);
    return COCO_SUCCESS;
}

int write_large_iovs(IoWriter *skt, iovec *iovs, int size, ssize_t *pnwrite) {
    int ret = COCO_SUCCESS;
    static int limits = 1024;

    // send in a time.
    if (size < limits) {
        if ((ret = skt->Writev(iovs, size, pnwrite)) != COCO_SUCCESS) {
            if (!coco_is_client_gracefully_close(ret)) {
                coco_error("send with writev failed. ret=%d", ret);
            }
            return ret;
        }
        return ret;
    }

    // send in multiple times.
    int cur_iov = 0;
    while (cur_iov < size) {
        int cur_count = coco_min(limits, size - cur_iov);
        if ((ret = skt->Writev(iovs + cur_iov, cur_count, pnwrite)) != COCO_SUCCESS) {
            if (!coco_is_client_gracefully_close(ret)) {
                coco_error("send with writev failed. ret=%d", ret);
            }
            return ret;
        }
        cur_iov += cur_count;
    }

    return ret;
}

std::string coco_get_peer_ip(int fd) {
    // discovery client information
    sockaddr_storage addr;
    socklen_t addrlen = sizeof(addr);
    if (getpeername(fd, (sockaddr *)&addr, &addrlen) == -1) {
        return "";
    }

    char saddr[64];
    char *h = (char *)saddr;
    socklen_t nbh = (socklen_t)sizeof(saddr);
    const int r0 = getnameinfo((const sockaddr *)&addr, addrlen, h, nbh, NULL, 0, NI_NUMERICHOST);
    if (r0) {
        return "";
    }

    return std::string(saddr);
}

int coco_get_peer_port(int fd) {
    // discovery client information
    sockaddr_storage addr;
    socklen_t addrlen = sizeof(addr);
    if (getpeername(fd, (sockaddr *)&addr, &addrlen) == -1) {
        return 0;
    }

    int port = 0;
    switch (addr.ss_family) {
        case AF_INET:
            port = ntohs(((sockaddr_in *)&addr)->sin_port);
            break;
        case AF_INET6:
            port = ntohs(((sockaddr_in6 *)&addr)->sin6_port);
            break;
    }

    return port;
}

std::string GetRemoteAddr(int fd) {
    sockaddr_storage addr;
    socklen_t addrlen = sizeof(addr);
    if (getpeername(fd, (sockaddr *)&addr, &addrlen) == -1) {
        return "";
    }

    return GetRemoteAddr(*((sockaddr_in *)&addr));
}

std::string GetRemoteAddr(sockaddr_in &in) {
    static char _ip_convert_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(in.sin_addr), _ip_convert_str, INET_ADDRSTRLEN);
    auto len = strlen(_ip_convert_str);
    _ip_convert_str[len] = ':';
    sprintf(_ip_convert_str + len + 1, "%d", ntohs(in.sin_port));

    return _ip_convert_str;
}