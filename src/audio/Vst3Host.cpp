// VST3 plugin probing and hosting (VST 3 SDK 3.8, MIT; third_party/vst3sdk).
// VST is a registered trademark of Steinberg Media Technologies GmbH.
#include <QString>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>

#include "DynLib.h"
#include "Plugins.h"
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif

#if !defined(__APPLE__) && !defined(_WIN32)
#include <QSocketNotifier>
#include <QTimer>
#endif

namespace Steinberg {
DEF_CLASS_IID(IPlugView)
DEF_CLASS_IID(IPlugFrame)
#if !defined(__APPLE__) && !defined(_WIN32)
namespace Linux {
DEF_CLASS_IID(IRunLoop)
DEF_CLASS_IID(IEventHandler)
DEF_CLASS_IID(ITimerHandler)
}  // namespace Linux
#endif
}  // namespace Steinberg

namespace montage::plugins {

namespace {

using namespace Steinberg;
using namespace Steinberg::Vst;

// FUnknown for objects owned by the instance (reference counts are kept for
// the plugin's sake, but the instance decides when they go).
#define MONTAGE_OWNED_FUNKNOWN                                       \
    uint32 PLUGIN_API addRef() override { return ++refs_; }          \
    uint32 PLUGIN_API release() override { return --refs_; }         \
    std::atomic<uint32> refs_{1};

// Receives edits made in the plugin's editor.
class ComponentHandler : public IComponentHandler {
public:
    EditorListener* listener = nullptr;
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(iid, FUnknown::iid) || FUnknownPrivate::iidEqual(iid, IComponentHandler::iid)) {
            addRef();
            *obj = static_cast<IComponentHandler*>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    MONTAGE_OWNED_FUNKNOWN
    tresult PLUGIN_API beginEdit(ParamID id) override {
        if (listener) listener->editorGesture(id, true);
        return kResultOk;
    }
    tresult PLUGIN_API performEdit(ParamID id, ParamValue v) override {
        if (listener) listener->editorParameter(id, v);
        return kResultOk;
    }
    tresult PLUGIN_API endEdit(ParamID id) override {
        if (listener) listener->editorGesture(id, false);
        return kResultOk;
    }
    tresult PLUGIN_API restartComponent(int32) override { return kResultOk; }
};

#if !defined(__APPLE__) && !defined(_WIN32)
// X11 editors need the host's run loop for their timers and sockets.
class RunLoop : public Linux::IRunLoop {
public:
    ~RunLoop() {
        for (auto& [h, n] : fds_) delete n;
        for (auto& [h, t] : timers_) delete t;
    }
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(iid, FUnknown::iid) || FUnknownPrivate::iidEqual(iid, Linux::IRunLoop::iid)) {
            addRef();
            *obj = static_cast<Linux::IRunLoop*>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    MONTAGE_OWNED_FUNKNOWN
    tresult PLUGIN_API registerEventHandler(Linux::IEventHandler* handler, Linux::FileDescriptor fd) override {
        auto* n = new QSocketNotifier(fd, QSocketNotifier::Read);
        QObject::connect(n, &QSocketNotifier::activated, [handler, fd] { handler->onFDIsSet(fd); });
        fds_.emplace_back(handler, n);
        return kResultOk;
    }
    tresult PLUGIN_API unregisterEventHandler(Linux::IEventHandler* handler) override {
        for (auto it = fds_.begin(); it != fds_.end();)
            if (it->first == handler) {
                delete it->second;
                it = fds_.erase(it);
            } else {
                ++it;
            }
        return kResultOk;
    }
    tresult PLUGIN_API registerTimer(Linux::ITimerHandler* handler, Linux::TimerInterval ms) override {
        auto* t = new QTimer;
        QObject::connect(t, &QTimer::timeout, [handler] { handler->onTimer(); });
        t->start(int(ms));
        timers_.emplace_back(handler, t);
        return kResultOk;
    }
    tresult PLUGIN_API unregisterTimer(Linux::ITimerHandler* handler) override {
        for (auto it = timers_.begin(); it != timers_.end();)
            if (it->first == handler) {
                delete it->second;
                it = timers_.erase(it);
            } else {
                ++it;
            }
        return kResultOk;
    }

private:
    std::vector<std::pair<Linux::IEventHandler*, QSocketNotifier*>> fds_;
    std::vector<std::pair<Linux::ITimerHandler*, QTimer*>> timers_;
};
#endif

// The window the editor lives in, as the plugin sees it.
class PlugFrame : public IPlugFrame {
public:
    EditorListener* listener = nullptr;
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(iid, FUnknown::iid) || FUnknownPrivate::iidEqual(iid, IPlugFrame::iid)) {
            addRef();
            *obj = static_cast<IPlugFrame*>(this);
            return kResultOk;
        }
#if !defined(__APPLE__) && !defined(_WIN32)
        if (FUnknownPrivate::iidEqual(iid, Linux::IRunLoop::iid)) return runLoop_.queryInterface(iid, obj);
#endif
        *obj = nullptr;
        return kNoInterface;
    }
    MONTAGE_OWNED_FUNKNOWN
    tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* r) override {
        if (!view || !r) return kInvalidArgument;
        if (listener) listener->editorResize(r->getWidth(), r->getHeight());
        view->onSize(r);
        return kResultTrue;
    }

