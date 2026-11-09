#include "Ocio.h"

#include "GpuCompositor.h"

#ifdef MONTAGE_WITH_OCIO
#include <OpenColorIO/OpenColorIO.h>

#include <QDateTime>
#include <QFileInfo>
#include <cstdlib>
#include <functional>
#include <map>
#include <mutex>
#include <regex>
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

OCIO::ConstProcessorRcPtr processorOf(const Effect& e, std::string* error, bool dynamic = false, FrameTime t = 0);

OCIO::ConstCPUProcessorRcPtr processorFor(const Effect& e, FrameTime t, std::string* error) {
    OCIO::ConstProcessorRcPtr proc = processorOf(e, error, false, t);
    if (!proc) return nullptr;
    static std::map<const OCIO::Processor*, std::pair<OCIO::ConstProcessorRcPtr, OCIO::ConstCPUProcessorRcPtr>> cache;
    {
        std::lock_guard lock(ocioMutex());
        if (auto it = cache.find(proc.get()); it != cache.end()) return it->second.second;
    }
    try {
        OCIO::ConstCPUProcessorRcPtr cpu = proc->getDefaultCPUProcessor();
        std::lock_guard lock(ocioMutex());
        if (cache.size() > 64) cache.clear();
        cache[proc.get()] = {proc, cpu};  // (the processor held, so its address is not taken again)
        return cpu;
    } catch (const OCIO::Exception& ex) {
        if (error) *error = ex.what();
        return nullptr;
    }
}

