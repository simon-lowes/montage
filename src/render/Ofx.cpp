#include "Ofx.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <map>
#include <thread>
#include <type_traits>
#include <utility>

#include "audio/DynLib.h"
#include "audio/PluginHost.h"
#include "core/Effects.h"
#include "core/Expression.h"
#include "core/ProjectLock.h"
#include "media/SuperScale.h"
#include "render/ColorSpace.h"
#include "render/OfxGl.h"
#include "render/OfxRemote.h"
#include "ofxColour.h"
#include "ofxCore.h"
#include "ofxDrawSuite.h"
#include "ofxGPURender.h"
#include "ofxImageEffect.h"
#include "ofxInteract.h"
#include "ofxKeySyms.h"
#include "ofxOld.h"
#include "ofxMemory.h"
#include "ofxMessage.h"
#include "ofxMultiThread.h"
#include "ofxParam.h"
#include "ofxProgress.h"
#include "ofxProperty.h"
#include "ofxTimeLine.h"

namespace montage::ofx {

namespace {

// ---- Property sets ------------------------------------------------------------------------------------------------

struct Prop {
    char type = 0;  // 'i', 'd', 's', 'p'
    std::vector<int> i;
    std::vector<double> d;
    std::vector<std::string> s;
    std::vector<void*> p;
    int size() const { return type == 'i' ? int(i.size()) : type == 'd' ? int(d.size()) : type == 's' ? int(s.size()) : int(p.size()); }
};

struct PropertySet {
    std::map<std::string, Prop> props;
    std::mutex mutex;
    PropertySet() = default;
    PropertySet(const PropertySet& o) : props(o.props) {}
    PropertySet& operator=(const PropertySet& o) {
        props = o.props;
        return *this;
    }
    void setInt(const char* k, std::vector<int> v) { auto& p = props[k]; p = Prop{}; p.type = 'i'; p.i = std::move(v); }
    void setDouble(const char* k, std::vector<double> v) { auto& p = props[k]; p = Prop{}; p.type = 'd'; p.d = std::move(v); }
    void setString(const char* k, std::vector<std::string> v) { auto& p = props[k]; p = Prop{}; p.type = 's'; p.s = std::move(v); }
    void setPointer(const char* k, void* v) { auto& p = props[k]; p = Prop{}; p.type = 'p'; p.p = {v}; }
    int getInt(const char* k, int def = 0, int index = 0) const {
        auto it = props.find(k);
        return it != props.end() && it->second.type == 'i' && index < int(it->second.i.size()) ? it->second.i[size_t(index)] : def;
    }
    double getDouble(const char* k, double def = 0, int index = 0) const {
        auto it = props.find(k);
        if (it == props.end()) return def;
        if (it->second.type == 'd' && index < int(it->second.d.size())) return it->second.d[size_t(index)];
        if (it->second.type == 'i' && index < int(it->second.i.size())) return it->second.i[size_t(index)];
        return def;
    }
    std::string getString(const char* k, const std::string& def = {}, int index = 0) const {
        auto it = props.find(k);
        return it != props.end() && it->second.type == 's' && index < int(it->second.s.size()) ? it->second.s[size_t(index)] : def;
    }
    std::vector<std::string> strings(const char* k) const {
        auto it = props.find(k);
        return it != props.end() && it->second.type == 's' ? it->second.s : std::vector<std::string>{};
    }
    std::vector<double> doubles(const char* k) const {
        auto it = props.find(k);
        if (it == props.end()) return {};
        if (it->second.type == 'd') return it->second.d;
        std::vector<double> out(it->second.i.begin(), it->second.i.end());
        return out;
    }
};

PropertySet* ps(OfxPropertySetHandle h) { return reinterpret_cast<PropertySet*>(h); }
OfxPropertySetHandle handle(PropertySet* p) { return reinterpret_cast<OfxPropertySetHandle>(p); }

template <typename T>
OfxStatus setOne(OfxPropertySetHandle h, const char* name, int index, char type, const T& v) {
    if (!h || !name || index < 0) return kOfxStatErrBadHandle;
    PropertySet* set = ps(h);
    std::lock_guard<std::mutex> lock(set->mutex);
    Prop& p = set->props[name];
    if (p.type && p.type != type) {
        // A double set where an int was (or the other way round) converts; a string or pointer cannot.
        if (!((p.type == 'i' || p.type == 'd') && (type == 'i' || type == 'd'))) return kOfxStatErrValue;
    }
    if (!p.type) p.type = type;
    if constexpr (std::is_same_v<T, int>) {
        if (p.type == 'd') {
            if (int(p.d.size()) <= index) p.d.resize(size_t(index) + 1);
            p.d[size_t(index)] = v;
        } else {
            if (int(p.i.size()) <= index) p.i.resize(size_t(index) + 1);
            p.i[size_t(index)] = v;
        }
    } else if constexpr (std::is_same_v<T, double>) {
        if (p.type == 'i') {
            if (int(p.i.size()) <= index) p.i.resize(size_t(index) + 1);
            p.i[size_t(index)] = int(v);
        } else {
            if (int(p.d.size()) <= index) p.d.resize(size_t(index) + 1);
            p.d[size_t(index)] = v;
        }
    } else if constexpr (std::is_same_v<T, std::string>) {
        if (int(p.s.size()) <= index) p.s.resize(size_t(index) + 1);
        p.s[size_t(index)] = v;
    } else {
        if (int(p.p.size()) <= index) p.p.resize(size_t(index) + 1);
        p.p[size_t(index)] = v;
    }
    return kOfxStatOK;
}

OfxStatus propSetPointer(OfxPropertySetHandle h, const char* n, int i, void* v) { return setOne(h, n, i, 'p', v); }
OfxStatus propSetString(OfxPropertySetHandle h, const char* n, int i, const char* v) { return setOne(h, n, i, 's', std::string(v ? v : "")); }
OfxStatus propSetDouble(OfxPropertySetHandle h, const char* n, int i, double v) { return setOne(h, n, i, 'd', v); }
OfxStatus propSetInt(OfxPropertySetHandle h, const char* n, int i, int v) { return setOne(h, n, i, 'i', v); }

OfxStatus propSetPointerN(OfxPropertySetHandle h, const char* n, int count, void* const* v) {
    for (int k = 0; k < count; ++k)
        if (OfxStatus st = propSetPointer(h, n, k, v[k]); st != kOfxStatOK) return st;
    return kOfxStatOK;
}
OfxStatus propSetStringN(OfxPropertySetHandle h, const char* n, int count, const char* const* v) {
    if (!h || !n) return kOfxStatErrBadHandle;
    {
        std::lock_guard<std::mutex> lock(ps(h)->mutex);
        Prop& p = ps(h)->props[n];
        if (p.type == 's') p.s.clear();
    }
    for (int k = 0; k < count; ++k)
        if (OfxStatus st = propSetString(h, n, k, v[k]); st != kOfxStatOK) return st;
    return kOfxStatOK;
}
OfxStatus propSetDoubleN(OfxPropertySetHandle h, const char* n, int count, const double* v) {
    for (int k = 0; k < count; ++k)
        if (OfxStatus st = propSetDouble(h, n, k, v[k]); st != kOfxStatOK) return st;
    return kOfxStatOK;
}
OfxStatus propSetIntN(OfxPropertySetHandle h, const char* n, int count, const int* v) {
    for (int k = 0; k < count; ++k)
        if (OfxStatus st = propSetInt(h, n, k, v[k]); st != kOfxStatOK) return st;
    return kOfxStatOK;
}

const Prop* findProp(OfxPropertySetHandle h, const char* n) {
    if (!h || !n) return nullptr;
    auto it = ps(h)->props.find(n);
    return it == ps(h)->props.end() ? nullptr : &it->second;
}

OfxStatus propGetPointer(OfxPropertySetHandle h, const char* n, int i, void** v) {
    if (!h) return kOfxStatErrBadHandle;
    std::lock_guard<std::mutex> lock(ps(h)->mutex);
    const Prop* p = findProp(h, n);
    if (!p) return kOfxStatErrUnknown;
    if (p->type != 'p' || i < 0 || i >= int(p->p.size())) return kOfxStatErrBadIndex;
    *v = p->p[size_t(i)];
    return kOfxStatOK;
}
OfxStatus propGetString(OfxPropertySetHandle h, const char* n, int i, char** v) {
    if (!h) return kOfxStatErrBadHandle;
    std::lock_guard<std::mutex> lock(ps(h)->mutex);
    const Prop* p = findProp(h, n);
    if (!p) return kOfxStatErrUnknown;
    if (p->type != 's' || i < 0 || i >= int(p->s.size())) return kOfxStatErrBadIndex;
    *v = const_cast<char*>(p->s[size_t(i)].c_str());  // lives as long as the property is not set again
    return kOfxStatOK;
}
OfxStatus propGetDouble(OfxPropertySetHandle h, const char* n, int i, double* v) {
    if (!h) return kOfxStatErrBadHandle;
    std::lock_guard<std::mutex> lock(ps(h)->mutex);
    const Prop* p = findProp(h, n);
    if (!p) return kOfxStatErrUnknown;
    if (p->type == 'd' && i >= 0 && i < int(p->d.size())) *v = p->d[size_t(i)];
    else if (p->type == 'i' && i >= 0 && i < int(p->i.size())) *v = p->i[size_t(i)];
    else return kOfxStatErrBadIndex;
    return kOfxStatOK;
}
OfxStatus propGetInt(OfxPropertySetHandle h, const char* n, int i, int* v) {
    if (!h) return kOfxStatErrBadHandle;
    std::lock_guard<std::mutex> lock(ps(h)->mutex);
    const Prop* p = findProp(h, n);
    if (!p) return kOfxStatErrUnknown;
    if (p->type == 'i' && i >= 0 && i < int(p->i.size())) *v = p->i[size_t(i)];
    else if (p->type == 'd' && i >= 0 && i < int(p->d.size())) *v = int(p->d[size_t(i)]);
    else return kOfxStatErrBadIndex;
    return kOfxStatOK;
}
OfxStatus propGetPointerN(OfxPropertySetHandle h, const char* n, int count, void** v) {
    for (int k = 0; k < count; ++k)
        if (OfxStatus st = propGetPointer(h, n, k, v + k); st != kOfxStatOK) return st;
    return kOfxStatOK;
}
OfxStatus propGetStringN(OfxPropertySetHandle h, const char* n, int count, char** v) {
    for (int k = 0; k < count; ++k)
        if (OfxStatus st = propGetString(h, n, k, v + k); st != kOfxStatOK) return st;
    return kOfxStatOK;
}
OfxStatus propGetDoubleN(OfxPropertySetHandle h, const char* n, int count, double* v) {
    for (int k = 0; k < count; ++k)
        if (OfxStatus st = propGetDouble(h, n, k, v + k); st != kOfxStatOK) return st;
    return kOfxStatOK;
}
OfxStatus propGetIntN(OfxPropertySetHandle h, const char* n, int count, int* v) {
    for (int k = 0; k < count; ++k)
        if (OfxStatus st = propGetInt(h, n, k, v + k); st != kOfxStatOK) return st;
    return kOfxStatOK;
}
OfxStatus propReset(OfxPropertySetHandle h, const char* n) {
    if (!h) return kOfxStatErrBadHandle;
    std::lock_guard<std::mutex> lock(ps(h)->mutex);
    return findProp(h, n) ? kOfxStatOK : kOfxStatErrUnknown;
}
OfxStatus propGetDimension(OfxPropertySetHandle h, const char* n, int* count) {
    if (!h) return kOfxStatErrBadHandle;
    std::lock_guard<std::mutex> lock(ps(h)->mutex);
    const Prop* p = findProp(h, n);
    if (!p) return kOfxStatErrUnknown;
    *count = p->size();
    return kOfxStatOK;
}

OfxPropertySuiteV1 gPropertySuite = {propSetPointer, propSetString, propSetDouble, propSetInt, propSetPointerN, propSetStringN,
                                     propSetDoubleN, propSetIntN, propGetPointer, propGetString, propGetDouble, propGetInt,
                                     propGetPointerN, propGetStringN, propGetDoubleN, propGetIntN, propReset, propGetDimension};

// ---- Objects --------------------------------------------------------------------------------------------------------

struct EffectObj;
struct Instance;

struct ParamObj {
    std::string name, type;
    PropertySet props;
    std::vector<double> value;  // set by the plugin or the defaults, used when the effect has no value for it
    std::string text;
    Instance* owner = nullptr;
};

struct ParamSetObj {
    PropertySet props;
    std::vector<std::unique_ptr<ParamObj>> params;
    Instance* owner = nullptr;
    ParamObj* find(const std::string& n) {
        for (auto& p : params)
            if (p->name == n) return p.get();
        return nullptr;
    }
};

struct ClipObj {
    std::string name;
    PropertySet props;
    EffectObj* effect = nullptr;
};

struct ImageObj {
    PropertySet props;
    void* owner = nullptr;  // the instance, for output images
    std::vector<float> f;
    std::vector<uint8_t> b;
};

struct EffectObj {
    PropertySet props;
    ParamSetObj params;
    std::vector<std::unique_ptr<ClipObj>> clips;
    Instance* instance = nullptr;  // set for instances
    ClipObj* clip(const std::string& n) {
        for (auto& c : clips)
            if (c->name == n) return c.get();
        return nullptr;
    }
};

struct Binary {
    std::string path;
    plugins::DynLib lib;
    int (*count)() = nullptr;
    OfxPlugin* (*get)(int) = nullptr;
};

struct Loaded {
    PluginDesc desc;
    OfxPlugin* plugin = nullptr;
    std::unique_ptr<EffectObj> descriptor, filter;  // as described, and described in the context Montage runs it in
    std::string context = kOfxImageEffectContextFilter;
    bool unsafe = false;  // renders one at a time
    std::atomic<const std::atomic<bool>*> holderWaiting{nullptr};  // (that render's thread's gl::waitingFlag)
    OfxPluginEntryPoint* overlay = nullptr;  // its overlay interact's main entry
    int overlayVersion = 0;                  // 1: draws with OpenGL, 2: with the Draw Suite
    std::mutex renderMutex;
};

struct NativeSpace;

// What an instance is rendering now: the frames and the effect's values.
struct Instance {
    Loaded* plugin = nullptr;
    EffectObj obj;
    const Effect* effect = nullptr;
    const Image* source = nullptr;                // the Source clip's picture now (a filter's)
    std::map<std::string, const Image*> images;   // pictures given whole by name (a transition's SourceFrom and SourceTo)
    int width = 0, height = 0;                    // the pictures' size
    double time = 0, scale = 1;
    double progress = -1;                         // a transition's, driving its Transition parameter
    const FrameFetch* fetch = nullptr;
    const NativeSpace* convert = nullptr;         // colourspace pictures fetched now go into (colour management)
    const ColorSpace* space = nullptr;            // ...from this one
    Image* output = nullptr;
    Effect* editing = nullptr;  // where an on-screen control's parameter changes go, while it holds the pen
    bool edited = false;        // ...and whether it made any
    bool gl = false;            // rendering with OpenGL now
    bool glAttached = false;    // kOfxActionOpenGLContextAttached has been sent
    bool glRefused = false;     // it failed kOfxActionOpenGLContextAttached: rendered on the CPU
    gl::Target target;          // the OpenGL render's output
    std::vector<unsigned> textures;  // input textures it has not freed yet
    const InputResolver* inputs = nullptr;      // the compositor's, for a General plugin's further inputs
    std::map<std::string, Image> prefetched;     // ...those connected, made at the render's own frame beforehand
    const std::vector<float>* ownMask = nullptr;  // the effect's own mask, for an input connected to it
    std::string error;
    bool floatImages = true;
    std::string colourStyle;  // the colour management agreed (OpenFX 1.5), empty for none
    bool wrote = false;  // the plugin gave its output image back
    double projectW = 1920, projectH = 1080;  // full-size frame, for parameters given as a fraction of it
    Effect last;  // the effect's values at its last render here, for kOfxActionInstanceChanged
    bool fresh = true;  // not rendered yet
};

std::atomic<int> gInstances{0};
std::atomic<int> gLiveInstances{0};

ParamSetObj* pset(OfxParamSetHandle h) { return reinterpret_cast<ParamSetObj*>(h); }
ParamObj* param(OfxParamHandle h) { return reinterpret_cast<ParamObj*>(h); }
EffectObj* effectObj(OfxImageEffectHandle h) { return reinterpret_cast<EffectObj*>(h); }
ClipObj* clipObj(OfxImageClipHandle h) { return reinterpret_cast<ClipObj*>(h); }

// ---- Parameters -----------------------------------------------------------------------------------------------------

int dimensionsOf(const std::string& type) {
    if (type == kOfxParamTypeRGBA) return 4;
    if (type == kOfxParamTypeRGB || type == kOfxParamTypeDouble3D || type == kOfxParamTypeInteger3D) return 3;
    if (type == kOfxParamTypeDouble2D || type == kOfxParamTypeInteger2D) return 2;
    if (type == kOfxParamTypeDouble || type == kOfxParamTypeInteger || type == kOfxParamTypeBoolean || type == kOfxParamTypeChoice) return 1;
    return 0;
}

// A spatial double whose defaults and ranges are a fraction of the project (kOfxParamCoordinatesNormalised): the axis
// of dimension k (0 width, 1 height), else -1. Montage keeps such values as fractions, so they follow the frame size,
// and hands the plugin canonical coordinates, as the API says values always are.
int normalisedAxis(const PropertySet& props, int k) {
    if (props.getString(kOfxParamPropDefaultCoordinateSystem) != kOfxParamCoordinatesNormalised) return -1;
    const std::string t = props.getString(kOfxParamPropDoubleType);
    if (t == kOfxParamDoubleTypeX || t == kOfxParamDoubleTypeXAbsolute) return 0;
    if (t == kOfxParamDoubleTypeY || t == kOfxParamDoubleTypeYAbsolute) return 1;
    if (t == kOfxParamDoubleTypeXY || t == kOfxParamDoubleTypeXYAbsolute) return k < 2 ? k : -1;
    return -1;
}
bool integerType(const std::string& type) {
    return type == kOfxParamTypeInteger || type == kOfxParamTypeBoolean || type == kOfxParamTypeChoice || type == kOfxParamTypeInteger2D ||
           type == kOfxParamTypeInteger3D;
}
bool stringType(const std::string& type) { return type == kOfxParamTypeString || type == kOfxParamTypeCustom; }

// The effect parameter that holds dimension `k` of a plugin parameter.
std::string effectKey(const std::string& type, const std::string& name, int k) {
    static const char* rgba[] = {".r", ".g", ".b", ".a"};
    static const char* xyz[] = {".x", ".y", ".z"};
    const int n = dimensionsOf(type);
    if (n <= 1) return "param." + name;
    if (type == kOfxParamTypeRGB || type == kOfxParamTypeRGBA) return "param." + name + rgba[k];
    return "param." + name + xyz[k];
}

OfxStatus paramDefine(OfxParamSetHandle h, const char* typeName, const char* name, OfxPropertySetHandle* props) {
    if (!h || !typeName || !name) return kOfxStatErrBadHandle;
    const std::string type = typeName;
    ParamSetObj* set = pset(h);
    if (set->find(name)) return kOfxStatErrExists;
    auto p = std::make_unique<ParamObj>();
    p->name = name;
    p->type = type;
    p->owner = set->owner;
    PropertySet& pr = p->props;
    pr.setString(kOfxPropType, {kOfxTypeParameter});
    pr.setString(kOfxParamPropType, {type});
    pr.setString(kOfxPropName, {name});
    pr.setString(kOfxPropLabel, {name});
    pr.setString(kOfxPropShortLabel, {name});
    pr.setString(kOfxPropLongLabel, {name});
    pr.setString(kOfxParamPropScriptName, {name});
    pr.setString(kOfxParamPropHint, {""});
    pr.setString(kOfxParamPropParent, {""});
    pr.setInt(kOfxParamPropSecret, {0});
    pr.setInt(kOfxParamPropEnabled, {1});
    pr.setInt(kOfxParamPropCanUndo, {1});
    pr.setInt(kOfxParamPropEvaluateOnChange, {1});
    pr.setInt(kOfxParamPropPersistant, {1});
    pr.setString(kOfxParamPropCacheInvalidation, {kOfxParamInvalidateValueChange});
    pr.setPointer(kOfxParamPropDataPtr, nullptr);
    pr.setInt(kOfxParamPropIsAnimating, {0});
    pr.setInt(kOfxParamPropIsAutoKeying, {0});
    const int n = dimensionsOf(type);
    if (n > 0) {
        pr.setInt(kOfxParamPropAnimates, {1});
        if (integerType(type)) {
            pr.setInt(kOfxParamPropDefault, std::vector<int>(size_t(n), 0));
            pr.setInt(kOfxParamPropMin, std::vector<int>(size_t(n), type == kOfxParamTypeChoice || type == kOfxParamTypeBoolean ? 0 : INT_MIN));
            pr.setInt(kOfxParamPropMax, std::vector<int>(size_t(n), type == kOfxParamTypeBoolean ? 1 : INT_MAX));
            pr.setInt(kOfxParamPropDisplayMin, std::vector<int>(size_t(n), 0));
            pr.setInt(kOfxParamPropDisplayMax, std::vector<int>(size_t(n), 100));
        } else {
            const bool colour = type == std::string(kOfxParamTypeRGB) || type == std::string(kOfxParamTypeRGBA);
            pr.setDouble(kOfxParamPropDefault, std::vector<double>(size_t(n), 0.0));
            pr.setDouble(kOfxParamPropMin, std::vector<double>(size_t(n), colour ? 0.0 : -1e300));
            pr.setDouble(kOfxParamPropMax, std::vector<double>(size_t(n), colour ? 1.0 : 1e300));
            pr.setDouble(kOfxParamPropDisplayMin, std::vector<double>(size_t(n), 0.0));
            pr.setDouble(kOfxParamPropDisplayMax, std::vector<double>(size_t(n), 1.0));
            pr.setDouble(kOfxParamPropIncrement, {0.01});
            pr.setInt(kOfxParamPropDigits, {2});
            pr.setString(kOfxParamPropDoubleType, {kOfxParamDoubleTypePlain});
            pr.setString(kOfxParamPropDefaultCoordinateSystem, {kOfxParamCoordinatesCanonical});
        }
        if (type == std::string(kOfxParamTypeChoice)) pr.setString(kOfxParamPropChoiceOption, {});
    } else if (stringType(type)) {
        pr.setString(kOfxParamPropDefault, {""});
        pr.setString(kOfxParamPropStringMode, {kOfxParamStringIsSingleLine});
        pr.setInt(kOfxParamPropAnimates, {0});
    } else if (type == std::string(kOfxParamTypeGroup)) {
        pr.setInt(kOfxParamPropGroupOpen, {1});
    } else if (type == std::string(kOfxParamTypePage)) {
        pr.setString(kOfxParamPropPageChild, {});
    }
    if (props) *props = handle(&p->props);
    set->params.push_back(std::move(p));
    return kOfxStatOK;
}

OfxStatus paramGetHandle(OfxParamSetHandle h, const char* name, OfxParamHandle* out, OfxPropertySetHandle* props) {
    if (!h || !name) return kOfxStatErrBadHandle;
    ParamObj* p = pset(h)->find(name);
    if (!p) return kOfxStatErrUnknown;
    if (out) *out = reinterpret_cast<OfxParamHandle>(p);
    if (props) *props = handle(&p->props);
    return kOfxStatOK;
}
OfxStatus paramSetGetPropertySet(OfxParamSetHandle h, OfxPropertySetHandle* props) {
    if (!h || !props) return kOfxStatErrBadHandle;
    *props = handle(&pset(h)->props);
    return kOfxStatOK;
}
OfxStatus paramGetPropertySet(OfxParamHandle h, OfxPropertySetHandle* props) {
    if (!h || !props) return kOfxStatErrBadHandle;
    *props = handle(&param(h)->props);
    return kOfxStatOK;
}

// The parameter's value at OFX time `t` (clip frames): the effect's keyframed value, else the plugin's own.
double valueAt(const ParamObj& p, int k, double t) {
    // A transition's Transition parameter is its progress, set by Montage.
    if (p.owner && p.owner->progress >= 0 && p.name == kOfxImageEffectTransitionParamName) return p.owner->progress;
    const int axis = normalisedAxis(p.props, k);
    const double size = axis < 0 || !p.owner ? 1.0 : axis == 0 ? p.owner->projectW : p.owner->projectH;
    if (p.owner && p.owner->effect) {
        auto it = p.owner->effect->params.find(effectKey(p.type, p.name, k));
        if (it != p.owner->effect->params.end()) return it->second.at(FrameTime(std::floor(t + 1e-6))) * size;
    }
    if (k < int(p.value.size())) return p.value[size_t(k)];  // as the plugin set it: canonical already
    return p.props.getDouble(kOfxParamPropDefault, 0, k) * size;
}
std::string textOf(const ParamObj& p) {
    if (p.owner && p.owner->effect) {
        auto it = p.owner->effect->strings.find("str." + p.name);
        if (it != p.owner->effect->strings.end()) return it->second;
    }
    return p.text.empty() ? p.props.getString(kOfxParamPropDefault) : p.text;
}
double ownerTime(const ParamObj& p) { return p.owner ? p.owner->time : 0; }

OfxStatus getValues(ParamObj* p, double t, va_list ap) {
    const int n = dimensionsOf(p->type);
    if (stringType(p->type)) {
        // The pointer stays valid until the next call for this parameter.
        static thread_local std::map<const ParamObj*, std::string> held;
        std::string& s = held[p];
        s = textOf(*p);
        *va_arg(ap, char**) = const_cast<char*>(s.c_str());
        return kOfxStatOK;
    }
    if (n == 0) return kOfxStatErrUnsupported;
    for (int k = 0; k < n; ++k) {
        const double v = valueAt(*p, k, t);
        if (integerType(p->type)) *va_arg(ap, int*) = int(std::lround(v));
        else *va_arg(ap, double*) = v;
    }
    return kOfxStatOK;
}

OfxStatus paramGetValue(OfxParamHandle h, ...) {
    if (!h) return kOfxStatErrBadHandle;
    va_list ap;
    va_start(ap, h);
    const OfxStatus st = getValues(param(h), ownerTime(*param(h)), ap);
    va_end(ap);
    return st;
}
OfxStatus paramGetValueAtTime(OfxParamHandle h, OfxTime time, ...) {
    if (!h) return kOfxStatErrBadHandle;
    va_list ap;
    va_start(ap, time);
    const OfxStatus st = getValues(param(h), time, ap);
    va_end(ap);
    return st;
}
OfxStatus paramGetDerivative(OfxParamHandle h, OfxTime time, ...) {
    if (!h) return kOfxStatErrBadHandle;
    ParamObj* p = param(h);
    va_list ap;
    va_start(ap, time);
    const int n = dimensionsOf(p->type);
    for (int k = 0; k < n && !integerType(p->type); ++k) *va_arg(ap, double*) = valueAt(*p, k, time + 1) - valueAt(*p, k, time);
    va_end(ap);
    return n && !integerType(p->type) ? kOfxStatOK : kOfxStatErrUnsupported;
}
OfxStatus paramGetIntegral(OfxParamHandle h, OfxTime t1, OfxTime t2, ...) {
    if (!h) return kOfxStatErrBadHandle;
    ParamObj* p = param(h);
    va_list ap;
    va_start(ap, t2);
    const int n = dimensionsOf(p->type);
    for (int k = 0; k < n && !integerType(p->type); ++k) {
        double sum = 0;
        for (double t = t1; t < t2; t += 1) sum += valueAt(*p, k, t) * std::min(1.0, t2 - t);
        *va_arg(ap, double*) = sum;
    }
    va_end(ap);
    return n && !integerType(p->type) ? kOfxStatOK : kOfxStatErrUnsupported;
}
// A value the plug-in sets: while an on-screen control holds the pen, into the effect being edited (keyed at `t` where
// the parameter is animated, as the Inspector keys one), else its own.
OfxStatus setValues(ParamObj* p, double t, va_list ap) {
    const int n = dimensionsOf(p->type);
    Effect* editing = p->owner ? p->owner->editing : nullptr;
    if (stringType(p->type)) {
        const char* s = va_arg(ap, const char*);
        auto it = editing ? editing->strings.find("str." + p->name) : std::map<std::string, std::string>::iterator{};
        if (editing && it != editing->strings.end()) {
            if (it->second != (s ? s : "")) p->owner->edited = true;
            it->second = s ? s : "";
        } else {
            p->text = s ? s : "";
        }
        return kOfxStatOK;
    }
    if (n == 0) return kOfxStatOK;  // a push button "set" is nothing
    std::vector<double> v(static_cast<size_t>(n));
    for (int k = 0; k < n; ++k) v[size_t(k)] = integerType(p->type) ? double(va_arg(ap, int)) : va_arg(ap, double);
    bool kept = false;
    if (editing) {
        const FrameTime at = FrameTime(std::floor(t + 1e-6));
        for (int k = 0; k < n; ++k) {
            auto it = editing->params.find(effectKey(p->type, p->name, k));
            if (it == editing->params.end()) continue;
            const int axis = normalisedAxis(p->props, k);
            const double size = axis < 0 ? 1.0 : axis == 0 ? p->owner->projectW : p->owner->projectH;
            const double value = v[size_t(k)] / std::max(1e-9, size);
            if (std::fabs(it->second.at(at) - value) > 1e-12) p->owner->edited = true;
            it->second.set(at, value);
            kept = true;
        }
    }
    if (!kept) p->value = std::move(v);
    return kOfxStatOK;
}
OfxStatus paramSetValue(OfxParamHandle h, ...) {
    if (!h) return kOfxStatErrBadHandle;
    va_list ap;
    va_start(ap, h);
    const OfxStatus st = setValues(param(h), ownerTime(*param(h)), ap);
    va_end(ap);
    return st;
}
OfxStatus paramSetValueAtTime(OfxParamHandle h, OfxTime time, ...) {
    if (!h) return kOfxStatErrBadHandle;
    va_list ap;
    va_start(ap, time);
    const OfxStatus st = setValues(param(h), time, ap);
    va_end(ap);
    return st;
}
const Param* keyedParam(const ParamObj* p) {
    if (!p->owner || !p->owner->effect) return nullptr;
    auto it = p->owner->effect->params.find(effectKey(p->type, p->name, 0));
    return it == p->owner->effect->params.end() || it->second.keys.empty() ? nullptr : &it->second;
}
OfxStatus paramGetNumKeys(OfxParamHandle h, unsigned int* n) {
    if (!h || !n) return kOfxStatErrBadHandle;
    const Param* k = keyedParam(param(h));
    *n = k ? unsigned(k->keys.size()) : 0;
    return kOfxStatOK;
}
OfxStatus paramGetKeyTime(OfxParamHandle h, unsigned int nth, OfxTime* t) {
    if (!h || !t) return kOfxStatErrBadHandle;
    const Param* k = keyedParam(param(h));
    if (!k || nth >= k->keys.size()) return kOfxStatErrBadIndex;
    *t = double(k->keys[nth].t);
    return kOfxStatOK;
}
OfxStatus paramGetKeyIndex(OfxParamHandle h, OfxTime t, int direction, int* index) {
    if (!h || !index) return kOfxStatErrBadHandle;
    const Param* k = keyedParam(param(h));
    if (!k) return kOfxStatFailed;
    for (int i = 0; i < int(k->keys.size()); ++i) {
        const double kt = double(k->keys[size_t(i)].t);
        if ((direction == 0 && std::fabs(kt - t) < 1e-6) || (direction > 0 && kt > t + 1e-6)) {
            *index = i;
            return kOfxStatOK;
        }
    }
    if (direction < 0)
        for (int i = int(k->keys.size()) - 1; i >= 0; --i)
            if (double(k->keys[size_t(i)].t) < t - 1e-6) {
                *index = i;
                return kOfxStatOK;
            }
    return kOfxStatFailed;
}
OfxStatus paramDeleteKey(OfxParamHandle, OfxTime) { return kOfxStatOK; }
OfxStatus paramDeleteAllKeys(OfxParamHandle) { return kOfxStatOK; }
OfxStatus paramCopy(OfxParamHandle, OfxParamHandle, OfxTime, const OfxRangeD*) { return kOfxStatErrUnsupported; }
OfxStatus paramEditBegin(OfxParamSetHandle, const char*) { return kOfxStatOK; }
OfxStatus paramEditEnd(OfxParamSetHandle) { return kOfxStatOK; }

OfxParameterSuiteV1 gParameterSuite = {paramDefine, paramGetHandle, paramSetGetPropertySet, paramGetPropertySet, paramGetValue,
                                       paramGetValueAtTime, paramGetDerivative, paramGetIntegral, paramSetValue, paramSetValueAtTime,
                                       paramGetNumKeys, paramGetKeyTime, paramGetKeyIndex, paramDeleteKey, paramDeleteAllKeys,
                                       paramCopy, paramEditBegin, paramEditEnd};

// ---- Image effect suite ---------------------------------------------------------------------------------------------

OfxStatus getPropertySet(OfxImageEffectHandle h, OfxPropertySetHandle* props) {
    if (!h || !props) return kOfxStatErrBadHandle;
    *props = handle(&effectObj(h)->props);
    return kOfxStatOK;
}
OfxStatus getParamSet(OfxImageEffectHandle h, OfxParamSetHandle* set) {
    if (!h || !set) return kOfxStatErrBadHandle;
    *set = reinterpret_cast<OfxParamSetHandle>(&effectObj(h)->params);
    return kOfxStatOK;
}
OfxStatus clipDefine(OfxImageEffectHandle h, const char* name, OfxPropertySetHandle* props) {
    if (!h || !name) return kOfxStatErrBadHandle;
    EffectObj* e = effectObj(h);
    ClipObj* c = e->clip(name);
    if (!c) {
        auto clip = std::make_unique<ClipObj>();
        clip->name = name;
        clip->effect = e;
        PropertySet& pr = clip->props;
        pr.setString(kOfxPropType, {kOfxTypeClip});
        pr.setString(kOfxPropName, {name});
        pr.setString(kOfxPropLabel, {name});
        pr.setString(kOfxImageEffectPropSupportedComponents, {});
        pr.setInt(kOfxImageEffectPropTemporalClipAccess, {0});
        pr.setInt(kOfxImageClipPropOptional, {0});
        pr.setInt(kOfxImageClipPropIsMask, {0});
        pr.setString(kOfxImageClipPropFieldExtraction, {kOfxImageFieldDoubled});
        pr.setInt(kOfxImageEffectPropSupportsTiles, {1});
        c = clip.get();
        e->clips.push_back(std::move(clip));
    }
    if (props) *props = handle(&c->props);
    return kOfxStatOK;
}
OfxStatus clipGetHandle(OfxImageEffectHandle h, const char* name, OfxImageClipHandle* clip, OfxPropertySetHandle* props) {
    if (!h || !name) return kOfxStatErrBadHandle;
    ClipObj* c = effectObj(h)->clip(name);
    if (!c) return kOfxStatErrUnknown;
    if (clip) *clip = reinterpret_cast<OfxImageClipHandle>(c);
    if (props) *props = handle(&c->props);
    return kOfxStatOK;
}
OfxStatus clipGetPropertySet(OfxImageClipHandle h, OfxPropertySetHandle* props) {
    if (!h || !props) return kOfxStatErrBadHandle;
    *props = handle(&clipObj(h)->props);
    return kOfxStatOK;
}

std::atomic<uint64_t> gImageSerial{0};

// kOfxImagePropUniqueIdentifier: a source picture is named by what it holds (its size and a hash of its pixels), so a
// plugin that caches analysis per image finds it again for the same picture and not for another; outputs are each new.
std::string imageIdentifier(const Image& from, bool isOutput) {
    if (isOutput) return "out:" + std::to_string(++gImageSerial);
    uint64_t h = 1469598103934665603ull ^ (uint64_t(uint32_t(from.width)) << 32 | uint32_t(from.height));
    const size_t words = from.px.size() / 2;
    const auto* data = reinterpret_cast<const unsigned char*>(from.px.data());
    for (size_t i = 0; i < words; i += 7) {  // every seventh pair of floats: quick, and touches every row
        uint64_t v;
        std::memcpy(&v, data + i * 8, 8);
        h = (h ^ v) * 0x100000001b3ull;
        h ^= h >> 29;
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, "src:%016llx", static_cast<unsigned long long>(h));
    return buf;
}

// An image for the plugin, bottom row first as OFX counts, from a premultiplied float image (an output: `w` x `h`,
// to be written).
ImageObj* makeImage(const Instance& inst, const Image& from, bool isOutput, int outW = 0, int outH = 0) {
    std::unique_ptr<ImageObj> owned(new ImageObj);  // (freed if a picture too large throws)
    ImageObj* img = owned.get();
    const int w = isOutput ? outW : from.width, h = isOutput ? outH : from.height;
    const size_t n = size_t(w) * size_t(h) * 4;
    void* data = nullptr;
    int rowBytes = 0;
    if (inst.floatImages) {
        img->f.resize(n);
        if (!isOutput)
            for (int y = 0; y < h; ++y) std::memcpy(img->f.data() + size_t(y) * size_t(w) * 4, from.row(h - 1 - y), size_t(w) * 4 * sizeof(float));
        data = img->f.data();
        rowBytes = w * 4 * int(sizeof(float));
    } else {
        img->b.resize(n);
        if (!isOutput)
            for (int y = 0; y < h; ++y) {
                const float* s = from.row(h - 1 - y);
                uint8_t* d = img->b.data() + size_t(y) * size_t(w) * 4;
                for (int x = 0; x < w * 4; ++x) d[x] = uint8_t(std::clamp(s[x], 0.0f, 1.0f) * 255.0f + 0.5f);
            }
        data = img->b.data();
        rowBytes = w * 4;
    }
    PropertySet& p = img->props;
    p.setString(kOfxPropType, {kOfxTypeImage});
    p.setPointer(kOfxImagePropData, data);
    p.setInt(kOfxImagePropBounds, {0, 0, w, h});
    p.setInt(kOfxImagePropRegionOfDefinition, {0, 0, w, h});
    p.setInt(kOfxImagePropRowBytes, {rowBytes});
    p.setString(kOfxImageEffectPropPixelDepth, {inst.floatImages ? kOfxBitDepthFloat : kOfxBitDepthByte});
    p.setString(kOfxImageEffectPropComponents, {kOfxImageComponentRGBA});
    p.setString(kOfxImageEffectPropPreMultiplication, {kOfxImagePreMultiplied});
    p.setDouble(kOfxImageEffectPropRenderScale, {inst.scale, inst.scale});
    p.setDouble(kOfxImagePropPixelAspectRatio, {1.0});
    p.setString(kOfxImagePropField, {kOfxImageFieldNone});
    p.setString(kOfxImagePropUniqueIdentifier, {imageIdentifier(from, isOutput)});
    p.setPointer("montage.image", img);  // found again on release
    return owned.release();
}

// The picture input clip `name` holds at `time` (null when it has none: an input connected to nothing), sized as the
// render's pictures; `scratch` holds it when it is made here. (Defined after colour management.)
const Image* inputPicture(Instance& inst, const std::string& name, double time, Image& scratch);

OfxStatus clipGetImageUnguarded(OfxImageClipHandle h, OfxTime time, OfxPropertySetHandle* imageHandle) {
    if (!h || !imageHandle) return kOfxStatErrBadHandle;
    ClipObj* c = clipObj(h);
    Instance* inst = c->effect ? c->effect->instance : nullptr;
    if (!inst || inst->width <= 0 || inst->height <= 0) return kOfxStatFailed;
    ImageObj* img = nullptr;
    if (c->name == kOfxImageEffectOutputClipName) {
        if (!inst->output) return kOfxStatFailed;
        img = makeImage(*inst, Image(), true, inst->width, inst->height);
        // Where the plugin writes: kept so render() can read it back.
        inst->output->width = inst->width;
        inst->output->height = inst->height;
        img->props.setPointer("montage.output", inst->output);
        img->owner = inst;
    } else {
        Image scratch;
        const Image* pic = inputPicture(*inst, c->name, time, scratch);
        if (!pic) return kOfxStatFailed;  // (an input connected to nothing: the plugin takes it as transparent)
        img = makeImage(*inst, *pic, false);
    }
    *imageHandle = handle(&img->props);
    return kOfxStatOK;
}
// (nothing thrown crosses back into the plug-in: a picture too large for memory is an error it is told of)
OfxStatus clipGetImage(OfxImageClipHandle h, OfxTime time, const OfxRectD*, OfxPropertySetHandle* imageHandle) {
    try {
        return clipGetImageUnguarded(h, time, imageHandle);
    } catch (const std::bad_alloc&) {
        return kOfxStatErrMemory;
    } catch (...) {
        return kOfxStatFailed;
    }
}

OfxStatus clipReleaseImage(OfxPropertySetHandle h) {
    if (!h) return kOfxStatErrBadHandle;
    void* self = nullptr;
    if (propGetPointer(h, "montage.image", 0, &self) != kOfxStatOK || !self) return kOfxStatErrBadHandle;
    ImageObj* img = static_cast<ImageObj*>(self);
    void* out = nullptr;
    propGetPointer(h, "montage.output", 0, &out);
    if (out) {
        // Copied back when released (and again by render() if the plugin keeps it).
        Image* dst = static_cast<Image*>(out);
        const int w = dst->width, hgt = dst->height;
        if (dst->px.size() != size_t(w) * size_t(hgt) * 4) dst->px.resize(size_t(w) * size_t(hgt) * 4);
        for (int y = 0; y < hgt; ++y) {
            float* d = dst->row(hgt - 1 - y);
            if (!img->f.empty()) std::memcpy(d, img->f.data() + size_t(y) * size_t(w) * 4, size_t(w) * 4 * sizeof(float));
            else
                for (int x = 0; x < w * 4; ++x) d[x] = img->b[size_t(y) * size_t(w) * 4 + size_t(x)] / 255.0f;
        }
        if (Instance* inst = static_cast<Instance*>(img->owner)) inst->wrote = true;
    }
    delete img;
    return kOfxStatOK;
}

OfxStatus clipGetRegionOfDefinition(OfxImageClipHandle h, OfxTime, OfxRectD* bounds) {
    if (!h || !bounds) return kOfxStatErrBadHandle;
    Instance* inst = clipObj(h)->effect ? clipObj(h)->effect->instance : nullptr;
    const double w = inst ? inst->width / inst->scale : 0, hh = inst ? inst->height / inst->scale : 0;
    *bounds = OfxRectD{0, 0, w, hh};
    return kOfxStatOK;
}
int effectAbort(OfxImageEffectHandle) { return 0; }

struct MemoryBlock {
    std::vector<uint8_t> bytes;
    int locks = 0;
};
OfxStatus imageMemoryAlloc(OfxImageEffectHandle, size_t n, OfxImageMemoryHandle* out) {
    if (!out) return kOfxStatErrBadHandle;
    try {
        auto m = std::make_unique<MemoryBlock>();
        m->bytes.resize(n);
        *out = reinterpret_cast<OfxImageMemoryHandle>(m.release());
        return kOfxStatOK;
    } catch (...) {
        *out = nullptr;
        return kOfxStatErrMemory;
    }
}
OfxStatus imageMemoryFree(OfxImageMemoryHandle h) {
    delete reinterpret_cast<MemoryBlock*>(h);
    return kOfxStatOK;
}
OfxStatus imageMemoryLock(OfxImageMemoryHandle h, void** ptr) {
    if (!h || !ptr) return kOfxStatErrBadHandle;
    auto* m = reinterpret_cast<MemoryBlock*>(h);
    ++m->locks;
    *ptr = m->bytes.data();
    return kOfxStatOK;
}
OfxStatus imageMemoryUnlock(OfxImageMemoryHandle h) {
    if (!h) return kOfxStatErrBadHandle;
    --reinterpret_cast<MemoryBlock*>(h)->locks;
    return kOfxStatOK;
}

OfxImageEffectSuiteV1 gImageEffectSuite = {getPropertySet, getParamSet, clipDefine, clipGetHandle, clipGetPropertySet, clipGetImage,
                                           clipReleaseImage, clipGetRegionOfDefinition, effectAbort, imageMemoryAlloc, imageMemoryFree,
                                           imageMemoryLock, imageMemoryUnlock};

// ---- OpenGL render suite --------------------------------------------------------------------------------------------

constexpr int kGlTexture2D = 0x0DE1;
std::atomic<int> gGpuRenders{0};

struct TextureObj {
    PropertySet props;
    unsigned texture = 0;
    bool output = false;
    Instance* inst = nullptr;
};

OfxStatus clipLoadTextureUnguarded(OfxImageClipHandle h, OfxTime time, OfxPropertySetHandle* out) {
    if (!h || !out) return kOfxStatErrBadHandle;
    ClipObj* c = clipObj(h);
    Instance* inst = c->effect ? c->effect->instance : nullptr;
    if (!inst || !inst->gl || !gl::onThread() || inst->width <= 0) return kOfxStatFailed;
    auto t = std::make_unique<TextureObj>();
    t->inst = inst;
    std::string id;
    if (c->name == kOfxImageEffectOutputClipName) {
        // The output: the framebuffer's texture, which is drawn into from now.
        t->texture = inst->target.texture;
        t->output = true;
        gl::bind(inst->target);
        id = "out:" + std::to_string(++gImageSerial);
    } else {
        Image scratch;
        const Image* pic = inputPicture(*inst, c->name, time, scratch);
        if (!pic) return kOfxStatFailed;
        t->texture = gl::upload(*pic);
        if (!t->texture) return kOfxStatErrMemory;
        inst->textures.push_back(t->texture);
        id = imageIdentifier(*pic, false);
    }
    PropertySet& p = t->props;
    p.setInt(kOfxImageEffectPropOpenGLTextureIndex, {int(t->texture)});
    p.setInt(kOfxImageEffectPropOpenGLTextureTarget, {kGlTexture2D});
    p.setString(kOfxImageEffectPropPixelDepth, {kOfxBitDepthFloat});
    p.setString(kOfxImageEffectPropComponents, {kOfxImageComponentRGBA});
    p.setString(kOfxImageEffectPropPreMultiplication, {kOfxImagePreMultiplied});
    p.setDouble(kOfxImageEffectPropRenderScale, {inst->scale, inst->scale});
    p.setDouble(kOfxImagePropPixelAspectRatio, {1.0});
    p.setInt(kOfxImagePropBounds, {0, 0, inst->width, inst->height});
    p.setInt(kOfxImagePropRegionOfDefinition, {0, 0, inst->width, inst->height});
    p.setInt(kOfxImagePropRowBytes, {inst->width * 16});
    p.setString(kOfxImagePropField, {kOfxImageFieldNone});
    p.setString(kOfxImagePropUniqueIdentifier, {id});
    p.setPointer("montage.texture", t.get());
    *out = handle(&t.release()->props);
    return kOfxStatOK;
}

OfxStatus clipFreeTexture(OfxPropertySetHandle h) {
    if (!h) return kOfxStatErrBadHandle;
    void* self = nullptr;
    if (propGetPointer(h, "montage.texture", 0, &self) != kOfxStatOK || !self) return kOfxStatErrBadHandle;
    auto* t = static_cast<TextureObj*>(self);
    // (an input's texture goes with it; the output's stays for Montage to read back)
    if (!t->output && gl::onThread() && t->inst) {
        auto& live = t->inst->textures;
        if (auto it = std::find(live.begin(), live.end(), t->texture); it != live.end()) {
            live.erase(it);
            gl::destroyTexture(t->texture);
        }
    }
    delete t;
    return kOfxStatOK;
}

OfxStatus flushResources() { return kOfxStatReplyDefault; }

// (as clipGetImage: nothing thrown crosses back into the plug-in)
OfxStatus clipLoadTexture(OfxImageClipHandle h, OfxTime time, const char*, const OfxRectD*, OfxPropertySetHandle* out) {
    try {
        return clipLoadTextureUnguarded(h, time, out);
    } catch (const std::bad_alloc&) {
        return kOfxStatErrMemory;
    } catch (...) {
        return kOfxStatFailed;
    }
}

OfxImageEffectOpenGLRenderSuiteV1 gOpenGLSuite = {clipLoadTexture, clipFreeTexture, flushResources};

// ---- Interact and draw suites (on-screen controls) ------------------------------------------------------------------

struct InteractObj {
    PropertySet props;
    bool redraw = false;
};
InteractObj* interactObj(OfxInteractHandle h) { return reinterpret_cast<InteractObj*>(h); }

OfxStatus interactSwapBuffers(OfxInteractHandle h) { return h ? kOfxStatOK : kOfxStatErrBadHandle; }
OfxStatus interactRedraw(OfxInteractHandle h) {
    if (!h) return kOfxStatErrBadHandle;
    interactObj(h)->redraw = true;
    return kOfxStatOK;
}
OfxStatus interactGetPropertySet(OfxInteractHandle h, OfxPropertySetHandle* props) {
    if (!h || !props) return kOfxStatErrBadHandle;
    *props = handle(&interactObj(h)->props);
    return kOfxStatOK;
}
OfxInteractSuiteV1 gInteractSuite = {interactSwapBuffers, interactRedraw, interactGetPropertySet};

// What a control draws through the Draw Suite during one draw: recorded, and painted by the editor.
struct DrawCtx {
    Overlay* out = nullptr;
    std::array<float, 4> colour{1, 1, 1, 1};
    float width = 0;
    int stipple = 0;
};
DrawCtx* drawCtx(OfxDrawContextHandle h) { return reinterpret_cast<DrawCtx*>(h); }

OfxStatus drawGetColour(OfxDrawContextHandle h, OfxStandardColour which, OfxRGBAColourF* c) {
    if (!h || !c) return kOfxStatFailed;
    // (Montage's overlay colours: the TransformOverlay's white box, its yellow while dragged)
    switch (which) {
        case kOfxStandardColourOverlayBackground: *c = {0, 0, 0, 1}; break;
        case kOfxStandardColourOverlayActive: *c = {1, 0.85f, 0.2f, 1}; break;
        case kOfxStandardColourOverlaySelected: *c = {1, 0.85f, 0.2f, 1}; break;
        case kOfxStandardColourOverlayDeselected: *c = {1, 1, 1, 1}; break;
        case kOfxStandardColourOverlayMarqueeFG: *c = {1, 1, 1, 1}; break;
        case kOfxStandardColourOverlayMarqueeBG: *c = {0, 0, 0, 0.5f}; break;
        case kOfxStandardColourOverlayText: *c = {1, 1, 1, 1}; break;
        default: return kOfxStatErrValue;
    }
    return kOfxStatOK;
}
OfxStatus drawSetColour(OfxDrawContextHandle h, const OfxRGBAColourF* c) {
    if (!h || !c) return kOfxStatFailed;
    drawCtx(h)->colour = {c->r, c->g, c->b, c->a};
    return kOfxStatOK;
}
OfxStatus drawSetLineWidth(OfxDrawContextHandle h, float width) {
    if (!h) return kOfxStatFailed;
    drawCtx(h)->width = std::max(0.0f, width);
    return kOfxStatOK;
}
OfxStatus drawSetLineStipple(OfxDrawContextHandle h, OfxDrawLineStipplePattern pattern) {
    if (!h) return kOfxStatFailed;
    if (pattern < kOfxDrawLineStipplePatternSolid || pattern > kOfxDrawLineStipplePatternDotDash) return kOfxStatErrValue;
    drawCtx(h)->stipple = int(pattern);
    return kOfxStatOK;
}
OfxStatus drawDraw(OfxDrawContextHandle h, OfxDrawPrimitive primitive, const OfxPointD* points, int count) {
    if (!h || !drawCtx(h)->out) return kOfxStatFailed;
    if (!points || count <= 0 || primitive < kOfxDrawPrimitiveLines || primitive > kOfxDrawPrimitiveEllipse) return kOfxStatErrValue;
    const bool twoCorners = primitive == kOfxDrawPrimitiveRectangle || primitive == kOfxDrawPrimitiveEllipse;
    if ((twoCorners && count != 2) || (primitive == kOfxDrawPrimitiveLines && count % 2) || count > 1000000) return kOfxStatErrValue;
    DrawOp op;
    op.kind = DrawOp::Kind(int(primitive));
    op.colour = drawCtx(h)->colour;
    op.width = drawCtx(h)->width;
    op.stipple = drawCtx(h)->stipple;
    op.points.reserve(size_t(count));
    for (int i = 0; i < count; ++i) op.points.push_back({points[i].x, points[i].y});
    drawCtx(h)->out->ops.push_back(std::move(op));
    return kOfxStatOK;
}
OfxStatus drawText(OfxDrawContextHandle h, const char* text, const OfxPointD* pos, int alignment) {
    if (!h || !drawCtx(h)->out) return kOfxStatFailed;
    if (!text || !pos) return kOfxStatErrValue;
    DrawOp op;
    op.kind = DrawOp::Text;
    op.colour = drawCtx(h)->colour;
    op.points = {{pos->x, pos->y}};
    op.text = text;
    op.align = alignment;
    drawCtx(h)->out->ops.push_back(std::move(op));
    return kOfxStatOK;
}
OfxDrawSuiteV1 gDrawSuite = {drawGetColour, drawSetColour, drawSetLineWidth, drawSetLineStipple, drawDraw, drawText};

// ---- Memory, threads, messages, progress, timeline --------------------------------------------------------------------

OfxStatus memoryAlloc(void*, size_t n, void** out) {
    if (!out) return kOfxStatErrBadHandle;
    *out = std::malloc(std::max<size_t>(n, 1));
    return *out ? kOfxStatOK : kOfxStatErrMemory;
}
OfxStatus memoryFree(void* p) {
    std::free(p);
    return kOfxStatOK;
}
OfxMemorySuiteV1 gMemorySuite = {memoryAlloc, memoryFree};

thread_local unsigned tThreadIndex = 0;
thread_local bool tSpawned = false;

OfxStatus multiThread(OfxThreadFunctionV1 func, unsigned int n, void* arg) {
    if (!func) return kOfxStatErrBadHandle;
    n = std::max(1u, n);
    std::vector<std::thread> threads;
    const ExpressionContext where = currentExpressionContext();  // (settings the plug-in reads on its threads)
    for (unsigned i = 1; i < n; ++i)
        threads.emplace_back([=] {
            ExpressionScope scope(where.project, where.sequence, where.clip, where.cancel);
            tThreadIndex = i;
            tSpawned = true;
            func(i, n, arg);
        });
    const unsigned savedIndex = tThreadIndex;
    const bool savedSpawned = tSpawned;
    tThreadIndex = 0;
    tSpawned = true;
    func(0, n, arg);
    tThreadIndex = savedIndex;
    tSpawned = savedSpawned;
    for (auto& t : threads) t.join();
    return kOfxStatOK;
}
OfxStatus multiThreadNumCPUs(unsigned int* n) {
    if (!n) return kOfxStatErrBadHandle;
    *n = std::max(1u, std::thread::hardware_concurrency());
    return kOfxStatOK;
}
OfxStatus multiThreadIndex(unsigned int* i) {
    if (!i) return kOfxStatErrBadHandle;
    *i = tThreadIndex;
    return kOfxStatOK;
}
int multiThreadIsSpawnedThread() { return tSpawned ? 1 : 0; }
OfxStatus mutexCreate(OfxMutexHandle* m, int lockCount) {
    if (!m) return kOfxStatErrBadHandle;
    auto* mutex = new std::recursive_mutex;
    for (int i = 0; i < lockCount; ++i) mutex->lock();
    *m = reinterpret_cast<OfxMutexHandle>(mutex);
    return kOfxStatOK;
}
OfxStatus mutexDestroy(const OfxMutexHandle m) {
    delete reinterpret_cast<std::recursive_mutex*>(m);
    return kOfxStatOK;
}
OfxStatus mutexLock(const OfxMutexHandle m) {
    if (!m) return kOfxStatErrBadHandle;
    reinterpret_cast<std::recursive_mutex*>(m)->lock();
    return kOfxStatOK;
}
OfxStatus mutexUnLock(const OfxMutexHandle m) {
    if (!m) return kOfxStatErrBadHandle;
    reinterpret_cast<std::recursive_mutex*>(m)->unlock();
    return kOfxStatOK;
}
OfxStatus mutexTryLock(const OfxMutexHandle m) {
    if (!m) return kOfxStatErrBadHandle;
    return reinterpret_cast<std::recursive_mutex*>(m)->try_lock() ? kOfxStatOK : kOfxStatFailed;
}
OfxMultiThreadSuiteV1 gMultiThreadSuite = {multiThread, multiThreadNumCPUs, multiThreadIndex, multiThreadIsSpawnedThread, mutexCreate,
                                           mutexDestroy, mutexLock, mutexUnLock, mutexTryLock};

Instance* instanceOf(void* handle) {
    // Messages and progress name the effect instance (or its descriptor).
    if (!handle) return nullptr;
    return effectObj(reinterpret_cast<OfxImageEffectHandle>(handle))->instance;
}
std::string formatted(const char* format, va_list ap) {
    if (!format) return {};
    char buf[2048];
    std::vsnprintf(buf, sizeof buf, format, ap);
    return buf;
}
OfxStatus message(void* h, const char* type, const char*, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    const std::string text = formatted(format, ap);
    va_end(ap);
    if (type && std::strcmp(type, kOfxMessageError) == 0)
        if (Instance* inst = instanceOf(h)) inst->error = text;
    if (type && std::strcmp(type, kOfxMessageQuestion) == 0) return kOfxStatReplyNo;  // nobody to ask mid-render
    return kOfxStatOK;
}
OfxStatus setPersistentMessage(void* h, const char* type, const char*, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    const std::string text = formatted(format, ap);
    va_end(ap);
    if (type && (std::strcmp(type, kOfxMessageError) == 0 || std::strcmp(type, kOfxMessageWarning) == 0))
        if (Instance* inst = instanceOf(h)) inst->error = text;
    return kOfxStatOK;
}
OfxStatus clearPersistentMessage(void* h) {
    if (Instance* inst = instanceOf(h)) inst->error.clear();
    return kOfxStatOK;
}
OfxStatus messageV1(void* h, const char* type, const char* id, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    const std::string text = formatted(format, ap);
    va_end(ap);
    return message(h, type, id, "%s", text.c_str());
}
OfxMessageSuiteV1 gMessageSuiteV1 = {messageV1};
OfxMessageSuiteV2 gMessageSuiteV2 = {message, setPersistentMessage, clearPersistentMessage};

OfxStatus progressStart(void*, const char*) { return kOfxStatOK; }
OfxStatus progressUpdate(void*, double) { return kOfxStatOK; }
OfxStatus progressEnd(void*) { return kOfxStatOK; }
OfxStatus progressStartV2(void*, const char*, const char*) { return kOfxStatOK; }
OfxProgressSuiteV1 gProgressSuiteV1 = {progressStart, progressUpdate, progressEnd};
OfxProgressSuiteV2 gProgressSuiteV2 = {progressStartV2, progressUpdate, progressEnd};

OfxStatus getTime(void* h, double* t) {
    if (!t) return kOfxStatErrBadHandle;
    Instance* inst = instanceOf(h);
    *t = inst ? inst->time : 0;
    return kOfxStatOK;
}
OfxStatus gotoTime(void*, double) { return kOfxStatOK; }
OfxStatus getTimeBounds(void* h, double* first, double* last) {
    if (!first || !last) return kOfxStatErrBadHandle;
    Instance* inst = instanceOf(h);
    *first = 0;
    *last = inst ? std::max(inst->time, inst->obj.props.getDouble(kOfxImageEffectInstancePropEffectDuration, 0)) : 0;
    return kOfxStatOK;
}
OfxTimeLineSuiteV1 gTimeLineSuite = {getTime, gotoTime, getTimeBounds};

// ---- Colour management (OpenFX 1.5) --------------------------------------------------------------------------------

constexpr const char* kNativeConfig = "ofx-native-v1.5_aces-v1.3_ocio-v2.3";

// The style agreed with a plugin that describes itself with `p`: the highest both take (Montage gives the core
// colourspaces), empty when it takes none or not the native config.
std::string colourStyleWith(const PropertySet& p) {
    const std::string style = p.getString(kOfxImageEffectPropColourManagementStyle);
    const std::vector<std::string> configs = p.strings(kOfxImageEffectPropColourManagementAvailableConfigs);
    if (!configs.empty() && std::find(configs.begin(), configs.end(), kNativeConfig) == configs.end()) return {};
    if (style == kOfxImageEffectColourManagementBasic) return kOfxImageEffectColourManagementBasic;
    if (style == kOfxImageEffectColourManagementCore || style == kOfxImageEffectColourManagementFull || style == kOfxImageEffectColourManagementOCIO)
        return kOfxImageEffectColourManagementCore;
    return {};
}

// The native config's colourspaces Montage can make, by their encoding (primaries and curve).
struct NativeSpace {
    const char* name;
    Primaries primaries;
    Transfer transfer;
    bool display;
    bool basic;
};
constexpr NativeSpace kNativeSpaces[] = {
    {kOfxColourspaceRec1886Rec709Display, Primaries::Bt709, Transfer::Bt1886, true, false},
    {kOfxColourspaceSrgbDisplay, Primaries::Bt709, Transfer::Srgb, true, false},
    {kOfxColourspaceRec1886Rec2020Display, Primaries::Bt2020, Transfer::Bt1886, true, false},
    {kOfxColourspaceDisplayp3Display, Primaries::P3D65, Transfer::Srgb, true, false},
    {kOfxColourspaceRec2100PqDisplay, Primaries::Bt2020, Transfer::Pq, true, false},
    {kOfxColourspaceRec2100HlgDisplay, Primaries::Bt2020, Transfer::Hlg, true, false},
    {kOfxColourspaceSt2084P3d65Display, Primaries::P3D65, Transfer::Pq, true, false},
    {kOfxColourspaceG24Rec709Tx, Primaries::Bt709, Transfer::Bt1886, false, false},
    {kOfxColourspaceSrgbTx, Primaries::Bt709, Transfer::Srgb, false, false},
    {kOfxColourspaceSrgbEncodedP3d65Tx, Primaries::P3D65, Transfer::Srgb, false, false},
    {kOfxColourspaceSrgbEncodedAp1Tx, Primaries::Ap1, Transfer::Srgb, false, false},
    {kOfxColourspaceLinRec709Srgb, Primaries::Bt709, Transfer::Linear, false, false},
    {kOfxColourspaceLinRec2020, Primaries::Bt2020, Transfer::Linear, false, false},
    {kOfxColourspaceLinP3d65, Primaries::P3D65, Transfer::Linear, false, false},
    {kOfxColourspaceACEScg, Primaries::Ap1, Transfer::Linear, false, false},
    {kOfxColourspaceACES20651, Primaries::Ap0, Transfer::Linear, false, false},
    {kOfxColourspaceACEScct, Primaries::Ap1, Transfer::AcesCct, false, false},
    {"scene_linear", Primaries::Ap1, Transfer::Linear, false, false},  // (the config's roles)
    {"aces_interchange", Primaries::Ap0, Transfer::Linear, false, false},
    {"color_timing", Primaries::Ap1, Transfer::AcesCct, false, false},
    {"compositing_log", Primaries::Ap1, Transfer::AcesCct, false, false},
    // The basic colourspaces: a linear and a log one Montage picks.
    {kOfxColourspaceOfxSceneLinear, Primaries::Bt709, Transfer::Linear, false, true},
    {kOfxColourspaceOfxSceneLog, Primaries::Ap1, Transfer::AcesCct, false, true},
};

const NativeSpace* nativeSpace(const std::string& name) {
    for (const NativeSpace& n : kNativeSpaces)
        if (name == n.name) return &n;
    return nullptr;
}

bool rawSpace(const std::string& name) {
    return name == kOfxColourspaceOfxRaw || name == kOfxColourspaceRaw || name == "data";
}

// Montage's working space by the config's name (the basic display one in the basic style). A scene-referred one the
// config has no name for is given as ACEScct (the basic scene log one), which the pictures are then converted to.
std::string workingSpaceName(const ColorSpace& working, const std::string& style) {
    const bool basic = style == kOfxImageEffectColourManagementBasic;
    if (basic && !working.sceneReferred) return working.hdr() ? kOfxColourspaceOfxDisplayHdr : kOfxColourspaceOfxDisplaySdr;
    for (const NativeSpace& n : kNativeSpaces)
        if (n.display != working.sceneReferred && (!basic || n.basic) && n.primaries == working.primaries && n.transfer == working.transfer)
            return n.name;
    if (working.sceneReferred) return basic ? kOfxColourspaceOfxSceneLog : kOfxColourspaceACEScct;
    return kOfxColourspaceRec1886Rec709Display;
}

// A premultiplied picture from one encoding to another: its curve undone, its primaries changed, the other curve
// applied, with nothing clipped or tone mapped, so a picture taken there and back is as it was.
void reencode(Image& img, Primaries fromP, Transfer fromT, Primaries toP, Transfer toT) {
    if (fromP == toP && fromT == toT) return;
    double m[9];
    primariesMatrix(fromP, toP, m);
    parallelRows(img.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* p = img.row(y);
            for (int x = 0; x < img.width; ++x, p += 4) {
                const float a = p[3];
                if (a <= 0) continue;
                double c[3];
                for (int k = 0; k < 3; ++k) c[k] = toLinear(fromT, double(p[k] / a));
                for (int k = 0; k < 3; ++k) {
                    const double l = m[k * 3] * c[0] + m[k * 3 + 1] * c[1] + m[k * 3 + 2] * c[2];
                    p[k] = float(fromLinear(toT, l)) * a;
                }
            }
        }
    });
}

