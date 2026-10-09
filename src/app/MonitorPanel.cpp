#include "MonitorPanel.h"
#include "Settings.h"

#include "ExposureView.h"

#include <QApplication>
#include <QComboBox>
#include <QDrag>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QSettings>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <utility>

#include <QFutureWatcher>
#include <QtConcurrent>

#include "EditorState.h"
#include "PlaybackController.h"
#include "media/Image.h"
#include "media/MediaPool.h"
#include "render/Compositor.h"
#include "render/Spherical.h"
#include "core/EditOps.h"
#include "Theme.h"

namespace montage {

// ---------------------------------------------------------------------------
// ViewerWidget

ViewerWidget::ViewerWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(160, 90);
    setAttribute(Qt::WA_OpaquePaintEvent);
}

void ViewerWidget::setImage(const QImage& img) {
    image_ = img;
    update();
}

void ViewerWidget::setPlaceholder(const QString& text) {
    placeholder_ = text;
    update();
}

void ViewerWidget::setSafeMargins(bool on) {
    safe_ = on;
    update();
}

void ViewerWidget::setExposureView(int mode) {
    exposure_ = std::clamp(mode, 0, 3);
    update();
}

QImage ViewerWidget::shownImage() const {
    if (exposure_ == 0 || image_.isNull()) return image_;
    if (exposedKey_ != image_.cacheKey() || exposedMode_ != exposure_) {
        exposed_ = montage::exposureView(image_, ExposureView(exposure_));
        exposedKey_ = image_.cacheKey();
        exposedMode_ = exposure_;
    }
    return exposed_;
}

QRectF ViewerWidget::twoUpRect(bool right) const {
    // Each half fitted to the picture's shape (the sequence's, from whichever image there is).
    const double gap = 6, labelH = fontMetrics().height() + 8;
    const QRectF half(right ? width() / 2.0 + gap / 2 : 0, 0, width() / 2.0 - gap / 2, std::max(1.0, height() - labelH));
    QSize shape = twoUpImages_[0].isNull() ? twoUpImages_[1].size() : twoUpImages_[0].size();
    if (shape.isEmpty()) shape = image_.isNull() ? QSize(16, 9) : image_.size();
    const QSizeF fs = QSizeF(shape).scaled(half.size(), Qt::KeepAspectRatio);
    return QRectF(half.center().x() - fs.width() / 2, half.center().y() - fs.height() / 2, fs.width(), fs.height());
}

void ViewerWidget::setTwoUp(const QImage& left, const QImage& right, const QString& leftLabel, const QString& rightLabel) {
    twoUp_ = true;
    twoUpImages_[0] = left, twoUpImages_[1] = right;
    twoUpLabels_[0] = leftLabel, twoUpLabels_[1] = rightLabel;
    update();
}

void ViewerWidget::clearTwoUp() {
    twoUp_ = false;
    twoUpImages_[0] = twoUpImages_[1] = QImage();
    update();
}

void ViewerWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(0x0e, 0x0f, 0x11));
    if (twoUp_) {
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        for (int i = 0; i < 2; ++i) {
            const QRectF r = twoUpRect(i == 1);
            if (twoUpImages_[i].isNull()) p.fillRect(r, Qt::black);
            else p.drawImage(r, twoUpImages_[i]);
            p.setPen(QPen(QColor(255, 255, 255, 60), 1));
            p.drawRect(r.adjusted(-0.5, -0.5, 0.5, 0.5));
            p.setPen(QColor(235, 235, 235));
            p.drawText(QRectF(r.left(), r.bottom() + 2, r.width(), fontMetrics().height() + 4), Qt::AlignCenter, twoUpLabels_[i]);
        }
        return;
    }
    if (image_.isNull()) {
        p.setPen(theme::kTextDim);
        p.drawText(rect(), Qt::AlignCenter, placeholder_);
        return;
    }
    const QRectF r = imageRect();
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawImage(r, exposure_ ? shownImage() : image_);
    if (safe_) {
        p.setPen(QPen(QColor(255, 255, 255, 90), 1, Qt::DashLine));
        p.drawRect(r.adjusted(r.width() * 0.05, r.height() * 0.05, -r.width() * 0.05, -r.height() * 0.05));
        p.drawRect(r.adjusted(r.width() * 0.1, r.height() * 0.1, -r.width() * 0.1, -r.height() * 0.1));
        p.drawLine(QPointF(r.center().x() - 10, r.center().y()), QPointF(r.center().x() + 10, r.center().y()));
        p.drawLine(QPointF(r.center().x(), r.center().y() - 10), QPointF(r.center().x(), r.center().y() + 10));
    }
    if (!compare_.isNull()) {
        // The reference fitted to the same area, shown left of the divider.
        const QSizeF fs = QSizeF(compare_.size()).scaled(r.size(), Qt::KeepAspectRatio);
        const QRectF rr(r.center().x() - fs.width() / 2, r.center().y() - fs.height() / 2, fs.width(), fs.height());
        const double x = dividerX();
        p.save();
        p.setClipRect(QRectF(r.left(), r.top(), x - r.left(), r.height()));
        p.fillRect(r, QColor(0x0e, 0x0f, 0x11));
        p.drawImage(rr, compare_);
        p.restore();
        p.setPen(QPen(QColor(255, 255, 255, 200), 1));
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        p.setBrush(QColor(255, 255, 255, 220));
        p.drawEllipse(QPointF(x, r.center().y()), 4, 4);
        QFont f = p.font();
        f.setPointSizeF(std::max(7.0, f.pointSizeF() - 1));
        p.setFont(f);
        const QFontMetrics fm(f);
        auto tag = [&](const QString& text, bool left) {
            const QSizeF ts(fm.horizontalAdvance(text) + 10, fm.height() + 4);
            const QRectF box(left ? r.left() + 6 : r.right() - 6 - ts.width(), r.top() + 6, ts.width(), ts.height());
            p.fillRect(box, QColor(0, 0, 0, 150));
            p.setPen(QColor(235, 235, 235));
            p.drawText(box, Qt::AlignCenter, text);
        };
        tag(compareLabel_.isEmpty() ? tr("Reference") : compareLabel_, true);
        tag(tr("Current"), false);
    }
    for (const auto& paint : overlays_) paint(p, r);
}

void ViewerWidget::setCompare(const QImage& reference, const QString& label) {
    compare_ = reference;
    compareLabel_ = label;
    update();
}

void ViewerWidget::clearCompare() {
    compare_ = QImage();
    draggingSplit_ = false;
    update();
}

void ViewerWidget::setSplit(double s) {
    split_ = std::clamp(s, 0.0, 1.0);
    update();
}

double ViewerWidget::dividerX() const {
    const QRectF r = imageRect();
    return r.left() + r.width() * split_;
}

QRectF ViewerWidget::imageRect() const {
    if (image_.isNull()) return {};
    QSizeF s = QSizeF(image_.size()).scaled(size(), Qt::KeepAspectRatio);
    return QRectF((width() - s.width()) / 2, (height() - s.height()) / 2, s.width(), s.height());
}

void ViewerWidget::setLookAround(bool on) {
    if (lookAround_ == on) return;
    lookAround_ = on;
    if (on) setCursor(Qt::OpenHandCursor);
    else unsetCursor();
}

void ViewerWidget::mousePressEvent(QMouseEvent* e) {
    pressPos_ = e->pos();
    if (lookAround_ && e->button() == Qt::LeftButton && !comparing() && !twoUp_ && imageRect().contains(e->position())) {
        looking_ = true;
        setCursor(Qt::ClosedHandCursor);
        emit lookStarted();
        e->accept();
        return;
    }
    if (comparing() && e->button() == Qt::LeftButton && !image_.isNull() && std::fabs(e->position().x() - dividerX()) <= 6) {
        draggingSplit_ = true;
        setCursor(Qt::SplitHCursor);
        e->accept();
        return;
    }
    QWidget::mousePressEvent(e);
}

