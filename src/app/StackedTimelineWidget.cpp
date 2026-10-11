#include "StackedTimelineWidget.h"

#include <QApplication>
#include <QComboBox>
#include <QDrag>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

#include "EditorState.h"
#include "Theme.h"

namespace montage {

QMimeData* sequenceClipsMime(Id sequence, const std::vector<Id>& clips) {
    QStringList ids;
    for (Id id : clips) ids << QString::number(id);
    auto* m = new QMimeData;
    m->setData(kSequenceClipsMime, (QString::number(sequence) + ';' + ids.join(',')).toUtf8());
    return m;
}

QMimeData* sequenceRangeMime(Id sequence, FrameTime in, FrameTime out) {
    auto* m = new QMimeData;
    m->setData(kSequenceRangeMime, QStringLiteral("%1;%2;%3").arg(sequence).arg(in).arg(out).toUtf8());
    return m;
}

bool hasSequencePart(const QMimeData* mime) {
    return mime && (mime->hasFormat(kSequenceClipsMime) || mime->hasFormat(kSequenceRangeMime));
}

bool sequencePartFromMime(const Project& p, const QMimeData* mime, Id* sequence, edit::SequenceRange* part) {
    if (!mime) return false;
    const bool clips = mime->hasFormat(kSequenceClipsMime);
    if (!clips && !mime->hasFormat(kSequenceRangeMime)) return false;
    const QStringList f = QString::fromUtf8(mime->data(clips ? kSequenceClipsMime : kSequenceRangeMime)).split(';');
    if (f.size() < (clips ? 2 : 3)) return false;
    const Sequence* s = p.findSequence(f[0].toULongLong());
    if (!s) return false;
    if (clips) {
        std::vector<Id> ids;
        for (const QString& id : f[1].split(',', Qt::SkipEmptyParts)) ids.push_back(id.toULongLong());
        *part = edit::copySequenceClips(*s, ids);
    } else {
        *part = edit::copySequenceRange(*s, f[1].toLongLong(), f[2].toLongLong());
    }
    if (sequence) *sequence = s->id;
    return !part->clips.empty();
}

// ---------------------------------------------------------------------------
// The tracks, drawn to fit the width (Ctrl+wheel zooms, the wheel scrolls).

class StackedTimelineCanvas : public QWidget {
public:
    explicit StackedTimelineCanvas(StackedTimelineWidget* owner) : QWidget(owner), w_(owner) {
        setFocusPolicy(Qt::StrongFocus);
        setMinimumHeight(90);
        setMouseTracking(false);
    }

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent*) override { scrubbing_ = false; drag_ = Drag::None; }
    void wheelEvent(QWheelEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    bool event(QEvent* e) override {
        // With focus here, I, O, X and the arrows mark and move this timeline, not the open sequence.
        if (e->type() == QEvent::ShortcutOverride) {
            auto* k = static_cast<QKeyEvent*>(e);
            static const int keys[] = {Qt::Key_I, Qt::Key_O, Qt::Key_X, Qt::Key_Left, Qt::Key_Right, Qt::Key_Home, Qt::Key_End};
            if (!(k->modifiers() & ~Qt::KeypadModifier) && std::find(std::begin(keys), std::end(keys), k->key()) != std::end(keys)) {
                e->accept();
                return true;
            }
        }
        return QWidget::event(e);
    }

private:
    static constexpr int kRuler = 18, kHeader = 34, kVideoH = 26, kAudioH = 20;
    struct Row {
        TrackRef ref;
        int y = 0, h = 0;
    };
    enum class Drag { None, Clips, Range };

    std::vector<Row> rows() const {
        std::vector<Row> out;
        const Sequence* s = w_->sequence();
        if (!s) return out;
        int y = kRuler;
        for (int i = int(s->videoTracks.size()) - 1; i >= 0; --i, y += kVideoH) out.push_back({{TrackKind::Video, i}, y, kVideoH});
        for (int i = 0; i < int(s->audioTracks.size()); ++i, y += kAudioH) out.push_back({{TrackKind::Audio, i}, y, kAudioH});
        return out;
    }
    FrameTime length() const {
        const Sequence* s = w_->sequence();
        return std::max<FrameTime>(1, s ? s->duration() : 1);
    }
    double perFrame() const { return std::max(1.0, double(width() - kHeader - 4)) / double(length()) * zoom_; }
    double xFor(double f) const { return kHeader + (f - scroll_) * perFrame(); }
    FrameTime frameAt(double x) const {
        return std::clamp<FrameTime>(FrameTime(std::floor(scroll_ + (x - kHeader) / perFrame())), 0, length());
    }
    void clampScroll() {
        const double visible = double(width() - kHeader - 4) / perFrame();
        scroll_ = std::clamp(scroll_, 0.0, std::max(0.0, double(length()) - visible));
    }
    const Clip* clipAt(const QPoint& pos) const {
        const Sequence* s = w_->sequence();
        if (!s) return nullptr;
        for (const Row& r : rows()) {
            if (pos.y() < r.y || pos.y() >= r.y + r.h) continue;
            const auto& tracks = r.ref.kind == TrackKind::Video ? s->videoTracks : s->audioTracks;
            const FrameTime f = frameAt(pos.x());
            for (const Clip& c : tracks[size_t(r.ref.index)].clips)
                if (f >= c.start && f < c.end()) return &c;
        }
        return nullptr;
    }

    StackedTimelineWidget* w_;
    double zoom_ = 1, scroll_ = 0;
    bool scrubbing_ = false;
    Drag drag_ = Drag::None;
    QPoint pressPos_;
};

void StackedTimelineCanvas::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), theme::kPanel);
    const Sequence* s = w_->sequence();
    if (!s) {
        p.setPen(theme::kTextDim);
        p.drawText(rect(), Qt::AlignCenter, tr("Make another sequence to stack it here"));
        return;
    }
    const Project& proj = w_->state_->project();
    // Ruler: a tick about every 80 px, labelled with timecode.
    p.fillRect(QRect(0, 0, width(), kRuler), theme::kPanelAlt);
    const double fps = std::max(1.0, s->fpsValue());
    FrameTime step = 1;
    for (double seconds : {1.0 / fps, 5.0 / fps, 10.0 / fps, 1.0, 5.0, 10.0, 30.0, 60.0, 300.0, 600.0}) {
        step = std::max<FrameTime>(1, FrameTime(std::llround(seconds * fps)));
        if (step * perFrame() >= 80) break;
    }
    QFont small = font();
    small.setPointSizeF(small.pointSizeF() * 0.8);
    p.setFont(small);
    for (FrameTime f = FrameTime(scroll_) / step * step; xFor(double(f)) < width(); f += step) {
        const int x = int(xFor(double(f)));
        if (x < kHeader) continue;
        p.setPen(theme::kBorder);
        p.drawLine(x, kRuler - 6, x, kRuler);
        p.setPen(theme::kTextDim);
        p.drawText(QRect(x + 2, 0, 90, kRuler - 2), Qt::AlignLeft | Qt::AlignVCenter, timecodeString(s, f));
    }
    // The marked range, over the ruler and the tracks.
    if (w_->in_ >= 0 || w_->out_ >= 0) {
        const double a = xFor(double(std::max<FrameTime>(0, w_->in_)));
        const double b = xFor(double(w_->out_ >= 0 ? w_->out_ + 1 : length()));
        p.fillRect(QRectF(a, 0, b - a, height()), theme::kInOut);
    }
    for (const Row& r : rows()) {
        p.setPen(theme::kBorder);
        p.drawLine(0, r.y + r.h - 1, width(), r.y + r.h - 1);
        p.setPen(theme::kTextDim);
        p.drawText(QRect(4, r.y, kHeader - 6, r.h), Qt::AlignVCenter,
                   (r.ref.kind == TrackKind::Video ? QStringLiteral("V%1") : QStringLiteral("A%1")).arg(r.ref.index + 1));
        const auto& tracks = r.ref.kind == TrackKind::Video ? s->videoTracks : s->audioTracks;
        p.save();
        p.setClipRect(QRect(kHeader, r.y, width() - kHeader, r.h));
        for (const Clip& c : tracks[size_t(r.ref.index)].clips) {
            const QRectF box(xFor(double(c.start)), r.y + 2, std::max(1.0, double(c.duration) * perFrame()), r.h - 5);
            if (box.right() < kHeader || box.left() > width()) continue;
            QColor col = theme::labelColor(c.colorLabel);
            if (!col.isValid()) {
                const MediaItem* m = proj.findMedia(c.mediaId);
                col = !c.enabled                                    ? theme::kDisabledClip
                      : c.isGenerator()                             ? theme::kGeneratorClip
                      : m && m->kind == MediaKind::Sequence         ? theme::kCompoundClip
                      : r.ref.kind == TrackKind::Video              ? theme::kVideoClip
                                                                    : theme::kAudioClip;
            }
            p.fillRect(box, col);
            const bool selected = std::find(w_->selection_.begin(), w_->selection_.end(), c.id) != w_->selection_.end();
            p.setPen(selected ? QPen(theme::kSelection, 2) : QPen(col.darker(150)));
            p.drawRect(box.adjusted(0.5, 0.5, -0.5, -0.5));
            p.setPen(theme::kText);
            p.drawText(box.adjusted(3, 0, -2, 0), Qt::AlignVCenter | Qt::AlignLeft,
                       p.fontMetrics().elidedText(QString::fromStdString(c.name), Qt::ElideRight, int(box.width()) - 5));
        }
        p.restore();
    }
    const int x = int(xFor(double(w_->playhead_)));
    if (x >= kHeader) {
        p.setPen(QPen(theme::kPlayhead, 1));
        p.drawLine(x, 0, x, height());
    }
}

