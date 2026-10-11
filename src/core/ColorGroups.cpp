#include "ColorGroups.h"

#include <algorithm>

namespace montage {

const ColorGroup* findColorGroup(const Sequence& s, Id group) {
    if (!group) return nullptr;
    for (const ColorGroup& g : s.colorGroups)
        if (g.id == group) return &g;
    return nullptr;
}

ColorGroup* findColorGroup(Sequence& s, Id group) { return const_cast<ColorGroup*>(findColorGroup(std::as_const(s), group)); }

const ColorGroup* colorGroupOf(const Sequence& s, const Clip& c) { return findColorGroup(s, c.colorGroup); }

std::vector<const Effect*> gradeChain(const Sequence& s, const Clip& c) {
    std::vector<const Effect*> out;
    const ColorGroup* g = colorGroupOf(s, c);
    if (g)
        for (const Effect& e : g->pre) out.push_back(&e);
    for (const Effect& e : c.effects) out.push_back(&e);
    if (g)
        for (const Effect& e : g->post) out.push_back(&e);
    return out;
}

std::vector<Id> colorGroupMembers(const Sequence& s, Id group) {
    std::vector<std::pair<FrameTime, Id>> found;
    for (const Track& t : s.videoTracks)
        for (const Clip& c : t.clips)
            if (group && c.colorGroup == group) found.push_back({c.start, c.id});
    std::sort(found.begin(), found.end());
    std::vector<Id> out;
    for (const auto& [at, id] : found) out.push_back(id);
    return out;
}

namespace edit {

namespace {

// The video clips among `clips` (linked sound and other kinds left out).
std::vector<Clip*> videoClips(Sequence& s, const std::vector<Id>& clips) {
    std::vector<Clip*> out;
    for (Track& t : s.videoTracks)
        for (Clip& c : t.clips)
            if (std::find(clips.begin(), clips.end(), c.id) != clips.end()) out.push_back(&c);
    return out;
}

}  // namespace

Result makeColorGroup(Project& p, Sequence& s, const std::vector<Id>& clips, const std::string& name, Id* created) {
    const std::vector<Clip*> members = videoClips(s, clips);
    if (members.empty()) return Result::fail("Select the video clips to group");
    ColorGroup g;
    g.id = p.newId();
    g.postId = p.newId();
    g.name = name;
    if (g.name.empty()) {
        for (int n = int(s.colorGroups.size()) + 1;; ++n) {
            const std::string candidate = "Group " + std::to_string(n);
            if (std::none_of(s.colorGroups.begin(), s.colorGroups.end(), [&](const ColorGroup& x) { return x.name == candidate; })) {
                g.name = candidate;
                break;
            }
        }
    }
    for (Clip* c : members) c->colorGroup = g.id;
    s.colorGroups.push_back(std::move(g));
    if (created) *created = s.colorGroups.back().id;
    return {};
}

Result addToColorGroup(Sequence& s, const std::vector<Id>& clips, Id group) {
    if (!findColorGroup(s, group)) return Result::fail("No such colour group");
    bool any = false;
    for (Clip* c : videoClips(s, clips))
        if (c->colorGroup != group) c->colorGroup = group, any = true;
    return any ? Result{} : Result::fail("Those clips are in the group already");
}

Result removeFromColorGroup(Sequence& s, const std::vector<Id>& clips) {
    bool any = false;
    for (Clip* c : videoClips(s, clips))
        if (c->colorGroup) c->colorGroup = 0, any = true;
    return any ? Result{} : Result::fail("Those clips are in no colour group");
}

Result renameColorGroup(Sequence& s, Id group, const std::string& name) {
    ColorGroup* g = findColorGroup(s, group);
    if (!g) return Result::fail("No such colour group");
    if (name.empty() || name == g->name) return Result::fail("");
    g->name = name;
    return {};
}

Result deleteColorGroup(Sequence& s, Id group) {
    const auto it = std::find_if(s.colorGroups.begin(), s.colorGroups.end(), [&](const ColorGroup& g) { return g.id == group; });
    if (it == s.colorGroups.end()) return Result::fail("No such colour group");
    s.colorGroups.erase(it);
    for (Track& t : s.videoTracks)
        for (Clip& c : t.clips)
            if (c.colorGroup == group) c.colorGroup = 0;
    return {};
}

}  // namespace edit

}  // namespace montage