void ViewerWidget::mouseReleaseEvent(QMouseEvent* e) {
    if (looking_) {
        looking_ = false;
        setCursor(lookAround_ ? Qt::OpenHandCursor : Qt::ArrowCursor);
        emit lookFinished();
        e->accept();
        return;
    }
    if (draggingSplit_) {
        draggingSplit_ = false;
        unsetCursor();
        e->accept();
        return;
    }
    QWidget::mouseReleaseEvent(e);
}

void ViewerWidget::mouseMoveEvent(QMouseEvent* e) {
    if (looking_) {
        const QRectF r = imageRect();
        if (r.width() > 0 && r.height() > 0)
            emit lookMoved((e->position().x() - pressPos_.x()) / r.width(), (e->position().y() - pressPos_.y()) / r.height());
        return;
    }
    if (draggingSplit_) {
        const QRectF r = imageRect();
        if (r.width() > 0) setSplit((e->position().x() - r.left()) / r.width());
        return;
    }
    if (dragSource_ && (e->buttons() & Qt::LeftButton) &&
        (e->pos() - pressPos_).manhattanLength() > QApplication::startDragDistance())
        emit dragRequested();
}

// ---------------------------------------------------------------------------
// ScrubBar

ScrubBar::ScrubBar(QWidget* parent) : QWidget(parent) {
    setFixedHeight(18);
    setCursor(Qt::PointingHandCursor);
}

void ScrubBar::setRange(FrameTime duration) {
    duration_ = std::max<FrameTime>(1, duration);
    update();
}

void ScrubBar::setPosition(FrameTime t) {
    pos_ = t;
    update();
}

void ScrubBar::setMarks(FrameTime in, FrameTime out) {
    in_ = in;
    out_ = out;
    update();
}

void ScrubBar::setMarkers(std::vector<FrameTime> markers) {
    markers_ = std::move(markers);
    update();
}

void ScrubBar::setWaveform(std::shared_ptr<const Peaks> peaks, double secondsPerFrame) {
    if (peaks && peaks->minmax.empty()) peaks = nullptr;
    if (secondsPerFrame <= 0) secondsPerFrame = 1.0 / 30;
    if (peaks == peaks_ && secondsPerFrame == secondsPerFrame_) return;
    peaks_ = std::move(peaks);
    secondsPerFrame_ = secondsPerFrame;
    setFixedHeight(peaks_ ? 40 : 18);
    update();
}

FrameTime ScrubBar::frameAt(int x) const {
    double u = std::clamp(double(x - 4) / std::max(1, width() - 8), 0.0, 1.0);
    return FrameTime(std::llround(u * double(duration_)));
}

void ScrubBar::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), theme::kPanel);
    int w = width() - 8;
    auto xOf = [&](FrameTime t) { return 4 + int(double(t) / double(duration_) * w); };
    if (peaks_) {
        // Each pixel column shows the loudest swing of the audio under it.
        const double rate = peaks_->sampleRate, perBucket = peaks_->samplesPerBucket;
        const size_t buckets = peaks_->minmax.size() / 2;
        const double mid = height() / 2.0, half = height() / 2.0 - 2;
        p.fillRect(QRect(4, 1, w, height() - 2), theme::kWindow);
        p.setPen(QColor(theme::kAudioClip.red(), theme::kAudioClip.green(), theme::kAudioClip.blue(), 200));
        for (int x = 0; x < w; ++x) {
            double fA = double(x) / w * double(duration_), fB = double(x + 1) / w * double(duration_);
            size_t bA = size_t(fA * secondsPerFrame_ * rate / perBucket);
            size_t bB = std::max(bA + 1, size_t(fB * secondsPerFrame_ * rate / perBucket));
            if (bA >= buckets) break;
            bB = std::min(bB, buckets);
            float lo = 0, hi = 0;
            for (size_t b = bA; b < bB; ++b) {
                lo = std::min(lo, peaks_->minmax[b * 2]);
                hi = std::max(hi, peaks_->minmax[b * 2 + 1]);
            }
            int y1 = int(mid - std::min(1.0f, hi) * half), y2 = int(mid - std::max(-1.0f, lo) * half);
            p.drawLine(4 + x, y1, 4 + x, std::max(y1, y2));
        }
    } else {
        p.fillRect(QRect(4, height() / 2 - 2, w, 4), theme::kBorder);
    }
    if (in_ >= 0 || out_ >= 0) {
        int a = xOf(std::max<FrameTime>(0, in_)), b = xOf(out_ >= 0 ? out_ : duration_);
        p.fillRect(QRect(a, 2, std::max(1, b - a), height() - 4), QColor(61, 139, 255, 80));
    }
    p.setPen(theme::kSnap);
    for (FrameTime m : markers_) p.drawLine(xOf(m), 2, xOf(m), 7);
    int x = xOf(pos_);
    p.setPen(QPen(theme::kPlayhead, 2));
    p.drawLine(x, 1, x, height() - 1);
}