void StackedTimelineCanvas::mousePressEvent(QMouseEvent* e) {
    setFocus();
    pressPos_ = e->position().toPoint();
    drag_ = Drag::None;
    if (e->button() != Qt::LeftButton) return;
    if (pressPos_.y() < kRuler) {
        scrubbing_ = true;
        w_->setPlayhead(frameAt(pressPos_.x()));
        return;
    }
    std::vector<Id> sel = w_->selection_;
    if (const Clip* c = clipAt(pressPos_)) {
        auto it = std::find(sel.begin(), sel.end(), c->id);
        if (e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier)) {
            if (it != sel.end()) sel.erase(it);
            else sel.push_back(c->id);
        } else if (it == sel.end()) {
            sel = {c->id};
        }
        w_->setSelection(sel);
        if (!w_->selection_.empty()) drag_ = Drag::Clips;
        return;
    }
    w_->setSelection({});
    const FrameTime f = frameAt(pressPos_.x());
    if ((w_->in_ >= 0 || w_->out_ >= 0) && f >= std::max<FrameTime>(0, w_->in_) && (w_->out_ < 0 || f <= w_->out_)) drag_ = Drag::Range;
}

void StackedTimelineCanvas::mouseMoveEvent(QMouseEvent* e) {
    const QPoint pos = e->position().toPoint();
    if (scrubbing_) {
        w_->setPlayhead(frameAt(pos.x()));
        return;
    }
    if (drag_ == Drag::None || (pos - pressPos_).manhattanLength() < QApplication::startDragDistance()) return;
    drag_ = Drag::None;
    if (QMimeData* data = w_->dragData()) {
        auto* d = new QDrag(this);
        d->setMimeData(data);
        d->exec(Qt::CopyAction);
    }
}