// What a render's colour management came to: the colourspace the plugin's pictures are given in and the one its
// output is in (null: the working space; raw: left as it is).
struct ColourPlan {
    const NativeSpace* source = nullptr;
    const NativeSpace* output = nullptr;
    bool rawOutput = false;
};

thread_local const InputResolver* tInputs = nullptr;
thread_local const std::vector<float>* tOwnMask = nullptr;
// Set while the thread does what a render on the OpenGL thread handed back to it (gl::callBack): plug-ins it renders
// then run on the CPU, as the OpenGL thread is busy waiting for it.
thread_local bool tNoGl = false;
struct NoGl {
    bool previous = tNoGl;
    NoGl() { tNoGl = true; }
    ~NoGl() { tNoGl = previous; }
};
// Runs compositor work (a frame at another time, a connected input) on the thread that asked for the render, with
// its scopes, even when the plug-in asks from the OpenGL thread.
bool onRenderThread(const std::function<bool()>& fn) {
    bool ok = false;
    gl::callBack([&] {
        NoGl noGl;
        ok = fn();
    });
    return ok;
}

const Image* inputPicture(Instance& inst, const std::string& name, double time, Image& scratch) {
    const int w = inst.width, h = inst.height;
    auto fit = [&](Image& img) {
        if (img.width != w || img.height != h) img = resizeImage(img, w, h);
    };
    if (auto it = inst.images.find(name); it != inst.images.end()) return it->second;
    if (name == kOfxImageEffectSimpleSourceClipName && inst.source) {
        // Another frame, sized like this one (else this one stands in).
        const FrameFetch* fetch = inst.fetch;
        if (std::fabs(time - inst.time) > 1e-6 && fetch && *fetch && onRenderThread([&] { return (*fetch)(time, scratch); }) &&
            !scratch.empty()) {
            fit(scratch);
            return &scratch;
        }
        return inst.source;
    }
    // A General plugin's further input: what its effect connects it to.
    if (!inst.effect) return nullptr;
    const std::string connection = inst.effect->s(inputKey(name));
    if (connection.empty()) return nullptr;
    if (connection == "mask") {
        // (no mask drawn: all of the picture)
        const std::vector<float>* m = inst.ownMask;
        scratch = Image(w, h);
        if (!m || m->size() != size_t(w) * size_t(h)) std::fill(scratch.px.begin(), scratch.px.end(), 1.0f);
        else
            for (size_t i = 0; i < m->size(); ++i) std::fill_n(&scratch.px[i * 4], 4, (*m)[i]);
        return &scratch;
    }
    if (auto it = inst.prefetched.find(name); it != inst.prefetched.end() && std::fabs(time - inst.time) <= 1e-6) {
        scratch = it->second;
    } else {
        const InputResolver* inputs = inst.inputs;
        if (!inputs || !*inputs || !onRenderThread([&] { return (*inputs)(connection, time, scratch); }) || scratch.empty()) return nullptr;
        fit(scratch);
    }
    if (inst.convert && inst.space)
        reencode(scratch, inst.space->primaries, inst.space->transfer, inst.convert->primaries, inst.convert->transfer);
    return &scratch;
}

