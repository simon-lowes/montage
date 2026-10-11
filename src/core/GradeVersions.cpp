#include "GradeVersions.h"

#include <algorithm>

#include "Effects.h"

namespace montage {

bool isGradeEffect(const Effect& e) {
    const EffectInfo* info = findEffectInfo(e.type);
    return info && info->category == EffectCategory::VideoFilter && info->group == "Color";
}

namespace {

// Takes the grade out of the stack, noting where each effect was (how many other effects came before it).
std::vector<Effect> takeGrade(Clip& c, std::vector<int>& anchors) {
    std::vector<Effect> grade;
    anchors.clear();
    std::vector<Effect> rest;
    for (const Effect& e : c.effects) {
        if (isGradeEffect(e)) {
            grade.push_back(e);
            anchors.push_back(int(rest.size()));
        } else {
            rest.push_back(e);
        }
    }
    c.effects = std::move(rest);
    return grade;
}

// Puts a grade back among the other effects: each where it was, or (without places) where the grade it
// replaces was, else first.
void putGrade(Clip& c, const std::vector<Effect>& grade, const std::vector<int>& own, const std::vector<int>& replaced) {
    auto anchor = [&](size_t i) {
        const std::vector<int>& a = own.size() == grade.size() ? own : replaced;
        if (a.empty()) return 0;
        return a[std::min(i, a.size() - 1)];
    };
    std::vector<Effect> out;
    size_t next = 0;
    for (size_t k = 0; k <= c.effects.size(); ++k) {
        while (next < grade.size() && (anchor(next) <= int(k) || k == c.effects.size())) out.push_back(grade[next++]);
        if (k < c.effects.size()) out.push_back(c.effects[k]);
    }
    c.effects = std::move(out);
}

}  // namespace

namespace edit {

Result addGradeVersion(Project& p, Sequence& s, Id clipId, const std::string& name, bool empty) {
    Clip* c = clipById(s, clipId);
    if (!c) return Result::fail("No such clip");
    bool onVideo = false;
    for (const Track& t : s.videoTracks)
        for (const Clip& k : t.clips) onVideo = onVideo || k.id == clipId;
    if (!onVideo) return Result::fail("Grades are for picture clips");
    if (c->gradeVersions.empty()) {
        c->gradeVersions.push_back({"Version 1", {}, {}});
        c->gradeVersion = 0;
    }
    std::vector<int> anchors;
    std::vector<Effect> current = takeGrade(*c, anchors);
    c->gradeVersions[size_t(c->gradeVersion)].effects = current;
    c->gradeVersions[size_t(c->gradeVersion)].anchors = anchors;
    std::vector<Effect> fresh;
    if (!empty)
        for (const Effect& e : current) {
            Effect copy = e;
            copy.id = p.newId();
            fresh.push_back(copy);
        }
    const std::string label = name.empty() ? "Version " + std::to_string(c->gradeVersions.size() + 1) : name;
    c->gradeVersions.push_back({label, {}, {}});
    c->gradeVersion = int(c->gradeVersions.size()) - 1;
    putGrade(*c, fresh, empty ? std::vector<int>{} : anchors, anchors);
    return {};
}

Result switchGradeVersion(Sequence& s, Id clipId, int index) {
    Clip* c = clipById(s, clipId);
    if (!c) return Result::fail("No such clip");
    if (index < 0 || index >= int(c->gradeVersions.size())) return Result::fail("No such grade version");
    if (index == c->gradeVersion) return Result::fail("");
    std::vector<int> anchors;
    GradeVersion& from = c->gradeVersions[size_t(c->gradeVersion)];
    from.effects = takeGrade(*c, anchors);
    from.anchors = anchors;
    GradeVersion& to = c->gradeVersions[size_t(index)];
    putGrade(*c, to.effects, to.anchors, anchors);
    to.effects.clear();  // live in the stack now
    to.anchors.clear();
    c->gradeVersion = index;
    return {};
}

Result removeGradeVersion(Sequence& s, Id clipId, int index) {
    Clip* c = clipById(s, clipId);
    if (!c) return Result::fail("No such clip");
    if (index < 0 || index >= int(c->gradeVersions.size())) return Result::fail("No such grade version");
    if (c->gradeVersions.size() < 2) return Result::fail("A clip keeps at least one grade");
    if (index == c->gradeVersion) {
        const int show = index > 0 ? index - 1 : 1;
        if (Result r = switchGradeVersion(s, clipId, show); !r.ok) return r;
    }
    c->gradeVersions.erase(c->gradeVersions.begin() + index);
    if (c->gradeVersion > index) --c->gradeVersion;
    if (c->gradeVersions.size() == 1) c->gradeVersions.clear(), c->gradeVersion = 0;  // one grade: no versions
    return {};
}

Result renameGradeVersion(Sequence& s, Id clipId, int index, const std::string& name) {
    Clip* c = clipById(s, clipId);
    if (!c) return Result::fail("No such clip");
    if (index < 0 || index >= int(c->gradeVersions.size())) return Result::fail("No such grade version");
    if (name.empty()) return Result::fail("A version needs a name");
    if (c->gradeVersions[size_t(index)].name == name) return Result::fail("");
    c->gradeVersions[size_t(index)].name = name;
    return {};
}

}  // namespace edit

}  // namespace montage
