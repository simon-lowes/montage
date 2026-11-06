#include "EqPanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "EditorState.h"
#include "EqDisplay.h"
#include "FlowLayout.h"
#include "Settings.h"
#include "Theme.h"
#include "core/EditOps.h"
#include "core/Effects.h"

namespace montage {

namespace {

// The band picked on each EQ, and the panels showing each, so the Inspector's and the large window's agree.
std::map<Id, std::string>& picked() {
    static std::map<Id, std::string> m;
    return m;
}
std::set<EqPanel*>& panels() {
    static std::set<EqPanel*> s;
    return s;
}
std::map<Id, QPointer<QWidget>>& windows() {
    static std::map<Id, QPointer<QWidget>> m;
    return m;
}

std::vector<EqDisplay::Band> bandsOf(const Effect& e, FrameTime t) {
    std::vector<EqDisplay::Band> out;
    for (const std::string& name : eqBands(e)) {
        EqDisplay::Band b;
        b.name = name;
        b.type = eqBandType(e, name);
        b.hz = std::clamp(e.p(name + "_hz", t, eqDefaultHz(name)), 20.0, 20000.0);
        b.db = e.p(name + "_db", t, 0);
        b.q = std::min(e.p(name + "_q", t, eqDefaultQ(b.type)), eqMaxQ(b.type));  // (as playback takes it)
        b.slope = std::clamp(int(std::lround(e.p(name + "_slope", t, 1))), 0, 3);
        b.on = e.p(name + "_on", t, 1) > 0.5;
        out.push_back(b);
    }
    return out;
}

QDoubleSpinBox* spin(QWidget* parent, const char* name, double lo, double hi, int decimals, double step, const QString& suffix) {
    auto* s = new QDoubleSpinBox(parent);
    s->setObjectName(QString::fromLatin1(name));
    s->setRange(lo, hi);
    s->setDecimals(decimals);
    s->setSingleStep(step);
    s->setSuffix(suffix);
    s->setKeyboardTracking(false);
    s->setAccelerated(true);
    return s;
}

// A label and its control, kept together when the strip wraps.
QWidget* labelled(QWidget* parent, const QString& text, QWidget* control) {
    auto* w = new QWidget(parent);
    auto* h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(3);
    auto* l = new QLabel(text, w);
    l->setBuddy(control);
    control->setParent(w);
    h->addWidget(l);
    h->addWidget(control);
    return w;
}

}  // namespace

EqPanel::EqPanel(EditorState* state, Id owner, Id effect, std::function<FrameTime()> time, bool large, QWidget* parent)
    : QWidget(parent), state_(state), owner_(owner), effect_(effect), time_(std::move(time)), large_(large) {
    setObjectName(QStringLiteral("eqPanel"));
    panels().insert(this);
    build();
    connect(state_, &EditorState::projectChanged, this, &EqPanel::refresh);
    connect(state_, &EditorState::playheadChanged, this, &EqPanel::refresh);
    connect(state_, &EditorState::heardAnalysis, display_, &EqDisplay::setSpectrum);
    refresh();
}

EqPanel::~EqPanel() {
    panels().erase(this);
    if (!soloing_.empty()) state_->setAudition({});  // (nothing held stays soloed)
    if (dragging_) state_->endGesture(true);
}

void EqPanel::build() {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);
    QSettings settings = appSettings();

