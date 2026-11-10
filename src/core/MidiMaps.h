// Montage — MIDI Learn's maps on disk: one JSON file per controller, named after the device (so a controller
// unplugged and plugged back in, on whatever port, finds its map again), in the app's data folder ("MIDI Maps").
// The app (app/MidiLearn) reads and writes them as mappings are learnt; the MCP tool montage_midi_map edits them.
#pragma once

#include "control/MidiMap.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <vector>

namespace montage::midimaps {

struct Map {
    QString device;     // the controller's name, as its MIDI port gives it
    int bankSize = 8;   // the strips a bank moves by
    std::vector<midimap::Mapping> mappings;  // (a control may have several in a layer, one a target)
};

QJsonObject toJson(const midimap::Mapping& m);
// A mapping read back (false, with why, for an unknown control, encoding or a missing target).
bool fromJson(const QJsonObject& o, midimap::Mapping& out, QString* error = nullptr);
QJsonObject toJson(const Map& map);
// A map read back; a mapping that cannot be read is left out (and said why in `skipped`), the rest kept.
bool fromJson(const QJsonObject& o, Map& out, QString* error = nullptr, QStringList* skipped = nullptr);

// Where maps are kept: $MONTAGE_MIDI_MAPS, else "MIDI Maps" in the app's data folder.
QString folder();
// The file for a device's map in a folder (the name made safe for every file system).
QString fileFor(const QString& folder, const QString& device);
bool load(const QString& file, Map& out, QString* error = nullptr, QStringList* skipped = nullptr);
bool save(const QString& file, const Map& map, QString* error = nullptr);
// The devices with maps in a folder.
QStringList devices(const QString& folder);

// What a target string names, checked for form (whether it exists is the app's to say): "command:<id>",
// "widget:<path>" (optionally "#x" or "#y" for a colour wheel's puck), "strip:<n>:volume|pan|mute|solo" (n from 1,
// counted from the bank's first track), "master:volume", "bank:previous|next".
bool validTarget(const QString& target, QString* error = nullptr);

}  // namespace montage::midimaps
