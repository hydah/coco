#pragma once

#include <stdint.h>

#include <atomic>
#include <chrono>
#include <string>

// A short name per thread for the output of these examples: T0 is the first thread to
// ask, the main one, then T1, T2...
inline const char *ThreadName() {
    static std::atomic<int> next(0);
    static thread_local std::string name = "T" + std::to_string(next++);
    return name.c_str();
}

inline int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