// ---- The host -------------------------------------------------------------------------------------------------------

PropertySet& hostProps() {
    static PropertySet* p = [] {
        auto* h = new PropertySet;
        h->setString(kOfxPropType, {kOfxTypeImageEffectHost});
        h->setString(kOfxPropName, {"org.montage.editor"});
        h->setString(kOfxPropLabel, {"Montage"});
        h->setInt(kOfxPropAPIVersion, {1, 5});
        h->setInt(kOfxPropVersion, {0, 1, 0});
        h->setString(kOfxPropVersionLabel, {"0.1"});
        h->setInt(kOfxImageEffectHostPropIsBackground, {0});
        h->setInt(kOfxImageEffectPropSupportsOverlays, {1});
        h->setInt(kOfxImageEffectPropSupportsMultiResolution, {1});
        h->setInt(kOfxImageEffectPropSupportsTiles, {0});
        h->setInt(kOfxImageEffectPropTemporalClipAccess, {1});
        h->setString(kOfxImageEffectPropSupportedComponents, {kOfxImageComponentRGBA});
        h->setString(kOfxImageEffectPropSupportedContexts,
                     {kOfxImageEffectContextFilter, kOfxImageEffectContextGeneral, kOfxImageEffectContextTransition, kOfxImageEffectContextGenerator});
        h->setString(kOfxImageEffectPropSupportedPixelDepths, {kOfxBitDepthFloat, kOfxBitDepthByte});
        h->setInt(kOfxImageEffectPropSupportsMultipleClipDepths, {0});
        h->setInt(kOfxImageEffectPropSupportsMultipleClipPARs, {0});
        h->setInt(kOfxImageEffectPropSetableFrameRate, {0});
        h->setInt(kOfxImageEffectPropSetableFielding, {0});
        h->setInt(kOfxImageEffectInstancePropSequentialRender, {0});
        h->setInt(kOfxParamHostPropSupportsCustomInteract, {0});
        h->setInt(kOfxParamHostPropSupportsStringAnimation, {0});
        h->setInt(kOfxParamHostPropSupportsChoiceAnimation, {0});
        h->setInt(kOfxParamHostPropSupportsBooleanAnimation, {0});
        h->setInt(kOfxParamHostPropSupportsCustomAnimation, {0});
        h->setInt(kOfxParamHostPropMaxParameters, {-1});
        h->setInt(kOfxParamHostPropMaxPages, {0});
        h->setInt(kOfxParamHostPropPageRowColumnCount, {0, 0});
        h->setPointer(kOfxPropHostOSHandle, nullptr);
        // OpenGL rendering (render/OfxGl; a plugin that needs it fails with why where there is none).
        h->setString(kOfxImageEffectPropOpenGLRenderSupported, {"true"});
        h->setString(kOfxImageEffectPropCPURenderSupported, {"true"});
        // Colour management (OpenFX 1.5): the core colourspaces of the native config.
        h->setString(kOfxImageEffectPropColourManagementStyle, {kOfxImageEffectColourManagementCore});
        h->setString(kOfxImageEffectPropColourManagementAvailableConfigs, {kNativeConfig});
        return h;
    }();
    return *p;
}

