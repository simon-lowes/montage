// Montage — a guard on the audio device's callback (audio/AudioDevice.h). A device's callback runs on the driver's
// thread, against a deadline of a few milliseconds: memory taken from the heap, a lock, or a wait there is where clicks
// and dropouts come from on a busy computer. Each backend marks its callback with a Scope; while a thread is inside
// one, every allocation, lock and wait it makes is counted. The counts are always kept (the tests read them), and are
// written to the log when the device closes with MONTAGE_AUDIO_GUARD_LOG set.
//
// What is seen, on Linux: allocations and frees made by Montage's own code and the libraries built into it (the linker
// passes those calls through here; nothing is replaced, so the allocator and the tools that watch it work as before),
// and, with glibc, mutex locks, condition waits and sleeps from anywhere in the program (the pthread calls are passed
// through here). On other systems nothing is seen and only the callbacks are counted; sees() says which.
#pragma once

#include <chrono>
#include <cstdint>

namespace montage::rtguard {

struct Counts {
    int64_t callbacks = 0;    // scopes entered
    int64_t allocations = 0;  // heap allocations and frees inside a scope
    int64_t locks = 0;        // mutex locks inside a scope
    int64_t waits = 0;        // condition waits and sleeps inside a scope
};
Counts counts();
void reset();

enum Sees { Allocations = 1, Locks = 2, Waits = 4 };
int sees();  // what this build can count (a set of Sees)

// Marks the calling thread as inside a device's callback for as long as it lives (scopes nest).
class Scope {
public:
    Scope() noexcept;
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};
bool inCallback() noexcept;

// A signal a callback can raise without a lock, an allocation or a wait, for a worker to sleep on: a semaphore of the
// system's own (POSIX, Mach through libdispatch, or a Win32 event).
class Wake {
public:
    Wake();
    ~Wake();
    Wake(const Wake&) = delete;
    Wake& operator=(const Wake&) = delete;
    void post() noexcept;       // any thread, the callback's too: raised once until it is waited for
    void wait(int ms) noexcept;  // until raised, or `ms` at most

private:
    struct Impl;
    Impl* d_;
};

// Sleeps until `when`, as near it as the system wakes a thread: on Windows on a high-resolution waitable timer, where
// std::this_thread::sleep_until ends on a tick of the system's timer (15.6 ms apart, unless a program has asked for
// finer); elsewhere as std::this_thread::sleep_until.
void sleepUntil(std::chrono::steady_clock::time_point when) noexcept;

}  // namespace montage::rtguard
