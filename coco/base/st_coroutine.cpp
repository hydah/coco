
#include <assert.h>
#include "coco/base/st_coroutine.hpp"
#include "coco/base/log.hpp"
#include "coco/base/error.hpp"

IStCoroutineHandler::IStCoroutineHandler()
{
}

IStCoroutineHandler::~IStCoroutineHandler()
{
}

void IStCoroutineHandler::on_coroutine_start()
{
}

int IStCoroutineHandler::on_before_cycle()
{
    int ret = ERROR_SUCCESS;
    return ret;
}

int IStCoroutineHandler::on_end_cycle()
{
    int ret = ERROR_SUCCESS;
    return ret;
}

void IStCoroutineHandler::on_coroutine_stop()
{
}

namespace internal {
    StCoroutineContext *_st_context = new StCoroutineContext();
    StCoroutineContext::StCoroutineContext()
    {
    }

    StCoroutineContext::~StCoroutineContext()
    {
    }

    int StCoroutineContext::generate_id()
    {
        static int id = 100;

        int gid = id++;
        cache[st_thread_self()] = gid;
        return gid;
    }

    int StCoroutineContext::get_id()
    {
        return cache[st_thread_self()];
    }

    int StCoroutineContext::set_id(int v)
    {
        st_thread_t self = st_thread_self();

        int ov = 0;
        if (cache.find(self) != cache.end()) {
            ov = cache[self];
        }

        cache[self] = v;

        return ov;
    }

    void StCoroutineContext::clear_cid()
    {
        st_thread_t self = st_thread_self();
        std::map<st_thread_t, int>::iterator it = cache.find(self);
        if (it != cache.end()) {
            cache.erase(it);
        }
    }

    StCoroutine::StCoroutine(const char* name, IStCoroutineHandler* coroutine_handler, int64_t interval_us, bool joinable)
    {
        _name = name;
        handler = coroutine_handler;
        cycle_interval_us = interval_us;

        tid = NULL;
        loop = false;
        really_terminated = true;
        _cid = -1;
        _joinable = joinable;
        disposed = false;

        // in start(), the coroutine cycle method maybe stop and remove the coroutine itself,
        // and the coroutine start() is waiting for the _cid, and segment fault then.
        // @see https://github.com/ossrs/srs/issues/110
        // coroutine will set _cid, callback on_coroutine_start(), then wait for the can_run signal.
        can_run = false;
    }

    StCoroutine::~StCoroutine()
    {
        stop();
    }

    int StCoroutine::cid()
    {
        return _cid;
    }

    int StCoroutine::start()
    {
        int ret = ERROR_SUCCESS;

        if(tid) {
            coco_info("coroutine %s already running.", _name);
            return ret;
        }

        if((tid = st_thread_create(coroutine_fun, this, (_joinable? 1:0), 0)) == NULL){
            ret = ERROR_ST_CREATE_CYCLE_THREAD;
            coco_error("StCoroutine st_coroutine_create failed. ret=%d", ret);
            return ret;
        }
        disposed = false;
        // we set to loop to true for coroutine to run.
        loop = true;

        // wait for cid to ready, for parent coroutine to get the cid.
        int cnt = 0;
        while (_cid < 0) {
            st_usleep(1000);
            coco_trace("start usleep try cnt %d", ++cnt);
        }

        // now, cycle coroutine can run.
        can_run = true;

        coco_trace("StCoroutine start coroutine: [tid]:%p [cid]:%d", tid, _cid);
        return ret;
    }

    void StCoroutine::stop()
    {
        if (!tid) {
            return;
        }

        loop = false;

        dispose();
        coco_trace("StCoroutine stop coroutine: [tid]:%p [cid]:%d", tid, _cid);

        _cid = -1;
        can_run = false;
        tid = NULL;
    }

    bool StCoroutine::can_loop()
    {
        return loop;
    }

    void StCoroutine::stop_loop()
    {
        loop = false;
    }

    void StCoroutine::dispose()
    {
        if (disposed) {
            return;
        }

        // the interrupt will cause the socket to read/write error,
        // which will terminate the cycle coroutine.
        st_thread_interrupt(tid);

        // when joinable, wait util quit.
        if (_joinable) {
            // wait the coroutine to exit.
            int ret = st_thread_join(tid, NULL);
            if (ret) {
                coco_warn("[StCoroutine ANCHOR] core: ignore join coroutine failed.");
            }
        }

        // wait the coroutine actually terminated.
        // sometimes the coroutine join return -1, for example,
        // when coroutine use st_recvfrom, the coroutine join return -1.
        // so here, we use a variable to ensure the coroutine stopped.
        // @remark even the coroutine not joinable, we must ensure the coroutine stopped when stop.
        while (!really_terminated) {
            st_usleep(10 * 1000);

            if (really_terminated) {
                break;
            }
            coco_warn("core: wait coroutine to actually terminated");
        }

        disposed = true;
    }

    int StCoroutine::cycle()
    {
        return handler->cycle();
    }

