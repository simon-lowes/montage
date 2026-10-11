#include "PanFollowDialog.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <algorithm>
#include <cmath>
#include <functional>

#include "EditorState.h"
#include "core/EditOps.h"
#include "core/Surround.h"
#include "render/Compositor.h"

namespace montage {

namespace {
const double kSizes[3] = {0.1, 0.2, 0.35};  // fractions of the frame height
}  // namespace

// The program frame where tracking starts, with the point to follow and the square around it; a click moves the point.
class PanFollowPicker : public QWidget {
public:
    explicit PanFollowPicker(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumSize(480, 270);
        setCursor(Qt::CrossCursor);
    }
    void setFrame(QImage img) {
        frame_ = std::move(img);
        update();
    }
    void setPoint(double x, double y, double size) {
        x_ = x;
        y_ = y;
        size_ = size;
        update();
    }
    std::function<void(double, double)> picked;

protected:
    QRectF picture() const {
        if (frame_.isNull()) return rect();
        const QSizeF fit = QSizeF(frame_.size()).scaled(size(), Qt::KeepAspectRatio);
        return QRectF(QPointF((width() - fit.width()) / 2, (height() - fit.height()) / 2), fit);
    }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), Qt::black);
        const QRectF r = picture();
        if (!frame_.isNull()) p.drawImage(r, frame_);
        const QPointF c(r.left() + x_ * r.width(), r.top() + y_ * r.height());
        const double half = size_ * r.height() / 2;
        p.setRenderHint(QPainter::Antialiasing);
        for (const auto& [colour, w] : {std::pair{QColor(0, 0, 0, 160), 3.0}, std::pair{QColor(255, 220, 60), 1.5}}) {
            p.setPen(QPen(colour, w));
            p.drawRect(QRectF(c.x() - half, c.y() - half, 2 * half, 2 * half));
            p.drawLine(QPointF(c.x() - 8, c.y()), QPointF(c.x() + 8, c.y()));
            p.drawLine(QPointF(c.x(), c.y() - 8), QPointF(c.x(), c.y() + 8));
        }
    }
    void mousePressEvent(QMouseEvent* e) override {
        const QRectF r = picture();
        if (r.width() <= 0 || r.height() <= 0 || !picked) return;
        picked(std::clamp((e->position().x() - r.left()) / r.width(), 0.0, 1.0),
               std::clamp((e->position().y() - r.top()) / r.height(), 0.0, 1.0));
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (e->buttons() & Qt::LeftButton) mousePressEvent(e);
    }

private:
    QImage frame_;
    double x_ = 0.5, y_ = 0.5, size_ = 0.2;
};

PanFollowDialog::PanFollowDialog(EditorState* state, Id audioClip, QWidget* parent)
    : QDialog(parent), state_(state), clip_(audioClip) {
    setWindowTitle(tr("Pan to Follow"));
    auto* v = new QVBoxLayout(this);
    auto* intro = new QLabel(tr("Click what the sound comes from. It is followed through the shot both ways, and the "
                                "track's panning is written to move with it."));
    intro->setWordWrap(true);
    v->addWidget(intro);
    picker_ = new PanFollowPicker;
    picker_->setObjectName(QStringLiteral("panFollowPicker"));
    v->addWidget(picker_, 1);
    auto* opts = new QHBoxLayout;
    opts->addWidget(new QLabel(tr("Follow:")));
    size_ = new QComboBox;
    size_->setObjectName(QStringLiteral("panFollowSize"));
    size_->addItems({tr("Small Area"), tr("Medium Area"), tr("Large Area")});
    size_->setCurrentIndex(1);
    opts->addWidget(size_);
    opts->addSpacing(12);
    opts->addWidget(new QLabel(tr("Picture width on the stage:")));
    width_ = new QSpinBox;
    width_->setObjectName(QStringLiteral("panFollowWidth"));
    width_->setRange(10, 100);
    width_->setSuffix(QStringLiteral("%"));
    width_->setValue(100);
    width_->setToolTip(tr("100%: the picture's edges are hard left and right (the front left and right speakers in "
                          "surround). A 360° sequence's picture is always the whole circle."));
    opts->addWidget(width_);
    opts->addStretch(1);
    v->addLayout(opts);
    auto* row = new QHBoxLayout;
    info_ = new QLabel;
    info_->setWordWrap(true);
    row->addWidget(info_, 1);
    progress_ = new QProgressBar;
    progress_->setRange(0, 1000);
    progress_->setVisible(false);
    row->addWidget(progress_);
    v->addLayout(row);
    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    track_ = new QPushButton(tr("Track and Pan"));
    track_->setObjectName(QStringLiteral("panFollowTrack"));
    track_->setDefault(true);
    auto* close = new QPushButton(tr("Close"));
    buttons->addWidget(track_);
    buttons->addWidget(close);
    v->addLayout(buttons);

    picker_->picked = [this](double x, double y) { setPoint(x, y); };
    connect(size_, &QComboBox::currentIndexChanged, this, [this] { setPoint(x_, y_); });
    connect(track_, &QPushButton::clicked, this, [this] {
        if (running_) stop();
        else track();
    });
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    connect(&watcher_, &QFutureWatcher<bool>::finished, this, [this] { finish(); });
    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this] {
        if (running_) progress_->setValue(int(std::lround(fraction_->load() * 1000)));
    });
    timer->start(100);
    // Another sequence or project: whatever is being followed is no longer there to write to.
    connect(state_, &EditorState::sequenceSwitched, this, [this] {
        closed_ = true;
        cancel_->store(true);
        QDialog::close();
    });
    // Edits meanwhile: the picture and the frame tracking starts at are brought up to date (shortly after, once).
    refresh_ = new QTimer(this);
    refresh_->setSingleShot(true);
    refresh_->setInterval(250);
    connect(refresh_, &QTimer::timeout, this, [this] {
        if (running_)
            refreshLater_ = true;  // (once it is done)
        else
            refreshFrame(false);
    });
    connect(state_, &EditorState::projectChanged, refresh_, qOverload<>(&QTimer::start));
    refreshFrame(true);
}