// The effect's transform, one processor a transform (kept for the last 64). With its exposure, contrast and gamma
// (once any is set: liveControls), an ExposureContrastTransform on the input first: at the values at `t`, or (`dynamic`,
// for OCIO's GPU shader) as dynamic properties, set while the shader runs without building it again.
OCIO::ConstProcessorRcPtr processorOf(const Effect& e, std::string* error, bool dynamic, FrameTime t) {
    const std::string configName = e.s("config");
    OCIO::ConstConfigRcPtr cfg = loadConfig(configName, error);
    if (!cfg) return nullptr;
    const bool display = e.s("mode") == kDisplayMode;
    const bool inverse = e.p("inverse", 0) > 0.5;
    const std::string src = e.s("src"), dst = e.s("dst"), disp = e.s("display"), view = e.s("view"), look = e.s("look");
    const bool live = ocioLiveControls(e);
    const double exposure = e.p("exposure", t, 0), contrast = e.p("contrast", t, 1), gamma = e.p("gamma", t, 1);
    const std::string key = configName + '\x1f' + std::to_string(reinterpret_cast<uintptr_t>(cfg.get())) + '\x1f' +
                            (display ? "d" : "c") + (inverse ? "i" : "f") + '\x1f' + src + '\x1f' + dst + '\x1f' +
                            disp + '\x1f' + view + '\x1f' + look + '\x1f' +
                            (!live ? std::string("-") : dynamic ? std::string("dynamic")
                                                                : std::to_string(exposure) + ' ' + std::to_string(contrast) + ' ' + std::to_string(gamma));
    static std::map<std::string, OCIO::ConstProcessorRcPtr> cache;
    {
        std::lock_guard lock(ocioMutex());
        if (auto it = cache.find(key); it != cache.end()) return it->second;
    }
    try {
        const char* source = src.empty() ? OCIO::ROLE_SCENE_LINEAR : src.c_str();
        OCIO::GroupTransformRcPtr group = OCIO::GroupTransform::Create();
        if (live) {
            OCIO::ExposureContrastTransformRcPtr ec = OCIO::ExposureContrastTransform::Create();
            ec->setStyle(OCIO::EXPOSURE_CONTRAST_LINEAR);
            ec->setPivot(0.18);
            ec->setExposure(exposure), ec->setContrast(contrast), ec->setGamma(gamma);
            if (dynamic) ec->makeExposureDynamic(), ec->makeContrastDynamic(), ec->makeGammaDynamic();
            group->appendTransform(ec);
        }
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
        std::lock_guard lock(ocioMutex());
        if (cache.size() > 64) cache.clear();
        cache[key] = proc;
        return proc;
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

bool ocioLiveControls(const Effect& e) {
    for (const auto& [name, def] : {std::pair<const char*, double>{"exposure", 0}, {"contrast", 1}, {"gamma", 1}})
        if (auto it = e.params.find(name); it != e.params.end() && (it->second.varies() || it->second.value != def)) return true;
    return false;
}

bool applyOcio(const Effect& e, Image& img, std::string* error, FrameTime t) {
    if (img.empty()) return true;
    OCIO::ConstCPUProcessorRcPtr cpu = processorFor(e, t, error);
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

// GLSL reading the table `name` (a 1D table made a 2D texture one texel high, 2D or 3D) at normalised coordinates as
// texture() would with clamping at the edges: linearly between texels, or the nearest, in 32-bit floats.
static std::string samplingFunction(const std::string& name, int dims, bool linear) {
    const std::string N = name;
    std::string f;
    if (dims == 1) {
        f = "vec4 " + N + "_at(float x) {\n    const int n = textureSize(" + N + ", 0).x;\n";
        if (!linear) return f + "    return texelFetch(" + N + ", ivec2(clamp(int(floor(x * float(n))), 0, n - 1), 0), 0);\n}\n";
        return f + "    const float p = clamp(x * float(n) - 0.5, 0.0, float(n - 1));\n"
                   "    const int i = int(floor(p)), j = min(i + 1, n - 1);\n"
                   "    return mix(texelFetch(" + N + ", ivec2(i, 0), 0), texelFetch(" + N + ", ivec2(j, 0), 0), p - float(i));\n}\n";
    }
    if (dims == 2) {
        f = "vec4 " + N + "_at(vec2 c) {\n    const ivec2 n = textureSize(" + N + ", 0);\n";
        if (!linear) return f + "    return texelFetch(" + N + ", clamp(ivec2(floor(c * vec2(n))), ivec2(0), n - 1), 0);\n}\n";
        return f + "    const vec2 p = clamp(c * vec2(n) - 0.5, vec2(0.0), vec2(n - 1));\n"
                   "    const ivec2 i = ivec2(floor(p)), j = min(i + 1, n - 1);\n"
                   "    const vec2 t = p - vec2(i);\n"
                   "    const vec4 a = mix(texelFetch(" + N + ", i, 0), texelFetch(" + N + ", ivec2(j.x, i.y), 0), t.x);\n"
                   "    const vec4 b = mix(texelFetch(" + N + ", ivec2(i.x, j.y), 0), texelFetch(" + N + ", j, 0), t.x);\n"
                   "    return mix(a, b, t.y);\n}\n";
    }
    f = "vec4 " + N + "_at(vec3 c) {\n    const ivec3 n = textureSize(" + N + ", 0);\n";
    if (!linear) return f + "    return texelFetch(" + N + ", clamp(ivec3(floor(c * vec3(n))), ivec3(0), n - 1), 0);\n}\n";
    auto at = [&](const char* x, const char* y, const char* z) {
        return "texelFetch(" + N + ", ivec3(" + x + ", " + y + ", " + z + "), 0)";
    };
    return f + "    const vec3 p = clamp(c * vec3(n) - 0.5, vec3(0.0), vec3(n - 1));\n"
               "    const ivec3 i = ivec3(floor(p)), j = min(i + 1, n - 1);\n"
               "    const vec3 t = p - vec3(i);\n"
               "    const vec4 a = mix(mix(" + at("i.x", "i.y", "i.z") + ", " + at("j.x", "i.y", "i.z") + ", t.x), mix(" + at("i.x", "j.y", "i.z") + ", " +
               at("j.x", "j.y", "i.z") + ", t.x), t.y);\n"
               "    const vec4 b = mix(mix(" + at("i.x", "i.y", "j.z") + ", " + at("j.x", "i.y", "j.z") + ", t.x), mix(" + at("i.x", "j.y", "j.z") + ", " +
               at("j.x", "j.y", "j.z") + ", t.x), t.y);\n"
               "    return mix(a, b, t.z);\n}\n";
}

std::shared_ptr<const gpu::ShaderStep> ocioShaderStep(const Effect& e, std::string* error) {
    OCIO::ConstProcessorRcPtr proc = processorOf(e, error, true);
    if (!proc) return nullptr;
    // (Kept by processor, which each entry holds so its address is not reused; one OCIO cannot draw is kept too, with
    // why, so it is not tried again each frame.)
    struct Entry {
        OCIO::ConstProcessorRcPtr proc;
        std::shared_ptr<const gpu::ShaderStep> step;
        std::string why;
    };
    static std::map<const OCIO::Processor*, Entry> cache;
    {
        std::lock_guard lock(ocioMutex());
        if (auto it = cache.find(proc.get()); it != cache.end()) {
            if (!it->second.step && error) *error = it->second.why;
            return it->second.step;
        }
    }
    auto keep = [&](std::shared_ptr<const gpu::ShaderStep> made, const std::string& why) {
        if (!made && error) *error = why;
        std::lock_guard lock(ocioMutex());
        if (cache.size() > 16) cache.clear();
        cache[proc.get()] = {proc, made, why};
        return made;
    };
    auto step = std::make_shared<gpu::ShaderStep>();
    try {
        OCIO::GpuShaderDescRcPtr desc = OCIO::GpuShaderDesc::CreateShaderDesc();
        desc->setLanguage(OCIO::GPU_LANGUAGE_GLSL_4_0);
        desc->setFunctionName("ocioTransform");
        desc->setResourcePrefix("ocio");
        OCIO::ConstGPUProcessorRcPtr gpuProc = proc->getDefaultGPUProcessor();
        gpuProc->extractGpuShaderInfo(desc);
        // Its dynamic properties (the live exposure, contrast and gamma): each float uniform read from the values the
        // step is drawn with (ShaderStep::uniforms), in the Params block; any other kind is the CPU's.
        for (unsigned i = 0; i < desc->getNumUniforms(); ++i) {
            OCIO::GpuShaderDesc::UniformData data;
            const char* name = desc->getUniform(i, data);
            if (!name || data.m_type != OCIO::UNIFORM_DOUBLE || step->uniforms.size() >= 64)
                return keep(nullptr, "the transform has values set while it runs that the GPU does not take");
            step->uniforms.push_back(name);
        }
        step->key = std::string("ocio:") + gpuProc->getCacheID();
        step->function = "ocioTransform";
        // Each sampler's binding, from 2 in the order its texture comes (1D textures, then 3D).
        std::map<std::string, int> binding;
        std::map<std::string, bool> interpolate;
        for (unsigned i = 0; i < desc->getNumTextures(); ++i) {
            const char *texName = nullptr, *sampler = nullptr;
            unsigned w = 0, h = 0;
            OCIO::GpuShaderDesc::TextureType channel = OCIO::GpuShaderDesc::TEXTURE_RGB_CHANNEL;
            OCIO::Interpolation interp = OCIO::INTERP_LINEAR;
#if OCIO_VERSION_HEX >= 0x02030000
            OCIO::GpuShaderDesc::TextureDimensions dims = OCIO::GpuShaderDesc::TEXTURE_2D;
            desc->getTexture(i, texName, sampler, w, h, channel, dims, interp);
#else
            desc->getTexture(i, texName, sampler, w, h, channel, interp);
#endif
            const float* values = nullptr;
            desc->getTextureValues(i, values);
            gpu::ShaderStep::Texture t;
            t.width = int(w), t.height = int(std::max(1u, h)), t.channels = channel == OCIO::GpuShaderDesc::TEXTURE_RED_CHANNEL ? 1 : 3;
            interpolate[sampler] = interp != OCIO::INTERP_NEAREST;
            t.linear = false;  // (read texel by texel and interpolated in 32-bit floats by the shader; see below)
            t.values.assign(values, values + size_t(t.width) * size_t(t.height) * size_t(t.channels));
            binding[sampler] = 2 + int(step->textures.size());
            step->textures.push_back(std::move(t));
        }
        for (unsigned i = 0; i < desc->getNum3DTextures(); ++i) {
            const char *texName = nullptr, *sampler = nullptr;
            unsigned edge = 0;
            OCIO::Interpolation interp = OCIO::INTERP_LINEAR;
            desc->get3DTexture(i, texName, sampler, edge, interp);
            const float* values = nullptr;
            desc->get3DTextureValues(i, values);
            gpu::ShaderStep::Texture t;
            t.width = t.height = t.depth = int(edge);
            interpolate[sampler] = interp != OCIO::INTERP_NEAREST;
            t.linear = false;
            t.values.assign(values, values + size_t(edge) * edge * edge * 3);
            binding[sampler] = 2 + int(step->textures.size());
            step->textures.push_back(std::move(t));
        }
        // The samplers given their bindings, 1D ones as 2D textures one texel high. Each is read through a function of
        // its own, which `texture(name, ...)` becomes: it fetches the texels and interpolates them in 32-bit floats, as
        // OCIO's CPU path does, so the tables keep full precision (no half floats) and need no filtering of float
        // textures (which some devices lack: optional on Vulkan, an extension on GLES, some Apple GPUs only).
        std::string text = desc->getShaderText();
        for (size_t i = 0; i < step->uniforms.size(); ++i) {
            const std::regex uniform("uniform\\s+float\\s+" + step->uniforms[i] + "\\s*;");
            if (!std::regex_search(text, uniform)) return keep(nullptr, "the shader does not declare its value " + step->uniforms[i]);
            text = std::regex_replace(text, uniform,
                                      "#define " + step->uniforms[i] + " montageValues[" + std::to_string(i / 4) + "]." + "xyzw"[i % 4]);
        }
        static const std::regex declaration(R"(uniform\s+sampler([123])D\s+(\w+)\s*;)");
        std::vector<std::string> names;
        for (auto d = std::sregex_iterator(text.begin(), text.end(), declaration); d != std::sregex_iterator(); ++d)
            names.push_back((*d)[2]);
        for (const std::string& name : names)
            text = std::regex_replace(text, std::regex("texture\\s*\\(\\s*" + name + "\\s*,"), name + "_at(");
        std::string out;
        auto it = std::sregex_iterator(text.begin(), text.end(), declaration);
        size_t last = 0;
        for (; it != std::sregex_iterator(); ++it) {
            const std::smatch& m = *it;
            const std::string name = m[2];
            const auto b = binding.find(name);
            if (b == binding.end()) return keep(nullptr, "the shader names a texture it does not list: " + name);
            out += text.substr(last, size_t(m.position(0)) - last);
            const std::string dim = m[1] == "3" ? "3D" : "2D";
            out += "layout(binding = " + std::to_string(b->second) + ") uniform sampler" + dim + " " + name + ";\n";
            out += samplingFunction(name, m[1] == "3" ? 3 : m[1] == "2" ? 2 : 1, interpolate[name]);
            last = size_t(m.position(0) + m.length(0));
        }
        out += text.substr(last);
        if (binding.size() != step->textures.size()) return keep(nullptr, "the shader's textures could not be told apart");
        step->source = std::move(out);
    } catch (const OCIO::Exception& ex) {
        return keep(nullptr, ex.what());
    }
    return keep(step, {});
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

bool aces2OutputAvailable() {
#if OCIO_VERSION_HEX >= 0x02050000
    static const bool available = [] {  // (its built-in ACES 2.0 config there to load)
        try {
            return bool(OCIO::Config::CreateFromBuiltinConfig("studio-config-v4.0.0_aces-v2.0_ocio-v2.5"));
        } catch (const OCIO::Exception&) {
            return false;
        }
    }();
    return available;
#else
    return false;
#endif
}

bool applyAces2Output(const std::string& display, const std::string& view, float* rgb, size_t count, bool inverse, std::string* error) {
#if OCIO_VERSION_HEX >= 0x02050000
    static std::map<std::string, OCIO::ConstCPUProcessorRcPtr> cache;
    static std::map<std::string, std::string> failed;  // (made once: the reason it could not be)
    OCIO::ConstCPUProcessorRcPtr cpu;
    const std::string key = display + '|' + view + (inverse ? "|inverse" : "");
    try {
        {
            std::lock_guard lock(ocioMutex());
            if (auto it = cache.find(key); it != cache.end()) cpu = it->second;
            else if (auto f = failed.find(key); f != failed.end()) {
                if (error) *error = f->second;
                return false;
            }
        }
        if (!cpu) {
            OCIO::ConstConfigRcPtr cfg = OCIO::Config::CreateFromBuiltinConfig("studio-config-v4.0.0_aces-v2.0_ocio-v2.5");
            OCIO::DisplayViewTransformRcPtr t = OCIO::DisplayViewTransform::Create();
            t->setSrc("ACES2065-1");
            t->setDisplay(display.c_str());
            t->setView(view.c_str());
            cpu = cfg->getProcessor(t, inverse ? OCIO::TRANSFORM_DIR_INVERSE : OCIO::TRANSFORM_DIR_FORWARD)->getDefaultCPUProcessor();
            std::lock_guard lock(ocioMutex());
            cache[key] = cpu;
        }
        OCIO::PackedImageDesc desc(rgb, long(count), 1, 3);
        cpu->apply(desc);
        return true;
    } catch (const OCIO::Exception& ex) {
        if (error) *error = ex.what();
        if (!cpu) {
            std::lock_guard lock(ocioMutex());
            failed[key] = ex.what();
        }
        return false;
    }
#else
    (void)display, (void)view, (void)rgb, (void)count, (void)inverse;
    if (error) *error = "ACES 2.0 needs OpenColorIO 2.5 or later (this is " + ocioVersion() + ")";
    return false;
#endif
}

bool applyOcioFile(const std::string& path, float* rgb, size_t count, std::string* error) {
    try {
        OCIO::FileTransformRcPtr t = OCIO::FileTransform::Create();
        t->setSrc(path.c_str());
        t->setInterpolation(OCIO::INTERP_BEST);
        OCIO::ConstConfigRcPtr raw = OCIO::Config::CreateRaw();
        OCIO::ConstCPUProcessorRcPtr cpu = raw->getProcessor(t)->getDefaultCPUProcessor();
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
bool ocioLiveControls(const Effect&) { return false; }
bool applyOcio(const Effect&, Image&, std::string* error, FrameTime) {
    if (error) *error = "Montage was built without OpenColorIO";
    return false;
}
std::shared_ptr<const gpu::ShaderStep> ocioShaderStep(const Effect&, std::string* error) {
    if (error) *error = "Montage was built without OpenColorIO";
    return nullptr;
}
bool applyOcioBuiltin(const std::string&, float*, size_t, bool, std::string* error) {
    if (error) *error = "Montage was built without OpenColorIO";
    return false;
}
bool applyOcioFile(const std::string&, float*, size_t, std::string* error) {
    if (error) *error = "Montage was built without OpenColorIO";
    return false;
}
bool aces2OutputAvailable() { return false; }
bool applyAces2Output(const std::string&, const std::string&, float*, size_t, bool, std::string* error) {
    if (error) *error = "Montage was built without OpenColorIO";
    return false;
}

#endif

}  // namespace montage
