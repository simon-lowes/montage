// CLAP libraries that misbehave while loading, to test scan isolation:
// built with -DMONTAGE_TEST_HANG (never finishes initialising) or
// -DMONTAGE_TEST_CRASH (kills its process like a crash would).
#include <clap/clap.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

static bool entry_init(const char* path) {
    (void)path;
#if defined(MONTAGE_TEST_HANG)
    for (;;) {
#ifdef _WIN32
        Sleep(1000);
#else
        sleep(1);
#endif
    }
#else
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), 3);  // no crash dialog on CI
#else
    abort();
#endif
#endif
    return false;
}
static void entry_deinit(void) {}
static const void* entry_factory(const char* id) { (void)id; return NULL; }

CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, entry_init, entry_deinit, entry_factory};