private:
#if !defined(__APPLE__) && !defined(_WIN32)
    RunLoop runLoop_;
#endif
};

FIDString viewPlatformType() {
#if defined(__APPLE__)
    return kPlatformTypeNSView;
#elif defined(_WIN32)
    return kPlatformTypeHWND;
#else
    return kPlatformTypeX11EmbedWindowID;
#endif
}

using GetFactoryProc = IPluginFactory*(PLUGIN_API*)();

// A loaded VST3 module: the library, its entry/exit calls and its factory.
// Shared by every instance from the same file.
struct Vst3Module {
    DynLib lib;
    IPtr<IPluginFactory> factory;
    bool (*exitFn)() = nullptr;
#ifdef __APPLE__
    CFBundleRef bundle = nullptr;
#endif

    ~Vst3Module() {
        factory = nullptr;
        if (exitFn) exitFn();
#ifdef __APPLE__
        if (bundle) CFRelease(bundle);
#endif
    }

    bool load(const std::string& path, std::string* error) {
        if (!lib.open(pluginBinary(path), error)) return false;
#if defined(_WIN32)
        if (auto init = reinterpret_cast<bool (*)()>(lib.symbol("InitDll")); init && !init()) {
            if (error) *error = "the plugin's InitDll failed";
            return false;
        }
        exitFn = reinterpret_cast<bool (*)()>(lib.symbol("ExitDll"));
#elif defined(__APPLE__)
        CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(path.c_str()),
                                                               CFIndex(path.size()), true);
        bundle = url ? CFBundleCreate(nullptr, url) : nullptr;
        if (url) CFRelease(url);
        if (auto entry = reinterpret_cast<bool (*)(CFBundleRef)>(lib.symbol("bundleEntry")); entry && !entry(bundle)) {
            if (error) *error = "the plugin's bundleEntry failed";
            return false;
        }
        exitFn = reinterpret_cast<bool (*)()>(lib.symbol("bundleExit"));
#else
        if (auto entry = reinterpret_cast<bool (*)(void*)>(lib.symbol("ModuleEntry")); entry && !entry(lib.handle())) {
            if (error) *error = "the plugin's ModuleEntry failed";
            return false;
        }
        exitFn = reinterpret_cast<bool (*)()>(lib.symbol("ModuleExit"));
#endif
        auto get = reinterpret_cast<GetFactoryProc>(lib.symbol("GetPluginFactory"));
        if (!get) {
            if (error) *error = "not a VST3 plugin (no GetPluginFactory)";
            return false;
        }
        factory = owned(get());
        if (!factory) {
            if (error) *error = "the plugin returned no factory";
            return false;
        }
        return true;
    }
};

