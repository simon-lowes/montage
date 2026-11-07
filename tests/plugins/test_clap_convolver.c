// A CLAP plugin for Montage's tests:
//   org.montage.test.convolver   stereo convolution with a fixed 1024-tap impulse (a decaying noise tail after 32
//                                samples of silence), reporting those 32 samples as its latency.
// Built as MontageTestConvolver.clap.
#include <clap/clap.h>
#include <stdlib.h>
#include <string.h>

#define TAPS 1024
#define PREDELAY 32

typedef struct {
    clap_plugin_t plugin;
    float ir[TAPS];
    float hist[2][TAPS];
    int pos;
} Conv;

static const char* const kFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_REVERB, NULL};
static const clap_plugin_descriptor_t kDesc = {CLAP_VERSION_INIT, "org.montage.test.convolver", "Montage Test Convolver",
                                               "Montage", "", "", "", "1.0.0", "Convolution test", kFeatures};

static uint32_t ports_count(const clap_plugin_t* p, bool in) { (void)p; (void)in; return 1; }
static bool ports_get(const clap_plugin_t* p, uint32_t i, bool in, clap_audio_port_info_t* info) {
    (void)p; (void)in;
    if (i != 0) return false;
    memset(info, 0, sizeof *info);
    strcpy(info->name, "main");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}
static const clap_plugin_audio_ports_t kPorts = {ports_count, ports_get};
static uint32_t latency_get(const clap_plugin_t* p) { (void)p; return PREDELAY; }
static const clap_plugin_latency_t kLatency = {latency_get};

static bool init(const clap_plugin_t* p) {
    Conv* c = (Conv*)p->plugin_data;
    unsigned s = 12345u;
    float env = 0.5f;
    for (int k = 0; k < TAPS; ++k) {
        s = s * 1103515245u + 12345u;
        const float noise = (float)((s >> 9) & 0xffff) / 32768.0f - 1.0f;
        c->ir[k] = k < PREDELAY ? 0.0f : noise * env;
        if (k >= PREDELAY) env *= 0.995f;
    }
    c->ir[PREDELAY] = 0.7f;
    return true;
}
static void destroy(const clap_plugin_t* p) { free(p->plugin_data); }
static bool activate(const clap_plugin_t* p, double sr, uint32_t a, uint32_t b) { (void)p; (void)sr; (void)a; (void)b; return true; }
static void deactivate(const clap_plugin_t* p) { (void)p; }
static bool start(const clap_plugin_t* p) { (void)p; return true; }
static void stop(const clap_plugin_t* p) { (void)p; }
static void reset(const clap_plugin_t* p) {
    Conv* c = (Conv*)p->plugin_data;
    memset(c->hist, 0, sizeof c->hist);
    c->pos = 0;
}
static clap_process_status process(const clap_plugin_t* p, const clap_process_t* proc) {
    Conv* c = (Conv*)p->plugin_data;
    for (uint32_t i = 0; i < proc->frames_count; ++i) {
        for (int ch = 0; ch < 2; ++ch) {
            c->hist[ch][c->pos] = proc->audio_inputs[0].data32[ch][i];
            float acc = 0.0f;
            int j = c->pos;
            for (int k = 0; k < TAPS; ++k) {
                acc += c->ir[k] * c->hist[ch][j];
                j = j == 0 ? TAPS - 1 : j - 1;
            }
            proc->audio_outputs[0].data32[ch][i] = acc;
        }
        c->pos = (c->pos + 1) % TAPS;
    }
    return CLAP_PROCESS_CONTINUE;
}
static const void* extension(const clap_plugin_t* p, const char* id) {
    (void)p;
    if (!strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kPorts;
    if (!strcmp(id, CLAP_EXT_LATENCY)) return &kLatency;
    return NULL;
}
static void main_thread(const clap_plugin_t* p) { (void)p; }

static const clap_plugin_t* create(const clap_plugin_factory_t* f, const clap_host_t* host, const char* id) {
    (void)f; (void)host;
    if (strcmp(id, kDesc.id)) return NULL;
    Conv* c = (Conv*)calloc(1, sizeof *c);
    c->plugin.desc = &kDesc;
    c->plugin.plugin_data = c;
    c->plugin.init = init;
    c->plugin.destroy = destroy;
    c->plugin.activate = activate;
    c->plugin.deactivate = deactivate;
    c->plugin.start_processing = start;
    c->plugin.stop_processing = stop;
    c->plugin.reset = reset;
    c->plugin.process = process;
    c->plugin.get_extension = extension;
    c->plugin.on_main_thread = main_thread;
    return &c->plugin;
}
static uint32_t count(const clap_plugin_factory_t* f) { (void)f; return 1; }
static const clap_plugin_descriptor_t* desc(const clap_plugin_factory_t* f, uint32_t i) { (void)f; return i == 0 ? &kDesc : NULL; }
static const clap_plugin_factory_t kFactory = {count, desc, create};
static bool entry_init(const char* path) { (void)path; return true; }
static void entry_deinit(void) {}
static const void* entry_factory(const char* id) { return !strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &kFactory : NULL; }
CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, entry_init, entry_deinit, entry_factory};
