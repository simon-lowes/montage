// CLAP plugin probing and hosting (https://github.com/free-audio/clap, MIT).
#include <clap/clap.h>

#include <QFileInfo>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <utility>

#include "DynLib.h"
#include "Plugins.h"

namespace montage::plugins {

namespace {

// A loaded CLAP library. The entry's init/deinit must run once per load, so
// instances of plugins from the same file share one ClapLibrary.
struct ClapLibrary {
    DynLib lib;
    const clap_plugin_entry_t* entry = nullptr;
    const clap_plugin_factory_t* factory = nullptr;

    ~ClapLibrary() {
        if (entry) entry->deinit();
    }

    bool load(const std::string& path, std::string* error) {
        if (!lib.open(pluginBinary(path), error)) return false;
        auto* e = static_cast<const clap_plugin_entry_t*>(lib.symbol("clap_entry"));
        if (!e) {
            if (error) *error = "not a CLAP plugin (no clap_entry symbol)";
            return false;
        }
        if (!clap_version_is_compatible(e->clap_version)) {
            if (error) *error = "unsupported CLAP version";
            return false;
        }
        if (!e->init(path.c_str())) {
            if (error) *error = "the plugin failed to initialise";
            return false;
        }
        entry = e;
        factory = static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
        if (!factory) {
            if (error) *error = "the plugin has no plugin factory";
            return false;
        }
        return true;
    }
};

std::shared_ptr<ClapLibrary> openLibrary(const std::string& path, std::string* error) {
    static std::mutex m;
    static std::map<std::string, std::weak_ptr<ClapLibrary>> open;
    std::lock_guard lock(m);
    if (auto existing = open[path].lock()) return existing;
    auto lib = std::make_shared<ClapLibrary>();
    if (!lib->load(path, error)) return nullptr;
    open[path] = lib;
    return lib;
}

std::string categoryFromFeatures(const char* const* features, bool* instrument) {
    static const std::pair<const char*, const char*> kMap[] = {
        {CLAP_PLUGIN_FEATURE_EQUALIZER, "EQ"},       {CLAP_PLUGIN_FEATURE_FILTER, "Filter"},
        {CLAP_PLUGIN_FEATURE_COMPRESSOR, "Dynamics"}, {CLAP_PLUGIN_FEATURE_LIMITER, "Dynamics"},
        {CLAP_PLUGIN_FEATURE_GATE, "Dynamics"},      {CLAP_PLUGIN_FEATURE_REVERB, "Reverb"},
        {CLAP_PLUGIN_FEATURE_DELAY, "Delay"},        {CLAP_PLUGIN_FEATURE_DISTORTION, "Distortion"},
        {CLAP_PLUGIN_FEATURE_CHORUS, "Modulation"},  {CLAP_PLUGIN_FEATURE_FLANGER, "Modulation"},
        {CLAP_PLUGIN_FEATURE_PITCH_SHIFTER, "Pitch"}, {CLAP_PLUGIN_FEATURE_RESTORATION, "Restoration"},
        {CLAP_PLUGIN_FEATURE_ANALYZER, "Analyzer"},  {CLAP_PLUGIN_FEATURE_MASTERING, "Mastering"},
        {CLAP_PLUGIN_FEATURE_UTILITY, "Utility"},
    };
    std::string category;
    for (const char* const* f = features; f && *f; ++f) {
        if (std::strcmp(*f, CLAP_PLUGIN_FEATURE_INSTRUMENT) == 0) *instrument = true;
        for (const auto& [feature, name] : kMap)
            if (category.empty() && std::strcmp(*f, feature) == 0) category = name;
    }
    return category;
}

// ---- Host callbacks --------------------------------------------------------

// What the host side of an instance answers (host_data points at one).
struct HostSide {
    std::atomic<bool> flushRequested{false};
    std::atomic<bool> callbackRequested{false};
    EditorListener* listener = nullptr;
};

const clap_host_gui_t kHostGui{
    [](const clap_host_t*) {},  // resize_hints_changed
    [](const clap_host_t* h, uint32_t w, uint32_t hgt) -> bool {
        auto* side = static_cast<HostSide*>(h->host_data);
        if (side->listener) side->listener->editorResize(int(w), int(hgt));
        return true;
    },
    [](const clap_host_t*) -> bool { return true; },  // request_show
    [](const clap_host_t*) -> bool { return true; },  // request_hide
    [](const clap_host_t*, bool) {},                  // closed
};
const clap_host_params_t kHostParams{
    [](const clap_host_t*, clap_param_rescan_flags) {},
    [](const clap_host_t*, clap_id, clap_param_clear_flags) {},
    [](const clap_host_t* h) { static_cast<HostSide*>(h->host_data)->flushRequested = true; },
};

const void* hostGetExtension(const clap_host_t*, const char* id) {
    if (std::strcmp(id, CLAP_EXT_GUI) == 0) return &kHostGui;
    if (std::strcmp(id, CLAP_EXT_PARAMS) == 0) return &kHostParams;
    return nullptr;
}
void hostRequestRestart(const clap_host_t*) {}
void hostRequestProcess(const clap_host_t*) {}
void hostRequestCallback(const clap_host_t* h) { static_cast<HostSide*>(h->host_data)->callbackRequested = true; }

const char* clapWindowApi() {
#if defined(__APPLE__)
    return CLAP_WINDOW_API_COCOA;
#elif defined(_WIN32)
    return CLAP_WINDOW_API_WIN32;
#else
    return CLAP_WINDOW_API_X11;
#endif
}

// ---- Event lists -------------------------------------------------------------

struct EventList {
    std::vector<clap_event_param_value_t> events;
    clap_input_events_t in{this, &size, &get};

