#include "LearnFromClip.h"

#include <QDir>
#include <QFileInfo>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <set>

#include "Compositor.h"
#include "core/EditOps.h"
#include "core/Effects.h"

namespace montage {

namespace {

// An effect a Learned Effect stands in for, turned off when it was given (marked on the effect itself, so it stays
// marked through copies, which give effects new ids).
bool learnedOff(const Effect& e) {
    const auto it = e.strings.find("learned_off");
    return it != e.strings.end() && it->second == "1";
}

}  // namespace

bool learningPair(const Project& p, const Sequence& s, const Clip& c, FrameTime t, learn::Pair& out, std::string* error) {
    double fw = 0, fh = 0;
    if (c.mediaId == 0 && !c.isGenerator()) {
        if (error) *error = "The clip has no picture";
        return false;
    }
    if (!clipFrameSize(p, s, c, fw, fh) || fw < 1 || fh < 1) {
        fw = s.width;
        fh = s.height;
    }
    // A sequence of the clip alone, its own size, with the clip unmoved and filling it.
    Sequence alone = s;
    alone.width = std::max(8, int(std::lround(fw)));
    alone.height = std::max(8, int(std::lround(fh)));
    for (auto* tracks : {&alone.videoTracks, &alone.audioTracks}) tracks->clear();
    alone.captionTracks.clear();
    Track track;
    track.kind = TrackKind::Video;
    track.name = "V1";
    Clip clip = c;
    clip.motion = makeEffect("transform", c.motion.id);
    clip.enabled = true;
    // Its own effects, those a Learned Effect stands in for on again; not its grade's nodes or its colour group's
    // post-clip grade, which run after the network. (The group's pre-clip grade runs before it: in both pictures.)
    for (Effect& e : clip.effects)
        if (learnedOff(e)) e.enabled = true;
    std::erase_if(clip.effects, [](const Effect& e) { return e.type == "learned_effect" || e.node != 0; });
    clip.grade = {};
    for (ColorGroup& g : alone.colorGroups)
        if (g.id == c.colorGroup) g.post.clear();
    track.clips.push_back(clip);
    alone.videoTracks.push_back(track);
    RenderOptions o;
    o.highQuality = true;
    out.after = renderSequenceFrame(p, alone, t, o);
    // And without them.
    Clip& bare = alone.videoTracks[0].clips[0];
    bare.effects.clear();
    out.before = renderSequenceFrame(p, alone, t, o);
    out.frame = t;
    if (out.before.empty() || out.after.empty() || out.before.width != out.after.width || out.before.height != out.after.height) {
        if (error) *error = "The clip's picture could not be rendered at that frame";
        return false;
    }
    return true;
}

std::vector<FrameTime> exampleFrames(const Sequence& s, const Clip& c, FrameTime playhead) {
    std::set<FrameTime> frames;
    for (const Marker& m : s.markers)
        if (c.contains(m.t)) frames.insert(m.t);
    if (frames.empty())
        for (const Effect& e : c.effects)
            if ((e.enabled || learnedOff(e)) && e.type != "learned_effect")
                for (const auto& [name, param] : e.params)
                    for (const Keyframe& k : param.keys)
                        if (c.contains(c.start + k.t)) frames.insert(c.start + k.t);
    if (frames.empty() && c.contains(playhead)) frames.insert(playhead);
    return {frames.begin(), frames.end()};
}

std::string learnedNetworkOf(const Clip& c) {
    for (const Effect& e : c.effects)
        if (e.type == "learned_effect")
            if (const auto it = e.strings.find("network"); it != e.strings.end() && !it->second.empty()) return it->second;
    return {};
}

std::string newLearnedNetworkPath(const std::string& projectPath, const Project& p, const Clip& c, bool reuse) {
    QString folder, stem;
    if (projectPath.empty()) {
        // (Clips' ids repeat between projects: a project never saved is told apart by a name of its own.)
        folder = QDir::tempPath() + QStringLiteral("/Montage Learned Effects");
        stem = QStringLiteral("untitled-%1-clip-%2").arg(QUuid::createUuid().toString(QUuid::Id128).left(8)).arg(qulonglong(c.id));
    } else {
        const QFileInfo project(QString::fromStdString(projectPath));
        folder = project.absolutePath() + QLatin1Char('/') + project.completeBaseName() + QStringLiteral(" Learned Effects");
        stem = QStringLiteral("clip-%1").arg(qulonglong(c.id));
    }
    QDir().mkpath(folder);
    std::set<std::string> used;
    for (const Sequence& s : p.sequences)
        for (const auto* tracks : {&s.videoTracks, &s.audioTracks})
            for (const Track& t : *tracks)
                for (const Clip& other : t.clips)
                    if (std::string n = learnedNetworkOf(other); !n.empty()) used.insert(QFileInfo(QString::fromStdString(n)).absoluteFilePath().toStdString());
    for (int n = 1;; ++n) {
        const QString path = folder + QLatin1Char('/') + stem + (n == 1 ? QString() : QStringLiteral("-%1").arg(n)) + QStringLiteral(".mlnet");
        if ((!QFileInfo::exists(path) || (reuse && !projectPath.empty())) && !used.count(QFileInfo(path).absoluteFilePath().toStdString()))
            return path.toStdString();
    }
}

bool networkInUse(const Project& p, const std::string& network) {
    const QString wanted = QFileInfo(QString::fromStdString(network)).absoluteFilePath();
    for (const Sequence& s : p.sequences)
        for (const Track& t : s.videoTracks)
            for (const Clip& c : t.clips)
                for (const Effect& e : c.effects)
                    if (e.type == "learned_effect")
                        if (const auto it = e.strings.find("network");
                            it != e.strings.end() && QFileInfo(QString::fromStdString(it->second)).absoluteFilePath() == wanted)
                            return true;
    return false;
}

int applyLearnedNetwork(Project& p, Sequence& s, const std::vector<Id>& clips, const std::string& network, Id examples, bool replaceExamples) {
    int changed = 0;
    for (Id id : clips) {
        const auto loc = edit::locate(s, id);
        Clip* c = edit::clipById(s, id);
        if (!c || !loc || loc->track.kind != TrackKind::Video) continue;
        auto it = std::find_if(c->effects.begin(), c->effects.end(), [](const Effect& e) { return e.type == "learned_effect"; });
        if (it == c->effects.end()) {
            c->effects.insert(c->effects.begin(), makeEffect(p, "learned_effect"));
            it = c->effects.begin();
        }
        it->strings["network"] = network;
        it->enabled = true;
        if (replaceExamples && id == examples)
            for (Effect& e : c->effects)
                if (e.type != "learned_effect" && e.node == 0 && e.enabled) {
                    e.enabled = false;
                    e.strings["learned_off"] = "1";  // (so the examples can be made again from the clip as it was worked on)
                }
        ++changed;
    }
    return changed;
}

}  // namespace montage