    // Above the display: add a band, the range shown, the spectrum, the large window and the tips.
    auto* bar = new QHBoxLayout;
    bar->setSpacing(4);
    addBand_ = new QToolButton(this);
    addBand_->setObjectName(QStringLiteral("eqAddBand"));
    addBand_->setText(tr("Add Band"));
    addBand_->setToolTip(tr("Add a bell at 1 kHz (or double-click the display where you want one)"));
    connect(addBand_, &QToolButton::clicked, this, [this] {
        reshape(tr("Add EQ Band"), [](Effect& e) { return addEqBand(e, kEqBell, 1000, 0); });
    });
    bar->addWidget(addBand_);
    range_ = new QComboBox(this);
    range_->setObjectName(QStringLiteral("eqRange"));
    range_->setToolTip(tr("Gain shown above and below 0 dB"));
    range_->setAccessibleName(tr("Display range"));
    for (int db : {6, 12, 24, 30}) range_->addItem(tr("±%1 dB").arg(db), db);
    range_->setCurrentIndex(std::max(0, range_->findData(settings.value(QStringLiteral("eq/range"), 12).toInt())));
    connect(range_, &QComboBox::currentIndexChanged, this, [this] {
        display_->setRange(range_->currentData().toDouble());
        appSettings().setValue(QStringLiteral("eq/range"), range_->currentData().toInt());
    });
    bar->addWidget(range_);
    spectrum_ = new QCheckBox(tr("Spectrum"), this);
    spectrum_->setObjectName(QStringLiteral("eqSpectrum"));
    spectrum_->setToolTip(tr("Show the spectrum of what plays behind the curve"));
    spectrum_->setChecked(settings.value(QStringLiteral("eq/spectrum"), true).toBool());
    connect(spectrum_, &QCheckBox::toggled, this, [this](bool on) {
        display_->setShowSpectrum(on);
        appSettings().setValue(QStringLiteral("eq/spectrum"), on);
    });
    bar->addWidget(spectrum_);
    bar->addStretch(1);
    if (!large_) {
        openLarge_ = new QPushButton(tr("Large Window"), this);
        openLarge_->setObjectName(QStringLiteral("eqOpenLarge"));
        openLarge_->setToolTip(tr("Open this EQ in a window of its own, as large as you like"));
        connect(openLarge_, &QPushButton::clicked, this,
                [this] { openWindow(state_, owner_, effect_, time_, QWidget::window()); });
        bar->addWidget(openLarge_);
    }
    help_ = new QToolButton(this);
    help_->setObjectName(QStringLiteral("eqShowTips"));
    help_->setText(QStringLiteral("?"));
    help_->setToolTip(tr("Show the tips"));
    help_->setAccessibleName(tr("Show tips"));
    connect(help_, &QToolButton::clicked, this, [this] {
        appSettings().setValue(tipsHiddenKey(), false);
        for (EqPanel* p : panels()) p->showTips(true);
    });
    bar->addWidget(help_);
    v->addLayout(bar);