const void* fetchSuite(OfxPropertySetHandle, const char* name, int version) {
    if (!name) return nullptr;
    const std::string n = name;
    if (n == kOfxPropertySuite && version == 1) return &gPropertySuite;
    if (n == kOfxImageEffectSuite && version == 1) return &gImageEffectSuite;
    if (n == kOfxParameterSuite && version == 1) return &gParameterSuite;
    if (n == kOfxMemorySuite && version == 1) return &gMemorySuite;
    if (n == kOfxMultiThreadSuite && version == 1) return &gMultiThreadSuite;
    if (n == kOfxMessageSuite && version == 1) return &gMessageSuiteV1;
    if (n == kOfxMessageSuite && version == 2) return &gMessageSuiteV2;
    if (n == kOfxProgressSuite && version == 1) return &gProgressSuiteV1;
    if (n == kOfxProgressSuite && version == 2) return &gProgressSuiteV2;
    if (n == kOfxTimeLineSuite && version == 1) return &gTimeLineSuite;
    if (n == kOfxInteractSuite && version == 1) return &gInteractSuite;
    if (n == kOfxDrawSuite && version == 1) return &gDrawSuite;
    if (n == kOfxOpenGLRenderSuite && version == 1) return &gOpenGLSuite;
    return nullptr;
}

OfxHost* host() {
    static OfxHost h = {handle(&hostProps()), fetchSuite};
    return &h;
}

bool ok(OfxStatus st) { return st == kOfxStatOK || st == kOfxStatReplyDefault; }

std::mutex gLoadMutex;
std::map<std::string, std::unique_ptr<Binary>>& binaries() {
    static std::map<std::string, std::unique_ptr<Binary>> b;
    return b;
}
std::map<std::string, std::unique_ptr<Loaded>>& loadedPlugins() {
    static std::map<std::string, std::unique_ptr<Loaded>> l;
    return l;
}

Binary* openBinary(const std::string& path, std::string* error) {
    auto& all = binaries();
    if (auto it = all.find(path); it != all.end()) return it->second.get();
    auto b = std::make_unique<Binary>();
    b->path = path;
    if (!b->lib.open(path, error)) return nullptr;
    b->count = reinterpret_cast<int (*)()>(b->lib.symbol("OfxGetNumberOfPlugins"));
    b->get = reinterpret_cast<OfxPlugin* (*)(int)>(b->lib.symbol("OfxGetPlugin"));
    if (!b->count || !b->get) {
        if (error) *error = "Not an OpenFX plugin (no OfxGetNumberOfPlugins / OfxGetPlugin)";
        return nullptr;
    }
    // Optional bundle-wide setup (OFX 1.4).
    if (auto setHost = reinterpret_cast<OfxStatus (*)(const OfxHost*)>(b->lib.symbol("OfxSetHost"))) setHost(host());
    Binary* raw = b.get();
    all[path] = std::move(b);
    return raw;
}

std::string bundleOf(const std::string& binary) {
    // .../Name.ofx.bundle/Contents/<arch>/Name.ofx
    QDir d = QFileInfo(QString::fromStdString(binary)).absoluteDir();
    d.cdUp();
    d.cdUp();
    return d.absolutePath().toStdString();
}

void describeParams(EffectObj& e, PluginDesc& d) {
    for (const auto& p : e.params.params) {
        ParamDesc pd;
        pd.name = p->name;
        pd.type = p->type;
        pd.label = p->props.getString(kOfxPropLabel, p->name);
        pd.secret = p->props.getInt(kOfxParamPropSecret) != 0;
        pd.animates = p->props.getInt(kOfxParamPropAnimates, 1) != 0;
        const int n = dimensionsOf(p->type);
        for (int k = 0; k < n; ++k) pd.def.push_back(p->props.getDouble(kOfxParamPropDefault, 0, k));
        if (n > 0) {
            pd.min = p->props.getDouble(kOfxParamPropMin, -1e9);
            pd.max = p->props.getDouble(kOfxParamPropMax, 1e9);
            pd.displayMin = p->props.getDouble(kOfxParamPropDisplayMin, pd.min);
            pd.displayMax = p->props.getDouble(kOfxParamPropDisplayMax, pd.max);
        }
        pd.choices = p->props.strings(kOfxParamPropChoiceOption);
        pd.doubleType = p->props.getString(kOfxParamPropDoubleType);
        pd.normalised = normalisedAxis(p->props, 0) >= 0;
        if (stringType(p->type)) pd.stringDefault = p->props.getString(kOfxParamPropDefault);
        d.params.push_back(pd);
    }
}