std::shared_ptr<Vst3Module> openModule(const std::string& path, std::string* error) {
    static std::mutex m;
    static std::map<std::string, std::weak_ptr<Vst3Module>> open;
    std::lock_guard lock(m);
    if (auto existing = open[path].lock()) return existing;
    auto mod = std::make_shared<Vst3Module>();
    if (!mod->load(path, error)) return nullptr;
    open[path] = mod;
    return mod;
}

std::string fuidString(const TUID cid) {
    char8 buf[64] = {};
    FUID::fromTUID(cid).toString(buf);
    return buf;
}

std::string utf8(const TChar* s) { return QString::fromUtf16(reinterpret_cast<const char16_t*>(s)).toStdString(); }

std::string categoryFromSubCategories(const std::string& subs, bool* instrument) {
    *instrument = subs.find("Instrument") != std::string::npos;
    static const std::pair<const char*, const char*> kMap[] = {
        {"EQ", "EQ"},       {"Filter", "Filter"},   {"Dynamics", "Dynamics"}, {"Reverb", "Reverb"},
        {"Delay", "Delay"}, {"Distortion", "Distortion"}, {"Modulation", "Modulation"}, {"Pitch", "Pitch"},
        {"Restoration", "Restoration"}, {"Analyzer", "Analyzer"}, {"Spatial", "Spatial"}, {"Mastering", "Mastering"},
        {"Tools", "Utility"},
    };
    for (const auto& [word, cat] : kMap)
        if (subs.find(word) != std::string::npos) return cat;
    return {};
}

// ---- Instance ------------------------------------------------------------------

class Vst3Instance final : public Instance {
public:
    ~Vst3Instance() override {
        closeEditor();
        if (controller_) controller_->setComponentHandler(nullptr);
        if (processor_ && processing_) processor_->setProcessing(false);
        if (component_ && active_) component_->setActive(false);
        if (componentCP_ && controllerCP_) {
            componentCP_->disconnect(controllerCP_);
            controllerCP_->disconnect(componentCP_);
        }
        if (controller_ && separateController_) controller_->terminate();
        if (component_) component_->terminate();
        controller_ = nullptr;
        processor_ = nullptr;
        component_ = nullptr;
    }

    bool create(const Descriptor& d, std::string* error) {
        module_ = openModule(d.path, error);
        if (!module_) return false;
        FUID fuid;
        if (!fuid.fromString(d.pluginId.c_str())) {
            if (error) *error = "invalid VST3 class id";
            return false;
        }
        TUID cid;
        fuid.toTUID(cid);
        host_ = owned(new HostApplication());
        IComponent* comp = nullptr;
        if (module_->factory->createInstance(cid, IComponent::iid, reinterpret_cast<void**>(&comp)) != kResultOk || !comp) {
            if (error) *error = "the plugin could not be created";
            return false;
        }
        component_ = owned(comp);
        if (component_->initialize(host_) != kResultOk) {
            if (error) *error = "the plugin failed to initialise";
            return false;
        }
        processor_ = FUnknownPtr<IAudioProcessor>(component_);
        if (!processor_) {
            if (error) *error = "the plugin does not process audio";
            return false;
        }
        // Single-component plugins implement the controller themselves.
        if (FUnknownPtr<IEditController> single(component_); single) {
            controller_ = single;
        } else {
            TUID ccid;
            IEditController* ctrl = nullptr;
            if (component_->getControllerClassId(ccid) == kResultOk &&
                module_->factory->createInstance(ccid, IEditController::iid, reinterpret_cast<void**>(&ctrl)) == kResultOk && ctrl) {
                controller_ = owned(ctrl);
                separateController_ = true;
                if (controller_->initialize(host_) != kResultOk) controller_ = nullptr;
            }
        }
        if (controller_ && separateController_) {
            componentCP_ = FUnknownPtr<IConnectionPoint>(component_);
            controllerCP_ = FUnknownPtr<IConnectionPoint>(controller_);
            if (componentCP_ && controllerCP_) {
                componentCP_->connect(controllerCP_);
                controllerCP_->connect(componentCP_);
            }
            syncControllerToComponent();
        }
        if (processor_->canProcessSampleSize(kSample32) != kResultOk) {
            if (error) *error = "the plugin cannot process 32-bit audio";
            return false;
        }
        setupBuses();
        if (outChannels_ <= 0) {
            if (error) *error = "the plugin has no audio output";
            return false;
        }
        if (controller_) {
            changes_ = owned(new ParameterChanges(controller_->getParameterCount()));
            controller_->setComponentHandler(&handler_);
        }
        else changes_ = owned(new ParameterChanges(0));
        return true;
    }

