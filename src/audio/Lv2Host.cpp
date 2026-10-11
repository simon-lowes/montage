// LV2 hosting through lilv: the plugin's ports become parameters (control
// inputs), audio is run through its audio ports, settings are saved as LV2
// state, and a lv2:reportsLatency output feeds delay compensation.
#include <lilv/lilv.h>
#include <lv2/atom/atom.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/core/lv2.h>
#include <lv2/options/options.h>
#include <lv2/state/state.h>
#include <lv2/urid/urid.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

#include "Plugins.h"

namespace montage::plugins {

namespace {

// One lilv world for the process. lilv is not thread safe: world access
// (loading bundles, instantiating, state) is serialised; running is not.
struct World {
    LilvWorld* world = lilv_world_new();
    std::mutex m;
    std::set<std::string> bundles;
    std::mutex uridM;
    std::unordered_map<std::string, LV2_URID> ids;
    std::vector<std::string> uris{""};
    LV2_URID_Map map{this, &World::mapUri};
    LV2_URID_Unmap unmap{this, &World::unmapUri};
    LilvNode *audioPort, *controlPort, *cvPort, *atomPort, *inputPort, *outputPort, *optional, *integer, *toggled,
        *enumeration, *sideChain;

    World() {
        audioPort = lilv_new_uri(world, LV2_CORE__AudioPort);
        controlPort = lilv_new_uri(world, LV2_CORE__ControlPort);
        cvPort = lilv_new_uri(world, LV2_CORE__CVPort);
        atomPort = lilv_new_uri(world, LV2_ATOM__AtomPort);
        inputPort = lilv_new_uri(world, LV2_CORE__InputPort);
        outputPort = lilv_new_uri(world, LV2_CORE__OutputPort);
        optional = lilv_new_uri(world, LV2_CORE__connectionOptional);
        integer = lilv_new_uri(world, LV2_CORE__integer);
        toggled = lilv_new_uri(world, LV2_CORE__toggled);
        enumeration = lilv_new_uri(world, LV2_CORE__enumeration);
        sideChain = lilv_new_uri(world, LV2_CORE_PREFIX "isSideChain");
    }
    static LV2_URID mapUri(LV2_URID_Map_Handle h, const char* uri) {
        auto* w = static_cast<World*>(h);
        std::lock_guard lock(w->uridM);
        auto [it, added] = w->ids.emplace(uri, LV2_URID(w->uris.size()));
        if (added) w->uris.emplace_back(uri);
        return it->second;
    }
    static const char* unmapUri(LV2_URID_Unmap_Handle h, LV2_URID id) {
        auto* w = static_cast<World*>(h);
        std::lock_guard lock(w->uridM);
        return id > 0 && id < w->uris.size() ? w->uris[id].c_str() : nullptr;
    }
    LV2_URID urid(const char* uri) { return mapUri(this, uri); }

    // The plugin with this URI, loading its bundle first (it may be outside LV2_PATH).
    const LilvPlugin* plugin(const Descriptor& d) {
        if (!d.path.empty() && bundles.insert(d.path).second) {
            std::string dir = d.path;
            if (dir.back() != '/' && dir.back() != '\\') dir += '/';
            LilvNode* uri = lilv_new_file_uri(world, nullptr, dir.c_str());
            lilv_world_load_bundle(world, uri);
            lilv_node_free(uri);
        }
        LilvNode* uri = lilv_new_uri(world, d.pluginId.c_str());
        const LilvPlugin* p = lilv_plugins_get_by_uri(lilv_world_get_all_plugins(world), uri);
        lilv_node_free(uri);
        return p;
    }
};

World& world() {
    static World* w = new World();  // never freed: plugins may outlive static destruction
    return *w;
}

class Lv2Instance final : public Instance {
public:
    ~Lv2Instance() override { freeInstance(); }