PanFollowDialog::~PanFollowDialog() {
    cancel_->store(true);
    watcher_.waitForFinished();
}

void PanFollowDialog::closeEvent(QCloseEvent* e) {
    // Closed while following: stopped, and nothing written afterwards.
    closed_ = true;
    cancel_->store(true);
    QDialog::closeEvent(e);
}

void PanFollowDialog::refreshFrame(bool first) {
    // Tracking starts at the playhead, or the middle of the clip when the playhead is off it.
    const Sequence* s = state_->sequence();
    const Clip* c = s ? edit::clipById(*s, clip_) : nullptr;
    if (!c || c->duration <= 0) {
        info_->setText(tr("The audio clip is no longer in the sequence."));
        track_->setEnabled(false);
        if (first) setPoint(0.5, 0.5);
        return;
    }
    if (first) from_ = state_->playhead();
    if (from_ < c->start || from_ >= c->end()) from_ = c->start + c->duration / 2;
    const bool picture = panFollowSource(state_->project(), *s, *c, from_) != nullptr;
    track_->setEnabled(picture);
    // (Not over what the last attempt said, such as the clips having moved.)
    if ((keys_ == 0 && !outcome_) || !picture)
        info_->setText(!picture ? tr("There is no video on screen with this sound to follow.")
                       : stereoLane() ? tr("Writes the track's pan.")
                                      : tr("Writes the track's surround position."));
    RenderOptions o;
    o.scale = std::min(1.0, 960.0 / std::max(1, s->width));
    o.displaySpace = "rec709";
    const Image view = renderProgramFrame(state_->project(), *s, from_, o);
    if (!view.empty()) {
        QImage img(view.width, view.height, QImage::Format_RGBA8888);
        toRgba8(view, img.bits(), size_t(img.bytesPerLine()));
        picker_->setFrame(img);
    }
    if (!first) return;
    // Starting on the subject of the shot, the likeliest source of the sound.
    double x = 0.5, y = 0.5;
    if (picture) panFollowSubject(state_->project(), *s, *c, from_, x, y);
    setPoint(x, y);
}

bool PanFollowDialog::stereoLane() const {
    const Sequence* s = state_->sequence();
    const auto loc = s ? edit::locate(*s, clip_) : std::nullopt;
    if (!loc || loc->track.kind != TrackKind::Audio) return true;
    return panFollowStereo(*s, *trackAt(*s, loc->track));
}

void PanFollowDialog::setPoint(double x, double y) {
    x_ = std::clamp(x, 0.0, 1.0);
    y_ = std::clamp(y, 0.0, 1.0);
    picker_->setPoint(x_, y_, kSizes[std::clamp(size_->currentIndex(), 0, 2)]);
}

void PanFollowDialog::setSize(int size) { size_->setCurrentIndex(std::clamp(size, 0, 2)); }

void PanFollowDialog::setWidth(double width) { width_->setValue(int(std::lround(std::clamp(width, 0.1, 1.0) * 100))); }

