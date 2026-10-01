#pragma once

#include <stdint.h>
#include <stdio.h>
#include <cstdlib>
#include <string>

namespace coco {

enum eDbgMask {
    dbg_connect = 0x1,
    dbg_accept = 0x1 << 1,
    dbg_write = 0x1 << 2,
    dbg_read = 0x1 << 3,
    dbg_close = 0x1 << 4,
    dbg_st = 0x1 << 5,
    dbg_api = 0x1 << 6,
    dbg_ignore = 0x1 << 7,
    dbg_event = 0x1 << 8,
    dbg_fd = 0x1 << 9,
    dbg_timer = 0x1 << 10,
    dbg_conn_visitor = 0x1 << 11,
    dbg_ack_timeout = 0x1 << 12,
    dbg_error = 0x1 << 13,
    dbg_user = 0x1 << 31,
    dbg_all = 0xffffffff,
};

enum LogLevel {
    log_dbg = 0x1,
    log_trace = 0x2,
    log_info = 0x3,
    log_warn = 0x4,
    log_error = 0x5,
};
extern FILE *osf;
extern uint64_t debug_mask;
extern int log_level;

std::string get_cur_time();
const char *basefile(const char *file);
std::string bin2hex(const char *data, size_t length, const std::string &split = "");
std::string gen_log_header(int log_level, const char *basefile, int line, const char *func);
}  // namespace coco

// The macros are used outside the namespace too, so every name in them is qualified.
#define coco_log_print(lvl, type, fmt, ...)                                                   \
    do {                                                                                      \
        if ((lvl) >= ::coco::log_level) {                                                     \
            if ((lvl) != ::coco::log_dbg || (type) == ::coco::dbg_user ||                     \
                (::coco::debug_mask & (type)) != 0) {                                         \
                std::string h = ::coco::gen_log_header((lvl), ::coco::basefile(__FILE__),     \
                                                       __LINE__, __FUNCTION__);               \
                fprintf(::coco::osf, "%s " fmt "\n", h.c_str(), ##__VA_ARGS__);               \
                fflush(::coco::osf);                                                          \
            }                                                                                 \
        }                                                                                     \
    } while (0)

#define coco_debug_print(type, fmt, ...) coco_log_print(::coco::log_dbg, type, fmt, ##__VA_ARGS__)

#define coco_dbg(fmt, ...) coco_log_print(::coco::log_dbg, ::coco::dbg_user, fmt, ##__VA_ARGS__)
#define coco_info(fmt, ...) coco_log_print(::coco::log_info, ::coco::dbg_user, fmt, ##__VA_ARGS__)
#define coco_trace(fmt, ...) coco_log_print(::coco::log_trace, ::coco::dbg_user, fmt, ##__VA_ARGS__)
#define coco_warn(fmt, ...) coco_log_print(::coco::log_warn, ::coco::dbg_user, fmt, ##__VA_ARGS__)
#define coco_error(fmt, ...) coco_log_print(::coco::log_error, ::coco::dbg_user, fmt, ##__VA_ARGS__)
