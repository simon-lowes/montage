// Montage — the render cache (Premiere's Render In to Out, Final Cut's
// background render, Resolve's render cache): rendered preview frames kept on
// disk, found again by a key made from everything that decides the frame, so
// heavy effects play back in real time and stay rendered until something
// that shows in them changes.
#pragma once

#include <QByteArray>
#include <QImage>
#include <QMutex>
#include <QSet>
#include <QString>
#include <atomic>
#include <functional>
#include <utility>
#include <vector>

#include "Compositor.h"

namespace montage {

// A fingerprint of frame t of `seq` as renderProgramFrame draws it with `o`:
// the sequence's settings, every clip on screen at t (its settings and how far
// into it t is, not where it sits, so moving a clip keeps its frames), the
// transitions there, the media files (path, size, date), nested sequences and
// the visible caption. Empty when nothing is on screen.
QByteArray frameKey(const Project& p, const Sequence& seq, FrameTime t, const RenderOptions& o);

class RenderCache {
public:
    // Frames under `dir` (made if missing).
    explicit RenderCache(const QString& dir);
    // The application's cache (its cache folder's "render" folder).
    static RenderCache& instance();

    QString dir() const { return dir_; }
    bool has(const QByteArray& key) const;
    QImage load(const QByteArray& key) const;
    bool store(const QByteArray& key, const QImage& frame);
    void clear();
    int count() const;
    qint64 bytes() const;

private:
    QString path(const QByteArray& key) const;
    QString dir_;
    mutable QMutex m_;
    QSet<QByteArray> keys_;
};

using RenderProgress = std::function<void(double fraction)>;
// Renders frames [from, to] of `seq` that are not cached yet into `cache`;
// returns how many were rendered (-1 if cancelled).
int renderToCache(const Project& p, const Sequence& seq, FrameTime from, FrameTime to, const RenderOptions& o,
                  RenderCache& cache, const RenderProgress& progress = {}, const std::atomic<bool>* cancel = nullptr);
// The stretches of the sequence worth rendering ahead (Final Cut's background render): frames under a video clip
// with effects or a generated picture (titles, shapes), or inside a transition, as merged [first, end) ranges.
std::vector<std::pair<FrameTime, FrameTime>> rangesToRender(const Sequence& seq);
// The cached stretches of frames [from, to) as [first, end) ranges, for the render bar.
std::vector<std::pair<FrameTime, FrameTime>> cachedRanges(const Project& p, const Sequence& seq, FrameTime from, FrameTime to,
                                                          const RenderOptions& o, const RenderCache& cache);

}  // namespace montage
