// A VST3 effect for Montage's tests, written directly against the VST 3
// interfaces: a stereo gain with a separate edit controller (the layout most
// commercial plugins use). Parameter 3 "Gain" is normalised 0..1 = gain 0..2,
// default 0.5 (unity). Built as MontageTestVst3.vst3.
#include <atomic>
#include <cstring>

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstunits.h"
#include "pluginterfaces/vst/vstspeaker.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

const FUID kComponentCID(0x4D6F6E74, 0x61676554, 0x65737447, 0x61696E31);
const FUID kControllerCID(0x4D6F6E74, 0x61676554, 0x65737443, 0x74726C31);
constexpr ParamID kGainId = 3;

void copyTitle(String128 dst, const char16_t* src) {
    size_t i = 0;
    for (; src[i] && i < 127; ++i) dst[i] = TChar(src[i]);
    dst[i] = 0;
}

bool readDouble(IBStream* s, double& v) {
    int32 got = 0;
    return s && s->read(&v, sizeof v, &got) == kResultOk && got == int32(sizeof v);
}

class GainComponent : public IComponent, public IAudioProcessor {
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(iid, FUnknown::iid) || FUnknownPrivate::iidEqual(iid, IPluginBase::iid) ||
            FUnknownPrivate::iidEqual(iid, IComponent::iid)) {
            addRef();
            *obj = static_cast<IComponent*>(this);
            return kResultOk;
        }
        if (FUnknownPrivate::iidEqual(iid, IAudioProcessor::iid)) {
            addRef();
            *obj = static_cast<IAudioProcessor*>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return ++refs_; }
    uint32 PLUGIN_API release() override {
        uint32 r = --refs_;
        if (r == 0) delete this;
        return r;
    }

    // IPluginBase
    tresult PLUGIN_API initialize(FUnknown*) override { return kResultOk; }
    tresult PLUGIN_API terminate() override { return kResultOk; }

    // IComponent
    tresult PLUGIN_API getControllerClassId(TUID classId) override {
        kControllerCID.toTUID(classId);
        return kResultOk;
    }
    tresult PLUGIN_API setIoMode(IoMode) override { return kResultOk; }
    int32 PLUGIN_API getBusCount(MediaType type, BusDirection) override { return type == kAudio ? 1 : 0; }
    tresult PLUGIN_API getBusInfo(MediaType type, BusDirection dir, int32 index, BusInfo& bus) override {
        if (type != kAudio || index != 0) return kInvalidArgument;
        bus.mediaType = kAudio;
        bus.direction = dir;
        bus.channelCount = 2;
        copyTitle(bus.name, u"Main");
        bus.busType = kMain;
        bus.flags = BusInfo::kDefaultActive;
        return kResultOk;
    }
    tresult PLUGIN_API getRoutingInfo(RoutingInfo&, RoutingInfo&) override { return kNotImplemented; }
    tresult PLUGIN_API activateBus(MediaType, BusDirection, int32, TBool) override { return kResultOk; }
    tresult PLUGIN_API setActive(TBool) override { return kResultOk; }
    tresult PLUGIN_API setState(IBStream* s) override { return readDouble(s, norm_) ? kResultOk : kResultFalse; }
    tresult PLUGIN_API getState(IBStream* s) override {
        int32 wrote = 0;
        return s->write(&norm_, sizeof norm_, &wrote);
    }

    // IAudioProcessor
    tresult PLUGIN_API setBusArrangements(SpeakerArrangement* in, int32 numIns, SpeakerArrangement* out, int32 numOuts) override {
        return numIns == 1 && numOuts == 1 && in[0] == SpeakerArr::kStereo && out[0] == SpeakerArr::kStereo ? kResultOk
                                                                                                      : kResultFalse;
    }
    tresult PLUGIN_API getBusArrangement(BusDirection, int32, SpeakerArrangement& arr) override {
        arr = SpeakerArr::kStereo;
        return kResultOk;
    }
    tresult PLUGIN_API canProcessSampleSize(int32 size) override { return size == kSample32 ? kResultOk : kResultFalse; }
    uint32 PLUGIN_API getLatencySamples() override { return 0; }
    tresult PLUGIN_API setupProcessing(ProcessSetup&) override { return kResultOk; }
    tresult PLUGIN_API setProcessing(TBool) override { return kResultOk; }
    tresult PLUGIN_API process(ProcessData& data) override {
        if (IParameterChanges* changes = data.inputParameterChanges) {
            for (int32 i = 0; i < changes->getParameterCount(); ++i) {
                IParamValueQueue* q = changes->getParameterData(i);
                if (!q || q->getParameterId() != kGainId || q->getPointCount() <= 0) continue;
                int32 offset = 0;
                ParamValue v = 0;
                if (q->getPoint(q->getPointCount() - 1, offset, v) == kResultOk) norm_ = v;
            }
        }
        if (data.numInputs < 1 || data.numOutputs < 1) return kResultOk;
        const float g = float(norm_ * 2.0);
        for (int32 c = 0; c < 2; ++c) {
            const float* in = data.inputs[0].channelBuffers32[c];
            float* out = data.outputs[0].channelBuffers32[c];
            for (int32 i = 0; i < data.numSamples; ++i) out[i] = in[i] * g;
        }
        return kResultOk;
    }
    uint32 PLUGIN_API getTailSamples() override { return 0; }

private:
    virtual ~GainComponent() = default;
    std::atomic<uint32> refs_{1};
    double norm_ = 0.5;
};

