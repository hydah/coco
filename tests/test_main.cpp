#include <string>

#include "st.h"

#include "coco/coco_api.h"
#include "coco/log/log.hpp"
#include "test_util.hpp"

using namespace coco;

namespace cotest {

std::map<std::string, TestFn> &Registry() {
    static std::map<std::string, TestFn> cases;
    return cases;
}

std::atomic<int> &Failures() {
    static std::atomic<int> failures(0);
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
            // Helpers run in a child process of the case that needs one, never by themselves.
            if (c.first.compare(0, 6, "Helper") == 0) {
                continue;
            }
            fprintf(stderr, "[ RUN  ] %s\n", c.first.c_str());
            int before = cotest::Failures();
            c.second();
            fprintf(stderr, "[ %s ] %s\n", cotest::Failures() == before ? " OK " : "FAIL",
                    c.first.c_str());
        }
    }
    return cotest::Failures() == 0 ? 0 : 1;
}
