// Montage — the effect presets saved on this computer (core/EffectPresets.h), one ".montagepreset" file each in the
// application's Presets folder, shown in the Effects browser.
#pragma once

#include <QString>
#include <utility>
#include <vector>

#include "core/EffectPresets.h"

namespace montage::presets {

QString folder();  // made if missing
// Every preset there, by name, with its file.
std::vector<std::pair<QString, EffectPreset>> all();
bool load(const QString& file, EffectPreset& out, QString* error = nullptr);
// Saves under the preset's name (replacing one of the same name); the file it wrote.
QString save(const EffectPreset& preset, QString* error = nullptr);
bool remove(const QString& file);

}  // namespace montage::presets