void ScrubBar::mousePressEvent(QMouseEvent* e) { emit seekRequested(frameAt(e->pos().x())); }

void ScrubBar::mouseMoveEvent(QMouseEvent* e) {
    if (e->buttons() & Qt::LeftButton) emit seekRequested(frameAt(e->pos().x()));
}

// ---------------------------------------------------------------------------
// MonitorPanel

namespace {
QToolButton* tool(QWidget* parent, const QString& text, const QString& tip, const char* icon = nullptr) {
    auto* b = new QToolButton(parent);
    if (icon) b->setIcon(theme::icon(icon));
    else b->setText(text);
    b->setToolTip(tip);
    b->setAutoRaise(true);
    return b;
}
}  // namespace

MonitorPanel::MonitorPanel(Mode mode, EditorState* state, PlaybackController* controller, QWidget* parent)
    : QWidget(parent), mode_(mode), state_(state), controller_(controller) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(2, 2, 2, 2);
    lay->setSpacing(2);
    viewer_ = new ViewerWidget(this);
    viewer_->setPlaceholder(mode == Mode::Source ? tr("Double-click a clip in the Media bin to view it here")
                                                 : tr("Program"));
    lay->addWidget(viewer_, 1);
    scrub_ = new ScrubBar(this);
    lay->addWidget(scrub_);

    auto* bar = new QHBoxLayout;
    bar->setSpacing(1);
    timecode_ = new QLineEdit(this);
    timecode_->setFont(theme::monoFont(10));
    timecode_->setFixedWidth(100);
    timecode_->setAlignment(Qt::AlignCenter);
    timecode_->setToolTip(tr("Current time — type a timecode, +frames or seconds (e.g. 2.5s) and press Enter"));
    timecode_->setStyleSheet(QString("color: %1; background: transparent; border: none;").arg(theme::kAccent.name()));
    bar->addWidget(timecode_);
    bar->addStretch();

    auto* inBtn = tool(this, QString(), tr("Mark In (I)"), "mark-in");
    auto* outBtn = tool(this, QString(), tr("Mark Out (O)"), "mark-out");
    auto* goIn = tool(this, QString(), tr("Go to In (Shift+I)"), "to-in");
    auto* back = tool(this, QString(), tr("Step Back (Left)"), "step-back");
    playButton_ = tool(this, QString(), tr("Play / Pause (Space)"), "play");
    auto* fwd = tool(this, QString(), tr("Step Forward (Right)"), "step-forward");
    auto* goOut = tool(this, QString(), tr("Go to Out (Shift+O)"), "to-out");
    for (QToolButton* b : {inBtn, outBtn, goIn, back, playButton_, fwd, goOut}) bar->addWidget(b);
    connect(inBtn, &QToolButton::clicked, this, &MonitorPanel::markIn);
    connect(outBtn, &QToolButton::clicked, this, &MonitorPanel::markOut);
    connect(goIn, &QToolButton::clicked, this, &MonitorPanel::goToIn);
    connect(goOut, &QToolButton::clicked, this, &MonitorPanel::goToOut);
    connect(back, &QToolButton::clicked, controller_, [this] { controller_->step(-1); });
    connect(fwd, &QToolButton::clicked, controller_, [this] { controller_->step(1); });
    connect(playButton_, &QToolButton::clicked, controller_, &PlaybackController::togglePlay);

    if (mode == Mode::Source) {
        auto* ins = tool(this, tr("Insert"), tr("Insert into the timeline at the playhead (,)"));
        auto* ovr = tool(this, tr("Overwrite"), tr("Overwrite onto the timeline at the playhead (.)"));
        bar->addSpacing(8);
        bar->addWidget(ins);
        bar->addWidget(ovr);
        connect(ins, &QToolButton::clicked, this, [this] { state_->insertFromSource(false); });
        connect(ovr, &QToolButton::clicked, this, [this] { state_->insertFromSource(true); });
        viewer_->setDragSource(true);
        connect(viewer_, &ViewerWidget::dragRequested, this, [this] {
            if (!state_->sourceMedia()) return;
            auto* mime = new QMimeData;
            mime->setData("application/x-montage-media",
                          QString("%1:%2:%3").arg(state_->sourceMedia()).arg(state_->sourceIn()).arg(state_->sourceOut()).toUtf8());
            auto* drag = new QDrag(this);
            drag->setMimeData(mime);
            if (!viewer_->image().isNull())
                drag->setPixmap(QPixmap::fromImage(viewer_->image().scaledToWidth(120, Qt::SmoothTransformation)));
            drag->exec(Qt::CopyAction);
        });
    } else {
        auto* lift = tool(this, tr("Lift"), tr("Lift In–Out from targeted tracks (;)"));
        auto* extract = tool(this, tr("Extract"), tr("Extract In–Out and close the gap (')"));
        auto* safe = tool(this, tr("Safe"), tr("Show title / action safe guides"));
        safe->setCheckable(true);
        auto* loop = tool(this, tr("Loop"), tr("Loop playback between In and Out"));
        loop->setCheckable(true);
        auto* still = tool(this, QString(), tr("Export Frame..."), "save");
        auto* proxy = tool(this, tr("Proxy"), tr("Play back with proxy media where available (exports always use originals)"));
        proxy->setCheckable(true);
        auto* cc = tool(this, tr("CC"), tr("Show captions (the first visible caption track)"));
        cc->setCheckable(true);
        cc->setObjectName("showCaptions");
        bar->addSpacing(8);
        for (QToolButton* b : {lift, extract, safe, loop, proxy, cc, still}) bar->addWidget(b);
        connect(proxy, &QToolButton::toggled, this, [this](bool on) { controller_->setUseProxies(on); });
        connect(cc, &QToolButton::toggled, this, [this](bool on) {
            controller_->setShowCaptions(on);
            appSettings().setValue("program/showCaptions", on);
        });
        cc->setChecked(appSettings().value("program/showCaptions", true).toBool());
        connect(lift, &QToolButton::clicked, this, [this] {
            const Sequence* s = state_->sequence();
            if (!s) return;
            FrameTime a = s->inPoint, b = s->outPoint >= 0 ? s->outPoint + 1 : -1;
            state_->apply(tr("Lift"), [a, b](Project& p, Sequence& sq) { return edit::liftRange(p, sq, a, b, allTracks(sq)); });
        });
        connect(extract, &QToolButton::clicked, this, [this] {
            const Sequence* s = state_->sequence();
            if (!s) return;
            FrameTime a = s->inPoint, b = s->outPoint >= 0 ? s->outPoint + 1 : -1;
            state_->apply(tr("Extract"), [a, b](Project& p, Sequence& sq) { return edit::extractRange(p, sq, a, b, allTracks(sq)); });
        });
        connect(safe, &QToolButton::toggled, viewer_, &ViewerWidget::setSafeMargins);
        connect(loop, &QToolButton::toggled, this, [this](bool on) { controller_->setLoop(on); });
        connect(still, &QToolButton::clicked, this, &MonitorPanel::exportFrameRequested);
        resolution_ = new QComboBox(this);
        resolution_->addItem(tr("Full"), 1.0);
        resolution_->addItem(tr("1/2"), 0.5);
        resolution_->addItem(tr("1/4"), 0.25);
        resolution_->addItem(tr("1/8"), 0.125);
        resolution_->setCurrentIndex(1);
        resolution_->setToolTip(tr("Playback resolution (paused frames always render at full quality)"));
        connect(resolution_, &QComboBox::currentIndexChanged, this,
                [this] { controller_->setPreviewScale(resolution_->currentData().toDouble()); });
        bar->addWidget(resolution_);
    }
    // Exposure checks on either monitor.
    exposure_ = new QComboBox(this);
    exposure_->setObjectName(QStringLiteral("exposureView"));
    exposure_->addItem(tr("Exposure: Off"));
    exposure_->addItem(tr("Zebras 100 %"));
    exposure_->addItem(tr("Zebras 70 %"));
    exposure_->addItem(tr("False Colour"));
    exposure_->setToolTip(tr("Zebra stripes over clipping (or skin-tone exposure), or false colour by brightness: purple crushed, "
                             "blue near black, green 18 % grey, pink a stop over, yellow near clipping, red clipped"));
    connect(exposure_, &QComboBox::currentIndexChanged, viewer_, &ViewerWidget::setExposureView);
    bar->addWidget(exposure_);
    bar->addStretch();
    durationLabel_ = new QLabel(this);
    durationLabel_->setFont(theme::monoFont(9));
    durationLabel_->setStyleSheet(QString("color: %1;").arg(theme::kTextDim.name()));
    bar->addWidget(durationLabel_);
    lay->addLayout(bar);

    connect(scrub_, &ScrubBar::seekRequested, controller_, &PlaybackController::seek);
    connect(controller_, &PlaybackController::frameRendered, viewer_, [this](const QImage& img, FrameTime) {
        if (mode_ == Mode::Source && !state_->sourceMedia()) return;
        viewer_->setImage(img);
    });
    connect(controller_, &PlaybackController::positionChanged, this, [this] { refresh(); });
    connect(controller_, &PlaybackController::playingChanged, this, [this](bool playing) {
        playButton_->setIcon(theme::icon(playing ? "pause" : "play"));
    });
    connect(timecode_, &QLineEdit::returnPressed, this, [this] {
        const Sequence* s = controller_->sequence();
        FrameTime t = 0;
        QString text = timecode_->text().trimmed();
        bool relative = text.startsWith('+') || text.startsWith('-');
        if (s && parseTimecode(text.toStdString(), s->fps, t)) controller_->seek(relative ? controller_->position() + t : t);
        refresh();
        viewer_->setFocus();
    });
    connect(state_, &EditorState::projectChanged, this, &MonitorPanel::refresh);
    connect(state_, &EditorState::sourceChanged, this, &MonitorPanel::refresh);
    if (mode_ == Mode::Source) connect(state_, &EditorState::mediaReady, this, [this] { refresh(); });
    if (mode_ == Mode::Program) {
        // Look around: dragging the picture of a selected Reframe 360° clip pans and tilts its view, one undo step a
        // drag; when the view is keyed, the drag keys it at the playhead.
        connect(state_, &EditorState::selectionChanged, this, &MonitorPanel::updateLookAround);
        connect(state_, &EditorState::projectChanged, this, &MonitorPanel::updateLookAround);
        connect(controller_, &PlaybackController::positionChanged, this, &MonitorPanel::updateLookAround);
        connect(viewer_, &ViewerWidget::lookStarted, this, [this] {
            const Sequence* s = state_->sequence();
            const Clip* c = s ? edit::clipById(*s, lookClip_) : nullptr;
            if (!c) return;
            lookAt_ = std::clamp<FrameTime>(controller_->position() - c->start, 0, c->duration - 1);
            for (const Effect& e : c->effects)
                if (e.type == "reframe_360") {
                    lookYaw_ = e.p("yaw", lookAt_), lookPitch_ = e.p("pitch", lookAt_), lookFov_ = e.p("fov", lookAt_, 100);
                    if (e.p("projection", lookAt_) > 0.5) lookFov_ = std::min(lookFov_, 120.0);  // a planet turns slower
                }
            state_->beginGesture(tr("Look Around"));
        });
        connect(viewer_, &ViewerWidget::lookMoved, this, [this](double dx, double dy) {
            if (!state_->inGesture()) return;
            const Sequence* s = state_->sequence();
            const double aspect = s && s->width > 0 ? double(s->height) / s->width : 9.0 / 16;
            // Grabbing the scene: dragging right turns the view left, dragging down tilts it up.
            double yaw = lookYaw_ - dx * lookFov_, pitch = std::clamp(lookPitch_ + dy * lookFov_ * aspect, -90.0, 90.0);
            yaw -= 360 * std::floor((yaw + 180) / 360);
            const Id clip = lookClip_;
            const FrameTime at = lookAt_;
            state_->updateGesture([clip, at, yaw, pitch](Project& p, Sequence& seq) {
                Clip* c = edit::clipById(seq, clip);
                if (!c) return;
                bool keyed = false;
                for (const Effect& e : c->effects)
                    if (e.type == "reframe_360")
                        keyed = (e.params.count("yaw") && e.params.at("yaw").animated()) || (e.params.count("pitch") && e.params.at("pitch").animated());
                edit::setReframe360(p, seq, clip, {yaw, pitch, std::nullopt, std::nullopt, std::nullopt}, keyed ? at : -1);
            });
        });
        connect(viewer_, &ViewerWidget::lookFinished, this, [this] {
            if (state_->inGesture()) state_->endGesture(true);
        });
    }
    refresh();
}

