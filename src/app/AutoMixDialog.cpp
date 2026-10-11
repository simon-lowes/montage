#include "AutoMixDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QProgressDialog>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <memory>

#include "EditorState.h"
#include "core/EditOps.h"

namespace montage {

namespace {

QDoubleSpinBox* level(QWidget* parent, const char* name, double value, double lo = -40, double hi = -6) {
    auto* s = new QDoubleSpinBox(parent);
    s->setObjectName(QString::fromLatin1(name));
    s->setRange(lo, hi);
    s->setDecimals(1);
    s->setSingleStep(0.5);
    s->setSuffix(QObject::tr(" LUFS"));
    s->setValue(value);
    return s;
}

// Presets: (name, dialogue, music, effects). Dialogue anchors the mix; the whole then lands about 2 LU above it.
struct Preset {
    const char* name;
    double dialogue, music, effects;
};
const Preset kPresets[] = {{QT_TRANSLATE_NOOP("AutoMix", "Web and streaming (about -16 LUFS)"), -18, -22, -24},
                           {QT_TRANSLATE_NOOP("AutoMix", "Podcast (about -16 LUFS, music lower)"), -18, -26, -26},
                           {QT_TRANSLATE_NOOP("AutoMix", "Broadcast (about -23 LUFS)"), -25, -29, -31}};

}  // namespace

AutoMixDialog::AutoMixDialog(EditorState* state, std::vector<ClipMix> plan, QWidget* parent)
    : QDialog(parent), state_(state), plan_(std::move(plan)) {
    setWindowTitle(tr("Auto Mix"));
    resize(640, 480);
    auto* lay = new QVBoxLayout(this);
    auto* intro = new QLabel(tr("Each audio clip was listened to and recognised as below; change any that are wrong. "
                                "Applying sets every clip's volume (and evens out dialogue with keyframes), then dips "
                                "the music under speech. It is one undo step, and the result is ordinary clip volume."),
                             this);
    intro->setWordWrap(true);
    lay->addWidget(intro);
    table_ = new QTableWidget(this);
    table_->setObjectName(QStringLiteral("autoMixClips"));
    table_->setColumnCount(4);
    table_->setHorizontalHeaderLabels({tr("Clip"), tr("Is"), tr("Measured"), tr("Volume")});
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table_->verticalHeader()->hide();
    table_->setSelectionMode(QAbstractItemView::NoSelection);
    lay->addWidget(table_, 1);

    auto* form = new QFormLayout;
    preset_ = new QComboBox(this);
    preset_->setObjectName(QStringLiteral("autoMixPreset"));
    for (const Preset& p : kPresets) preset_->addItem(QCoreApplication::translate("AutoMix", p.name));
    dialogue_ = level(this, "autoMixDialogue", kPresets[0].dialogue);
    music_ = level(this, "autoMixMusic", kPresets[0].music);
    effects_ = level(this, "autoMixEffects", kPresets[0].effects);
    ride_ = new QCheckBox(tr("Even out dialogue within each clip (up to 6 dB either way)"), this);
    ride_->setObjectName(QStringLiteral("autoMixRide"));
    ride_->setChecked(true);
    auto* duckRow = new QWidget(this);
    auto* dh = new QHBoxLayout(duckRow);
    dh->setContentsMargins(0, 0, 0, 0);
    duck_ = new QCheckBox(tr("Dip music under dialogue by"), duckRow);
    duck_->setObjectName(QStringLiteral("autoMixDuck"));
    duck_->setChecked(true);
    duckDb_ = new QDoubleSpinBox(duckRow);
    duckDb_->setObjectName(QStringLiteral("autoMixDuckDb"));
    duckDb_->setRange(-30, -3);
    duckDb_->setValue(-12);
    duckDb_->setSuffix(tr(" dB"));
    dh->addWidget(duck_);
    dh->addWidget(duckDb_);
    dh->addStretch(1);
    form->addRow(tr("Mix for:"), preset_);
    form->addRow(tr("Dialogue at:"), dialogue_);
    form->addRow(tr("Music on its own at:"), music_);
    form->addRow(tr("Effects at:"), effects_);
    form->addRow(QString(), ride_);
    form->addRow(QString(), duckRow);
    lay->addLayout(form);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Apply Mix"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(buttons);

    connect(preset_, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (i < 0 || i >= int(std::size(kPresets))) return;
        for (auto [box, v] : {std::pair{dialogue_, kPresets[i].dialogue}, std::pair{music_, kPresets[i].music},
                              std::pair{effects_, kPresets[i].effects}}) {
            QSignalBlocker b(box);
            box->setValue(v);
        }
        replan();
    });
    for (QDoubleSpinBox* box : {dialogue_, music_, effects_}) connect(box, &QDoubleSpinBox::valueChanged, this, [this] { replan(); });
    connect(ride_, &QCheckBox::toggled, this, [this] { replan(); });
    connect(duck_, &QCheckBox::toggled, duckDb_, &QWidget::setEnabled);
    fillTable();
}

