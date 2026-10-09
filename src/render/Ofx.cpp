#include "Ofx.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <thread>
#include <type_traits>
#include <utility>

#include "audio/DynLib.h"
#include "core/Effects.h"
#include "media/SuperScale.h"
#include "ofxCore.h"
#include "ofxImageEffect.h"
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
    std::unique_ptr<EffectObj> descriptor, filter;  // as described, and described in the filter context
    std::string context = kOfxImageEffectContextFilter;
    bool unsafe = false;  // renders one at a time
    std::mutex renderMutex;
};

// What an instance is rendering now: the frames and the effect's values.
struct Instance {
    Loaded* plugin = nullptr;
    EffectObj obj;
    const Effect* effect = nullptr;
    const Image* source = nullptr;
    double time = 0, scale = 1;
    const FrameFetch* fetch = nullptr;
    Image* output = nullptr;
    std::string error;
    bool floatImages = true;
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
OfxStatus setValues(ParamObj* p, va_list ap) {
    const int n = dimensionsOf(p->type);
    if (stringType(p->type)) {
        const char* s = va_arg(ap, const char*);
        p->text = s ? s : "";
        return kOfxStatOK;
    }
    if (n == 0) return kOfxStatOK;  // a push button "set" is nothing
    p->value.resize(size_t(n));
    for (int k = 0; k < n; ++k) p->value[size_t(k)] = integerType(p->type) ? double(va_arg(ap, int)) : va_arg(ap, double);
    return kOfxStatOK;
}
OfxStatus paramSetValue(OfxParamHandle h, ...) {
    if (!h) return kOfxStatErrBadHandle;
    va_list ap;
    va_start(ap, h);
    const OfxStatus st = setValues(param(h), ap);
    va_end(ap);
    return st;
}
OfxStatus paramSetValueAtTime(OfxParamHandle h, OfxTime time, ...) {
    if (!h) return kOfxStatErrBadHandle;
    va_list ap;
    va_start(ap, time);
    const OfxStatus st = setValues(param(h), ap);
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

// An image for the plugin, bottom row first as OFX counts, from a premultiplied float image.
ImageObj* makeImage(const Instance& inst, const Image& from, bool isOutput) {
    auto* img = new ImageObj;
    const int w = from.width, h = from.height;
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
    return img;
}

OfxStatus clipGetImage(OfxImageClipHandle h, OfxTime time, const OfxRectD*, OfxPropertySetHandle* imageHandle) {
    if (!h || !imageHandle) return kOfxStatErrBadHandle;
    ClipObj* c = clipObj(h);
    Instance* inst = c->effect ? c->effect->instance : nullptr;
    if (!inst || !inst->source) return kOfxStatFailed;
    ImageObj* img = nullptr;
    if (c->name == kOfxImageEffectOutputClipName) {
        img = makeImage(*inst, *inst->source, true);
        if (!inst->output) return kOfxStatFailed;
        // Where the plugin writes: kept so render() can read it back.
        inst->output->width = inst->source->width;
        inst->output->height = inst->source->height;
        img->props.setPointer("montage.output", inst->output);
        img->owner = inst;
    } else {
        Image other;
        // Another frame, sized like this one (else this one stands in).
        if (std::fabs(time - inst->time) > 1e-6 && inst->fetch && *inst->fetch && (*inst->fetch)(time, other) && !other.empty()) {
            if (other.width != inst->source->width || other.height != inst->source->height)
                other = resizeImage(other, inst->source->width, inst->source->height);
            img = makeImage(*inst, other, false);
        } else {
            img = makeImage(*inst, *inst->source, false);
        }
    }
    *imageHandle = handle(&img->props);
    return kOfxStatOK;
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
    const double w = inst && inst->source ? inst->source->width / inst->scale : 0, hh = inst && inst->source ? inst->source->height / inst->scale : 0;
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
    auto* m = new MemoryBlock;
    m->bytes.resize(n);
    *out = reinterpret_cast<OfxImageMemoryHandle>(m);
    return kOfxStatOK;
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
    for (unsigned i = 1; i < n; ++i)
        threads.emplace_back([=] {
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
        h->setInt(kOfxImageEffectPropSupportsOverlays, {0});
        h->setInt(kOfxImageEffectPropSupportsMultiResolution, {1});
        h->setInt(kOfxImageEffectPropSupportsTiles, {0});
        h->setInt(kOfxImageEffectPropTemporalClipAccess, {1});
        h->setString(kOfxImageEffectPropSupportedComponents, {kOfxImageComponentRGBA});
        h->setString(kOfxImageEffectPropSupportedContexts, {kOfxImageEffectContextFilter, kOfxImageEffectContextGeneral});
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
    const std::vector<std::string> contexts = dp.strings(kOfxImageEffectPropSupportedContexts);
    if (std::find(contexts.begin(), contexts.end(), kOfxImageEffectContextFilter) != contexts.end()) l->context = kOfxImageEffectContextFilter;
    else if (std::find(contexts.begin(), contexts.end(), kOfxImageEffectContextGeneral) != contexts.end()) l->context = kOfxImageEffectContextGeneral;
    else {
        if (error) *error = std::string(plugin->pluginIdentifier) + " is not a filter (generators and transitions are not supported yet)";
        return nullptr;
    }
    l->filter = std::make_unique<EffectObj>();
    l->filter->props = dp;
    l->filter->props.setString(kOfxImageEffectPropContext, {l->context});
    PropertySet in;
    in.setString(kOfxImageEffectPropContext, {l->context});
    if (!ok(plugin->mainEntry(kOfxImageEffectActionDescribeInContext, reinterpret_cast<OfxImageEffectHandle>(l->filter.get()), handle(&in), nullptr))) {
        if (error) *error = std::string(plugin->pluginIdentifier) + " failed to describe itself as a filter";
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
    l->unsafe = fp.getString(kOfxImageEffectPluginRenderThreadSafety) == kOfxImageEffectRenderUnsafe;
    describeParams(*l->filter, d);
    return l;
}

QJsonObject toJson(const PluginDesc& d) {
    QJsonArray params;
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
                       {"temporal", d.temporal}, {"float", d.floatImages}, {"params", params}};
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
Loaded* loadPlugin(const PluginDesc& d, std::string* error) {
    std::lock_guard<std::mutex> lock(gLoadMutex);
    auto& all = loadedPlugins();
    if (auto it = all.find(d.id); it != all.end()) return it->second.get();
    if (auto f = failedLoads().find(d.id); f != failedLoads().end()) {
        if (error) *error = f->second;
        return nullptr;
    }
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
std::unique_ptr<Instance> createInstance(Loaded* l, std::string* error) {
    auto inst = std::make_unique<Instance>();
    inst->plugin = l;
    inst->floatImages = l->desc.floatImages;
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
    o.props.setInt(kOfxPropIsInteractive, {0});
    o.props.setInt(kOfxImageEffectInstancePropSequentialRender, {0});
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
        cp.setInt(kOfxImageClipPropConnected, {1});
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
    inst->plugin->plugin->mainEntry(kOfxActionDestroyInstance, reinterpret_cast<OfxImageEffectHandle>(&inst->obj), nullptr, nullptr);
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
    if (out.empty() && error && error->empty()) *error = "No OpenFX filters in " + binary;
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
    QJsonObject fresh;
    std::vector<PluginDesc> found;
    std::vector<std::pair<std::string, std::string>> blocked;
    for (const std::string& bin : findBinaries(dirs)) {
        const QFileInfo fi(QString::fromStdString(bin));
        const QString stamp = QString::number(fi.size()) + ":" + QString::number(fi.lastModified().toMSecsSinceEpoch());
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

Effect makeEffect(Project& p, const PluginDesc& d) {
    Effect e = montage::makeEffect(p, "ofx");
    e.strings["ofx_id"] = d.id;
    e.strings["ofx_name"] = d.label;
    e.strings["ofx_group"] = d.group;
    e.strings["ofx_binary"] = d.binary;
    e.strings["ofx_identifier"] = d.identifier;
    e.strings["ofx_version"] = std::to_string(d.versionMajor);
    e.strings["ofx_description"] = descriptionsToJson({d});
    for (const ParamDesc& pd : d.params) {
        if (pd.secret) continue;
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

bool applyEffect(const Effect& e, double t, Image& img, double scale, const FrameFetch& fetch, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (img.empty()) return true;
    // Only a plug-in this computer's scan found and did not block runs: never a binary named by the project file, which
    // could be anything, or one that crashed the probe.
    Registry& registry = Registry::instance();
    registry.ensureScanned();
    PluginDesc d;
    if (!registry.find(e.s("ofx_id"), d)) {
        const std::vector<PluginDesc> saved = descriptionsFromJson(e.s("ofx_description"));
        const std::string name = effectName(e);
        if (!saved.empty())
            for (const auto& [binary, why] : registry.blocked())
                if (binary == saved.front().binary) return fail(name + " is blocked: " + why);
        return fail(name + " is not installed");
    }
    Loaded* l = loadPlugin(d, error ? error : nullptr);
    if (!l) return false;
    Instance* inst = checkOut(l, e.id, error);
    if (!inst) return false;
    struct Return {
        Instance* inst;
        ~Return() { checkIn(inst); }
    } giveBack{inst};
    std::unique_lock<std::mutex> serial(l->renderMutex, std::defer_lock);
    if (l->unsafe) serial.lock();
    Image out(img.width, img.height);
    inst->effect = &e;
    inst->source = &img;
    inst->time = t;
    inst->scale = std::max(1e-3, scale);
    inst->fetch = &fetch;
    inst->output = &out;
    inst->error.clear();
    inst->wrote = false;
    const double fullW = img.width / inst->scale, fullH = img.height / inst->scale;
    inst->projectW = fullW, inst->projectH = fullH;
    inst->obj.props.setDouble(kOfxImageEffectPropProjectSize, {fullW, fullH});
    inst->obj.props.setDouble(kOfxImageEffectPropProjectExtent, {fullW, fullH});
    announceChanges(*inst, e, t);
    auto entry = l->plugin->mainEntry;
    const OfxImageEffectHandle h = reinterpret_cast<OfxImageEffectHandle>(&inst->obj);
    PropertySet seq;
    seq.setDouble(kOfxImageEffectPropFrameRange, {t, t});
    seq.setDouble(kOfxImageEffectPropFrameStep, {1.0});
    seq.setInt(kOfxPropIsInteractive, {0});
    seq.setDouble(kOfxImageEffectPropRenderScale, {inst->scale, inst->scale});
    seq.setInt(kOfxImageEffectPropSequentialRenderStatus, {0});
    seq.setInt(kOfxImageEffectPropInteractiveRenderStatus, {0});
    entry(kOfxImageEffectActionBeginSequenceRender, h, handle(&seq), nullptr);
    PropertySet args;
    args.setDouble(kOfxPropTime, {t});
    args.setString(kOfxImageEffectPropFieldToRender, {kOfxImageFieldNone});
    args.setInt(kOfxImageEffectPropRenderWindow, {0, 0, img.width, img.height});
    args.setDouble(kOfxImageEffectPropRenderScale, {inst->scale, inst->scale});
    args.setInt(kOfxImageEffectPropSequentialRenderStatus, {0});
    args.setInt(kOfxImageEffectPropInteractiveRenderStatus, {0});
    args.setInt(kOfxImageEffectPropRenderQualityDraft, {0});
    const OfxStatus st = entry(kOfxImageEffectActionRender, h, handle(&args), nullptr);
    entry(kOfxImageEffectActionEndSequenceRender, h, handle(&seq), nullptr);
    inst->effect = nullptr;
    inst->source = nullptr;
    inst->fetch = nullptr;
    inst->output = nullptr;
    if (!ok(st)) return fail(inst->error.empty() ? d.label + " failed to render" : d.label + ": " + inst->error);
    if (!inst->wrote || out.px.size() != img.px.size()) return fail(d.label + " gave no picture back");
    img = std::move(out);
    return true;
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