void MonitorPanel::updateLookAround() {
    lookClip_ = 0;
    const Sequence* s = state_->sequence();
    const FrameTime t = controller_->position();
    if (s)
        for (Id id : state_->selectedClips())
            if (const Clip* c = edit::clipById(*s, id); c && c->contains(t))
                for (const Effect& e : c->effects)
                    if (e.type == "reframe_360" && e.enabled) lookClip_ = id;
    if (!state_->inGesture() || !lookClip_) viewer_->setLookAround(lookClip_ != 0);
}

FrameTime MonitorPanel::duration() const {
    const Sequence* s = controller_->sequence();
    if (!s) return 0;
    return s->duration();
}

FrameTime MonitorPanel::inPoint() const {
    if (mode_ == Mode::Source) return state_->sourceIn();
    const Sequence* s = state_->sequence();
    return s ? s->inPoint : -1;
}

FrameTime MonitorPanel::outPoint() const {
    if (mode_ == Mode::Source) return state_->sourceOut();
    const Sequence* s = state_->sequence();
    return s ? s->outPoint : -1;
}

void MonitorPanel::showTrimView(FrameTime left, FrameTime right, const QString& leftLabel, const QString& rightLabel) {
    trimView_ = true;
    trimFrames_[0] = left, trimFrames_[1] = right;
    trimLabels_[0] = leftLabel, trimLabels_[1] = rightLabel;
    if (!viewer_->twoUp()) viewer_->setTwoUp({}, {}, leftLabel, rightLabel);
    if (trimBusy_) trimPending_ = true;
    else renderTrimView();
}