    static uint32_t size(const clap_input_events_t* list) {
        return uint32_t(static_cast<const EventList*>(list->ctx)->events.size());
    }
    static const clap_event_header_t* get(const clap_input_events_t* list, uint32_t index) {
        return &static_cast<const EventList*>(list->ctx)->events[index].header;
    }
};

bool discardEvent(const clap_output_events_t*, const clap_event_header_t*) { return true; }
const clap_output_events_t kDiscardEvents{nullptr, &discardEvent};

clap_event_param_value_t paramEvent(uint32_t id, double value) {
    clap_event_param_value_t ev{};
    ev.header.size = sizeof ev;
    ev.header.time = 0;
    ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    ev.header.type = CLAP_EVENT_PARAM_VALUE;
    ev.header.flags = 0;
    ev.param_id = id;
    ev.cookie = nullptr;
    ev.note_id = -1;
    ev.port_index = -1;
    ev.channel = -1;
    ev.key = -1;
    ev.value = value;
    return ev;
}

// ---- Instance ------------------------------------------------------------------

class ClapInstance final : public Instance {
public:
    ~ClapInstance() override {
        if (!plugin_) return;
        closeEditor();
        if (processing_) plugin_->stop_processing(plugin_);
        if (active_) plugin_->deactivate(plugin_);
        plugin_->destroy(plugin_);
    }

