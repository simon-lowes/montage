// Montage — productions (Premiere's Productions, Avid's shared projects, Resolve's shared project libraries): a
// folder on a shared drive holding a team's projects (one per reel, episode or editor), each locked by whoever is
// editing it (core/ProjectLock.h) and readable by everyone else, with sequences brought from one project into
// another, their media linked where it is, never copied.
#pragma once

#include <QDateTime>
#include <string>
#include <vector>

#include "Model.h"
#include "ProjectLock.h"

namespace montage {

struct ProductionProject {
    std::string path;      // the .montage file
    std::string relative;  // its path within the production, '/' between folders
    std::string name;
    QDateTime modified;
    LockStatus lock;
};

// A production is a folder with a "production.json" naming it.
bool isProduction(const std::string& folder);
bool createProduction(const std::string& folder, const std::string& name, std::string* error = nullptr);
std::string productionName(const std::string& folder);
// The production a project is in (the nearest folder above it that is one), or "".
std::string productionOf(const std::string& projectPath);
// The projects in a production (its subfolders too), by their path within it, each with who is editing it.
std::vector<ProductionProject> listProduction(const std::string& folder);

// Brings sequences of another project into `into`: each with the sequences it nests, the media its clips play (a
// file the project already has is used, not added twice; subclips keep their source) and new ids throughout, each as
// a sequence of its own in the bin. Returns the new ids of the sequences asked for, in order (none when none exist).
std::vector<Id> importFromProject(Project& into, const Project& from, const std::vector<Id>& sequences);

}  // namespace montage