void MonitorPanel::endTrimView() {
    trimView_ = trimPending_ = false;
    viewer_->clearTwoUp();
}

void MonitorPanel::renderTrimView() {
    const std::shared_ptr<const Project> project = controller_->snapshot();
    const Sequence* s = controller_->sequence();
    if (!project || !s) return;
    // Each half at about the size it is shown.
    const double scale = std::clamp(viewer_->width() * viewer_->devicePixelRatioF() / 2.0 / std::max(1, s->width), 0.1, 1.0);
    const Id seq = s->id;
    const bool proxies = controller_->useProxies();
    const FrameTime frames[2] = {trimFrames_[0], trimFrames_[1]};
    const QString labels[2] = {trimLabels_[0], trimLabels_[1]};
    trimBusy_ = true;
    auto* watcher = new QFutureWatcher<std::pair<QImage, QImage>>(this);
    connect(watcher, &QFutureWatcher<std::pair<QImage, QImage>>::finished, this, [this, watcher, labels] {
        const auto images = watcher->result();
        watcher->deleteLater();
        trimBusy_ = false;
        if (!trimView_) return;
        if (trimPending_) {
            trimPending_ = false;
            renderTrimView();  // newer frames wanted: these are already stale
            return;
        }
        viewer_->setTwoUp(images.first, images.second, labels[0], labels[1]);
    });
    watcher->setFuture(QtConcurrent::run([project, seq, frames, scale, proxies] {
        auto one = [&](FrameTime t) -> QImage {
            const Sequence* sq = project->findSequence(seq);
            if (!sq || t < 0 || t >= sq->duration()) return {};
            RenderOptions o;
            o.scale = scale;
            o.useProxies = proxies;
            o.displaySpace = "rec709";
            const Image img = renderProgramFrame(*project, *sq, t, o);
            QImage out(img.width, img.height, QImage::Format_RGBA8888);
            toRgba8(img, out.bits(), size_t(out.bytesPerLine()));
            return out;
        };
        return std::pair<QImage, QImage>{one(frames[0]), one(frames[1])};
    }));
}

