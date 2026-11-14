#include "ch.h"
#include "hal.h"
#include "nvic.h"

#include <pthread.h>

#include "chprintf.h"
#include "usb_serial_device_to_host.h"
#include "usb_serial_host_to_device.hpp"

#include <cstdarg>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

namespace {

thread_local Thread* current_thread = nullptr;
thread_local std::vector<Mutex*> mutex_stack;

std::recursive_mutex sys_lock;
std::mutex registry_lock;
std::map<std::thread::id, Thread*> registry;

uint64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void thread_entry(Thread* self, tfunc_t fn, void* arg) {
    current_thread = self;
    {
        std::lock_guard<std::mutex> lk(registry_lock);
        registry[std::this_thread::get_id()] = self;
    }
    self->p_u.rdymsg = fn(arg);
    {
        std::lock_guard<std::mutex> lk(registry_lock);
        registry.erase(std::this_thread::get_id());
    }
    current_thread = nullptr;
}

Thread* spawn(tprio_t prio, tfunc_t fn, void* arg) {
    (void)prio;
    auto* t = new Thread();
    /* The firmware treats p_refs != 0 as "the thread object is live and can be
     * waited on"; the host keeps it set for the lifetime of the object. */
    t->p_refs = 1;
    Thread* raw = t;
    t->handle = std::thread([raw, fn, arg] { thread_entry(raw, fn, arg); });
    return t;
}

}  // namespace

SerialUSBDriver SUSBD1{};

/* The upstream USB serial header declares this handler pointer and the KISS TNC
 * app sets it through the UsbSerialInputHandler RAII wrapper. Its definition
 * normally lives in usb_serial_host_to_device.cpp, which pulls in the device's
 * USB stack and is not built on the host; define it here so the app links. On
 * the host no incoming USB bytes arrive, so it stays unused. */
usb_serial_input_handler_t usb_serial_active_input_handler = nullptr;

HostNVIC host_nvic;
HostSCB host_scb;

SDCDriver SDCD1;

void sdcStart(SDCDriver*, const void*) {}
void sdcStop(SDCDriver*) {}
msg_t sdcConnect(SDCDriver*) { return CH_SUCCESS; }
void sdcDisconnect(SDCDriver*) {}

Thread* chThdCreateFromHeap(void* heap, size_t size, tprio_t prio, tfunc_t fn, void* arg) {
    (void)heap;
    (void)size;
    return spawn(prio, fn, arg);
}

Thread* chThdCreateStatic(void* wsp, size_t size, tprio_t prio, tfunc_t fn, void* arg) {
    (void)wsp;
    (void)size;
    return spawn(prio, fn, arg);
}

Thread* chThdSelf() {
    return current_thread;
}

void chThdTerminate(Thread* tp) {
    if (tp) tp->terminate = true;
}

bool chThdShouldTerminate() {
    return current_thread ? current_thread->terminate.load() : false;
}

msg_t chThdWait(Thread* tp) {
    if (!tp) return 0;
    if (tp->handle.joinable()) {
        if (tp->handle.get_id() == std::this_thread::get_id()) {
            return 0;
        }
        tp->handle.join();
    }
    const msg_t result = tp->p_u.rdymsg;
    delete tp;
    return result;
}

void chThdExit(msg_t msg) {
    (void)msg;
    pthread_exit(nullptr);
}

void chThdSleepMilliseconds(uint32_t ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

void chThdSleepMicroseconds(uint32_t us) {
    std::this_thread::sleep_for(std::chrono::microseconds(us));
}

void chThdSleep(systime_t ticks) {
    chThdSleepMilliseconds(ticks);
}

void chThdSleepS(systime_t ticks) {
    chThdSleepMilliseconds(ticks);
}

void chThdYield() {
    std::this_thread::yield();
}

systime_t chTimeNow() {
    return static_cast<systime_t>(now_ms());
}

void chRegSetThreadName(const char* name) {
    if (current_thread) current_thread->name = name;
}

void chEvtSignal(Thread* tp, eventmask_t mask) {
    if (!tp) return;
    {
        std::lock_guard<std::mutex> lk(tp->lock);
        tp->events |= mask;
    }
    tp->cv.notify_all();
}

void chEvtSignalI(Thread* tp, eventmask_t mask) {
    chEvtSignal(tp, mask);
}

void chEvtBroadcastI(eventmask_t mask) {
    (void)mask;
}

void chEvtInit(eventmask_t* ep) {
    if (ep) *ep = 0;
}

eventmask_t chEvtWaitAny(eventmask_t mask) {
    Thread* tp = current_thread;
    if (!tp) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return 0;
    }
    std::unique_lock<std::mutex> lk(tp->lock);
    tp->cv.wait(lk, [tp, mask] { return (tp->events & mask) != 0; });
    const eventmask_t result = tp->events & mask;
    tp->events &= ~mask;
    return result;
}

eventmask_t chEvtWaitAnyTimeout(eventmask_t mask, systime_t timeout) {
    Thread* tp = current_thread;
    if (!tp) return 0;
    std::unique_lock<std::mutex> lk(tp->lock);
    const bool signalled = tp->cv.wait_for(
        lk, std::chrono::milliseconds(timeout),
        [tp, mask] { return (tp->events & mask) != 0; });
    const eventmask_t result = signalled ? (tp->events & mask) : 0;
    tp->events &= ~mask;
    return result;
}