// Loads, describes and describes in the filter context the plug-in at `index` of `bin`.
std::unique_ptr<Loaded> describe(Binary* bin, int index, std::string* error) {
    OfxPlugin* plugin = bin->get(index);
    if (!plugin || !plugin->pluginApi || std::strcmp(plugin->pluginApi, kOfxImageEffectPluginApi) != 0 || !plugin->mainEntry) return nullptr;
    auto l = std::make_unique<Loaded>();
    l->plugin = plugin;
    plugin->setHost(host());
    if (!ok(plugin->mainEntry(kOfxActionLoad, nullptr, nullptr, nullptr))) {
        if (error) *error = std::string("The plugin ") + plugin->pluginIdentifier + " failed to load";
        return nullptr;
    }
    l->descriptor = std::make_unique<EffectObj>();
    PropertySet& dp = l->descriptor->props;
    dp.setString(kOfxPropType, {kOfxTypeImageEffect});
    dp.setString(kOfxPropLabel, {plugin->pluginIdentifier});
    dp.setString(kOfxImageEffectPluginPropGrouping, {""});
    dp.setString(kOfxImageEffectPropSupportedContexts, {});
    dp.setString(kOfxImageEffectPropSupportedPixelDepths, {});
    dp.setString(kOfxPluginPropFilePath, {bundleOf(bin->path)});
    dp.setInt(kOfxImageEffectPropTemporalClipAccess, {0});
    dp.setString(kOfxImageEffectPluginRenderThreadSafety, {kOfxImageEffectRenderInstanceSafe});
    dp.setInt(kOfxImageEffectPluginPropHostFrameThreading, {0});
    dp.setInt(kOfxImageEffectPropSupportsMultiResolution, {1});
    dp.setInt(kOfxImageEffectPropSupportsTiles, {1});
    if (!ok(plugin->mainEntry(kOfxActionDescribe, reinterpret_cast<OfxImageEffectHandle>(l->descriptor.get()), nullptr, nullptr))) {
        if (error) *error = std::string(plugin->pluginIdentifier) + " failed to describe itself";
        return nullptr;
    }
    // The first context it takes of: Filter, General (clip effects), Transition, Generator.
    const std::vector<std::string> contexts = dp.strings(kOfxImageEffectPropSupportedContexts);
    l->context.clear();
    for (const char* c : {kOfxImageEffectContextFilter, kOfxImageEffectContextGeneral, kOfxImageEffectContextTransition, kOfxImageEffectContextGenerator})
        if (l->context.empty() && std::find(contexts.begin(), contexts.end(), c) != contexts.end()) l->context = c;
    if (l->context.empty()) {
        if (error) *error = std::string(plugin->pluginIdentifier) + " takes none of the contexts Montage runs (filter, general, transition, generator)";
        return nullptr;
    }
    l->filter = std::make_unique<EffectObj>();
    l->filter->props = dp;
    l->filter->props.setString(kOfxImageEffectPropContext, {l->context});
    PropertySet in;
    in.setString(kOfxImageEffectPropContext, {l->context});
    if (!ok(plugin->mainEntry(kOfxImageEffectActionDescribeInContext, reinterpret_cast<OfxImageEffectHandle>(l->filter.get()), handle(&in), nullptr))) {
        if (error) *error = std::string(plugin->pluginIdentifier) + " failed to describe itself in the " + l->context.substr(21) + " context";
        return nullptr;
    }
    const PropertySet& fp = l->filter->props;
    PluginDesc& d = l->desc;
    d.identifier = plugin->pluginIdentifier;
    d.versionMajor = int(plugin->pluginVersionMajor);
    d.versionMinor = int(plugin->pluginVersionMinor);
    d.id = d.identifier + "/" + std::to_string(d.versionMajor);
    d.label = fp.getString(kOfxPropLabel, d.identifier);
    d.group = fp.getString(kOfxImageEffectPluginPropGrouping);
    d.description = fp.getString(kOfxPropPluginDescription);
    d.binary = bin->path;
    d.bundle = bundleOf(bin->path);
    d.index = index;
    d.temporal = fp.getInt(kOfxImageEffectPropTemporalClipAccess) != 0;
    const std::vector<std::string> depths = fp.strings(kOfxImageEffectPropSupportedPixelDepths);
    d.floatImages = depths.empty() || std::find(depths.begin(), depths.end(), kOfxBitDepthFloat) != depths.end();
    d.colour = colourStyleWith(fp);
    l->unsafe = fp.getString(kOfxImageEffectPluginRenderThreadSafety) == kOfxImageEffectRenderUnsafe;
    d.context = l->context;
    if (l->context == kOfxImageEffectContextGeneral)
        for (const auto& c : l->filter->clips)
            if (c->name != kOfxImageEffectOutputClipName && c->name != kOfxImageEffectSimpleSourceClipName)
                d.inputs.push_back(InputDesc{c->name, c->props.getString(kOfxPropLabel, c->name), c->props.getInt(kOfxImageClipPropIsMask) != 0,
                                             c->props.getInt(kOfxImageClipPropOptional) != 0});
    const std::string gpu = fp.getString(kOfxImageEffectPropOpenGLRenderSupported, "false");
    d.openGL = gpu == "true" || gpu == "needed" ? gpu : "false";
    // Its on-screen controls: the Draw Suite's (V2) where it has both.
    void* entry = nullptr;
    for (const auto& [name, version] : {std::pair{kOfxImageEffectPluginPropOverlayInteractV2, 2}, std::pair{kOfxImageEffectPluginPropOverlayInteractV1, 1}}) {
        auto it = fp.props.find(name);
        if (!entry && it != fp.props.end() && it->second.type == 'p' && !it->second.p.empty() && it->second.p[0]) {
            entry = it->second.p[0];
            l->overlayVersion = version;
        }
    }
    l->overlay = reinterpret_cast<OfxPluginEntryPoint*>(entry);
    d.overlay = l->overlay ? l->overlayVersion : 0;
    describeParams(*l->filter, d);
    return l;
}

QJsonObject toJson(const PluginDesc& d) {
    QJsonArray params, inputs;
    for (const InputDesc& in : d.inputs)
        inputs.append(QJsonObject{{"name", QString::fromStdString(in.name)}, {"label", QString::fromStdString(in.label)}, {"mask", in.mask},
                                  {"optional", in.optional}});
    for (const ParamDesc& p : d.params) {
        QJsonArray def, choices;
        for (double v : p.def) def.append(v);
        for (const std::string& c : p.choices) choices.append(QString::fromStdString(c));
        params.append(QJsonObject{{"name", QString::fromStdString(p.name)}, {"label", QString::fromStdString(p.label)},
                                  {"type", QString::fromStdString(p.type)}, {"default", def}, {"min", p.min}, {"max", p.max},
                                  {"displayMin", p.displayMin}, {"displayMax", p.displayMax}, {"choices", choices},
                                  {"string", QString::fromStdString(p.stringDefault)}, {"doubleType", QString::fromStdString(p.doubleType)},
                                  {"normalised", p.normalised}, {"animates", p.animates}, {"secret", p.secret}});
    }
    return QJsonObject{{"id", QString::fromStdString(d.id)}, {"identifier", QString::fromStdString(d.identifier)},
                       {"versionMajor", d.versionMajor}, {"versionMinor", d.versionMinor}, {"label", QString::fromStdString(d.label)},
                       {"group", QString::fromStdString(d.group)}, {"description", QString::fromStdString(d.description)},
                       {"binary", QString::fromStdString(d.binary)}, {"bundle", QString::fromStdString(d.bundle)}, {"index", d.index},
                       {"temporal", d.temporal}, {"float", d.floatImages}, {"colour", QString::fromStdString(d.colour)}, {"params", params},
                       {"context", QString::fromStdString(d.context)}, {"inputs", inputs}, {"openGL", QString::fromStdString(d.openGL)},
                       {"overlay", d.overlay}};
}

PluginDesc fromJson(const QJsonObject& o) {
    PluginDesc d;
    d.id = o.value("id").toString().toStdString();
    d.identifier = o.value("identifier").toString().toStdString();
    d.versionMajor = o.value("versionMajor").toInt(1);
    d.versionMinor = o.value("versionMinor").toInt();
    d.label = o.value("label").toString().toStdString();
    d.group = o.value("group").toString().toStdString();
    d.description = o.value("description").toString().toStdString();
    d.binary = o.value("binary").toString().toStdString();
    d.bundle = o.value("bundle").toString().toStdString();
    d.index = o.value("index").toInt();
    d.temporal = o.value("temporal").toBool();
    d.floatImages = o.value("float").toBool(true);
    d.colour = o.value("colour").toString().toStdString();
    d.context = o.value("context").toString(kOfxImageEffectContextFilter).toStdString();
    for (const QJsonValue& v : o.value("inputs").toArray()) {
        const QJsonObject io = v.toObject();
        d.inputs.push_back(InputDesc{io.value("name").toString().toStdString(), io.value("label").toString().toStdString(),
                                     io.value("mask").toBool(), io.value("optional").toBool()});
    }
    d.openGL = o.value("openGL").toString("false").toStdString();
    d.overlay = o.value("overlay").toInt();
    for (const QJsonValue& v : o.value("params").toArray()) {
        const QJsonObject po = v.toObject();
        ParamDesc p;
        p.name = po.value("name").toString().toStdString();
        p.label = po.value("label").toString().toStdString();
        p.type = po.value("type").toString().toStdString();
        for (const QJsonValue& x : po.value("default").toArray()) p.def.push_back(x.toDouble());
        p.min = po.value("min").toDouble(-1e9);
        p.max = po.value("max").toDouble(1e9);
        p.displayMin = po.value("displayMin").toDouble(p.min);
        p.displayMax = po.value("displayMax").toDouble(p.max);
        for (const QJsonValue& c : po.value("choices").toArray()) p.choices.push_back(c.toString().toStdString());
        p.stringDefault = po.value("string").toString().toStdString();
        p.doubleType = po.value("doubleType").toString().toStdString();
        p.normalised = po.value("normalised").toBool();
        p.animates = po.value("animates").toBool(true);
        p.secret = po.value("secret").toBool();
        d.params.push_back(p);
    }
    return d;
}

// Plug-ins that failed to load in this process, by id, with why: not tried again on every frame until a rescan.
std::map<std::string, std::string>& failedLoads() {
    static std::map<std::string, std::string> f;
    return f;
}

// The plug-in for `d`, loaded and described in this process.
std::function<void(const std::string&, bool)>& loadWatcher() {
    static auto* w = new std::function<void(const std::string&, bool)>;
    return *w;
}

Loaded* loadPlugin(const PluginDesc& d, std::string* error) {
    std::lock_guard<std::mutex> lock(gLoadMutex);
    auto& all = loadedPlugins();
    if (auto it = all.find(d.id); it != all.end()) return it->second.get();
    if (auto f = failedLoads().find(d.id); f != failedLoads().end()) {
        if (error) *error = f->second;
        return nullptr;
    }
    // (told before its code first runs, and after: a crash between is this plug-in's)
    struct Watch {
        const std::string& id;
        explicit Watch(const std::string& i) : id(i) {
            if (loadWatcher()) loadWatcher()(id, true);
        }
        ~Watch() {
            if (loadWatcher()) loadWatcher()(id, false);
        }
    } watch(d.id);
    auto failed = [&](const std::string& why) -> Loaded* {
        failedLoads()[d.id] = why;
        if (error) *error = why;
        return nullptr;
    };
    std::string why;
    Binary* bin = openBinary(d.binary, &why);
    if (!bin) return failed(why.empty() ? d.label + " could not be opened" : why);
    const int n = bin->count();
    for (int i = 0; i < n; ++i) {
        OfxPlugin* p = bin->get(i);
        if (!p || !p->pluginIdentifier || p->pluginIdentifier != d.identifier || int(p->pluginVersionMajor) != d.versionMajor) continue;
        auto l = describe(bin, i, &why);
        if (!l) return failed(why.empty() ? d.label + " failed to load" : why);
        Loaded* raw = l.get();
        all[d.id] = std::move(l);
        return raw;
    }
    return failed(d.label + " is no longer in " + d.binary);
}

// Makes an instance of `l` (kOfxActionCreateInstance) with the host's clip and parameter objects.
std::unique_ptr<Instance> createInstance(Loaded* l, std::string* error, bool interactive = false) {
    auto inst = std::make_unique<Instance>();
    inst->plugin = l;
    inst->floatImages = l->desc.floatImages;
    inst->colourStyle = l->desc.colour;
    EffectObj& o = inst->obj;
    o.instance = inst.get();
    o.props = l->filter->props;
    o.props.setString(kOfxPropType, {kOfxTypeImageEffectInstance});
    o.props.setString(kOfxImageEffectPropContext, {l->context});
    o.props.setPointer(kOfxPropInstanceData, nullptr);
    o.props.setDouble(kOfxImageEffectPropProjectSize, {1920, 1080});
    o.props.setDouble(kOfxImageEffectPropProjectExtent, {1920, 1080});
    o.props.setDouble(kOfxImageEffectPropProjectOffset, {0, 0});
    o.props.setDouble(kOfxImageEffectPropProjectPixelAspectRatio, {1.0});
    o.props.setDouble(kOfxImageEffectInstancePropEffectDuration, {1e6});
    o.props.setDouble(kOfxImageEffectPropFrameRate, {25.0});
    o.props.setInt(kOfxPropIsInteractive, {interactive ? 1 : 0});
    o.props.setInt(kOfxImageEffectInstancePropSequentialRender, {0});
    if (!inst->colourStyle.empty()) {
        o.props.setString(kOfxImageEffectPropColourManagementStyle, {inst->colourStyle});
        o.props.setString(kOfxImageEffectPropColourManagementConfig, {kNativeConfig});
    }
    o.params.props = l->filter->params.props;
    o.params.owner = inst.get();
    for (const auto& p : l->filter->params.params) {
        auto copy = std::make_unique<ParamObj>(*p);
        copy->owner = inst.get();
        o.params.params.push_back(std::move(copy));
    }
    for (const auto& c : l->filter->clips) {
        auto copy = std::make_unique<ClipObj>(*c);
        copy->effect = &o;
        PropertySet& cp = copy->props;
        // (a General plugin's further inputs are connected as its effect says, at each render)
        const bool extra = l->context == kOfxImageEffectContextGeneral && c->name != kOfxImageEffectOutputClipName &&
                           c->name != kOfxImageEffectSimpleSourceClipName;
        cp.setInt(kOfxImageClipPropConnected, {extra ? 0 : 1});
        cp.setString(kOfxImageEffectPropPixelDepth, {inst->floatImages ? kOfxBitDepthFloat : kOfxBitDepthByte});
        cp.setString(kOfxImageEffectPropComponents, {kOfxImageComponentRGBA});
        cp.setString(kOfxImageClipPropUnmappedPixelDepth, {inst->floatImages ? kOfxBitDepthFloat : kOfxBitDepthByte});
        cp.setString(kOfxImageClipPropUnmappedComponents, {kOfxImageComponentRGBA});
        cp.setString(kOfxImageEffectPropPreMultiplication, {kOfxImagePreMultiplied});
        cp.setDouble(kOfxImagePropPixelAspectRatio, {1.0});
        cp.setDouble(kOfxImageEffectPropFrameRate, {25.0});
        cp.setDouble(kOfxImageEffectPropFrameRange, {0, 1e6});
        cp.setDouble(kOfxImageEffectPropUnmappedFrameRange, {0, 1e6});
        cp.setDouble(kOfxImageEffectPropUnmappedFrameRate, {25.0});
        cp.setString(kOfxImageClipPropFieldOrder, {kOfxImageFieldNone});
        cp.setInt(kOfxImageClipPropContinuousSamples, {0});
        o.clips.push_back(std::move(copy));
    }
    if (!ok(l->plugin->mainEntry(kOfxActionCreateInstance, reinterpret_cast<OfxImageEffectHandle>(&o), nullptr, nullptr))) {
        if (error) *error = l->desc.label + " could not start";
        return nullptr;
    }
    ++gInstances;
    return inst;
}

void destroyInstance(std::unique_ptr<Instance> inst) {
    if (!inst) return;
    auto entry = inst->plugin->plugin->mainEntry;
    const OfxImageEffectHandle h = reinterpret_cast<OfxImageEffectHandle>(&inst->obj);
    // (what it set up on Montage's OpenGL context, let go there)
    if (inst->glAttached) gl::run([&] { entry(kOfxActionOpenGLContextDetached, h, nullptr, nullptr); });
    entry(kOfxActionDestroyInstance, h, nullptr, nullptr);
    --gLiveInstances;
}

// Instances belong to one clip effect each (a plug-in's per-instance state, such as an analysis or a loaded profile,
// never leaks between effects) and render one frame at a time: a render checks one out, making it if none of that
// effect's is free, and gives it back after. Free instances beyond a limit are destroyed, least recently used first,
// so threads coming and going do not pile them up.
struct PoolEntry {
    std::unique_ptr<Instance> inst;
    Loaded* plugin = nullptr;
    Id effect = 0;
    bool busy = false;
    uint64_t lastUse = 0;
};
std::mutex gPoolMutex;
std::vector<PoolEntry>& instancePool() {
    static std::vector<PoolEntry> pool;
    return pool;
}
constexpr size_t kIdleInstances = 16;

Instance* checkOut(Loaded* l, Id effect, std::string* error) {
    {
        std::lock_guard<std::mutex> lock(gPoolMutex);
        for (PoolEntry& e : instancePool())
            if (!e.busy && e.plugin == l && e.effect == effect) {
                e.busy = true;
                return e.inst.get();
            }
    }
    auto inst = createInstance(l, error);
    if (!inst) return nullptr;
    ++gLiveInstances;
    Instance* raw = inst.get();
    std::lock_guard<std::mutex> lock(gPoolMutex);
    instancePool().push_back(PoolEntry{std::move(inst), l, effect, true, 0});
    return raw;
}

void checkIn(Instance* inst) {
    static uint64_t clock = 0;
    std::vector<std::unique_ptr<Instance>> retire;
    {
        std::lock_guard<std::mutex> lock(gPoolMutex);
        auto& pool = instancePool();
        size_t idle = 0;
        for (PoolEntry& e : pool) {
            if (e.inst.get() == inst) e.busy = false, e.lastUse = ++clock;
            if (!e.busy) ++idle;
        }
        while (idle > kIdleInstances) {
            auto oldest = pool.end();
            for (auto it = pool.begin(); it != pool.end(); ++it)
                if (!it->busy && (oldest == pool.end() || it->lastUse < oldest->lastUse)) oldest = it;
            retire.push_back(std::move(oldest->inst));
            pool.erase(oldest);
            --idle;
        }
    }
    for (auto& r : retire) destroyInstance(std::move(r));
}