void MonitorPanel::refresh() {
    const Sequence* s = controller_->sequence();
    FrameTime d = duration();
    scrub_->setRange(std::max<FrameTime>(d, 1));
    scrub_->setPosition(controller_->position());
    scrub_->setMarks(inPoint(), outPoint());
    if (mode_ == Mode::Program && state_->sequence()) {
        std::vector<FrameTime> m;
        for (const auto& mk : state_->sequence()->markers) m.push_back(mk.t);
        scrub_->setMarkers(m);
    }
    if (mode_ == Mode::Source) updateWaveform();
    if (!timecode_->hasFocus()) timecode_->setText(timecodeString(s, controller_->position()));
    FrameTime in = inPoint(), out = outPoint();
    QString dur = timecodeString(s, d);
    if (in >= 0 || out >= 0) {
        FrameTime a = std::max<FrameTime>(0, in), b = out >= 0 ? out + 1 : d;
        dur = tr("In–Out %1").arg(timecodeString(s, std::max<FrameTime>(0, b - a)));
    }
    durationLabel_->setText(dur);
}

void MonitorPanel::updateWaveform() {
    const Sequence* s = controller_->sequence();
    const MediaItem* m = state_->project().findMedia(state_->sourceMedia());
    if (!s || !m || !m->hasAudio || m->path.empty() || m->kind == MediaKind::Image || m->kind == MediaKind::Sequence) {
        scrub_->setWaveform(nullptr, 0);
        return;
    }
    PeaksPtr pk = MediaPool::instance().peaksIfReady(m->path);
    if (!pk && waveformRequested_ != m->path) {  // decoded once in the background; mediaReady brings it in
        waveformRequested_ = m->path;
        state_->startAudioDecode(*m);
    }
    scrub_->setWaveform(pk, 1.0 / s->fpsValue());
}

void MonitorPanel::markIn() {
    if (mode_ == Mode::Source) state_->setSourceIn(controller_->position());
    else state_->setInPoint(controller_->position());
}

void MonitorPanel::markOut() {
    if (mode_ == Mode::Source) state_->setSourceOut(controller_->position());
    else state_->setOutPoint(controller_->position());
}

void MonitorPanel::goToIn() {
    if (inPoint() >= 0) controller_->seek(inPoint());
}

void MonitorPanel::goToOut() {
    if (outPoint() >= 0) controller_->seek(outPoint());
}

void MonitorPanel::mousePressEvent(QMouseEvent* e) {
    emit activated();
    QWidget::mousePressEvent(e);
}

}  // namespace montage
