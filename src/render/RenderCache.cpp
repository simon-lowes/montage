#include "RenderCache.h"
#include "TrimPasses.h"
#include "core/GradeGraph.h"
#include "media/LipSync.h"
#include "media/MotionDeblur.h"
#include "media/Rife.h"
#include "media/Proxy.h"
#include "media/SuperScale.h"
#include "media/VideoDepth.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QStandardPaths>
#include <algorithm>
#include <map>
#include <memory>

#include "core/Captions.h"
#include "core/Effects.h"
#include "core/ColorGroups.h"
#include "core/EditOps.h"
#include "core/Expression.h"
#include "core/GraphicsTemplate.h"
#include "core/ProjectIO.h"

namespace montage {

namespace {

// Bump when rendering changes in a way that should make old frames stale.
constexpr int kCacheVersion = 1;

// Object masks are large and never change once made (edits make new ones), so
// each is hashed once while it lives.
QByteArray objectHash(const std::shared_ptr<const ObjectMask>& m) {
    static QMutex lock;
    static std::map<const ObjectMask*, std::pair<std::weak_ptr<const ObjectMask>, QByteArray>> known;
    QMutexLocker l(&lock);
    auto it = known.find(m.get());
    if (it != known.end() && !it->second.first.expired() && it->second.first.lock() == m) return it->second.second;
    // (Names don't show in the picture: renaming an object keeps its frames.)
    ObjectMask unnamed = *m;
    unnamed.name.clear();
    for (ObjectMask& x : unnamed.more) x.name.clear();
    const QByteArray h = QCryptographicHash::hash(QByteArray::fromStdString(objectMaskToJsonString(unnamed)), QCryptographicHash::Sha1);
    known[m.get()] = {m, h};
    if (known.size() > 256)  // forget the ones that are gone
        for (auto i = known.begin(); i != known.end();) i = i->second.first.expired() ? known.erase(i) : std::next(i);
    return h;
}

// The clip's settings that show in its pixels (not its id, name, place or label).
void addClip(QCryptographicHash& h, const Sequence& seq, const Clip& c) {
    Clip k = c;
    k.id = 0;
    k.start = 0;
    k.name.clear();
    k.linkGroup = 0;
    k.colorLabel = 0;
    k.audio = Effect();
    k.audioAngle = -1;
    std::vector<QByteArray> objects;
    for (Effect* e : {&k.generator, &k.motion, &k.timing}) e->id = 0;
    for (Effect& e : k.effects) {
        e.id = 0;
        if (e.object) {
            objects.push_back(objectHash(e.object));
            e.object.reset();
        }
    }
    h.addData(QByteArray::fromStdString(clipToJsonString(k)));
    for (const QByteArray& o : objects) h.addData(o);
    // Effects whose model is downloaded later change the picture then (shared nodes' too).
    for (const Effect* e : liveEffects(seq, c))
        if (e->type == "motion_deblur") h.addData(deblurModel().installed() ? "deblur:1" : "deblur:0");
        else if (e->type == "super_scale") h.addData(upscaleModel().installed() ? "upscale:1" : "upscale:0");
        else if (e->type == "frame_repair") h.addData(rifeAvailable() && rifeModel().installed() ? "rife:1" : "rife:0");
        else if (e->type == "learned_effect") {
            // (Trained further, the network file changes, and so does the picture.)
            const QFileInfo fi(QString::fromStdString(e->s("network")));
            h.addData(QByteArray::number(fi.size()) + ':' + QByteArray::number(fi.lastModified().toMSecsSinceEpoch()));
        }
}

void addMedia(QCryptographicHash& h, const MediaItem& m, const RenderOptions& o) {
    // The file the picture is read from (a proxy attached, made again, or standing in for an offline file changes it).
    const std::string path = pictureSource(m, o.useProxies, o.offlineProxies);
    const QFileInfo fi(QString::fromStdString(path));
    h.addData(QByteArray::fromStdString(path));
    h.addData(QByteArray::number(fi.size()));
    h.addData(QByteArray::number(fi.lastModified().toMSecsSinceEpoch()));
    h.addData(QByteArray::fromStdString(m.colorSpace));
    if (!m.colorOverride.empty()) h.addData(QByteArray("|as ") + QByteArray::fromStdString(m.colorOverride));  // (Interpret Colour)
    h.addData(QByteArray::number(m.width) + 'x' + QByteArray::number(m.height));
}

bool addFrame(QCryptographicHash& h, const Project& p, const Sequence& seq, FrameTime t, const RenderOptions& o, int depth);

// One clip's part of the frame: its settings, how far into it t is, and what it shows.
void addClipAt(QCryptographicHash& h, const Project& p, const Sequence& seq, const Clip& c, FrameTime t, const RenderOptions& o,
               int depth) {
    addClip(h, seq, c);
    // Lip Sync's picture follows the sound its track plays over the 30 s stretches (from the clip's start) its window
    // draws on: each clip there, where it is against the clip, and which tracks are heard.
    for (const Effect* e : liveEffects(seq, c))
        if (e->type == "lip_sync" && e->enabled) {
            h.addData(lipSyncModel().installed() ? "lipsync:1" : "lipsync:0");
            const int track = lipSyncTrack(seq, e->s("track"));
            h.addData(QByteArray::number(track));
            if (track < 0) continue;
            int64_t first = 0, last = 0;
            lipSyncSegments(t - c.start, std::max(1.0, seq.fpsValue()), first, last);
            const double span = double(kLipSyncSegment) / kLipSyncRate * seq.fpsValue();
            const double from = double(c.start) + double(first) * span, to = double(c.start) + double(last + 1) * span;
            h.addData(QByteArray::number(first) + ':' + QByteArray::number(last));
            for (size_t k = 0; k < seq.audioTracks.size(); ++k) {
                const Track& at = seq.audioTracks[k];
                if ((track > 0 && int(k) != track - 1) || (track == 0 && at.muted)) continue;
                h.addData("|track" + QByteArray::number(qint64(k)));
                for (const Clip& a : at.clips) {
                    if (double(a.end()) <= from || double(a.start) >= to) continue;
                    Clip k2 = a;
                    k2.id = 0;
                    k2.linkGroup = 0;
                    k2.start -= c.start;
                    h.addData(QByteArray::fromStdString(clipToJsonString(k2)));
                    if (const MediaItem* m = a.mediaId ? p.findMedia(a.mediaId) : nullptr) addMedia(h, *m, o);
                }
            }
        }
    // Its colour group's grades run on it too.
    if (const ColorGroup* g = colorGroupOf(seq, c))
        for (const auto* chain : {&g->pre, &g->post}) {
            h.addData(chain == &g->pre ? QByteArray("|pre") : QByteArray("|post"));
            for (Effect e : *chain) {
                e.id = 0;
                h.addData(QByteArray::fromStdString(effectToJsonString(e)));
            }
        }
    // And the shared grade nodes its graph uses (the graph itself is in the clip's settings).
    for (const GradeNode& n : c.grade.nodes)
        if (const SharedNode* sn = findSharedNode(seq, n.shared)) {
            h.addData(QByteArray("|shared"));
            for (Effect e : sn->effects) {
                e.id = 0;
                h.addData(QByteArray::fromStdString(effectToJsonString(e)));
            }
        }
    h.addData(QByteArray::number(qint64(t - c.start)));
    if (const MediaItem* m = c.mediaId ? p.findMedia(c.mediaId) : nullptr) {
        if (m->kind == MediaKind::Sequence) {
            // A nested sequence: its own frame, at the time the clip shows.
            if (const Sequence* nested = p.findSequence(m->sequenceId); nested && depth < 8 && nested->id != seq.id) {
                Sequence filled;  // a graphic's own values (its media swapped in too)
                if (!nested->fields.empty() && !c.fields.empty()) {
                    filled = withFieldValues(p, *nested, c.fields);
                    nested = &filled;
                }
                const FrameTime nf = FrameTime(std::floor(c.sourceFrameAt(t) * nested->fpsValue() / seq.fpsValue() + 1e-6));
                RenderOptions no = o;
                no.captions = false;
                if (nested->multicam) no.soloVideoTrack = c.angle;
                addFrame(h, p, *nested, nf, no, depth + 1);
            }
        } else {
            addMedia(h, *m, o);
            // Depth effects take the file's depth analysis where there is one: analysed (again), the frame changes.
            const auto chain = liveEffects(seq, c);
            if (m->kind == MediaKind::Video && std::any_of(chain.begin(), chain.end(), [&](const Effect* e) {
                    return needsDepth(*e, t - c.start) || (e->enabled && e->type == "sky_replace" && e->p("use_depth", t - c.start, 1) > 0.5);
                }))
                h.addData(QByteArray("|depth") + QByteArray::fromStdString(analysedDepthStamp(m->path)));
        }
    }
}

bool addFrame(QCryptographicHash& h, const Project& p, const Sequence& seq, FrameTime t, const RenderOptions& o, int depth) {
    h.addData(QByteArray::number(seq.width) + 'x' + QByteArray::number(seq.height) + '@' + QByteArray::number(seq.fpsValue(), 'g', 10));
    h.addData(QByteArray::fromStdString(seq.colorSpace) + QByteArray::number(seq.hdrPeakNits));
    if (!seq.workingSpace.empty()) h.addData(QByteArray::fromStdString("|working " + seq.workingSpace + ' ' + seq.outputTransform));
    if (seq.stereo3d) h.addData("|stereo");  // clips placed in depth (a nested sequence's too)
    bool any = false;
    for (size_t ti = 0; ti < seq.videoTracks.size(); ++ti) {
        const Track& track = seq.videoTracks[ti];
        if (o.soloVideoTrack >= 0 ? int(ti) != o.soloVideoTrack : track.muted) continue;
        h.addData(QByteArray("|track") + QByteArray::number(qint64(ti)));
        // A transition here decides the frame from both its clips.
        bool inTransition = false;
        for (const Transition& tr : track.transitions) {
            FrameTime a, b;
            if (!edit::transitionRange(track, tr, a, b) || t < a || t >= b) continue;
            Transition k = tr;
            k.id = k.clipA = k.clipB = 0;
            k.params.id = 0;
            h.addData(QByteArray::fromStdString(k.type) + QByteArray::number(qint64(t - a)) + '/' + QByteArray::number(qint64(b - a)));
            for (const auto& [name, prm] : k.params.params) h.addData(QByteArray::fromStdString(name) + QByteArray::number(prm.at(0), 'g', 12));
            for (Id id : {tr.clipA, tr.clipB})
                for (const Clip& c : track.clips)
                    if (c.id == id) addClipAt(h, p, seq, c, t, o, depth);
            // A Smooth Cut morphs the transition's first and last frames.
            if (tr.type == "smooth_cut") h.addData(QByteArray::number(qint64(a)) + '-' + QByteArray::number(qint64(b)));
            inTransition = any = true;
            break;
        }
        if (inTransition) continue;
        if (const Clip* c = edit::clipAt(seq, TrackRef{TrackKind::Video, int(ti)}, t); c && c->enabled) {
            addClipAt(h, p, seq, *c, t, o, depth);
            any = true;
        }
    }
    return any;
}

}  // namespace

QByteArray clipLayerKey(const Project& p, const Sequence& seq, const Clip& c, FrameTime t, const RenderOptions& o) {
    QCryptographicHash h(QCryptographicHash::Sha1);
    h.addData(QByteArray("montage-layer-") + QByteArray::number(kCacheVersion));
    h.addData(QByteArray::number(o.scale, 'g', 10) + (o.useProxies ? "P" : "") + (o.highQuality ? "H" : "") + '#' +
              QByteArray::fromStdString(o.displaySpace) + (seq.stereo3d ? QByteArray("#3D") + QByteArray::number(int(o.stereoView)) : QByteArray()));
    if (!o.outputSpace.empty() || o.sceneLight)
        h.addData(QByteArray::fromStdString("#out|" + o.outputSpace + '|') + QByteArray::number(o.outputPeak) + (o.sceneLight ? "|light" : ""));
    h.addData(QByteArray::number(seq.width) + 'x' + QByteArray::number(seq.height) + '@' + QByteArray::number(seq.fpsValue(), 'g', 10));
    h.addData(QByteArray::fromStdString(seq.colorSpace) + QByteArray::number(seq.hdrPeakNits));
    if (!seq.workingSpace.empty()) h.addData(QByteArray::fromStdString("|working " + seq.workingSpace + ' ' + seq.outputTransform));
    addClipAt(h, p, seq, c, t, o, 0);
    return h.result().toHex();
}

QByteArray frameKey(const Project& p, const Sequence& seq, FrameTime t, const RenderOptions& o) {
    if (usesExpressions(p)) return {};  // (not cached: what they give can depend on what the key does not cover)
    QCryptographicHash h(QCryptographicHash::Sha1);
    h.addData(QByteArray("montage-frame-") + QByteArray::number(kCacheVersion));
    h.addData(QByteArray::number(o.scale, 'g', 10) + (o.useProxies ? "P" : "") + (o.highQuality ? "H" : "") + '#' +
              QByteArray::fromStdString(o.displaySpace) + '#' + QByteArray::number(o.soloVideoTrack) +
              (seq.stereo3d ? QByteArray("#3D") + QByteArray::number(int(o.stereoView)) : QByteArray()));
    if (!o.outputSpace.empty() || o.sceneLight)
        h.addData(QByteArray::fromStdString("#out|" + o.outputSpace + '|') + QByteArray::number(o.outputPeak) + (o.sceneLight ? "|light" : ""));
    if (const TrimTarget* trim = o.trimTarget.empty() ? nullptr : findTrimTarget(seq, o.trimTarget))
        h.addData(QByteArray::fromStdString("#trim|" + trim->id + "|" + trim->colorSpace + "|") + QByteArray::number(trim->peakNits));
    bool any = addFrame(h, p, seq, t, o, 0);
    if (o.captions)
        if (const CaptionTrack* track = captionTrackFor(seq)) {
            if (const Caption* cue = captionAt(*track, t)) {
                // The track's style and the whole cue (its text, times, place, italics, word times and colour).
                CaptionTrack shown = *track;
                shown.captions = {*cue};
                shown.id = 0;
                h.addData(QByteArray::fromStdString(captionTrackToJsonString(shown)));
                any = true;
            }
        }
    return any ? h.result().toHex() : QByteArray();
}

// ---------------------------------------------------------------------------

RenderCache::RenderCache(const QString& dir) : dir_(dir) {
    QDir().mkpath(dir_);
    QDirIterator it(dir_, {QStringLiteral("*.jpg")}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) keys_.insert(QFileInfo(it.next()).completeBaseName().toLatin1());
}

RenderCache& RenderCache::instance() {
    static RenderCache cache(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/render"));
    return cache;
}

QString RenderCache::path(const QByteArray& key) const {
    return dir_ + '/' + QString::fromLatin1(key.left(2)) + '/' + QString::fromLatin1(key) + QStringLiteral(".jpg");
}

bool RenderCache::has(const QByteArray& key) const {
    QMutexLocker l(&m_);
    return !key.isEmpty() && keys_.contains(key);
}

QImage RenderCache::load(const QByteArray& key) const {
    if (!has(key)) return {};
    QImage img(path(key));
    return img.isNull() ? QImage() : img.convertToFormat(QImage::Format_RGBA8888);
}

bool RenderCache::store(const QByteArray& key, const QImage& frame) {
    if (key.isEmpty() || frame.isNull()) return false;
    const QString file = path(key);
    QDir().mkpath(QFileInfo(file).absolutePath());
    // Written aside and renamed, so a frame is never seen half written.
    const QString part = file + QStringLiteral(".part");
    if (!frame.convertToFormat(QImage::Format_RGB888).save(part, "JPG", 92)) return false;
    QFile::remove(file);
    if (!QFile::rename(part, file)) return false;
    QMutexLocker l(&m_);
    keys_.insert(key);
    return true;
}

void RenderCache::clear() {
    QMutexLocker l(&m_);
    QDir(dir_).removeRecursively();
    QDir().mkpath(dir_);
    keys_.clear();
}

int RenderCache::count() const {
    QMutexLocker l(&m_);
    return int(keys_.size());
}

qint64 RenderCache::bytes() const {
    qint64 total = 0;
    QDirIterator it(dir_, {QStringLiteral("*.jpg")}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) total += QFileInfo(it.next()).size();
    return total;
}

std::vector<std::pair<FrameTime, FrameTime>> rangesToRender(const Sequence& seq) {
    std::vector<std::pair<FrameTime, FrameTime>> spans;
    for (const Track& t : seq.videoTracks) {
        if (t.muted) continue;
        for (const Clip& c : t.clips)
            if (c.enabled && (!c.effects.empty() || !c.grade.empty() || (c.isGenerator() && c.generator.type != "color" && c.generator.type != "adjustment")))
                spans.push_back({c.start, c.end()});
        for (const Transition& tr : t.transitions) {
            FrameTime a = 0, b = 0;
            if (edit::transitionRange(t, tr, a, b)) spans.push_back({a, b});
        }
    }
    std::sort(spans.begin(), spans.end());
    std::vector<std::pair<FrameTime, FrameTime>> merged;
    for (const auto& r : spans) {
        if (r.second <= r.first) continue;
        if (!merged.empty() && r.first <= merged.back().second) merged.back().second = std::max(merged.back().second, r.second);
        else merged.push_back(r);
    }
    return merged;
}

int renderToCache(const Project& p, const Sequence& seq, FrameTime from, FrameTime to, const RenderOptions& o, RenderCache& cache,
                  const RenderProgress& progress, const std::atomic<bool>* cancel) {
    int rendered = 0;
    const FrameTime n = std::max<FrameTime>(1, to - from + 1);
    for (FrameTime t = from; t <= to; ++t) {
        if (cancel && cancel->load()) return -1;
        const QByteArray key = frameKey(p, seq, t, o);
        if (!key.isEmpty() && !cache.has(key)) {
            // Rendered ahead, so effects that take seconds a frame run; a frame they could not finish is not kept.
            RenderIssues issues;
            RenderOptions ahead = o;
            ahead.slowEffects = true;
            ahead.issues = &issues;
            ahead.cancel = cancel;
            const Image img = renderProgramFrame(p, seq, t, ahead);
            if (issues.incomplete) {
                if (cancel && cancel->load()) return -1;
                if (progress) progress(double(t - from + 1) / double(n));
                continue;
            }
            QImage q(img.width, img.height, QImage::Format_RGBA8888);
            toRgba8(img, q.bits(), size_t(q.bytesPerLine()));
            if (cache.store(key, q)) ++rendered;
        }
        if (progress) progress(double(t - from + 1) / double(n));
    }
    return rendered;
}

std::vector<std::pair<FrameTime, FrameTime>> cachedRanges(const Project& p, const Sequence& seq, FrameTime from, FrameTime to,
                                                          const RenderOptions& o, const RenderCache& cache) {
    std::vector<std::pair<FrameTime, FrameTime>> out;
    for (FrameTime t = from; t < to; ++t) {
        const QByteArray key = frameKey(p, seq, t, o);
        if (key.isEmpty() || !cache.has(key)) continue;
        if (!out.empty() && out.back().second == t) out.back().second = t + 1;
        else out.emplace_back(t, t + 1);
    }
    return out;
}

}  // namespace montage