    bool create(const Descriptor& d, std::string* error) {
        World& w = world();
        std::lock_guard lock(w.m);
        plugin_ = w.plugin(d);
        if (!plugin_) {
            if (error) *error = "the LV2 plugin is not installed: " + d.pluginId;
            return false;
        }
        const uint32_t n = lilv_plugin_get_num_ports(plugin_);
        const uint32_t latencyPort = lilv_plugin_has_latency(plugin_) ? lilv_plugin_get_latency_port_index(plugin_) : UINT32_MAX;
        std::vector<float> mins(n), maxs(n), defs(n);
        lilv_plugin_get_port_ranges_float(plugin_, mins.data(), maxs.data(), defs.data());
        for (uint32_t i = 0; i < n; ++i) {
            const LilvPort* port = lilv_plugin_get_port_by_index(plugin_, i);
            Port p;
            p.index = i;
            const bool in = lilv_port_is_a(plugin_, port, w.inputPort);
            if (lilv_port_is_a(plugin_, port, w.audioPort)) p.kind = in ? Port::AudioIn : Port::AudioOut;
            else if (lilv_port_is_a(plugin_, port, w.controlPort)) p.kind = in ? Port::ControlIn : Port::ControlOut;
            else if (lilv_port_is_a(plugin_, port, w.atomPort)) p.kind = in ? Port::AtomIn : Port::AtomOut;
            else if (lilv_port_is_a(plugin_, port, w.cvPort)) p.kind = Port::Cv;
            else p.kind = Port::Other;
            if (LilvNode* name = lilv_port_get_name(plugin_, port)) {
                p.name = lilv_node_as_string(name);
                lilv_node_free(name);
            }
            p.symbol = lilv_node_as_string(lilv_port_get_symbol(plugin_, port));
            p.min = std::isnan(mins[i]) ? 0.0f : mins[i];
            p.max = std::isnan(maxs[i]) ? 1.0f : maxs[i];
            p.def = std::isnan(defs[i]) ? p.min : defs[i];
            p.value = p.def;
            p.optional = lilv_port_has_property(plugin_, port, w.optional);
            p.latency = p.kind == Port::ControlOut && i == latencyPort;
            p.stepped = lilv_port_has_property(plugin_, port, w.integer) || lilv_port_has_property(plugin_, port, w.toggled) ||
                        lilv_port_has_property(plugin_, port, w.enumeration);
            // Audio inputs marked as a side chain are the key, not the signal.
            if (p.kind == Port::AudioIn && lilv_port_has_property(plugin_, port, w.sideChain)) keyIn_.push_back(int(ports_.size()));
            else if (p.kind == Port::AudioIn) audioIn_.push_back(int(ports_.size()));
            if (p.kind == Port::AudioOut) audioOut_.push_back(int(ports_.size()));
            ports_.push_back(p);
        }
        if (audioIn_.empty() || audioOut_.empty()) {
            if (error) *error = "the LV2 plugin is not an audio effect";
            return false;
        }
        return instantiate(48000, 1024, error);
    }

    bool activate(double sampleRate, int maxFrames) override {
        std::lock_guard lock(world().m);
        if (inst_ && sampleRate == sampleRate_ && maxFrames <= maxFrames_) {
            restart();
            return true;
        }
        return instantiate(sampleRate, std::max(1, maxFrames), nullptr);
    }

    void process(float* const* channels, int numChannels, int frames) override {
        if (!inst_ || numChannels <= 0) return;
        for (int offset = 0; offset < frames; offset += maxFrames_) {
            const int n = std::min(maxFrames_, frames - offset);
            const int ins = int(audioIn_.size()), outs = int(audioOut_.size());
            for (int k = 0; k < ins; ++k) {
                float* dst = buffers_[size_t(audioIn_[size_t(k)])].data();
                if (ins == 1 && numChannels >= 2)  // mono effect: the mid
                    for (int i = 0; i < n; ++i) dst[i] = 0.5f * (channels[0][offset + i] + channels[1][offset + i]);
                else if (k < numChannels) std::memcpy(dst, channels[k] + offset, size_t(n) * sizeof(float));
                else std::fill(dst, dst + n, 0.0f);
            }
            // The key: what the host gave (stereo to a mono key as its mid), or silence.
            const int kins = int(keyIn_.size());
            for (int k = 0; k < kins; ++k) {
                float* dst = buffers_[size_t(keyIn_[size_t(k)])].data();
                if (keyChannels_ <= 0) std::fill(dst, dst + n, 0.0f);
                else if (kins == 1 && keyChannels_ >= 2)
                    for (int i = 0; i < n; ++i) dst[i] = 0.5f * (key_[0][offset + i] + key_[1][offset + i]);
                else std::memcpy(dst, key_[std::min(k, keyChannels_ - 1)] + offset, size_t(n) * sizeof(float));
            }
            resetAtoms();
            lilv_instance_run(inst_, uint32_t(n));
            for (int c = 0; c < numChannels; ++c) {
                const float* src = buffers_[size_t(audioOut_[size_t(std::min(c, outs - 1))])].data();
                std::memcpy(channels[c] + offset, src, size_t(n) * sizeof(float));
            }
        }
    }

    bool hasSidechain() override { return !keyIn_.empty(); }
    void setSidechain(const float* const* channels, int numChannels) override {
        key_ = channels;
        keyChannels_ = channels ? numChannels : 0;
    }

