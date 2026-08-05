#ifndef DLSM_GT_ADAPTER_H
#define DLSM_GT_ADAPTER_H

#include "dlsm/greenthread.h"
#include "dlsm/sync.h"

/*
 * Header-only host adapter. These macros deliberately add no wrapper ABI and
 * no adapter library. A host may override any individual mapping before this
 * header is included. Link the CMake aggregate target dlsm::gt to obtain both
 * dlsm::greenthread and dlsm::sync.
 *
 * Native-thread boundary: this adapter intentionally does not map or replace
 * pthread_create, pthread_join, or pthread TLS. A host must keep a real
 * NATIVE_THREAD for signal ownership/sigwait, watchdogs, pthread-identity or
 * pthread-TLS-dependent code, and file I/O that must remain on a dedicated OS
 * thread. Such a thread is outside GT context: dlsm_gt_self() is NULL, so it
 * must not call operations documented as GT-only (for example yield, park,
 * GT-local set, or GT mutex lock). APIs explicitly supporting an external
 * pthread, such as task submission and task wait, remain available.
 * Potentially blocking work that does not require persistent pthread identity
 * may instead use DLSM_GT_ADAPTER_BLOCKING_CALL from a GT.
 * See dlsm/greenthread.h for the complete fork, signal, profiler, unwinder,
 * and sanitizer compatibility contract.
 */

#ifndef DLSM_GT_ADAPTER_RUNTIME
#define DLSM_GT_ADAPTER_RUNTIME dlsm_gt_runtime
#endif
#ifndef DLSM_GT_ADAPTER_TASK
#define DLSM_GT_ADAPTER_TASK dlsm_gt_task
#endif
#ifndef DLSM_GT_ADAPTER_TASK_OPTIONS
#define DLSM_GT_ADAPTER_TASK_OPTIONS dlsm_gt_task_options
#endif
#ifndef DLSM_GT_ADAPTER_TASK_OPTIONS_INIT
#define DLSM_GT_ADAPTER_TASK_OPTIONS_INIT DLSM_GT_TASK_OPTIONS_INIT
#endif
#ifndef DLSM_GT_ADAPTER_KEY
#define DLSM_GT_ADAPTER_KEY dlsm_gt_key
#endif
#ifndef DLSM_GT_ADAPTER_MUTEX
#define DLSM_GT_ADAPTER_MUTEX dlsm_gt_mutex
#endif
#ifndef DLSM_GT_ADAPTER_CONDITION
#define DLSM_GT_ADAPTER_CONDITION dlsm_gt_condition
#endif
#ifndef DLSM_GT_ADAPTER_TICKER
#define DLSM_GT_ADAPTER_TICKER dlsm_gt_ticker
#endif

#ifndef DLSM_GT_ADAPTER_RUNTIME_NEW
#define DLSM_GT_ADAPTER_RUNTIME_NEW(nvp, stack_bytes) \
    dlsm_gt_runtime_new((nvp), (stack_bytes))
#endif
#ifndef DLSM_GT_ADAPTER_RUNTIME_START
#define DLSM_GT_ADAPTER_RUNTIME_START(runtime) dlsm_gt_start((runtime))
#endif
#ifndef DLSM_GT_ADAPTER_RUNTIME_STOP
#define DLSM_GT_ADAPTER_RUNTIME_STOP(runtime) dlsm_gt_stop((runtime))
#endif
#ifndef DLSM_GT_ADAPTER_RUNTIME_WAIT
#define DLSM_GT_ADAPTER_RUNTIME_WAIT(runtime) dlsm_gt_wait((runtime))
#endif
#ifndef DLSM_GT_ADAPTER_RUNTIME_RUN
#define DLSM_GT_ADAPTER_RUNTIME_RUN(runtime) dlsm_gt_run((runtime))
#endif
#ifndef DLSM_GT_ADAPTER_RUNTIME_FREE
#define DLSM_GT_ADAPTER_RUNTIME_FREE(runtime) dlsm_gt_runtime_free((runtime))
#endif

