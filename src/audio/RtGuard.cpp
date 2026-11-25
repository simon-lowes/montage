#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "RtGuard.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <new>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <dispatch/dispatch.h>
#else
#include <cerrno>
#include <semaphore.h>
#include <time.h>
#endif

#if defined(MONTAGE_RT_GUARD_INTERPOSE) && defined(__linux__) && defined(__GLIBC__)
#define MONTAGE_RT_GUARD_PTHREAD 1
#include <dlfcn.h>
#include <pthread.h>
#endif

namespace montage::rtguard {

namespace {

// (plain atomics, started before any code runs: the replaced operators below are called from the very first
// allocation of the program)
std::atomic<int> gInside{0};  // threads inside a scope now (so nothing else pays for a thread-local read)
std::atomic<int64_t> gCallbacks{0}, gAllocations{0}, gLocks{0}, gWaits{0};
thread_local int tDepth = 0;

inline bool inside() noexcept { return gInside.load(std::memory_order_relaxed) > 0 && tDepth > 0; }

}  // namespace

Counts counts() { return {gCallbacks.load(), gAllocations.load(), gLocks.load(), gWaits.load()}; }

void reset() {
    gCallbacks = 0;
    gAllocations = 0;
    gLocks = 0;
    gWaits = 0;
}

int sees() {
    int what = 0;
#ifdef MONTAGE_RT_GUARD_WRAP
    what |= Allocations;
#endif
#ifdef MONTAGE_RT_GUARD_PTHREAD
    what |= Locks | Waits;
#endif
    return what;
}

Scope::Scope() noexcept {
    if (tDepth++ == 0) {
        gInside.fetch_add(1, std::memory_order_relaxed);
        gCallbacks.fetch_add(1, std::memory_order_relaxed);
    }
}

Scope::~Scope() {
    if (--tDepth == 0) gInside.fetch_sub(1, std::memory_order_relaxed);
}

bool inCallback() noexcept { return inside(); }

namespace detail {
void noteAllocation() noexcept;
void noteLock() noexcept;
void noteWait() noexcept;
void noteAllocation() noexcept {
    if (inside()) gAllocations.fetch_add(1, std::memory_order_relaxed);
}
void noteLock() noexcept {
    if (inside()) gLocks.fetch_add(1, std::memory_order_relaxed);
}
void noteWait() noexcept {
    if (inside()) gWaits.fetch_add(1, std::memory_order_relaxed);
}
}  // namespace detail

// ---- Wake.

#if defined(_WIN32)
struct Wake::Impl {
    HANDLE event = nullptr;
};
Wake::Wake() : d_(new Impl) { d_->event = CreateEventW(nullptr, FALSE, FALSE, nullptr); }  // (auto-reset: raised once)
Wake::~Wake() {
    if (d_->event) CloseHandle(d_->event);
    delete d_;
}
void Wake::post() noexcept {
    if (d_->event) SetEvent(d_->event);
}
void Wake::wait(int ms) noexcept {
    if (d_->event) WaitForSingleObject(d_->event, DWORD(ms < 0 ? 0 : ms));
    else Sleep(DWORD(ms < 1 ? 1 : ms));
}
#elif defined(__APPLE__)
struct Wake::Impl {
    dispatch_semaphore_t sem = nullptr;
    std::atomic<bool> raised{false};
};
Wake::Wake() : d_(new Impl) { d_->sem = dispatch_semaphore_create(0); }
Wake::~Wake() {
    // (a semaphore must be at its first count when it is let go: what was raised and never waited for is taken back)
    if (d_->sem) {
        if (d_->raised.exchange(false)) dispatch_semaphore_wait(d_->sem, DISPATCH_TIME_NOW);
        dispatch_release(d_->sem);
    }
    delete d_;
}
void Wake::post() noexcept {
    if (d_->sem && !d_->raised.exchange(true)) dispatch_semaphore_signal(d_->sem);
}
void Wake::wait(int ms) noexcept {
    if (!d_->sem) return;
    const long timedOut = dispatch_semaphore_wait(d_->sem, dispatch_time(DISPATCH_TIME_NOW, int64_t(ms < 0 ? 0 : ms) * 1000000));
    if (!timedOut) d_->raised = false;
}
#else
struct Wake::Impl {
    sem_t sem;
    std::atomic<bool> raised{false};
};
Wake::Wake() : d_(new Impl) { sem_init(&d_->sem, 0, 0); }
Wake::~Wake() {
    sem_destroy(&d_->sem);
    delete d_;
}
void Wake::post() noexcept {
    if (!d_->raised.exchange(true)) sem_post(&d_->sem);  // (async-signal-safe: no lock, no wait)
}
void Wake::wait(int ms) noexcept {
    // (on the steady clock where the C library waits on it, glibc 2.30 and later: the wall clock set by hand or
    // stepped by a time server neither ends the wait early nor holds it for as long as the clock went back)
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 30))
    const clockid_t clock = CLOCK_MONOTONIC;