class GainController : public IEditController {
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(iid, FUnknown::iid) || FUnknownPrivate::iidEqual(iid, IPluginBase::iid) ||
            FUnknownPrivate::iidEqual(iid, IEditController::iid)) {
            addRef();
            *obj = static_cast<IEditController*>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return ++refs_; }
    uint32 PLUGIN_API release() override {
        uint32 r = --refs_;
        if (r == 0) delete this;
        return r;
    }
    tresult PLUGIN_API initialize(FUnknown*) override { return kResultOk; }
    tresult PLUGIN_API terminate() override { return kResultOk; }

    tresult PLUGIN_API setComponentState(IBStream* s) override { return readDouble(s, norm_) ? kResultOk : kResultFalse; }
    tresult PLUGIN_API setState(IBStream*) override { return kResultOk; }
    tresult PLUGIN_API getState(IBStream*) override { return kResultOk; }
    int32 PLUGIN_API getParameterCount() override { return 1; }
    tresult PLUGIN_API getParameterInfo(int32 index, ParameterInfo& info) override {
        if (index != 0) return kInvalidArgument;
        info = ParameterInfo{};
        info.id = kGainId;
        copyTitle(info.title, u"Gain");
        copyTitle(info.shortTitle, u"Gain");
        copyTitle(info.units, u"x");
        info.stepCount = 0;
        info.defaultNormalizedValue = 0.5;
        info.unitId = kRootUnitId;
        info.flags = ParameterInfo::kCanAutomate;
        return kResultOk;
    }
    tresult PLUGIN_API getParamStringByValue(ParamID, ParamValue, String128) override { return kNotImplemented; }
    tresult PLUGIN_API getParamValueByString(ParamID, TChar*, ParamValue&) override { return kNotImplemented; }
    ParamValue PLUGIN_API normalizedParamToPlain(ParamID, ParamValue v) override { return v * 2; }
    ParamValue PLUGIN_API plainParamToNormalized(ParamID, ParamValue v) override { return v / 2; }
    ParamValue PLUGIN_API getParamNormalized(ParamID id) override { return id == kGainId ? norm_ : 0; }
    tresult PLUGIN_API setParamNormalized(ParamID id, ParamValue v) override {
        if (id != kGainId) return kInvalidArgument;
        norm_ = v;
        return kResultOk;
    }
    tresult PLUGIN_API setComponentHandler(IComponentHandler*) override { return kResultOk; }
    IPlugView* PLUGIN_API createView(FIDString) override { return nullptr; }

private:
    virtual ~GainController() = default;
    std::atomic<uint32> refs_{1};
    double norm_ = 0.5;
};

class Factory : public IPluginFactory2 {
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(iid, FUnknown::iid) || FUnknownPrivate::iidEqual(iid, IPluginFactory::iid) ||
            FUnknownPrivate::iidEqual(iid, IPluginFactory2::iid)) {
            addRef();
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }  // static object
    uint32 PLUGIN_API release() override { return 1; }

    tresult PLUGIN_API getFactoryInfo(PFactoryInfo* info) override {
        *info = PFactoryInfo("Montage", "https://github.com/simon-lowes/montage", "", PFactoryInfo::kUnicode);
        return kResultOk;
    }
    int32 PLUGIN_API countClasses() override { return 2; }
    tresult PLUGIN_API getClassInfo(int32 index, PClassInfo* info) override {
        PClassInfo2 info2;
        if (getClassInfo2(index, &info2) != kResultOk) return kInvalidArgument;
        *info = PClassInfo(info2.cid, info2.cardinality, info2.category, info2.name);
        return kResultOk;
    }
    tresult PLUGIN_API getClassInfo2(int32 index, PClassInfo2* info) override {
        TUID cid;
        if (index == 0) {
            kComponentCID.toTUID(cid);
            *info = PClassInfo2(cid, PClassInfo::kManyInstances, kVstAudioEffectClass, "Montage Test VST3 Gain", 0,
                                "Fx|Tools", "Montage", "1.0.0", kVstVersionString);
            return kResultOk;
        }
        if (index == 1) {
            kControllerCID.toTUID(cid);
            *info = PClassInfo2(cid, PClassInfo::kManyInstances, kVstComponentControllerClass,
                                "Montage Test VST3 Gain Controller", 0, "", "Montage", "1.0.0", kVstVersionString);
            return kResultOk;
        }
        return kInvalidArgument;
    }
    tresult PLUGIN_API createInstance(FIDString cid, FIDString iid, void** obj) override {
        FUnknown* created = nullptr;
        if (FUID::fromTUID(reinterpret_cast<const char*>(cid)) == kComponentCID)
            created = static_cast<IComponent*>(new GainComponent);
        else if (FUID::fromTUID(reinterpret_cast<const char*>(cid)) == kControllerCID)
            created = new GainController;
        if (!created) {
            *obj = nullptr;
            return kNoInterface;
        }
        tresult r = created->queryInterface(reinterpret_cast<const char*>(iid), obj);
        created->release();
        return r;
    }
};

Factory gFactory;

}  // namespace

extern "C" {
SMTG_EXPORT_SYMBOL IPluginFactory* PLUGIN_API GetPluginFactory() {
    gFactory.addRef();
    return &gFactory;
}
#if defined(_WIN32)
SMTG_EXPORT_SYMBOL bool InitDll() { return true; }
SMTG_EXPORT_SYMBOL bool ExitDll() { return true; }
#elif defined(__APPLE__)
SMTG_EXPORT_SYMBOL bool bundleEntry(void*) { return true; }
SMTG_EXPORT_SYMBOL bool bundleExit() { return true; }
#else
SMTG_EXPORT_SYMBOL bool ModuleEntry(void*) { return true; }
SMTG_EXPORT_SYMBOL bool ModuleExit() { return true; }
#endif
}
