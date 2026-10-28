#include "ColorGroups.h"

#include <algorithm>
#include <map>
#include <set>

namespace montage {

const ColorGroup* findColorGroup(const Sequence& s, Id group) {
    if (!group) return nullptr;
    for (const ColorGroup& g : s.colorGroups)
        if (g.id == group) return &g;
    return nullptr;
}

ColorGroup* findColorGroup(Sequence& s, Id group) { return const_cast<ColorGroup*>(findColorGroup(std::as_const(s), group)); }

const ColorGroup* colorGroupOf(const Sequence& s, const Clip& c) { return findColorGroup(s, c.colorGroup); }

std::vector<const ColorGroup*> colorGroupsOf(const Sequence& s, const Clip& c) {
    std::vector<const ColorGroup*> out;
    for (const ColorGroup* g = colorGroupOf(s, c); g && out.size() < 8; g = findColorGroup(s, g->parent)) {
        if (std::find(out.begin(), out.end(), g) != out.end()) break;  // (a loop, which edits refuse to make)
        out.insert(out.begin(), g);
    }
    return out;
}

std::vector<const Effect*> gradeChain(const Sequence& s, const Clip& c) {
    std::vector<const Effect*> out;
    const std::vector<const ColorGroup*> groups = colorGroupsOf(s, c);
    for (const ColorGroup* g : groups)
        for (const Effect& e : g->pre) out.push_back(&e);
    for (const Effect& e : c.effects) out.push_back(&e);
    // The shared grade nodes its graph uses (core/GradeGraph.h).
    for (const GradeNode& n : c.grade.nodes)
        if (n.shared)
            for (const SharedNode& sn : s.sharedNodes)
                if (sn.id == n.shared)
                    for (const Effect& e : sn.effects) out.push_back(&e);
    for (auto g = groups.rbegin(); g != groups.rend(); ++g)
        for (const Effect& e : (*g)->post) out.push_back(&e);
    return out;
}

std::vector<Id> colorGroupMembers(const Sequence& s, Id group, bool nested) {
    std::vector<std::pair<FrameTime, Id>> found;
    auto inside = [&](const Clip& c) {
        if (!nested) return c.colorGroup == group;
        const std::vector<const ColorGroup*> chain = colorGroupsOf(s, c);
        return std::any_of(chain.begin(), chain.end(), [&](const ColorGroup* g) { return g->id == group; });
    };
    for (const Track& t : s.videoTracks)
        for (const Clip& c : t.clips)
            if (group && inside(c)) found.push_back({c.start, c.id});
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
    for (ColorGroup& g : s.colorGroups)
        if (g.parent == group) g.parent = 0;
    return {};
}

namespace {

// A copy of a group's grades with ids of their own.
std::vector<Effect> copiedEffects(Project& p, const std::vector<Effect>& v) {
    std::vector<Effect> out = v;
    for (Effect& e : out) e.id = p.newId();
    return out;
}

std::string freeGroupName(const Sequence& s, const std::string& base) {
    for (int n = 1;; ++n) {
        const std::string candidate = base + " " + std::to_string(n);
        if (std::none_of(s.colorGroups.begin(), s.colorGroups.end(), [&](const ColorGroup& x) { return x.name == candidate; })) return candidate;
    }
}

}  // namespace

Result splitColorGroup(Project& p, Sequence& s, const std::vector<Id>& clips, Id* created) {
    const std::vector<Clip*> members = videoClips(s, clips);
    const ColorGroup* from = members.empty() ? nullptr : findColorGroup(s, members.front()->colorGroup);
    if (!from) return Result::fail("Select clips in a colour group to split it");
    if (std::any_of(members.begin(), members.end(), [&](const Clip* c) { return c->colorGroup != from->id; }))
        return Result::fail("Those clips are in different groups");
    if (colorGroupMembers(s, from->id).size() == members.size()) return Result::fail("Those are all its clips: there is nothing to split off");
    ColorGroup g;
    g.id = p.newId();
    g.postId = p.newId();
    g.name = freeGroupName(s, from->name);
    g.pre = copiedEffects(p, from->pre);
    g.post = copiedEffects(p, from->post);
    g.parent = from->parent;
    g.kind = from->kind;
    for (Clip* c : members) c->colorGroup = g.id;
    s.colorGroups.push_back(std::move(g));
    if (created) *created = s.colorGroups.back().id;
    return {};
}

Result joinColorGroups(Sequence& s, Id group, const std::vector<Id>& others) {
    const ColorGroup* into = findColorGroup(s, group);
    if (!into) return Result::fail("No such colour group");
    std::vector<Id> gone;
    for (Id o : others)
        if (o != group && findColorGroup(s, o)) gone.push_back(o);
    if (gone.empty()) return Result::fail("Choose the groups to join to it");
    // (a group cannot take in one of the groups around it)
    size_t steps = 0;
    for (const ColorGroup* g = findColorGroup(s, into->parent); g && steps++ < s.colorGroups.size(); g = findColorGroup(s, g->parent))
        if (std::find(gone.begin(), gone.end(), g->id) != gone.end()) return Result::fail("A group cannot be joined into a group inside it");
    for (Track& t : s.videoTracks)
        for (Clip& c : t.clips)
            if (std::find(gone.begin(), gone.end(), c.colorGroup) != gone.end()) c.colorGroup = group;
    for (ColorGroup& g : s.colorGroups)
        if (std::find(gone.begin(), gone.end(), g.parent) != gone.end()) g.parent = group;
    std::erase_if(s.colorGroups, [&](const ColorGroup& g) { return std::find(gone.begin(), gone.end(), g.id) != gone.end(); });
    return {};
}

Result setColorGroupLocked(Sequence& s, Id group, bool locked) {
    ColorGroup* g = findColorGroup(s, group);
    if (!g) return Result::fail("No such colour group");
    if (g->locked == locked) return Result::fail("");
    g->locked = locked;
    return {};
}

Result setColorGroupParent(Sequence& s, Id group, Id parent) {
    ColorGroup* g = findColorGroup(s, group);
    if (!g) return Result::fail("No such colour group");
    if (parent && !findColorGroup(s, parent)) return Result::fail("No such colour group");
    size_t steps = 0;
    for (const ColorGroup* q = findColorGroup(s, parent); q && steps++ < s.colorGroups.size(); q = findColorGroup(s, q->parent))
        if (q->id == group) return Result::fail("A group cannot go inside itself or a group inside it");
    if (g->parent == parent) return Result::fail("");
    g->parent = parent;
    return {};
}

Result applyShotGrouping(Project& p, Sequence& s, const ShotGrouping& found) {
    // The groups this remakes: those an earlier run made and nobody locked (a locked scene keeps its angles).
    std::set<Id> remade;
    for (const ColorGroup& g : s.colorGroups)
        if (!g.kind.empty() && !g.locked) remade.insert(g.id);
    for (const ColorGroup& g : s.colorGroups)
        if (g.locked)
            for (const ColorGroup& inner : s.colorGroups)
                if (inner.parent == g.id) remade.erase(inner.id);
    // A scene around a locked group stays too (the group keeps its place), though new angles may join it.
    std::set<Id> kept;
    for (const ColorGroup& g : s.colorGroups)
        if (g.locked && g.parent && remade.count(g.parent)) kept.insert(g.parent);
    for (Id g : kept) remade.erase(g);
    // An old group is reused by the new one that has most of its shots (and at least half of them, the first such
    // when it splits evenly; a kept scene's locked groups' shots not counted).
    std::map<Id, std::set<Id>> oldMembers;
    for (Id g : remade)
        for (Id c : colorGroupMembers(s, g, true)) oldMembers[g].insert(c);
    for (Id g : kept) {
        std::set<Id>& members = oldMembers[g];
        for (const Track& t : s.videoTracks)
            for (const Clip& c : t.clips) {
                const std::vector<const ColorGroup*> chain = colorGroupsOf(s, c);
                const bool inside = std::any_of(chain.begin(), chain.end(), [&](const ColorGroup* x) { return x->id == g; });
                const bool locked = std::any_of(chain.begin(), chain.end(), [&](const ColorGroup* x) { return x->locked; });
                if (inside && !locked) members.insert(c.id);
            }
    }
    std::set<Id> reused;
    auto reuse = [&](const std::vector<Id>& clips, const std::string& kind) -> Id {
        Id best = 0;
        size_t bestCount = 0;
        for (const auto& [g, members] : oldMembers) {
            const ColorGroup* old = findColorGroup(s, g);
            if (!old || old->kind != kind || reused.count(g)) continue;
            const size_t n = size_t(std::count_if(clips.begin(), clips.end(), [&](Id c) { return members.count(c) > 0; }));
            if (n > bestCount && 2 * n >= members.size()) best = g, bestCount = n;
        }
        if (best) reused.insert(best);
        return best;
    };
    std::vector<Id> sceneIds(found.scenes.size(), 0);
    for (size_t k = 0; k < found.scenes.size(); ++k) {
        // (the scene around a group that stays, which the scenes were found with)
        // (a locked scene takes no new angles)
        const ColorGroup* around = k < found.sceneGroups.size() ? findColorGroup(s, found.sceneGroups[k]) : nullptr;
        if (around && !around->locked && !reused.count(around->id) && !remade.count(around->id)) {
            sceneIds[k] = found.sceneGroups[k];
            reused.insert(sceneIds[k]);
            continue;
        }
        std::vector<Id> clips;
        for (const ShotGrouping::Angle& a : found.angles)
            if (a.scene == int(k)) clips.insert(clips.end(), a.clips.begin(), a.clips.end());
        if (clips.empty()) continue;
        sceneIds[k] = reuse(clips, "scene");
    }
    std::vector<Id> angleIds(found.angles.size(), 0);
    for (size_t k = 0; k < found.angles.size(); ++k) angleIds[k] = reuse(found.angles[k].clips, "angle");
    // Clips of the groups being remade leave them (those it could not describe, such as generators, stay in a group
    // reused); the groups not reused go (with their grades).
    std::set<Id> placed;
    for (const ShotGrouping::Angle& a : found.angles) placed.insert(a.clips.begin(), a.clips.end());
    for (Track& t : s.videoTracks)
        for (Clip& c : t.clips)
            if (remade.count(c.colorGroup) && (placed.count(c.id) || !reused.count(c.colorGroup))) c.colorGroup = 0;
    // (the grades of the groups remade, for the new groups made of their shots: a group split evenly is reused by one
    // half, its grade copied to the other)
    std::map<Id, ColorGroup> gone;
    for (const ColorGroup& g : s.colorGroups)
        if (remade.count(g.id) && (!g.pre.empty() || !g.post.empty())) gone[g.id] = g;
    std::erase_if(s.colorGroups, [&](const ColorGroup& g) { return remade.count(g.id) && !reused.count(g.id); });
    for (ColorGroup& g : s.colorGroups)
        if (g.parent && !findColorGroup(s, g.parent)) g.parent = 0;
    // A new group takes the grade of the graded group remade that held most of its shots (so a group split in two
    // keeps its grade in both halves).
    auto make = [&](const std::string& kind, const std::string& name, const std::vector<Id>& clips) {
        ColorGroup g;
        g.id = p.newId();
        g.postId = p.newId();
        g.kind = kind;
        g.name = name.empty() ? freeGroupName(s, kind == "scene" ? "Scene" : "Angle") : name;
        const ColorGroup* from = nullptr;
        size_t most = 0;
        for (const auto& [id, old] : gone) {
            if (old.kind != kind) continue;
            const std::set<Id>& members = oldMembers[id];
            const size_t n = size_t(std::count_if(clips.begin(), clips.end(), [&](Id c) { return members.count(c) > 0; }));
            if (n > most) from = &old, most = n;
        }
        if (from) {
            g.pre = from->pre, g.post = from->post;
            for (auto* list : {&g.pre, &g.post})
                for (Effect& e : *list) e.id = p.newId();
        }
        s.colorGroups.push_back(std::move(g));
        return s.colorGroups.back().id;
    };
    for (size_t k = 0; k < found.scenes.size(); ++k) {
        bool used = false;
        for (const ShotGrouping::Angle& a : found.angles) used = used || (a.scene == int(k) && !a.clips.empty());
        if (!used) continue;
        if (!sceneIds[k]) {
            std::vector<Id> clips;
            for (const ShotGrouping::Angle& a : found.angles)
                if (a.scene == int(k)) clips.insert(clips.end(), a.clips.begin(), a.clips.end());
            sceneIds[k] = make("scene", found.scenes[k], clips);
        }
        else findColorGroup(s, sceneIds[k])->parent = 0;
    }
    for (size_t k = 0; k < found.angles.size(); ++k) {
        const ShotGrouping::Angle& a = found.angles[k];
        if (a.clips.empty()) continue;
        if (!angleIds[k]) angleIds[k] = make("angle", "", a.clips);
        ColorGroup* g = findColorGroup(s, angleIds[k]);
        g->parent = a.scene >= 0 && size_t(a.scene) < sceneIds.size() ? sceneIds[size_t(a.scene)] : 0;
        for (Clip* c : videoClips(s, a.clips))  // (a clip in a kept group stays there, unless it is straight in a kept scene)
            if (!findColorGroup(s, c->colorGroup) || kept.count(c->colorGroup)) c->colorGroup = g->id;
    }
    return {};
}

}  // namespace edit

}  // namespace montage
