// Montage — clip effects that run third-party audio plugins.
#pragma once

#include <optional>
#include <string>

#include "Plugins.h"
#include "core/Model.h"

namespace montage::plugins {

// Builds a "plugin" clip effect for `d`: the plugin's identity, its initial
// state, and one keyframeable parameter per automatable plugin parameter.
// Loads the plugin briefly to read its parameters.
std::optional<Effect> makePluginEffect(Project& p, const Descriptor& d, std::string* error = nullptr);

// Plugin state is opaque bytes, kept base64-encoded in the effect's "state" string.
std::string encodeState(const std::string& bytes);
std::string decodeState(const std::string& base64);

}  // namespace montage::plugins
