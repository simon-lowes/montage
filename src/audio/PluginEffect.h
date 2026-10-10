// Montage — clip effects that run third-party audio plugins.
#pragma once

#include <optional>
#include <string>

#include "Plugins.h"
#include "core/Model.h"

namespace montage::plugins {

// Builds a "plugin" clip effect for `d`: the plugin's identity, its initial
// state, and one keyframeable parameter per automatable plugin parameter.
// Loads the plugin briefly to read its parameters. A plugin with a key input
// is marked ("key_input"); its "sidechain" string names the key track's id.
std::optional<Effect> makePluginEffect(Project& p, const Descriptor& d, std::string* error = nullptr);

// Effect type strings used by browsers and drag and drop: catalogue types
// ("eq3") or "plugin:<descriptor id>" for an installed plugin.
constexpr const char* kPluginTypePrefix = "plugin:";
bool isPluginType(const std::string& type);
std::string pluginType(const Descriptor& d);
// Builds either kind; nullopt (with `error`) if a plugin cannot be loaded.
std::optional<Effect> makeEffectOfType(Project& p, const std::string& type, std::string* error = nullptr);
// Display name for a type string, or for an effect in a clip's stack.
std::string effectTypeName(const std::string& type);
std::string effectName(const Effect& e);

// Plugin state is opaque bytes, kept base64-encoded in the effect's "state" string.
std::string encodeState(const std::string& bytes);
std::string decodeState(const std::string& base64);

}  // namespace montage::plugins
