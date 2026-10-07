#include "PluginEffect.h"

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

}  // namespace montage::plugins