    bool create(const Descriptor& d, std::string* error) {
        lib_ = openLibrary(d.path, error);
        if (!lib_) return false;
        host_.clap_version = CLAP_VERSION;
        host_.host_data = &side_;
        host_.name = "Montage";
        host_.vendor = "Montage";
        host_.url = "https://github.com/simon-lowes/montage";
        host_.version = MONTAGE_VERSION;
        host_.get_extension = &hostGetExtension;
        host_.request_restart = &hostRequestRestart;
        host_.request_process = &hostRequestProcess;
        host_.request_callback = &hostRequestCallback;
        plugin_ = lib_->factory->create_plugin(lib_->factory, &host_, d.pluginId.c_str());
        if (!plugin_) {
            if (error) *error = "the plugin could not be created";
            return false;
        }
        if (!plugin_->init(plugin_)) {
            plugin_->destroy(plugin_);
            plugin_ = nullptr;
            if (error) *error = "the plugin failed to initialise";
            return false;
        }
        params_ = static_cast<const clap_plugin_params_t*>(plugin_->get_extension(plugin_, CLAP_EXT_PARAMS));
        state_ = static_cast<const clap_plugin_state_t*>(plugin_->get_extension(plugin_, CLAP_EXT_STATE));
        latency_ = static_cast<const clap_plugin_latency_t*>(plugin_->get_extension(plugin_, CLAP_EXT_LATENCY));
        gui_ = static_cast<const clap_plugin_gui_t*>(plugin_->get_extension(plugin_, CLAP_EXT_GUI));
        if (auto* ports = static_cast<const clap_plugin_audio_ports_t*>(
                plugin_->get_extension(plugin_, CLAP_EXT_AUDIO_PORTS))) {
            // Every port gets buffers (CLAP wants as many as it declares): the main ones carry the sound, a second input
            // port is the key (sidechain), as CLAP's compressors and gates have it, and any others get silence.
            for (int dir = 0; dir < 2; ++dir) {
                auto& list = dir == 0 ? inPorts_ : outPorts_;
                list.clear();
                const uint32_t n = ports->count(plugin_, dir == 0);
                for (uint32_t i = 0; i < n; ++i) {
                    clap_audio_port_info_t info{};
                    list.push_back(ports->get(plugin_, i, dir == 0, &info) ? int(info.channel_count) : 0);
                }
            }
            inChannels_ = inPorts_.empty() ? 0 : inPorts_[0];
            keyChannels_ = inPorts_.size() > 1 ? inPorts_[1] : 0;
            outChannels_ = outPorts_.empty() ? 0 : outPorts_[0];
        }
        if (outChannels_ <= 0) {
            if (error) *error = "the plugin has no audio output";
            return false;
        }
        return true;
    }

    bool activate(double sampleRate, int maxFrames) override {
        if (active_) {
            if (processing_) plugin_->stop_processing(plugin_);
            plugin_->deactivate(plugin_);
            active_ = processing_ = false;
        }
        maxFrames_ = std::max(1, maxFrames);
        if (!plugin_->activate(plugin_, sampleRate, 1, uint32_t(maxFrames_))) return false;
        active_ = true;
        processing_ = plugin_->start_processing(plugin_);
        in_.assign(size_t(std::max(1, inChannels_)) * size_t(maxFrames_), 0.0f);
        keyBuf_.assign(size_t(std::max(0, keyChannels_)) * size_t(maxFrames_), 0.0f);
        // The other ports' buffers.
        int extra = 0;
        for (size_t i = 2; i < inPorts_.size(); ++i) extra += inPorts_[i];
        for (size_t i = 1; i < outPorts_.size(); ++i) extra += outPorts_[i];
        spare_.assign(size_t(extra) * size_t(maxFrames_), 0.0f);
        out_.assign(size_t(outChannels_) * size_t(maxFrames_), 0.0f);
        return processing_;
    }

    bool hasSidechain() override { return keyChannels_ > 0; }
    void setSidechain(const float* const* channels, int numChannels) override {
        key_ = channels;
        keyIn_ = channels ? numChannels : 0;
    }

    void process(float* const* channels, int numChannels, int frames) override {
        if (!processing_ || numChannels <= 0) return;
        for (int offset = 0; offset < frames; offset += maxFrames_) {
            const int n = std::min(maxFrames_, frames - offset);
            processBlock(channels, numChannels, offset, n);
        }
    }

