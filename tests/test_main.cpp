#include <string>

#include "coco_api.h"
#include "log/log.hpp"
#include "test_util.hpp"

namespace cotest {

std::map<std::string, TestFn> &Registry() {
    static std::map<std::string, TestFn> cases;
    return cases;
}

int &Failures() {
    static int failures = 0;
    return failures;
}

bool WaitUntil(std::function<bool()> pred, int timeout_ms) {
    for (int waited = 0; waited < timeout_ms; ++waited) {
        if (pred()) {
            return true;
        }
        CocoSleepMs(1);
    }
    return pred();
}

static void *GoEntry(void *arg) {
    TestFn *fn = (TestFn *)arg;
    (*fn)();
    delete fn;
    return NULL;
}

st_thread_t Go(std::function<void()> fn) { return st_thread_create(GoEntry, new TestFn(fn), 1, 0); }

}  // namespace cotest

// Usage: coco_tests [case]. Without a case every registered case runs in this process;
// ctest runs each case in its own process so a crash or hang is attributed to one case.
int main(int argc, char **argv) {
    log_level = log_warn;
    if (CocoInit() != 0) {
        fprintf(stderr, "CocoInit failed\n");
        return 2;
    }

    auto &cases = cotest::Registry();
    if (argc > 1) {
        auto it = cases.find(argv[1]);
        if (it == cases.end()) {
            fprintf(stderr, "unknown case: %s\n", argv[1]);
            return 2;
        }
        it->second();
    } else {
        for (auto &c : cases) {
            fprintf(stderr, "[ RUN  ] %s\n", c.first.c_str());
            int before = cotest::Failures();
            c.second();
            fprintf(stderr, "[ %s ] %s\n", cotest::Failures() == before ? " OK " : "FAIL",
                    c.first.c_str());
        }
    }
    return cotest::Failures() == 0 ? 0 : 1;
}
