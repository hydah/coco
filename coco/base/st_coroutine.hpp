#ifndef ST_COROUTINE_HPP
#define ST_COROUTINE_HPP

#include <map>
#include "st/obj/st.h"
// @see StEndlessCoroutine, StOneCycleCoroutine, StReusableCoroutine
/**
 * the handler for the coroutine, callback interface.
 * the coroutine model defines as:
 *     handler->on_coroutine_start()
 *     while loop:
 *        handler->on_before_cycle()
 *        handler->cycle()
 *        handler->on_end_cycle()
 *        if !loop then break for user stop coroutine.
 *        sleep(CycleIntervalMilliseconds)
 *     handler->on_coroutine_stop()
 * when stop, the coroutine will interrupt the st_coroutine,
 * which will cause the socket to return error and
 * terminate the cycle coroutine.
 *
 * @remark why should check can_loop() in cycle method?
 *       when coroutine interrupt, the socket maybe not got EINT,
 *       espectially on st_usleep(), so the cycle must check the loop,
 *       when handler->cycle() has loop itself, for example:
 *               while (true):
 *                   if (read_from_socket(skt) < 0) break;
 *       if coroutine stop when read_from_socket, it's ok, the loop will break,
 *       but when coroutine stop interrupt the s_usleep(0), then the loop is
 *       death loop.
 *       in a word, the handler->cycle() must:
 *               while (coroutine->can_loop()):
 *                   if (read_from_socket(skt) < 0) break;
 *       check the loop, then it works.
 *
 * @remark why should use stop_loop() to terminate coroutine in itself?
 *       in the coroutine itself, that is the cycle method,
 *       if itself want to terminate the coroutine, should never use stop(),
 *       but use stop_loop() to set the loop to false and terminate normally.
 *
 * @remark when should set the interval_us, and when not?
 *       the cycle will invoke util cannot loop, eventhough the return code of cycle is error,
 *       so the interval_us used to sleep for each cycle.
 */
class IStCoroutineHandler
{
public:
    IStCoroutineHandler();
    virtual ~IStCoroutineHandler();
public:
    virtual void on_coroutine_start();
    virtual int on_before_cycle();
    virtual int cycle() = 0;
    virtual int on_end_cycle();
    virtual void on_coroutine_stop();
};

// the internal classes, user should never use it.
// user should use the public classes at the bellow:
namespace internal {
    class StCoroutineContext
    {
    private:
        std::map<st_thread_t, int> cache;
    public:
        StCoroutineContext();
        virtual ~StCoroutineContext();
    public:
        virtual int generate_id();
        virtual int get_id();
        virtual int set_id(int v);
    public:
        virtual void clear_cid();
    };
    /**
     * provides servies from st_coroutine_t,
     * for common coroutine usage.
     */
    class StCoroutine
    {
    private:
        st_thread_t tid;
        int _cid;
        bool loop;
        bool can_run;
        bool really_terminated;
        bool _joinable;
        const char* _name;
        bool disposed;
    public:
        IStCoroutineHandler* handler;
        int64_t cycle_interval_us;
    public:
        /**
         * initialize the coroutine.
         * @param name, human readable name for st debug.
         * @param coroutine_handler, the cycle handler for the coroutine.
         * @param interval_us, the sleep interval when cycle finished.
         * @param joinable, if joinable, other coroutine must stop the coroutine.
         * @remark if joinable, coroutine never quit itself, or memory leak.
         * @see: https://github.com/ossrs/srs/issues/78
         * @remark about st debug, see st-1.9/README, _st_iterate_coroutines_flag
         */
        /**
         * TODO: FIXME: maybe all coroutine must be reap by others coroutines,
         * @see: https://github.com/ossrs/srs/issues/77
         */
        StCoroutine(const char* name, IStCoroutineHandler* coroutine_handler, int64_t interval_us, bool joinable);
        virtual ~StCoroutine();
    public:
        /**
         * get the context id. @see: IStCoroutineContext.get_id().
         * used for parent coroutine to get the id.
         * @remark when start coroutine, parent coroutine will block and wait for this id ready.
         */
        virtual int cid();
        /**
         * start the coroutine, invoke the cycle of handler util
         * user stop the coroutine.
         * @remark ignore any error of cycle of handler.
         * @remark user can start multiple times, ignore if already started.
         * @remark wait for the cid is set by coroutine pfn.
         */
        virtual int start();
        /**
         * stop the coroutine, wait for the coroutine to terminate.
         * @remark user can stop multiple times, ignore if already stopped.
         */
        virtual void stop();
    public:
        /**
         * whether the coroutine should loop,
         * used for handler->cycle() which has a loop method,
         * to check this method, break if false.
         */
        virtual bool can_loop();
        /**
         * for the loop coroutine to stop the loop.
         * other coroutine can directly use stop() to stop loop and wait for quit.
         * this stop loop method only set loop to false.
         */
        virtual void stop_loop();
        virtual int cycle();
        virtual void set_cycle_interval_us(int64_t interval_us);
    private:
        virtual void dispose();
        virtual void coroutine_cycle();
        static void* coroutine_fun(void* arg);
    };
}


class StEndlessCoroutine : public internal::StCoroutine
{
public:
    StEndlessCoroutine(const char* n, IStCoroutineHandler* h);
    virtual ~StEndlessCoroutine();
};

class StOneCycleCoroutine : public internal::StCoroutine
{
public:
    StOneCycleCoroutine(const char* n, IStCoroutineHandler* h);
    virtual ~StOneCycleCoroutine();

public:
    virtual int cycle();
};

class StReusableCoroutine : public internal::StCoroutine
{
public:
    StReusableCoroutine(const char* n, IStCoroutineHandler* h, int64_t interval_us = 0);
    virtual ~StReusableCoroutine();
};

class StReusableCoroutine2 : public internal::StCoroutine
{
public:
    StReusableCoroutine2(const char* n, IStCoroutineHandler* h, int64_t interval_us = 0);
    virtual ~StReusableCoroutine2();

public:
    /**
     * interrupt the coroutine to stop loop.
     * we only set the loop flag to false, not really interrupt the coroutine.
     */
    virtual void interrupt();
    /**
     * whether the coroutine is interrupted,
     * for the cycle has its loop, the inner loop should quit when coroutine
     * is interrupted.
     */
    virtual bool interrupted();
public:
    virtual void set_cycle_interval_us(int64_t interval_us);
};

extern int get_stid();

// initialize st, requires epoll.
extern int st_init();

// close the netfd, and close the underlayer fd.
// @remark when close, user must ensure io completed.
extern void close_stfd(st_netfd_t& stfd);
extern void st_us_loop(int64_t dur);

#endif