void StackedTimelineCanvas::wheelEvent(QWheelEvent* e) {
    const double steps = e->angleDelta().y() / 120.0;
    if (e->modifiers() & Qt::ControlModifier) {
        const double at = scroll_ + (e->position().x() - kHeader) / perFrame();  // keep the frame under the pointer
        zoom_ = std::clamp(zoom_ * std::pow(1.25, steps), 1.0, 400.0);
        scroll_ = at - (e->position().x() - kHeader) / perFrame();
    } else {
        scroll_ -= steps * 60 / perFrame();
    }
    clampScroll();
    update();
    e->accept();
}

void StackedTimelineCanvas::keyPressEvent(QKeyEvent* e) {
    const FrameTime t = w_->playhead_;
    switch (e->key()) {
    case Qt::Key_I: w_->setMarks(t, w_->out_ >= 0 && w_->out_ < t ? -1 : w_->out_); break;
    case Qt::Key_O: w_->setMarks(w_->in_ > t ? -1 : w_->in_, t); break;
    case Qt::Key_X: w_->setMarks(-1, -1); break;
    case Qt::Key_Left: w_->setPlayhead(t - 1); break;
    case Qt::Key_Right: w_->setPlayhead(t + 1); break;
    case Qt::Key_Home: w_->setPlayhead(0); break;
    case Qt::Key_End: w_->setPlayhead(length()); break;
    default: QWidget::keyPressEvent(e); return;
    }
    e->accept();
}

// ---------------------------------------------------------------------------

StackedTimelineWidget::StackedTimelineWidget(EditorState* state, QWidget* parent) : QWidget(parent), state_(state) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(4, 4, 4, 4);
    lay->setSpacing(4);
    auto* bar = new QHBoxLayout;
    picker_ = new QComboBox(this);
    picker_->setObjectName(QStringLiteral("stackedSequence"));
    picker_->setToolTip(tr("The sequence to take clips from"));
    swap_ = new QPushButton(tr("Swap"), this);
    swap_->setObjectName(QStringLiteral("stackedSwap"));
    swap_->setToolTip(tr("Edit this sequence, and stack the one open now"));
    info_ = new QLabel(this);
    info_->setObjectName(QStringLiteral("stackedInfo"));
    info_->setStyleSheet(QStringLiteral("color: %1").arg(theme::kTextDim.name()));
    bar->addWidget(picker_, 1);
    bar->addWidget(info_);
    bar->addWidget(swap_);
    lay->addLayout(bar);
    canvas_ = new StackedTimelineCanvas(this);
    canvas_->setObjectName(QStringLiteral("stackedCanvas"));
    canvas_->setToolTip(tr("Drag selected clips, or the range marked with I and O, onto the timeline (Ctrl inserts)"));
    lay->addWidget(canvas_, 1);
    connect(picker_, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (!filling_ && i >= 0) setSequence(picker_->itemData(i).toULongLong());
    });
    connect(swap_, &QPushButton::clicked, this, [this] { swap(); });
    connect(state_, &EditorState::projectChanged, this, &StackedTimelineWidget::refresh);
    connect(state_, &EditorState::sequenceSwitched, this, &StackedTimelineWidget::refresh);
    refresh();
}

