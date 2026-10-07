// Montage — audio mixer panel.
#include "MixerPanel.h"

#include <QDial>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

#include "AudioMeterWidget.h"
#include "EditorState.h"
#include "Theme.h"

namespace montage {

namespace {

constexpr int kStripWidth = 80;
constexpr int kMasterWidth = 68;
constexpr int kFaderMin = -600;  // tenths of a dB
constexpr int kFaderMax = 120;

QString dbText(double db) {
    if (std::abs(db) < 0.05)
        return QStringLiteral("0.0 dB");
    return QStringLiteral("%1%2 dB").arg(db > 0 ? QStringLiteral("+") : QString()).arg(db, 0, 'f', 1);
}

QString panText(int v) {
    if (v == 0)
        return QStringLiteral("C");
    return v < 0 ? QStringLiteral("L%1").arg(-v) : QStringLiteral("R%1").arg(v);
}

QLabel* smallLabel(QWidget* parent, int pixelSize = 10) {
    auto* l = new QLabel(parent);
    l->setAlignment(Qt::AlignCenter);
    QFont f = l->font();
    f.setPixelSize(pixelSize);
    l->setFont(f);
    return l;
}

}  // namespace

MixerPanel::MixerPanel(EditorState* state, QWidget* parent) : QWidget(parent), state_(state) {
    setStyleSheet(QStringLiteral("QFrame#mixerStrip { background: %1; border: 1px solid %2; border-radius: 3px; }"
                                 "QToolButton#muteButton, QToolButton#soloButton { background: %3; color: %4;"
                                 " border: 1px solid %2; border-radius: 2px; min-width: 22px; min-height: 18px;"
                                 " font-weight: bold; }"
                                 "QToolButton#muteButton:checked { background: %5; color: %7; }"
                                 "QToolButton#soloButton:checked { background: %6; color: %7; }")
                      .arg(theme::kPanelAlt.name(), theme::kBorder.name(), theme::kPanel.name(),
                           theme::kTextDim.name(), theme::kMeterRed.name(), theme::kMeterYellow.name(),
                           theme::kWindow.name()));

    auto* outer = new QHBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    scroll_ = new QScrollArea(this);
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    stripHost_ = new QWidget(scroll_);
    stripLayout_ = new QHBoxLayout(stripHost_);
    stripLayout_->setContentsMargins(4, 4, 4, 4);
    stripLayout_->setSpacing(3);
    emptyLabel_ = new QLabel(tr("No audio tracks"), stripHost_);
    emptyLabel_->setStyleSheet(QStringLiteral("color: %1;").arg(theme::kTextDim.name()));
    stripLayout_->addWidget(emptyLabel_);
    stripLayout_->addStretch(1);
    scroll_->setWidget(stripHost_);
    outer->addWidget(scroll_, 1);

    // The master strip stays pinned at the right edge, outside the scroll area.
    auto* sep = new QFrame(this);
    sep->setFrameShape(QFrame::VLine);
    sep->setStyleSheet(QStringLiteral("color: %1;").arg(theme::kBorder.name()));
    outer->addWidget(sep);
    outer->addWidget(makeMasterStrip());

    connect(state_, &EditorState::projectChanged, this, &MixerPanel::syncToProject);
    connect(state_, &EditorState::sequenceSwitched, this, &MixerPanel::syncToProject);
    rebuild();
}

QWidget* MixerPanel::makeMasterStrip() {
    auto* box = new QFrame(this);
    box->setObjectName(QStringLiteral("mixerStrip"));
    box->setFixedWidth(kMasterWidth);
    auto* v = new QVBoxLayout(box);
    v->setContentsMargins(4, 4, 4, 4);
    v->setSpacing(3);
    auto* name = smallLabel(box, 11);
    QFont f = name->font();
    f.setBold(true);
    name->setFont(f);
    name->setText(tr("Master"));
    v->addWidget(name);
    masterMeter_ = new AudioMeterWidget(box);
    masterMeter_->setShowScale(true);
    v->addWidget(masterMeter_, 1, Qt::AlignHCenter);
    return box;
}

MixerPanel::Strip MixerPanel::makeStrip(int index) {
    Strip s;
    auto* box = new QFrame(stripHost_);
    box->setObjectName(QStringLiteral("mixerStrip"));
    box->setFixedWidth(kStripWidth);
    s.box = box;
    auto* v = new QVBoxLayout(box);
    v->setContentsMargins(4, 4, 4, 4);
    v->setSpacing(3);

    s.name = smallLabel(box, 11);
    QFont nf = s.name->font();
    nf.setBold(true);
    s.name->setFont(nf);
    v->addWidget(s.name);

    s.pan = new QDial(box);
    s.pan->setRange(-100, 100);
    s.pan->setSingleStep(1);
    s.pan->setPageStep(10);
    s.pan->setNotchesVisible(true);
    s.pan->setNotchTarget(8);
    s.pan->setWrapping(false);
    s.pan->setFixedSize(34, 34);
    s.pan->setToolTip(tr("Pan (double-click to centre)"));
    s.pan->installEventFilter(this);
    v->addWidget(s.pan, 0, Qt::AlignHCenter);
    s.panLabel = smallLabel(box);
    s.panLabel->setStyleSheet(QStringLiteral("color: %1;").arg(theme::kTextDim.name()));
    v->addWidget(s.panLabel);

    auto* mid = new QHBoxLayout;
    mid->setContentsMargins(0, 0, 0, 0);
    mid->setSpacing(2);
    s.fader = new QSlider(Qt::Vertical, box);
    s.fader->setRange(kFaderMin, kFaderMax);
    s.fader->setSingleStep(1);
    s.fader->setPageStep(30);
    s.fader->setTickPosition(QSlider::TicksLeft);
    s.fader->setTickInterval(60);
    s.fader->setMinimumHeight(80);
    s.fader->setToolTip(tr("Volume (double-click for 0 dB)"));
    s.fader->installEventFilter(this);
    s.meter = new AudioMeterWidget(box);
    s.meter->setCompact(true);
    mid->addWidget(s.fader);
    mid->addWidget(s.meter);
    v->addLayout(mid, 1);

    s.dbLabel = smallLabel(box);
    v->addWidget(s.dbLabel);

    auto* buttons = new QHBoxLayout;
    buttons->setContentsMargins(0, 0, 0, 0);
    buttons->setSpacing(3);
    s.mute = new QToolButton(box);
    s.mute->setObjectName(QStringLiteral("muteButton"));
    s.mute->setText(tr("M"));
    s.mute->setToolTip(tr("Mute"));
    s.mute->setCheckable(true);
    s.solo = new QToolButton(box);
    s.solo->setObjectName(QStringLiteral("soloButton"));
    s.solo->setText(tr("S"));
    s.solo->setToolTip(tr("Solo"));
    s.solo->setCheckable(true);
    buttons->addStretch(1);
    buttons->addWidget(s.mute);
    buttons->addWidget(s.solo);
    buttons->addStretch(1);
    v->addLayout(buttons);

    connect(s.fader, &QSlider::valueChanged, this, [this, index](int value) { setVolume(index, value / 10.0); });
    connect(s.pan, &QDial::valueChanged, this, [this, index](int value) { setPan(index, value / 100.0); });
    connect(s.mute, &QToolButton::toggled, this, [this, index](bool on) { setMute(index, on); });
    connect(s.solo, &QToolButton::toggled, this, [this, index](bool on) { setSolo(index, on); });
    return s;
}

void MixerPanel::syncToProject() {
    const Sequence* seq = state_->sequence();
    const size_t count = seq ? seq->audioTracks.size() : 0;
    if (count != strips_.size())
        rebuild();
    else
        refresh();
}

void MixerPanel::rebuild() {
    for (Strip& s : strips_) {
        stripLayout_->removeWidget(s.box);
        s.box->hide();
        s.box->deleteLater();  // may be called from one of the strip's own signals
    }
    strips_.clear();

    const Sequence* seq = state_->sequence();
    const int count = seq ? int(seq->audioTracks.size()) : 0;
    for (int i = 0; i < count; ++i) {
        strips_.push_back(makeStrip(i));
        stripLayout_->insertWidget(stripLayout_->count() - 1, strips_.back().box);  // before the stretch
    }
    emptyLabel_->setVisible(count == 0);
    refresh();
}

void MixerPanel::refresh() {
    const Sequence* seq = state_->sequence();
    if (!seq)
        return;
    for (size_t i = 0; i < strips_.size() && i < seq->audioTracks.size(); ++i) {
        const Track& t = seq->audioTracks[i];
        Strip& s = strips_[i];
        const QString name = t.name.empty() ? tr("A%1").arg(i + 1) : QString::fromStdString(t.name);
        s.name->setText(s.name->fontMetrics().elidedText(name, Qt::ElideRight, kStripWidth - 12));
        s.name->setToolTip(name);
        {
            const QSignalBlocker block(s.fader);
            s.fader->setValue(int(std::lround(std::clamp(t.volumeDb, -60.0, 12.0) * 10.0)));
        }
        s.dbLabel->setText(dbText(t.volumeDb));
        {
            const QSignalBlocker block(s.pan);
            s.pan->setValue(int(std::lround(std::clamp(t.pan, -1.0, 1.0) * 100.0)));
        }
        s.panLabel->setText(panText(s.pan->value()));
        {
            const QSignalBlocker block(s.mute);
            s.mute->setChecked(t.muted);
        }
        {
            const QSignalBlocker block(s.solo);
            s.solo->setChecked(t.solo);
        }
    }
}

void MixerPanel::setVolume(int index, double db) {
    if (index < int(strips_.size()))
        strips_[index].dbLabel->setText(dbText(db));
    const bool ok = state_->edit(
        tr("Track Volume"),
        [index, db](Project&, Sequence& s) {
            if (index >= int(s.audioTracks.size()) || s.audioTracks[index].volumeDb == db)
                return false;
            s.audioTracks[index].volumeDb = db;
            return true;
        },
        QStringLiteral("track-volume-%1").arg(index));
    if (!ok)
        refresh();
}

void MixerPanel::setPan(int index, double pan) {
    if (index < int(strips_.size()))
        strips_[index].panLabel->setText(panText(int(std::lround(pan * 100.0))));
    const bool ok = state_->edit(
        tr("Track Pan"),
        [index, pan](Project&, Sequence& s) {
            if (index >= int(s.audioTracks.size()) || s.audioTracks[index].pan == pan)
                return false;
            s.audioTracks[index].pan = pan;
            return true;
        },
        QStringLiteral("track-pan-%1").arg(index));
    if (!ok)
        refresh();
}

void MixerPanel::setMute(int index, bool on) {
    const bool ok = state_->edit(on ? tr("Mute Track") : tr("Unmute Track"), [index, on](Project&, Sequence& s) {
        if (index >= int(s.audioTracks.size()) || s.audioTracks[index].muted == on)
            return false;
        s.audioTracks[index].muted = on;
        return true;
    });
    if (!ok)
        refresh();
}

void MixerPanel::setSolo(int index, bool on) {
    const bool ok = state_->edit(on ? tr("Solo Track") : tr("Unsolo Track"), [index, on](Project&, Sequence& s) {
        if (index >= int(s.audioTracks.size()) || s.audioTracks[index].solo == on)
            return false;
        s.audioTracks[index].solo = on;
        return true;
    });
    if (!ok)
        refresh();
}

void MixerPanel::setLevels(float masterL, float masterR, const QVector<float>& trackPeaks) {
    masterMeter_->setLevels(masterL, masterR);
    for (size_t i = 0; i < strips_.size(); ++i) {
        const qsizetype l = qsizetype(i) * 2;
        if (l + 1 < trackPeaks.size())
            strips_[i].meter->setLevels(trackPeaks[l], trackPeaks[l + 1]);
    }
}

void MixerPanel::resetMeters() {
    masterMeter_->reset();
    for (Strip& s : strips_)
        s.meter->reset();
}

bool MixerPanel::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::MouseButtonDblClick) {
        // Double-click resets pan to centre and the fader to 0 dB (via valueChanged, so it is undoable).
        if (auto* dial = qobject_cast<QDial*>(watched)) {
            dial->setValue(0);
            return true;
        }
        if (auto* slider = qobject_cast<QSlider*>(watched)) {
            slider->setValue(0);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

}  // namespace montage