    bool activate(double sampleRate, int maxFrames) override {
        if (active_) {
            if (processing_) processor_->setProcessing(false);
            component_->setActive(false);
            active_ = processing_ = false;
        }
        maxFrames_ = std::max(1, maxFrames);
        ProcessSetup setup{kRealtime, kSample32, maxFrames_, sampleRate};
        if (processor_->setupProcessing(setup) != kResultOk) return false;
        if (component_->setActive(true) != kResultOk) return false;
        active_ = true;
        processor_->setProcessing(true);  // kNotImplemented is allowed
        processing_ = true;
        allocate();
        return true;
    }

    void process(float* const* channels, int numChannels, int frames) override {
        if (!processing_ || numChannels <= 0) return;
        for (int offset = 0; offset < frames; offset += maxFrames_)
            processBlock(channels, numChannels, offset, std::min(maxFrames_, frames - offset));
    }

    std::vector<ParamInfo> parameters() override {
        std::vector<ParamInfo> out;
        if (!controller_) return out;
        const int32 count = controller_->getParameterCount();
        for (int32 i = 0; i < count; ++i) {
            ParameterInfo info{};
            if (controller_->getParameterInfo(i, info) != kResultOk) continue;
            if (info.flags & (ParameterInfo::kIsReadOnly | ParameterInfo::kIsHidden | ParameterInfo::kIsBypass)) continue;
            ParamInfo p;
            p.id = info.id;
            p.name = utf8(info.title);
            // VST3 parameters are normalised to 0..1 at the API.
            p.min = 0;
            p.max = 1;
            p.def = info.defaultNormalizedValue;
            p.automatable = (info.flags & ParameterInfo::kCanAutomate) != 0;
            p.stepped = info.stepCount > 0;
            out.push_back(p);
        }
        return out;
    }

    bool hasEditor() override {
        if (view_) return true;
        if (!controller_) return false;
        IPtr<IPlugView> v = owned(controller_->createView(ViewType::kEditor));
        return v && v->isPlatformTypeSupported(viewPlatformType()) == kResultTrue;
    }

    bool openEditor(void* parent, EditorListener* listener, int& width, int& height) override {
        if (view_ || !controller_ || !parent) return false;
        view_ = owned(controller_->createView(ViewType::kEditor));
        if (!view_ || view_->isPlatformTypeSupported(viewPlatformType()) != kResultTrue) {
            view_ = nullptr;
            return false;
        }
        handler_.listener = listener;
        frame_.listener = listener;
        view_->setFrame(&frame_);
        if (view_->attached(parent, viewPlatformType()) != kResultOk) {
            view_->setFrame(nullptr);
            view_ = nullptr;
            handler_.listener = frame_.listener = nullptr;
            return false;
        }
        ViewRect r;
        if (view_->getSize(&r) == kResultOk) {
            width = r.getWidth();
            height = r.getHeight();
        }
        return true;
    }