bool PanFollowDialog::track(bool wait) {
    if (running_ || !track_->isEnabled()) return false;
    if (state_->readOnly()) {
        info_->setText(tr("The project is read-only: nothing can be written to it."));
        return false;
    }
    const Sequence* s = state_->sequence();
    const auto loc = s ? edit::locate(*s, clip_) : std::nullopt;
    const Clip* c = s ? edit::clipById(*s, clip_) : nullptr;
    if (!c || !loc) {
        info_->setText(tr("The audio clip is no longer in the sequence."));
        return false;
    }
    if (from_ < c->start || from_ >= c->end()) refreshFrame(false);
    // Tracked on a copy, so editing can go on meanwhile; where things were is noted, to check before writing.
    auto project = std::make_shared<Project>(state_->project());
    seq_ = s->id;
    clipStart_ = c->start;
    clipTrack_ = loc->track;
    const Clip clip = *c;
    const Id seqId = s->id;
    const FrameTime from = from_;
    const double x = x_, y = y_, size = kSizes[std::clamp(size_->currentIndex(), 0, 2)];
    cancel_ = std::make_shared<std::atomic<bool>>(false);
    fraction_->store(0);
    pending_ = std::make_shared<Pending>();
    error_ = std::make_shared<std::string>();
    running_ = true;
    keys_ = 0;
    outcome_ = false;
    track_->setText(tr("Stop"));
    progress_->setValue(0);
    progress_->setVisible(true);
    info_->setText(tr("Following…"));
    auto cancel = cancel_;
    auto fraction = fraction_;
    auto out = pending_;
    auto error = error_;
    watcher_.setFuture(QtConcurrent::run([project, seqId, clip, from, x, y, size, cancel, fraction, out, error] {
        const Sequence* seq = project->findSequence(seqId);
        if (!seq) return false;
        const bool ok = trackPanFollow(*project, *seq, clip, from, x, y, size, out->keys, [&](double f) { fraction->store(f); },
                                       cancel.get(), error.get(), &out->source);
        if (const Clip* src = ok ? edit::clipById(*seq, out->source) : nullptr) out->sourceStart = src->start;
        return ok;
    }));
    if (wait) {
        watcher_.waitForFinished();
        finish();
    }
    return true;
}

void PanFollowDialog::stop() { cancel_->store(true); }

void PanFollowDialog::finish() {
    if (!running_) return;  // (already handled when waited for)
    running_ = false;
    track_->setText(tr("Track and Pan"));
    progress_->setVisible(false);
    if (closed_) return;
    outcome_ = true;
    if (refreshLater_) {
        refreshLater_ = false;
        refresh_->start();
    }
    if (!watcher_.future().result()) {
        info_->setText(cancel_->load() ? tr("Stopped.") : tr("Could not follow it: %1").arg(QString::fromStdString(*error_)));
        return;
    }
    applyPending();
}

void PanFollowDialog::applyPending() {
    if (closed_ || !pending_ || pending_->keys.empty()) return;
    // Not in the middle of a drag or a fader pass: written once that is over.
    if (state_->inGesture()) {
        QTimer::singleShot(150, this, [this] { applyPending(); });
        return;
    }
    const auto pending = std::move(pending_);
    const Sequence* s = state_->sequence();
    if (!s || s->id != seq_) return;
    // The frames tracked belong to where the clips were: if they moved meanwhile, the path would land elsewhere.
    const auto loc = edit::locate(*s, clip_);
    const Clip* c = edit::clipById(*s, clip_);
    const Clip* src = edit::clipById(*s, pending->source);
    if (!c || !loc) {
        info_->setText(tr("The audio clip is no longer in the sequence."));
        return;
    }
    if (c->start != clipStart_ || !(loc->track == clipTrack_) || !src || src->start != pending->sourceStart) {
        info_->setText(tr("The clips moved while it was being followed: track again."));
        return;
    }
    const auto keys = pending->keys;
    const double width = width_->value() / 100.0;
    const Id clip = clip_;
    const bool ok = state_->edit(tr("Pan to Follow"), [keys, width, clip](Project&, Sequence& seq) {
        const auto at = edit::locate(seq, clip);
        if (!at || at->track.kind != TrackKind::Audio) return false;
        return applyPanFollow(seq, *trackAt(seq, at->track), keys, width);
    });
    if (!ok) {
        info_->setText(state_->readOnly() ? tr("The project is read-only: nothing can be written to it.")
                                          : tr("The audio clip is no longer in the sequence."));
        return;
    }
    keys_ = int(keys.size());
    span_ = keys.back().t - keys.front().t + 1;
    info_->setText(status());
}

QString PanFollowDialog::status() const {
    if (running_) return tr("Following…");
    if (keys_ == 0) return info_->text();
    const Sequence* s = state_->sequence();
    const double seconds = s ? double(span_) / s->fpsValue() : 0;
    return tr("Followed for %1 s: the track's panning now moves with it (Read automation).").arg(seconds, 0, 'f', 1);
}

}  // namespace montage