#ifndef DLSM_GT_ADAPTER_TASK_SPAWN
#define DLSM_GT_ADAPTER_TASK_SPAWN(runtime, entry, arg) \
    dlsm_gt_spawn((runtime), (entry), (arg))
#endif
#ifndef DLSM_GT_ADAPTER_TASK_SPAWN_EX
#define DLSM_GT_ADAPTER_TASK_SPAWN_EX(runtime, entry, arg, options) \
    dlsm_gt_spawn_ex((runtime), (entry), (arg), (options))
#endif
#ifndef DLSM_GT_ADAPTER_TASK_SPAWN_DETACHED
#define DLSM_GT_ADAPTER_TASK_SPAWN_DETACHED(runtime, entry, arg, options) \
    dlsm_gt_spawn_detached((runtime), (entry), (arg), (options))
#endif
#ifndef DLSM_GT_ADAPTER_TASK_WAIT
#define DLSM_GT_ADAPTER_TASK_WAIT(task) dlsm_gt_task_wait((task))
#endif
#ifndef DLSM_GT_ADAPTER_TASK_CANCEL
#define DLSM_GT_ADAPTER_TASK_CANCEL(task) dlsm_gt_task_cancel((task))
#endif
#ifndef DLSM_GT_ADAPTER_TASK_RETAIN
#define DLSM_GT_ADAPTER_TASK_RETAIN(task) dlsm_gt_task_retain((task))
#endif
#ifndef DLSM_GT_ADAPTER_TASK_RELEASE
#define DLSM_GT_ADAPTER_TASK_RELEASE(task) dlsm_gt_task_release((task))
#endif
#ifndef DLSM_GT_ADAPTER_TASK_YIELD
#define DLSM_GT_ADAPTER_TASK_YIELD() dlsm_gt_yield()
#endif
#ifndef DLSM_GT_ADAPTER_TASK_POLL
#define DLSM_GT_ADAPTER_TASK_POLL() dlsm_gt_poll()
#endif

#ifndef DLSM_GT_ADAPTER_KEY_CREATE
#define DLSM_GT_ADAPTER_KEY_CREATE(key, destructor) \
    dlsm_gt_key_create((key), (destructor))
#endif
#ifndef DLSM_GT_ADAPTER_KEY_DELETE
#define DLSM_GT_ADAPTER_KEY_DELETE(key) dlsm_gt_key_delete((key))
#endif
#ifndef DLSM_GT_ADAPTER_LOCAL_SET
#define DLSM_GT_ADAPTER_LOCAL_SET(key, value) \
    dlsm_gt_setspecific((key), (value))
#endif
#ifndef DLSM_GT_ADAPTER_LOCAL_GET
#define DLSM_GT_ADAPTER_LOCAL_GET(key) dlsm_gt_getspecific((key))
#endif

#ifndef DLSM_GT_ADAPTER_MUTEX_INIT
#define DLSM_GT_ADAPTER_MUTEX_INIT(mutex) dlsm_gt_mutex_init_for_gt((mutex))
#endif
#ifndef DLSM_GT_ADAPTER_MUTEX_LOCK
#define DLSM_GT_ADAPTER_MUTEX_LOCK(mutex) dlsm_gt_mutex_lock((mutex))
#endif
#ifndef DLSM_GT_ADAPTER_MUTEX_TIMEDLOCK
#define DLSM_GT_ADAPTER_MUTEX_TIMEDLOCK(mutex, deadline_ns) \
    dlsm_gt_mutex_timedlock((mutex), (deadline_ns))
#endif
#ifndef DLSM_GT_ADAPTER_MUTEX_TRYLOCK
#define DLSM_GT_ADAPTER_MUTEX_TRYLOCK(mutex, acquired) \
    dlsm_gt_mutex_trylock((mutex), (acquired))
