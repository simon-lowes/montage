// CLAP plugins for Montage's tests: a library with two effects.
//   org.montage.test.gain    stereo gain, one parameter (id 7, 0..2, default 1), saves its state, and has
//                            an "editor" that, when shown, asks to be 400x250 and turns the gain to 0.25
//                            (as if the user dragged its knob) so hosts can test the editor protocol
//   org.montage.test.invert  polarity inversion, no parameters
// Built as MontageTestPlugins.clap.
#include <clap/clap.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    clap_plugin_t plugin;
    const clap_host_t* host;
    int invert;
    double gain;
    int guiCreated;
    void* parent;          // the window handle the host gave the editor
    int pendingKnob;       // a knob change waiting for the host's flush
} TestPlugin;

static const char* const kGainFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_UTILITY, NULL};
static const char* const kInvertFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_FILTER, NULL};

static const clap_plugin_descriptor_t kGain = {CLAP_VERSION_INIT, "org.montage.test.gain", "Montage Test Gain",
                                               "Montage", "", "", "", "1.0.0", "Test gain", kGainFeatures};
static const clap_plugin_descriptor_t kInvert = {CLAP_VERSION_INIT, "org.montage.test.invert", "Montage Test Invert",
                                                 "Montage", "", "", "", "1.0.0", "Test inverter", kInvertFeatures};

// ---- audio ports: one stereo in, one stereo out ----
static uint32_t ports_count(const clap_plugin_t* p, bool is_input) { (void)p; (void)is_input; return 1; }
static bool ports_get(const clap_plugin_t* p, uint32_t index, bool is_input, clap_audio_port_info_t* info) {
    (void)p; (void)is_input;
    if (index != 0) return false;
    memset(info, 0, sizeof *info);
    info->id = 0;
    strcpy(info->name, "main");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}
static const clap_plugin_audio_ports_t kPorts = {ports_count, ports_get};

// ---- params (gain only) ----
static void apply_events(TestPlugin* t, const clap_input_events_t* in) {
    uint32_t n = in->size(in);
    for (uint32_t i = 0; i < n; ++i) {
        const clap_event_header_t* h = in->get(in, i);
        if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE) {
            const clap_event_param_value_t* ev = (const clap_event_param_value_t*)h;
            if (ev->param_id == 7) t->gain = ev->value;
        }
    }
}
static uint32_t params_count(const clap_plugin_t* p) { (void)p; return 1; }
static bool params_info(const clap_plugin_t* p, uint32_t index, clap_param_info_t* info) {
    (void)p;
    if (index != 0) return false;
    memset(info, 0, sizeof *info);
    info->id = 7;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    strcpy(info->name, "Gain");
    info->min_value = 0;
    info->max_value = 2;
    info->default_value = 1;
    return true;
}
static bool params_value(const clap_plugin_t* p, clap_id id, double* out) {
    if (id != 7) return false;
    *out = ((TestPlugin*)p->plugin_data)->gain;
    return true;
}
static bool params_to_text(const clap_plugin_t* p, clap_id id, double v, char* buf, uint32_t size) { (void)p; (void)id; (void)v; (void)buf; (void)size; return false; }
static bool params_from_text(const clap_plugin_t* p, clap_id id, const char* s, double* v) { (void)p; (void)id; (void)s; (void)v; return false; }
static void push_event(const clap_output_events_t* out, uint16_t type, double value) {
    if (type == CLAP_EVENT_PARAM_VALUE) {
        clap_event_param_value_t ev;
        memset(&ev, 0, sizeof ev);
        ev.header.size = sizeof ev;
        ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        ev.header.type = type;
        ev.param_id = 7;
        ev.note_id = ev.port_index = ev.channel = ev.key = -1;
        ev.value = value;
        out->try_push(out, &ev.header);
    } else {
        clap_event_param_gesture_t ev;
        memset(&ev, 0, sizeof ev);
        ev.header.size = sizeof ev;
        ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        ev.header.type = type;
        ev.param_id = 7;
        out->try_push(out, &ev.header);
    }
}
static void params_flush(const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t* out) {
    TestPlugin* t = (TestPlugin*)p->plugin_data;
    apply_events(t, in);
    if (t->pendingKnob) {
        t->pendingKnob = 0;
        t->gain = 0.25;
        push_event(out, CLAP_EVENT_PARAM_GESTURE_BEGIN, 0);
        push_event(out, CLAP_EVENT_PARAM_VALUE, 0.25);
        push_event(out, CLAP_EVENT_PARAM_GESTURE_END, 0);
    }
}
static const clap_plugin_params_t kParams = {params_count, params_info, params_value, params_to_text, params_from_text, params_flush};