    void closeEditor() override {
        if (!view_) return;
        view_->removed();
        view_->setFrame(nullptr);
        view_ = nullptr;
        handler_.listener = frame_.listener = nullptr;
    }

    bool editorResizable() override { return view_ && view_->canResize() == kResultTrue; }

    void setEditorSize(int width, int height) override {
        if (!view_) return;
        ViewRect r(0, 0, width, height);
        view_->checkSizeConstraint(&r);
        view_->onSize(&r);
    }

    double parameter(uint32_t id) override { return controller_ ? controller_->getParamNormalized(id) : 0; }

    void setParameter(uint32_t id, double value) override {
        if (controller_) controller_->setParamNormalized(id, value);
        int32 index = 0;
        if (IParamValueQueue* q = changes_->addParameterData(id, index)) {
            int32 point = 0;
            q->addPoint(0, value, point);
        }
    }

    // State = component state + controller state, each length-prefixed.
    std::string saveState() override {
        MemoryStream comp, ctrl;
        if (component_->getState(&comp) != kResultOk) return {};
        if (controller_ && separateController_) controller_->getState(&ctrl);
        std::string out = "MVS3";
        appendBlock(out, comp);
        appendBlock(out, ctrl);
        return out;
    }

    bool loadState(const std::string& state) override {
        if (state.size() < 12 || state.compare(0, 4, "MVS3") != 0) return false;
        size_t pos = 4;
        std::string comp, ctrl;
        if (!readBlock(state, pos, comp) || !readBlock(state, pos, ctrl)) return false;
        MemoryStream cs(comp.data(), TSize(comp.size()));
        if (component_->setState(&cs) != kResultOk) return false;
        if (controller_ && separateController_) {
            MemoryStream again(comp.data(), TSize(comp.size()));
            controller_->setComponentState(&again);
            if (!ctrl.empty()) {
                MemoryStream ks(ctrl.data(), TSize(ctrl.size()));
                controller_->setState(&ks);
            }
        }
        return true;
    }

    int latencySamples() override { return processor_ ? int(processor_->getLatencySamples()) : 0; }
    bool hasSidechain() override { return keyBus_ >= 0; }
    void setSidechain(const float* const* channels, int numChannels) override {
        key_ = channels;
        keyIn_ = channels ? numChannels : 0;
    }

    void reset() override {
        if (!active_) return;
        // Deactivating and reactivating is how VST3 hosts flush plugin state.
        if (processing_) processor_->setProcessing(false);
        component_->setActive(false);
        component_->setActive(true);
        processor_->setProcessing(true);
    }

private:
    void syncControllerToComponent() {
        MemoryStream s;
        if (component_->getState(&s) != kResultOk) return;
        int64 pos = 0;
        s.seek(0, IBStream::kIBSeekSet, &pos);
        controller_->setComponentState(&s);
    }

