// Montage — project file (.montage, JSON) reading and writing.
#pragma once

#include <string>

#include "Model.h"

namespace montage {

constexpr int kProjectFormatVersion = 1;

// Serialises to a JSON document. Media paths are stored absolute and also
// relative to `projectPath`'s directory, so a project folder can be moved.
std::string projectToJson(const Project& p, const std::string& projectPath = {});
// One clip as JSON (as in project files).
std::string clipToJsonString(const Clip& c);
bool clipFromJsonString(const std::string& json, Clip& out);
bool projectFromJson(const std::string& json, Project& out, std::string* error = nullptr,
                     const std::string& projectPath = {});

bool saveProject(const Project& p, const std::string& path, std::string* error = nullptr);
bool loadProject(const std::string& path, Project& out, std::string* error = nullptr);

}  // namespace montage
