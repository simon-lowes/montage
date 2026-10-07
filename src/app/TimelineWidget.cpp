#include "TimelineWidget.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QFileInfo>
#include <QInputDialog>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QToolTip>
#include <QUrl>
#include <cmath>
#include <set>

#include "EditorState.h"
#include "Theme.h"
#include "ThumbnailCache.h"
#include "core/Effects.h"
#include "media/MediaPool.h"

namespace montage {

namespace {

constexpr int kHeaderW = 176;
constexpr int kRulerH = 30;
constexpr int kDividerH = 8;
constexpr int kVideoH = 62;
constexpr int kAudioH = 52;
constexpr int kEdgeGrab = 6;
constexpr int kSnapPx = 9;
constexpr int kNameStrip = 16;
constexpr int kBtn = 20;

int trackHeight(const Track& t) { return t.height > 0 ? t.height : (t.kind == TrackKind::Video ? kVideoH : kAudioH); }

QColor clipColor(const Project& p, const Clip& c, TrackKind kind) {
    if (!c.enabled) return theme::kDisabledClip;
    QColor label = theme::labelColor(c.colorLabel);
    if (label.isValid()) return label;
    if (c.isGenerator()) return theme::kGeneratorClip;
    if (const MediaItem* m = p.findMedia(c.mediaId))
        if (m->kind == MediaKind::Sequence) return theme::kCompoundClip;
    return kind == TrackKind::Video ? theme::kVideoClip : theme::kAudioClip;
}

// Parses "id[:in:out],id..." from the media drag payload.
struct MediaRef {
    Id id = 0;
    FrameTime in = -1, out = -1;
};
std::vector<MediaRef> parseMediaMime(const QMimeData* mime) {
    std::vector<MediaRef> out;
    if (!mime->hasFormat("application/x-montage-media")) return out;
    for (const QString& part : QString::fromUtf8(mime->data("application/x-montage-media")).split(',', Qt::SkipEmptyParts)) {
        QStringList f = part.split(':');
        MediaRef r;
        r.id = f.value(0).toULongLong();
        if (f.size() >= 3) {
            r.in = f[1].toLongLong();
            r.out = f[2].toLongLong();
        }
        if (r.id) out.push_back(r);
    }
    return out;
}

}  // namespace

TimelineWidget::TimelineWidget(EditorState* state, QWidget* parent) : QAbstractScrollArea(parent), state_(state) {
    setAcceptDrops(true);
    viewport()->setAcceptDrops(true);
    viewport()->setMouseTracking(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);
    horizontalScrollBar()->setSingleStep(20);
    verticalScrollBar()->setSingleStep(20);
    connect(state_, &EditorState::projectChanged, this, [this] {
        updateScrollBars();
        viewport()->update();
    });
    connect(state_, &EditorState::selectionChanged, viewport(), qOverload<>(&QWidget::update));
    connect(state_, &EditorState::playheadChanged, viewport(), qOverload<>(&QWidget::update));
    connect(state_, &EditorState::sequenceSwitched, this, [this] {
        horizontalScrollBar()->setValue(0);
        zoomToFit();
    });
    connect(state_, &EditorState::mediaReady, viewport(), qOverload<>(&QWidget::update));
    connect(&ThumbnailCache::instance(), &ThumbnailCache::ready, viewport(), qOverload<>(&QWidget::update));
    updateScrollBars();
}

void TimelineWidget::setTool(Tool t) {
    if (tool_ == t) return;
    tool_ = t;
    switch (t) {
        case Tool::Razor: viewport()->setCursor(Qt::CrossCursor); break;
        case Tool::Hand: viewport()->setCursor(Qt::OpenHandCursor); break;
        case Tool::Slip:
        case Tool::Slide: viewport()->setCursor(Qt::SizeHorCursor); break;
        default: viewport()->unsetCursor(); break;
    }
    emit toolChanged(t);
}

// ---------------------------------------------------------------------------
// Geometry

std::vector<TimelineWidget::Row> TimelineWidget::rows() const {
    std::vector<Row> out;
    const Sequence* s = state_->sequence();
    if (!s) return out;
    int y = kRulerH - verticalScrollBar()->value();
    for (int i = int(s->videoTracks.size()) - 1; i >= 0; --i) {
        int h = trackHeight(s->videoTracks[size_t(i)]);
        out.push_back({{TrackKind::Video, i}, y, h});
        y += h;
    }
    y += kDividerH;
    for (int i = 0; i < int(s->audioTracks.size()); ++i) {
        int h = trackHeight(s->audioTracks[size_t(i)]);
        out.push_back({{TrackKind::Audio, i}, y, h});
        y += h;
    }
    return out;
}

int TimelineWidget::contentHeight() const {
    const Sequence* s = state_->sequence();
    if (!s) return 0;
    int h = kDividerH + 40;
    for (const auto& t : s->videoTracks) h += trackHeight(t);
    for (const auto& t : s->audioTracks) h += trackHeight(t);
    return h;
}

int TimelineWidget::dividerY() const {
    const Sequence* s = state_->sequence();
    int y = kRulerH - verticalScrollBar()->value();
    if (s)
        for (const auto& t : s->videoTracks) y += trackHeight(t);
    return y;
}

std::optional<TimelineWidget::Row> TimelineWidget::rowAt(int y) const {
    for (const Row& r : rows())
        if (y >= r.y && y < r.y + r.h) return r;
    return std::nullopt;
}

int TimelineWidget::xForFrame(FrameTime f) const {
    return kHeaderW + int(std::floor(double(f) * ppf_)) - horizontalScrollBar()->value();
}

double TimelineWidget::frameAtX(int x) const { return (double(x - kHeaderW + horizontalScrollBar()->value())) / ppf_; }

FrameTime TimelineWidget::frameRound(int x) const { return std::max<FrameTime>(0, FrameTime(std::llround(frameAtX(x)))); }

void TimelineWidget::updateScrollBars() {
    const Sequence* s = state_->sequence();
    FrameTime dur = s ? s->duration() : 0;
    double fps = s ? s->fpsValue() : 30;
    int vw = viewport()->width() - kHeaderW;
    int content = int((double(dur) + fps * 30) * ppf_);
    horizontalScrollBar()->setPageStep(std::max(1, vw));
    horizontalScrollBar()->setRange(0, std::max(0, content - vw / 2));
    int vh = viewport()->height() - kRulerH;
    verticalScrollBar()->setPageStep(std::max(1, vh));
    verticalScrollBar()->setRange(0, std::max(0, contentHeight() - vh));
}

void TimelineWidget::resizeEvent(QResizeEvent* e) {
    QAbstractScrollArea::resizeEvent(e);
    updateScrollBars();
}

void TimelineWidget::scrollContentsBy(int, int) { viewport()->update(); }

void TimelineWidget::zoomAround(int x, double factor) {
    double f = frameAtX(x);
    ppf_ = std::clamp(ppf_ * factor, 0.005, 80.0);
    updateScrollBars();
    horizontalScrollBar()->setValue(int(std::lround(f * ppf_ - (x - kHeaderW))));
    viewport()->update();
}

void TimelineWidget::zoomIn() { zoomAround(xForFrame(state_->playhead()), 1.5); }
void TimelineWidget::zoomOut() { zoomAround(xForFrame(state_->playhead()), 1 / 1.5); }

void TimelineWidget::zoomToFit() {
    const Sequence* s = state_->sequence();
    FrameTime dur = s ? std::max<FrameTime>(s->duration(), FrameTime(s->fpsValue() * 10)) : 300;
    int vw = std::max(100, viewport()->width() - kHeaderW - 30);
    ppf_ = std::clamp(double(vw) / double(dur), 0.005, 80.0);
    updateScrollBars();
    horizontalScrollBar()->setValue(0);
    viewport()->update();
}

void TimelineWidget::followPlayhead(FrameTime t) {
    int x = xForFrame(t);
    int right = viewport()->width() - 30;
    if (x > right || x < kHeaderW) horizontalScrollBar()->setValue(int(double(t) * ppf_) - (viewport()->width() - kHeaderW) / 10);
}

// ---------------------------------------------------------------------------
// Hit testing

QRect TimelineWidget::headerButtonRect(const Row& r, HeaderButton b) const {
    int y = r.y + std::min(6, r.h / 2 - kBtn / 2);
    switch (b) {
        case HeaderButton::Target: return {6, y, 30, kBtn};
        case HeaderButton::Lock: return {kHeaderW - 6 - kBtn, y, kBtn, kBtn};
        case HeaderButton::Visible:
        case HeaderButton::Solo: return {kHeaderW - 8 - 2 * kBtn, y, kBtn, kBtn};
        case HeaderButton::Mute: return {kHeaderW - 10 - 3 * kBtn, y, kBtn, kBtn};
        case HeaderButton::Name: return {40, y, kHeaderW - 50 - 3 * kBtn, kBtn};
        default: return {};
    }
}

TimelineWidget::HeaderButton TimelineWidget::headerButtonAt(const Row& r, const QPoint& pos) const {
    bool video = r.ref.kind == TrackKind::Video;
    for (HeaderButton b : {HeaderButton::Target, HeaderButton::Lock, HeaderButton::Name}) {
        if (headerButtonRect(r, b).contains(pos)) return b;
    }
    if (video && headerButtonRect(r, HeaderButton::Visible).contains(pos)) return HeaderButton::Visible;
    if (!video && headerButtonRect(r, HeaderButton::Solo).contains(pos)) return HeaderButton::Solo;
    if (!video && headerButtonRect(r, HeaderButton::Mute).contains(pos)) return HeaderButton::Mute;
    return HeaderButton::None;
}

TimelineWidget::Hit TimelineWidget::hitTest(const QPoint& pos) const {
    Hit h;
    h.frame = frameRound(pos.x());
    const Sequence* s = state_->sequence();
    if (!s) return h;
    if (pos.y() < kRulerH) {
        h.kind = pos.x() >= kHeaderW ? HitKind::Ruler : HitKind::None;
        return h;
    }
    auto row = rowAt(pos.y());
    if (!row) {
        h.kind = pos.x() >= kHeaderW ? HitKind::Body : HitKind::None;
        return h;
    }
    h.track = row->ref;
    if (pos.x() < kHeaderW) {
        h.kind = HitKind::Header;
        h.button = headerButtonAt(*row, pos);
        return h;
    }
    h.kind = HitKind::Body;
    const Track* t = trackAt(*s, row->ref);
    // Edges first (they sit on top of transitions), nearest edge wins.
    int bestDist = kEdgeGrab + 1;
    for (const Clip& c : t->clips) {
        int xs = xForFrame(c.start), xe = xForFrame(c.end());
        if (xe < pos.x() - kEdgeGrab - 2 || xs > pos.x() + kEdgeGrab + 2) continue;
        bool narrow = xe - xs < 3 * kEdgeGrab;
        int dIn = std::abs(pos.x() - xs), dOut = std::abs(pos.x() - xe);
        if (!narrow || pos.x() < xs + 3 || pos.x() > xe - 3) {
            if (dIn <= kEdgeGrab && dIn < bestDist && pos.x() >= xs - 1) {
                bestDist = dIn;
                h.kind = HitKind::ClipIn;
                h.clip = c.id;
            }
            if (dOut <= kEdgeGrab && dOut < bestDist && pos.x() <= xe + 1) {
                bestDist = dOut;
                h.kind = HitKind::ClipOut;
                h.clip = c.id;
            }
        }
    }
    if (h.clip) return h;
    for (const auto& tr : t->transitions) {
        FrameTime a, b;
        if (!edit::transitionRange(*t, tr, a, b)) continue;
        int xa = xForFrame(a), xb = xForFrame(b);
        if (pos.x() >= xa && pos.x() <= xb && pos.y() >= row->y + row->h / 2) {
            h.kind = HitKind::Transition;
            h.transition = tr.id;
            return h;
        }
    }
    for (const Clip& c : t->clips) {
        if (pos.x() >= xForFrame(c.start) && pos.x() < xForFrame(c.end())) {
            h.kind = HitKind::ClipBody;
            h.clip = c.id;
            return h;
        }
    }
    return h;
}

// ---------------------------------------------------------------------------
// Snapping

FrameTime TimelineWidget::snapFrame(FrameTime f, const std::vector<Id>& exclude, bool* snapped) {
    if (snapped) *snapped = false;
    const Sequence* s = state_->gestureBase();
    if (!s || !state_->snapping()) return f;
    auto pts = edit::snapPoints(*s, exclude);
    FrameTime tol = std::max<FrameTime>(1, FrameTime(kSnapPx / ppf_));
    bool hit = false;
    FrameTime r = edit::snap(pts, f, tol, &hit);
    if (snapped) *snapped = hit;
    snapIndicator_ = hit ? r : -1;
    return r;
}

FrameTime TimelineWidget::snapDelta(const std::vector<Id>& ids, FrameTime delta) {
    snapIndicator_ = -1;
    const Sequence* s = state_->gestureBase();
    if (!s || !state_->snapping()) return delta;
    auto pts = edit::snapPoints(*s, ids);
    FrameTime tol = std::max<FrameTime>(1, FrameTime(kSnapPx / ppf_));
    FrameTime best = delta, bestDist = tol + 1;
    for (Id id : ids) {
        const Clip* c = edit::clipById(*s, id);
        if (!c) continue;
        for (FrameTime edge : {c->start + delta, c->end() + delta}) {
            bool hit = false;
            FrameTime sn = edit::snap(pts, edge, tol, &hit);
            if (hit && std::llabs(sn - edge) < bestDist) {
                bestDist = std::llabs(sn - edge);
                best = delta + (sn - edge);
                snapIndicator_ = sn;
            }
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Painting

void TimelineWidget::paintEvent(QPaintEvent*) {
    QPainter p(viewport());
    const Sequence* s = state_->sequence();
    p.fillRect(viewport()->rect(), theme::kWindow);
    if (!s) return;
    auto rs = rows();
    const int W = viewport()->width();

    p.save();
    p.setClipRect(QRect(kHeaderW, kRulerH, W - kHeaderW, viewport()->height() - kRulerH));
    // In/Out range shading.
    if (s->inPoint >= 0 || s->outPoint >= 0) {
        int a = xForFrame(std::max<FrameTime>(0, s->inPoint));
        int b = s->outPoint >= 0 ? xForFrame(s->outPoint + 1) : W;
        p.fillRect(QRect(a, kRulerH, b - a, viewport()->height()), QColor(61, 139, 255, 18));
    }
    for (const Row& r : rs) {
        if (r.y + r.h < kRulerH || r.y > viewport()->height()) continue;
        const Track* t = trackAt(*s, r.ref);
        QColor bg = (r.ref.index % 2) ? theme::kPanel : theme::kPanel.darker(108);
        if (t->locked) bg = bg.darker(125);
        p.fillRect(QRect(kHeaderW, r.y, W - kHeaderW, r.h), bg);
        p.setPen(QColor(0, 0, 0, 90));
        p.drawLine(kHeaderW, r.y + r.h - 1, W, r.y + r.h - 1);
        for (const Clip& c : t->clips) {
            int xs = xForFrame(c.start), xe = xForFrame(c.end());
            if (xe < kHeaderW || xs > W) continue;
            QRect cr(xs, r.y + 1, std::max(2, xe - xs), r.h - 3);
            paintClip(p, r, c, cr);
        }
        for (const auto& tr : t->transitions) {
            FrameTime a, b;
            if (!edit::transitionRange(*t, tr, a, b)) continue;
            QRect tr2(xForFrame(a), r.y + r.h / 2, std::max(4, xForFrame(b) - xForFrame(a)), r.h / 2 - 2);
            bool sel = state_->selectedTransition() == tr.id;
            p.setPen(QPen(sel ? theme::kAccent : QColor(255, 255, 255, 160), sel ? 2 : 1));
            p.setBrush(QColor(20, 20, 24, 170));
            p.drawRoundedRect(tr2, 3, 3);
            p.setPen(QColor(255, 255, 255, 120));
            if (tr.clipA && tr.clipB) {
                p.drawLine(tr2.bottomLeft(), tr2.topRight());
                p.drawLine(tr2.topLeft(), tr2.bottomRight());
            } else if (tr.clipB) {
                p.drawLine(tr2.bottomLeft(), tr2.topRight());
            } else {
                p.drawLine(tr2.topLeft(), tr2.bottomRight());
            }
            if (tr2.width() > 60) {
                const EffectInfo* info = findEffectInfo(tr.type);
                p.setPen(theme::kText);
                QFont f = p.font();
                f.setPointSize(7);
                p.setFont(f);
                p.drawText(tr2.adjusted(3, 0, -3, 0), Qt::AlignCenter,
                           p.fontMetrics().elidedText(info ? QString::fromStdString(info->displayName) : QString(),
                                                      Qt::ElideRight, tr2.width() - 6));
            }
        }
    }
    // Divider between video and audio.
    int dy = dividerY();
    p.fillRect(QRect(kHeaderW, dy, W - kHeaderW, kDividerH), QColor(0x14, 0x15, 0x18));
    // Drop ghosts.
    for (const Ghost& g : ghosts_) {
        for (const Row& r : rs)
            if (r.ref == g.track) {
                QRect gr(xForFrame(g.start), r.y + 2, std::max(4, int(g.duration * ppf_)), r.h - 5);
                p.setPen(QPen(theme::kAccent, 1, Qt::DashLine));
                p.setBrush(QColor(61, 139, 255, 60));
                p.drawRoundedRect(gr, 3, 3);
            }
    }
    // Snap indicator, playhead.
    if (snapIndicator_ >= 0 && (drag_.started || !ghosts_.empty())) {
        int x = xForFrame(snapIndicator_);
        p.setPen(QPen(theme::kSnap, 1));
        p.drawLine(x, kRulerH, x, viewport()->height());
    }
    int px = xForFrame(state_->playhead());
    p.setPen(QPen(theme::kPlayhead, 1));
    p.drawLine(px, kRulerH, px, viewport()->height());
    // Rubber band.
    if (drag_.kind == DragKind::Rubber && drag_.started) {
        p.setPen(QPen(theme::kAccent, 1));
        p.setBrush(QColor(61, 139, 255, 40));
        p.drawRect(drag_.band.normalized());
    }
    p.restore();

    paintHeaders(p, rs);
    paintRuler(p);
    // Live readout during drags.
    if (drag_.started && !drag_.label.isEmpty()) {
        QFont f = theme::monoFont(9);
        p.setFont(f);
        QRect tr = p.fontMetrics().boundingRect(drag_.label).adjusted(-6, -3, 6, 3);
        tr.moveTopLeft(hoverPos_ + QPoint(14, 18));
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 200));
        p.drawRoundedRect(tr, 3, 3);
        p.setPen(theme::kText);
        p.drawText(tr, Qt::AlignCenter, drag_.label);
    }
}

void TimelineWidget::paintClip(QPainter& p, const Row& row, const Clip& c, const QRect& r) {
    const Project& proj = state_->project();
    QColor base = clipColor(proj, c, row.ref.kind);
    bool sel = state_->isSelected(c.id);
    p.setPen(Qt::NoPen);
    p.setBrush(sel ? base.lighter(125) : base);
    p.drawRoundedRect(r, 3, 3);
    p.save();
    p.setClipRect(r.adjusted(1, 1, -1, -1), Qt::IntersectClip);
    QRect body = r.adjusted(0, kNameStrip, 0, 0);
    if (row.ref.kind == TrackKind::Video) {
        if (c.isGenerator()) {
            if (c.generator.type == "color" && body.height() > 4) {
                QColor col = QColor::fromRgbF(float(std::clamp(c.generator.p("color.r", 0), 0.0, 1.0)),
                                              float(std::clamp(c.generator.p("color.g", 0), 0.0, 1.0)),
                                              float(std::clamp(c.generator.p("color.b", 0), 0.0, 1.0)));
                p.fillRect(body.adjusted(2, 0, -2, -2), col);
            } else if (c.generator.type == "title" && body.height() > 10) {
                p.setPen(QColor(255, 255, 255, 200));
                QFont f = p.font();
                f.setPointSize(8);
                f.setItalic(true);
                p.setFont(f);
                QString text = QString::fromStdString(c.generator.s("text")).simplified();
                p.drawText(body.adjusted(6, 0, -4, -2), Qt::AlignLeft | Qt::AlignVCenter,
                           p.fontMetrics().elidedText("“" + text + "”", Qt::ElideRight, body.width() - 10));
            }
        } else {
            paintThumbnails(p, c, body);
        }
    } else {
        paintWaveform(p, c, body.adjusted(0, 1, 0, -2), base.lighter(170));
    }
    if (!c.enabled) {
        p.setPen(QPen(QColor(0, 0, 0, 60), 1));
        for (int x = r.left() - r.height(); x < r.right(); x += 8) p.drawLine(x, r.bottom(), x + r.height(), r.top());
    }
    // Name strip with badges.
    p.fillRect(QRect(r.left(), r.top(), r.width(), kNameStrip), QColor(0, 0, 0, 70));
    QString badges;
    if (c.speed != 1.0 || c.reverse) badges += QString(" %1%2%").arg(c.reverse ? "-" : "").arg(c.speed * 100, 0, 'f', 0);
    if (!c.effects.empty()) badges += " fx";
    QFont f = p.font();
    f.setPointSize(8);
    p.setFont(f);
    p.setPen(theme::kText);
    QRect nameR = r.adjusted(5, 1, -4, 0);
    nameR.setHeight(kNameStrip - 2);
    QString name = QString::fromStdString(c.name);
    if (!badges.isEmpty()) {
        int bw = p.fontMetrics().horizontalAdvance(badges);
        if (nameR.width() > bw + 30) {
            p.setPen(theme::kSnap);
            p.drawText(nameR, Qt::AlignRight | Qt::AlignVCenter, badges);
            nameR.setRight(nameR.right() - bw - 4);
            p.setPen(theme::kText);
        }
    }
    p.drawText(nameR, Qt::AlignLeft | Qt::AlignVCenter, p.fontMetrics().elidedText(name, Qt::ElideRight, nameR.width()));
    if (c.linkGroup == 0 && row.ref.kind == TrackKind::Video && proj.findMedia(c.mediaId) &&
        proj.findMedia(c.mediaId)->hasAudio && !c.isGenerator()) {
        // Unlinked A/V indicator.
        p.setPen(theme::kPlayhead);
        p.drawText(r.adjusted(0, 1, -4, 0), Qt::AlignRight | Qt::AlignTop, QString());
    }
    // Keyframe markers along the bottom edge.
    if (r.width() > 24) {
        std::set<FrameTime> keys;
        auto collect = [&keys](const Effect& e) {
            for (const auto& [n, param] : e.params)
                for (const auto& k : param.keys) keys.insert(k.t);
        };
        collect(c.motion);
        collect(c.audio);
        collect(c.generator);
        for (const auto& e : c.effects) collect(e);
        p.setPen(Qt::NoPen);
        p.setBrush(sel ? theme::kSnap : QColor(255, 255, 255, 150));
        int y = r.bottom() - 5;
        for (FrameTime k : keys) {
            if (k < 0 || k >= c.duration) continue;
            int x = xForFrame(c.start + k);
            QPolygon d;
            d << QPoint(x, y - 3) << QPoint(x + 3, y) << QPoint(x, y + 3) << QPoint(x - 3, y);
            p.drawPolygon(d);
        }
    }
    p.restore();
    if (sel) {
        p.setPen(QPen(theme::kSelection, 2));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(r.adjusted(1, 1, -1, -1), 3, 3);
    } else {
        p.setPen(QPen(base.darker(150), 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(r, 3, 3);
    }
}

void TimelineWidget::paintThumbnails(QPainter& p, const Clip& c, const QRect& body) {
    const Project& proj = state_->project();
    const Sequence* s = state_->sequence();
    const MediaItem* m = proj.findMedia(c.mediaId);
    if (!m || !s || body.height() < 16) return;
    if (m->kind != MediaKind::Video && m->kind != MediaKind::Image) return;
    int th = body.height() - 2;
    double aspect = (m->width > 0 && m->height > 0) ? double(m->width) / m->height : 16.0 / 9.0;
    int tw = std::max(8, int(th * aspect));
    double fps = s->fpsValue();
    QString path = QString::fromStdString(m->path);
    int left = std::max(body.left(), kHeaderW - tw), right = std::min(body.right(), viewport()->width());
    // Tile thumbnails from the clip start; each shows the source frame at its left edge.
    int first = std::max(0, (left - body.left()) / tw);
    for (int i = first;; ++i) {
        int x = body.left() + i * tw;
        if (x > right) break;
        FrameTime tl = c.start + FrameTime(double(i * tw) / ppf_);
        double sec = m->kind == MediaKind::Image ? 0.0 : std::clamp(c.sourceFrameAt(tl) / fps, 0.0, std::max(0.0, m->duration - 0.05));
        double quantum = std::max(1.0 / fps, double(tw) / ppf_ / fps);
        QImage img = ThumbnailCache::instance().get(path, sec, tw, th, quantum);
        if (!img.isNull()) p.drawImage(QRect(x, body.top() + 1, tw, th), img);
        else p.fillRect(QRect(x, body.top() + 1, tw - 1, th), QColor(0, 0, 0, 40));
        if (m->kind == MediaKind::Image) break;
    }
}

void TimelineWidget::paintWaveform(QPainter& p, const Clip& c, const QRect& r, const QColor& col) {
    const Project& proj = state_->project();
    const Sequence* s = state_->sequence();
    const MediaItem* m = proj.findMedia(c.mediaId);
    if (!m || !s || r.height() < 6 || !m->hasAudio || m->kind == MediaKind::Sequence) return;
    PeaksPtr pk = MediaPool::instance().peaksIfReady(m->path);
    if (!pk || pk->minmax.empty()) {
        p.setPen(QColor(255, 255, 255, 60));
        p.drawLine(r.left(), r.center().y(), r.right(), r.center().y());
        return;
    }
    const double fps = s->fpsValue();
    const double rate = pk->sampleRate;
    const size_t buckets = pk->minmax.size() / 2;
    int x0 = std::max(r.left(), kHeaderW), x1 = std::min(r.right(), viewport()->width());
    double gain = std::pow(10.0, c.audio.p("gain_db", 0, 0) / 20.0);
    double mid = r.center().y(), half = r.height() / 2.0;
    p.setPen(col);
    for (int x = x0; x <= x1; ++x) {
        double fA = frameAtX(x), fB = frameAtX(x + 1);
        if (fB <= c.start || fA >= c.end()) continue;
        double sA = c.sourceFrameAt(FrameTime(std::floor(fA))) / fps * rate;
        double sB = c.sourceFrameAt(FrameTime(std::floor(fB))) / fps * rate;
        if (sB < sA) std::swap(sA, sB);
        size_t bA = size_t(std::max(0.0, sA / pk->samplesPerBucket));
        size_t bB = size_t(std::max(0.0, sB / pk->samplesPerBucket)) + 1;
        if (bA >= buckets) continue;
        bB = std::min(bB, buckets);
        float lo = 0, hi = 0;
        for (size_t b = bA; b < bB; ++b) {
            lo = std::min(lo, pk->minmax[b * 2]);
            hi = std::max(hi, pk->minmax[b * 2 + 1]);
        }
        int y1 = int(mid - std::min(1.0, hi * gain) * half), y2 = int(mid - std::max(-1.0, lo * gain) * half);
        p.drawLine(x, y1, x, std::max(y1, y2));
    }
}

void TimelineWidget::paintRuler(QPainter& p) {
    const Sequence* s = state_->sequence();
    const int W = viewport()->width();
    p.fillRect(QRect(0, 0, W, kRulerH), theme::kPanelAlt);
    p.setPen(theme::kBorder);
    p.drawLine(0, kRulerH - 1, W, kRulerH - 1);
    // Corner: timecode of the playhead.
    p.setFont(theme::monoFont(11));
    p.setPen(theme::kAccent);
    p.drawText(QRect(8, 0, kHeaderW - 8, kRulerH), Qt::AlignVCenter | Qt::AlignLeft, timecodeString(s, state_->playhead()));
    p.save();
    p.setClipRect(QRect(kHeaderW, 0, W - kHeaderW, kRulerH));
    double fps = s->fpsValue();
    // Choose a major tick spacing of at least ~90 px.
    static const double candidates[] = {1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600, 7200, 18000, 36000, 108000, 216000};
    double majorFrames = candidates[0];
    for (double cand : candidates) {
        double frames = cand <= 15 && cand < fps ? cand : std::round(cand / 30.0 * fps);
        if (cand > 15 || cand >= fps) frames = std::max(1.0, std::round((cand / 30.0) * fps));
        majorFrames = std::max(1.0, frames);
        if (majorFrames * ppf_ >= 90) break;
    }
    double minorFrames = std::max(1.0, majorFrames / 5);
    FrameTime startF = FrameTime(std::floor(frameAtX(kHeaderW) / minorFrames) * minorFrames);
    FrameTime endF = FrameTime(frameAtX(W)) + 1;
    QFont f = theme::monoFont(8);
    p.setFont(f);
    for (double fr = double(std::max<FrameTime>(0, startF)); fr <= double(endF); fr += minorFrames) {
        FrameTime ft = FrameTime(std::llround(fr));
        int x = xForFrame(ft);
        bool major = std::fmod(fr + 0.5, majorFrames) < 1.0;
        p.setPen(major ? theme::kTextDim : theme::kBorder);
        p.drawLine(x, major ? kRulerH - 12 : kRulerH - 6, x, kRulerH - 1);
        if (major) {
            p.setPen(theme::kTextDim);
            p.drawText(x + 3, 12, timecodeString(s, ft));
        }
    }
    // In/out band.
    if (s->inPoint >= 0 || s->outPoint >= 0) {
        int a = xForFrame(std::max<FrameTime>(0, s->inPoint));
        int b = s->outPoint >= 0 ? xForFrame(s->outPoint + 1) : W;
        p.fillRect(QRect(a, kRulerH - 6, b - a, 5), theme::kAccent);
    }
    // Markers.
    for (const auto& m : s->markers) {
        int x = xForFrame(m.t);
        QColor col = theme::labelColor(m.color);
        if (!col.isValid()) col = theme::kSnap;
        QPolygon poly;
        poly << QPoint(x - 5, 14) << QPoint(x + 5, 14) << QPoint(x + 5, 20) << QPoint(x, 25) << QPoint(x - 5, 20);
        p.setPen(Qt::NoPen);
        p.setBrush(col);
        p.drawPolygon(poly);
        if (m.duration > 0) p.fillRect(QRect(x, 14, int(m.duration * ppf_), 3), col);
    }
    // Playhead handle.
    int px = xForFrame(state_->playhead());
    QPolygon head;
    head << QPoint(px - 6, 16) << QPoint(px + 6, 16) << QPoint(px + 6, 22) << QPoint(px, kRulerH - 1) << QPoint(px - 6, 22);
    p.setBrush(theme::kPlayhead);
    p.setPen(Qt::NoPen);
    p.drawPolygon(head);
    p.restore();
}

void TimelineWidget::paintHeaders(QPainter& p, const std::vector<Row>& rs) {
    const Sequence* s = state_->sequence();
    const int H = viewport()->height();
    p.save();
    p.setClipRect(QRect(0, kRulerH, kHeaderW, H - kRulerH));
    p.fillRect(QRect(0, kRulerH, kHeaderW, H), theme::kPanel);
    QFont f = p.font();
    f.setPointSize(8);
    p.setFont(f);
    auto button = [&](const QRect& r, const QString& text, bool on, const QColor& onColor) {
        p.setPen(QPen(on ? onColor : theme::kBorder, 1));
        p.setBrush(on ? onColor.darker(160) : theme::kPanelAlt);
        p.drawRoundedRect(r, 3, 3);
        p.setPen(on ? Qt::white : theme::kTextDim);
        p.drawText(r, Qt::AlignCenter, text);
    };
    for (const Row& r : rs) {
        const Track* t = trackAt(*s, r.ref);
        QRect hr(0, r.y, kHeaderW, r.h);
        p.fillRect(hr, (r.ref.index % 2) ? theme::kPanelAlt : theme::kPanelAlt.darker(106));
        p.setPen(QColor(0, 0, 0, 120));
        p.drawLine(0, r.y + r.h - 1, kHeaderW, r.y + r.h - 1);
        bool video = r.ref.kind == TrackKind::Video;
        bool target = video ? state_->targetVideoTrack() == r.ref.index : state_->targetAudioTrack() == r.ref.index;
        button(headerButtonRect(r, HeaderButton::Target), QString::fromStdString(t->name), target, theme::kAccent);
        button(headerButtonRect(r, HeaderButton::Lock), QStringLiteral("L"), t->locked, QColor(0xc8, 0x8a, 0x3c));
        if (video) {
            button(headerButtonRect(r, HeaderButton::Visible), t->muted ? QStringLiteral("—") : QStringLiteral("◉"), !t->muted,
                   QColor(0x4a, 0x80, 0xc8));
        } else {
            button(headerButtonRect(r, HeaderButton::Mute), QStringLiteral("M"), t->muted, QColor(0xc8, 0x46, 0x46));
            button(headerButtonRect(r, HeaderButton::Solo), QStringLiteral("S"), t->solo, QColor(0xd8, 0xc2, 0x3a));
        }
        if (r.h >= 36) {
            p.setPen(theme::kTextDim);
            QRect info(8, r.y + r.h - 16, kHeaderW - 16, 14);
            QString detail = video ? tr("Video") : (t->volumeDb != 0 ? tr("%1 dB").arg(t->volumeDb, 0, 'f', 1) : tr("Audio"));
            p.drawText(info, Qt::AlignLeft | Qt::AlignVCenter, detail);
        }
    }
    int dy = dividerY();
    p.fillRect(QRect(0, dy, kHeaderW, kDividerH), QColor(0x14, 0x15, 0x18));
    p.setPen(theme::kBorder);
    p.drawLine(kHeaderW - 1, kRulerH, kHeaderW - 1, H);
    p.restore();
}

// ---------------------------------------------------------------------------
// Mouse

void TimelineWidget::handleHeaderClick(const Hit& hit) {
    if (!hit.track) return;
    TrackRef ref = *hit.track;
    switch (hit.button) {
        case HeaderButton::Target:
            if (ref.kind == TrackKind::Video) state_->setTargetVideoTrack(ref.index);
            else state_->setTargetAudioTrack(ref.index);
            break;
        case HeaderButton::Lock:
            state_->edit(tr("Lock Track"), [ref](Project&, Sequence& s) {
                Track* t = trackAt(s, ref);
                t->locked = !t->locked;
                return true;
            });
            break;
        case HeaderButton::Visible:
        case HeaderButton::Mute:
            state_->edit(ref.kind == TrackKind::Video ? tr("Toggle Track Output") : tr("Mute Track"), [ref](Project&, Sequence& s) {
                Track* t = trackAt(s, ref);
                t->muted = !t->muted;
                return true;
            });
            break;
        case HeaderButton::Solo:
            state_->edit(tr("Solo Track"), [ref](Project&, Sequence& s) {
                Track* t = trackAt(s, ref);
                t->solo = !t->solo;
                return true;
            });
            break;
        default: break;
    }
}

void TimelineWidget::mousePressEvent(QMouseEvent* e) {
    setFocus();
    hoverPos_ = e->pos();
    const Sequence* s = state_->sequence();
    if (!s) return;
    Hit hit = hitTest(e->pos());
    drag_ = DragState{};
    drag_.pressPos = e->pos();
    drag_.pressFrame = hit.frame;
    drag_.pressTrack = hit.track;
    drag_.hOffsetAtPress = horizontalScrollBar()->value();
    drag_.vOffsetAtPress = verticalScrollBar()->value();

    if (e->button() == Qt::MiddleButton || (e->button() == Qt::LeftButton && tool_ == Tool::Hand)) {
        drag_.kind = DragKind::Pan;
        viewport()->setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (e->button() != Qt::LeftButton) return;
    if (hit.kind == HitKind::Header) {
        handleHeaderClick(hit);
        return;
    }
    if (hit.kind == HitKind::Ruler) {
        drag_.kind = DragKind::Scrub;
        drag_.started = true;
        FrameTime f = hit.frame;
        if (e->modifiers() & Qt::ShiftModifier) f = snapFrame(f, {});
        state_->setPlayhead(f);
        return;
    }
    beginDrag(e, hit);
}

void TimelineWidget::beginDrag(QMouseEvent* e, const Hit& hit) {
    const Sequence* s = state_->sequence();
    const bool shift = e->modifiers() & Qt::ShiftModifier;
    const bool ctrl = e->modifiers() & Qt::ControlModifier;
    const bool alt = e->modifiers() & Qt::AltModifier;

    if (tool_ == Tool::Razor && (hit.kind == HitKind::ClipBody || hit.kind == HitKind::ClipIn || hit.kind == HitKind::ClipOut)) {
        FrameTime f = snapFrame(hit.frame, {});
        snapIndicator_ = -1;
        TrackRef t = *hit.track;
        if (shift) state_->apply(tr("Add Edit to All Tracks"), [f](Project& p, Sequence& sq) { return edit::razorAll(p, sq, f); });
        else
            state_->apply(tr("Add Edit"), [f, t](Project& p, Sequence& sq) {
                // Razor cuts linked partners too, like the industry-standard tools.
                const Clip* c = edit::clipAt(sq, t, f);
                if (!c) return edit::Result::fail("No clip here");
                std::vector<TrackRef> tracks{t};
                for (Id l : edit::linkedClips(sq, c->id))
                    if (auto loc = edit::locate(sq, l)) tracks.push_back(loc->track);
                edit::Result last = edit::Result::fail("Nothing to cut");
                std::map<Id, Id> regroup;
                for (TrackRef tr : tracks) {
                    const Clip* under = edit::clipAt(sq, tr, f);
                    Id group = under ? under->linkGroup : 0;
                    auto r = edit::razor(p, sq, tr, f);
                    if (r.ok) {
                        last = r;
                        if (group && !r.created.empty()) {
                            auto it = regroup.find(group);
                            if (it == regroup.end()) it = regroup.emplace(group, p.newId()).first;
                            if (Clip* rc = edit::clipById(sq, r.created[0])) rc->linkGroup = it->second;
                        }
                    }
                }
                return last;
            });
        return;
    }
    if (hit.kind == HitKind::Transition) {
        state_->selectTransition(hit.transition);
        return;
    }
    if (hit.kind == HitKind::ClipBody || hit.kind == HitKind::ClipIn || hit.kind == HitKind::ClipOut) {
        Id id = hit.clip;
        // Selection.
        if (ctrl || shift) {
            std::vector<Id> sel = state_->selectedClips();
            auto group = alt ? std::vector<Id>{id} : edit::linkedClips(*s, id);
            if (state_->isSelected(id)) {
                for (Id g : group) sel.erase(std::remove(sel.begin(), sel.end(), g), sel.end());
            } else {
                sel.insert(sel.end(), group.begin(), group.end());
            }
            state_->setSelection(sel, false);
            if (hit.kind == HitKind::ClipBody) return;
        } else if (!state_->isSelected(id) || alt) {
            state_->setSelection({id}, !alt);
        }
        drag_.clip = id;
        drag_.unlinked = alt;
        drag_.insertMode = ctrl;
        if (hit.kind == HitKind::ClipBody) {
            if (tool_ == Tool::Slip) drag_.kind = DragKind::Slip;
            else if (tool_ == Tool::Slide) drag_.kind = DragKind::Slide;
            else drag_.kind = DragKind::Move;
            drag_.ids = state_->selectedClips();
        } else {
            drag_.edge = hit.kind == HitKind::ClipIn ? edit::Edge::In : edit::Edge::Out;
            drag_.kind = DragKind::Trim;
            if (const Clip* c = edit::clipById(*s, id)) drag_.origEdge = drag_.edge == edit::Edge::In ? c->start : c->end();
            if (tool_ == Tool::Roll) {
                // Roll needs the adjacent clip on the other side of the edit.
                auto loc = edit::locate(*s, id);
                const Track* t = trackAt(*s, loc->track);
                size_t i = loc->index;
                if (drag_.edge == edit::Edge::Out && i + 1 < t->clips.size() && t->clips[i + 1].start == t->clips[i].end()) {
                    drag_.kind = DragKind::Roll;
                    drag_.neighbor = t->clips[i + 1].id;
                } else if (drag_.edge == edit::Edge::In && i > 0 && t->clips[i - 1].end() == t->clips[i].start) {
                    drag_.kind = DragKind::Roll;
                    drag_.neighbor = drag_.clip;
                    drag_.clip = t->clips[i - 1].id;
                }
            }
        }
        return;
    }
    // Empty area: rubber band selection.
    if (!(ctrl || shift)) state_->clearSelection();
    drag_.kind = DragKind::Rubber;
}

void TimelineWidget::updateDrag(QMouseEvent* e) {
    // Positions and clamps come from the state at the start of the drag; the
    // live state already contains the previous update of this gesture.
    if (!state_->sequence()) return;
    QPoint pos = e->pos();
    if (!drag_.started) {
        if ((pos - drag_.pressPos).manhattanLength() < QApplication::startDragDistance()) return;
        drag_.started = true;
        switch (drag_.kind) {
            case DragKind::Move: state_->beginGesture(drag_.insertMode ? tr("Insert Move") : tr("Move")); break;
            case DragKind::Trim: state_->beginGesture(tool_ == Tool::Ripple ? tr("Ripple Trim") : tr("Trim")); break;
            case DragKind::Roll: state_->beginGesture(tr("Roll Edit")); break;
            case DragKind::Slip: state_->beginGesture(tr("Slip")); break;
            case DragKind::Slide: state_->beginGesture(tr("Slide")); break;
            default: break;
        }
    }
    const Sequence* s = state_->gestureBase();  // fetched after beginGesture() above
    const FrameTime raw = frameRound(pos.x()) - drag_.pressFrame;
    auto signedTc = [s](FrameTime d) { return (d < 0 ? "-" : "+") + timecodeString(s, std::llabs(d)); };
    switch (drag_.kind) {
        case DragKind::Scrub: {
            FrameTime f = frameRound(pos.x());
            if (e->modifiers() & Qt::ShiftModifier) f = snapFrame(f, {});
            state_->setPlayhead(f);
            break;
        }
        case DragKind::Pan:
            horizontalScrollBar()->setValue(drag_.hOffsetAtPress - (pos.x() - drag_.pressPos.x()));
            verticalScrollBar()->setValue(drag_.vOffsetAtPress - (pos.y() - drag_.pressPos.y()));
            break;
        case DragKind::Move: {
            FrameTime delta = snapDelta(drag_.ids, raw);
            int vd = 0, ad = 0;
            auto row = rowAt(pos.y());
            if (row && drag_.pressTrack && row->ref.kind == drag_.pressTrack->kind) {
                int d = row->ref.index - drag_.pressTrack->index;
                if (row->ref.kind == TrackKind::Video) vd = d;
                else ad = d;
            }
            // Clamp track movement so every clip stays on an existing track.
            int vMin = 0, vMax = int(s->videoTracks.size()) - 1, aMin = 0, aMax = int(s->audioTracks.size()) - 1;
            for (Id id : drag_.ids)
                if (auto loc = edit::locate(*s, id)) {
                    if (loc->track.kind == TrackKind::Video) {
                        vd = std::clamp(vd, vMin - loc->track.index, vMax - loc->track.index);
                    } else {
                        ad = std::clamp(ad, aMin - loc->track.index, aMax - loc->track.index);
                    }
                }
            std::vector<Id> ids = drag_.ids;
            bool ins = drag_.insertMode;
            state_->updateGesture([=](Project& p, Sequence& sq) { edit::moveClips(p, sq, ids, delta, vd, ad, ins); });
            drag_.label = signedTc(delta);
            break;
        }
        case DragKind::Trim: {
            Id id = drag_.clip;
            auto mode = tool_ == Tool::Ripple ? edit::TrimMode::Ripple : edit::TrimMode::Normal;
            bool linked = !drag_.unlinked;
            edit::Edge edge = drag_.edge;
            bool snapped = false;
            FrameTime target = snapFrame(drag_.origEdge + raw, edit::linkedClips(*s, id), &snapped);
            FrameTime delta = snapped ? target - drag_.origEdge : raw;
            FrameTime applied = 0;
            state_->updateGesture([&](Project& p, Sequence& sq) { applied = edit::trim(p, sq, id, edge, delta, mode, linked).applied; });
            drag_.label = signedTc(applied);
            break;
        }
        case DragKind::Roll: {
            Id a = drag_.clip, b = drag_.neighbor;
            bool snapped = false;
            std::vector<Id> ex = edit::expandLinks(*s, {a, b});
            FrameTime target = snapFrame(drag_.origEdge + raw, ex, &snapped);
            FrameTime delta = snapped ? target - drag_.origEdge : raw;
            FrameTime applied = 0;
            state_->updateGesture([&](Project& p, Sequence& sq) { applied = edit::roll(p, sq, a, b, delta).applied; });
            drag_.label = signedTc(applied);
            break;
        }
        case DragKind::Slip: {
            Id id = drag_.clip;
            FrameTime applied = 0;
            state_->updateGesture([&](Project& p, Sequence& sq) { applied = edit::slip(p, sq, id, -raw).applied; });
            drag_.label = tr("Slip %1").arg(signedTc(applied));
            break;
        }
        case DragKind::Slide: {
            Id id = drag_.clip;
            FrameTime delta = snapDelta({id}, raw);
            FrameTime applied = 0;
            state_->updateGesture([&](Project& p, Sequence& sq) { applied = edit::slide(p, sq, id, delta).applied; });
            drag_.label = tr("Slide %1").arg(signedTc(applied));
            break;
        }
        case DragKind::Rubber: {
            drag_.band = QRect(drag_.pressPos, pos);
            QRect band = drag_.band.normalized();
            std::vector<Id> sel;
            if (e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier)) sel = state_->selectedClips();
            for (const Row& r : rows()) {
                if (r.y + r.h < band.top() || r.y > band.bottom()) continue;
                for (const Clip& c : trackAt(*s, r.ref)->clips) {
                    QRect cr(xForFrame(c.start), r.y, xForFrame(c.end()) - xForFrame(c.start), r.h);
                    if (cr.intersects(band) && std::find(sel.begin(), sel.end(), c.id) == sel.end()) sel.push_back(c.id);
                }
            }
            state_->setSelection(sel, true);
            break;
        }
        default: break;
    }
    viewport()->update();
}

void TimelineWidget::mouseMoveEvent(QMouseEvent* e) {
    hoverPos_ = e->pos();
    if (drag_.kind != DragKind::None && (e->buttons() & (Qt::LeftButton | Qt::MiddleButton))) {
        updateDrag(e);
        return;
    }
    // Hover cursor feedback.
    if (tool_ == Tool::Select || tool_ == Tool::Ripple || tool_ == Tool::Roll) {
        Hit h = hitTest(e->pos());
        if (h.kind == HitKind::ClipIn || h.kind == HitKind::ClipOut) viewport()->setCursor(Qt::SizeHorCursor);
        else if (tool_ == Tool::Select) viewport()->unsetCursor();
    }
}

void TimelineWidget::mouseReleaseEvent(QMouseEvent* e) {
    if (drag_.kind == DragKind::Pan) {
        setTool(tool_);  // restore cursor
        if (tool_ == Tool::Hand) viewport()->setCursor(Qt::OpenHandCursor);
    }
    bool gesture = drag_.started && (drag_.kind == DragKind::Move || drag_.kind == DragKind::Trim || drag_.kind == DragKind::Roll ||
                                     drag_.kind == DragKind::Slip || drag_.kind == DragKind::Slide);
    if (gesture) state_->endGesture(true);
    drag_ = DragState{};
    snapIndicator_ = -1;
    viewport()->update();
    QAbstractScrollArea::mouseReleaseEvent(e);
}

void TimelineWidget::mouseDoubleClickEvent(QMouseEvent* e) {
    Hit h = hitTest(e->pos());
    if (h.kind == HitKind::ClipBody) {
        emit clipActivated(h.clip);
    } else if (h.kind == HitKind::Header && h.button == HeaderButton::Name && h.track) {
        const Track* t = trackAt(*state_->sequence(), *h.track);
        bool ok = false;
        QString name = QInputDialog::getText(this, tr("Rename Track"), tr("Name:"), QLineEdit::Normal,
                                             QString::fromStdString(t->name), &ok);
        TrackRef ref = *h.track;
        if (ok && !name.isEmpty())
            state_->edit(tr("Rename Track"), [ref, name](Project&, Sequence& s) {
                trackAt(s, ref)->name = name.toStdString();
                return true;
            });
    }
}

void TimelineWidget::wheelEvent(QWheelEvent* e) {
    QPoint delta = e->angleDelta();
    if (e->modifiers() & (Qt::ControlModifier | Qt::AltModifier)) {
        int d = delta.y() != 0 ? delta.y() : delta.x();
        zoomAround(int(e->position().x()), d > 0 ? 1.25 : 0.8);
        e->accept();
        return;
    }
    if ((e->modifiers() & Qt::ShiftModifier) || e->position().x() < kHeaderW) {
        verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y() / 2);
    } else {
        int d = delta.x() != 0 ? delta.x() : delta.y();
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - d);
    }
    e->accept();
}

void TimelineWidget::leaveEvent(QEvent* e) {
    if (drag_.kind == DragKind::None) viewport()->unsetCursor();
    QAbstractScrollArea::leaveEvent(e);
}

void TimelineWidget::contextMenuEvent(QContextMenuEvent* e) {
    Hit h = hitTest(e->pos());
    contextFrame_ = h.frame;
    contextTrack_ = h.track;
    QMenu menu(this);
    if (h.kind == HitKind::ClipBody || h.kind == HitKind::ClipIn || h.kind == HitKind::ClipOut) {
        if (!state_->isSelected(h.clip)) state_->setSelection({h.clip});
        menu.addActions(clipActions_);
    } else if (h.kind == HitKind::Transition) {
        state_->selectTransition(h.transition);
        Id id = h.transition;
        menu.addAction(tr("Delete Transition"), this, [this, id] {
            state_->apply(tr("Delete Transition"), [id](Project&, Sequence& s) { return edit::removeTransition(s, id); });
        });
    } else if (h.kind == HitKind::Header && h.track) {
        TrackRef ref = *h.track;
        bool video = ref.kind == TrackKind::Video;
        menu.addAction(video ? tr("Add Video Track") : tr("Add Audio Track"), this, [this, ref] {
            state_->edit(tr("Add Track"), [ref](Project& p, Sequence& s) {
                edit::addTrack(p, s, ref.kind);
                return true;
            });
        });
        menu.addAction(tr("Delete Track"), this, [this, ref] {
            state_->apply(tr("Delete Track"), [ref](Project&, Sequence& s) { return edit::removeTrack(s, ref); });
        });
        QAction* sync = menu.addAction(tr("Sync Lock"));
        sync->setCheckable(true);
        sync->setChecked(trackAt(*state_->sequence(), ref)->syncLock);
        connect(sync, &QAction::toggled, this, [this, ref](bool on) {
            state_->edit(tr("Sync Lock"), [ref, on](Project&, Sequence& s) {
                trackAt(s, ref)->syncLock = on;
                return true;
            });
        });
        QMenu* height = menu.addMenu(tr("Track Height"));
        for (auto [label, px] : {std::pair{tr("Small"), 36}, {tr("Medium"), 0}, {tr("Large"), 96}, {tr("Extra Large"), 140}}) {
            int hpx = px;
            height->addAction(label, this, [this, ref, hpx] {
                state_->edit(tr("Track Height"), [ref, hpx](Project&, Sequence& s) {
                    trackAt(s, ref)->height = hpx;
                    return true;
                });
            });
        }
    } else {
        menu.addActions(emptyActions_);
    }
    if (!menu.isEmpty()) menu.exec(e->globalPos());
}

// ---------------------------------------------------------------------------
// Drag & drop

void TimelineWidget::dragEnterEvent(QDragEnterEvent* e) {
    const QMimeData* m = e->mimeData();
    if (m->hasFormat("application/x-montage-media") || m->hasFormat("application/x-montage-effect") || m->hasUrls())
        e->acceptProposedAction();
}

std::vector<TimelineWidget::Ghost> TimelineWidget::ghostsFor(const QMimeData* mime, const QPoint& pos) const {
    std::vector<Ghost> out;
    const Sequence* s = state_->sequence();
    if (!s) return out;
    auto refs = parseMediaMime(mime);
    if (refs.empty()) return out;
    auto row = rowAt(pos.y());
    int vt = row && row->ref.kind == TrackKind::Video ? row->ref.index : state_->targetVideoTrack();
    int at = row && row->ref.kind == TrackKind::Audio ? row->ref.index : state_->targetAudioTrack();
    FrameTime f = frameRound(pos.x());
    for (const auto& r : refs) {
        const MediaItem* m = state_->project().findMedia(r.id);
        if (!m) continue;
        FrameTime len = mediaFrames(*m, *s);
        if (m->kind == MediaKind::Sequence)
            if (const Sequence* n = state_->project().findSequence(m->sequenceId)) len = std::max<FrameTime>(1, n->duration());
        if (len >= kInfiniteFrames) len = FrameTime(std::llround(5 * s->fpsValue()));
        if (r.in >= 0 || r.out >= 0) len = (r.out >= 0 ? r.out + 1 : len) - std::max<FrameTime>(0, r.in);
        bool v = m->hasVideo || m->kind == MediaKind::Image || m->kind == MediaKind::Sequence;
        bool a = m->hasAudio && m->kind != MediaKind::Image;
        if (v) out.push_back({{TrackKind::Video, std::min(vt, int(s->videoTracks.size()) - 1)}, f, len});
        if (a) out.push_back({{TrackKind::Audio, std::min(at, int(s->audioTracks.size()) - 1)}, f, len});
        f += len;
    }
    return out;
}

void TimelineWidget::dragMoveEvent(QDragMoveEvent* e) {
    const QMimeData* m = e->mimeData();
    hoverPos_ = e->position().toPoint();
    if (m->hasFormat("application/x-montage-media")) {
        ghosts_ = ghostsFor(m, hoverPos_);
        // Snap the drop point.
        if (!ghosts_.empty()) {
            bool snapped = false;
            FrameTime f = snapFrame(ghosts_.front().start, {}, &snapped);
            FrameTime shift = f - ghosts_.front().start;
            for (auto& g : ghosts_) g.start += shift;
        }
    } else {
        ghosts_.clear();
    }
    e->acceptProposedAction();
    viewport()->update();
}

void TimelineWidget::dragLeaveEvent(QDragLeaveEvent*) {
    ghosts_.clear();
    snapIndicator_ = -1;
    viewport()->update();
}

void TimelineWidget::dropMedia(const QMimeData* mime, const QPoint& pos, bool insertMode) {
    const Sequence* s = state_->sequence();
    auto refs = parseMediaMime(mime);
    auto ghosts = ghostsFor(mime, pos);
    if (refs.empty() || ghosts.empty() || !s) return;
    FrameTime start = snapFrame(ghosts.front().start, {});
    auto row = rowAt(pos.y());
    int vt = row && row->ref.kind == TrackKind::Video ? row->ref.index : state_->targetVideoTrack();
    int at = row && row->ref.kind == TrackKind::Audio ? row->ref.index : state_->targetAudioTrack();
    std::vector<Id> created;
    QString matched;
    state_->apply(insertMode ? tr("Insert") : tr("Overwrite"), [&](Project& p, Sequence& sq) {
        FrameTime f = start;
        edit::Result last = edit::Result::fail("Nothing to place");
        TrackRef v{TrackKind::Video, std::min(vt, int(sq.videoTracks.size()) - 1)};
        TrackRef a{TrackKind::Audio, std::min(at, int(sq.audioTracks.size()) - 1)};
        if (const MediaItem* first = p.findMedia(refs.front().id); first && edit::matchSequenceToMedia(sq, *first))
            matched = QString::fromStdString(first->name);
        for (const auto& r : refs) {
            const MediaItem* m = p.findMedia(r.id);
            if (!m) continue;
            if (m->kind == MediaKind::Sequence && m->sequenceId == sq.id) {
                last = edit::Result::fail("A sequence cannot be placed inside itself");
                continue;
            }
            double in = r.in >= 0 ? double(r.in) : 0.0;
            double out = r.out >= 0 ? double(r.out + 1) : -1.0;
            auto res = edit::placeMedia(p, sq, r.id, f, in, out, v, a, insertMode);
            if (!res.ok) {
                last = res;
                continue;
            }
            last = res;
            created.insert(created.end(), res.created.begin(), res.created.end());
            for (Id id : res.created)
                if (const Clip* c = edit::clipById(sq, id)) f = std::max(f, c->end());
        }
        if (!created.empty()) last.ok = true;
        return last;
    });
    if (!created.empty()) state_->setSelection(created);
    if (!matched.isEmpty() && !created.empty())
        state_->message(tr("Sequence settings changed to match %1").arg(matched), 6000);
}

void TimelineWidget::dropEffect(const QString& typeQ, const QPoint& pos) {
    const Sequence* s = state_->sequence();
    std::string type = typeQ.toStdString();
    const EffectInfo* info = findEffectInfo(type);
    if (!info || !s) return;
    Hit h = hitTest(pos);
    auto row = rowAt(pos.y());
    switch (info->category) {
        case EffectCategory::VideoFilter:
        case EffectCategory::AudioFilter: {
            if (!h.clip) return;
            auto loc = edit::locate(*s, h.clip);
            bool wantVideo = info->category == EffectCategory::VideoFilter;
            if (!loc || (loc->track.kind == TrackKind::Video) != wantVideo) {
                state_->message(wantVideo ? tr("Drop video effects onto video clips") : tr("Drop audio effects onto audio clips"));
                return;
            }
            Id id = h.clip;
            state_->edit(tr("Add %1").arg(QString::fromStdString(info->displayName)), [id, type](Project& p, Sequence& sq) {
                Clip* c = edit::clipById(sq, id);
                if (!c) return false;
                c->effects.push_back(makeEffect(p, type));
                return true;
            });
            state_->setSelection({id}, false);
            break;
        }
        case EffectCategory::VideoTransition:
        case EffectCategory::AudioTransition: {
            if (!h.clip) return;
            const Clip* c = edit::clipById(*s, h.clip);
            FrameTime f = frameRound(pos.x());
            edit::Edge edge = (f - c->start) < (c->end() - f) ? edit::Edge::In : edit::Edge::Out;
            Id id = h.clip;
            FrameTime dur = FrameTime(std::llround(s->fpsValue()));
            state_->apply(tr("Add Transition"), [=](Project& p, Sequence& sq) { return edit::addTransition(p, sq, id, edge, type, dur); });
            break;
        }
        case EffectCategory::Generator: {
            int vt = row && row->ref.kind == TrackKind::Video ? row->ref.index : state_->targetVideoTrack();
            FrameTime f = snapFrame(frameRound(pos.x()), {});
            FrameTime len = FrameTime(std::llround(5 * s->fpsValue()));
            std::vector<Id> created;
            state_->apply(tr("Add %1").arg(QString::fromStdString(info->displayName)), [&](Project& p, Sequence& sq) {
                Clip c = makeGeneratorClip(p, type, len);
                c.start = f;
                auto r = edit::overwrite(p, sq, {TrackKind::Video, std::min(vt, int(sq.videoTracks.size()) - 1)}, c);
                created = r.created;
                return r;
            });
            if (!created.empty()) state_->setSelection(created);
            break;
        }
        default: break;
    }
}

void TimelineWidget::dropEvent(QDropEvent* e) {
    const QMimeData* m = e->mimeData();
    QPoint pos = e->position().toPoint();
    bool insertMode = e->modifiers() & Qt::ControlModifier;
    ghosts_.clear();
    if (m->hasFormat("application/x-montage-media")) {
        dropMedia(m, pos, insertMode);
    } else if (m->hasFormat("application/x-montage-effect")) {
        dropEffect(QString::fromUtf8(m->data("application/x-montage-effect")), pos);
    } else if (m->hasUrls()) {
        QStringList files;
        for (const QUrl& u : m->urls())
            if (u.isLocalFile()) files << u.toLocalFile();
        QStringList errors;
        auto ids = state_->importFiles(files, &errors);
        if (!errors.isEmpty()) state_->message(errors.join("; "), 8000);
        if (!ids.empty()) {
            QStringList parts;
            for (Id id : ids) parts << QString::number(id);
            QMimeData md;
            md.setData("application/x-montage-media", parts.join(',').toUtf8());
            dropMedia(&md, pos, insertMode);
        }
    }
    snapIndicator_ = -1;
    e->acceptProposedAction();
    viewport()->update();
}

}  // namespace montage
