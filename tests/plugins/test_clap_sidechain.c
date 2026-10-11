// A CLAP plugin for Montage's tests with a key input: org.montage.test.ducker turns its main input down by what
// arrives on its second input port (the sidechain), sample for sample: out = in * (1 - min(1, |key|)). With no key
// (or silence there) it passes the sound unchanged. Built as MontageTestSidechain.clap.
#include <clap/clap.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char* const kFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_COMPRESSOR, NULL};
static const clap_plugin_descriptor_t kDucker = {CLAP_VERSION_INIT, "org.montage.test.ducker", "Montage Test Ducker",
                                                 "Montage", "", "", "", "1.0.0", "Test ducker with a key input", kFeatures};

// ---- audio ports: main stereo in and out, and a stereo key input ----
static uint32_t ports_count(const clap_plugin_t* p, bool is_input) { (void)p; return is_input ? 2 : 1; }
static bool ports_get(const clap_plugin_t* p, uint32_t index, bool is_input, clap_audio_port_info_t* info) {
    (void)p;
    if (index > (is_input ? 1u : 0u)) return false;
    memset(info, 0, sizeof *info);
    info->id = index;
    strcpy(info->name, index == 0 ? "main" : "sidechain");
    info->flags = index == 0 ? CLAP_AUDIO_PORT_IS_MAIN : 0;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}
static const clap_plugin_audio_ports_t kPorts = {ports_count, ports_get};

static bool plugin_init(const clap_plugin_t* p) { (void)p; return true; }
static void plugin_destroy(const clap_plugin_t* p) { free((void*)p); }
static bool plugin_activate(const clap_plugin_t* p, double sr, uint32_t minf, uint32_t maxf) { (void)p; (void)sr; (void)minf; (void)maxf; return true; }
static void plugin_deactivate(const clap_plugin_t* p) { (void)p; }
static bool plugin_start(const clap_plugin_t* p) { (void)p; return true; }
static void plugin_stop(const clap_plugin_t* p) { (void)p; }
static void plugin_reset(const clap_plugin_t* p) { (void)p; }
static clap_process_status plugin_process(const clap_plugin_t* p, const clap_process_t* proc) {
    (void)p;
    const int keyed = proc->audio_inputs_count >= 2 && proc->audio_inputs[1].channel_count >= 2;
    for (uint32_t c = 0; c < 2; ++c) {
        const float* in = proc->audio_inputs[0].data32[c];
        const float* key = keyed ? proc->audio_inputs[1].data32[c] : NULL;
        float* out = proc->audio_outputs[0].data32[c];
        for (uint32_t i = 0; i < proc->frames_count; ++i) {
            const float k = key ? fminf(1.0f, fabsf(key[i])) : 0.0f;
            out[i] = in[i] * (1.0f - k);
        }
    }
    return CLAP_PROCESS_CONTINUE;
}
static const void* plugin_extension(const clap_plugin_t* p, const char* id) {
    (void)p;
    return !strcmp(id, CLAP_EXT_AUDIO_PORTS) ? &kPorts : NULL;
}
static void plugin_main_thread(const clap_plugin_t* p) { (void)p; }

static const clap_plugin_t* create(const clap_host_t* host) {
    (void)host;
    clap_plugin_t* t = (clap_plugin_t*)calloc(1, sizeof *t);
    t->desc = &kDucker;
    t->init = plugin_init;
    t->destroy = plugin_destroy;
    t->activate = plugin_activate;
    t->deactivate = plugin_deactivate;
    t->start_processing = plugin_start;
    t->stop_processing = plugin_stop;
    t->reset = plugin_reset;
    t->process = plugin_process;
    t->get_extension = plugin_extension;
    t->on_main_thread = plugin_main_thread;
    return t;
}

static uint32_t factory_count(const clap_plugin_factory_t* f) { (void)f; return 1; }
static const clap_plugin_descriptor_t* factory_desc(const clap_plugin_factory_t* f, uint32_t i) { (void)f; return i == 0 ? &kDucker : NULL; }
static const clap_plugin_t* factory_create(const clap_plugin_factory_t* f, const clap_host_t* host, const char* id) {
    (void)f;
    return !strcmp(id, kDucker.id) ? create(host) : NULL;
}
static const clap_plugin_factory_t kFactory = {factory_count, factory_desc, factory_create};

static bool entry_init(const char* path) { (void)path; return true; }
static void entry_deinit(void) {}
static const void* entry_factory(const char* id) { return !strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &kFactory : NULL; }

CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, entry_init, entry_deinit, entry_factory};
