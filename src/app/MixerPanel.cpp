// Montage — audio mixer panel.
#include "MixerPanel.h"

#include <QComboBox>
#include <QDial>
#include <QInputDialog>
#include <QMenu>
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
    masterFx_ = new QToolButton(box);
    masterFx_->setObjectName(QStringLiteral("masterFx"));
    masterFx_->setText(tr("FX"));
    masterFx_->setToolTip(tr("Effects on the whole mix (shown in the Inspector)"));
    connect(masterFx_, &QToolButton::clicked, this, [this] {
        if (const Sequence* s = state_->sequence()) inspect(s->id);
    });
    v->addWidget(masterFx_, 0, Qt::AlignHCenter);
    auto* mid = new QHBoxLayout;
    mid->setContentsMargins(0, 0, 0, 0);
    mid->setSpacing(2);
    masterFader_ = new QSlider(Qt::Vertical, box);
    masterFader_->setObjectName(QStringLiteral("masterFader"));
    masterFader_->setRange(kFaderMin, kFaderMax);
    masterFader_->setPageStep(30);
    masterFader_->setToolTip(tr("Master volume (double-click for 0 dB)"));
    masterFader_->installEventFilter(this);
    masterMeter_ = new AudioMeterWidget(box);
    masterMeter_->setShowScale(true);
    mid->addWidget(masterFader_);
    mid->addWidget(masterMeter_);
    v->addLayout(mid, 1);
    masterDb_ = smallLabel(box);
    v->addWidget(masterDb_);
    connect(masterFader_, &QSlider::valueChanged, this, [this](int value) {
        const double db = value / 10.0;
        masterDb_->setText(dbText(db));
        state_->edit(tr("Master Volume"), [db](Project&, Sequence& s) {
            if (s.masterVolumeDb == db) return false;
            s.masterVolumeDb = db;
            return true;
        }, QStringLiteral("master-volume"));
    });
    return box;
}

void MixerPanel::inspect(Id owner) {
    state_->inspectChain(owner);
    emit effectsRequested();
}

void MixerPanel::addBus() {
    Id created = 0;
    state_->edit(tr("Add Bus"), [&created, this](Project& p, Sequence& s) {
        Bus b;
        b.id = created = p.newId();
        b.name = tr("Bus %1").arg(s.buses.size() + 1).toStdString();
        s.buses.push_back(b);
        return true;
    });
}

MixerPanel::BusStrip MixerPanel::makeBusStrip(Id bus) {
    BusStrip b;
    auto* box = new QFrame(stripHost_);
    box->setObjectName(QStringLiteral("mixerStrip"));
    box->setFixedWidth(kStripWidth);
    box->setContextMenuPolicy(Qt::CustomContextMenu);
    b.box = box;
    auto* v = new QVBoxLayout(box);
    v->setContentsMargins(4, 4, 4, 4);
    v->setSpacing(3);
    b.name = smallLabel(box, 11);
    QFont nf = b.name->font();
    nf.setBold(true);
    nf.setItalic(true);
    b.name->setFont(nf);
    v->addWidget(b.name);
    b.fx = new QToolButton(box);
    b.fx->setObjectName(QStringLiteral("fxButton"));
    b.fx->setToolTip(tr("This bus's effects (shown in the Inspector)"));
    v->addWidget(b.fx, 0, Qt::AlignHCenter);
    auto* mid = new QHBoxLayout;
    b.fader = new QSlider(Qt::Vertical, box);
    b.fader->setRange(kFaderMin, kFaderMax);
    b.fader->setPageStep(30);
    b.fader->setMinimumHeight(80);
    b.fader->setToolTip(tr("Bus volume (double-click for 0 dB)"));
    b.fader->installEventFilter(this);
    mid->addWidget(b.fader, 0, Qt::AlignHCenter);
    v->addLayout(mid, 1);
    b.dbLabel = smallLabel(box);
    v->addWidget(b.dbLabel);
    b.mute = new QToolButton(box);
    b.mute->setObjectName(QStringLiteral("muteButton"));
    b.mute->setText(tr("M"));
    b.mute->setCheckable(true);
    v->addWidget(b.mute, 0, Qt::AlignHCenter);
    auto editBus = [this, bus](const QString& label, std::function<bool(Bus&)> fn, const QString& merge = {}) {
        state_->edit(label, [bus, fn](Project&, Sequence& s) {
            for (Bus& x : s.buses)
                if (x.id == bus) return fn(x);
            return false;
        }, merge);
    };
    connect(b.fx, &QToolButton::clicked, this, [this, bus] { inspect(bus); });
    connect(b.fader, &QSlider::valueChanged, this, [editBus, bus](int value) {
        const double db = value / 10.0;
        editBus(tr("Bus Volume"), [db](Bus& x) {
            if (x.volumeDb == db) return false;
            x.volumeDb = db;
            return true;
        }, QStringLiteral("bus-volume-%1").arg(bus));
    });
    connect(b.mute, &QToolButton::toggled, this, [editBus](bool on) {
        editBus(on ? tr("Mute Bus") : tr("Unmute Bus"), [on](Bus& x) {
            if (x.muted == on) return false;
            x.muted = on;
            return true;
        });
    });
    connect(box, &QWidget::customContextMenuRequested, this, [this, box, bus, editBus](const QPoint& pos) {
        QMenu menu;
        menu.addAction(tr("Rename..."), this, [this, bus, editBus] {
            const Sequence* s = state_->sequence();
            QString current;
            for (const Bus& x : s->buses)
                if (x.id == bus) current = QString::fromStdString(x.name);
            bool ok = false;
            const QString name = QInputDialog::getText(this, tr("Rename Bus"), tr("Name:"), QLineEdit::Normal, current, &ok);
            if (ok && !name.trimmed().isEmpty())
                editBus(tr("Rename Bus"), [name](Bus& x) {
                    x.name = name.trimmed().toStdString();
                    return true;
                });
        });
        menu.addAction(tr("Delete Bus"), this, [this, bus] {
            state_->edit(tr("Delete Bus"), [bus](Project&, Sequence& s) {
                auto it = std::find_if(s.buses.begin(), s.buses.end(), [bus](const Bus& x) { return x.id == bus; });
                if (it == s.buses.end()) return false;
                s.buses.erase(it);
                for (Track& t : s.audioTracks)
                    if (t.output == bus) t.output = 0;  // back to the master
                return true;
            });
        });
        menu.exec(box->mapToGlobal(pos));
    });
    return b;
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

    s.fx = new QToolButton(box);
    s.fx->setObjectName(QStringLiteral("fxButton"));
    s.fx->setToolTip(tr("This track's insert effects (shown in the Inspector)"));
    v->addWidget(s.fx, 0, Qt::AlignHCenter);
    s.output = new QComboBox(box);
    s.output->setObjectName(QStringLiteral("outputCombo"));
    s.output->setToolTip(tr("Where this track goes: the master or a bus"));
    QFont of = s.output->font();
    of.setPixelSize(10);
    s.output->setFont(of);
    v->addWidget(s.output);

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
    connect(s.fx, &QToolButton::clicked, this, [this, index] {
        const Sequence* seq = state_->sequence();
        if (seq && index < int(seq->audioTracks.size())) inspect(seq->audioTracks[size_t(index)].id);
    });
    connect(s.output, &QComboBox::activated, this, [this, index, combo = s.output](int i) {
        const Id bus = combo->itemData(i).toULongLong();
        state_->edit(tr("Track Output"), [index, bus](Project&, Sequence& sq) {
            if (index >= int(sq.audioTracks.size()) || sq.audioTracks[size_t(index)].output == bus) return false;
            sq.audioTracks[size_t(index)].output = bus;
            return true;
        });
    });
    return s;
}

