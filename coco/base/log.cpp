#include <unistd.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/time.h>
#include <string.h>
#include "coco/base/log.hpp"

FILE* osf = stdout;
uint64_t debug_mask = 0;
int log_level = log_trace;

std::string get_cur_time()
{
    bool utc = false;
    struct timeval tv;
    struct timezone tz;
    gettimeofday(&tv, &tz);

    struct tm local;
    if (utc) {
        if (gmtime_r(&tv.tv_sec, &local) == NULL) {
            return std::string();
        }
    } else {
        if (localtime_r(&tv.tv_sec, &local) == NULL) {
            return std::string();
        }
    }
    char buffer[128];
    snprintf(buffer, sizeof(buffer), "%d-%02d-%02d %02d:%02d:%02d.%06lu",
             1900 + local.tm_year, 1 + local.tm_mon, local.tm_mday,
             local.tm_hour, local.tm_min, local.tm_sec, tv.tv_usec);
    return std::string(buffer);
}

const char* basefile(const char* file)
{
    const char* p = strrchr(file, '/');
    if (p) return p + 1;

    p = strrchr(file, '\\');
    if (p) return p + 1;

    return file;
}

std::string bin2hex(const char* data, size_t length, const std::string& split)
{
    if (!data || !length) return "";

    static const char *hex = "0123456789abcdefg";

    char buf[2] = {};
    std::string s;
    s.reserve(length * 2 + split.size() * (length - 1));
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = data[i];
        buf[0] = hex[c >> 4];
        buf[1] = hex[c & 0xf];
        s.append(buf, 2);

        if (i + 1 < length)
            s.append(split);
    }
    return s;
}

std::string gen_log_header(int lvl, const char* basefile, int line, const char* func)
{
    char buffer[128];
    snprintf(buffer, sizeof(buffer), "[%s][%s:%s:%d][%d][%d]",
             get_cur_time().c_str(),
             basefile,
             func,
             line,
             getpid(),
             coco_get_stid());

    return std::string(buffer);
}