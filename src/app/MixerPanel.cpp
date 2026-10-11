// Montage — audio mixer panel.
#include "MixerPanel.h"

#include <QComboBox>
#include <QDial>
#include <QInputDialog>
#include <QMenu>
#include <QPointer>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

#include "AudioMeterWidget.h"
#include "EditorState.h"
#include "SurroundPanner.h"
#include "core/Automation.h"
#include "core/Surround.h"
#include "Theme.h"

namespace montage {

namespace {

// Whether a track plays its automation lanes (and its fader edits them).
bool readsLanes(const Track& t) {
    const AutomationMode m = trackAutomation(t);
    return m == AutomationMode::Read || m == AutomationMode::Latch || m == AutomationMode::Touch;
}
bool readsLanes(const Bus& b) {
    const AutomationMode m = busAutomation(b);
    return m == AutomationMode::Read || m == AutomationMode::Latch || m == AutomationMode::Touch;
}
bool writes(AutomationMode m) { return m == AutomationMode::Write || m == AutomationMode::Latch || m == AutomationMode::Touch; }
// A switch's lane as written (a mute, an insert's on/off, choice or stepped parameter): each key holds to the next, at
// a whole value where the values are whole (a mute's 0 or 1, a choice's index).
Param switchLane(Param lane, bool whole = true) {
    for (Keyframe& k : lane.keys) {
        k.interp = Interp::Hold;
        if (whole) k.v = std::round(k.v);
    }
    return lane;
}

// A switch lane (a mute) set to `v` from `at` to its next point, staying as it was before `at`.
void switchFrom(Param& lane, FrameTime at, double v) {
    const bool earlier = std::any_of(lane.keys.begin(), lane.keys.end(), [at](const Keyframe& k) { return k.t < at; });
    const double before = at > 0 ? lane.at(at - 1) : v;
    auto put = [&lane](FrameTime t, double value) {
        auto it = std::find_if(lane.keys.begin(), lane.keys.end(), [t](const Keyframe& k) { return k.t == t; });
        if (it == lane.keys.end()) it = lane.keys.insert(lane.keys.end(), Keyframe{t, value});
        it->v = value;
        it->interp = Interp::Hold;
    };
    if (at > 0 && !earlier) put(at - 1, before);
    put(at, v);
    std::sort(lane.keys.begin(), lane.keys.end(), [](const Keyframe& a, const Keyframe& b) { return a.t < b.t; });
}

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
    connect(state_, &EditorState::playheadChanged, this, [this] { followAutomation(); });
    // Another sequence made active during an automation pass: what the pass wrote is kept on its own sequence and
    // recording starts again on the new one. Another project opened takes the pass with it.
    connect(state_, &EditorState::sequenceSwitched, this, [this] {
        if (!recordingAutomation()) return;
        if (state_->projectGeneration() != passGeneration_) {
            discardPass();
            return;
        }
        if (stopAt_ >= 0) {  // stopped, waiting for a drag that the switch has ended: written as that stop
            finishPass(stopAt_, true);
            return;
        }
        finishPass(lastTick_, false);
        resume_ = true;
    });
    state_->setParameterCapture([self = QPointer<MixerPanel>(this)](const EditorState::ParameterMove& m) {
        return self && self->captureInsert(m);
    });
    state_->setLiveParameter([self = QPointer<MixerPanel>(this)](Id owner, Id effect, const std::string& param) -> std::optional<double> {
        return self ? self->liveInsert(owner, effect, param) : std::nullopt;
    });
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
        }, mergeKey_.isEmpty() ? QStringLiteral("master-volume") : mergeKey_);
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
    b.id = bus;
    b.fx = new QToolButton(box);
    b.fx->setObjectName(QStringLiteral("fxButton"));
    b.fx->setToolTip(tr("This bus's effects (shown in the Inspector)"));
    v->addWidget(b.fx, 0, Qt::AlignHCenter);
    b.automation = new QComboBox(box);
    b.automation->setObjectName(QStringLiteral("busAutomationMode"));
    for (int m = 0; m <= 4; ++m) b.automation->addItem(tr(automationModeName(AutomationMode(m))));
    b.automation->setToolTip(tr("Bus automation: Off ignores it; Read plays it; Write records the fader, pan and mute from play to "
                                "stop; Latch from the first move to stop; Touch while you hold them"));
    QFont small = b.automation->font();
    small.setPixelSize(10);
    b.automation->setFont(small);
    v->addWidget(b.automation);
    b.pan = new QDial(box);
    b.pan->setObjectName(QStringLiteral("busPan"));
    b.pan->setRange(-100, 100);
    b.pan->setNotchesVisible(true);
    b.pan->setNotchTarget(8);
    b.pan->setWrapping(false);
    b.pan->setFixedSize(30, 30);
    b.pan->setToolTip(tr("Bus pan (double-click to centre)"));
    b.pan->installEventFilter(this);
    v->addWidget(b.pan, 0, Qt::AlignHCenter);
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
    b.surround = new SurroundPanner(box);
    b.surround->setObjectName(QStringLiteral("surroundPanner"));
    b.surround->hide();
    v->insertWidget(2, b.surround, 0, Qt::AlignHCenter);
    auto editBus = [this, bus](const QString& label, std::function<bool(Bus&)> fn, const QString& merge = {}) {
        state_->edit(label, [bus, fn](Project&, Sequence& s) {
            for (Bus& x : s.buses)
                if (x.id == bus) return fn(x);
            return false;
        }, merge);
    };
    connect(b.fx, &QToolButton::clicked, this, [this, bus] { inspect(bus); });
    connect(b.surround, &SurroundPanner::changed, this, [editBus, bus](const SurroundPan& pan, bool) {
        editBus(tr("Bus Surround Pan"), [pan](Bus& x) {
            if (x.surround == pan) return false;
            x.surround = pan;
            return true;
        }, QStringLiteral("bus-surround-%1").arg(bus));
    });
    connect(b.fader, &QSlider::valueChanged, this, [this, bus](int value) { setBusVolume(bus, value / 10.0); });
    connect(b.pan, &QDial::valueChanged, this, [this, bus](int value) { setBusPan(bus, value / 100.0); });
    connect(b.mute, &QToolButton::toggled, this, [this, bus](bool on) { setBusMute(bus, on); });
    connect(b.automation, &QComboBox::currentIndexChanged, this, [editBus](int m) {
        editBus(tr("Bus Automation Mode"), [m](Bus& x) {
            if (x.automation == m) return false;
            x.automation = m;
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
            // Tracks routed to it go back to the master; sends to it go.
            state_->apply(tr("Delete Bus"), [bus](Project&, Sequence& s) { return edit::removeBus(s, bus); });
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
    s.automation = new QComboBox(box);
    s.automation->setObjectName(QStringLiteral("automationMode"));
    for (int m = 0; m <= 4; ++m) s.automation->addItem(tr(automationModeName(AutomationMode(m))));
    s.automation->setToolTip(tr("Fader automation: Off ignores it; Read plays it; Write records the fader and pan from play to stop; "
                                "Latch from the first move to stop; Touch while you hold them"));
    s.automation->setFont(of);
    v->addWidget(s.automation);
    // Aux sends: a row per send (its bus and level, right-click for pre-fader, mute or remove), and "+ Send".
    auto* sendBox = new QWidget(box);
    s.sendLayout = new QVBoxLayout(sendBox);
    s.sendLayout->setContentsMargins(0, 0, 0, 0);
    s.sendLayout->setSpacing(1);
    v->addWidget(sendBox);
    if (const Sequence* seq = state_->sequence(); seq && index < int(seq->audioTracks.size()))
        for (size_t k = 0; k < seq->audioTracks[size_t(index)].sends.size(); ++k) s.sends.push_back(makeSendRow(s, index, int(k)));
    s.addSend = new QToolButton(box);
    s.addSend->setObjectName(QStringLiteral("addSend"));
    s.addSend->setText(tr("+ Send"));
    s.addSend->setFont(of);
    s.addSend->setToolTip(tr("Send this track to a bus as well, at its own level (an effects return such as a shared reverb)"));
    v->addWidget(s.addSend, 0, Qt::AlignHCenter);
    connect(s.addSend, &QToolButton::clicked, this, [this, index, button = s.addSend] {
        const Sequence* seq = state_->sequence();
        if (!seq || index >= int(seq->audioTracks.size())) return;
        const Track& t = seq->audioTracks[size_t(index)];
        QMenu menu;
        for (const Bus& b : seq->buses) {
            // Not the bus it already goes to (that would add it there twice), nor one it already sends to.
            if (b.id == t.output || std::any_of(t.sends.begin(), t.sends.end(), [&](const Send& sd) { return sd.bus == b.id; })) continue;
            menu.addAction(QString::fromStdString(b.name), this, [this, index, bus = b.id] { addSend(index, bus); });
        }
        if (!menu.isEmpty()) menu.addSeparator();
        menu.addAction(tr("New Effects Return"), this, [this, index] { newReturn(index); });
        menu.exec(button->mapToGlobal(QPoint(0, button->height())));
    });

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
    s.surround = new SurroundPanner(box);
    s.surround->setObjectName(QStringLiteral("surroundPanner"));
    s.surround->setObjectsAllowed(true);
    s.surround->hide();
    v->addWidget(s.surround, 0, Qt::AlignHCenter);
    connect(s.surround, &SurroundPanner::changed, this, [this, index](const SurroundPan& pan, bool final) { setSurround(index, pan, final); });
    connect(s.surround, &SurroundPanner::animateRequested, this, [this, index](bool on) { animateSurround(index, on); });

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
    connect(s.automation, &QComboBox::currentIndexChanged, this, [this, index](int m) { setAutomationMode(index, m); });
    connect(s.fx, &QToolButton::clicked, this, [this, index] {
        const Sequence* seq = state_->sequence();
        if (seq && index < int(seq->audioTracks.size())) inspect(seq->audioTracks[size_t(index)].id);
    });
    connect(s.output, &QComboBox::activated, this, [this, index, combo = s.output](int i) {
        const Id bus = combo->itemData(i).toULongLong();
        state_->edit(tr("Track Output"), [index, bus](Project&, Sequence& sq) {
            if (index >= int(sq.audioTracks.size()) || sq.audioTracks[size_t(index)].output == bus) return false;
            Track& t = sq.audioTracks[size_t(index)];
            t.output = bus;
            // A send to the bus it now feeds would add it there twice.
            if (bus) std::erase_if(t.sends, [bus](const Send& sd) { return sd.bus == bus; });
            return true;
        });
    });
    return s;
}

MixerPanel::Strip::SendRow MixerPanel::makeSendRow(Strip& strip, int track, int send) {
    Strip::SendRow r;
    r.box = new QWidget(strip.sendLayout->parentWidget());
    r.box->setContextMenuPolicy(Qt::CustomContextMenu);
    auto* l = new QVBoxLayout(r.box);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(0);
    r.name = smallLabel(r.box, 9);
    r.name->setStyleSheet(QStringLiteral("color: %1;").arg(theme::kTextDim.name()));
    r.level = new QSlider(Qt::Horizontal, r.box);
    r.level->setObjectName(QStringLiteral("sendLevel"));
    r.level->setRange(kFaderMin, kFaderMax);
    r.level->setPageStep(30);
    r.level->setFixedHeight(14);
    r.level->installEventFilter(this);  // double-click: 0 dB
    l->addWidget(r.name);
    l->addWidget(r.level);
    strip.sendLayout->addWidget(r.box);
    connect(r.level, &QSlider::valueChanged, this, [this, track, send](int value) { setSendLevel(track, send, value / 10.0); });
    connect(r.box, &QWidget::customContextMenuRequested, this, [this, track, send, box = r.box](const QPoint& pos) {
        const Sequence* seq = state_->sequence();
        if (recordingAutomation() || !seq || track >= int(seq->audioTracks.size()) || send >= int(seq->audioTracks[size_t(track)].sends.size()))
            return;
        const Send& sd = seq->audioTracks[size_t(track)].sends[size_t(send)];
        QMenu menu;
        QAction* pre = menu.addAction(tr("Pre-Fader"));
        pre->setCheckable(true);
        pre->setChecked(sd.preFader);
        QAction* mute = menu.addAction(tr("Mute Send"));
        mute->setCheckable(true);
        mute->setChecked(sd.muted);
        QAction* remove = menu.addAction(tr("Remove Send"));
        QAction* chosen = menu.exec(box->mapToGlobal(pos));
        if (chosen == pre) editSend(track, send, tr("Send Pre/Post Fader"), [on = pre->isChecked()](Send& x) { x.preFader = on; return true; });
        else if (chosen == mute) editSend(track, send, tr("Mute Send"), [on = mute->isChecked()](Send& x) { x.muted = on; return true; });
        else if (chosen == remove) {
            const Id bus = sd.bus;
            state_->apply(tr("Remove Send"), [track, bus](Project&, Sequence& s) { return edit::removeSend(s, track, bus); });
        }
    });
    return r;
}

void MixerPanel::addSend(int track, Id bus) {
    const Sequence* seq = state_->sequence();
    if (!seq || track < 0 || track >= int(seq->audioTracks.size())) return;
    const auto& sends = seq->audioTracks[size_t(track)].sends;
    if (std::any_of(sends.begin(), sends.end(), [bus](const Send& sd) { return sd.bus == bus; })) return;  // (one per bus)
    state_->apply(tr("Add Send"), [track, bus](Project&, Sequence& s) { return edit::setSend(s, track, bus, 0, false); });
}

Id MixerPanel::newReturn(int track) {
    Id created = 0;
    state_->edit(tr("New Effects Return"), [&created, track, this](Project& p, Sequence& s) {
        if (track < 0 || track >= int(s.audioTracks.size())) return false;
        Bus b;
        b.id = created = p.newId();
        b.name = tr("Return %1").arg(s.buses.size() + 1).toStdString();
        s.buses.push_back(b);
        return edit::setSend(s, track, b.id, 0, false).ok;
    });
    return created;
}

void MixerPanel::editSend(int track, int send, const QString& label, const std::function<bool(Send&)>& fn) {
    state_->edit(label, [track, send, fn](Project&, Sequence& s) {
        if (track >= int(s.audioTracks.size()) || send >= int(s.audioTracks[size_t(track)].sends.size())) return false;
        return fn(s.audioTracks[size_t(track)].sends[size_t(send)]);
    });
}

bool MixerPanel::recordingLane(const QString& id) const {
    return std::any_of(lanes_.begin(), lanes_.end(), [&](const LaneRecording& l) { return l.id == id; });
}

std::optional<double> MixerPanel::liveInsert(Id owner, Id effect, const std::string& param) const {
    const QString id = QStringLiteral("insert-%1-%2-%3").arg(owner).arg(effect).arg(QString::fromStdString(param));
    for (const LaneRecording& lr : lanes_)
        if (lr.id == id) return lr.shown;
    return std::nullopt;
}

bool MixerPanel::passWrote() const {
    for (const Recording& r : recording_)
        if (r.volume.wrote() || r.pan.wrote() || (r.surround && (r.x.wrote() || r.y.wrote() || r.z.wrote())) || r.changedOther) return true;
    return std::any_of(lanes_.begin(), lanes_.end(), [](const LaneRecording& l) { return l.rec.wrote(); });
}

void MixerPanel::rebaseRecorders() {
    // Lanes and levels edited during the pass (a point added, undo, a fader's level set elsewhere): what the recorders
    // show and glide back to, and lay what they write over.
    const Sequence* seq = state_->sequence();
    if (!seq || seq->id != passSeq_) return;
    Sequence s = *seq;  // (a lane looked up for the first time may be added to its effect)
    for (Recording& r : recording_) {
        if (r.track >= int(s.audioTracks.size()) || s.audioTracks[size_t(r.track)].id != r.trackId) continue;
        const Track& tr = s.audioTracks[size_t(r.track)];
        r.volume.rebase(tr.volumeAuto, tr.volumeDb);
        r.pan.rebase(tr.panAuto, tr.pan);
        if (r.surround) {
            r.x.rebase(tr.surroundXAuto, tr.surround.x);
            r.y.rebase(tr.surroundYAuto, tr.surround.y);
            r.z.rebase(tr.surroundZAuto, tr.surround.z);
        }
    }
    for (LaneRecording& lr : lanes_) {
        if (lr.valid && !lr.valid(s)) continue;
        const double still = lr.stillIn ? lr.stillIn(s) : 0;  // (before laneIn, which may add an unstored parameter)
        if (const Param* lane = lr.laneIn(s)) lr.rec.rebase(*lane, lr.stillIn ? still : lane->value);
    }
    liveState_.clear();
}

bool MixerPanel::captureInsert(const EditorState::ParameterMove& m) {
    const Sequence* seq = state_->sequence();
    if (!recordingAutomation() || !seq || !m.owner || m.owner == seq->id) return false;  // the master has no mode
    // A track or bus writing its automation.
    AutomationMode mode = AutomationMode::Off;
    for (const Track& tr : seq->audioTracks)
        if (tr.id == m.owner) mode = trackAutomation(tr);
    for (const Bus& b : seq->buses)
        if (b.id == m.owner) mode = busAutomation(b);
    if (!writes(mode)) return false;
    const QString id = QStringLiteral("insert-%1-%2-%3").arg(m.owner).arg(m.effect).arg(QString::fromStdString(m.param));
    auto lane = std::find_if(lanes_.begin(), lanes_.end(), [&](const LaneRecording& l) { return l.id == id; });
    if (lane == lanes_.end()) {
        // Its lane as it is (the pass plays over the project without changing it).
        const Effect* e = edit::ownedEffect(const_cast<Sequence&>(*seq), m.owner, m.effect);
        if (!e) return false;
        Param before(m.before);
        if (const auto p = e->params.find(m.param); p != e->params.end()) before = p->second;
        // Only what is moved is written: Write as Latch, from the first move; a switch or choice as Latch in Touch too.
        const AutomationMode rec = m.toggle || mode == AutomationMode::Write ? AutomationMode::Latch : mode;
        const FrameTime second = FrameTime(std::lround(seq->fpsValue()));
        const FrameTime window = std::max<FrameTime>(1, second / 2);  // a control without a held state counts as held this long
        const Id owner = m.owner, effect = m.effect;
        const std::string param = m.param;
        lanes_.push_back({id, AutomationRecorder(rec, before, before.value, lastTick_, second),
                          m.toggle && m.whole ? 0.25 : std::max(1e-9, m.tolerance),
                          m.toggle,
                          [this, id] {
                              const auto it = insertMoves_.find(id);
                              return it == insertMoves_.end() ? 0.0 : it->second.value;
                          },
                          [this, id, window] {
                              const auto it = insertMoves_.find(id);
                              if (it == insertMoves_.end()) return false;
                              return it->second.held ? it->second.held() : lastTick_ - it->second.at < window;
                          },
                          [](double) {},  // its Inspector row and plugin editor ask liveInsert()
                          [owner, effect, param](Sequence& s, double v) {
                              if (Effect* fx = edit::ownedEffect(s, owner, effect)) fx->params[param] = Param(v);
                          },
                          [owner, effect, param, fallback = before.value](Sequence& s) -> Param* {
                              // (not stored yet: added at its default, which stays if nothing is written to it)
                              Effect* fx = edit::ownedEffect(s, owner, effect);
                              return fx ? &fx->params.try_emplace(param, Param(fallback)).first->second : nullptr;
                          },
                          {}, {}, false, 0.0});
        lanes_.back().whole = m.toggle && m.whole;
        lanes_.back().stillIn = [owner, effect, param, fallback = before.value](const Sequence& s) {
            if (const Effect* fx = edit::ownedEffect(const_cast<Sequence&>(s), owner, effect))
                if (const auto it = fx->params.find(param); it != fx->params.end()) return it->second.value;
            return fallback;  // (not stored: its default)
        };
        lane = lanes_.end() - 1;
    }
    lane->shown = m.value;
    insertMoves_[id] = {m.value, lastTick_, m.held};
    // Heard at once, not at the next frame.
    pendingLive_[id] = {lane->live, m.value};
    liveState_.clear();
    emit liveMix(liveOverlay());
    return true;
}

void MixerPanel::setSendLevel(int track, int send, double db) {
    // While a pass is written the recorder samples the send's level; nothing else is edited meanwhile.
    if (recordingAutomation()) {
        if (!recordingLane(QStringLiteral("send-%1-%2").arg(track).arg(send))) refresh();
        return;
    }
    const FrameTime at = state_->playhead();
    const bool ok = state_->edit(
        tr("Send Level"),
        [track, send, db, at](Project&, Sequence& s) {
            if (track >= int(s.audioTracks.size()) || send >= int(s.audioTracks[size_t(track)].sends.size())) return false;
            const Track& t = s.audioTracks[size_t(track)];
            Send& sd = s.audioTracks[size_t(track)].sends[size_t(send)];
            if (sd.levelAuto.animated() && readsLanes(t)) {
                if (sd.levelAuto.at(at) == db && sd.levelAuto.keyAt(at)) return false;
                sd.levelAuto.set(at, db);
                return true;
            }
            if (sd.levelDb == db) return false;
            sd.levelDb = db;
            return true;
        },
        mergeKey_.isEmpty() ? QStringLiteral("send-level-%1-%2").arg(track).arg(send) : mergeKey_);
    if (!ok) refresh();
}

void MixerPanel::setBusVolume(Id bus, double db) {
    if (recordingAutomation()) {
        if (!recordingLane(QStringLiteral("bus-volume-%1").arg(bus))) refresh();
        return;
    }
    const FrameTime at = state_->playhead();
    const bool ok = state_->edit(
        tr("Bus Volume"),
        [bus, db, at](Project&, Sequence& s) {
            for (Bus& b : s.buses) {
                if (b.id != bus) continue;
                if (b.volumeAuto.animated() && readsLanes(b)) {
                    if (b.volumeAuto.at(at) == db && b.volumeAuto.keyAt(at)) return false;
                    b.volumeAuto.set(at, db);
                    return true;
                }
                if (b.volumeDb == db) return false;
                b.volumeDb = db;
                return true;
            }
            return false;
        },
        mergeKey_.isEmpty() ? QStringLiteral("bus-volume-%1").arg(bus) : mergeKey_);
    if (!ok) refresh();
}

void MixerPanel::setBusPan(Id bus, double pan) {
    if (recordingAutomation()) {
        if (!recordingLane(QStringLiteral("bus-pan-%1").arg(bus))) refresh();
        return;
    }
    const FrameTime at = state_->playhead();
    const bool ok = state_->edit(
        tr("Bus Pan"),
        [bus, pan, at](Project&, Sequence& s) {
            for (Bus& b : s.buses) {
                if (b.id != bus) continue;
                if (b.panAuto.animated() && readsLanes(b)) {
                    if (b.panAuto.at(at) == pan && b.panAuto.keyAt(at)) return false;
                    b.panAuto.set(at, pan);
                    return true;
                }
                if (b.pan == pan) return false;
                b.pan = pan;
                return true;
            }
            return false;
        },
        mergeKey_.isEmpty() ? QStringLiteral("bus-pan-%1").arg(bus) : mergeKey_);
    if (!ok) refresh();
}

void MixerPanel::setBusMute(Id bus, bool on) {
    if (recordingAutomation()) {
        // Written as a switch from this change (Latch and Touch), or all along (Write).
        const QString id = QStringLiteral("bus-mute-%1").arg(bus);
        if (recordingLane(id)) touched_.insert(id);
        else refresh();
        return;
    }
    const FrameTime at = state_->playhead();
    const bool ok = state_->edit(on ? tr("Mute Bus") : tr("Unmute Bus"), [bus, on, at](Project&, Sequence& s) {
        for (Bus& b : s.buses)
            if (b.id == bus) {
                // As a track's: a bus reading its mute lane switches the lane at the playhead.
                const bool lane = readsLanes(b) && b.muteAuto.animated();
                if (busMutedAt(b, double(at)) == on) return false;
                if (!on && b.muted) b.muted = false;
                if (lane && laneMuted(b.muteAuto, false, double(at)) != on) switchFrom(b.muteAuto, at, on ? 1 : 0);
                else if (on) b.muted = true;
                return true;
            }
        return false;
    });
    if (!ok) refresh();
}

std::vector<std::string> MixerPanel::audioFolders() const {
    std::vector<std::string> out;
    if (const Sequence* seq = state_->sequence())
        for (const Track& t : seq->audioTracks)
            if (!t.folder.empty() && std::find(out.begin(), out.end(), t.folder) == out.end()) out.push_back(t.folder);
    return out;
}

// A folder's fader: a VCA adding its level to every track in the folder, as Fairlight's and Pro Tools' VCAs do.
MixerPanel::FolderStrip MixerPanel::makeFolderStrip(const std::string& folder) {
    FolderStrip f;
    f.folder = folder;
    auto* box = new QFrame(stripHost_);
    box->setObjectName(QStringLiteral("mixerFolderStrip"));
    box->setFixedWidth(kStripWidth);
    box->setStyleSheet(QStringLiteral("#mixerFolderStrip { border-top: 3px solid #d8a85a; }"));
    f.box = box;
    auto* v = new QVBoxLayout(box);
    v->setContentsMargins(4, 4, 4, 4);
    v->setSpacing(3);
    f.name = smallLabel(box, 11);
    QFont nf = f.name->font();
    nf.setBold(true);
    f.name->setFont(nf);
    f.name->setText(f.name->fontMetrics().elidedText(QString::fromStdString(folder), Qt::ElideRight, kStripWidth - 12));
    f.name->setToolTip(tr("Folder %1: its fader raises or lowers all of its tracks").arg(QString::fromStdString(folder)));
    v->addWidget(f.name);
    auto* tag = smallLabel(box);
    tag->setText(tr("VCA"));
    v->addWidget(tag);
    f.fader = new QSlider(Qt::Vertical, box);
    f.fader->setObjectName(QStringLiteral("folderFader"));
    f.fader->setRange(kFaderMin, kFaderMax);
    f.fader->setPageStep(30);
    f.fader->setMinimumHeight(80);
    f.fader->setToolTip(tr("Folder level, added to each of its tracks (double-click for 0 dB)"));
    f.fader->installEventFilter(this);
    v->addWidget(f.fader, 1, Qt::AlignHCenter);
    f.dbLabel = smallLabel(box);
    v->addWidget(f.dbLabel);
    connect(f.fader, &QSlider::valueChanged, this, [this, folder](int value) {
        const double db = value / 10.0;
        state_->edit(tr("Folder Level"), [folder, db](Project&, Sequence& s) {
            if (edit::folderGain(s, TrackKind::Audio, folder) == db) return false;
            edit::setFolderGain(s, TrackKind::Audio, folder, db);
            return true;
        }, QStringLiteral("folder-volume-") + QString::fromStdString(folder));
    });
    return f;
}

void MixerPanel::syncToProject() {
    if (recordingAutomation()) passBaseChanged_ = true;
    const Sequence* seq = state_->sequence();
    const size_t count = seq ? seq->audioTracks.size() : 0;
    const size_t buses = seq ? seq->buses.size() : 0;
    std::vector<std::string> shown;
    for (const FolderStrip& f : folderStrips_) shown.push_back(f.folder);
    bool sendsChanged = false;
    for (size_t i = 0; seq && i < count && i < strips_.size(); ++i) sendsChanged = sendsChanged || seq->audioTracks[i].sends.size() != strips_[i].sends.size();
    if (count != strips_.size() || buses != busStrips_.size() || shown != audioFolders() || sendsChanged)
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
    for (FolderStrip& f : folderStrips_) {
        stripLayout_->removeWidget(f.box);
        f.box->hide();
        f.box->deleteLater();
    }
    folderStrips_.clear();
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
    for (const std::string& folder : audioFolders()) {
        folderStrips_.push_back(makeFolderStrip(folder));
        stripLayout_->insertWidget(stripLayout_->count() - 1, folderStrips_.back().box);
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
    for (FolderStrip& f : folderStrips_) {
        const double db = edit::folderGain(*seq, TrackKind::Audio, f.folder);
        if (!f.fader->isSliderDown()) {
            const QSignalBlocker block(f.fader);
            f.fader->setValue(int(std::lround(std::clamp(db, -60.0, 12.0) * 10.0)));
        }
        f.dbLabel->setText(dbText(db));
    }
    for (size_t i = 0; i < strips_.size() && i < seq->audioTracks.size(); ++i) {
        const Track& t = seq->audioTracks[i];
        Strip& s = strips_[i];
        const QString name = t.name.empty() ? tr("A%1").arg(i + 1) : QString::fromStdString(t.name);
        s.name->setText(s.name->fontMetrics().elidedText(name, Qt::ElideRight, kStripWidth - 12));
        s.name->setToolTip(name);
        // The automation at the playhead on tracks that read it; a fader being held stays where the hand is.
        const double db = trackVolumeAt(t, double(state_->playhead())), pan = trackPanAt(t, double(state_->playhead()));
        if (!s.fader->isSliderDown()) {
            const QSignalBlocker block(s.fader);
            s.fader->setValue(int(std::lround(std::clamp(db, -60.0, 12.0) * 10.0)));
            s.dbLabel->setText(dbText(db));
        }
        if (!s.pan->isSliderDown()) {
            const QSignalBlocker block(s.pan);
            s.pan->setValue(int(std::lround(std::clamp(pan, -1.0, 1.0) * 100.0)));
        }
        s.panLabel->setText(panText(s.pan->value()));
        {
            const QSignalBlocker block(s.automation);
            s.automation->setCurrentIndex(int(trackAutomation(t)));
        }
        s.automation->setEnabled(!recordingAutomation());
        // In surround, a track going straight to the master is placed among the speakers;
        // one going to a bus is panned in stereo into it.
        const bool surround = layoutChannels(seq->audioLayout) > 2 && !t.output;
        s.pan->setVisible(!surround);
        s.panLabel->setVisible(!surround);
        s.surround->setVisible(surround);
        s.surround->setSpeakerLayout(seq->audioLayout);
        if (!s.surround->isDragging()) s.surround->setPan(trackSurroundAt(t, double(state_->playhead())));
        s.surround->setAnimated(surroundAnimated(t));
        {
            // Static mute, or the mute lane at the playhead on tracks that read it.
            const QSignalBlocker block(s.mute);
            s.mute->setChecked(trackMutedAt(t, double(state_->playhead())));
        }
        for (size_t k = 0; k < s.sends.size() && k < t.sends.size(); ++k) {
            const Send& sd = t.sends[k];
            Strip::SendRow& row = s.sends[k];
            QString busName = tr("(gone)");
            for (const Bus& b : seq->buses)
                if (b.id == sd.bus) busName = QString::fromStdString(b.name);
            const QString label = busName + (sd.preFader ? tr(" pre") : QString()) + (sd.muted ? tr(" (off)") : QString());
            row.name->setText(row.name->fontMetrics().elidedText(label, Qt::ElideRight, kStripWidth - 10));
            const double db = sendLevelAt(t, sd, double(state_->playhead()));
            row.level->setToolTip(tr("Send to %1: %2, %3 (double-click for 0 dB; right-click for pre-fader, mute or remove)")
                                      .arg(busName, dbText(db), sd.preFader ? tr("before the fader") : tr("after the fader")));
            if (!row.level->isSliderDown()) {
                const QSignalBlocker block(row.level);
                row.level->setValue(int(std::lround(std::clamp(db, -60.0, 12.0) * 10.0)));
            }
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
        // Fader, pan and mute as their lanes give them at the playhead on a bus that reads them.
        const double at = double(state_->playhead());
        const double db = busVolumeAt(b, at);
        if (!st.fader->isSliderDown()) {
            const QSignalBlocker block(st.fader);
            st.fader->setValue(int(std::lround(std::clamp(db, -60.0, 12.0) * 10.0)));
        }
        st.dbLabel->setText(dbText(db));
        if (!st.pan->isSliderDown()) {
            const QSignalBlocker block(st.pan);
            st.pan->setValue(int(std::lround(std::clamp(busPanAt(b, at), -1.0, 1.0) * 100.0)));
        }
        {
            const QSignalBlocker block(st.mute);
            st.mute->setChecked(busMutedAt(b, at));
        }
        {
            const QSignalBlocker block(st.automation);
            st.automation->setCurrentIndex(int(busAutomation(b)));
        }
        st.automation->setEnabled(!recordingAutomation());
        st.pan->setVisible(layoutChannels(seq->audioLayout) <= 2);
        st.surround->setVisible(layoutChannels(seq->audioLayout) > 2);
        st.surround->setSpeakerLayout(seq->audioLayout);
        st.surround->setPan(b.surround);
    }
    masterFx_->setText(seq->masterEffects.empty() ? tr("FX") : tr("FX %1").arg(seq->masterEffects.size()));
    {
        const QSignalBlocker block(masterFader_);
        masterFader_->setValue(int(std::lround(std::clamp(seq->masterVolumeDb, -60.0, 12.0) * 10.0)));
    }
    masterDb_->setText(dbText(seq->masterVolumeDb));
    if (recordingAutomation()) restoreControls();  // (the project does not hold what a pass plays)
}

void MixerPanel::setVolume(int index, double db) {
    if (index < int(strips_.size()))
        strips_[index].dbLabel->setText(dbText(db));
    // While automation is written the recorder samples the faders; nothing else is edited meanwhile.
    if (recordingAutomation()) {
        if (std::none_of(recording_.begin(), recording_.end(), [&](const Recording& r) { return r.track == index; })) refresh();
        return;
    }
    // On a track playing its volume lane, the fader sets the lane at the playhead.
    const FrameTime at = state_->playhead();
    const bool ok = state_->edit(
        tr("Track Volume"),
        [index, db, at](Project&, Sequence& s) {
            if (index >= int(s.audioTracks.size()))
                return false;
            Track& t = s.audioTracks[size_t(index)];
            if (t.volumeAuto.animated() && readsLanes(t)) {
                if (t.volumeAuto.at(at) == db && t.volumeAuto.keyAt(at)) return false;
                t.volumeAuto.set(at, db);
                return true;
            }
            if (t.volumeDb == db)
                return false;
            t.volumeDb = db;
            return true;
        },
        mergeKey_.isEmpty() ? QStringLiteral("track-volume-%1").arg(index) : mergeKey_);
    if (!ok)
        refresh();
}

void MixerPanel::setPan(int index, double pan) {
    if (index < int(strips_.size()))
        strips_[index].panLabel->setText(panText(int(std::lround(pan * 100.0))));
    if (recordingAutomation()) {
        if (std::none_of(recording_.begin(), recording_.end(), [&](const Recording& r) { return r.track == index; })) refresh();
        return;
    }
    const FrameTime at = state_->playhead();
    const bool ok = state_->edit(
        tr("Track Pan"),
        [index, pan, at](Project&, Sequence& s) {
            if (index >= int(s.audioTracks.size()))
                return false;
            Track& t = s.audioTracks[size_t(index)];
            if (t.panAuto.animated() && readsLanes(t)) {
                if (t.panAuto.at(at) == pan && t.panAuto.keyAt(at)) return false;
                t.panAuto.set(at, pan);
                return true;
            }
            if (t.pan == pan)
                return false;
            t.pan = pan;
            return true;
        },
        mergeKey_.isEmpty() ? QStringLiteral("track-pan-%1").arg(index) : mergeKey_);
    if (!ok)
        refresh();
}

void MixerPanel::setAutomationMode(int index, int mode) {
    state_->edit(tr("Automation Mode"), [index, mode](Project&, Sequence& s) {
        if (index >= int(s.audioTracks.size()) || s.audioTracks[size_t(index)].automation == mode) return false;
        s.audioTracks[size_t(index)].automation = mode;
        return true;
    });
}

void MixerPanel::followAutomation() {
    if (recordingAutomation()) return;
    const Sequence* seq = state_->sequence();
    if (!seq) return;
    const double at = double(state_->playhead());
    // Mutes, send levels and buses follow their lanes too.
    for (size_t i = 0; i < strips_.size() && i < seq->audioTracks.size(); ++i) {
        const Track& t = seq->audioTracks[i];
        if (!readsLanes(t)) continue;
        Strip& s = strips_[i];
        if (t.muteAuto.animated()) {
            const QSignalBlocker block(s.mute);
            s.mute->setChecked(trackMutedAt(t, at));
        }
        for (size_t k = 0; k < s.sends.size() && k < t.sends.size(); ++k)
            if (t.sends[k].levelAuto.animated() && !s.sends[k].level->isSliderDown()) {
                const QSignalBlocker block(s.sends[k].level);
                s.sends[k].level->setValue(int(std::lround(std::clamp(sendLevelAt(t, t.sends[k], at), -60.0, 12.0) * 10.0)));
            }
    }
    for (size_t i = 0; i < busStrips_.size() && i < seq->buses.size(); ++i) {
        const Bus& b = seq->buses[i];
        if (!readsLanes(b)) continue;
        BusStrip& st = busStrips_[i];
        if (b.volumeAuto.animated() && !st.fader->isSliderDown()) {
            const QSignalBlocker block(st.fader);
            st.fader->setValue(int(std::lround(std::clamp(busVolumeAt(b, at), -60.0, 12.0) * 10.0)));
            st.dbLabel->setText(dbText(busVolumeAt(b, at)));
        }
        if (b.panAuto.animated() && !st.pan->isSliderDown()) {
            const QSignalBlocker block(st.pan);
            st.pan->setValue(int(std::lround(std::clamp(busPanAt(b, at), -1.0, 1.0) * 100.0)));
        }
        if (b.muteAuto.animated()) {
            const QSignalBlocker block(st.mute);
            st.mute->setChecked(busMutedAt(b, at));
        }
    }
    for (size_t i = 0; i < strips_.size() && i < seq->audioTracks.size(); ++i) {
        const Track& t = seq->audioTracks[i];
        if (!readsLanes(t)) continue;
        Strip& s = strips_[i];
        if (surroundAnimated(t) && !s.surround->isDragging()) s.surround->setPan(trackSurroundAt(t, at));
        if (!t.volumeAuto.animated() && !t.panAuto.animated()) continue;
        if (!s.fader->isSliderDown()) {
            const double db = trackVolumeAt(t, at);
            const QSignalBlocker block(s.fader);
            s.fader->setValue(int(std::lround(std::clamp(db, -60.0, 12.0) * 10.0)));
            s.dbLabel->setText(dbText(db));
        }
        if (!s.pan->isSliderDown()) {
            const QSignalBlocker block(s.pan);
            s.pan->setValue(int(std::lround(std::clamp(trackPanAt(t, at), -1.0, 1.0) * 100.0)));
            s.panLabel->setText(panText(s.pan->value()));
        }
    }
}

void MixerPanel::playbackStarted(FrameTime t) {
    const Sequence* seq = state_->sequence();
    if (recordingAutomation() || !seq) return;  // (a stopped pass waiting for a drag: this playback is not recorded)
    for (size_t i = 0; i < seq->audioTracks.size() && i < strips_.size(); ++i) {
        const Track& tr = seq->audioTracks[i];
        const AutomationMode m = trackAutomation(tr);
        if (m != AutomationMode::Write && m != AutomationMode::Latch && m != AutomationMode::Touch) continue;
        const FrameTime glide = FrameTime(std::lround(seq->fpsValue()));  // a second back to what was there
        const bool surround = layoutChannels(seq->audioLayout) > 2 && !tr.output;
        recording_.push_back({int(i), m, AutomationRecorder(m, tr.volumeAuto, tr.volumeDb, t, glide),
                              AutomationRecorder(m, tr.panAuto, tr.pan, t, glide), tr.volumeDb, tr.pan, tr.volumeAuto, tr.panAuto, surround,
                              AutomationRecorder(m, tr.surroundXAuto, tr.surround.x, t, glide),
                              AutomationRecorder(m, tr.surroundYAuto, tr.surround.y, t, glide),
                              AutomationRecorder(m, tr.surroundZAuto, tr.surround.z, t, glide), tr.surround, tr.surroundXAuto,
                              tr.surroundYAuto, tr.surroundZAuto});
        recording_.back().trackId = tr.id;
    }
    // The other lanes: track mutes and send levels, bus faders, pans and mutes. Mutes are switches: Touch writes them
    // from the first change to the stop, as Latch does.
    const FrameTime second = FrameTime(std::lround(seq->fpsValue()));
    auto addLane = [&](const QString& id, AutomationMode m, const Param& lane, double still, FrameTime glide, double tolerance, bool hold,
                       std::function<double()> value, std::function<bool()> held, std::function<void(double)> show,
                       std::function<void(Sequence&, double)> live, std::function<Param*(Sequence&)> laneIn) {
        lanes_.push_back({id, AutomationRecorder(m, lane, still, t, glide), tolerance, hold, std::move(value), std::move(held), std::move(show),
                          std::move(live), std::move(laneIn), {}, {}, false, 0.0});
        lanes_.back().whole = hold;  // (mutes: 0 or 1)
    };
    auto switchMode = [](AutomationMode m) { return m == AutomationMode::Touch ? AutomationMode::Latch : m; };
    auto setSlider = [](QAbstractSlider* sl, double v, double scale, double lo, double hi) {
        const QSignalBlocker block(sl);
        sl->setValue(int(std::lround(std::clamp(v, lo, hi) * scale)));
    };
    for (size_t i = 0; i < seq->audioTracks.size() && i < strips_.size(); ++i) {
        const Track& tr = seq->audioTracks[i];
        const AutomationMode m = trackAutomation(tr);
        if (!writes(m)) continue;
        const int index = int(i);
        if (!tr.muted) {
            const QString id = QStringLiteral("track-mute-%1").arg(index);
            addLane(
                id, switchMode(m), tr.muteAuto, 0, 1, 0.25, true,
                [this, index] { return index < int(strips_.size()) && strips_[size_t(index)].mute->isChecked() ? 1.0 : 0.0; },
                [this, id] { return touched_.count(id) > 0; },
                [this, index](double v) {
                    if (index >= int(strips_.size())) return;
                    const QSignalBlocker block(strips_[size_t(index)].mute);
                    strips_[size_t(index)].mute->setChecked(v >= 0.5);
                },
                [index](Sequence& s, double v) {
                    if (index >= int(s.audioTracks.size())) return;
                    s.audioTracks[size_t(index)].muteAuto = Param();
                    s.audioTracks[size_t(index)].muted = v >= 0.5;
                },
                [index](Sequence& s) { return index < int(s.audioTracks.size()) ? &s.audioTracks[size_t(index)].muteAuto : nullptr; });
            lanes_.back().valid = [index, id = tr.id](const Sequence& s) {
                return index < int(s.audioTracks.size()) && s.audioTracks[size_t(index)].id == id;
            };
        }
        for (size_t k = 0; k < tr.sends.size() && k < strips_[i].sends.size(); ++k) {
            const int send = int(k);
            // (by index, checked each time: the strips may have been rebuilt meanwhile)
            auto row = [this, index, send]() -> QSlider* {
                if (index >= int(strips_.size()) || send >= int(strips_[size_t(index)].sends.size())) return nullptr;
                return strips_[size_t(index)].sends[size_t(send)].level;
            };
            auto sendIn = [index, send](Sequence& s) -> Send* {
                if (index >= int(s.audioTracks.size()) || send >= int(s.audioTracks[size_t(index)].sends.size())) return nullptr;
                return &s.audioTracks[size_t(index)].sends[size_t(send)];
            };
            addLane(
                QStringLiteral("send-%1-%2").arg(index).arg(send), m, tr.sends[k].levelAuto, tr.sends[k].levelDb, second, 0.1, false,
                [row, still = tr.sends[k].levelDb] {
                    // The slider stops at -60 dB: a send set lower (-96, off) stays there unless raised.
                    const double v = row() ? row()->value() / 10.0 : still;
                    return v <= -60 && still < -60 ? still : v;
                },
                [row] { return row() && row()->isSliderDown(); },
                [row, setSlider](double v) {
                    if (QSlider* sl = row()) setSlider(sl, v, 10, -60, 12);
                },
                [sendIn](Sequence& s, double v) {
                    if (Send* sd = sendIn(s)) sd->levelAuto = Param(), sd->levelDb = v;
                },
                [sendIn](Sequence& s) {
                    Send* sd = sendIn(s);
                    return sd ? &sd->levelAuto : nullptr;
                });
            lanes_.back().stillIn = [sendIn, still = tr.sends[k].levelDb](const Sequence& s) {
                const Send* sd = sendIn(const_cast<Sequence&>(s));
                return sd ? sd->levelDb : still;
            };
            lanes_.back().valid = [index, send, id = tr.id, bus = tr.sends[k].bus](const Sequence& s) {
                if (index >= int(s.audioTracks.size()) || s.audioTracks[size_t(index)].id != id) return false;
                const auto& sends = s.audioTracks[size_t(index)].sends;
                return send < int(sends.size()) && sends[size_t(send)].bus == bus;
            };
        }
    }
    const bool surroundMix = layoutChannels(seq->audioLayout) > 2;
    for (size_t j = 0; j < seq->buses.size() && j < busStrips_.size(); ++j) {
        const Bus& b = seq->buses[j];
        const AutomationMode m = busAutomation(b);
        if (!writes(m)) continue;
        const Id bus = b.id;
        // Its strip by id, looked up each time (the strips may have been rebuilt meanwhile).
        auto strip = [this, bus]() -> BusStrip* {
            for (BusStrip& x : busStrips_)
                if (x.id == bus) return &x;
            return nullptr;
        };
        auto busIn = [bus](Sequence& s) -> Bus* {
            for (Bus& x : s.buses)
                if (x.id == bus) return &x;
            return nullptr;
        };
        addLane(
            QStringLiteral("bus-volume-%1").arg(bus), m, b.volumeAuto, b.volumeDb, second, 0.1, false,
            [strip, still = b.volumeDb] {
                const double v = strip() ? strip()->fader->value() / 10.0 : still;
                return v <= -60 && still < -60 ? still : v;  // (as a send's: lower than the fader goes stays)
            },
            [strip] { return strip() && strip()->fader->isSliderDown(); },
            [strip, setSlider](double v) {
                if (BusStrip* st = strip()) {
                    setSlider(st->fader, v, 10, -60, 12);
                    st->dbLabel->setText(dbText(v));
                }
            },
            [busIn](Sequence& s, double v) {
                if (Bus* x = busIn(s)) x->volumeAuto = Param(), x->volumeDb = v;
            },
            [busIn](Sequence& s) {
                Bus* x = busIn(s);
                return x ? &x->volumeAuto : nullptr;
            });
        lanes_.back().stillIn = [busIn, still = b.volumeDb](const Sequence& s) {
            const Bus* x = busIn(const_cast<Sequence&>(s));
            return x ? x->volumeDb : still;
        };
        if (!surroundMix)
            addLane(
                QStringLiteral("bus-pan-%1").arg(bus), m, b.panAuto, b.pan, second, 0.005, false,
                [strip, still = b.pan] { return strip() ? strip()->pan->value() / 100.0 : still; },
                [strip] { return strip() && strip()->pan->isSliderDown(); },
                [strip, setSlider](double v) {
                    if (BusStrip* st = strip()) setSlider(st->pan, v, 100, -1, 1);
                },
                [busIn](Sequence& s, double v) {
                    if (Bus* x = busIn(s)) x->panAuto = Param(), x->pan = v;
                },
                [busIn](Sequence& s) {
                    Bus* x = busIn(s);
                    return x ? &x->panAuto : nullptr;
                });
        if (!surroundMix)
            lanes_.back().stillIn = [busIn, still = b.pan](const Sequence& s) {
                const Bus* x = busIn(const_cast<Sequence&>(s));
                return x ? x->pan : still;
            };
        if (!b.muted) {
            const QString id = QStringLiteral("bus-mute-%1").arg(bus);
            addLane(
                id, switchMode(m), b.muteAuto, 0, 1, 0.25, true, [strip] { return strip() && strip()->mute->isChecked() ? 1.0 : 0.0; },
                [this, id] { return touched_.count(id) > 0; },
                [strip](double v) {
                    if (BusStrip* st = strip()) {
                        const QSignalBlocker block(st->mute);
                        st->mute->setChecked(v >= 0.5);
                    }
                },
                [busIn](Sequence& s, double v) {
                    if (Bus* x = busIn(s)) x->muteAuto = Param(), x->muted = v >= 0.5;
                },
                [busIn](Sequence& s) {
                    Bus* x = busIn(s);
                    return x ? &x->muteAuto : nullptr;
                });
        }
        if (m == AutomationMode::Write) busesFromWrite_.push_back(bus);
    }
    if (!recordingAutomation()) return;
    passSeq_ = seq->id;
    passGeneration_ = state_->projectGeneration();
    liveState_.clear();
    touched_.clear();
    for (Strip& s : strips_) s.automation->setEnabled(false);
    for (BusStrip& b : busStrips_) b.automation->setEnabled(false);
    playbackPosition(t);
}

void MixerPanel::playbackPosition(FrameTime t) {
    if (stopAt_ >= 0) return;  // stopped, the pass waiting to be written
    if (resume_ && !recordingAutomation()) {
        resume_ = false;
        playbackStarted(t);
        return;
    }
    if (!recordingAutomation()) {
        followAutomation();
        return;
    }
    lastTick_ = t;
    pendingLive_.clear();
    // After an edit: lanes whose track or send moved are left as they were, and the pass takes its gesture back.
    if (const Sequence* seq = state_->sequence()) {
        std::erase_if(recording_, [&](const Recording& r) {
            return r.track >= int(seq->audioTracks.size()) || seq->audioTracks[size_t(r.track)].id != r.trackId;
        });
        std::erase_if(lanes_, [&](const LaneRecording& l) { return l.valid && !l.valid(*seq); });
    }
    if (!recordingAutomation()) {
        discardPass();
        return;
    }
    if (passBaseChanged_) {
        passBaseChanged_ = false;
        rebaseRecorders();
    }
    struct Live {
        int track;
        bool volume, pan;
        double db, panValue;
        bool x = false, y = false, z = false, surround = false;
        SurroundPan position{};
    };
    std::vector<Live> live;
    QString key;
    for (Recording& r : recording_) {
        if (r.track >= int(strips_.size())) continue;
        Strip& st = strips_[size_t(r.track)];
        const double db = st.fader->value() / 10.0, pan = st.pan->value() / 100.0;
        const double showDb = r.volume.tick(t, db, st.fader->isSliderDown());
        const double showPan = r.pan.tick(t, pan, st.pan->isSliderDown());
        // Not writing: the fader shows (and the mix plays) the automation that was there.
        if (!r.volume.writing() && !st.fader->isSliderDown()) {
            const QSignalBlocker block(st.fader);
            st.fader->setValue(int(std::lround(std::clamp(showDb, -60.0, 12.0) * 10.0)));
            st.dbLabel->setText(dbText(showDb));
        }
        if (!r.pan.writing() && !st.pan->isSliderDown()) {
            const QSignalBlocker block(st.pan);
            st.pan->setValue(int(std::lround(std::clamp(showPan, -1.0, 1.0) * 100.0)));
            st.panLabel->setText(panText(st.pan->value()));
        }
        r.shownDb = r.volume.writing() || st.fader->isSliderDown() ? db : showDb;
        r.shownPan = r.pan.writing() || st.pan->isSliderDown() ? pan : showPan;
        Live lv{r.track, r.volume.writing(), r.pan.writing(), db, pan};
        if (r.surround) {
            // The panner: written while dragged (or all along in Write), showing what was there otherwise.
            const SurroundPan now = st.surround->pan();
            const bool held = st.surround->isDragging();
            SurroundPan shown = now;
            shown.x = r.x.tick(t, now.x, held);
            shown.y = r.y.tick(t, now.y, held);
            shown.z = r.z.tick(t, now.z, held);
            lv.x = r.x.writing(), lv.y = r.y.writing(), lv.z = r.z.writing();
            lv.surround = r.changedOther;  // (width, LFE and object as changed during the pass; the project's otherwise)
            lv.position = now;
            lv.position.width = r.position.width, lv.position.lfeDb = r.position.lfeDb, lv.position.object = r.position.object;
            if (!held && !(lv.x && lv.y && lv.z)) st.surround->setPan(shown);
            r.shownPosition = held ? now : shown;
            key += QStringLiteral("s%1%2%3,%4,%5,%6;").arg(int(lv.x)).arg(int(lv.y)).arg(int(lv.z)).arg(now.x).arg(now.y).arg(now.z);
        }
        live.push_back(lv);
        key += QStringLiteral("%1:%2%3%4,%5;").arg(r.track).arg(int(r.volume.writing())).arg(int(r.pan.writing())).arg(db).arg(pan);
    }
    // The other lanes: each control written while it is, showing its lane while it is not.
    std::vector<std::pair<std::function<void(Sequence&, double)>, double>> laneLive;
    for (LaneRecording& lr : lanes_) {
        const double v = lr.value();
        const bool held = lr.held();
        const double shown = lr.rec.tick(t, v, held);
        if (!lr.rec.writing() && !held) lr.show(shown);
        lr.shown = lr.rec.writing() || held ? v : shown;
        if (lr.rec.writing()) laneLive.emplace_back(lr.live, v);
        key += QStringLiteral("%1:%2,%3;").arg(lr.id).arg(int(lr.rec.writing())).arg(v);
    }
    if (key == liveState_) return;
    liveState_ = key;
    // What plays meanwhile: the fader on a lane being written, the lanes as they were otherwise.
    lastLive_ = [live, laneLive](Sequence& s) {
        for (const auto& [apply, v] : laneLive) apply(s, v);
        for (const Live& l : live) {
            if (l.track >= int(s.audioTracks.size())) continue;
            Track& tr = s.audioTracks[size_t(l.track)];
            if (l.volume) tr.volumeAuto = Param(), tr.volumeDb = l.db;
            if (l.pan) tr.panAuto = Param(), tr.pan = l.panValue;
            if (l.x) tr.surroundXAuto = Param(), tr.surround.x = l.position.x;
            if (l.y) tr.surroundYAuto = Param(), tr.surround.y = l.position.y;
            if (l.z) tr.surroundZAuto = Param(), tr.surround.z = l.position.z;
            if (l.surround) tr.surround.width = l.position.width, tr.surround.lfeDb = l.position.lfeDb, tr.surround.object = l.position.object;
        }
    };
    emit liveMix(liveOverlay());
}

std::function<void(Sequence&)> MixerPanel::liveOverlay() const {
    if (pendingLive_.empty()) return lastLive_;
    return [base = lastLive_, pending = pendingLive_](Sequence& s) {
        if (base) base(s);
        for (const auto& [key, move] : pending) move.first(s, move.second);
    };
}

Sequence MixerPanel::heard() const {
    Sequence s = state_->sequence() ? *state_->sequence() : Sequence();
    if (const auto apply = liveOverlay()) apply(s);
    return s;
}

void MixerPanel::restoreControls() {
    // What the pass shows on its controls, put back after the strips were refreshed or rebuilt meanwhile (only on the
    // tracks and sends they still are).
    const Sequence* seq = state_->sequence();
    if (!seq || seq->id != passSeq_) return;
    for (const Recording& r : recording_) {
        if (r.track >= int(strips_.size()) || r.track >= int(seq->audioTracks.size()) || seq->audioTracks[size_t(r.track)].id != r.trackId)
            continue;
        Strip& st = strips_[size_t(r.track)];
        if (!st.fader->isSliderDown()) {
            const QSignalBlocker block(st.fader);
            st.fader->setValue(int(std::lround(std::clamp(r.shownDb, -60.0, 12.0) * 10.0)));
            st.dbLabel->setText(dbText(r.shownDb));
        }
        if (!st.pan->isSliderDown()) {
            const QSignalBlocker block(st.pan);
            st.pan->setValue(int(std::lround(std::clamp(r.shownPan, -1.0, 1.0) * 100.0)));
            st.panLabel->setText(panText(st.pan->value()));
        }
        if (r.surround && st.surround && !st.surround->isDragging()) st.surround->setPan(r.shownPosition);
    }
    // (A switch's "held" means pressed during the pass: it shows what the pass plays all the same.)
    for (const LaneRecording& lr : lanes_)
        if ((!lr.valid || lr.valid(*seq)) && (lr.hold || !lr.held())) lr.show(lr.shown);
}

void MixerPanel::playbackStopped(FrameTime t) {
    resume_ = false;
    if (!recordingAutomation() || stopAt_ >= 0) return;  // (already waiting: written at the first stop)
    stopAt_ = t;
    emit liveMix({});  // what plays from now on is the project, even if the pass waits to be written
    finishWhenFree();
}

void MixerPanel::finishWhenFree() {
    if (!recordingAutomation() || stopAt_ < 0) return;
    // Another component's gesture (a drag held through the stop) is left to finish: the pass is written after it.
    if (passWrote() && state_->inGesture()) {
        QTimer::singleShot(50, this, [this] { finishWhenFree(); });
        return;
    }
    const FrameTime t = stopAt_;
    stopAt_ = -1;
    finishPass(t, true);
}

void MixerPanel::clearPass() {
    recording_.clear();
    lanes_.clear();
    touched_.clear();
    busesFromWrite_.clear();
    liveState_.clear();
    lastLive_ = nullptr;
    insertMoves_.clear();
    pendingLive_.clear();
    stopAt_ = -1;
    passBaseChanged_ = false;
}

void MixerPanel::discardPass() {
    clearPass();
    emit liveMix({});
    refresh();
}

void MixerPanel::finishPass(FrameTime t, bool final) {
    // One edit of the project as it is now (edits made during the pass included): the lanes the pass wrote, laid over
    // what those lanes hold now, on the sequence it was recorded on. Lanes nothing was written to are left alone, and
    // a pass that wrote nothing makes no edit.
    emit liveMix({});
    const bool commit = passWrote() && state_->projectGeneration() == passGeneration_;
    // The pass ends before its edit, so what refreshes for the edit (the strips, Inspector rows, plugin editors) reads
    // the project, not the pass.
    std::vector<Recording> recording = std::move(recording_);
    std::vector<LaneRecording> lanes = std::move(lanes_);
    const std::vector<Id> fromWrite = busesFromWrite_;
    const Id sequence = passSeq_;
    clearPass();
    if (commit) {
        state_->edit(tr("Write Automation"), [&](Project& p, Sequence&) {
            Sequence* s = p.findSequence(sequence);
            if (!s) return false;
            bool wrote = false;
            auto write = [&](AutomationRecorder& rec, Param& lane, double still, double tolerance, bool hold, bool whole) {
                if (!rec.wrote()) return;
                wrote = true;
                rec.rebase(lane, still);
                Param out = rec.finish(t, tolerance);
                if (hold) out = switchLane(out, whole);
                if (out.animated() || lane.animated()) lane = out;
            };
            for (Recording& r : recording) {
                if (r.track >= int(s->audioTracks.size()) || s->audioTracks[size_t(r.track)].id != r.trackId) continue;
                Track& tr = s->audioTracks[size_t(r.track)];
                write(r.volume, tr.volumeAuto, tr.volumeDb, 0.1, false, false);
                write(r.pan, tr.panAuto, tr.pan, 0.005, false, false);
                if (r.surround) {
                    write(r.x, tr.surroundXAuto, tr.surround.x, 0.005, false, false);
                    write(r.y, tr.surroundYAuto, tr.surround.y, 0.005, false, false);
                    write(r.z, tr.surroundZAuto, tr.surround.z, 0.005, false, false);
                }
                if (r.changedOther) {
                    tr.surround.width = r.position.width, tr.surround.lfeDb = r.position.lfeDb, tr.surround.object = r.position.object;
                    wrote = true;
                }
            }
            for (LaneRecording& lr : lanes) {
                if (lr.valid && !lr.valid(*s)) continue;
                const double still = lr.stillIn ? lr.stillIn(*s) : 0;
                if (Param* lane = lr.laneIn(*s)) write(lr.rec, *lane, lr.stillIn ? still : lane->value, lr.tolerance, lr.hold, lr.whole);
            }
            // Write hands over to Touch, so the next pass does not write over this one unless a control is moved.
            if (wrote && final) {
                for (const Recording& r : recording)
                    if (r.mode == AutomationMode::Write && r.track < int(s->audioTracks.size()) && s->audioTracks[size_t(r.track)].id == r.trackId)
                        s->audioTracks[size_t(r.track)].automation = int(AutomationMode::Touch);
                for (Bus& b : s->buses)
                    if (std::find(fromWrite.begin(), fromWrite.end(), b.id) != fromWrite.end()) b.automation = int(AutomationMode::Touch);
            }
            return wrote;
        });
    }
    refresh();
}

void MixerPanel::setSurround(int index, const SurroundPan& pan, bool) {
    // While automation is written the recorder samples the panner's position; its width, LFE and object flag are
    // kept for the end of the pass. Other tracks' panners wait for the stop.
    if (recordingAutomation()) {
        bool recorded = false;
        for (Recording& r : recording_)
            if (r.track == index && r.surround) {
                recorded = true;
                if (r.position.width != pan.width || r.position.lfeDb != pan.lfeDb || r.position.object != pan.object) {
                    r.position.width = pan.width, r.position.lfeDb = pan.lfeDb, r.position.object = pan.object;
                    r.changedOther = true;
                    liveState_.clear();
                }
            }
        if (!recorded) refresh();
        return;
    }
    // A moving position is keyed at the playhead; its width, LFE and object flag stay the track's own.
    const FrameTime at = state_->playhead();
    state_->edit(
        tr("Track Surround Pan"),
        [index, pan, at](Project&, Sequence& s) {
            if (index >= int(s.audioTracks.size())) return false;
            Track& t = s.audioTracks[size_t(index)];
            if (surroundAnimated(t) && readsLanes(t)) {
                const SurroundPan was = t.surround;
                t.surround.width = pan.width, t.surround.lfeDb = pan.lfeDb, t.surround.object = pan.object;
                bool changed = !(t.surround == was);
                for (auto [lane, v] : {std::pair<Param*, double>{&t.surroundXAuto, pan.x}, {&t.surroundYAuto, pan.y}, {&t.surroundZAuto, pan.z}})
                    if (lane->keys.empty() || lane->at(at) != v || !lane->keyAt(at)) {
                        if (lane->keys.empty()) lane->addKey(at, v);  // (set() on an empty lane changes only its still value)
                        else lane->set(at, v);
                        changed = true;
                    }
                return changed;
            }
            if (t.surround == pan) return false;
            t.surround = pan;
            return true;
        },
        QStringLiteral("track-surround-%1").arg(index));
}

void MixerPanel::animateSurround(int index, bool on) {
    if (recordingAutomation()) {  // not while a pass is written: the edit would end it
        refresh();
        return;
    }
    const FrameTime at = state_->playhead();
    state_->edit(on ? tr("Animate Surround Position") : tr("Stop Surround Animation"), [index, on, at](Project&, Sequence& s) {
        if (index >= int(s.audioTracks.size())) return false;
        Track& t = s.audioTracks[size_t(index)];
        const SurroundPan here = trackSurroundAt(t, double(at));
        if (on) {
            if (surroundAnimated(t)) return false;
            t.surroundXAuto.addKey(at, here.x);
            t.surroundYAuto.addKey(at, here.y);
            t.surroundZAuto.addKey(at, here.z);
            if (!readsLanes(t)) t.automation = int(AutomationMode::Read);
            return true;
        }
        if (!surroundAnimated(t)) return false;
        // It stays where it is at the playhead.
        t.surround.x = here.x, t.surround.y = here.y, t.surround.z = here.z;
        t.surroundXAuto = Param(), t.surroundYAuto = Param(), t.surroundZAuto = Param();
        return true;
    });
    refresh();
}

void MixerPanel::setMute(int index, bool on) {
    if (recordingAutomation()) {
        // Written as a switch from this change (Latch and Touch), or all along (Write).
        const QString id = QStringLiteral("track-mute-%1").arg(index);
        if (recordingLane(id)) touched_.insert(id);
        else refresh();
        return;
    }
    // On a track reading a mute lane the button switches the lane at the playhead (to its next point), as the fader
    // keys its lane; a static mute is still turned off by it.
    const FrameTime at = state_->playhead();
    const bool ok = state_->edit(on ? tr("Mute Track") : tr("Unmute Track"), [index, on, at](Project&, Sequence& s) {
        if (index >= int(s.audioTracks.size())) return false;
        Track& t = s.audioTracks[size_t(index)];
        const bool lane = readsLanes(t) && t.muteAuto.animated();
        if (trackMutedAt(t, double(at)) == on) return false;
        if (!on && t.muted) t.muted = false;
        if (lane && laneMuted(t.muteAuto, false, double(at)) != on) switchFrom(t.muteAuto, at, on ? 1 : 0);
        else if (on) t.muted = true;
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