void MixerPanel::syncToProject() {
    const Sequence* seq = state_->sequence();
    const size_t count = seq ? seq->audioTracks.size() : 0;
    const size_t buses = seq ? seq->buses.size() : 0;
    if (count != strips_.size() || buses != busStrips_.size())
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
    for (BusStrip& b : busStrips_) {
        stripLayout_->removeWidget(b.box);
        b.box->hide();
        b.box->deleteLater();
    }
    busStrips_.clear();
    if (!addBus_) {
        addBus_ = new QToolButton(stripHost_);
        addBus_->setObjectName(QStringLiteral("addBus"));
        addBus_->setText(tr("+ Bus"));
        addBus_->setToolTip(tr("Add a bus: route tracks to it to process them together"));
        connect(addBus_, &QToolButton::clicked, this, &MixerPanel::addBus);
        stripLayout_->insertWidget(stripLayout_->count() - 1, addBus_);
    }
    stripLayout_->removeWidget(addBus_);

    const Sequence* seq = state_->sequence();
    const int count = seq ? int(seq->audioTracks.size()) : 0;
    for (int i = 0; i < count; ++i) {
        strips_.push_back(makeStrip(i));
        stripLayout_->insertWidget(stripLayout_->count() - 1, strips_.back().box);  // before the stretch
    }
    if (seq)
        for (const Bus& b : seq->buses) {
            busStrips_.push_back(makeBusStrip(b.id));
            stripLayout_->insertWidget(stripLayout_->count() - 1, busStrips_.back().box);
        }
    stripLayout_->insertWidget(stripLayout_->count() - 1, addBus_, 0, Qt::AlignTop);
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
        s.fx->setText(t.effects.empty() ? tr("FX") : tr("FX %1").arg(t.effects.size()));
        {
            const QSignalBlocker block(s.output);
            s.output->clear();
            s.output->addItem(tr("Master"), QVariant::fromValue<qulonglong>(0));
            for (const Bus& b : seq->buses) {
                s.output->addItem(QString::fromStdString(b.name), QVariant::fromValue<qulonglong>(b.id));
                if (b.id == t.output) s.output->setCurrentIndex(s.output->count() - 1);
            }
        }
    }
    for (size_t i = 0; i < busStrips_.size() && i < seq->buses.size(); ++i) {
        const Bus& b = seq->buses[i];
        BusStrip& st = busStrips_[i];
        const QString name = QString::fromStdString(b.name);
        st.name->setText(st.name->fontMetrics().elidedText(name, Qt::ElideRight, kStripWidth - 12));
        st.name->setToolTip(tr("%1 (bus; right-click to rename or delete)").arg(name));
        st.fx->setText(b.effects.empty() ? tr("FX") : tr("FX %1").arg(b.effects.size()));
        {
            const QSignalBlocker block(st.fader);
            st.fader->setValue(int(std::lround(std::clamp(b.volumeDb, -60.0, 12.0) * 10.0)));
        }
        st.dbLabel->setText(dbText(b.volumeDb));
        {
            const QSignalBlocker block(st.mute);
            st.mute->setChecked(b.muted);
        }
    }
    masterFx_->setText(seq->masterEffects.empty() ? tr("FX") : tr("FX %1").arg(seq->masterEffects.size()));
    {
        const QSignalBlocker block(masterFader_);
        masterFader_->setValue(int(std::lround(std::clamp(seq->masterVolumeDb, -60.0, 12.0) * 10.0)));
    }
    masterDb_->setText(dbText(seq->masterVolumeDb));
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