    display_ = new EqDisplay(this);
    display_->setMinimumHeight(large_ ? 320 : 170);
    display_->setRange(range_->currentData().toDouble());
    display_->setShowSpectrum(spectrum_->isChecked());
    v->addWidget(display_, 1);
    connect(display_, &EqDisplay::selectionChanged, this, [this](const std::string& band) { pick(band); });
    connect(display_, &EqDisplay::dragStarted, this, [this](const std::string& band) {
        state_->beginGesture(tr("Move EQ Band"));
        dragging_ = state_->inGesture();
        const int i = display_->bandIndex(band);
        pressHz_ = i < 0 ? 0 : display_->bands()[size_t(i)].hz;
        pressDb_ = i < 0 ? 0 : display_->bands()[size_t(i)].db;
    });
    connect(display_, &EqDisplay::bandMoved, this, [this](const std::string& band, double hz, double db, bool final) {
        if (!dragging_) return;
        const FrameTime t = time_();
        const Id owner = owner_, effect = effect_;
        // Only what the dot moved is written (a dot taken and let go where it was writes nothing, and makes no undo
        // step): on a keyframed band, a key on the other would change its curve.
        const bool moveHz = hz != pressHz_, moveDb = db != pressDb_;
        state_->updateGesture([=](Project&, Sequence& s) {
            Effect* e = edit::ownedEffect(s, owner, effect);
            if (!e) return;
            if (moveHz) e->params[band + "_hz"].set(t, hz);
            if (moveDb && eqTypeHasGain(eqBandType(*e, band))) e->params[band + "_db"].set(t, db);
        });
        if (final) {
            dragging_ = false;
            state_->endGesture(true);
        }
    });
    connect(display_, &EqDisplay::bandStepped, this, [this](const std::string& band, const std::string& field, double value) {
        write(band, {{field, value}}, tr("Change EQ Band"), QStringLiteral("step"));
    });
    connect(display_, &EqDisplay::addRequested, this, [this](double hz, double db, int type) {
        reshape(tr("Add EQ Band"), [=](Effect& e) { return addEqBand(e, type, hz, db); });
    });
    connect(display_, &EqDisplay::removeRequested, this, [this](const std::string& band) {
        reshape(tr("Remove EQ Band"), [band](Effect& e) {
            removeEqBand(e, band);
            return std::string();
        });
    });
    connect(display_, &EqDisplay::typeRequested, this, [this](const std::string& band, int type) {
        reshape(tr("Change EQ Band Type"), [band, type](Effect& e) {
            setEqBandType(e, band, type);
            return band;
        });
    });
    connect(display_, &EqDisplay::slopeRequested, this, [this](const std::string& band, int slope) {
        write(band, {{"_slope", double(slope)}}, tr("Change EQ Band Slope"), {});
    });
    connect(display_, &EqDisplay::onRequested, this, [this](const std::string& band, bool on) {
        write(band, {{"_on", on ? 1.0 : 0.0}}, on ? tr("Switch EQ Band On") : tr("Switch EQ Band Off"), {});
    });
    connect(display_, &EqDisplay::soloHeld, this, [this](const std::string& band, bool held) { solo(band, held); });