// ---- state: the gain as 8 bytes ----
static bool state_save(const clap_plugin_t* p, const clap_ostream_t* s) {
    double g = ((TestPlugin*)p->plugin_data)->gain;
    return s->write(s, &g, sizeof g) == (int64_t)sizeof g;
}
static bool state_load(const clap_plugin_t* p, const clap_istream_t* s) {
    double g = 0;
    if (s->read(s, &g, sizeof g) != (int64_t)sizeof g) return false;
    ((TestPlugin*)p->plugin_data)->gain = g;
    return true;
}
static const clap_plugin_state_t kState = {state_save, state_load};

// ---- gui: no real window, just the protocol ----
static bool gui_supported(const clap_plugin_t* p, const char* api, bool floating) { (void)p; (void)api; return !floating; }
static bool gui_preferred(const clap_plugin_t* p, const char** api, bool* floating) { (void)p; (void)api; (void)floating; return false; }
static bool gui_create(const clap_plugin_t* p, const char* api, bool floating) {
    (void)api; (void)floating;
    ((TestPlugin*)p->plugin_data)->guiCreated = 1;
    return true;
}
static void gui_destroy(const clap_plugin_t* p) { ((TestPlugin*)p->plugin_data)->guiCreated = 0; }
static bool gui_set_scale(const clap_plugin_t* p, double s) { (void)p; (void)s; return true; }
static bool gui_get_size(const clap_plugin_t* p, uint32_t* w, uint32_t* h) { (void)p; *w = 320; *h = 200; return true; }
static bool gui_can_resize(const clap_plugin_t* p) { (void)p; return true; }
static bool gui_hints(const clap_plugin_t* p, clap_gui_resize_hints_t* h) { (void)p; (void)h; return false; }
static bool gui_adjust(const clap_plugin_t* p, uint32_t* w, uint32_t* h) { (void)p; (void)w; (void)h; return true; }
static bool gui_set_size(const clap_plugin_t* p, uint32_t w, uint32_t h) { (void)p; (void)w; (void)h; return true; }
static bool gui_set_parent(const clap_plugin_t* p, const clap_window_t* w) {
    TestPlugin* t = (TestPlugin*)p->plugin_data;
    t->parent = w->ptr;
    return t->guiCreated && w->ptr != NULL;
}
static bool gui_set_transient(const clap_plugin_t* p, const clap_window_t* w) { (void)p; (void)w; return false; }
static void gui_suggest_title(const clap_plugin_t* p, const char* title) { (void)p; (void)title; }
static bool gui_show(const clap_plugin_t* p) {
    TestPlugin* t = (TestPlugin*)p->plugin_data;
    const clap_host_gui_t* hg = (const clap_host_gui_t*)t->host->get_extension(t->host, CLAP_EXT_GUI);
    const clap_host_params_t* hp = (const clap_host_params_t*)t->host->get_extension(t->host, CLAP_EXT_PARAMS);
    if (hg) hg->request_resize(t->host, 400, 250);
    t->pendingKnob = 1;
    if (hp) hp->request_flush(t->host);
    return true;
}
static bool gui_hide(const clap_plugin_t* p) { (void)p; return true; }
static const clap_plugin_gui_t kGui = {gui_supported, gui_preferred, gui_create, gui_destroy, gui_set_scale,
                                       gui_get_size, gui_can_resize, gui_hints, gui_adjust, gui_set_size,
                                       gui_set_parent, gui_set_transient, gui_suggest_title, gui_show, gui_hide};

