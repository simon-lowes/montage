/* Montage test plug-ins: faults on request, for the plug-in host tests.
 *
 * MONTAGE_TEST_PLUGIN_FAULT="<kind>[:<call>[:<after>]]" makes the plug-in misbehave in the named call ("process" when
 * left out) after that many calls of it have gone well (0 when left out):
 *   crash    writes through a null pointer
 *   hang     never returns
 *   overrun  writes far past the end of the buffer it was given
 *   leak     takes 64 MB more memory each call (touched, never given back)
 * The tests set it only while a plug-in host starts, so it reaches the host and never the test's own process. */
#ifndef MONTAGE_TEST_FAULT_H
#define MONTAGE_TEST_FAULT_H

#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

static int montage_test_fault_calls[8];
static void* volatile montage_test_fault_leaked; /* (kept where the compiler cannot see it is never read) */

static void montage_test_fault_sleep(void) {
#ifdef _WIN32
    Sleep(100);
#else
    usleep(100000);
#endif
}

/* `call` names the call ("process", "activate", "save", "load"...); `buf`, when given, is a buffer of `n` floats the
 * call writes into. */
static void montage_test_fault(const char* call, float* buf, size_t n) {
    const char* spec = getenv("MONTAGE_TEST_PLUGIN_FAULT");
    if (!spec || !*spec) return;
    char kind[16] = {0}, which[32] = "process";
    int after = 0;
    const char* c1 = strchr(spec, ':');
    size_t kl = c1 ? (size_t)(c1 - spec) : strlen(spec);
    if (kl >= sizeof kind) return;
    memcpy(kind, spec, kl);
    if (c1) {
        const char* c2 = strchr(c1 + 1, ':');
        size_t wl = c2 ? (size_t)(c2 - c1 - 1) : strlen(c1 + 1);
        if (wl >= sizeof which) return;
        memcpy(which, c1 + 1, wl);
        which[wl] = 0;
        if (c2) after = atoi(c2 + 1);
    }
    if (strcmp(which, call) != 0) return;
    int slot = (int)(strlen(call) % 8);
    if (montage_test_fault_calls[slot]++ < after) return;
    if (!strcmp(kind, "crash")) {
        volatile int* nothing = NULL;
        *nothing = 1;
        abort();
    } else if (!strcmp(kind, "hang")) {
        for (;;) montage_test_fault_sleep();
    } else if (!strcmp(kind, "overrun")) {
        float* p = buf;
        if (!p) {
            p = (float*)malloc(16);
            n = 4;
        }
        /* Past the end: the next pages one by one, then a megabyte at a time until the memory beyond is not there to
         * write. (A page at a time all the way can take seconds when a large writable mapping lies beyond the buffer,
         * as it seems to on the macOS runner, and a crash that late reads as a hang.) */
        size_t i = n;
        for (; i < n + 64 * 1024; i += 1024) ((volatile float*)p)[i] = 1.0f;
        for (; i < ((size_t)1 << 32); i += (size_t)1 << 18) ((volatile float*)p)[i] = 1.0f;
        abort();
    } else if (!strcmp(kind, "leak")) {
        const size_t bytes = (size_t)64 << 20;
        char* m = (char*)malloc(bytes);
        if (m) memset(m, 1, bytes);
        montage_test_fault_leaked = m;
    }
}

#endif