    // The strip: the band picked, set exactly.
    strip_ = new QWidget(this);
    strip_->setObjectName(QStringLiteral("eqBandStrip"));
    strip_->setAutoFillBackground(true);
    strip_->setStyleSheet(QStringLiteral("#eqBandStrip { background: %1; border: 1px solid %2; }").arg(theme::kPanelAlt.name(), theme::kBorder.name()));
    auto* flow = new FlowLayout(strip_, 6);
    flow->setContentsMargins(6, 4, 6, 4);
    bandName_ = new QLabel(strip_);
    bandName_->setObjectName(QStringLiteral("eqBandName"));
    flow->addWidget(bandName_);
    type_ = new QComboBox(strip_);
    type_->setObjectName(QStringLiteral("eqBandType"));
    for (int t = 0; t < int(eqBandTypes().size()); ++t) {
        static const char* const names[] = {QT_TR_NOOP("Bell"),    QT_TR_NOOP("Low Shelf"), QT_TR_NOOP("High Shelf"), QT_TR_NOOP("Low Cut"),
                                            QT_TR_NOOP("High Cut"), QT_TR_NOOP("Notch"),     QT_TR_NOOP("Band-Pass"),  QT_TR_NOOP("Tilt")};
        type_->addItem(tr(names[t]));
    }
    flow->addWidget(labelled(strip_, tr("Type"), type_));
    hz_ = spin(strip_, "eqBandHz", 20, 20000, 1, 1, tr(" Hz"));
    flow->addWidget(labelled(strip_, tr("Freq"), hz_));
    db_ = spin(strip_, "eqBandDb", -30, 30, 1, 0.1, tr(" dB"));
    flow->addWidget(labelled(strip_, tr("Gain"), db_));
    q_ = spin(strip_, "eqBandQ", 0.1, 30, 2, 0.05, QString());
    flow->addWidget(labelled(strip_, tr("Q"), q_));
    slope_ = new QComboBox(strip_);
    slope_->setObjectName(QStringLiteral("eqBandSlope"));
    for (int s = 0; s < int(eqSlopes().size()); ++s) slope_->addItem(tr("%1 dB/oct").arg(12 * (s + 1)));
    flow->addWidget(labelled(strip_, tr("Slope"), slope_));
    on_ = new QCheckBox(tr("On"), strip_);
    on_->setObjectName(QStringLiteral("eqBandOn"));
    flow->addWidget(on_);
    solo_ = new QPushButton(tr("Solo"), strip_);
    solo_->setObjectName(QStringLiteral("eqBandSolo"));
    solo_->setToolTip(tr("Hold to hear only this band's part of the sound as it plays"));
    solo_->setAccessibleName(tr("Solo band (hold)"));
    flow->addWidget(solo_);
    remove_ = new QToolButton(strip_);
    remove_->setObjectName(QStringLiteral("eqBandDelete"));
    remove_->setText(tr("Delete"));
    remove_->setToolTip(tr("Delete this band"));
    flow->addWidget(remove_);
    v->addWidget(strip_);
    auto bandNow = [this] { return display_->selected(); };
    connect(type_, &QComboBox::activated, this, [this, bandNow](int t) {
        const std::string band = bandNow();
        if (!band.empty())
            reshape(tr("Change EQ Band Type"), [band, t](Effect& e) {
                setEqBandType(e, band, t);
                return band;
            });
    });
    connect(slope_, &QComboBox::activated, this, [this, bandNow](int s) { write(bandNow(), {{"_slope", double(s)}}, tr("Change EQ Band Slope"), {}); });
    connect(hz_, &QDoubleSpinBox::valueChanged, this, [this, bandNow](double v) { write(bandNow(), {{"_hz", v}}, tr("Change EQ Band Frequency"), QStringLiteral("hz")); });
    connect(db_, &QDoubleSpinBox::valueChanged, this, [this, bandNow](double v) { write(bandNow(), {{"_db", v}}, tr("Change EQ Band Gain"), QStringLiteral("db")); });
    connect(q_, &QDoubleSpinBox::valueChanged, this, [this, bandNow](double v) { write(bandNow(), {{"_q", v}}, tr("Change EQ Band Q"), QStringLiteral("q")); });
    connect(on_, &QCheckBox::toggled, this, [this, bandNow](bool on) {
        write(bandNow(), {{"_on", on ? 1.0 : 0.0}}, on ? tr("Switch EQ Band On") : tr("Switch EQ Band Off"), {});
    });
    connect(solo_, &QPushButton::pressed, this, [this, bandNow] { solo(bandNow(), true); });
    connect(solo_, &QPushButton::released, this, [this, bandNow] { solo(bandNow(), false); });
    connect(remove_, &QToolButton::clicked, this, [this, bandNow] {
        const std::string band = bandNow();
        if (!band.empty())
            reshape(tr("Remove EQ Band"), [band](Effect& e) {
                removeEqBand(e, band);
                return std::string();
            });
    });