// ---- plugin ----
static bool plugin_init(const clap_plugin_t* p) { (void)p; return true; }
static void plugin_destroy(const clap_plugin_t* p) { free(p->plugin_data); }
static bool plugin_activate(const clap_plugin_t* p, double sr, uint32_t minf, uint32_t maxf) { (void)p; (void)sr; (void)minf; (void)maxf; return true; }
static void plugin_deactivate(const clap_plugin_t* p) { (void)p; }
static bool plugin_start(const clap_plugin_t* p) { (void)p; return true; }
static void plugin_stop(const clap_plugin_t* p) { (void)p; }
static void plugin_reset(const clap_plugin_t* p) { (void)p; }
static clap_process_status plugin_process(const clap_plugin_t* p, const clap_process_t* proc) {
    TestPlugin* t = (TestPlugin*)p->plugin_data;
    apply_events(t, proc->in_events);
    const float factor = t->invert ? -1.0f : (float)t->gain;
    for (uint32_t c = 0; c < 2; ++c) {
        const float* in = proc->audio_inputs[0].data32[c];
        float* out = proc->audio_outputs[0].data32[c];
        for (uint32_t i = 0; i < proc->frames_count; ++i) out[i] = in[i] * factor;
    }
    return CLAP_PROCESS_CONTINUE;
}
static const void* plugin_extension(const clap_plugin_t* p, const char* id) {
    TestPlugin* t = (TestPlugin*)p->plugin_data;
    if (!strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kPorts;
    if (!t->invert && !strcmp(id, CLAP_EXT_PARAMS)) return &kParams;
    if (!t->invert && !strcmp(id, CLAP_EXT_STATE)) return &kState;
    if (!t->invert && !strcmp(id, CLAP_EXT_GUI)) return &kGui;
    return NULL;
}
static void plugin_main_thread(const clap_plugin_t* p) { (void)p; }

static const clap_plugin_t* create(const clap_host_t* host, const clap_plugin_descriptor_t* desc, int invert) {
    TestPlugin* t = (TestPlugin*)calloc(1, sizeof *t);
    t->host = host;
    t->invert = invert;
    t->gain = 1.0;
    t->plugin.desc = desc;
    t->plugin.plugin_data = t;
    t->plugin.init = plugin_init;
    t->plugin.destroy = plugin_destroy;
    t->plugin.activate = plugin_activate;
    t->plugin.deactivate = plugin_deactivate;
    t->plugin.start_processing = plugin_start;
    t->plugin.stop_processing = plugin_stop;
    t->plugin.reset = plugin_reset;
    t->plugin.process = plugin_process;
    t->plugin.get_extension = plugin_extension;
    t->plugin.on_main_thread = plugin_main_thread;
    return &t->plugin;
}

// ---- factory / entry ----
static uint32_t factory_count(const clap_plugin_factory_t* f) { (void)f; return 2; }
static const clap_plugin_descriptor_t* factory_desc(const clap_plugin_factory_t* f, uint32_t i) {
    (void)f;
    return i == 0 ? &kGain : i == 1 ? &kInvert : NULL;
}
static const clap_plugin_t* factory_create(const clap_plugin_factory_t* f, const clap_host_t* host, const char* id) {
    (void)f;
    if (!strcmp(id, kGain.id)) return create(host, &kGain, 0);
    if (!strcmp(id, kInvert.id)) return create(host, &kInvert, 1);
    return NULL;
}
static const clap_plugin_factory_t kFactory = {factory_count, factory_desc, factory_create};

static bool entry_init(const char* path) { (void)path; return true; }
static void entry_deinit(void) {}
static const void* entry_factory(const char* id) { return !strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &kFactory : NULL; }

CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, entry_init, entry_deinit, entry_factory};