// Tells the instance which of its parameters changed since it last rendered (kOfxActionInstanceChanged, as a user edit),
// for plug-ins that cache work from their values.
void announceChanges(Instance& inst, const Effect& e, double t) {
    if (inst.fresh) {
        inst.fresh = false;
        inst.last = e;
        return;
    }
    std::vector<std::string> changed;
    auto differs = [&](const std::string& key) {
        const auto a = e.params.find(key);
        const auto b = std::as_const(inst.last.params).find(key);
        return (a == e.params.end()) != (b == inst.last.params.end()) || (a != e.params.end() && !(a->second == b->second));
    };
    for (const auto& p : inst.obj.params.params) {
        bool d = false;
        if (stringType(p->type)) {
            d = e.s("str." + p->name) != inst.last.s("str." + p->name);
        } else {
            for (int k = 0; k < dimensionsOf(p->type) && !d; ++k) d = differs(effectKey(p->type, p->name, k));
        }
        if (d) changed.push_back(p->name);
    }
    inst.last = e;
    if (changed.empty()) return;
    auto entry = inst.plugin->plugin->mainEntry;
    const OfxImageEffectHandle h = reinterpret_cast<OfxImageEffectHandle>(&inst.obj);
    PropertySet reason;
    reason.setString(kOfxPropChangeReason, {kOfxChangeUserEdited});
    entry(kOfxActionBeginInstanceChanged, h, handle(&reason), nullptr);
    for (const std::string& name : changed) {
        PropertySet args;
        args.setString(kOfxPropType, {kOfxTypeParameter});
        args.setString(kOfxPropName, {name});
        args.setString(kOfxPropChangeReason, {kOfxChangeUserEdited});
        args.setDouble(kOfxPropTime, {t});
        args.setDouble(kOfxImageEffectPropRenderScale, {inst.scale, inst.scale});
        entry(kOfxActionInstanceChanged, h, handle(&args), nullptr);
    }
    entry(kOfxActionEndInstanceChanged, h, handle(&reason), nullptr);
}

}  // namespace

// ---- Public -------------------------------------------------------------------------------------------------------------

int ParamDesc::dimensions() const { return dimensionsOf(type); }
bool ParamDesc::numeric() const { return dimensionsOf(type) > 0; }

std::vector<std::string> defaultSearchPaths() {
    std::vector<std::string> out;
    const QByteArray env = qgetenv("OFX_PLUGIN_PATH");
#ifdef _WIN32
    const char sep = ';';
#else
    const char sep = ':';
#endif
    for (const QByteArray& p : env.split(sep))
        if (!p.trimmed().isEmpty()) out.push_back(QString::fromLocal8Bit(p.trimmed()).toStdString());
#if defined(_WIN32)
    out.push_back("C:/Program Files/Common Files/OFX/Plugins");
#elif defined(__APPLE__)
    out.push_back("/Library/OFX/Plugins");
#else
    out.push_back("/usr/OFX/Plugins");
#endif
    return out;
}

std::vector<std::string> findBinaries(const std::vector<std::string>& dirs) {
#if defined(_WIN32)
    const QStringList archs = {"Win64"};
#elif defined(__APPLE__)
    const QStringList archs = {"MacOS", "MacOS-x86-64", "MacOS-arm-64"};
#elif defined(__aarch64__)
    const QStringList archs = {"Linux-aarch64", "Linux-arm-64"};
#else
    const QStringList archs = {"Linux-x86-64"};
#endif
    std::vector<std::string> out;
    for (const std::string& dir : dirs) {
        QDirIterator it(QString::fromStdString(dir), {"*.ofx.bundle"}, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString bundle = it.next();
            const QString name = QFileInfo(bundle).fileName().chopped(int(std::strlen(".bundle")));  // Name.ofx
            for (const QString& arch : archs) {
                const QString bin = bundle + "/Contents/" + arch + "/" + name;
                if (QFileInfo(bin).isFile()) {
                    out.push_back(QDir::cleanPath(bin).toStdString());
                    break;
                }
            }
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<PluginDesc> describeBinary(const std::string& binary, std::string* error) {
    std::lock_guard<std::mutex> lock(gLoadMutex);
    Binary* bin = openBinary(binary, error);
    if (!bin) return {};
    std::vector<PluginDesc> out;
    const int n = bin->count();
    for (int i = 0; i < n; ++i) {
        std::string err;
        auto l = describe(bin, i, &err);
        if (!l) continue;
        out.push_back(l->desc);
        loadedPlugins()[l->desc.id] = std::move(l);
    }
    if (out.empty() && error && error->empty()) *error = "No OpenFX image effects Montage can run in " + binary;
    return out;
}

std::string descriptionsToJson(const std::vector<PluginDesc>& ds) {
    QJsonArray a;
    for (const PluginDesc& d : ds) a.append(toJson(d));
    return QJsonDocument(a).toJson(QJsonDocument::Compact).toStdString();
}

std::vector<PluginDesc> descriptionsFromJson(const std::string& json) {
    std::vector<PluginDesc> out;
    for (const QJsonValue& v : QJsonDocument::fromJson(QByteArray::fromStdString(json)).array()) out.push_back(fromJson(v.toObject()));
    return out;
}

Registry& Registry::instance() {
    static Registry r;
    return r;
}

Registry::Registry() {
    cachePath_ = (QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/ofx-cache.json").toStdString();
    QString exe = QCoreApplication::applicationDirPath() + "/montage-plugin-probe";
#ifdef _WIN32
    exe += ".exe";
#endif
    probe_ = exe.toStdString();
}

void Registry::setCachePath(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    cachePath_ = path;
}
void Registry::setProbeExecutable(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    probe_ = path;
}
void Registry::setSearchPaths(const std::vector<std::string>& dirs) {
    std::lock_guard<std::mutex> lock(mutex_);
    paths_ = dirs;
}

void Registry::ensureScanned() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (scanned_) return;
    }
    scan();
}

int Registry::scan(std::vector<std::string>* log) {
    std::lock_guard<std::mutex> scanning(scanMutex_);  // one scan at a time
    std::string cachePath, probe;
    std::vector<std::string> dirs;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cachePath = cachePath_, probe = probe_;
        dirs = paths_.empty() ? defaultSearchPaths() : paths_;
    }
    // The cache: per binary, its size and time and what it held (or why it failed).
    QJsonObject cache;
    {
        QFile f(QString::fromStdString(cachePath));
        if (f.open(QIODevice::ReadOnly)) cache = QJsonDocument::fromJson(f.readAll()).object();
    }
    // A plug-in whose on-screen controls were running when Montage stopped without warning: blocked like one that
    // crashes on loading, until its binary changes.
    // Each process has its own guard file (its id in the name); one of this process, or of another Montage still
    // running, is not a crash.
    QStringList crashedBinaries;
    {
        QDir dir(QFileInfo(QString::fromStdString(cachePath)).absolutePath());
        for (const QString& name : dir.entryList({"ofx-interact-*.guard"}, QDir::Files)) {
            bool numeric = false;
            const qint64 pid = name.mid(13, name.size() - 13 - 6).toLongLong(&numeric);
            if (!numeric || pid == QCoreApplication::applicationPid() || processAlive(pid)) continue;
            QFile g(dir.filePath(name));
            if (g.open(QIODevice::ReadOnly)) {
                QByteArray b = g.readAll();
                b.truncate(b.indexOf('\0') < 0 ? b.size() : b.indexOf('\0'));
                // ('1' then the binary: a control was running; '0': none was. An older file is the binary alone.)
                if (b.startsWith("1\n")) crashedBinaries << QString::fromUtf8(b.mid(2)).trimmed();
                else if (!b.startsWith("0\n") && !b.trimmed().isEmpty()) crashedBinaries << QString::fromUtf8(b).trimmed();
            }
            g.close();
            QFile::remove(dir.filePath(name));
        }
    }
    QJsonObject fresh;
    std::vector<PluginDesc> found;
    std::vector<std::pair<std::string, std::string>> blocked;
    for (const std::string& bin : findBinaries(dirs)) {
        const QFileInfo fi(QString::fromStdString(bin));
        // (The last number is the description's version: a description from before it is made again.)
        // (3: since contexts, inputs, OpenGL and overlays were described)
        const QString stamp = QString::number(fi.size()) + ":" + QString::number(fi.lastModified().toMSecsSinceEpoch()) + ":3";
        QJsonObject entry = cache.value(QString::fromStdString(bin)).toObject();
        if (entry.value("stamp").toString() != stamp) {
            // New or changed: described in the probe, out of this process.
            entry = QJsonObject{{"stamp", stamp}};
            QProcess proc;
            proc.start(QString::fromStdString(probe), {"OFX", QString::fromStdString(bin)});
            const bool finished = proc.waitForFinished(30000);
            if (!finished) {
                proc.kill();
                proc.waitForFinished(2000);
                entry["error"] = "Timed out while loading";
            } else if (proc.exitStatus() != QProcess::NormalExit) {
                entry["error"] = "Crashed while loading";
            } else if (proc.exitCode() != 0) {
                entry["error"] = QString::fromUtf8(proc.readAllStandardError()).trimmed();
                if (entry.value("error").toString().isEmpty()) entry["error"] = "Could not be loaded";
            } else {
                entry["plugins"] = QJsonDocument::fromJson(proc.readAllStandardOutput()).array();
            }
            if (log) log->push_back((entry.contains("error") ? "blocked " : "probed ") + bin);
        } else if (log) {
            log->push_back("cached " + bin);
        }
        if (crashedBinaries.contains(QString::fromStdString(bin))) {
            entry = QJsonObject{{"stamp", stamp}, {"error", "Crashed while showing its on-screen controls"}};
            if (log) log->push_back("blocked " + bin);
        }
        fresh[QString::fromStdString(bin)] = entry;
        if (entry.contains("error")) blocked.push_back({bin, entry.value("error").toString().toStdString()});
        for (const QJsonValue& v : entry.value("plugins").toArray()) found.push_back(fromJson(v.toObject()));
    }
    QDir().mkpath(QFileInfo(QString::fromStdString(cachePath)).absolutePath());
    QSaveFile out(QString::fromStdString(cachePath));
    if (out.open(QIODevice::WriteOnly)) {
        out.write(QJsonDocument(fresh).toJson());
        out.commit();
    }
    std::sort(found.begin(), found.end(), [](const PluginDesc& a, const PluginDesc& b) { return a.label < b.label; });
    {
        std::lock_guard<std::mutex> loading(gLoadMutex);
        failedLoads().clear();  // tried again after a rescan
    }
    std::lock_guard<std::mutex> lock(mutex_);
    plugins_ = found;
    blocked_ = blocked;
    scanned_ = true;
    return int(plugins_.size());
}

std::vector<PluginDesc> Registry::plugins() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return plugins_;
}
std::vector<std::pair<std::string, std::string>> Registry::blocked() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return blocked_;
}
std::string Registry::guardPath() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return (QFileInfo(QString::fromStdString(cachePath_)).absolutePath() + "/ofx-interact-" +
            QString::number(QCoreApplication::applicationPid()) + ".guard")
        .toStdString();
}

void Registry::blockAfterCrash(const std::string& binary, const std::string& reason) {
    std::string cachePath;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cachePath = cachePath_;
        plugins_.erase(std::remove_if(plugins_.begin(), plugins_.end(), [&](const PluginDesc& d) { return d.binary == binary; }), plugins_.end());
        blocked_.erase(std::remove_if(blocked_.begin(), blocked_.end(), [&](const auto& b) { return b.first == binary; }), blocked_.end());
        blocked_.push_back({binary, reason});
    }
    // Kept in the cache with the file's stamp, so the next scan keeps it blocked until the file changes.
    std::lock_guard<std::mutex> scanning(scanMutex_);
    if (cachePath.empty()) return;
    QFile f(QString::fromStdString(cachePath));
    if (!f.open(QIODevice::ReadOnly)) return;
    QJsonObject cache = QJsonDocument::fromJson(f.readAll()).object();
    f.close();
    const QString key = QString::fromStdString(binary);
    QJsonObject entry = cache.value(key).toObject();
    if (!entry.contains("stamp")) return;
    cache[key] = QJsonObject{{"stamp", entry.value("stamp")}, {"error", QString::fromStdString(reason)}};
    QSaveFile out(QString::fromStdString(cachePath));
    if (out.open(QIODevice::WriteOnly)) {
        out.write(QJsonDocument(cache).toJson());
        out.commit();
    }
}

void Registry::adopt(const PluginDesc& d) {
    std::lock_guard<std::mutex> lock(mutex_);
    scanned_ = true;  // (never scans by itself)
    for (PluginDesc& have : plugins_)
        if (have.id == d.id) {
            have = d;
            return;
        }
    plugins_.push_back(d);
}

bool Registry::lookUp(const Effect& e, PluginDesc& d, std::string* error) {
    // Only a plug-in this computer's scan found and did not block runs: never a binary named by the project file, which
    // could be anything, or one that crashed the probe (or crashed while showing its controls).
    ensureScanned();
    if (find(e.s("ofx_id"), d)) return true;
    const std::vector<PluginDesc> saved = descriptionsFromJson(e.s("ofx_description"));
    const std::string name = effectName(e);
    if (error) *error = name + " is not installed";
    if (!saved.empty())
        for (const auto& [binary, why] : blocked())
            if (binary == saved.front().binary && error) *error = name + " is blocked: " + why;
    return false;
}

bool Registry::find(const std::string& id, PluginDesc& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const PluginDesc& d : plugins_)
        if (d.id == id) {
            out = d;
            return true;
        }
    return false;
}

bool isOfxType(const std::string& type) { return type.rfind(kTypePrefix, 0) == 0; }

