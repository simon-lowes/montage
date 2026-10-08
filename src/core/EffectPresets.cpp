#include "EffectPresets.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "ProjectIO.h"

namespace montage {

std::string presetToJson(const EffectPreset& p) {
    QJsonArray effects;
    for (const Effect& e : p.effects) effects.append(QJsonDocument::fromJson(QByteArray::fromStdString(effectToJsonString(e))).object());
    const QJsonObject root{{"format", "montage-effect-preset"},
                           {"version", 1},
                           {"name", QString::fromStdString(p.name)},
                           {"kind", p.video ? "video" : "audio"},
                           {"effects", effects}};
    return QJsonDocument(root).toJson(QJsonDocument::Indented).toStdString();
}

bool presetFromJson(const std::string& json, EffectPreset& out, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(json), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return fail("Not a preset file");
    const QJsonObject root = doc.object();
    if (root.value("format").toString() != "montage-effect-preset") return fail("Not a Montage effect preset");
    EffectPreset p;
    p.name = root.value("name").toString().toStdString();
    p.video = root.value("kind").toString() != "audio";
    for (const auto& v : root.value("effects").toArray()) {
        Effect e;
        if (effectFromJsonString(QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact).toStdString(), e)) p.effects.push_back(e);
    }
    if (p.effects.empty()) return fail("The preset has no effects");
    out = std::move(p);
    return true;
}

int applyPreset(Project& p, Clip& c, const EffectPreset& preset) {
    for (Effect e : preset.effects) {
        e.id = p.newId();
        c.effects.push_back(std::move(e));
    }
    return int(preset.effects.size());
}

}  // namespace montage