    std::vector<ParamInfo> parameters() override {
        std::vector<ParamInfo> out;
        if (!params_) return out;
        const uint32_t count = params_->count(plugin_);
        for (uint32_t i = 0; i < count; ++i) {
            clap_param_info_t info{};
            if (!params_->get_info(plugin_, i, &info)) continue;
            if (info.flags & (CLAP_PARAM_IS_HIDDEN | CLAP_PARAM_IS_READONLY)) continue;
            ParamInfo p;
            p.id = info.id;
            p.name = info.name;
            p.min = info.min_value;
            p.max = info.max_value;
            p.def = info.default_value;
            p.automatable = (info.flags & CLAP_PARAM_IS_AUTOMATABLE) != 0;
            p.stepped = (info.flags & CLAP_PARAM_IS_STEPPED) != 0;
            out.push_back(p);
        }
        return out;
    }

    double parameter(uint32_t id) override {
        for (auto it = pending_.events.rbegin(); it != pending_.events.rend(); ++it)
            if (it->param_id == id) return it->value;
        double v = 0;
        if (params_) params_->get_value(plugin_, id, &v);
        return v;
    }

    void setParameter(uint32_t id, double value) override {
        if (!params_) return;
        if (processing_) {
            pending_.events.push_back(paramEvent(id, value));
        } else {
            // Not processing: hand the change over directly.
            EventList one;
            one.events.push_back(paramEvent(id, value));
            params_->flush(plugin_, &one.in, &kDiscardEvents);
        }
    }

    std::string saveState() override {
        std::string out;
        if (!state_) return out;
        clap_ostream_t stream{&out, [](const clap_ostream_t* s, const void* buf, uint64_t size) -> int64_t {
                                  static_cast<std::string*>(s->ctx)->append(static_cast<const char*>(buf), size_t(size));
                                  return int64_t(size);
                              }};
        if (!state_->save(plugin_, &stream)) return {};
        return out;
    }

    bool loadState(const std::string& state) override {
        if (!state_) return false;
        struct Reader {
            const std::string* data;
            size_t pos;
        } reader{&state, 0};
        clap_istream_t stream{&reader, [](const clap_istream_t* s, void* buf, uint64_t size) -> int64_t {
                                  auto* r = static_cast<Reader*>(s->ctx);
                                  size_t n = std::min(size_t(size), r->data->size() - r->pos);
                                  std::memcpy(buf, r->data->data() + r->pos, n);
                                  r->pos += n;
                                  return int64_t(n);
                              }};
        return state_->load(plugin_, &stream);
    }

    int latencySamples() override { return latency_ && active_ ? int(latency_->get(plugin_)) : 0; }

    void reset() override {
        if (active_) plugin_->reset(plugin_);
    }

    bool hasEditor() override { return gui_ && gui_->is_api_supported(plugin_, clapWindowApi(), false); }

    bool openEditor(void* parent, EditorListener* listener, int& width, int& height) override {
        if (!hasEditor() || editorOpen_) return false;
        if (!gui_->create(plugin_, clapWindowApi(), false)) return false;
        side_.listener = listener;
        uint32_t w = 400, h = 300;
        gui_->get_size(plugin_, &w, &h);
        clap_window_t win{};
        win.api = clapWindowApi();
#if defined(__APPLE__)
        win.cocoa = parent;
#elif defined(_WIN32)
        win.win32 = parent;
#else
        win.x11 = static_cast<clap_xwnd>(reinterpret_cast<uintptr_t>(parent));
#endif
        if (!gui_->set_parent(plugin_, &win)) {
            gui_->destroy(plugin_);
            side_.listener = nullptr;
            return false;
        }
        editorOpen_ = true;
        gui_->show(plugin_);
        width = int(w);
        height = int(h);
        return true;
    }

    void closeEditor() override {
        if (!editorOpen_) return;
        idle();  // deliver the last changes
        gui_->hide(plugin_);
        gui_->destroy(plugin_);
        editorOpen_ = false;
        side_.listener = nullptr;
    }

    bool editorResizable() override { return editorOpen_ && gui_->can_resize(plugin_); }

    void setEditorSize(int width, int height) override {
        if (!editorOpen_) return;
        uint32_t w = uint32_t(std::max(1, width)), h = uint32_t(std::max(1, height));
        if (gui_->adjust_size(plugin_, &w, &h)) gui_->set_size(plugin_, w, h);
    }