#else
    const clockid_t clock = CLOCK_REALTIME;
#endif
    timespec until{};
    clock_gettime(clock, &until);
    until.tv_nsec += long(ms < 0 ? 0 : ms) * 1000000L;
    until.tv_sec += until.tv_nsec / 1000000000L;
    until.tv_nsec %= 1000000000L;
    int r;
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 30))
    do r = sem_clockwait(&d_->sem, clock, &until);
#else
    do r = sem_timedwait(&d_->sem, &until);
#endif
    while (r != 0 && errno == EINTR);
    if (r == 0) d_->raised = false;
}
#endif

// ---- sleepUntil.

#if defined(_WIN32)
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
namespace {
// One for each thread that sleeps on it (Windows 10 1803 and later; none before, and the tick is waited for then).
struct HighResolutionTimer {
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    ~HighResolutionTimer() {
        if (timer) CloseHandle(timer);
    }
};
}  // namespace

void sleepUntil(std::chrono::steady_clock::time_point when) noexcept {
    const auto left = when - std::chrono::steady_clock::now();
    if (left <= std::chrono::steady_clock::duration::zero()) return;
    thread_local HighResolutionTimer t;
    LARGE_INTEGER due;
    due.QuadPart = -std::max<LONGLONG>(1, LONGLONG(std::chrono::duration_cast<std::chrono::nanoseconds>(left).count() / 100));  // (from now, in 100 ns)
    if (t.timer && SetWaitableTimer(t.timer, &due, 0, nullptr, nullptr, FALSE)) {
        WaitForSingleObject(t.timer, INFINITE);
        return;
    }
    std::this_thread::sleep_until(when);
}
#else
void sleepUntil(std::chrono::steady_clock::time_point when) noexcept { std::this_thread::sleep_until(when); }
#endif

}  // namespace montage::rtguard

// ---------------------------------------------------------------------------
// The heap (Linux): the linker sends every call to these from Montage's own code, and the libraries built into it,
// through the wrappers here (its --wrap option, set where montage_engine is linked), which count and go on to the
// real ones. Nothing is replaced: the allocator, and the tools that watch it, see every call as before. Calls made
// inside shared libraries are not seen.

#ifdef MONTAGE_RT_GUARD_WRAP