namespace {

// Asks the plugin which colourspace it would like its pictures in (the first it lists that Montage can make) and which
// its output will be in, and tells it on its clips.
ColourPlan planColour(Instance& inst, const ColorSpace& working) {
    ColourPlan plan;
    const std::string& style = inst.colourStyle;
    const bool basic = style == kOfxImageEffectColourManagementBasic;
    const std::string workName = workingSpaceName(working, style);
    const NativeSpace* workNative = nativeSpace(workName);
    const bool workExact = !workNative || (workNative->primaries == working.primaries && workNative->transfer == working.transfer);
    auto basicName = [](const std::string& name) {
        const NativeSpace* n = nativeSpace(name);
        return (n && n->basic) || name == kOfxColourspaceOfxDisplaySdr || name == kOfxColourspaceOfxDisplayHdr || name == kOfxColourspaceOfxRaw;
    };
    auto isWorking = [&](const std::string& name) {
        if (basic && !basicName(name)) return false;
        if (name == workName) return workExact;
        if (name == kOfxColourspaceOfxDisplaySdr) return !working.sceneReferred && !working.hdr();
        if (name == kOfxColourspaceOfxDisplayHdr) return !working.sceneReferred && working.hdr();
        const NativeSpace* n = nativeSpace(name);
        return n && n->primaries == working.primaries && n->transfer == working.transfer;
    };
    auto entry = inst.plugin->plugin->mainEntry;
    const OfxImageEffectHandle h = reinterpret_cast<OfxImageEffectHandle>(&inst.obj);
    ClipObj* source = inst.obj.clip(kOfxImageEffectSimpleSourceClipName);
    for (auto& c : inst.obj.clips)
        if (!source && c->name != kOfxImageEffectOutputClipName) source = c.get();
    std::string chosen = workName;
    if (!workExact) plan.source = workNative;
    // (GetClipPreferences's outArgs hold what the host would do anyway.)
    PropertySet prefs;
    const char* depth = inst.floatImages ? kOfxBitDepthFloat : kOfxBitDepthByte;
    for (auto& c : inst.obj.clips) {
        prefs.setString(("OfxImageClipPropComponents_" + c->name).c_str(), {kOfxImageComponentRGBA});
        prefs.setString(("OfxImageClipPropDepth_" + c->name).c_str(), {depth});
        prefs.setDouble(("OfxImageClipPropPAR_" + c->name).c_str(), {1.0});
    }
    prefs.setDouble(kOfxImageEffectPropFrameRate, {25.0});
    prefs.setString(kOfxImageClipPropFieldOrder, {kOfxImageFieldNone});
    prefs.setString(kOfxImageEffectPropPreMultiplication, {kOfxImagePreMultiplied});
    prefs.setInt(kOfxImageClipPropContinuousSamples, {0});
    prefs.setInt(kOfxImageEffectFrameVarying, {0});
    if (source && ok(entry(kOfxImageEffectActionGetClipPreferences, h, nullptr, handle(&prefs))))
        for (const std::string& name : prefs.strings(("OfxImageClipPropPreferredColourspaces_" + source->name).c_str())) {
            if (isWorking(name) || (rawSpace(name) && (!basic || basicName(name)))) {
                chosen = name;
                plan.source = nullptr;
                break;
            }
            if (const NativeSpace* n = nativeSpace(name); n && (!basic || n->basic)) {
                chosen = name;
                plan.source = n;
                break;
            }
        }
    for (auto& c : inst.obj.clips)  // (not a mask: colour management is off for those)
        if (c->name != kOfxImageEffectOutputClipName && !c->props.getInt(kOfxImageClipPropIsMask))
            c->props.setString(kOfxImageClipPropColourspace, {chosen});
    // Its output: what it says, a clip's colourspace it names, or (by default) its source's.
    PropertySet in, out;
    in.setString(kOfxImageClipPropPreferredColourspaces, {workName});
    std::string made = chosen;
    if (entry(kOfxImageEffectActionGetOutputColourspace, h, handle(&in), handle(&out)) == kOfxStatOK)
        made = out.getString(kOfxImageClipPropColourspace, chosen);
    if (const std::string prefix = "OfxColourspace_"; made.rfind(prefix, 0) == 0) {
        const ClipObj* named = inst.obj.clip(made.substr(prefix.size()));
        made = named && named->name != kOfxImageEffectOutputClipName ? chosen : workName;
    }
    if (rawSpace(made)) plan.rawOutput = true;
    else if (!isWorking(made)) plan.output = nativeSpace(made);  // (one Montage cannot read is taken as the working space)
    if (ClipObj* o = inst.obj.clip(kOfxImageEffectOutputClipName)) o->props.setString(kOfxImageClipPropColourspace, {made});
    return plan;
}

// The plug-in for effect `e`, loaded, and an instance of it checked out (given back by the caller).
Instance* acquire(const Effect& e, PluginDesc& d, Loaded*& l, std::string* error) {
    if (!Registry::instance().lookUp(e, d, error)) return nullptr;
    l = loadPlugin(d, error);
    if (!l) return nullptr;
    return checkOut(l, e.id, error);
}

struct GiveBack {
    Instance* inst;
    ~GiveBack() {
        if (inst) checkIn(inst);
    }
};

// What one render is of.
struct Job {
    const Effect* effect = nullptr;
    double t = 0, scale = 1;
    int width = 0, height = 0;
    const Image* source = nullptr;  // a filter's
    const FrameFetch* fetch = nullptr;
    std::map<std::string, const Image*> images;  // a transition's
    double progress = -1;
};

// A General plugin's further inputs connected (or not) as the effect says, telling an instance that has rendered before
// when that changed.
void updateConnections(Instance& inst, const Effect& e) {
    if (inst.plugin->context != kOfxImageEffectContextGeneral) return;
    std::vector<std::string> changed;
    for (auto& c : inst.obj.clips) {
        if (c->name == kOfxImageEffectOutputClipName || c->name == kOfxImageEffectSimpleSourceClipName) continue;
        const int connected = e.s(inputKey(c->name)).empty() ? 0 : 1;
        if (c->props.getInt(kOfxImageClipPropConnected) == connected) continue;
        c->props.setInt(kOfxImageClipPropConnected, {connected});
        changed.push_back(c->name);
    }
    if (changed.empty() || inst.fresh) return;
    auto entry = inst.plugin->plugin->mainEntry;
    const OfxImageEffectHandle h = reinterpret_cast<OfxImageEffectHandle>(&inst.obj);
    PropertySet reason;
    reason.setString(kOfxPropChangeReason, {kOfxChangeUserEdited});
    entry(kOfxActionBeginInstanceChanged, h, handle(&reason), nullptr);
    for (const std::string& name : changed) {
        PropertySet args;
        args.setString(kOfxPropType, {kOfxTypeClip});
        args.setString(kOfxPropName, {name});
        args.setString(kOfxPropChangeReason, {kOfxChangeUserEdited});
        args.setDouble(kOfxPropTime, {inst.time});
        args.setDouble(kOfxImageEffectPropRenderScale, {inst.scale, inst.scale});
        entry(kOfxActionInstanceChanged, h, handle(&args), nullptr);
    }
    entry(kOfxActionEndInstanceChanged, h, handle(&reason), nullptr);
}

// Begin, Render and End sequence render; Render's status.
OfxStatus sendRender(Instance& inst, double t, bool gl) {
    auto entry = inst.plugin->plugin->mainEntry;
    const OfxImageEffectHandle h = reinterpret_cast<OfxImageEffectHandle>(&inst.obj);
    PropertySet seq;
    seq.setDouble(kOfxImageEffectPropFrameRange, {t, t});
    seq.setDouble(kOfxImageEffectPropFrameStep, {1.0});
    seq.setInt(kOfxPropIsInteractive, {0});
    seq.setDouble(kOfxImageEffectPropRenderScale, {inst.scale, inst.scale});
    seq.setInt(kOfxImageEffectPropSequentialRenderStatus, {0});
    seq.setInt(kOfxImageEffectPropInteractiveRenderStatus, {0});
    seq.setInt(kOfxImageEffectPropOpenGLEnabled, {gl ? 1 : 0});
    entry(kOfxImageEffectActionBeginSequenceRender, h, handle(&seq), nullptr);
    PropertySet args;
    args.setDouble(kOfxPropTime, {t});
    args.setString(kOfxImageEffectPropFieldToRender, {kOfxImageFieldNone});
    args.setInt(kOfxImageEffectPropRenderWindow, {0, 0, inst.width, inst.height});
    args.setDouble(kOfxImageEffectPropRenderScale, {inst.scale, inst.scale});
    args.setInt(kOfxImageEffectPropSequentialRenderStatus, {0});
    args.setInt(kOfxImageEffectPropInteractiveRenderStatus, {0});
    args.setInt(kOfxImageEffectPropRenderQualityDraft, {0});
    args.setInt(kOfxImageEffectPropOpenGLEnabled, {gl ? 1 : 0});
    const OfxStatus st = entry(kOfxImageEffectActionRender, h, handle(&args), nullptr);
    entry(kOfxImageEffectActionEndSequenceRender, h, handle(&seq), nullptr);
    return st;
}

std::atomic<bool> gUseGpu{true};

// Who renders each one-at-a-time plug-in now, and which one each thread waits for: a wait that would close a loop
// (this thread holds a plug-in another render waits for while waiting for the one it holds, as two of them stacked in
// opposite orders on two clips can) is refused rather than waited for forever.
struct RenderWaits {
    std::mutex m;
    struct Owner {
        std::thread::id thread;
        const std::atomic<bool>* glWaiting = nullptr;  // set while it waits for the OpenGL thread
    };
    std::map<const void*, Owner> owner;
    std::map<std::thread::id, const void*> waitingFor;
    // The thread doing the OpenGL thread's work while it waits (a render handed back to it), when that thread waits
    // itself: a holder waiting for the OpenGL thread waits on what this one waits for.
    std::thread::id forOpenGl;
};
RenderWaits& renderWaits() {
    static RenderWaits w;
    return w;
}

// Renders `job` with `inst` into `result` (on the GPU where the plug-in renders with OpenGL and it can, else on the CPU).
bool runRender(const PluginDesc& d, Loaded* l, Instance* inst, const Job& job, Image& result, std::string* error, const ColorSpace* working) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    // A plug-in that renders one at a time, reached again from its own render (an input whose clips run it too):
    // refused rather than waiting for itself.
    thread_local std::vector<const Loaded*> held;
    if (l->unsafe && std::find(held.begin(), held.end(), l) != held.end())
        return fail(d.label + " cannot render inside its own render (it renders one frame at a time)");
    // Connected inputs, made at this frame before the plug-in is locked (and on this thread, not OpenGL's).
    std::map<std::string, Image> prefetched;
    if (tInputs && *tInputs && job.effect)
        for (const InputDesc& in : d.inputs) {
            const std::string connection = job.effect->s(inputKey(in.name));
            if (connection.empty() || connection == "mask") continue;
            Image pic;
            if (!(*tInputs)(connection, job.t, pic) || pic.empty()) continue;
            if (pic.width != job.width || pic.height != job.height) pic = resizeImage(pic, job.width, job.height);
            prefetched[in.name] = std::move(pic);
        }
    std::unique_lock<std::mutex> serial(l->renderMutex, std::defer_lock);
    if (l->unsafe) {
        RenderWaits& w = renderWaits();
        const std::thread::id me = std::this_thread::get_id();
        while (!serial.try_lock()) {
            // Here for a render on the OpenGL thread, which waits for this one: if whoever renders the plug-in now is
            // itself waiting for the OpenGL thread, neither would ever go on, so this render is refused instead.
            if (gl::servingOpenGl()) {
                const std::atomic<bool>* holder = l->holderWaiting.load();
                if (holder && holder->load()) {
                    std::lock_guard g(w.m);
                    w.waitingFor.erase(me);
                    if (w.forOpenGl == me) w.forOpenGl = {};
                    return fail(d.label + " is busy in another render that waits for OpenGL (it renders one frame at a time)");
                }
            }
            {
                std::lock_guard g(w.m);
                w.waitingFor[me] = l;
                // (following who holds what each waits for, back to this thread: a loop)
                if (gl::servingOpenGl()) w.forOpenGl = me;
                const void* want = l;
                for (int hops = 0; hops < 64; ++hops) {
                    const auto o = w.owner.find(want);
                    if (o == w.owner.end()) break;
                    if (o->second.thread == me) {
                        w.waitingFor.erase(me);
                        if (w.forOpenGl == me) w.forOpenGl = {};
                        return fail(d.label + " is busy in another render that waits for one this render holds (it renders one frame at a time)");
                    }
                    auto next = w.waitingFor.find(o->second.thread);
                    // (a holder waiting for the OpenGL thread waits on what that thread waits for)
                    if (next == w.waitingFor.end() && o->second.glWaiting && o->second.glWaiting->load() && w.forOpenGl != std::thread::id()) {
                        if (w.forOpenGl == me) break;  // (this thread being it: the check above, on its holder, covers it)
                        next = w.waitingFor.find(w.forOpenGl);
                    }
                    if (next == w.waitingFor.end()) break;
                    want = next->second;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        {
            std::lock_guard g(w.m);
            w.waitingFor.erase(me);
            w.owner[l] = {me, gl::waitingFlag()};
            if (w.forOpenGl == me) w.forOpenGl = {};
        }
        l->holderWaiting = gl::waitingFlag();
        held.push_back(l);
    }
    struct Release {
        std::vector<const Loaded*>& held;
        Loaded* l;
        ~Release() {
            if (!l) return;
            held.pop_back();
            l->holderWaiting = nullptr;
            RenderWaits& w = renderWaits();
            std::lock_guard g(w.m);
            w.owner.erase(l);
        }
    } release{held, l->unsafe ? l : nullptr};
    Image out(job.width, job.height);
    inst->effect = job.effect;
    inst->time = job.t;
    inst->scale = std::max(1e-3, job.scale);
    inst->width = job.width, inst->height = job.height;
    inst->progress = job.progress;
    inst->fetch = job.fetch;
    inst->source = job.source;
    inst->images = job.images;
    inst->output = &out;
    inst->error.clear();
    inst->wrote = false;
    inst->convert = nullptr;
    inst->inputs = tInputs;
    inst->prefetched = std::move(prefetched);
    inst->ownMask = tOwnMask;
    const double fullW = job.width / inst->scale, fullH = job.height / inst->scale;
    inst->projectW = fullW, inst->projectH = fullH;
    inst->obj.props.setDouble(kOfxImageEffectPropProjectSize, {fullW, fullH});
    inst->obj.props.setDouble(kOfxImageEffectPropProjectExtent, {fullW, fullH});
    updateConnections(*inst, *job.effect);
    announceChanges(*inst, *job.effect, job.t);
    // Colour management: the pictures in the colourspace the plugin asked for, its output back into the working space.
    const ColorSpace& space = working ? *working : rec709Space();
    inst->space = &space;
    const ColourPlan plan = inst->colourStyle.empty() ? ColourPlan{} : planColour(*inst, space);
    Image convertedSource;
    std::map<std::string, Image> convertedImages;
    FrameFetch convertedFetch;
    if (const NativeSpace* to = plan.source) {
        inst->convert = to;
        if (job.source) {
            convertedSource = *job.source;
            reencode(convertedSource, space.primaries, space.transfer, to->primaries, to->transfer);
            inst->source = &convertedSource;
        }
        for (const auto& [name, img] : job.images)
            if (img) {
                Image& c = convertedImages[name] = *img;
                reencode(c, space.primaries, space.transfer, to->primaries, to->transfer);
                inst->images[name] = &c;
            }
        if (job.fetch && *job.fetch) {
            const FrameFetch& fetch = *job.fetch;
            convertedFetch = [&fetch, &space, to](double at, Image& other) {
                if (!fetch(at, other)) return false;
                reencode(other, space.primaries, space.transfer, to->primaries, to->transfer);
                return true;
            };
            inst->fetch = &convertedFetch;
        }
    }
    auto clear = [&] {
        inst->effect = nullptr;
        inst->source = nullptr;
        inst->images.clear();
        inst->fetch = nullptr;
        inst->output = nullptr;
        inst->convert = nullptr;
        inst->inputs = nullptr;
        inst->prefetched.clear();
        inst->ownMask = nullptr;
    };
    // OpenGL, where the plug-in renders with it.
    const bool gpu = d.openGL != "false" && gUseGpu && !inst->glRefused && !tNoGl && gl::available();
    if (d.openGL == "needed" && !gpu) {
        clear();
        return fail(d.label + " needs OpenGL, which is not available here");
    }
    OfxStatus st = kOfxStatFailed;
    bool rendered = false;
    if (gpu) {
        auto entry = l->plugin->mainEntry;
        const OfxImageEffectHandle h = reinterpret_cast<OfxImageEffectHandle>(&inst->obj);
        const ExpressionContext where = currentExpressionContext();
        // (whatever is thrown, the target and textures are let go and the render counts as failed, not as a picture)
        const bool ran = gl::run([&] {
            struct Textures {
                Instance* inst;
                ~Textures() {
                    for (unsigned t : inst->textures) gl::destroyTexture(t);
                    inst->textures.clear();
                }
            } textures{inst};
            ExpressionScope scope(where.project, where.sequence, where.clip, where.cancel);
            if (!inst->glAttached) {
                if (entry(kOfxActionOpenGLContextAttached, h, nullptr, nullptr) == kOfxStatFailed) inst->glRefused = true;
                else inst->glAttached = true;
            }
            if (inst->glAttached && gl::makeTarget(inst->target, job.width, job.height)) {
                struct Target {
                    Instance* inst;
                    ~Target() {
                        gl::release(inst->target);
                        inst->gl = false;
                    }
                } target{inst};
                inst->gl = true;
                gl::bind(inst->target);
                gl::clear();
                st = sendRender(*inst, job.t, true);
                if (ok(st)) gl::read(inst->target, out);
            }
        });
        if (!ran) st = kOfxStatFailed;
        rendered = ok(st);
        if (rendered) ++gGpuRenders;
        else if (d.openGL == "needed") {
            clear();
            return fail(inst->error.empty() ? d.label + " failed to render with OpenGL" : d.label + ": " + inst->error);
        }
        // (else on the CPU, as kOfxStatGLRenderFailed asks)
    }
    if (!rendered) {
        inst->error.clear();
        inst->wrote = false;
        st = sendRender(*inst, job.t, false);
        if (ok(st) && (!inst->wrote || out.px.size() != size_t(job.width) * size_t(job.height) * 4)) {
            clear();
            return fail(d.label + " gave no picture back");
        }
    }
    clear();
    if (!ok(st)) return fail(inst->error.empty() ? d.label + " failed to render" : d.label + ": " + inst->error);
    if (const NativeSpace* from = plan.output) reencode(out, from->primaries, from->transfer, space.primaries, space.transfer);
    result = std::move(out);
    return true;
}

}  // namespace

Effect makeEffect(Project& p, const PluginDesc& d) {
    Effect e = montage::makeEffect(p, "ofx");
    e.strings["ofx_id"] = d.id;
    e.strings["ofx_name"] = d.label;
    e.strings["ofx_group"] = d.group;
    e.strings["ofx_binary"] = d.binary;
    e.strings["ofx_identifier"] = d.identifier;
    e.strings["ofx_version"] = std::to_string(d.versionMajor);
    e.strings["ofx_description"] = descriptionsToJson({d});
    // A General plugin's further inputs, connected to nothing at first.
    for (const InputDesc& in : d.inputs) e.strings[inputKey(in.name)] = "";
    for (const ParamDesc& pd : d.params) {
        if (pd.secret) continue;
        if (d.transition() && pd.name == kOfxImageEffectTransitionParamName) continue;  // (the transition's progress drives it)
        if (stringType(pd.type)) {
            e.strings["str." + pd.name] = pd.stringDefault;
            continue;
        }
        const int n = pd.dimensions();
        if (n == 0) continue;
        const bool colour = pd.type == kOfxParamTypeRGB || pd.type == kOfxParamTypeRGBA;
        for (int k = 0; k < n; ++k) {
            const std::string key = effectKey(pd.type, pd.name, k);
            e.params[key] = Param(k < int(pd.def.size()) ? pd.def[size_t(k)] : 0.0);
            if (colour && k > 0 && k < 3) continue;  // one Colour row for r, g, b
            montage::ParamInfo meta;
            static const char* rgba[] = {"", "", "", " alpha"};
            static const char* xyz[] = {" X", " Y", " Z"};
            meta.label = pd.label + (n == 1 ? "" : colour ? rgba[k] : xyz[k]);
            const bool bounded = pd.displayMin > -1e8 && pd.displayMax < 1e8 && pd.displayMax > pd.displayMin;
            meta.min = bounded ? std::max(pd.displayMin, pd.min) : (pd.min > -1e8 ? pd.min : 0.0);
            meta.max = bounded ? std::min(pd.displayMax, pd.max) : (pd.max < 1e8 ? pd.max : 100.0);
            if (meta.max <= meta.min) meta.max = meta.min + 1;
            meta.def = k < int(pd.def.size()) ? pd.def[size_t(k)] : 0.0;
            meta.step = integerType(pd.type) ? 1 : 0.01;
            meta.keyframeable = pd.animates;
            if (pd.type == kOfxParamTypeBoolean) meta.kind = ParamKind::Bool;
            else if (pd.type == kOfxParamTypeChoice) {
                meta.kind = ParamKind::Choice;
                meta.choices = pd.choices;
                meta.min = 0;
                meta.max = std::max(0, int(pd.choices.size()) - 1);
            } else if (colour && k == 0) {
                meta.kind = ParamKind::Color;
                meta.defG = pd.def.size() > 1 ? pd.def[1] : 0;
                meta.defB = pd.def.size() > 2 ? pd.def[2] : 0;
            } else if (pd.doubleType == kOfxParamDoubleTypeAngle) {
                meta.kind = ParamKind::Angle;
            }
            const std::string metaKey = colour && k == 0 ? key.substr(6, key.size() - 6 - 2) : key.substr(6);
            e.strings["meta." + metaKey] = pluginParamMeta(meta);
        }
    }
    return e;
}

std::string effectName(const Effect& e) {
    auto it = e.strings.find("ofx_name");
    return it != e.strings.end() && !it->second.empty() ? it->second : "OpenFX Plugin";
}

bool applyEffect(const Effect& e, double t, Image& img, double scale, const FrameFetch& fetch, std::string* error, const ColorSpace* working) {
    if (img.empty()) return true;
    if (remote::isolated()) {
        // In a plug-in host (render/OfxRemote.h).
        PluginDesc d;
        if (!Registry::instance().lookUp(e, d, error)) return false;
        if (d.transition() || d.generator()) {
            if (error) *error = d.label + (d.transition() ? " is a transition" : " is a generator") + ", not a clip effect";
            return false;
        }
        Image out;
        if (!remote::render(remote::Kind::Effect, e, d, t, 0, scale, img.width, img.height, &img, nullptr, out, &fetch, error, working))
            return false;
        img = std::move(out);
        return true;
    }
    PluginDesc d;
    Loaded* l = nullptr;
    GiveBack inst{acquire(e, d, l, error)};
    if (!inst.inst) return false;
    if (d.transition() || d.generator()) {
        if (error) *error = d.label + (d.transition() ? " is a transition" : " is a generator") + ", not a clip effect";
        return false;
    }
    Job job;
    job.effect = &e;
    job.t = t;
    job.scale = scale;
    job.width = img.width, job.height = img.height;
    job.source = &img;
    job.fetch = &fetch;
    Image out;
    if (!runRender(d, l, inst.inst, job, out, error, working)) return false;
    img = std::move(out);
    return true;
}

bool applyTransition(const Effect& e, double t, double progress, const Image& from, const Image& to, Image& out, double scale, std::string* error,
                     const ColorSpace* working) {
    const Image& sized = from.empty() ? to : from;
    if (sized.empty()) {
        out = Image();
        return true;
    }
    const int w = sized.width, h = sized.height;
    // (a side with nothing is transparent; the other is fitted to the first)
    Image a = from.empty() ? Image(w, h) : from;
    Image b = to.empty() ? Image(w, h) : to.width == w && to.height == h ? to : resizeImage(to, w, h);
    if (remote::isolated()) {
        PluginDesc d;
        if (!Registry::instance().lookUp(e, d, error)) return false;
        if (!d.transition()) {
            if (error) *error = d.label + " is not a transition";
            return false;
        }
        return remote::render(remote::Kind::Transition, e, d, t, std::clamp(progress, 0.0, 1.0), scale, w, h, &a, &b, out, nullptr, error, working);
    }
    PluginDesc d;
    Loaded* l = nullptr;
    GiveBack inst{acquire(e, d, l, error)};
    if (!inst.inst) return false;
    if (!d.transition()) {
        if (error) *error = d.label + " is not a transition";
        return false;
    }
    Job job;
    job.effect = &e;
    job.t = t;
    job.scale = scale;
    job.width = w, job.height = h;
    job.images = {{kOfxImageEffectTransitionSourceFromClipName, &a}, {kOfxImageEffectTransitionSourceToClipName, &b}};
    job.progress = std::clamp(progress, 0.0, 1.0);
    return runRender(d, l, inst.inst, job, out, error, working);
}

bool renderGenerator(const Effect& e, double t, int w, int h, double scale, Image& out, std::string* error, const ColorSpace* working) {
    if (w <= 0 || h <= 0) return false;
    if (remote::isolated()) {
        PluginDesc d;
        if (!Registry::instance().lookUp(e, d, error)) return false;
        if (!d.generator()) {
            if (error) *error = d.label + " is not a generator";
            return false;
        }
        return remote::render(remote::Kind::Generator, e, d, t, 0, scale, w, h, nullptr, nullptr, out, nullptr, error, working);
    }
    PluginDesc d;
    Loaded* l = nullptr;
    GiveBack inst{acquire(e, d, l, error)};
    if (!inst.inst) return false;
    if (!d.generator()) {
        if (error) *error = d.label + " is not a generator";
        return false;
    }
    Job job;
    job.effect = &e;
    job.t = t;
    job.scale = scale;
    job.width = w, job.height = h;
    return runRender(d, l, inst.inst, job, out, error, working);
}

bool PluginDesc::transition() const { return context == kOfxImageEffectContextTransition; }
bool PluginDesc::generator() const { return context == kOfxImageEffectContextGenerator; }

std::string inputKey(const std::string& clip) { return "input." + clip; }

bool usesOwnMask(const Effect& e) {
    if (e.type != "ofx") return false;
    for (const auto& [key, value] : e.strings)
        if (key.rfind("input.", 0) == 0 && value == "mask") return true;
    return false;
}

// (each scope keeps its own resolver: one running is never assigned over by a scope it opens)
InputScope::InputScope(InputResolver resolve) : resolve_(std::move(resolve)), previous_(tInputs) { tInputs = &resolve_; }
InputScope::~InputScope() { tInputs = previous_; }
const InputResolver& currentInputs() {
    static const InputResolver none;
    return tInputs ? *tInputs : none;
}
OwnMaskScope::OwnMaskScope(const std::vector<float>* matte) : previous_(tOwnMask) { tOwnMask = matte; }
const std::vector<float>* currentOwnMask() { return tOwnMask; }
OwnMaskScope::~OwnMaskScope() { tOwnMask = previous_; }

bool initializeGpu(std::string* why) { return gl::initialize(why); }
bool gpuAvailable(std::string* why) { return gl::available(why); }
void setGpuRendering(bool on) { gUseGpu = on; }
bool gpuRendering() { return gUseGpu && gl::available(); }
int gpuRenders() { return gGpuRenders.load(); }

// ---- On-screen controls ---------------------------------------------------------------------------------------------

double View::pixelScale() const {
    const double screen = std::hypot(m[0], m[3]);
    return screen > 1e-12 ? 1.0 / screen : 1.0;
}
std::array<double, 2> View::toView(double x, double y) const { return {m[0] * x + m[1] * y + m[2], m[3] * x + m[4] * y + m[5]}; }
std::array<double, 2> View::toCanonical(double vx, double vy) const {
    const double det = m[0] * m[4] - m[1] * m[3];
    if (std::fabs(det) < 1e-18) return {0, 0};
    const double dx = vx - m[2], dy = vy - m[5];
    return {(m[4] * dx - m[1] * dy) / det, (-m[3] * dx + m[0] * dy) / det};
}

void paintOverlay(QPainter& p, const Overlay& o, const View& v) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    if (!o.gl.isNull()) p.drawImage(QPointF(0, 0), o.gl);
    auto at = [&](const std::array<double, 2>& c) {
        const auto w = v.toView(c[0], c[1]);
        return QPointF(w[0], w[1]);
    };
    for (const DrawOp& op : o.ops) {
        const QColor colour = QColor::fromRgbF(std::clamp(op.colour[0], 0.f, 1.f), std::clamp(op.colour[1], 0.f, 1.f), std::clamp(op.colour[2], 0.f, 1.f),
                                               std::clamp(op.colour[3], 0.f, 1.f));
        QPen pen(colour, op.width <= 0 ? 1.0 : double(op.width));
        pen.setCosmetic(true);
        static const Qt::PenStyle styles[] = {Qt::SolidLine, Qt::DotLine, Qt::DashLine, Qt::DashLine, Qt::DashDotLine};
        pen.setStyle(styles[std::clamp(op.stipple, 0, 4)]);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        QPolygonF poly;
        for (const auto& c : op.points) poly << at(c);
        switch (op.kind) {
            case DrawOp::Lines:
                for (int i = 0; i + 1 < poly.size(); i += 2) p.drawLine(poly[i], poly[i + 1]);
                break;
            case DrawOp::LineStrip: p.drawPolyline(poly); break;
            case DrawOp::LineLoop: p.drawPolygon(poly); break;
            case DrawOp::Rectangle:
            case DrawOp::Polygon: {
                QPolygonF fill = poly;
                if (op.kind == DrawOp::Rectangle && op.points.size() == 2) {
                    const auto& a = op.points[0];
                    const auto& b = op.points[1];
                    fill = QPolygonF{at({a[0], a[1]}), at({b[0], a[1]}), at({b[0], b[1]}), at({a[0], b[1]})};
                }
                p.setPen(Qt::NoPen);
                p.setBrush(colour);
                p.drawPolygon(fill);
                break;
            }
            case DrawOp::Ellipse: {
                if (op.points.size() != 2) break;
                const double cx = (op.points[0][0] + op.points[1][0]) / 2, cy = (op.points[0][1] + op.points[1][1]) / 2;
                const double rx = std::fabs(op.points[1][0] - op.points[0][0]) / 2, ry = std::fabs(op.points[1][1] - op.points[0][1]) / 2;
                QPolygonF ring;
                for (int k = 0; k < 96; ++k) {
                    const double a = 2 * M_PI * k / 96;
                    ring << at({cx + rx * std::cos(a), cy + ry * std::sin(a)});
                }
                p.drawPolygon(ring);
                break;
            }
            case DrawOp::Text: {
                if (poly.isEmpty()) break;
                const QFontMetricsF fm(p.font());
                const QString text = QString::fromStdString(op.text);
                const double w = fm.horizontalAdvance(text);
                double x = poly[0].x(), y = poly[0].y();
                const bool left = op.align & kOfxDrawTextAlignmentLeft, right = op.align & kOfxDrawTextAlignmentRight;
                if (left && right) x -= w / 2;
                else if (right) x -= w;
                const bool top = op.align & kOfxDrawTextAlignmentTop, base = op.align & kOfxDrawTextAlignmentBaseline;
                if (top && base) y += (fm.ascent() - fm.descent()) / 2;
                else if (top) y += fm.ascent();
                else if (op.align & kOfxDrawTextAlignmentBottom) y -= fm.descent();
                p.drawText(QPointF(x, y), text);
                break;
            }
        }
    }
    p.restore();
}

namespace {

// While a plug-in's on-screen control runs, a file names its binary; Montage stopping then (a crash inside it) leaves
// the file saying so, and the next scan blocks the plug-in (Registry::scan). The file is made once and kept mapped:
// marking it costs a few bytes in memory (which the system writes out even after a crash), not a file written and
// removed on every mouse move. Its first byte is '1' while a control runs, '0' between; then the binary's path.
// Plug-in renders are not guarded: they run all through playback, so a crash anywhere then would block them.
thread_local int tGuardDepth = 0;
struct GuardFile {
    std::mutex mutex;
    QFile file;
    uchar* map = nullptr;
    static constexpr qint64 kSize = 4096;
    static GuardFile& instance() {
        static GuardFile* g = new GuardFile;  // (never destroyed: its mapping lasts the process)
        return *g;
    }
    void mark(const std::string& binary) {
        std::lock_guard<std::mutex> lock(mutex);
        if (map && file.size() < kSize) {  // (cut short by someone else: made again, never written past its end)
            file.unmap(map);
            file.close();
            map = nullptr;
        }
        if (!map) {
            file.setFileName(QString::fromStdString(Registry::instance().guardPath()));
            if (!file.open(QIODevice::ReadWrite | QIODevice::Truncate) || !file.resize(kSize)) return;
            map = file.map(0, kSize);
            if (!map) return;
        }
        const size_t n = std::min(binary.size(), size_t(kSize) - 3);
        std::memcpy(map + 2, binary.data(), n);
        map[2 + n] = '\0';
        map[1] = '\n';
        map[0] = '1';
    }
    void clear() {
        std::lock_guard<std::mutex> lock(mutex);
        if (map && file.size() >= kSize) map[0] = '0';
    }
};
struct CrashGuard {
    bool on = false;
    explicit CrashGuard(const std::string& binary) {
        // (in a plug-in host a crash ends only the host, and Montage blames the plug-in itself)
        if (tGuardDepth++ > 0 || plugins::runningInHost()) return;
        GuardFile::instance().mark(binary);
        on = true;
    }
    ~CrashGuard() {
        --tGuardDepth;
        if (on) GuardFile::instance().clear();
    }
};

}  // namespace

struct Interact::Impl {
    PluginDesc desc;
    Loaded* plugin = nullptr;
    std::unique_ptr<Instance> inst;
    InteractObj descriptor, obj;
    bool created = false;