    std::vector<ParamInfo> parameters() override {
        std::vector<ParamInfo> out;
        for (const Port& p : ports_)
            if (p.kind == Port::ControlIn) {
                ParamInfo pi;
                pi.id = p.index;
                pi.name = p.name.empty() ? p.symbol : p.name;
                pi.min = p.min;
                pi.max = p.max;
                pi.def = p.def;
                pi.stepped = p.stepped;
                out.push_back(pi);
            }
        return out;
    }

    double parameter(uint32_t id) override { return id < ports_.size() ? ports_[id].value : 0.0; }

    void setParameter(uint32_t id, double value) override {
        if (id < ports_.size() && ports_[id].kind == Port::ControlIn) ports_[id].value = float(value);
    }

    std::string saveState() override {
        if (!inst_) return {};
        World& w = world();
        std::lock_guard lock(w.m);
        LilvState* state = lilv_state_new_from_instance(plugin_, inst_, &w.map, nullptr, nullptr, nullptr, nullptr,
                                                        &Lv2Instance::getPortValue, this,
                                                        LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE, features_.data());
        if (!state) return {};
        char* text = lilv_state_to_string(w.world, &w.map, &w.unmap, state, "urn:montage:state", nullptr);
        std::string out = text ? text : "";
        lilv_free(text);
        lilv_state_free(state);
        return out;
    }

    bool loadState(const std::string& text) override {
        if (!inst_ || text.empty()) return false;
        World& w = world();
        std::lock_guard lock(w.m);
        LilvState* state = lilv_state_new_from_string(w.world, &w.map, text.c_str());
        if (!state) return false;
        lilv_state_restore(state, inst_, &Lv2Instance::setPortValue, this, 0, features_.data());
        lilv_state_free(state);
        return true;
    }

    int latencySamples() override {
        for (const Port& p : ports_)
            if (p.latency) return std::max(0, int(std::lround(p.value)));
        return 0;
    }

    void reset() override {
        std::lock_guard lock(world().m);
        restart();
    }

private:
    struct Port {
        enum Kind { AudioIn, AudioOut, ControlIn, ControlOut, AtomIn, AtomOut, Cv, Other } kind = Other;
        uint32_t index = 0;
        std::string name, symbol;
        float min = 0, max = 1, def = 0, value = 0;
        bool optional = false, latency = false, stepped = false;
    };

    static constexpr uint32_t kAtomCapacity = 8192;

    // Instantiates at this rate and block size, keeping the control values.
    bool instantiate(double sampleRate, int maxFrames, std::string* error) {
        freeInstance();
        World& w = world();
        sampleRate_ = sampleRate;
        maxFrames_ = maxFrames;
        minFrames_ = 1;
        nominal_ = maxFrames;
        options_ = {
            {LV2_OPTIONS_INSTANCE, 0, w.urid(LV2_BUF_SIZE__minBlockLength), sizeof(int32_t), w.urid(LV2_ATOM__Int), &minFrames_},
            {LV2_OPTIONS_INSTANCE, 0, w.urid(LV2_BUF_SIZE__maxBlockLength), sizeof(int32_t), w.urid(LV2_ATOM__Int), &maxFrames_},
            {LV2_OPTIONS_INSTANCE, 0, w.urid(LV2_BUF_SIZE__nominalBlockLength), sizeof(int32_t), w.urid(LV2_ATOM__Int), &nominal_},
            {LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, nullptr}};
        mapFeature_ = {LV2_URID__map, &w.map};
        unmapFeature_ = {LV2_URID__unmap, &w.unmap};
        optionsFeature_ = {LV2_OPTIONS__options, options_.data()};
        boundedFeature_ = {LV2_BUF_SIZE__boundedBlockLength, nullptr};
        features_ = {&mapFeature_, &unmapFeature_, &optionsFeature_, &boundedFeature_, nullptr};
        inst_ = lilv_plugin_instantiate(plugin_, sampleRate, features_.data());
        if (!inst_) {
            if (error) *error = "the LV2 plugin could not be instantiated (it may need a feature Montage does not provide)";
            return false;
        }
        // Every port gets a buffer, as LV2 requires.
        buffers_.assign(ports_.size(), {});
        for (Port& p : ports_) {
            void* where = nullptr;
            switch (p.kind) {
                case Port::ControlIn:
                case Port::ControlOut: where = &p.value; break;
                case Port::AudioIn:
                case Port::AudioOut:
                case Port::Cv:
                    buffers_[p.index].assign(size_t(maxFrames), 0.0f);
                    where = buffers_[p.index].data();
                    break;
                case Port::AtomIn:
                case Port::AtomOut:
                    buffers_[p.index].assign(kAtomCapacity / sizeof(float), 0.0f);
                    where = buffers_[p.index].data();
                    break;
                case Port::Other: break;
            }
            lilv_instance_connect_port(inst_, p.index, where);
        }
        atomSequence_ = w.urid(LV2_ATOM__Sequence);
        atomChunk_ = w.urid(LV2_ATOM__Chunk);
        lilv_instance_activate(inst_);
        active_ = true;
        // Run one silent block so latency outputs are reported, then start afresh.
        for (int k : audioIn_) std::fill(buffers_[size_t(k)].begin(), buffers_[size_t(k)].end(), 0.0f);
        resetAtoms();
        lilv_instance_run(inst_, uint32_t(std::min(64, maxFrames)));
        restart();
        return true;
    }

