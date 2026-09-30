// Lifecycle of CoCoroutine, ConnRoutine, ListenRoutine and ConnManager.

#include <functional>
#include <string>

#include "base/coroutine.hpp"
#include "coco_api.h"
#include "common/error.hpp"
#include "test_util.hpp"

namespace {

struct Probe {
    int destroyed = 0;
    int cycle_returned = 0;
    // Set if a destructor ran before its DoCycle() returned.
    bool freed_while_running = false;
};

class TestConn : public ConnRoutine {
 public:
    typedef std::function<int(TestConn *)> Body;

    TestConn(ConnManager *mgr, Probe *probe, Body body, int dtor_sleep_ms = 0)
        : ConnRoutine(mgr), probe_(probe), body_(body), dtor_sleep_ms_(dtor_sleep_ms) {}

    ~TestConn() override {
        if (!returned_) {
            probe_->freed_while_running = true;
        }
        if (dtor_sleep_ms_ > 0) {
            CocoSleepMs(dtor_sleep_ms_);
        }
        probe_->destroyed++;
    }

    std::string GetRemoteAddr() override { return "test"; }

 protected:
    int DoCycle() override {
        int ret = body_(this);
        returned_ = true;
        probe_->cycle_returned++;
        return ret;
    }

 private:
    Probe *probe_;
    Body body_;
    int dtor_sleep_ms_;
    bool returned_ = false;
};

int BlockUntilStopped(TestConn *c) {
    while (!c->ShouldTermCycle()) {
        CocoSleepMs(1000);
    }
    return ERROR_THREAD_INTERRUPED;
}

class TestListen : public ListenRoutine {
 public:
    typedef std::function<int(TestListen *)> Body;

    explicit TestListen(Body body) : body_(body) {}
    ~TestListen() override { Stop(); }

    int Cycle() override {
        int ret = body_(this);
        exited = true;
        return ret;
    }

    bool exited = false;

 private:
    Body body_;
};

}  // namespace

COTEST(ConnDeletesItselfWhenCycleReturns) {
    ConnManager mgr;
    Probe probe;
    TestConn *c = new TestConn(&mgr, &probe, [](TestConn *) {
        CocoSleepMs(5);
        return COCO_SUCCESS;
    });
    CHECK_EQ(mgr.Size(), 0);
    CHECK_EQ(c->Start(), COCO_SUCCESS);
    CHECK_EQ(mgr.Size(), 1);

    CHECK(cotest::WaitUntil([&] { return probe.destroyed == 1; }));
    CHECK_EQ(mgr.Size(), 0);
    CHECK(!probe.freed_while_running);
}

COTEST(ConnDeletesItselfWhenCycleFails) {
    ConnManager mgr;
    Probe probe;
    TestConn *c = new TestConn(&mgr, &probe, [](TestConn *) { return ERROR_SOCKET_READ; });
    CHECK_EQ(c->Start(), COCO_SUCCESS);

    CHECK(cotest::WaitUntil([&] { return probe.destroyed == 1; }));
    CHECK_EQ(mgr.Size(), 0);
}

// The destructor runs on the finished coroutine's own stack and may yield there.
COTEST(ConnDestructorMayYield) {
    ConnManager mgr;
    Probe probe;
    TestConn *c = new TestConn(
        &mgr, &probe, [](TestConn *) { return COCO_SUCCESS; }, 10);
    CHECK_EQ(c->Start(), COCO_SUCCESS);

    CHECK(cotest::WaitUntil([&] { return probe.cycle_returned == 1; }));
    CHECK_EQ(probe.destroyed, 0);
    CHECK(cotest::WaitUntil([&] { return probe.destroyed == 1; }));
    CHECK_EQ(mgr.Size(), 0);
}

COTEST(ConnStopDoesNotWait) {
    ConnManager mgr;
    Probe probe;
    TestConn *c = new TestConn(&mgr, &probe, BlockUntilStopped);
    CHECK_EQ(c->Start(), COCO_SUCCESS);
    CocoSleepMs(2);

    c->Stop();
    CHECK_EQ(probe.destroyed, 0);

    CHECK(cotest::WaitUntil([&] { return probe.destroyed == 1; }));
    CHECK_EQ(mgr.Size(), 0);
    CHECK(!probe.freed_while_running);
}

// Stop() lands before the coroutine ever ran; its first check must see it.
COTEST(ConnStopBeforeFirstRun) {
    ConnManager mgr;
    Probe probe;
    TestConn *c = new TestConn(&mgr, &probe, BlockUntilStopped);
    CHECK_EQ(c->Start(), COCO_SUCCESS);
    c->Stop();

    CHECK(cotest::WaitUntil([&] { return probe.destroyed == 1; }, 100));
}

COTEST(ConnNeverStartedIsOwnedByCaller) {
    ConnManager mgr;
    Probe probe;
    TestConn *c = new TestConn(&mgr, &probe, BlockUntilStopped);
    CHECK_EQ(mgr.Size(), 0);
    delete c;
    CHECK_EQ(probe.destroyed, 1);
    CHECK_EQ(mgr.Size(), 0);
    mgr.Shutdown();
}