    ~Impl() {
        if (created) {
            CrashGuard guard(desc.binary);
            plugin->overlay(kOfxActionDestroyInstance, &obj, nullptr, nullptr);
        }
        if (inst) destroyInstance(std::move(inst));
    }
    // The instance reading `e` at `t`, its frame as the view says.
    void prepare(const Effect& e, double t, const View& v) {
        inst->effect = &e;
        inst->time = t;
        inst->scale = std::max(1e-3, v.renderScale);
        inst->projectW = v.frameW > 0 ? v.frameW : inst->projectW;
        inst->projectH = v.frameH > 0 ? v.frameH : inst->projectH;
        inst->width = int(std::lround(inst->projectW * inst->scale));
        inst->height = int(std::lround(inst->projectH * inst->scale));
        inst->obj.props.setDouble(kOfxImageEffectPropProjectSize, {inst->projectW, inst->projectH});
        inst->obj.props.setDouble(kOfxImageEffectPropProjectExtent, {inst->projectW, inst->projectH});
        const double ps = v.pixelScale();
        obj.props.setDouble(kOfxInteractPropPixelScale, {ps, ps});
        obj.props.setInt(kOfxInteractPropViewportSize, {v.width, v.height});
    }
    PropertySet args(const View& v, double t) const {
        PropertySet a;
        const double ps = v.pixelScale();
        a.setPointer(kOfxPropEffectInstance, reinterpret_cast<void*>(&inst->obj));
        a.setDouble(kOfxInteractPropPixelScale, {ps, ps});
        a.setDouble(kOfxInteractPropBackgroundColour, {0, 0, 0});
        a.setDouble(kOfxPropTime, {t});
        a.setDouble(kOfxImageEffectPropRenderScale, {inst->scale, inst->scale});
        return a;
    }
    void done() {
        inst->effect = nullptr;
        inst->editing = nullptr;
    }
};

Interact::Interact(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Interact::Interact(std::unique_ptr<remote::Controls> r) : remote_(std::move(r)) {}
Interact::~Interact() = default;

bool Interact::failed() const { return failed_ || (remote_ && remote_->failed()); }
bool Interact::unresponsive() const { return remote_ && !remote_->failed() && remote_->unresponsive(); }
std::string Interact::failure() const { return remote_ ? remote_->failure() : std::string(); }
bool Interact::waiting() const { return remote_ && remote_->waiting(); }
bool Interact::lateEdits(Effect& changes) { return remote_ && remote_->lateEdits(changes); }
bool Interact::tidyClosed() { return remote::drainClosedControls(); }

void setLoadWatcher(std::function<void(const std::string&, bool)> watch) {
    std::lock_guard<std::mutex> lock(gLoadMutex);
    loadWatcher() = std::move(watch);
}

std::unique_ptr<Interact> Interact::open(const Effect& e, std::string* error) {
    if (e.type != "ofx") return nullptr;
    Registry& registry = Registry::instance();
    registry.ensureScanned();
    PluginDesc d;
    if (!registry.find(e.s("ofx_id"), d) || d.overlay == 0) return nullptr;
    if (remote::isolated()) {
        auto controls = remote::Controls::open(e, d, error);
        return controls ? std::unique_ptr<Interact>(new Interact(std::move(controls))) : nullptr;
    }
    Loaded* l = loadPlugin(d, error);
    if (!l || !l->overlay) return nullptr;
    auto impl = std::make_unique<Impl>();
    impl->desc = d;
    impl->plugin = l;
    CrashGuard guard(d.binary);
    impl->inst = createInstance(l, error, true);
    if (!impl->inst) return nullptr;
    ++gLiveInstances;
    for (InteractObj* o : {&impl->descriptor, &impl->obj}) {
        PropertySet& p = o->props;
        p.setInt(kOfxInteractPropHasAlpha, {1});
        p.setInt(kOfxInteractPropBitDepth, {8});
        p.setPointer(kOfxPropInstanceData, nullptr);
        p.setDouble(kOfxInteractPropPixelScale, {1, 1});
        p.setDouble(kOfxInteractPropBackgroundColour, {0, 0, 0});
        p.setDouble(kOfxInteractPropSuggestedColour, {1, 1, 1});
        p.setString(kOfxInteractPropSlaveToParam, {});
        p.setInt(kOfxInteractPropViewportSize, {0, 0});
    }
    impl->obj.props.setPointer(kOfxPropEffectInstance, reinterpret_cast<void*>(&impl->inst->obj));
    l->overlay(kOfxActionDescribe, &impl->descriptor, nullptr, nullptr);
    if (!ok(l->overlay(kOfxActionCreateInstance, &impl->obj, nullptr, nullptr))) {
        if (error) *error = d.label + "'s on-screen controls could not start";
        return nullptr;
    }
    impl->created = true;
    PropertySet focus = impl->args(View{}, 0);
    l->overlay(kOfxInteractActionGainFocus, &impl->obj, handle(&focus), nullptr);
    return std::unique_ptr<Interact>(new Interact(std::move(impl)));
}

bool Interact::draw(const Effect& e, double t, const View& v, Overlay& out) {
    out = Overlay{};
    if (failed() || v.width <= 0 || v.height <= 0) return false;
    if (remote_) return remote_->draw(e, t, v, out);
    Impl& d = *impl_;
    d.prepare(e, t, v);
    DrawCtx ctx;
    ctx.out = &out;
    PropertySet a = d.args(v, t);
    a.setPointer(kOfxInteractPropDrawContext, &ctx);
    OfxStatus st = kOfxStatReplyDefault;
    if (d.plugin->overlayVersion == 1 && gl::available()) {
        // Drawn with OpenGL, in canonical coordinates, into a picture the size of the view: ahead of renders waiting
        // for the OpenGL thread, and guarded only while the control itself draws (not while it waits).
        gl::run(
            [&] {
                gl::Target target;
                if (!gl::makeTarget(target, v.width, v.height)) return;
                gl::bind(target);
                gl::clear();
                gl::overlayProjection(v.width, v.height, v.m);
                {
                    CrashGuard guard(d.desc.binary);
                    st = d.plugin->overlay(kOfxInteractActionDraw, &d.obj, handle(&a), nullptr);
                }
                out.gl = gl::readOverlay(target);
                gl::release(target);
            },
            true);
    } else if (d.plugin->overlayVersion == 2) {
        CrashGuard guard(d.desc.binary);
        st = d.plugin->overlay(kOfxInteractActionDraw, &d.obj, handle(&a), nullptr);
    }  // (an OpenGL control without OpenGL here draws nothing)
    d.done();
    if (st == kOfxStatFailed || st == kOfxStatErrFatal) {
        failed_ = true;
        return false;
    }
    return true;
}

bool Interact::pen(Pen kind, const Effect& e, double t, const View& v, double vx, double vy, double pressure, Effect& edited, bool& changed) {
    changed = false;
    if (failed()) return false;
    if (remote_) return remote_->pen(int(kind), e, t, v, vx, vy, pressure, edited, changed);
    Impl& d = *impl_;
    CrashGuard guard(d.desc.binary);
    d.prepare(e, t, v);
    edited = e;
    d.inst->effect = &edited;
    d.inst->editing = &edited;
    d.inst->edited = false;
    PropertySet a = d.args(v, t);
    const auto c = v.toCanonical(vx, vy);
    a.setDouble(kOfxInteractPropPenPosition, {c[0], c[1]});
    a.setInt(kOfxInteractPropPenViewportPosition, {int(std::lround(vx)), int(std::lround(v.height - vy))});  // (y up)
    a.setDouble(kOfxInteractPropPenPressure, {pressure});
    const char* action = kind == Pen::Down ? kOfxInteractActionPenDown : kind == Pen::Up ? kOfxInteractActionPenUp : kOfxInteractActionPenMotion;
    const OfxStatus st = d.plugin->overlay(action, &d.obj, handle(&a), nullptr);
    changed = d.inst->edited;
    d.done();
    if (st == kOfxStatFailed || st == kOfxStatErrFatal) failed_ = true;
    return st == kOfxStatOK;
}

bool Interact::key(bool down, int keysym, const std::string& text, const Effect& e, double t, const View& v, Effect& edited, bool& changed) {
    changed = false;
    if (failed()) return false;
    if (remote_) return remote_->key(down, keysym, text, e, t, v, edited, changed);
    Impl& d = *impl_;
    CrashGuard guard(d.desc.binary);
    d.prepare(e, t, v);
    edited = e;
    d.inst->effect = &edited;
    d.inst->editing = &edited;
    d.inst->edited = false;
    PropertySet a = d.args(v, t);
    a.setInt(kOfxPropKeySym, {keysym});
    a.setString(kOfxPropKeyString, {text});
    const OfxStatus st = d.plugin->overlay(down ? kOfxInteractActionKeyDown : kOfxInteractActionKeyUp, &d.obj, handle(&a), nullptr);
    changed = d.inst->edited;
    d.done();
    if (st == kOfxStatFailed || st == kOfxStatErrFatal) failed_ = true;
    return st == kOfxStatOK;
}

void Interact::focus(bool gained, const Effect& e, double t, const View& v) {
    if (failed()) return;
    if (remote_) {
        remote_->focus(gained, e, t, v);
        return;
    }
    Impl& d = *impl_;
    CrashGuard guard(d.desc.binary);
    d.prepare(e, t, v);
    PropertySet a = d.args(v, t);
    const OfxStatus st = d.plugin->overlay(gained ? kOfxInteractActionGainFocus : kOfxInteractActionLoseFocus, &d.obj, handle(&a), nullptr);
    d.done();
    if (st == kOfxStatFailed || st == kOfxStatErrFatal) failed_ = true;
}

int instancesCreated() { return gInstances.load(); }
int instancesAlive() { return gLiveInstances.load(); }

namespace {
thread_local FrameFetch tFetch;
}

FetchScope::FetchScope(FrameFetch fetch) : previous_(std::move(tFetch)) { tFetch = std::move(fetch); }
FetchScope::~FetchScope() { tFetch = std::move(previous_); }
const FrameFetch& currentFetch() { return tFetch; }

}  // namespace montage::ofx
