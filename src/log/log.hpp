#pragma once

#include <cstdlib>
#include <stdio.h>
#include <string>

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
std::string bin2hex(const char *data, size_t length,
                    const std::string &split = "");
std::string gen_log_header(int log_level, const char *basefile, int line,
                           const char *func);
#define log_print(lvl, type, fmt, ...)                                         \
  do {                                                                         \
    if ((lvl) >= log_level) {                                                  \
      if ((lvl) != log_dbg || (type) == dbg_user ||                            \
          (debug_mask & (type)) != 0) {                                        \
        std::string h =                                                        \
            gen_log_header((lvl), basefile(__FILE__), __LINE__, __FUNCTION__); \
        fprintf(osf, "%s " fmt "\n", h.c_str(), ##__VA_ARGS__);                \
        fflush(osf);                                                           \
      }                                                                        \
    }                                                                          \
  } while (0)

#define debug_print(type, fmt, ...) log_print(log_dbg, type, fmt, ##__VA_ARGS__)

#define coco_dbg(fmt, ...) log_print(log_dbg, dbg_user, fmt, ##__VA_ARGS__)
#define coco_info(fmt, ...) log_print(log_info, dbg_user, fmt, ##__VA_ARGS__)
#define coco_trace(fmt, ...) log_print(log_trace, dbg_user, fmt, ##__VA_ARGS__)
#define coco_warn(fmt, ...) log_print(log_warn, dbg_user, fmt, ##__VA_ARGS__)
#define coco_error(fmt, ...) log_print(log_error, dbg_user, fmt, ##__VA_ARGS__)