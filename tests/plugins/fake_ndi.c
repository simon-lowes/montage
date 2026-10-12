/* A stand-in for the NDI Runtime (libndi): the functions Montage's NDI output loads, keeping what they are given for
   tests to read back through fake_ndi_info. Video is paced at its frame rate when the sender clocks it. */
#ifdef _WIN32
#define PROCESSINGNDILIB_EXPORTS
#include <windows.h>
#else
#include <time.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Processing.NDI.Lib.h"

#ifdef _WIN32
#define FAKE_EXPORT __declspec(dllexport)
#else
#define FAKE_EXPORT __attribute__((visibility("default")))
#endif

typedef struct {
    char name[64];
    int clock;
} sender_t;

static int videos, audios;
static NDIlib_video_frame_v2_t lastVideo;
static char lastMeta[256];
static unsigned char firstBytes[8];
static NDIlib_audio_frame_v2_t lastAudio;
static float audioFirst[2];
static char lastName[64];
static int lastClock;

static void pause_for(int n, int d) {
    if (n <= 0 || d <= 0) return;
#ifdef _WIN32
    Sleep((DWORD)(1000LL * d / n));
#else
    struct timespec ts;
    long long ns = 1000000000LL * d / n;
    ts.tv_sec = (time_t)(ns / 1000000000LL);
    ts.tv_nsec = (long)(ns % 1000000000LL);
    nanosleep(&ts, NULL);
#endif
}

PROCESSINGNDILIB_API bool NDIlib_initialize(void) { return true; }
PROCESSINGNDILIB_API void NDIlib_destroy(void) {}
PROCESSINGNDILIB_API const char* NDIlib_version(void) { return "Fake NDI 6"; }

PROCESSINGNDILIB_API NDIlib_send_instance_t NDIlib_send_create(const NDIlib_send_create_t* c) {
    sender_t* s = (sender_t*)calloc(1, sizeof(sender_t));
    if (c && c->p_ndi_name) strncpy(s->name, c->p_ndi_name, sizeof s->name - 1);
    s->clock = c ? c->clock_video : 1;
    strncpy(lastName, s->name, sizeof lastName - 1);
    lastClock = s->clock;
    return (NDIlib_send_instance_t)s;
}

PROCESSINGNDILIB_API void NDIlib_send_destroy(NDIlib_send_instance_t p) { free(p); }

PROCESSINGNDILIB_API void NDIlib_send_send_video_async_v2(NDIlib_send_instance_t p, const NDIlib_video_frame_v2_t* f) {
    sender_t* s = (sender_t*)p;
    if (!f) return;
    ++videos;
    lastVideo = *f;
    lastMeta[0] = 0;
    if (f->p_metadata) strncpy(lastMeta, f->p_metadata, sizeof lastMeta - 1);
    memcpy(firstBytes, f->p_data, sizeof firstBytes);
    lastVideo.p_data = NULL;
    lastVideo.p_metadata = NULL;
    if (s && s->clock) pause_for(f->frame_rate_N, f->frame_rate_D);
}

PROCESSINGNDILIB_API void NDIlib_send_send_audio_v2(NDIlib_send_instance_t p, const NDIlib_audio_frame_v2_t* a) {
    (void)p;
    ++audios;
    lastAudio = *a;
    audioFirst[0] = a->p_data[0];
    audioFirst[1] = a->no_channels > 1 ? a->p_data[a->channel_stride_in_bytes / 4] : 0;
    lastAudio.p_data = NULL;
}

PROCESSINGNDILIB_API int NDIlib_send_get_no_connections(NDIlib_send_instance_t p, uint32_t timeout) {
    (void)p;
    (void)timeout;
    return 1;
}

FAKE_EXPORT int fake_ndi_info(char* out, int size) {
    return snprintf(out, (size_t)size,
                    "name=%s clock=%d videos=%d fourcc=%c%c%c%c x=%d y=%d rate=%d/%d stride=%d tc=%lld bytes=%u,%u,%u,%u "
                    "meta=%s audios=%d arate=%d ch=%d n=%d cstride=%d a0=%g a1=%g",
                    lastName, lastClock, videos, (char)(lastVideo.FourCC & 255), (char)(lastVideo.FourCC >> 8 & 255),
                    (char)(lastVideo.FourCC >> 16 & 255), (char)(lastVideo.FourCC >> 24 & 255), lastVideo.xres,
                    lastVideo.yres, lastVideo.frame_rate_N, lastVideo.frame_rate_D, lastVideo.line_stride_in_bytes,
                    (long long)lastVideo.timecode, firstBytes[0], firstBytes[1], firstBytes[2], firstBytes[3], lastMeta,
                    audios, lastAudio.sample_rate, lastAudio.no_channels, lastAudio.no_samples,
                    lastAudio.channel_stride_in_bytes, audioFirst[0], audioFirst[1]);
}