COTEST(ManagerShutdownWaitsForAllConns) {
    ConnManager mgr;
    Probe probe;
    const int n = 3;
    for (int i = 0; i < n; ++i) {
        TestConn *c = new TestConn(&mgr, &probe, BlockUntilStopped, i * 5);
        CHECK_EQ(c->Start(), COCO_SUCCESS);
    }
    CocoSleepMs(2);
    CHECK_EQ(mgr.Size(), n);

    mgr.Shutdown();
    CHECK_EQ(probe.destroyed, n);
    CHECK_EQ(mgr.Size(), 0);
    CHECK(!probe.freed_while_running);

    // The manager stays usable after a shutdown.
    TestConn *c = new TestConn(&mgr, &probe, [](TestConn *) { return COCO_SUCCESS; });
    CHECK_EQ(c->Start(), COCO_SUCCESS);
    CHECK(cotest::WaitUntil([&] { return probe.destroyed == n + 1; }));
}

COTEST(ManagerDestructorShutsDownConns) {
    Probe probe;
    ConnManager *mgr = new ConnManager();
    for (int i = 0; i < 4; ++i) {
        TestConn *c = new TestConn(mgr, &probe, BlockUntilStopped);
        CHECK_EQ(c->Start(), COCO_SUCCESS);
    }
    CocoSleepMs(2);

    delete mgr;
    CHECK_EQ(probe.destroyed, 4);
    CHECK(!probe.freed_while_running);
}

// Shutdown must keep waiting when the coroutine calling it is interrupted.
COTEST(ManagerShutdownSurvivesInterrupt) {
    ConnManager mgr;
    Probe probe;
    // Ignores the first interrupt for a while, so Shutdown has to wait.
    TestConn *c = new TestConn(&mgr, &probe, [](TestConn *self) {
        BlockUntilStopped(self);
        CocoSleepMs(30);
        return COCO_SUCCESS;
    });
    CHECK_EQ(c->Start(), COCO_SUCCESS);
    CocoSleepMs(2);

    bool done_when_returned = false;
    st_thread_t stopper = cotest::Go([&] {
        mgr.Shutdown();
        done_when_returned = probe.destroyed == 1;
    });
    CocoSleepMs(5);
    st_thread_interrupt(stopper);
    st_thread_join(stopper, NULL);

    CHECK(done_when_returned);
}

COTEST(ManyConnsExitConcurrently) {
    ConnManager mgr;
    Probe probe;
    const int n = 500;
    for (int i = 0; i < n; ++i) {
        int ms = i % 7;
        TestConn *c = new TestConn(&mgr, &probe, [ms](TestConn *) {
            CocoSleepMs(ms);
            return COCO_SUCCESS;
        });
        CHECK_EQ(c->Start(), COCO_SUCCESS);
    }

    CHECK(cotest::WaitUntil([&] { return probe.destroyed == n; }, 3000));
    CHECK_EQ(mgr.Size(), 0);
    CHECK(!probe.freed_while_running);
}

COTEST(ListenStopWaitsForCycle) {
    TestListen *l = new TestListen([](TestListen *self) {
        while (!self->ShouldTermCycle()) {
            CocoSleepMs(1000);
        }
        CocoSleepMs(10);
        return COCO_SUCCESS;
    });
    CHECK_EQ(l->Start(), COCO_SUCCESS);
    CocoSleepMs(2);

    l->Stop();
    CHECK(l->exited);
    delete l;
}

COTEST(ListenStopBeforeStart) {
    TestListen *l = new TestListen([](TestListen *) { return COCO_SUCCESS; });
    l->Stop();
    CHECK(!l->exited);
    delete l;
}

// Stop() from inside Cycle() cannot join; the owner's later delete still has to.
COTEST(ListenStopFromItself) {
    TestListen *l = new TestListen([](TestListen *self) {
        self->Stop();
        CHECK(self->ShouldTermCycle());
        // The pending interrupt ends the first sleep at once.
        CocoSleepMs(1);
        CocoSleepMs(10);
        return COCO_SUCCESS;
    });
    CHECK_EQ(l->Start(), COCO_SUCCESS);
    CocoSleepMs(2);
    CHECK(!l->exited);

    delete l;
}

COTEST(ListenStopSurvivesInterrupt) {
    TestListen *l = new TestListen([](TestListen *self) {
        while (!self->ShouldTermCycle()) {
            CocoSleepMs(1000);
        }
        CocoSleepMs(30);
        return COCO_SUCCESS;
    });
    CHECK_EQ(l->Start(), COCO_SUCCESS);
    CocoSleepMs(2);

    bool exited_when_returned = false;
    st_thread_t stopper = cotest::Go([&] {
        l->Stop();
        exited_when_returned = l->exited;
    });
    CocoSleepMs(5);
    st_thread_interrupt(stopper);
    st_thread_join(stopper, NULL);

    CHECK(exited_when_returned);
    delete l;
}
