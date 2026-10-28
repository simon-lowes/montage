// Montage — the calling thread's scheduling kept across a library call that changes it. SVT-AV1 raises the thread
// that opens an encoder to real-time (first in, first out, priority 99) when it is allowed to, as root is; every
// thread started from that one afterwards would inherit it, so Montage's own workers ran real-time, and slower where
// a container rations real-time time.
#pragma once

#ifndef _WIN32
#include <pthread.h>
#include <sched.h>
#endif

namespace montage {

class KeepThreadScheduling {
public:
    KeepThreadScheduling() {
#ifndef _WIN32
        ok_ = pthread_getschedparam(pthread_self(), &policy_, &param_) == 0;
#endif
    }
    ~KeepThreadScheduling() { restore(); }
    KeepThreadScheduling(const KeepThreadScheduling&) = delete;
    KeepThreadScheduling& operator=(const KeepThreadScheduling&) = delete;
    // Back to what it was when this was made, if it changed.
    void restore() {
#ifndef _WIN32
        int policy = 0;
        sched_param now{};
        if (ok_ && pthread_getschedparam(pthread_self(), &policy, &now) == 0 &&
            (policy != policy_ || now.sched_priority != param_.sched_priority))
            pthread_setschedparam(pthread_self(), policy_, &param_);
#endif
    }

private:
#ifndef _WIN32
    bool ok_ = false;
    int policy_ = 0;
    sched_param param_{};
#endif
};

}  // namespace montage
