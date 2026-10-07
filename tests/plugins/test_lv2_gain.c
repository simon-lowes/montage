/* A test LV2 plugin for Montage: stereo gain (dB) delayed by 16 samples,
   reporting that latency on a lv2:reportsLatency output. */
#include <lv2/core/lv2.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define DELAY 16

typedef struct {
    const float* gain;
    float* latency;
    const float* in[2];
    float* out[2];
    float line[2][DELAY];
    int pos;
} Gain;

static LV2_Handle instantiate(const LV2_Descriptor* d, double rate, const char* path, const LV2_Feature* const* f) {
    (void)d;
    (void)rate;
    (void)path;
    (void)f;
    return calloc(1, sizeof(Gain));
}

static void connect_port(LV2_Handle h, uint32_t port, void* data) {
    Gain* g = (Gain*)h;
    switch (port) {
        case 0: g->gain = (const float*)data; break;
        case 1: g->latency = (float*)data; break;
        case 2: g->in[0] = (const float*)data; break;
        case 3: g->in[1] = (const float*)data; break;
        case 4: g->out[0] = (float*)data; break;
        case 5: g->out[1] = (float*)data; break;
    }
}

static void activate(LV2_Handle h) {
    Gain* g = (Gain*)h;
    memset(g->line, 0, sizeof g->line);
    g->pos = 0;
}

static void run(LV2_Handle h, uint32_t n) {
    Gain* g = (Gain*)h;
    const float k = powf(10.0f, *g->gain / 20.0f);
    if (g->latency) *g->latency = DELAY;
    for (uint32_t i = 0; i < n; ++i) {
        for (int c = 0; c < 2; ++c) {
            const float x = g->in[c][i];
            g->out[c][i] = g->line[c][g->pos] * k;
            g->line[c][g->pos] = x;
        }
        g->pos = (g->pos + 1) % DELAY;
    }
}

static void cleanup(LV2_Handle h) { free(h); }

static const LV2_Descriptor descriptor = {"urn:montage:test:lv2-gain", instantiate, connect_port, activate, run, NULL, cleanup, NULL};

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index) { return index == 0 ? &descriptor : NULL; }