    // The tips: open until hidden ("?" brings them back).
    tips_ = new QWidget(this);
    tips_->setObjectName(QStringLiteral("eqTips"));
    auto* tv = new QVBoxLayout(tips_);
    tv->setContentsMargins(0, 0, 0, 0);
    tv->setSpacing(2);
    auto* head = new QHBoxLayout;
    tipsToggle_ = new QToolButton(tips_);
    tipsToggle_->setObjectName(QStringLiteral("eqTipsToggle"));
    tipsToggle_->setArrowType(Qt::DownArrow);
    tipsToggle_->setAutoRaise(true);
    tipsToggle_->setAccessibleName(tr("Fold the tips"));
    auto* title = new QLabel(tr("Tips"), tips_);
    QFont bold = title->font();
    bold.setBold(true);
    title->setFont(bold);
    auto* hide = new QToolButton(tips_);
    hide->setObjectName(QStringLiteral("eqHideTips"));
    hide->setText(tr("Hide tips"));
    hide->setAutoRaise(true);
    head->addWidget(tipsToggle_);
    head->addWidget(title, 1);
    head->addWidget(hide);
    tv->addLayout(head);
    tipsBody_ = new QLabel(QStringList{
        tr("• Double-click an empty part of the display to add a band there (up to 16). Add Band puts one at 1 kHz."),
        tr("• Drag a band's dot sideways to change its frequency, up or down to change its gain. Hold Shift for finer "
           "steps; hold Ctrl (Cmd on a Mac) to move in one direction only."),
        tr("• Scroll over a dot to make its band narrower or wider (its Q)."),
        tr("• Right-click a dot to change its type (bell, shelf, cut, notch, band-pass or tilt), a cut's slope, or to "
           "switch it off or delete it. Double-click a dot to switch its band off or back on."),
        tr("• Pick a band and press Delete to remove it."),
        tr("• Hold Solo, or hold Alt (Option on a Mac) while you drag a dot, to hear only that band's part of the sound "
           "as it plays. Let go and everything comes back."),
        tr("• The strip under the display sets the picked band exactly. With the display focused, Tab moves between "
           "bands, the arrow keys move one and Page Up or Page Down change its Q."),
        tr("• Large Window opens the EQ in a window you can make as big as you like; it stays in step with this one."),
    }.join(QLatin1Char('\n')), tips_);
    static_cast<QLabel*>(tipsBody_)->setWordWrap(true);
    tipsBody_->setObjectName(QStringLiteral("eqTipsText"));
    tipsBody_->setStyleSheet(QStringLiteral("color: %1;").arg(theme::kTextDim.name()));
    tv->addWidget(tipsBody_);
    v->addWidget(tips_);
    connect(tipsToggle_, &QToolButton::clicked, this, [this] {
        tipsBody_->setVisible(!tipsBody_->isVisible());
        tipsToggle_->setArrowType(tipsBody_->isVisible() ? Qt::DownArrow : Qt::RightArrow);
    });
    connect(hide, &QToolButton::clicked, this, [] {
        appSettings().setValue(tipsHiddenKey(), true);
        for (EqPanel* p : panels()) p->showTips(false);
    });
    showTips(!settings.value(tipsHiddenKey(), false).toBool());
}

bool EqPanel::tipsShown() const { return !tips_->isHidden() && !tipsBody_->isHidden(); }

void EqPanel::showTips(bool show) {
    tips_->setVisible(show);
    tipsBody_->setVisible(true);
    tipsToggle_->setArrowType(Qt::DownArrow);
    help_->setVisible(!show);
}

void EqPanel::refresh() {
    const Sequence* s = state_->sequence();
    const Effect* e = s ? edit::ownedEffect(const_cast<Sequence&>(*s), owner_, effect_) : nullptr;
    if (!e) {
        setEnabled(false);
        // (the large window of an EQ that is gone closes)
        if (large_)
            if (QWidget* w = window(); w && w != this) QTimer::singleShot(0, w, &QWidget::close);
        return;
    }
    setEnabled(true);
    display_->setSampleRate(s->sampleRate);  // (the rate the EQ runs at)
    const FrameTime t = time_();
    display_->setBands(bandsOf(*e, t), e->p("output_db", t, 0));
    std::string band = picked()[effect_];
    if (display_->bandIndex(band) < 0) band.clear();
    display_->setSelected(band);
    addBand_->setEnabled(int(display_->bands().size()) < kEqMaxBands);
    const int i = display_->bandIndex(band);
    for (QWidget* w : strip_->findChildren<QWidget*>())
        if (w != bandName_) w->setEnabled(i >= 0);
    if (i < 0) {
        bandName_->setText(display_->bands().empty() ? tr("No bands yet: double-click the display to add one") : tr("No band picked"));
        return;
    }
    const EqDisplay::Band& b = display_->bands()[size_t(i)];
    bandName_->setText(b.name == "low" ? tr("Low band") : b.name == "high" ? tr("High band") : tr("Band %1").arg(QString::fromStdString(b.name.substr(1))));
    QSignalBlocker b1(type_), b2(slope_), b3(hz_), b4(db_), b5(q_), b6(on_);
    type_->setCurrentIndex(b.type);
    slope_->setCurrentIndex(b.slope);
    slope_->setEnabled(eqTypeHasSlope(b.type));
    hz_->setValue(b.hz);
    db_->setValue(b.db);
    db_->setEnabled(eqTypeHasGain(b.type));
    q_->setMaximum(eqMaxQ(b.type));
    q_->setValue(b.q);
    on_->setChecked(b.on);
}

