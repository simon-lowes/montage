#include "PluginEffect.h"
#include "render/Ofx.h"

#include <QByteArray>

#include "core/Effects.h"

namespace montage::plugins {

std::string encodeState(const std::string& bytes) { return QByteArray::fromStdString(bytes).toBase64().toStdString(); }

std::string decodeState(const std::string& base64) {
    return QByteArray::fromBase64(QByteArray::fromStdString(base64)).toStdString();
}

std::optional<Effect> makePluginEffect(Project& p, const Descriptor& d, std::string* error) {
    auto inst = instantiate(d, error);
    if (!inst) return std::nullopt;
    Effect e = makeEffect(p, "plugin");
    e.strings["plugin_id"] = d.id;
    e.strings["plugin_name"] = d.name;
    e.strings["plugin_vendor"] = d.vendor;
    e.strings["plugin_format"] = formatName(d.format);
    e.strings["state"] = encodeState(inst->saveState());
    // A key input: the effect then offers a Sidechain track ("sidechain", as the Compressor's).
    if (inst->hasSidechain()) e.strings["key_input"] = "1";
    for (const plugins::ParamInfo& pi : inst->parameters()) {
        if (!pi.automatable) continue;
        const std::string id = std::to_string(pi.id);
        montage::ParamInfo meta;
        meta.label = pi.name;
        meta.min = pi.min;
        meta.max = pi.max;
        meta.def = pi.def;
        meta.step = pi.stepped ? 1 : 0.01;
        e.params["param." + id] = Param(inst->parameter(pi.id));
        e.strings["meta." + id] = pluginParamMeta(meta);
    }
    return e;
}

bool isPluginType(const std::string& type) { return type.rfind(kPluginTypePrefix, 0) == 0; }

std::string pluginType(const Descriptor& d) { return kPluginTypePrefix + d.id; }

std::optional<Effect> makeEffectOfType(Project& p, const std::string& type, std::string* error) {
    if (ofx::isOfxType(type)) {
        // An OpenFX video plugin (render/Ofx.h).
        ofx::PluginDesc d;
        if (!ofx::Registry::instance().find(type.substr(std::char_traits<char>::length(ofx::kTypePrefix)), d)) {
            if (error) *error = "the OpenFX plugin is not installed";
            return std::nullopt;
        }
        return ofx::makeEffect(p, d);
    }
    if (!isPluginType(type)) return makeEffect(p, type);
    auto d = Registry::instance().find(type.substr(std::char_traits<char>::length(kPluginTypePrefix)));
    if (!d) {
        if (error) *error = "the plugin is not installed";
        return std::nullopt;
    }
    return makePluginEffect(p, *d, error);
}

std::string effectTypeName(const std::string& type) {
    if (ofx::isOfxType(type)) {
        ofx::PluginDesc d;
        return ofx::Registry::instance().find(type.substr(std::char_traits<char>::length(ofx::kTypePrefix)), d) ? d.label : type;
    }
    if (isPluginType(type)) {
        auto d = Registry::instance().find(type.substr(std::char_traits<char>::length(kPluginTypePrefix)));
        return d ? d->name : type;
    }
    const EffectInfo* info = findEffectInfo(type);
    return info ? info->displayName : type;
}

std::string effectName(const Effect& e) {
    if (e.type == "ofx") return ofx::effectName(e);
    if (e.type == "plugin") {
        const std::string fmt = e.s("plugin_format");
        return e.s("plugin_name", "Audio Plugin") + (fmt.empty() ? "" : " (" + fmt + ")");
    }
    return effectTypeName(e.type);
}

}  // namespace montage::plugins