    void setupBuses() {
        const int32 numIn = component_->getBusCount(kAudio, kInput);
        const int32 numOut = component_->getBusCount(kAudio, kOutput);
        // Ask for stereo main buses; keep whatever the plugin insists on otherwise.
        std::vector<SpeakerArrangement> ins(size_t(numIn), SpeakerArr::kStereo), outs(size_t(numOut), SpeakerArr::kStereo);
        for (int32 i = 1; i < numIn; ++i) processor_->getBusArrangement(kInput, i, ins[size_t(i)]);
        for (int32 i = 1; i < numOut; ++i) processor_->getBusArrangement(kOutput, i, outs[size_t(i)]);
        if (processor_->setBusArrangements(ins.data(), numIn, outs.data(), numOut) != kResultOk) {
            for (int32 i = 0; i < numIn; ++i) processor_->getBusArrangement(kInput, i, ins[size_t(i)]);
            for (int32 i = 0; i < numOut; ++i) processor_->getBusArrangement(kOutput, i, outs[size_t(i)]);
        }
        inBusChannels_.clear();
        outBusChannels_.clear();
        for (int32 i = 0; i < numIn; ++i) inBusChannels_.push_back(SpeakerArr::getChannelCount(ins[size_t(i)]));
        for (int32 i = 0; i < numOut; ++i) outBusChannels_.push_back(SpeakerArr::getChannelCount(outs[size_t(i)]));
        // The main buses carry audio, and the first auxiliary input is the key (sidechain); other buses stay inactive.
        if (numIn > 0) component_->activateBus(kAudio, kInput, 0, true);
        if (numOut > 0) component_->activateBus(kAudio, kOutput, 0, true);
        keyBus_ = -1;
        for (int32 i = 1; i < numIn && keyBus_ < 0; ++i) {
            BusInfo info{};
            if (component_->getBusInfo(kAudio, kInput, i, info) == kResultOk && info.busType == kAux && inBusChannels_[size_t(i)] > 0 &&
                component_->activateBus(kAudio, kInput, i, true) == kResultOk)
                keyBus_ = i;
        }
        inChannels_ = numIn > 0 ? inBusChannels_[0] : 0;
        outChannels_ = numOut > 0 ? outBusChannels_[0] : 0;
    }

    void allocate() {
        auto fill = [this](const std::vector<int32>& counts, std::vector<std::vector<float>>& storage,
                           std::vector<std::vector<float*>>& ptrs, std::vector<AudioBusBuffers>& buses) {
            storage.clear();
            ptrs.assign(counts.size(), {});
            buses.assign(counts.size(), AudioBusBuffers{});
            for (size_t b = 0; b < counts.size(); ++b) {
                for (int32 c = 0; c < counts[b]; ++c) {
                    storage.emplace_back(size_t(maxFrames_), 0.0f);
                    ptrs[b].push_back(storage.back().data());
                }
            }
            // Pointers into `storage` are stable now that it is fully built.
            size_t k = 0;
            for (size_t b = 0; b < counts.size(); ++b) {
                for (int32 c = 0; c < counts[b]; ++c) ptrs[b][size_t(c)] = storage[k++].data();
                buses[b].numChannels = counts[b];
                buses[b].channelBuffers32 = ptrs[b].empty() ? nullptr : ptrs[b].data();
            }
        };
        fill(inBusChannels_, inStorage_, inPtrs_, inBuses_);
        fill(outBusChannels_, outStorage_, outPtrs_, outBuses_);
    }

    void processBlock(float* const* channels, int numChannels, int offset, int n) {
        if (keyBus_ >= 0) {
            // The key: what the host gave (mono to both sides), or silence.
            float* const* dst = inBuses_[size_t(keyBus_)].channelBuffers32;
            const int kc = inBusChannels_[size_t(keyBus_)];
            for (int c = 0; c < kc; ++c) {
                if (keyIn_ <= 0) std::fill(dst[c], dst[c] + n, 0.0f);
                else if (kc == 1 && keyIn_ >= 2)
                    for (int i = 0; i < n; ++i) dst[0][i] = 0.5f * (key_[0][offset + i] + key_[1][offset + i]);
                else std::memcpy(dst[c], key_[std::min(c, keyIn_ - 1)] + offset, size_t(n) * sizeof(float));
            }
        }
        if (inChannels_ > 0) {
            float* const* dst = inBuses_[0].channelBuffers32;
            for (int c = 0; c < inChannels_; ++c) {
                if (inChannels_ == 1 && numChannels >= 2) {
                    for (int i = 0; i < n; ++i) dst[0][i] = 0.5f * (channels[0][offset + i] + channels[1][offset + i]);
                } else {
                    std::memcpy(dst[c], channels[std::min(c, numChannels - 1)] + offset, size_t(n) * sizeof(float));
                }
            }
        }
        ProcessData data{};
        data.processMode = kRealtime;
        data.symbolicSampleSize = kSample32;
        data.numSamples = n;
        data.numInputs = int32(inBuses_.size());
        data.numOutputs = int32(outBuses_.size());
        data.inputs = inBuses_.empty() ? nullptr : inBuses_.data();
        data.outputs = outBuses_.data();
        data.inputParameterChanges = changes_;
        const tresult r = processor_->process(data);
        changes_->clearQueue();
        if (r != kResultOk) return;
        float* const* src = outBuses_[0].channelBuffers32;
        for (int c = 0; c < numChannels; ++c)
            std::memcpy(channels[c] + offset, src[std::min(c, outChannels_ - 1)], size_t(n) * sizeof(float));
    }

