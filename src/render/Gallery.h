// Montage — the colour gallery (Resolve's Gallery, Baselight's stills): stills of the Program monitor, each with the
// grade of the clip it shows, kept in albums. A still can be wiped against the picture (the Program monitor's compare
// divider) and its grade applied to other clips. Stills are files, beside the project in "Gallery/<project>/<album>":
// the picture as a PNG (as the viewer shows it, Rec.709) and the grade as JSON (its effects, where they sat among the
// clip's others, its node graph and the shared nodes the graph shows), so they outlive the project and can be copied.
#pragma once

#include <string>
#include <vector>

#include "core/EditOps.h"
#include "core/Model.h"

namespace montage {

struct GalleryStill {
    std::string id;       // unique in its album
    std::string album;    // its folder's name
    std::string label;    // "1", "2"... unless renamed
    std::string picture;  // the PNG
    std::string clip, sequence, timecode;  // where it was grabbed
    std::string created;  // ISO date and time
    int width = 0, height = 0;
    // The grade.
    std::vector<Effect> effects;
    std::vector<int> anchors;
    GradeGraph graph;
    std::vector<SharedNode> sharedNodes;
};

// The project's gallery folder (`projectPath` "" for a project not yet saved: the user's Montage folder).
std::string galleryFolder(const std::string& projectPath);
constexpr const char* kDefaultAlbum = "Stills";
// Its albums, oldest first ("Stills" always among them).
std::vector<std::string> galleryAlbums(const std::string& folder);
bool addAlbum(const std::string& folder, const std::string& name, std::string* error = nullptr);
// The stills of an album, oldest first.
std::vector<GalleryStill> galleryStills(const std::string& folder, const std::string& album);
// By its id, else its label (false when several have it).
bool findStill(const std::string& folder, const std::string& album, const std::string& idOrLabel, GalleryStill& out);

// Grabs the Program monitor's picture at `t` with the grade of the top clip there into `album` (made if need be);
// at most `maxWidth` pixels wide. False with `error` when no clip shows there.
bool grabStill(const Project& p, const Sequence& s, FrameTime t, const std::string& folder, const std::string& album,
               GalleryStill& out, std::string* error = nullptr, int maxWidth = 1920);
bool removeStill(const std::string& folder, const GalleryStill& still);
// False when another still of its album has the label.
bool renameStill(const std::string& folder, GalleryStill& still, const std::string& label, std::string* error = nullptr);
// The stills this run grabbed into the unsaved project's gallery (galleryFolder("")), moved to `folder` once the
// project has a file; how many.
int moveUntitledStills(const std::string& folder);
// (Another project took the unsaved one's place: its stills stay where they are.)
void forgetUntitledStills();

namespace edit {
// The still's grade in place of each clip's (their other effects and grade versions left alone).
Result applyStillGrade(Project& p, Sequence& s, const GalleryStill& still, const std::vector<Id>& clips);
}  // namespace edit

}  // namespace montage