    void idle() override {
        if (side_.callbackRequested.exchange(false)) plugin_->on_main_thread(plugin_);
        // Parameter changes made in the editor arrive through a flush.
        if (!params_ || (!side_.flushRequested.exchange(false) && !editorOpen_)) return;
        EventList none;
        struct Collector {
            ClapInstance* self;
            clap_output_events_t out;
        } collector{this, {nullptr, nullptr}};
        collector.out.ctx = &collector;
        collector.out.try_push = [](const clap_output_events_t* list, const clap_event_header_t* ev) -> bool {
            auto* c = static_cast<Collector*>(list->ctx);
            EditorListener* l = c->self->side_.listener;
            if (!l || ev->space_id != CLAP_CORE_EVENT_SPACE_ID) return true;
            if (ev->type == CLAP_EVENT_PARAM_VALUE) {
                auto* pv = reinterpret_cast<const clap_event_param_value_t*>(ev);
                l->editorParameter(pv->param_id, pv->value);
            } else if (ev->type == CLAP_EVENT_PARAM_GESTURE_BEGIN || ev->type == CLAP_EVENT_PARAM_GESTURE_END) {
                auto* g = reinterpret_cast<const clap_event_param_gesture_t*>(ev);
                l->editorGesture(g->param_id, ev->type == CLAP_EVENT_PARAM_GESTURE_BEGIN);
            }
            return true;
        };
        params_->flush(plugin_, &none.in, &collector.out);
    }

private:
    void processBlock(float* const* channels, int numChannels, int offset, int n) {
        // The plugin's main ports may be mono or stereo; adapt our stereo to them.
        std::vector<float*> inPtrs, outPtrs;
        for (int c = 0; c < std::max(1, inChannels_); ++c) {
            float* dst = &in_[size_t(c) * size_t(maxFrames_)];
            if (inChannels_ == 1 && numChannels >= 2) {
                for (int i = 0; i < n; ++i) dst[i] = 0.5f * (channels[0][offset + i] + channels[1][offset + i]);
            } else {
                std::memcpy(dst, channels[std::min(c, numChannels - 1)] + offset, size_t(n) * sizeof(float));
            }
            inPtrs.push_back(dst);
        }
        for (int c = 0; c < outChannels_; ++c) outPtrs.push_back(&out_[size_t(c) * size_t(maxFrames_)]);

        std::vector<clap_audio_buffer_t> inBufs(std::max<size_t>(1, inPorts_.size())), outBufs(std::max<size_t>(1, outPorts_.size()));
        clap_audio_buffer_t& inBuf = inBufs[0];
        inBuf.data32 = inPtrs.data();
        inBuf.channel_count = uint32_t(inPtrs.size());
        // The key: what the host gave (mono to both sides, stereo to a mono port as its mid), or silence.
        std::vector<float*> keyPtrs;
        for (int c = 0; c < keyChannels_; ++c) {
            float* dst = &keyBuf_[size_t(c) * size_t(maxFrames_)];
            if (keyIn_ <= 0) std::fill(dst, dst + n, 0.0f);
            else if (keyChannels_ == 1 && keyIn_ >= 2)
                for (int i = 0; i < n; ++i) dst[i] = 0.5f * (key_[0][offset + i] + key_[1][offset + i]);
            else std::memcpy(dst, key_[std::min(c, keyIn_ - 1)] + offset, size_t(n) * sizeof(float));
            keyPtrs.push_back(dst);
        }
        if (inBufs.size() > 1) {
            inBufs[1].data32 = keyPtrs.data();
            inBufs[1].channel_count = uint32_t(keyPtrs.size());
        }
        // Any further ports: silence in, and somewhere to write out.
        std::vector<std::vector<float*>> sparePtrs;
        size_t used = 0;
        auto spare = [&](clap_audio_buffer_t& b, int channels, bool silence) {
            sparePtrs.emplace_back();
            for (int c = 0; c < channels; ++c) {
                float* p = &spare_[used];
                used += size_t(maxFrames_);
                if (silence) std::fill(p, p + n, 0.0f);
                sparePtrs.back().push_back(p);
            }
            b.data32 = sparePtrs.back().data();
            b.channel_count = uint32_t(channels);
        };
        sparePtrs.reserve(inPorts_.size() + outPorts_.size());
        for (size_t i = 2; i < inPorts_.size(); ++i) spare(inBufs[i], inPorts_[i], true);
        for (size_t i = 1; i < outPorts_.size(); ++i) spare(outBufs[i], outPorts_[i], false);
        clap_audio_buffer_t& outBuf = outBufs[0];
        outBuf.data32 = outPtrs.data();
        outBuf.channel_count = uint32_t(outPtrs.size());

        clap_process_t proc{};
        proc.steady_time = steadyTime_;
        proc.frames_count = uint32_t(n);
        proc.transport = nullptr;
        proc.audio_inputs = inPorts_.empty() ? nullptr : inBufs.data();
        proc.audio_inputs_count = uint32_t(inPorts_.size());
        proc.audio_outputs = outBufs.data();
        proc.audio_outputs_count = uint32_t(std::max<size_t>(1, outPorts_.size()));
        proc.in_events = &pending_.in;
        proc.out_events = &kDiscardEvents;
        const clap_process_status status = plugin_->process(plugin_, &proc);
        pending_.events.clear();
        steadyTime_ += n;
        if (status == CLAP_PROCESS_ERROR) return;  // leave the audio untouched
        for (int c = 0; c < numChannels; ++c)
            std::memcpy(channels[c] + offset, outPtrs[size_t(std::min(c, outChannels_ - 1))], size_t(n) * sizeof(float));
    }

