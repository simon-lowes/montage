#include "MidiMaps.h"

#include <algorithm>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

namespace montage::midimaps {

using midimap::Encoding;
using midimap::Mapping;

QJsonObject toJson(const Mapping& m) {
    QJsonObject o{{"control", QString::fromStdString(midimap::keyName(m.key))},
                  {"encoding", QString::fromStdString(midimap::encodingName(m.encoding))},
                  {"target", QString::fromStdString(m.target)}};
    if (!m.layer.empty()) o["layer"] = QString::fromStdString(m.layer);
    if (m.low != 0) o["low"] = m.low;
    if (m.high != 1) o["high"] = m.high;
    if (m.curve != 1) o["curve"] = m.curve;
    if (m.invert) o["invert"] = true;
    if (m.toggle) o["toggle"] = true;
    if (!m.pickup) o["pickup"] = false;
    if (m.step != 1.0 / 128) o["step"] = m.step;
    return o;
}

bool fromJson(const QJsonObject& o, Mapping& out, QString* error) {
    auto fail = [&](const QString& why) {
        if (error) *error = why;
        return false;
    };
    const QString control = o.value("control").toString();
    const auto key = midimap::keyFromName(control.toStdString());
    if (!key) return fail(QStringLiteral("unknown control \"%1\" (e.g. cc:1:7, cc14:1:7, note:10:36, nrpn:1:388)").arg(control));
    Mapping m;
    m.key = *key;
    if (o.contains("encoding")) {
        const auto e = midimap::encodingFromName(o.value("encoding").toString().toStdString());
        if (!e) return fail(QStringLiteral("unknown encoding \"%1\"").arg(o.value("encoding").toString()));
        m.encoding = *e;
    } else if (key->kind == midimap::Kind::Note || key->kind == midimap::Kind::Program) {
        m.encoding = Encoding::Button;
    }
    // Notes and programs are buttons; 14-bit controls, NRPN, RPN and pitch bend positions; 7-bit controllers any.
    const bool buttonOnly = key->kind == midimap::Kind::Note || key->kind == midimap::Kind::Program;
    const bool wide = key->kind != midimap::Kind::Cc && !buttonOnly;
    if ((buttonOnly && m.encoding != Encoding::Button) || (wide && m.encoding != Encoding::Absolute))
        return fail(QStringLiteral("%1 cannot be read as %2").arg(control, QString::fromStdString(midimap::encodingName(m.encoding))));
    const QString target = o.value("target").toString();
    QString why;
    if (!validTarget(target, &why)) return fail(why);
    m.target = target.toStdString();
    m.layer = o.value("layer").toString().toStdString();
    m.low = o.value("low").toDouble(0);
    m.high = o.value("high").toDouble(1);
    m.curve = o.value("curve").toDouble(1);
    if (!(m.curve > 0.01 && m.curve < 100)) return fail(QStringLiteral("curve must be between 0.01 and 100"));
    if (m.low < 0 || m.low > 1 || m.high < 0 || m.high > 1) return fail(QStringLiteral("low and high are parts of the target's range, 0 to 1"));
    m.invert = o.value("invert").toBool(false);
    m.toggle = o.value("toggle").toBool(false);
    m.pickup = o.value("pickup").toBool(true);
    m.step = o.value("step").toDouble(1.0 / 128);
    if (!(m.step > 0 && m.step <= 1)) return fail(QStringLiteral("step must be more than 0 and at most 1"));
    out = m;
    return true;
}

QJsonObject toJson(const Map& map) {
    QJsonArray mappings;
    for (const Mapping& m : map.mappings) mappings.append(toJson(m));
    return QJsonObject{{"device", map.device}, {"bankSize", map.bankSize}, {"mappings", mappings}};
}

bool fromJson(const QJsonObject& o, Map& out, QString* error, QStringList* skipped) {
    if (!o.value("mappings").isArray() && o.contains("mappings")) {
        if (error) *error = QStringLiteral("\"mappings\" is not a list");
        return false;
    }
    Map map;
    map.device = o.value("device").toString();
    map.bankSize = std::clamp(o.value("bankSize").toInt(8), 1, 64);
    const QJsonArray mappings = o.value("mappings").toArray();
    for (int i = 0; i < mappings.size(); ++i) {
        Mapping m;
        QString why;
        if (!fromJson(mappings[i].toObject(), m, &why)) {
            if (skipped) *skipped << QStringLiteral("mapping %1: %2").arg(i + 1).arg(why);
            continue;
        }
        map.mappings.push_back(m);
    }
    out = std::move(map);
    return true;
}

QString folder() {
    const QString env = qEnvironmentVariable("MONTAGE_MIDI_MAPS");
    if (!env.isEmpty()) return env;
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/MIDI Maps");
}

QString fileFor(const QString& dir, const QString& device) {
    QString name = device.trimmed();
    name.replace(QRegularExpression(QStringLiteral("[/\\\\:*?\"<>|\\x00-\\x1f]")), QStringLiteral("_"));
    while (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' '))) name.chop(1);
    if (name.isEmpty()) name = QStringLiteral("Controller");
    return QDir(dir).filePath(name.left(120) + QStringLiteral(".json"));
}

bool load(const QString& file, Map& out, QString* error, QStringList* skipped) {
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("cannot read %1").arg(file);
        return false;
    }
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(f.read(4 << 20), &pe);
    if (!doc.isObject()) {
        if (error) *error = QStringLiteral("%1 is not a MIDI map (%2)").arg(file, pe.errorString());
        return false;
    }
    return fromJson(doc.object(), out, error, skipped);
}

bool save(const QString& file, const Map& map, QString* error) {
    QDir().mkpath(QFileInfo(file).absolutePath());
    QSaveFile f(file);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error) *error = QStringLiteral("cannot write %1").arg(file);
        return false;
    }
    f.write(QJsonDocument(toJson(map)).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        if (error) *error = QStringLiteral("cannot write %1").arg(file);
        return false;
    }
    return true;
}

QStringList devices(const QString& dir) {
    QStringList out;
    for (const QFileInfo& fi : QDir(dir).entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Name)) {
        Map m;
        if (load(fi.absoluteFilePath(), m)) out << (m.device.isEmpty() ? fi.completeBaseName() : m.device);
    }
    return out;
}

bool validTarget(const QString& t, QString* error) {
    static const QRegularExpression forms(QRegularExpression::anchoredPattern(QStringLiteral(
        "command:\\S[^\\n]*|widget:[^#/\\s][^#/]*/[^#\\s][^#\\n]*(#[xy])?|strip:[1-9][0-9]?:(volume|pan|mute|solo)|"
        "master:volume|bank:(previous|next)")));
    if (forms.match(t).hasMatch()) return true;
    if (error)
        *error = QStringLiteral("unknown target \"%1\" (command:<id>, widget:<path>, strip:<n>:volume|pan|mute|solo, "
                                "master:volume, bank:previous|next)")
                     .arg(t);
    return false;
}

}  // namespace montage::midimaps