    static void appendBlock(std::string& out, MemoryStream& s) {
        const uint32_t size = uint32_t(s.getSize());
        out.append(reinterpret_cast<const char*>(&size), 4);
        if (size) out.append(s.getData(), size);
    }

    static bool readBlock(const std::string& in, size_t& pos, std::string& out) {
        if (pos + 4 > in.size()) return false;
        uint32_t size = 0;
        std::memcpy(&size, in.data() + pos, 4);
        pos += 4;
        if (pos + size > in.size()) return false;
        out.assign(in, pos, size);
        pos += size;
        return true;
    }

    ComponentHandler handler_;
    PlugFrame frame_;
    IPtr<IPlugView> view_;
    std::shared_ptr<Vst3Module> module_;
    IPtr<HostApplication> host_;
    IPtr<IComponent> component_;
    IPtr<IAudioProcessor> processor_;
    IPtr<IEditController> controller_;
    IPtr<IConnectionPoint> componentCP_, controllerCP_;
    IPtr<ParameterChanges> changes_;
    bool separateController_ = false;
    bool active_ = false, processing_ = false;
    int maxFrames_ = 1024;
    int inChannels_ = 0, outChannels_ = 0;
    std::vector<int32> inBusChannels_, outBusChannels_;
    int32 keyBus_ = -1;                  // the active auxiliary input (the key), or -1
    const float* const* key_ = nullptr;  // the key for this process() call
    int keyIn_ = 0;
    std::vector<std::vector<float>> inStorage_, outStorage_;
    std::vector<std::vector<float*>> inPtrs_, outPtrs_;
    std::vector<AudioBusBuffers> inBuses_, outBuses_;
};

}  // namespace

std::vector<Descriptor> probeVst3(const std::string& path, std::string* error) {
    std::vector<Descriptor> out;
    auto mod = openModule(path, error);
    if (!mod) return out;
    PFactoryInfo factoryInfo;
    mod->factory->getFactoryInfo(&factoryInfo);
    FUnknownPtr<IPluginFactory2> factory2(mod->factory);
    const int32 count = mod->factory->countClasses();
    for (int32 i = 0; i < count; ++i) {
        PClassInfo info;
        if (mod->factory->getClassInfo(i, &info) != kResultOk) continue;
        if (std::strcmp(info.category, kVstAudioEffectClass) != 0) continue;
        Descriptor d;
        d.format = Format::Vst3;
        d.pluginId = fuidString(info.cid);
        d.id = "vst3:" + d.pluginId;
        d.name = info.name;
        d.vendor = factoryInfo.vendor;
        d.path = path;
        if (factory2) {
            PClassInfo2 info2;
            if (factory2->getClassInfo2(i, &info2) == kResultOk) {
                if (info2.vendor[0]) d.vendor = info2.vendor;
                d.version = info2.version;
                d.category = categoryFromSubCategories(info2.subCategories, &d.instrument);
            }
        }
        out.push_back(d);
    }
    return out;
}

std::unique_ptr<Instance> instantiateVst3(const Descriptor& d, std::string* error) {
    auto inst = std::make_unique<Vst3Instance>();
    if (!inst->create(d, error)) return nullptr;
    return inst;
}

}  // namespace montage::plugins