    std::shared_ptr<ClapLibrary> lib_;
    clap_host_t host_{};
    const clap_plugin_t* plugin_ = nullptr;
    const clap_plugin_params_t* params_ = nullptr;
    const clap_plugin_state_t* state_ = nullptr;
    const clap_plugin_latency_t* latency_ = nullptr;
    const clap_plugin_gui_t* gui_ = nullptr;
    HostSide side_;
    bool editorOpen_ = false;
    int inChannels_ = 2, outChannels_ = 2;
    std::vector<int> inPorts_, outPorts_;  // every port's channels
    std::vector<float> spare_;             // buffers for ports other than the main ones and the key
    int keyChannels_ = 0;           // the key input port's channels (0: none)
    const float* const* key_ = nullptr;  // the key for this process() call
    int keyIn_ = 0;
    std::vector<float> keyBuf_;
    int maxFrames_ = 1024;
    bool active_ = false, processing_ = false;
    int64_t steadyTime_ = 0;
    EventList pending_;
    std::vector<float> in_, out_;
};

}  // namespace

std::vector<Descriptor> probeClap(const std::string& path, std::string* error) {
    std::vector<Descriptor> out;
    auto lib = openLibrary(path, error);
    if (!lib) return out;
    const uint32_t count = lib->factory->get_plugin_count(lib->factory);
    for (uint32_t i = 0; i < count; ++i) {
        const clap_plugin_descriptor_t* desc = lib->factory->get_plugin_descriptor(lib->factory, i);
        if (!desc || !desc->id) continue;
        Descriptor d;
        d.format = Format::Clap;
        d.pluginId = desc->id;
        d.id = "clap:" + d.pluginId;
        d.name = desc->name ? desc->name : desc->id;
        d.vendor = desc->vendor ? desc->vendor : "";
        d.version = desc->version ? desc->version : "";
        d.category = categoryFromFeatures(desc->features, &d.instrument);
        d.path = path;
        out.push_back(d);
    }
    return out;
}

std::unique_ptr<Instance> instantiateClap(const Descriptor& d, std::string* error) {
    auto inst = std::make_unique<ClapInstance>();
    if (!inst->create(d, error)) return nullptr;
    return inst;
}

}  // namespace montage::plugins