eventmask_t chEvtWaitOne(eventmask_t mask) {
    Thread* tp = current_thread;
    if (!tp) return 0;
    std::unique_lock<std::mutex> lk(tp->lock);
    tp->cv.wait(lk, [tp, mask] { return (tp->events & mask) != 0; });
    const eventmask_t bits = tp->events & mask;
    const eventmask_t result = bits & (~bits + 1);
    tp->events &= ~result;
    return result;
}

eventmask_t chEvtGetAndClearFlags(Thread* tp) {
    if (!tp) return 0;
    std::lock_guard<std::mutex> lk(tp->lock);
    const eventmask_t result = tp->events;
    tp->events = 0;
    return result;
}

void chMtxInit(Mutex* mp) {
    if (mp) mp->owner = nullptr;
}

void chMtxLock(Mutex* mp) {
    if (!mp) return;
    mp->impl.lock();
    mp->owner = current_thread;
    mutex_stack.push_back(mp);
}

bool chMtxTryLock(Mutex* mp) {
    if (!mp) return false;
    if (!mp->impl.try_lock()) return false;
    mp->owner = current_thread;
    mutex_stack.push_back(mp);
    return true;
}

void chMtxUnlock() {
    if (mutex_stack.empty()) return;
    Mutex* mp = mutex_stack.back();
    mutex_stack.pop_back();
    mp->impl.unlock();
}

void chMtxUnlockS() {
    chMtxUnlock();
}

void chSysLock() {
    sys_lock.lock();
}

void chSysUnlock() {
    sys_lock.unlock();
}

void chSysLockFromIsr() {
    chSysLock();
}

void chSysUnlockFromIsr() {
    chSysUnlock();
}

bool chSysIsLocked() {
    return false;
}

void chSchGoSleepS(int state) {
    (void)state;
    Thread* tp = current_thread;
    if (!tp) return;
    std::unique_lock<std::mutex> lk(tp->lock);
    tp->cv.wait(lk, [tp] { return tp->ready; });
    tp->ready = false;
}

void chSchReadyI(Thread* tp) {
    if (!tp) return;
    {
        std::lock_guard<std::mutex> lk(tp->lock);
        tp->ready = true;
    }
    tp->cv.notify_all();
}

void* chHeapAlloc(void* heap, size_t size) {
    (void)heap;
    return std::malloc(size);
}

void chHeapFree(void* p) {
    std::free(p);
}

size_t chCoreStatus() {
    return 1U << 20;
}

void chSysDisable() {}

void systick_stop() {}

void chSysHalt(const char* reason) {
    std::fprintf(stderr, "chSysHalt: %s\n", reason ? reason : "");
    std::abort();
}

void chDbgPanic(const char* msg) {
    std::fprintf(stderr, "PANIC: %s\n", msg ? msg : "(null)");
    std::abort();
}

void _chDbgPanic(const char* msg) {
    chDbgPanic(msg);
}

void chDbgAssertImpl(bool condition, const char* msg) {
    if (!condition) chDbgPanic(msg);
}

size_t chHeapStatus(void* heap, size_t* fragp) {
    (void)heap;
    if (fragp) *fragp = 0;
    return 1U << 20;
}

Thread* chSysGetIdleThread() {
    return nullptr;
}

systime_t chThdGetTicks(Thread* tp) {
    (void)tp;
    return chTimeNow();
}

/* Power/diagnostic stubs referenced by persistent memory and SD debug code. */
void sdio_cclk_set(int divider) {
    (void)divider;
}

bool sdcIsCardInserted(SDCDriver* sdcp) {
    (void)sdcp;
    return true;
}

halrtcnt_t halGetCounterValue() {
    return static_cast<halrtcnt_t>(now_ms() * 1000ULL);
}

uint32_t halGetCounterFrequency() {
    return 1000000U;
}

int sdcGetInfo(SDCDriver* sdcp, BlockDeviceInfo* bdip) {
    (void)sdcp;
    if (bdip) {
        bdip->blk_size = 512;
        bdip->blk_num = 0;
        bdip->blk_state = 0;
    }
    return CH_SUCCESS;
}

void chprintf(BaseSequentialStream* chp, const char* fmt, ...) {
    (void)chp;
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
}

void chSequentialStreamWrite(BaseSequentialStream* chp, const uint8_t* buf, size_t n) {
    (void)chp;
    std::fwrite(buf, 1, n, stderr);
}

uint8_t chSequentialStreamGet(BaseSequentialStream* chp) {
    (void)chp;
    return 0;
}

int chSequentialStreamPut(BaseSequentialStream* chp, uint8_t byte) {
    (void)chp;
    std::fputc(byte, stderr);
    return 0;
}

/* The upstream KISS TNC app routes bytes through UsbSerialInputHandler::write,
 * which calls this ChibiOS output-queue function. The host has no USB device
 * controller, so the bytes are discarded; the symbol exists so the upstream
 * USB serial header links. Returns the number of bytes accepted, matching the
 * ChibiOS contract (callers treat a short/zero return as "try again later"). */
size_t chOQWriteTimeout(OutputQueue* oqp, const uint8_t* bp, size_t n, systime_t time) {
    (void)oqp;
    (void)bp;
    (void)time;
    return n;
}