void EqPanel::pick(const std::string& band) {
    picked()[effect_] = band;
    for (EqPanel* p : panels())
        if (p->effect_ == effect_) p->refresh();
}

void EqPanel::write(const std::string& band, const std::vector<std::pair<std::string, double>>& values, const QString& label,
                    const QString& mergeKey) {
    if (band.empty()) return;
    const FrameTime t = time_();
    const Id owner = owner_, effect = effect_;
    state_->edit(label, [=](Project&, Sequence& s) {
        Effect* e = edit::ownedEffect(s, owner, effect);
        if (!e) return false;
        const bool gain = eqTypeHasGain(eqBandType(*e, band));
        for (const auto& [field, v] : values) {
            if (field == "_db" && !gain) continue;  // (a cut, notch or band-pass has no gain to keep)
            const std::string name = band + field;
            const bool whole = field == "_on" || field == "_slope";
            if (whole) e->params[name] = Param(v);  // (switches are not keyframed)
            else e->params[name].set(t, v);
        }
        return true;
    }, mergeKey.isEmpty() ? QString() : QStringLiteral("eq:%1:%2:%3").arg(effect).arg(QString::fromStdString(band), mergeKey));
}

void EqPanel::reshape(const QString& label, std::function<std::string(Effect&)> fn) {
    // (after this event: the Inspector replaces this panel as it rebuilds for the new bands)
    EditorState* state = state_;
    const Id owner = owner_, effect = effect_;
    QTimer::singleShot(0, state, [state, label, fn = std::move(fn), owner, effect] {
        state->edit(label, [&](Project&, Sequence& s) {
            Effect* e = edit::ownedEffect(s, owner, effect);
            if (!e) return false;
            picked()[effect] = fn(*e);  // (a band added is picked; one removed leaves none)
            return true;
        });
        for (EqPanel* p : panels())
            if (p->effect_ == effect) p->refresh();
    });
}

void EqPanel::solo(const std::string& band, bool held) {
    if (!held) {
        if (soloing_.empty()) return;
        soloing_.clear();
        state_->setAudition({});
        return;
    }
    if (band.empty()) return;
    soloing_ = band;
    const Id owner = owner_, effect = effect_;
    state_->setAudition([owner, effect, band](Sequence& s) {
        if (Effect* e = edit::ownedEffect(s, owner, effect)) e->strings["audition"] = band;
    });
}

QWidget* EqPanel::openWindowFor(Id effect) {
    const auto it = windows().find(effect);
    return it == windows().end() ? nullptr : it->second.data();
}

QWidget* EqPanel::openWindow(EditorState* state, Id owner, Id effect, std::function<FrameTime()> time, QWidget* parent) {
    if (QWidget* w = openWindowFor(effect)) {
        w->show();
        w->raise();
        w->activateWindow();
        return w;
    }
    auto* w = new QWidget(parent, Qt::Window);
    w->setObjectName(QStringLiteral("eqWindow"));
    w->setAttribute(Qt::WA_DeleteOnClose);
    w->setWindowTitle(tr("Parametric EQ"));
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(8, 8, 8, 8);
    v->addWidget(new EqPanel(state, owner, effect, std::move(time), true, w));
    w->resize(960, 600);
    windows()[effect] = w;
    w->show();
    return w;
}

}  // namespace montage
