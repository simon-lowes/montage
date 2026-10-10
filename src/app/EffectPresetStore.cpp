#include "EffectPresetStore.h"

#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <algorithm>

namespace montage::presets {

QString folder() {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/Presets");
    QDir().mkpath(dir);
    return dir;
}

bool load(const QString& file, EffectPreset& out, QString* error) {
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = f.errorString();
        return false;
    }
    std::string why;
    if (!presetFromJson(f.readAll().toStdString(), out, &why)) {
        if (error) *error = QString::fromStdString(why);
        return false;
    }
    return true;
}

std::vector<std::pair<QString, EffectPreset>> all() {
    std::vector<std::pair<QString, EffectPreset>> out;
    const QDir dir(folder());
    for (const QString& name : dir.entryList({QStringLiteral("*.montagepreset")}, QDir::Files)) {
        EffectPreset p;
        if (load(dir.filePath(name), p)) out.push_back({dir.filePath(name), std::move(p)});
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return QString::fromStdString(a.second.name).compare(QString::fromStdString(b.second.name), Qt::CaseInsensitive) < 0;
    });
    return out;
}

QString save(const EffectPreset& preset, QString* error) {
    // A file name from the preset's name, safe on every system.
    QString base = QString::fromStdString(preset.name).trimmed();
    for (QChar& c : base)
        if (!c.isLetterOrNumber() && c != ' ' && c != '-' && c != '_') c = '_';
    if (base.isEmpty()) base = QStringLiteral("Preset");
    const QString file = QDir(folder()).filePath(base + QStringLiteral(".montagepreset"));
    QFile f(file);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = f.errorString();
        return {};
    }
    f.write(QByteArray::fromStdString(presetToJson(preset)));
    return file;
}

bool remove(const QString& file) { return QFile::remove(file); }

}  // namespace montage::presets