    void restart() {
        if (!inst_) return;
        if (active_) lilv_instance_deactivate(inst_);
        lilv_instance_activate(inst_);
        active_ = true;
    }

    void resetAtoms() {
        for (const Port& p : ports_) {
            if (p.kind != Port::AtomIn && p.kind != Port::AtomOut) continue;
            auto* seq = reinterpret_cast<LV2_Atom_Sequence*>(buffers_[p.index].data());
            if (p.kind == Port::AtomIn) {
                seq->atom.size = sizeof(LV2_Atom_Sequence_Body);
                seq->atom.type = atomSequence_;
            } else {
                seq->atom.size = kAtomCapacity - sizeof(LV2_Atom);
                seq->atom.type = atomChunk_;
            }
            seq->body.unit = 0;
            seq->body.pad = 0;
        }
    }

    void freeInstance() {
        if (!inst_) return;
        if (active_) lilv_instance_deactivate(inst_);
        lilv_instance_free(inst_);
        inst_ = nullptr;
        active_ = false;
    }

    static const void* getPortValue(const char* symbol, void* self, uint32_t* size, uint32_t* type) {
        auto* me = static_cast<Lv2Instance*>(self);
        for (const Port& p : me->ports_)
            if (p.kind == Port::ControlIn && p.symbol == symbol) {
                *size = sizeof(float);
                *type = world().urid(LV2_ATOM__Float);
                return &p.value;
            }
        *size = *type = 0;
        return nullptr;
    }

    static void setPortValue(const char* symbol, void* self, const void* value, uint32_t size, uint32_t type) {
        auto* me = static_cast<Lv2Instance*>(self);
        World& w = world();
        double v;
        if (type == w.urid(LV2_ATOM__Float) && size == sizeof(float)) v = *static_cast<const float*>(value);
        else if (type == w.urid(LV2_ATOM__Double) && size == sizeof(double)) v = *static_cast<const double*>(value);
        else if (type == w.urid(LV2_ATOM__Int) && size == sizeof(int32_t)) v = *static_cast<const int32_t*>(value);
        else if (type == w.urid(LV2_ATOM__Long) && size == sizeof(int64_t)) v = double(*static_cast<const int64_t*>(value));
        else if (type == w.urid(LV2_ATOM__Bool) && size == sizeof(int32_t)) v = *static_cast<const int32_t*>(value) ? 1 : 0;
        else return;
        for (Port& p : me->ports_)
            if (p.kind == Port::ControlIn && p.symbol == symbol) p.value = float(v);
    }

    const LilvPlugin* plugin_ = nullptr;
    LilvInstance* inst_ = nullptr;
    bool active_ = false;
    std::vector<Port> ports_;
    std::vector<int> audioIn_, audioOut_, keyIn_;
    const float* const* key_ = nullptr;  // the key for this process() call
    int keyChannels_ = 0;
    std::vector<std::vector<float>> buffers_;
    double sampleRate_ = 48000;
    int32_t maxFrames_ = 1024, minFrames_ = 1, nominal_ = 1024;
    LV2_URID atomSequence_ = 0, atomChunk_ = 0;
    std::vector<LV2_Options_Option> options_;
    LV2_Feature mapFeature_{}, unmapFeature_{}, optionsFeature_{}, boundedFeature_{};
    std::vector<const LV2_Feature*> features_;
};

}  // namespace

std::unique_ptr<Instance> instantiateLv2(const Descriptor& d, std::string* error) {
    auto inst = std::make_unique<Lv2Instance>();
    if (!inst->create(d, error)) return nullptr;
    return inst;
}

}  // namespace montage::plugins