#endif
#ifndef DLSM_GT_ADAPTER_MUTEX_UNLOCK
#define DLSM_GT_ADAPTER_MUTEX_UNLOCK(mutex) dlsm_gt_mutex_unlock((mutex))
#endif
#ifndef DLSM_GT_ADAPTER_MUTEX_DESTROY
#define DLSM_GT_ADAPTER_MUTEX_DESTROY(mutex) dlsm_gt_mutex_destroy((mutex))
#endif

#ifndef DLSM_GT_ADAPTER_CONDITION_INIT
#define DLSM_GT_ADAPTER_CONDITION_INIT(condition) \
    dlsm_gt_condition_init_for_gt((condition))
#endif
#ifndef DLSM_GT_ADAPTER_CONDITION_WAIT
#define DLSM_GT_ADAPTER_CONDITION_WAIT(condition, mutex) \
    dlsm_gt_condition_wait((condition), (mutex))
#endif
#ifndef DLSM_GT_ADAPTER_CONDITION_TIMEDWAIT
#define DLSM_GT_ADAPTER_CONDITION_TIMEDWAIT(condition, mutex, deadline_ns) \
    dlsm_gt_condition_timedwait((condition), (mutex), (deadline_ns))
#endif
#ifndef DLSM_GT_ADAPTER_CONDITION_SIGNAL
#define DLSM_GT_ADAPTER_CONDITION_SIGNAL(condition) \
    dlsm_gt_condition_signal((condition))
#endif
#ifndef DLSM_GT_ADAPTER_CONDITION_BROADCAST
#define DLSM_GT_ADAPTER_CONDITION_BROADCAST(condition) \
    dlsm_gt_condition_broadcast((condition))
#endif
#ifndef DLSM_GT_ADAPTER_CONDITION_DESTROY
#define DLSM_GT_ADAPTER_CONDITION_DESTROY(condition) \
    dlsm_gt_condition_destroy((condition))
#endif

#ifndef DLSM_GT_ADAPTER_NOW
#define DLSM_GT_ADAPTER_NOW() dlsm_gt_now()
#endif
#ifndef DLSM_GT_ADAPTER_SLEEP_FOR
#define DLSM_GT_ADAPTER_SLEEP_FOR(duration_ns) \
    dlsm_gt_sleep_for((duration_ns))
#endif
#ifndef DLSM_GT_ADAPTER_SLEEP_UNTIL
#define DLSM_GT_ADAPTER_SLEEP_UNTIL(deadline_ns) \
    dlsm_gt_sleep_until((deadline_ns))
#endif
#ifndef DLSM_GT_ADAPTER_TICKER_NEW
#define DLSM_GT_ADAPTER_TICKER_NEW(runtime, interval_ns) \
    dlsm_gt_ticker_new((runtime), (interval_ns))
#endif
#ifndef DLSM_GT_ADAPTER_TICKER_WAIT
#define DLSM_GT_ADAPTER_TICKER_WAIT(ticker, expiration_count) \
    dlsm_gt_ticker_wait((ticker), (expiration_count))
#endif
#ifndef DLSM_GT_ADAPTER_TICKER_RESET
#define DLSM_GT_ADAPTER_TICKER_RESET(ticker, interval_ns) \
    dlsm_gt_ticker_reset((ticker), (interval_ns))
#endif
#ifndef DLSM_GT_ADAPTER_TICKER_STOP
#define DLSM_GT_ADAPTER_TICKER_STOP(ticker) dlsm_gt_ticker_stop((ticker))
#endif
#ifndef DLSM_GT_ADAPTER_TICKER_FREE
#define DLSM_GT_ADAPTER_TICKER_FREE(ticker) dlsm_gt_ticker_free((ticker))
#endif

#ifndef DLSM_GT_ADAPTER_BLOCKING_CALL
#define DLSM_GT_ADAPTER_BLOCKING_CALL(function, arg, result) \
    dlsm_gt_blocking_call((function), (arg), (result))
#endif

#endif /* DLSM_GT_ADAPTER_H */
