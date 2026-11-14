/*
 * Host replacement for the ChibiOS kernel API used by the Mayhem UI layer.
 *
 * PortaPack desktop port. This file is part of a GPLv3-or-later derived work;
 * see the upstream Mayhem firmware for the original licenses.
 *
 * Only the subset actually used by the application/common code is provided.
 * Semantics follow ChibiOS 2.6: 1 tick = 1 ms, event flags per thread,
 * mutexes are recursive and unlocked in LIFO order (chMtxUnlock()).
 */
#ifndef __CH_H__
#define __CH_H__

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

using msg_t = int32_t;
using systime_t = uint32_t;
using eventmask_t = uint32_t;
using eventid_t = uint32_t;
using tprio_t = int32_t;
using cnt_t = int32_t;
using tfunc_t = msg_t (*)(void*);

#define CH_FREQUENCY 1000
#define EVENT_MASK(mask) (static_cast<eventmask_t>(1U) << (mask))
#define ALL_EVENTS (static_cast<eventmask_t>(0xFFFFFFFFU))
#define TIME_IMMEDIATE (static_cast<systime_t>(0))
#define TIME_INFINITE (static_cast<systime_t>(0xFFFFFFFFU))
#define MS2ST(msec) (static_cast<systime_t>(msec))
#define US2ST(usec) (static_cast<systime_t>((usec) / 1000U))

#define NORMALPRIO 0
#define LOWPRIO (-1)
#define HIGHPRIO 1

#define THD_STATE_SUSPENDED 0

#define CORTEX_M0 0
#define CORTEX_M4 4

struct Thread;

struct Mutex {
    std::recursive_mutex impl;
    Thread* owner{nullptr};
};

/* FatFs re-entrancy uses this type through ffconf.h. */
struct Semaphore {
    std::recursive_mutex impl;
};

/* Per-thread state. The public fields mirror the ChibiOS ones used by the
 * firmware (notably p_u.rdymsg in ThreadWait). */
struct Thread {
    std::thread handle;
    std::atomic<bool> terminate{false};
    std::mutex lock;
    std::condition_variable cv;
    eventmask_t events{0};
    bool ready{false};
    const char* name{nullptr};
    /* ChibiOS reference counter. The firmware inspects it before chThdWait()
     * (e.g. external/flippertx) to tell whether a thread object is still
     * alive; the host keeps it at 1 for the lifetime of the thread. */
    cnt_t p_refs{0};
    union {
        msg_t rdymsg;
    } p_u{};

    Thread() = default;
    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;
};

/* Threads created by this port. */
Thread* chThdCreateFromHeap(void* heap, size_t size, tprio_t prio, tfunc_t fn, void* arg);
Thread* chThdCreateStatic(void* wsp, size_t size, tprio_t prio, tfunc_t fn, void* arg);
Thread* chThdSelf();
void chThdTerminate(Thread* tp);
bool chThdShouldTerminate();
msg_t chThdWait(Thread* tp);
void chThdExit(msg_t msg);
void chThdSleep(systime_t ticks);
void chThdSleepMilliseconds(uint32_t ms);
void chThdSleepMicroseconds(uint32_t us);
void chThdSleepS(systime_t ticks);
void chThdYield();
systime_t chTimeNow();
void chRegSetThreadName(const char* name);

/* Event flags. */
void chEvtSignal(Thread* tp, eventmask_t mask);
void chEvtSignalI(Thread* tp, eventmask_t mask);
void chEvtBroadcastI(eventmask_t mask);
void chEvtInit(eventmask_t* ep);
eventmask_t chEvtWaitAny(eventmask_t mask);
eventmask_t chEvtWaitAnyTimeout(eventmask_t mask, systime_t timeout);
eventmask_t chEvtWaitOne(eventmask_t mask);
eventmask_t chEvtGetAndClearFlags(Thread* tp);

/* Mutexes. chMtxUnlock() releases the most recently locked mutex of the
 * calling thread, as in ChibiOS 2.6. */
void chMtxInit(Mutex* mp);
void chMtxLock(Mutex* mp);
bool chMtxTryLock(Mutex* mp);
void chMtxUnlock();
void chMtxUnlockS();

/* Critical sections. */
void chSysLock();
void chSysUnlock();
void chSysLockFromIsr();
void chSysUnlockFromIsr();
bool chSysIsLocked();

/* Scheduler helpers used by ThreadWait. */
void chSchGoSleepS(int state);
void chSchReadyI(Thread* tp);

/* Heap used by the application. */
void* chHeapAlloc(void* heap, size_t size);
void chHeapFree(void* p);
size_t chCoreStatus();

void chSysDisable();
void systick_stop();
void chSysHalt(const char* reason = nullptr);
void chDbgPanic(const char* msg);
void chDbgAssertImpl(bool condition, const char* msg);
#define chDbgAssert(condition, msg, ...) chDbgAssertImpl((condition), (msg))

/* Diagnostics used by the debug/DFU screens. */
size_t chHeapStatus(void* heap, size_t* fragp);
Thread* chSysGetIdleThread();
systime_t chThdGetTicks(Thread* tp);

#define CH_IRQ_HANDLER(name) void name(void)
#define CH_IRQ_PROLOGUE()
#define CH_IRQ_EPILOGUE()

#ifdef __cplusplus
extern "C" {
#endif
/* ChibiOS-compatible aliases. */
void _chDbgPanic(const char* msg);
#ifdef __cplusplus
}
#endif

#endif /* __CH_H__ */
