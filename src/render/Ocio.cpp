#include "Ocio.h"

#ifdef MONTAGE_WITH_OCIO
#include <OpenColorIO/OpenColorIO.h>

#include <QDateTime>
#include <QFileInfo>
#include <cstdlib>
#include <functional>
#include <map>
#include <mutex>
namespace OCIO = OCIO_NAMESPACE;
#endif

namespace montage {

#ifdef MONTAGE_WITH_OCIO

namespace {

const char* kDisplayMode = "Display / View";

std::mutex& ocioMutex() {
    static std::mutex m;
    return m;
}

// The config an effect names: a file, an "ocio://" built-in, or "" for $OCIO
// (else OCIO's default built-in config). Cached; a file is reloaded when it changes.
OCIO::ConstConfigRcPtr loadConfig(const std::string& name, std::string* error) {
    static std::map<std::string, std::pair<qint64, OCIO::ConstConfigRcPtr>> cache;
    std::string path = name;
    if (path.empty()) {
        const char* env = std::getenv("OCIO");
        if (env && *env) path = env;
    }
#if OCIO_VERSION_HEX >= 0x02020000
    if (path.empty()) path = "ocio://default";
#endif
    if (path.empty()) {
        if (error) *error = "Choose an OpenColorIO config (.ocio file)";
        return nullptr;
    }
    const bool builtin = path.rfind("ocio://", 0) == 0;
    const qint64 stamp = builtin ? 0 : QFileInfo(QString::fromStdString(path)).lastModified().toMSecsSinceEpoch();
    std::lock_guard lock(ocioMutex());
    auto it = cache.find(path);
    if (it != cache.end() && it->second.first == stamp) return it->second.second;
    try {
        OCIO::ConstConfigRcPtr cfg = OCIO::Config::CreateFromFile(path.c_str());
        cache[path] = {stamp, cfg};
        return cfg;
    } catch (const OCIO::Exception& ex) {
        if (error) *error = ex.what();
        return nullptr;
    }
}

OCIO::ConstCPUProcessorRcPtr processorFor(const Effect& e, std::string* error) {
    const std::string configName = e.s("config");
    OCIO::ConstConfigRcPtr cfg = loadConfig(configName, error);
    if (!cfg) return nullptr;
    const bool display = e.s("mode") == kDisplayMode;
    const bool inverse = e.p("inverse", 0) > 0.5;
    const std::string src = e.s("src"), dst = e.s("dst"), disp = e.s("display"), view = e.s("view"), look = e.s("look");
    const std::string key = configName + '\x1f' + std::to_string(reinterpret_cast<uintptr_t>(cfg.get())) + '\x1f' +
                            (display ? "d" : "c") + (inverse ? "i" : "f") + '\x1f' + src + '\x1f' + dst + '\x1f' +
                            disp + '\x1f' + view + '\x1f' + look;
    static std::map<std::string, OCIO::ConstCPUProcessorRcPtr> cache;
    {
        std::lock_guard lock(ocioMutex());
        if (auto it = cache.find(key); it != cache.end()) return it->second;
    }
    try {
        const char* source = src.empty() ? OCIO::ROLE_SCENE_LINEAR : src.c_str();
        OCIO::GroupTransformRcPtr group = OCIO::GroupTransform::Create();
        if (!look.empty()) {
            OCIO::LookTransformRcPtr lt = OCIO::LookTransform::Create();
            lt->setSrc(source);
            lt->setDst(source);
            lt->setLooks(look.c_str());
            group->appendTransform(lt);
        }
        if (display) {
            OCIO::DisplayViewTransformRcPtr dvt = OCIO::DisplayViewTransform::Create();
            const char* d = disp.empty() ? cfg->getDefaultDisplay() : disp.c_str();
            dvt->setSrc(source);
            dvt->setDisplay(d);
            dvt->setView(view.empty() ? cfg->getDefaultView(d) : view.c_str());
            group->appendTransform(dvt);
        } else {
            OCIO::ColorSpaceTransformRcPtr cst = OCIO::ColorSpaceTransform::Create();
            cst->setSrc(source);
            cst->setDst(dst.empty() ? source : dst.c_str());
            group->appendTransform(cst);
        }
        OCIO::ConstProcessorRcPtr proc =
            cfg->getProcessor(group, inverse ? OCIO::TRANSFORM_DIR_INVERSE : OCIO::TRANSFORM_DIR_FORWARD);
        OCIO::ConstCPUProcessorRcPtr cpu = proc->getDefaultCPUProcessor();
        std::lock_guard lock(ocioMutex());
        if (cache.size() > 64) cache.clear();
        cache[key] = cpu;
        return cpu;
    } catch (const OCIO::Exception& ex) {
        if (error) *error = ex.what();
        return nullptr;
    }
}

std::vector<std::string> names(int n, const std::function<const char*(int)>& at) {
    std::vector<std::string> out;
    for (int i = 0; i < n; ++i)
        if (const char* s = at(i)) out.emplace_back(s);
    return out;
}

}  // namespace

bool ocioAvailable() { return true; }
std::string ocioVersion() { return OCIO::GetVersion(); }

std::vector<std::string> ocioBuiltinConfigs() {
    std::vector<std::string> out;
#if OCIO_VERSION_HEX >= 0x02020000
    const OCIO::BuiltinConfigRegistry& reg = OCIO::BuiltinConfigRegistry::Get();
    for (size_t i = reg.getNumBuiltinConfigs(); i-- > 0;)
        if (reg.isBuiltinConfigRecommended(i)) out.push_back(std::string("ocio://") + reg.getBuiltinConfigName(i));
#endif
    return out;
}

std::vector<std::string> ocioChoices(const Effect& e, const std::string& param) {
    OCIO::ConstConfigRcPtr cfg = loadConfig(e.s("config"), nullptr);
    if (!cfg) return {};
    try {
        if (param == "src" || param == "dst")
            return names(cfg->getNumColorSpaces(OCIO::SEARCH_REFERENCE_SPACE_ALL, OCIO::COLORSPACE_ACTIVE),
                         [&](int i) { return cfg->getColorSpaceNameByIndex(OCIO::SEARCH_REFERENCE_SPACE_ALL, OCIO::COLORSPACE_ACTIVE, i); });
        if (param == "display") return names(cfg->getNumDisplays(), [&](int i) { return cfg->getDisplay(i); });
        if (param == "view") {
            const std::string d = e.s("display").empty() ? cfg->getDefaultDisplay() : e.s("display");
            return names(cfg->getNumViews(d.c_str()), [&](int i) { return cfg->getView(d.c_str(), i); });
        }
        if (param == "look") {
            auto out = names(cfg->getNumLooks(), [&](int i) { return cfg->getLookNameByIndex(i); });
            out.insert(out.begin(), std::string());
            return out;
        }
    } catch (const OCIO::Exception&) {
    }
    return {};
}

bool applyOcio(const Effect& e, Image& img, std::string* error) {
    if (img.empty()) return true;
    OCIO::ConstCPUProcessorRcPtr cpu = processorFor(e, error);
    if (!cpu) return false;
    parallelRows(img.height, [&](int y0, int y1) {
        std::vector<float> row(size_t(img.width) * 4);
        for (int y = y0; y < y1; ++y) {
            float* p = img.row(y);
            // OCIO works on straight (unpremultiplied) colour.
            for (int x = 0; x < img.width; ++x) {
                const float a = p[x * 4 + 3];
                const float k = a > 0 ? 1 / a : 0;
                for (int c = 0; c < 3; ++c) row[size_t(x) * 4 + c] = p[x * 4 + c] * k;
                row[size_t(x) * 4 + 3] = a;
            }
            OCIO::PackedImageDesc desc(row.data(), img.width, 1, 4);
            cpu->apply(desc);
            for (int x = 0; x < img.width; ++x) {
                const float a = p[x * 4 + 3];
                for (int c = 0; c < 3; ++c) p[x * 4 + c] = row[size_t(x) * 4 + c] * a;
            }
        }
    });
    return true;
}

bool applyOcioBuiltin(const std::string& style, float* rgb, size_t count, bool inverse, std::string* error) {
    try {
        OCIO::BuiltinTransformRcPtr t = OCIO::BuiltinTransform::Create();
        t->setStyle(style.c_str());
        OCIO::ConstConfigRcPtr raw = OCIO::Config::CreateRaw();
        OCIO::ConstCPUProcessorRcPtr cpu =
            raw->getProcessor(t, inverse ? OCIO::TRANSFORM_DIR_INVERSE : OCIO::TRANSFORM_DIR_FORWARD)->getDefaultCPUProcessor();
        OCIO::PackedImageDesc desc(rgb, long(count), 1, 3);
        cpu->apply(desc);
        return true;
    } catch (const OCIO::Exception& ex) {
        if (error) *error = ex.what();
        return false;
    }
}

#else  // no OpenColorIO

bool ocioAvailable() { return false; }
std::string ocioVersion() { return {}; }
std::vector<std::string> ocioBuiltinConfigs() { return {}; }
std::vector<std::string> ocioChoices(const Effect&, const std::string&) { return {}; }
bool applyOcio(const Effect&, Image&, std::string* error) {
    if (error) *error = "Montage was built without OpenColorIO";
    return false;
}
bool applyOcioBuiltin(const std::string&, float*, size_t, bool, std::string* error) {
    if (error) *error = "Montage was built without OpenColorIO";
    return false;
}

#endif

}  // namespace montage
