#include "RenderCache.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QStandardPaths>
#include <map>
#include <memory>

#include "core/Captions.h"
#include "core/ColorGroups.h"
#include "core/EditOps.h"
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
    const QByteArray h = QCryptographicHash::hash(QByteArray::fromStdString(objectMaskToJsonString(*m)), QCryptographicHash::Sha1);
    known[m.get()] = {m, h};
    if (known.size() > 256)  // forget the ones that are gone
        for (auto i = known.begin(); i != known.end();) i = i->second.first.expired() ? known.erase(i) : std::next(i);
    return h;
}

// The clip's settings that show in its pixels (not its id, name, place or label).
void addClip(QCryptographicHash& h, const Clip& c) {
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
}

void addMedia(QCryptographicHash& h, const MediaItem& m, const RenderOptions& o) {
    const std::string& path = o.useProxies && !m.proxyPath.empty() ? m.proxyPath : m.path;
    const QFileInfo fi(QString::fromStdString(path));
    h.addData(QByteArray::fromStdString(path));
    h.addData(QByteArray::number(fi.size()));
    h.addData(QByteArray::number(fi.lastModified().toMSecsSinceEpoch()));
    h.addData(QByteArray::fromStdString(m.colorSpace));
    h.addData(QByteArray::number(m.width) + 'x' + QByteArray::number(m.height));
}

bool addFrame(QCryptographicHash& h, const Project& p, const Sequence& seq, FrameTime t, const RenderOptions& o, int depth);

// One clip's part of the frame: its settings, how far into it t is, and what it shows.
void addClipAt(QCryptographicHash& h, const Project& p, const Sequence& seq, const Clip& c, FrameTime t, const RenderOptions& o,
               int depth) {
    addClip(h, c);
    // Its colour group's grades run on it too.
    if (const ColorGroup* g = colorGroupOf(seq, c))
        for (const auto* chain : {&g->pre, &g->post}) {
            h.addData(chain == &g->pre ? QByteArray("|pre") : QByteArray("|post"));
            for (Effect e : *chain) {
                e.id = 0;
                h.addData(QByteArray::fromStdString(effectToJsonString(e)));
            }
        }
    h.addData(QByteArray::number(qint64(t - c.start)));
    if (const MediaItem* m = c.mediaId ? p.findMedia(c.mediaId) : nullptr) {
        if (m->kind == MediaKind::Sequence) {
            // A nested sequence: its own frame, at the time the clip shows.
            if (const Sequence* nested = p.findSequence(m->sequenceId); nested && depth < 8 && nested->id != seq.id) {
                const FrameTime nf = FrameTime(std::floor(c.sourceFrameAt(t) * nested->fpsValue() / seq.fpsValue() + 1e-6));
                RenderOptions no = o;
                no.captions = false;
                if (nested->multicam) no.soloVideoTrack = c.angle;
                addFrame(h, p, *nested, nf, no, depth + 1);
            }
        } else {
            addMedia(h, *m, o);
        }
    }
}

bool addFrame(QCryptographicHash& h, const Project& p, const Sequence& seq, FrameTime t, const RenderOptions& o, int depth) {
    h.addData(QByteArray::number(seq.width) + 'x' + QByteArray::number(seq.height) + '@' + QByteArray::number(seq.fpsValue(), 'g', 10));
    h.addData(QByteArray::fromStdString(seq.colorSpace) + QByteArray::number(seq.hdrPeakNits));
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

QByteArray frameKey(const Project& p, const Sequence& seq, FrameTime t, const RenderOptions& o) {
    QCryptographicHash h(QCryptographicHash::Sha1);
    h.addData(QByteArray("montage-frame-") + QByteArray::number(kCacheVersion));
    h.addData(QByteArray::number(o.scale, 'g', 10) + (o.useProxies ? "P" : "") + (o.highQuality ? "H" : "") + '#' +
              QByteArray::fromStdString(o.displaySpace) + '#' + QByteArray::number(o.soloVideoTrack) +
              (seq.stereo3d ? QByteArray("#3D") + QByteArray::number(int(o.stereoView)) : QByteArray()));
    bool any = addFrame(h, p, seq, t, o, 0);
    if (o.captions)
        if (const CaptionTrack* track = captionTrackFor(seq)) {
            if (const Caption* cue = captionAt(*track, t)) {
                CaptionTrack style = *track;
                style.captions.clear();
                style.id = 0;
                h.addData(QByteArray::fromStdString(captionTrackToJsonString(style)));
                h.addData(QByteArray::fromStdString(cue->text) + QByteArray::number(qint64(cue->start)) + '-' + QByteArray::number(qint64(cue->end)));
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
            if (c.enabled && (!c.effects.empty() || (c.isGenerator() && c.generator.type != "color" && c.generator.type != "adjustment")))
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
            const Image img = renderProgramFrame(p, seq, t, o);
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
