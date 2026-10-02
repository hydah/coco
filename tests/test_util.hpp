#pragma once

#include <stdio.h>

#include <atomic>
#include <functional>
#include <map>
#include <string>

#include "st.h"

namespace cotest {

typedef std::function<void()> TestFn;

std::map<std::string, TestFn> &Registry();
// CHECK may fail on any thread.
std::atomic<int> &Failures();

struct Register {
    Register(const char *name, TestFn fn) { Registry()[name] = fn; }
};

// Yields the current coroutine until pred() holds or timeout_ms elapses.
bool WaitUntil(std::function<bool()> pred, int timeout_ms = 1000);

// Runs fn on a new joinable ST thread; the caller must st_thread_join it.
st_thread_t Go(std::function<void()> fn);

}  // namespace cotest

#define COTEST(name)                                          \
    static void name();                                       \
    static cotest::Register cotest_reg_##name(#name, name);   \
    static void name()

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);  \
            ++cotest::Failures();                                                     \
        }                                                                             \
    } while (0)

#define CHECK_EQ(a, b)                                                                     \
    do {                                                                                   \
        long long va_ = (long long)(a), vb_ = (long long)(b);                              \
        if (va_ != vb_) {                                                                  \
            fprintf(stderr, "%s:%d: CHECK_EQ failed: %s (%lld) != %s (%lld)\n", __FILE__,  \
                    __LINE__, #a, va_, #b, vb_);                                           \
            ++cotest::Failures();                                                          \
        }                                                                                  \
    } while (0)