QWidget* StackedTimelineWidget::canvas() const { return canvas_; }

const Sequence* StackedTimelineWidget::sequence() const { return seq_ ? state_->project().findSequence(seq_) : nullptr; }

void StackedTimelineWidget::setSequence(Id id) {
    const Project& p = state_->project();
    const Sequence* s = id && id != p.activeSequence ? p.findSequence(id) : nullptr;
    if (!s)
        for (const Sequence& q : p.sequences)
            if (q.id != p.activeSequence) {
                s = &q;
                break;
            }
    const Id next = s ? s->id : 0;
    if (next != seq_) {
        seq_ = next;
        selection_.clear();
        playhead_ = 0;
        in_ = out_ = -1;
    }
    refresh();
}

void StackedTimelineWidget::refresh() {
    const Project& p = state_->project();
    const Sequence* s = sequence();
    if (!s || seq_ == p.activeSequence) {
        // Gone, or now the active one: show another.
        seq_ = 0;
        for (const Sequence& q : p.sequences)
            if (q.id != p.activeSequence) {
                seq_ = q.id;
                break;
            }
        selection_.clear();
        playhead_ = 0;
        in_ = out_ = -1;
        s = sequence();
    }
    filling_ = true;
    picker_->clear();
    for (const Sequence& q : p.sequences)
        if (q.id != p.activeSequence) picker_->addItem(QString::fromStdString(q.name), QVariant::fromValue<qulonglong>(q.id));
    picker_->setCurrentIndex(picker_->findData(QVariant::fromValue<qulonglong>(seq_)));
    filling_ = false;
    if (s) {
        std::erase_if(selection_, [&](Id id) { return !edit::clipById(*s, id); });
        playhead_ = std::clamp<FrameTime>(playhead_, 0, s->duration());
    }
    swap_->setEnabled(s != nullptr);
    updateInfo();
    canvas_->update();
    emit changed();
}

void StackedTimelineWidget::updateInfo() {
    const Sequence* s = sequence();
    if (!s) {
        info_->clear();
        return;
    }
    if (!selection_.empty()) info_->setText(tr("%n clip(s) selected", nullptr, int(selection_.size())));
    else if (in_ >= 0 || out_ >= 0)
        info_->setText(tr("%1 – %2").arg(timecodeString(s, std::max<FrameTime>(0, in_)), timecodeString(s, out_ >= 0 ? out_ + 1 : s->duration())));
    else info_->setText(timecodeString(s, playhead_));
}

void StackedTimelineWidget::setSelection(const std::vector<Id>& clips) {
    const Sequence* s = sequence();
    selection_.clear();
    if (s)
        for (Id id : clips)
            if (edit::clipById(*s, id) && std::find(selection_.begin(), selection_.end(), id) == selection_.end()) selection_.push_back(id);
    updateInfo();
    canvas_->update();
    emit changed();
}

void StackedTimelineWidget::setPlayhead(FrameTime t) {
    const Sequence* s = sequence();
    playhead_ = std::clamp<FrameTime>(t, 0, s ? s->duration() : 0);
    updateInfo();
    canvas_->update();
    emit changed();
}

void StackedTimelineWidget::setMarks(FrameTime in, FrameTime out) {
    in_ = in < 0 ? -1 : in;
    out_ = out < 0 ? -1 : out;
    if (in_ >= 0 && out_ >= 0 && out_ < in_) std::swap(in_, out_);
    updateInfo();
    canvas_->update();
    emit changed();
}

QMimeData* StackedTimelineWidget::dragData() const {
    if (!sequence()) return nullptr;
    if (!selection_.empty()) return sequenceClipsMime(seq_, selection_);
    if (in_ >= 0 || out_ >= 0) return sequenceRangeMime(seq_, std::max<FrameTime>(0, in_), out_ >= 0 ? out_ + 1 : -1);
    return nullptr;
}

bool StackedTimelineWidget::swap() {
    const Id shown = seq_, open = state_->project().activeSequence;
    if (!shown || !open) return false;
    state_->setActiveSequence(shown);
    setSequence(open);
    return state_->project().activeSequence == shown;
}

}  // namespace montage
