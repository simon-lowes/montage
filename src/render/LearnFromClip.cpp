#include "LearnFromClip.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "ColorSpace.h"
#include "Compositor.h"
#include "core/ColorGroups.h"
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
    // Its own effects, those a Learned Effect stands in for on again; not its grade's nodes or its colour groups'
    // post-clip grades, which run after the network. (The groups' pre-clip grades run before it: in both pictures.)
    for (Effect& e : clip.effects)
        if (learnedOff(e)) e.enabled = true;
    std::erase_if(clip.effects, [](const Effect& e) { return e.type == "learned_effect" || e.node != 0; });
    clip.grade = {};
    for (const ColorGroup* around : colorGroupsOf(s, c))
        for (ColorGroup& g : alone.colorGroups)
            if (g.id == around->id) g.post.clear();
    track.clips.push_back(clip);
    alone.videoTracks.push_back(track);
    RenderOptions o;
    o.highQuality = true;
    // (Graded scene-referred, the network learns in the working space it runs in.)
    o.sceneLight = sceneReferredWorking(alone);
    out.after = renderSequenceFrame(p, alone, t, o);
    // And without them.
    Clip& bare = alone.videoTracks[0].clips[0];
    bare.effects.clear();
    out.before = renderSequenceFrame(p, alone, t, o);
    if (o.sceneLight) {
        const ColorSpace& working = workingColorSpace(alone);
        convertExactly(out.after, linearColorSpace(working.primaries), working);
        convertExactly(out.before, linearColorSpace(working.primaries), working);
    }
    out.frame = t;
    if (out.before.empty() || out.after.empty() || out.before.width != out.after.width || out.before.height != out.after.height) {
        if (error) *error = "The clip's picture could not be rendered at that frame";
        return false;
    }
    return true;
}

int64_t learningPixelsEach(size_t frames) {
    // (Before and after, float RGBA: 32 bytes a pixel; about a gigabyte in all, never less than a 512-pixel square each.)
    constexpr int64_t kAll = int64_t(32) << 20;
    return std::max<int64_t>(512 * 512, kAll / int64_t(std::max<size_t>(1, frames)));
}

void fitLearningPair(learn::Pair& p, int64_t maxPixels) {
    const int W = p.before.width, H = p.before.height;
    if (int64_t(W) * H <= maxPixels || p.after.width != W || p.after.height != H) return;
    // The window, the frame's shape, at full size (the network learns at the picture's own scale).
    int w = std::clamp(int(std::sqrt(double(maxPixels) * W / H)), 1, W);
    int h = std::clamp(int(maxPixels / w), 1, H);
    // Where before and after differ most, in 32-pixel blocks (summed, so any window's total is four lookups).
    constexpr int B = 32;
    const int gw = (W + B - 1) / B, gh = (H + B - 1) / B;
    std::vector<double> sum(size_t(gw + 1) * size_t(gh + 1), 0.0);
    auto at = [&](int gx, int gy) -> double& { return sum[size_t(gy) * size_t(gw + 1) + size_t(gx)]; };
    for (int by = 0; by < gh; ++by)
        for (int bx = 0; bx < gw; ++bx) {
            double d = 0;
            for (int y = by * B; y < std::min(H, (by + 1) * B); ++y) {
                const float* a = p.before.at(bx * B, y);
                const float* b = p.after.at(bx * B, y);
                for (int i = 0; i < (std::min(W, (bx + 1) * B) - bx * B) * 4; ++i) d += std::fabs(double(b[i]) - double(a[i]));
            }
            at(bx + 1, by + 1) = d + at(bx, by + 1) + at(bx + 1, by) - at(bx, by);
        }
    const int ww = std::max(1, w / B), wh = std::max(1, h / B);
    int bestX = 0, bestY = 0;
    double best = -1;
    for (int by = 0; by + wh <= gh; ++by)
        for (int bx = 0; bx + ww <= gw; ++bx)
            if (const double d = at(bx + ww, by + wh) - at(bx, by + wh) - at(bx + ww, by) + at(bx, by); d > best) best = d, bestX = bx, bestY = by;
    const int x0 = std::min(bestX * B, W - w), y0 = std::min(bestY * B, H - h);
    auto cut = [&](const Image& from) {
        Image out(w, h, Image::Uninitialized{});
        for (int y = 0; y < h; ++y) std::copy_n(from.at(x0, y0 + y), size_t(w) * 4, out.row(y));
        return out;
    };
    p.before = cut(p.before);
    p.after = cut(p.after);
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
    // At most 128, spread evenly over those marked (as many as a gigabyte holds at the smallest each is cut to:
    // learningPixelsEach).
    constexpr size_t kMost = 128;
    const std::vector<FrameTime> all(frames.begin(), frames.end());
    if (all.size() <= kMost) return all;
    std::vector<FrameTime> some;
    for (size_t k = 0; k < kMost; ++k) some.push_back(all[k * (all.size() - 1) / (kMost - 1)]);
    return some;
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

std::map<std::string, std::string> keepLearnedNetworks(Project& p, const std::string& projectPath) {
    std::map<std::string, std::string> moved;
    if (projectPath.empty()) return moved;
    const QString temporary = QFileInfo(QDir::tempPath() + QStringLiteral("/Montage Learned Effects")).absoluteFilePath();
    for (Sequence& s : p.sequences)
        for (Track& t : s.videoTracks)
            for (Clip& c : t.clips)
                for (Effect& e : c.effects) {
                    if (e.type != "learned_effect") continue;
                    const auto it = e.strings.find("network");
                    if (it == e.strings.end() || it->second.empty()) continue;
                    const QFileInfo file(QString::fromStdString(it->second));
                    if (file.absolutePath() != temporary) continue;
                    auto to = moved.find(it->second);
                    if (to == moved.end()) {
                        const std::string path = newLearnedNetworkPath(projectPath, p, c);
                        if (!QFile::copy(file.absoluteFilePath(), QString::fromStdString(path))) continue;
                        to = moved.emplace(it->second, path).first;
                    }
                    it->second = to->second;
                }
    return moved;
}

void repointLearnedNetworks(Project& p, const std::map<std::string, std::string>& moved) {
    for (Sequence& s : p.sequences)
        for (Track& t : s.videoTracks)
            for (Clip& c : t.clips)
                for (Effect& e : c.effects)
                    if (e.type == "learned_effect")
                        if (const auto it = e.strings.find("network"); it != e.strings.end())
                            if (const auto to = moved.find(it->second); to != moved.end()) it->second = to->second;
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