extern "C" {

void* __real_malloc(std::size_t);
void* __real_calloc(std::size_t, std::size_t);
void* __real_realloc(void*, std::size_t);
void __real_free(void*);
int __real_posix_memalign(void**, std::size_t, std::size_t);
void* __real_aligned_alloc(std::size_t, std::size_t);
void* __real__Znwm(std::size_t);
void* __real__Znam(std::size_t);
void* __real__ZnwmRKSt9nothrow_t(std::size_t, const std::nothrow_t&);
void* __real__ZnamRKSt9nothrow_t(std::size_t, const std::nothrow_t&);
void __real__ZdlPv(void*);
void __real__ZdaPv(void*);
void __real__ZdlPvm(void*, std::size_t);
void __real__ZdaPvm(void*, std::size_t);

void* __wrap_malloc(std::size_t n) {
    montage::rtguard::detail::noteAllocation();
    return __real_malloc(n);
}
void* __wrap_calloc(std::size_t n, std::size_t size) {
    montage::rtguard::detail::noteAllocation();
    return __real_calloc(n, size);
}
void* __wrap_realloc(void* p, std::size_t n) {
    montage::rtguard::detail::noteAllocation();
    return __real_realloc(p, n);
}
void __wrap_free(void* p) {
    if (p) montage::rtguard::detail::noteAllocation();
    __real_free(p);
}
int __wrap_posix_memalign(void** out, std::size_t alignment, std::size_t n) {
    montage::rtguard::detail::noteAllocation();
    return __real_posix_memalign(out, alignment, n);
}
void* __wrap_aligned_alloc(std::size_t alignment, std::size_t n) {
    montage::rtguard::detail::noteAllocation();
    return __real_aligned_alloc(alignment, n);
}
void* __wrap__Znwm(std::size_t n) {
    montage::rtguard::detail::noteAllocation();
    return __real__Znwm(n);
}
void* __wrap__Znam(std::size_t n) {
    montage::rtguard::detail::noteAllocation();
    return __real__Znam(n);
}
void* __wrap__ZnwmRKSt9nothrow_t(std::size_t n, const std::nothrow_t& t) {
    montage::rtguard::detail::noteAllocation();
    return __real__ZnwmRKSt9nothrow_t(n, t);
}
void* __wrap__ZnamRKSt9nothrow_t(std::size_t n, const std::nothrow_t& t) {
    montage::rtguard::detail::noteAllocation();
    return __real__ZnamRKSt9nothrow_t(n, t);
}
void __wrap__ZdlPv(void* p) {
    if (p) montage::rtguard::detail::noteAllocation();
    __real__ZdlPv(p);
}
void __wrap__ZdaPv(void* p) {
    if (p) montage::rtguard::detail::noteAllocation();
    __real__ZdaPv(p);
}
void __wrap__ZdlPvm(void* p, std::size_t n) {
    if (p) montage::rtguard::detail::noteAllocation();
    __real__ZdlPvm(p, n);
}
void __wrap__ZdaPvm(void* p, std::size_t n) {
    if (p) montage::rtguard::detail::noteAllocation();
    __real__ZdaPvm(p, n);
}

}  // extern "C"

#endif

// ---------------------------------------------------------------------------
// Locks and waits (Linux with glibc): the program's own definitions are found before the C library's by everything
// loaded, count when the thread is inside a callback, and go on to the library's.

#ifdef MONTAGE_RT_GUARD_PTHREAD

namespace {

template <typename F>
F next(std::atomic<void*>& slot, const char* name) {
    void* f = slot.load(std::memory_order_relaxed);
    if (!f) {
        f = dlsym(RTLD_NEXT, name);
        slot.store(f, std::memory_order_relaxed);
    }
    return reinterpret_cast<F>(f);
}

std::atomic<void*> gMutexLock{nullptr}, gCondWait{nullptr}, gCondTimedWait{nullptr}, gCondClockWait{nullptr}, gNanosleep{nullptr},
    gClockNanosleep{nullptr};

}  // namespace

extern "C" {

// (the loader's own locks, taken while it looks the next definition up, do not come back through here)
int pthread_mutex_lock(pthread_mutex_t* m) {
    montage::rtguard::detail::noteLock();
    return next<int (*)(pthread_mutex_t*)>(gMutexLock, "pthread_mutex_lock")(m);
}

int pthread_cond_wait(pthread_cond_t* c, pthread_mutex_t* m) {
    montage::rtguard::detail::noteWait();
    return next<int (*)(pthread_cond_t*, pthread_mutex_t*)>(gCondWait, "pthread_cond_wait")(c, m);
}

int pthread_cond_timedwait(pthread_cond_t* c, pthread_mutex_t* m, const struct timespec* t) {
    montage::rtguard::detail::noteWait();
    return next<int (*)(pthread_cond_t*, pthread_mutex_t*, const struct timespec*)>(gCondTimedWait, "pthread_cond_timedwait")(c, m, t);
}

int pthread_cond_clockwait(pthread_cond_t* c, pthread_mutex_t* m, clockid_t clock, const struct timespec* t) {
    montage::rtguard::detail::noteWait();
    const auto f = next<int (*)(pthread_cond_t*, pthread_mutex_t*, clockid_t, const struct timespec*)>(gCondClockWait, "pthread_cond_clockwait");
    return f ? f(c, m, clock, t) : ENOSYS;
}

int nanosleep(const struct timespec* req, struct timespec* rem) {
    montage::rtguard::detail::noteWait();
    return next<int (*)(const struct timespec*, struct timespec*)>(gNanosleep, "nanosleep")(req, rem);
}

int clock_nanosleep(clockid_t clock, int flags, const struct timespec* req, struct timespec* rem) {
    montage::rtguard::detail::noteWait();
    return next<int (*)(clockid_t, int, const struct timespec*, struct timespec*)>(gClockNanosleep, "clock_nanosleep")(clock, flags, req, rem);
}

}  // extern "C"

#endif