MixOptions AutoMixDialog::options() const {
    MixOptions o;
    o.dialogueLufs = dialogue_->value();
    o.musicLufs = music_->value();
    o.effectsLufs = effects_->value();
    o.ride = ride_->isChecked();
    o.duck = duck_->isChecked();
    o.duckDb = duckDb_->value();
    return o;
}

void AutoMixDialog::setRole(int row, AudioRole role) {
    if (row < 0 || row >= int(plan_.size())) return;
    plan_[size_t(row)].role = role;
    replanClip(plan_[size_t(row)], options());
    fillTable();
}

void AutoMixDialog::replan() {
    const MixOptions o = options();
    for (ClipMix& m : plan_) replanClip(m, o);
    fillTable();
}

void AutoMixDialog::fillTable() {
    const Sequence* s = state_->sequence();
    table_->setRowCount(int(plan_.size()));
    for (int r = 0; r < int(plan_.size()); ++r) {
        const ClipMix& m = plan_[size_t(r)];
        const Clip* c = s ? edit::clipById(*s, m.clip) : nullptr;
        auto* name = new QTableWidgetItem(c ? QString::fromStdString(c->name) : QString());
        name->setFlags(Qt::ItemIsEnabled);
        table_->setItem(r, 0, name);
        auto* role = qobject_cast<QComboBox*>(table_->cellWidget(r, 1));
        if (!role) {
            role = new QComboBox(table_);
            role->setObjectName(QStringLiteral("autoMixRole%1").arg(r));
            for (AudioRole a : {AudioRole::Dialogue, AudioRole::Music, AudioRole::Effects, AudioRole::Silence})
                role->addItem(tr(audioRoleName(a)), int(a));
            connect(role, &QComboBox::activated, this, [this, r, role](int i) { setRole(r, AudioRole(role->itemData(i).toInt())); });
            table_->setCellWidget(r, 1, role);
        }
        {
            QSignalBlocker b(role);
            role->setCurrentIndex(role->findData(int(m.role)));
        }
        role->setToolTip(tr("Heard as %1 (speech %2, music %3)")
                             .arg(tr(audioRoleName(m.guess.role)))
                             .arg(m.guess.speech, 0, 'f', 2)
                             .arg(m.guess.music, 0, 'f', 2));
        auto* measured = new QTableWidgetItem(m.guess.loudness > -69 ? tr("%1 LUFS").arg(m.guess.loudness, 0, 'f', 1) : tr("silent"));
        measured->setFlags(Qt::ItemIsEnabled);
        table_->setItem(r, 2, measured);
        QString gain = m.role == AudioRole::Silence ? tr("unchanged") : QStringLiteral("%1%2 dB").arg(m.gainDb >= 0 ? "+" : "").arg(m.gainDb, 0, 'f', 1);
        if (!m.ride.empty()) gain += tr(", ridden");
        auto* g = new QTableWidgetItem(gain);
        g->setFlags(Qt::ItemIsEnabled);
        table_->setItem(r, 3, g);
    }
}

int AutoMixDialog::apply(EditorState* state, const std::vector<ClipMix>& plan, const MixOptions& o) {
    int changed = 0;
    state->edit(tr("Auto Mix"), [&](Project& p, Sequence& s) {
        changed = applyMix(p, s, plan, o);
        return changed > 0;
    });
    return changed;
}

int AutoMixDialog::run(EditorState* state, QWidget* parent) {
    const Sequence* seq = state->sequence();
    if (!seq) return -1;
    const Project project = state->project();
    const Sequence sequence = *seq;
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    auto done = std::make_shared<std::atomic<double>>(0.0);
    QProgressDialog progress(tr("Listening to the audio…"), tr("Cancel"), 0, 1000, parent);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(400);
    QObject::connect(&progress, &QProgressDialog::canceled, &progress, [cancel] { *cancel = true; });
    QTimer tick;
    QObject::connect(&tick, &QTimer::timeout, &progress, [&progress, done] { progress.setValue(int(*done * 1000)); });
    tick.start(100);
    using Out = std::pair<std::vector<ClipMix>, std::string>;
    QFutureWatcher<Out> watcher;
    QEventLoop wait;
    QObject::connect(&watcher, &QFutureWatcher<Out>::finished, &wait, &QEventLoop::quit);
    const MixOptions defaults;
    watcher.setFuture(QtConcurrent::run([project, sequence, defaults, cancel, done] {
        Out out;
        out.first = planMix(project, sequence, defaults, [done](double f) { *done = f; }, cancel.get(), &out.second);
        return out;
    }));
    if (!watcher.isFinished()) wait.exec();
    tick.stop();
    QObject::disconnect(&progress, &QProgressDialog::canceled, nullptr, nullptr);
    progress.close();
    if (*cancel) return -1;
    Out r = watcher.result();
    if (r.first.empty()) {
        state->message(r.second.empty() ? tr("There is no audio to mix") : QString::fromStdString(r.second), 5000);
        return -1;
    }
    AutoMixDialog dlg(state, std::move(r.first), parent);
    if (dlg.exec() != QDialog::Accepted) return -1;
    const int n = apply(state, dlg.plan(), dlg.options());
    state->message(tr("Mixed %n clip(s)", "", n), 5000);
    return n;
}

}  // namespace montage
