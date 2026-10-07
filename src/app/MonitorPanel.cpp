#include "MonitorPanel.h"

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
#include <QToolButton>
#include <QVBoxLayout>

#include "EditorState.h"
#include "PlaybackController.h"
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

void ViewerWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(0x0e, 0x0f, 0x11));
    if (image_.isNull()) {
        p.setPen(theme::kTextDim);
        p.drawText(rect(), Qt::AlignCenter, placeholder_);
        return;
    }
    QSizeF s = QSizeF(image_.size()).scaled(size(), Qt::KeepAspectRatio);
    QRectF r((width() - s.width()) / 2, (height() - s.height()) / 2, s.width(), s.height());
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawImage(r, image_);
    if (safe_) {
        p.setPen(QPen(QColor(255, 255, 255, 90), 1, Qt::DashLine));
        p.drawRect(r.adjusted(r.width() * 0.05, r.height() * 0.05, -r.width() * 0.05, -r.height() * 0.05));
        p.drawRect(r.adjusted(r.width() * 0.1, r.height() * 0.1, -r.width() * 0.1, -r.height() * 0.1));
        p.drawLine(QPointF(r.center().x() - 10, r.center().y()), QPointF(r.center().x() + 10, r.center().y()));
        p.drawLine(QPointF(r.center().x(), r.center().y() - 10), QPointF(r.center().x(), r.center().y() + 10));
    }
}

void ViewerWidget::mousePressEvent(QMouseEvent* e) {
    pressPos_ = e->pos();
    QWidget::mousePressEvent(e);
}

void ViewerWidget::mouseMoveEvent(QMouseEvent* e) {
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

FrameTime ScrubBar::frameAt(int x) const {
    double u = std::clamp(double(x - 4) / std::max(1, width() - 8), 0.0, 1.0);
    return FrameTime(std::llround(u * double(duration_)));
}

void ScrubBar::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), theme::kPanel);
    int w = width() - 8;
    auto xOf = [&](FrameTime t) { return 4 + int(double(t) / double(duration_) * w); };
    p.fillRect(QRect(4, height() / 2 - 2, w, 4), theme::kBorder);
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
        bar->addSpacing(8);
        for (QToolButton* b : {lift, extract, safe, loop, proxy, still}) bar->addWidget(b);
        connect(proxy, &QToolButton::toggled, this, [this](bool on) { controller_->setUseProxies(on); });
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
    refresh();
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
    if (!timecode_->hasFocus()) timecode_->setText(timecodeString(s, controller_->position()));
    FrameTime in = inPoint(), out = outPoint();
    QString dur = timecodeString(s, d);
    if (in >= 0 || out >= 0) {
        FrameTime a = std::max<FrameTime>(0, in), b = out >= 0 ? out + 1 : d;
        dur = tr("In–Out %1").arg(timecodeString(s, std::max<FrameTime>(0, b - a)));
    }
    durationLabel_->setText(dur);
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
