// A CLAP plugin for Montage's delay compensation tests:
//   org.montage.test.delay64   delays stereo audio by 64 samples and reports a latency of 64.
// Built as MontageTestLatency.clap.
#include <clap/clap.h>
#include <stdlib.h>
#include <string.h>

#define DELAY 64

typedef struct {
    clap_plugin_t plugin;
    float line[2][DELAY];
    int pos;
} Delay;

static const char* const kFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_DELAY, NULL};
static const clap_plugin_descriptor_t kDesc = {CLAP_VERSION_INIT, "org.montage.test.delay64", "Montage Test Delay 64",
                                               "Montage", "", "", "", "1.0.0", "Latency test", kFeatures};

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
static uint32_t latency_get(const clap_plugin_t* p) { (void)p; return DELAY; }
static const clap_plugin_latency_t kLatency = {latency_get};

static bool init(const clap_plugin_t* p) { (void)p; return true; }
static void destroy(const clap_plugin_t* p) { free(p->plugin_data); }
static bool activate(const clap_plugin_t* p, double sr, uint32_t a, uint32_t b) { (void)p; (void)sr; (void)a; (void)b; return true; }
static void deactivate(const clap_plugin_t* p) { (void)p; }
static bool start(const clap_plugin_t* p) { (void)p; return true; }
static void stop(const clap_plugin_t* p) { (void)p; }
static void reset(const clap_plugin_t* p) {
    Delay* d = (Delay*)p->plugin_data;
    memset(d->line, 0, sizeof d->line);
    d->pos = 0;
}
static clap_process_status process(const clap_plugin_t* p, const clap_process_t* proc) {
    Delay* d = (Delay*)p->plugin_data;
    for (uint32_t i = 0; i < proc->frames_count; ++i) {
        for (int c = 0; c < 2; ++c) {
            const float x = proc->audio_inputs[0].data32[c][i];
            proc->audio_outputs[0].data32[c][i] = d->line[c][d->pos];
            d->line[c][d->pos] = x;
        }
        d->pos = (d->pos + 1) % DELAY;
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
    Delay* d = (Delay*)calloc(1, sizeof *d);
    d->plugin.desc = &kDesc;
    d->plugin.plugin_data = d;
    d->plugin.init = init;
    d->plugin.destroy = destroy;
    d->plugin.activate = activate;
    d->plugin.deactivate = deactivate;
    d->plugin.start_processing = start;
    d->plugin.stop_processing = stop;
    d->plugin.reset = reset;
    d->plugin.process = process;
    d->plugin.get_extension = extension;
    d->plugin.on_main_thread = main_thread;
    return &d->plugin;
}
static uint32_t count(const clap_plugin_factory_t* f) { (void)f; return 1; }
static const clap_plugin_descriptor_t* desc(const clap_plugin_factory_t* f, uint32_t i) { (void)f; return i == 0 ? &kDesc : NULL; }
static const clap_plugin_factory_t kFactory = {count, desc, create};
static bool entry_init(const char* path) { (void)path; return true; }
static void entry_deinit(void) {}
static const void* entry_factory(const char* id) { return !strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &kFactory : NULL; }
CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, entry_init, entry_deinit, entry_factory};
