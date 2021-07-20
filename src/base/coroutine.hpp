#pragma once

#include <map>

#include "st.h"

#include "base/coroutine_mgr.hpp"
#include "common/error.hpp"

const int32_t kInvalidContextId = -1;
class CoroutineHandler;
class CoCoroutine;

class CoroutineHandler {
 public:
    CoroutineHandler() = default;
    virtual ~CoroutineHandler() = default;

    virtual int Cycle() = 0;
    int GetCoroutineState();
    bool ShouldTermCycle() { return GetCoroutineState() != COCO_SUCCESS; };

 protected:
    CoCoroutine *coroutine;
};

class CoroutineContext {
 public:
    CoroutineContext() = default;
    virtual ~CoroutineContext(){};

    virtual int generate_id();
    virtual int get_id();
    virtual int set_id(int v);
    virtual void clear_cid();

 private:
    std::map<st_thread_t, int> cache_;
};

class CoCoroutine {
 public:
    CoCoroutine(std::string n, CoroutineHandler *h);
    CoCoroutine(std::string n, CoroutineHandler *h, int32_t cid);
    ~CoCoroutine();

    void set_stack_size(int v);
    int32_t start();
    void stop();
    void interrupt();
    // 在handler cycle中，如果发现 coroutine err了，要退出cycle
    inline int32_t pull() { return trd_err_; }
    int32_t get_cid();

 private:
    int32_t cycle();
    static void *coroutine_fun(void *arg);

    std::string name;
    int stack_size;
    CoroutineHandler *handler;
    st_thread_t trd_;
    int trd_err_ = COCO_SUCCESS;
    int32_t cid_ = kInvalidContextId;

    bool started;
    bool interrupted;
    bool disposed;
    bool cycle_done;
};

class ListenRoutine : public CoroutineHandler {
 public:
    ListenRoutine();
    virtual ~ListenRoutine();

    virtual int Cycle() = 0;
    virtual int Start();
};

class ConnRoutine : public CoroutineHandler {
 public:
    ConnRoutine(ConnManager *manager);
    virtual ~ConnRoutine();

    // virtual void dispose();
    virtual int Start();
    virtual int Cycle();
    virtual void Stop();
    virtual std::string GetRemoteAddr() = 0;

 protected:
    virtual int DoCycle() = 0;
    ConnManager *manager_;

 private:
    int id;
};