    void StCoroutine::coroutine_cycle()
    {
        int ret = ERROR_SUCCESS;

        _st_context->generate_id();
        coco_trace("coroutine %s cycle start", _name);

        _cid = _st_context->get_id();

        assert(handler);
        handler->on_coroutine_start();

        // coroutine is running now.
        really_terminated = false;

        // wait for cid to ready, for parent coroutine to get the cid.
        int cnt = 0;
        while (!can_run && loop) {
            st_usleep(1000);
            coco_trace("cycle usleep try cnt %d", ++cnt);
        }

        while (loop) {
            if ((ret = handler->on_before_cycle()) != ERROR_SUCCESS) {
                coco_warn("coroutine %s on before cycle failed, ignored and retry, ret=%d", _name, ret);
                goto failed;
            }

            if ((ret = cycle()) != ERROR_SUCCESS) {
                if (!coco_is_client_gracefully_close(ret)) {
                    coco_warn("coroutine %s cycle failed, ignored and retry, ret=%d", _name, ret);
                }
                goto failed;
            }

            if ((ret = handler->on_end_cycle()) != ERROR_SUCCESS) {
                coco_warn("coroutine %s on end cycle failed, ignored and retry, ret=%d", _name, ret);
                goto failed;
            }

        failed:
            if (!loop) {
                break;
            }

            // to improve performance, donot sleep when interval is zero.
            // @see: https://github.com/ossrs/srs/issues/237
            if (cycle_interval_us != 0) {
                st_usleep(cycle_interval_us);
            }
        }

        // readly terminated now.
        really_terminated = true;

        //srs_trace("coroutine %s cycle finished: before on_coroutine_stop()", _name);
        handler->on_coroutine_stop();
    }

    void* StCoroutine::coroutine_fun(void* arg)
    {
        StCoroutine* obj = static_cast<StCoroutine*>(arg);
        assert(obj);

        obj->coroutine_cycle();

        if (_st_context) {
            _st_context->clear_cid();
        }

        st_thread_exit(NULL);

        return NULL;
    }

    void StCoroutine::set_cycle_interval_us(int64_t interval_us)
    {
        if (interval_us >= 0) {
            cycle_interval_us = interval_us;
        }
    }
}

/* endless coroutine */
StEndlessCoroutine::StEndlessCoroutine(const char* n, IStCoroutineHandler* h):internal::StCoroutine(n, h, 0, false)
{
}

StEndlessCoroutine::~StEndlessCoroutine()
{
}

/* one cycle coroutine */
StOneCycleCoroutine::StOneCycleCoroutine(const char* n, IStCoroutineHandler* h):internal::StCoroutine(n, h, 0, false)
{
}

StOneCycleCoroutine::~StOneCycleCoroutine()
{
}

int StOneCycleCoroutine::cycle()
{
    int ret = handler->cycle();
    stop_loop();
    return ret;
}

/* reuseable coroutine */
StReusableCoroutine::StReusableCoroutine(const char* n, IStCoroutineHandler* h, int64_t i):internal::StCoroutine(n, h, i, true)
{
}

StReusableCoroutine::~StReusableCoroutine()
{
}

/* reuseable coroutine2 */
StReusableCoroutine2::StReusableCoroutine2(const char* n, IStCoroutineHandler* h, int64_t i):internal::StCoroutine(n, h, i, true)
{
}

StReusableCoroutine2::~StReusableCoroutine2()
{
}

void StReusableCoroutine2::interrupt()
{
    stop_loop();
}

bool StReusableCoroutine2::interrupted()
{
    return !can_loop();
}

void StReusableCoroutine2::set_cycle_interval_us(int64_t interval_us)
{
    set_cycle_interval_us(interval_us);
}

int get_st_id() {
    return internal::_st_context->get_id();
}


#ifdef __linux__
#include <sys/epoll.h>

bool st_epoll_is_supported(void)
{
    struct epoll_event ev;

    ev.events = EPOLLIN;
    ev.data.ptr = NULL;
    /* Guaranteed to fail */
    epoll_ctl(-1, EPOLL_CTL_ADD, -1, &ev);

    return (errno != ENOSYS);
}
#endif

int coco_st_init()
{
    int ret = ERROR_SUCCESS;

#ifdef __linux__
    // check epoll, some old linux donot support epoll.
    // @see https://github.com/ossrs/srs/issues/162
    if (!st_epoll_is_supported()) {
        ret = ERROR_ST_SET_EPOLL;
        coco_error("epoll required on Linux. ret=%d", ret);
        return ret;
    }
#endif

    // Select the best event system available on the OS. In Linux this is
    // epoll(). On BSD it will be kqueue.
    if (st_set_eventsys(ST_EVENTSYS_ALT) == -1) {
        ret = ERROR_ST_SET_EPOLL;
        coco_error("st_set_eventsys use %s failed. ret=%d", st_get_eventsys_name(), ret);
        return ret;
    }
    coco_trace("st_set_eventsys to %s", st_get_eventsys_name());

    if(st_init() != 0){
        ret = ERROR_ST_INITIALIZE;
        coco_error("st_init failed. ret=%d", ret);
        return ret;
    }
    coco_trace("st_init success, use %s", st_get_eventsys_name());

    return ret;
}

void coco_close_stfd(st_netfd_t& stfd)
{
    if (stfd) {
        // we must ensure the close is ok.
        assert(st_netfd_close(stfd) != -1);
        stfd = NULL;
    }
}

void coco_uloop(int64_t dur)
{
    while(true) {
        st_usleep(dur);
    }
}

int coco_get_stid()
{
    return internal::_st_context->get_id();
}

