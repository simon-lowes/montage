#include "MonitoringPanel.h"

#include <QAudioDevice>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QMediaDevices>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include "EditorState.h"
#include "PlaybackController.h"
#include "Settings.h"
#include "core/Surround.h"

namespace montage {

namespace {

const char* kSettingsKey = "monitoring/settings";

QString setName(const QString& set) { return set == "headphones" ? MonitoringPanel::tr("Headphones") : set == "stereo" ? MonitoringPanel::tr("Stereo") : set; }

void keep(const PlaybackController* program) {
    MonitorSettings s = program->monitoring();
    s.soloTracks.clear();  // (monitor solos last only while Montage runs)
    QSettings st = appSettings();
    st.setValue(kSettingsKey, QString::fromUtf8(QJsonDocument(s.toJson()).toJson(QJsonDocument::Compact)));
    st.setValue("monitoring/output", program->audioOutput());
    st.setValue("monitoring/talkbackInput", program->talkbackInput());
}

QToolButton* toggle(const QString& text, const char* name, QWidget* parent) {
    auto* b = new QToolButton(parent);
    b->setText(text);
    b->setObjectName(QString::fromLatin1(name));
    b->setCheckable(true);
    b->setMinimumWidth(56);
    return b;
}

}  // namespace

void MonitoringPanel::restore(PlaybackController* program) {
    const QSettings st = appSettings();
    const QJsonObject o = QJsonDocument::fromJson(st.value(kSettingsKey).toString().toUtf8()).object();
    program->setAudioOutput(st.value("monitoring/output").toString());
    program->setTalkbackInput(st.value("monitoring/talkbackInput").toString());
    if (!o.isEmpty()) program->setMonitoring(MonitorSettings::fromJson(o));
}

MonitoringPanel::MonitoringPanel(EditorState* state, PlaybackController* program, QWidget* parent)
    : QWidget(parent), state_(state), program_(program) {
    setObjectName(QStringLiteral("monitoringPanel"));
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);
    auto* body = new QWidget(scroll);
    scroll->setWidget(body);
    auto* v = new QVBoxLayout(body);

    // The room: the device, the speaker set and each speaker.
    on_ = new QCheckBox(tr("Monitoring section"), body);
    on_->setObjectName(QStringLiteral("monitorOn"));
    on_->setToolTip(tr("Speakers, Listen As, Dim, Mono, Mute, monitor solos, calibration and the cue mix, after the mix in "
                       "playback alone (exports never hear them)"));
    v->addWidget(on_);
    auto* room = new QFormLayout;
    output_ = new QComboBox(body);
    output_->setObjectName(QStringLiteral("monitorOutput"));
    output_->addItem(tr("System default"), QString());
    for (const QAudioDevice& d : QMediaDevices::audioOutputs()) output_->addItem(d.description(), d.description());
    room->addRow(tr("Output"), output_);
    speakers_ = new QComboBox(body);
    speakers_->setObjectName(QStringLiteral("monitorSpeakers"));
    speakers_->addItem(tr("Headphones"), QStringLiteral("headphones"));
    for (const std::string& l : audioLayouts())
        if (!ambisonicLayout(l)) speakers_->addItem(setName(QString::fromStdString(l)), QString::fromStdString(l));
    room->addRow(tr("Speakers"), speakers_);
    v->addLayout(room);
    table_ = new QTableWidget(0, 4, body);
    table_->setObjectName(QStringLiteral("monitorSpeakerTable"));
    table_->setHorizontalHeaderLabels({tr("Speaker"), tr("Channel"), tr("Trim (dB)"), tr("Delay (samples)")});
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table_->verticalHeader()->hide();
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setMinimumHeight(140);
    v->addWidget(table_);
    auto* bassRow = new QHBoxLayout;
    bass_ = new QCheckBox(tr("Bass management"), body);
    bass_->setObjectName(QStringLiteral("monitorBass"));
    bass_->setToolTip(tr("The mains' lows and the LFE (10 dB up) to the subwoofer, through fourth-order Linkwitz-Riley crossovers"));
    crossover_ = new QDoubleSpinBox(body);
    crossover_->setObjectName(QStringLiteral("monitorCrossover"));
    crossover_->setRange(40, 200);
    crossover_->setDecimals(0);
    crossover_->setSuffix(tr(" Hz"));
    bassRow->addWidget(bass_);
    bassRow->addWidget(crossover_);
    bassRow->addStretch();
    v->addLayout(bassRow);

    // Listening.
    auto* listenBox = new QGroupBox(tr("Listening"), body);
    auto* lf = new QFormLayout(listenBox);
    listen_ = new QComboBox(listenBox);
    listen_->setObjectName(QStringLiteral("monitorListenAs"));
    lf->addRow(tr("Listen As"), listen_);
    auto* keys = new QHBoxLayout;
    dim_ = toggle(tr("Dim"), "monitorDim", listenBox);
    dim_->setToolTip(tr("Down 20 dB (F1 on a control surface)"));
    mono_ = toggle(tr("Mono"), "monitorMono", listenBox);
    mono_->setToolTip(tr("Both front speakers play (L + R) / 2 (F2)"));
    mute_ = toggle(tr("Mute"), "monitorMute", listenBox);
    mute_->setToolTip(tr("The speakers silent; the cue mix goes on (F3)"));
    keys->addWidget(dim_);
    keys->addWidget(mono_);
    keys->addWidget(mute_);
    keys->addStretch();
    lf->addRow(keys);
    level_ = new QDoubleSpinBox(listenBox);
    level_->setObjectName(QStringLiteral("monitorLevel"));
    level_->setRange(-60, 20);
    level_->setDecimals(1);
    level_->setSingleStep(0.5);
    level_->setSuffix(tr(" dB"));
    lf->addRow(tr("Monitor level"), level_);
    soloMode_ = new QComboBox(listenBox);
    soloMode_->setObjectName(QStringLiteral("monitorSoloMode"));
    soloMode_->addItem(tr("Project solo (exported)"), int(MonitorSolo::Project));
    soloMode_->addItem(tr("Solo in place (monitor)"), int(MonitorSolo::InPlace));
    soloMode_->addItem(tr("After fader (AFL)"), int(MonitorSolo::AfterFader));
    soloMode_->addItem(tr("Before fader (PFL)"), int(MonitorSolo::PreFader));
    soloMode_->setToolTip(tr("What the mixer's solo buttons do: solo in the project, or only in what is heard"));
    clearSolos_ = new QPushButton(tr("Clear"), listenBox);
    clearSolos_->setObjectName(QStringLiteral("monitorClearSolos"));
    auto* soloRow = new QHBoxLayout;
    soloRow->addWidget(soloMode_, 1);
    soloRow->addWidget(clearSolos_);
    lf->addRow(tr("Mixer solos"), soloRow);
    v->addWidget(listenBox);

    // Calibration.
    auto* calBox = new QGroupBox(tr("Calibration"), body);
    auto* cf = new QFormLayout(calBox);
    reference_ = new QComboBox(calBox);
    reference_->setObjectName(QStringLiteral("monitorReference"));
    for (int spl : {79, 82, 85}) reference_->addItem(tr("%1 dB SPL").arg(spl), spl);
    cf->addRow(tr("Reference"), reference_);
    noise_ = new QPushButton(tr("Play Noise on Selected Speaker"), calBox);
    noise_->setObjectName(QStringLiteral("monitorNoise"));
    noise_->setCheckable(true);
    noise_->setToolTip(tr("Pink noise, 500 Hz to 2 kHz at -20 dBFS RMS (a subwoofer's from 20 Hz to the crossover)"));
    cf->addRow(noise_);
    reading_ = new QDoubleSpinBox(calBox);
    reading_->setObjectName(QStringLiteral("monitorReading"));
    reading_->setRange(30, 130);
    reading_->setDecimals(1);
    reading_->setValue(85);
    reading_->setSuffix(tr(" dB SPL"));
    applyReading_ = new QPushButton(tr("Set Level"), calBox);
    applyReading_->setObjectName(QStringLiteral("monitorApplyReading"));
    applyReading_->setToolTip(tr("The meter's reading of the noise sets the monitor level, so the noise plays at the reference"));
    auto* readRow = new QHBoxLayout;
    readRow->addWidget(reading_, 1);
    readRow->addWidget(applyReading_);
    cf->addRow(tr("Meter reads"), readRow);
    v->addWidget(calBox);

    // The cue mix.
    auto* cueBox = new QGroupBox(tr("Cue mix (talent's headphones)"), body);
    auto* qf = new QFormLayout(cueBox);
    cueOn_ = new QCheckBox(tr("On"), cueBox);
    cueOn_->setObjectName(QStringLiteral("monitorCueOn"));
    qf->addRow(cueOn_);
    cueOutput_ = new QSpinBox(cueBox);
    cueOutput_->setObjectName(QStringLiteral("monitorCueOutput"));
    cueOutput_->setRange(1, 63);
    cueOutput_->setPrefix(tr("Channels "));
    qf->addRow(tr("Output"), cueOutput_);
    cueLevel_ = new QDoubleSpinBox(cueBox);
    cueLevel_->setObjectName(QStringLiteral("monitorCueLevel"));
    cueLevel_->setRange(-60, 20);
    cueLevel_->setDecimals(1);
    cueLevel_->setSuffix(tr(" dB"));
    qf->addRow(tr("Level"), cueLevel_);
    cueTracks_ = new QListWidget(cueBox);
    cueTracks_->setObjectName(QStringLiteral("monitorCueTracks"));
    cueTracks_->setMaximumHeight(90);
    qf->addRow(tr("Tracks"), cueTracks_);
    cueBeeps_ = new QCheckBox(tr("ADR beeps"), cueBox);
    cueBeeps_->setObjectName(QStringLiteral("monitorCueBeeps"));
    qf->addRow(cueBeeps_);
    cueProgram_ = new QCheckBox(tr("Programme"), cueBox);
    cueProgram_->setObjectName(QStringLiteral("monitorCueProgram"));
    cueProgramLevel_ = new QDoubleSpinBox(cueBox);
    cueProgramLevel_->setObjectName(QStringLiteral("monitorCueProgramLevel"));
    cueProgramLevel_->setRange(-60, 20);
    cueProgramLevel_->setDecimals(1);
    cueProgramLevel_->setSuffix(tr(" dB"));
    auto* progRow = new QHBoxLayout;
    progRow->addWidget(cueProgram_);
    progRow->addWidget(cueProgramLevel_, 1);
    qf->addRow(progRow);
    talkInput_ = new QComboBox(cueBox);
    talkInput_->setObjectName(QStringLiteral("monitorTalkbackInput"));
    talkInput_->addItem(tr("System default"), QString());
    for (const QAudioDevice& d : QMediaDevices::audioInputs()) talkInput_->addItem(d.description(), d.description());
    qf->addRow(tr("Talkback mic"), talkInput_);
    talk_ = new QToolButton(cueBox);
    talk_->setObjectName(QStringLiteral("monitorTalkback"));
    talk_->setText(tr("Talk"));
    talk_->setToolTip(tr("Hold to talk to the talent (F4 on a control surface)"));
    qf->addRow(talk_);
    v->addWidget(cueBox);
    v->addStretch();

    for (QCheckBox* c : {on_, bass_, cueOn_, cueBeeps_, cueProgram_}) connect(c, &QCheckBox::toggled, this, &MonitoringPanel::edited);
    for (QComboBox* c : {speakers_, listen_, soloMode_, reference_})
        connect(c, qOverload<int>(&QComboBox::currentIndexChanged), this, &MonitoringPanel::edited);
    for (QDoubleSpinBox* d : {crossover_, level_, cueLevel_, cueProgramLevel_})
        connect(d, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &MonitoringPanel::edited);
    connect(cueOutput_, qOverload<int>(&QSpinBox::valueChanged), this, &MonitoringPanel::edited);
    for (QToolButton* b : {dim_, mono_, mute_}) connect(b, &QToolButton::toggled, this, &MonitoringPanel::edited);
    connect(table_, &QTableWidget::cellChanged, this, &MonitoringPanel::edited);
    connect(cueTracks_, &QListWidget::itemChanged, this, &MonitoringPanel::edited);
    connect(output_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        if (refreshing_) return;
        program_->setAudioOutput(output_->currentData().toString());
        keep(program_);
    });
    connect(talkInput_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        if (refreshing_) return;
        program_->setTalkbackInput(talkInput_->currentData().toString());
        keep(program_);
    });
    connect(clearSolos_, &QPushButton::clicked, this, [this] {
        MonitorSettings s = program_->monitoring();
        s.soloTracks.clear();
        program_->setMonitoring(s);
    });
    connect(noise_, &QPushButton::toggled, this, [this](bool on) {
        if (refreshing_) return;
        calibrate(on ? std::max(0, table_->currentRow()) : -1);
    });
    connect(applyReading_, &QPushButton::clicked, this, [this] { applyReading(reading_->value()); });
    connect(talk_, &QToolButton::pressed, this, [this] { program_->setTalkback(true); });
    connect(talk_, &QToolButton::released, this, [this] { program_->setTalkback(false); });
    connect(program_, &PlaybackController::monitoringChanged, this, &MonitoringPanel::refresh);
    connect(state_, &EditorState::projectChanged, this, &MonitoringPanel::refresh);
    refresh();
}

void MonitoringPanel::apply(const MonitorSettings& s) {
    program_->setMonitoring(s);
    keep(program_);
}

void MonitoringPanel::calibrate(int speaker) {
    program_->setMonitorCalibration(speaker);
}

void MonitoringPanel::applyReading(double measuredSpl) {
    MonitorSettings s = program_->monitoring();
    s.levelDb = std::clamp(calibratedLevel(s.levelDb, measuredSpl, s.referenceSpl), -60.0, 20.0);
    apply(s);
}

void MonitoringPanel::toggleDim() {
    MonitorSettings s = program_->monitoring();
    s.dim = !s.dim;
    apply(s);
}

void MonitoringPanel::toggleMono() {
    MonitorSettings s = program_->monitoring();
    s.mono = !s.mono;
    apply(s);
}

void MonitoringPanel::toggleMute() {
    MonitorSettings s = program_->monitoring();
    s.mute = !s.mute;
    apply(s);
}

void MonitoringPanel::edited() {
    if (refreshing_) return;
    MonitorSettings s = program_->monitoring();
    s.on = on_->isChecked();
    const std::string set = speakers_->currentData().toString().toStdString();
    const bool newSet = set != s.speakers;
    s.speakers = set;
    if (newSet) {
        s.map.clear();
        s.listenAs.clear();
        s.soloSpeaker = -1;
    } else {
        s.map.clear();
        for (int r = 0; r < table_->rowCount(); ++r) {
            MonitorSpeaker m;
            auto num = [&](int col) { return table_->item(r, col) ? table_->item(r, col)->text().toDouble() : 0.0; };
            m.output = int(num(1)) - 1;
            m.trimDb = std::clamp(std::round(num(2) * 10) / 10, -40.0, 20.0);
            m.delay = std::clamp(int(std::lround(num(3))), 0, kMaxMonitorDelay);
            s.map.push_back(m);
        }
        s.listenAs = listen_->currentData().toString().toStdString();
    }
    s.bassManagement = bass_->isChecked();
    s.crossoverHz = crossover_->value();
    s.dim = dim_->isChecked();
    s.mono = mono_->isChecked();
    s.mute = mute_->isChecked();
    s.levelDb = level_->value();
    s.referenceSpl = reference_->currentData().toDouble();
    s.soloMode = MonitorSolo(soloMode_->currentData().toInt());
    if (s.soloMode == MonitorSolo::Project) s.soloTracks.clear();
    s.cue.on = cueOn_->isChecked();
    s.cue.output = cueOutput_->value() - 1;
    s.cue.levelDb = cueLevel_->value();
    s.cue.beeps = cueBeeps_->isChecked();
    s.cue.program = cueProgram_->isChecked();
    s.cue.programDb = cueProgramLevel_->value();
    s.cue.tracks.clear();
    for (int i = 0; i < cueTracks_->count(); ++i)
        if (cueTracks_->item(i)->checkState() == Qt::Checked) s.cue.tracks.push_back(i);
    apply(s);
}

void MonitoringPanel::refresh() {
    if (refreshing_) return;
    refreshing_ = true;
    const MonitorSettings& s = program_->monitoring();
    on_->setChecked(s.on);
    output_->setCurrentIndex(std::max(0, output_->findData(program_->audioOutput())));
    talkInput_->setCurrentIndex(std::max(0, talkInput_->findData(program_->talkbackInput())));
    speakers_->setCurrentIndex(std::max(0, speakers_->findData(QString::fromStdString(s.speakers))));
    const auto& sp = monitorSpeakers(s.speakers);
    const int keepRow = table_->currentRow();
    table_->setRowCount(int(sp.size()));
    for (int k = 0; k < int(sp.size()); ++k) {
        const MonitorSpeaker m = monitorSpeaker(s, k);
        // (Items kept, their text set: this may run while one of them tells of its edit.)
        auto cell = [&](int col, const QString& text) {
            QTableWidgetItem* item = table_->item(k, col);
            if (!item) {
                item = new QTableWidgetItem;
                if (col == 0) item->setFlags(item->flags() & ~Qt::ItemIsEditable);
                table_->setItem(k, col, item);
            }
            if (item->text() != text) item->setText(text);
        };
        cell(0, QString::fromLatin1(sp[size_t(k)].name));
        cell(1, QString::number(m.output + 1));
        cell(2, QString::number(m.trimDb, 'f', 1));
        cell(3, QString::number(m.delay));
    }
    if (keepRow >= 0 && keepRow < table_->rowCount()) table_->setCurrentCell(keepRow, 0);
    bass_->setChecked(s.bassManagement);
    bass_->setEnabled(s.speakers != "headphones" && s.speakers != "stereo");
    crossover_->setValue(s.crossoverHz);
    listen_->clear();
    listen_->addItem(s.speakers == "headphones" ? tr("Stereo") : tr("The speakers' own"), QString());
    for (const std::string& l : listenChoices(s.speakers))
        if (!(s.speakers == "headphones" && l == "stereo"))
            listen_->addItem(l == "binaural" ? tr("Binaural") : setName(QString::fromStdString(l)), QString::fromStdString(l));
    listen_->setCurrentIndex(std::max(0, listen_->findData(QString::fromStdString(s.listenAs))));
    dim_->setChecked(s.dim);
    mono_->setChecked(s.mono);
    mute_->setChecked(s.mute);
    level_->setValue(s.levelDb);
    reference_->setCurrentIndex(std::max(0, reference_->findData(int(std::lround(s.referenceSpl)))));
    noise_->setChecked(program_->monitorCalibration() >= 0);
    soloMode_->setCurrentIndex(std::max(0, soloMode_->findData(int(s.soloMode))));
    clearSolos_->setEnabled(!s.soloTracks.empty());
    cueOn_->setChecked(s.cue.on);
    cueOutput_->setValue(s.cue.output + 1);
    cueLevel_->setValue(s.cue.levelDb);
    cueBeeps_->setChecked(s.cue.beeps);
    cueProgram_->setChecked(s.cue.program);
    cueProgramLevel_->setValue(s.cue.programDb);
    // (Items kept while the tracks are the same in number: this may run while one tells of its check.)
    const Sequence* seq = state_->sequence();
    const int tracks = seq ? int(seq->audioTracks.size()) : 0;
    while (cueTracks_->count() > tracks) delete cueTracks_->takeItem(cueTracks_->count() - 1);
    while (cueTracks_->count() < tracks) {
        auto* item = new QListWidgetItem(cueTracks_);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    }
    for (int i = 0; i < tracks; ++i) {
        QListWidgetItem* item = cueTracks_->item(i);
        const QString text = QStringLiteral("A%1  %2").arg(i + 1).arg(QString::fromStdString(seq->audioTracks[size_t(i)].name));
        if (item->text() != text) item->setText(text);
        const Qt::CheckState want = std::find(s.cue.tracks.begin(), s.cue.tracks.end(), i) != s.cue.tracks.end() ? Qt::Checked : Qt::Unchecked;
        if (item->checkState() != want) item->setCheckState(want);
    }
    for (QWidget* w : std::initializer_list<QWidget*>{output_, speakers_, table_, bass_, crossover_, listen_, dim_, mono_, mute_, level_,
                                                       soloMode_, reference_, noise_, reading_, applyReading_, cueOn_, cueOutput_,
                                                       cueLevel_, cueTracks_, cueBeeps_, cueProgram_, cueProgramLevel_, talkInput_, talk_})
        w->setEnabled(s.on && (w != bass_ || bass_->isEnabled()));
    talk_->setEnabled(s.on && s.cue.on);
    refreshing_ = false;
}

QByteArray MonitoringPanel::toolJson() {
    return QJsonDocument(QJsonObject{
        {"name", "montage_monitoring"},
        {"title", "Monitoring section"},
        {"description",
         "The monitoring section of the open Montage window (playback only; exports never hear it): get its settings, or set "
         "them (any of the fields `get` returns, merged: on, speakers \"headphones\"/\"stereo\"/\"5.1\"/..., map [{output, trim_db, "
         "delay}], bass_management, crossover_hz, listen_as, dim, dim_db, mono, mute, solo_speaker, level_db, reference_spl, "
         "solo_mode project/in_place/afl/pfl, solo_tracks, cue {on, output, level_db, tracks, beeps, program, program_db, "
         "talkback_db}); calibrate (speaker index, or -1 to stop) and reading (the sound level meter's dB SPL: sets level_db "
         "for the reference); talkback true/false. Channels and tracks count from 0."},
        {"inputSchema",
         QJsonObject{{"type", "object"},
                     {"properties", QJsonObject{{"action", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"get", "set", "calibrate", "reading", "talkback"}}}},
                                                {"settings", QJsonObject{{"type", "object"}}},
                                                {"speaker", QJsonObject{{"type", "integer"}}},
                                                {"spl", QJsonObject{{"type", "number"}}},
                                                {"on", QJsonObject{{"type", "boolean"}}}}},
                     {"required", QJsonArray{"action"}}}}})
        .toJson(QJsonDocument::Compact);
}

QString MonitoringPanel::runTool(const QJsonObject& args, QJsonObject& structured, bool& isError) {
    const QString action = args.value("action").toString();
    auto state = [&] {
        structured = program_->monitoring().toJson();
        structured["calibrating"] = program_->monitorCalibration();
        structured["talkback"] = program_->talkback();
        structured["outputs"] = monitorOutputs(program_->monitoring());
        structured["listening_as"] = QString::fromStdString(state_->sequence() ? listenLayout(program_->monitoring(), *state_->sequence())
                                                                                  : program_->monitoring().speakers);
    };
    if (action == "get") {
        state();
        return tr("Monitoring %1").arg(program_->monitoring().on ? tr("on") : tr("off"));
    }
    if (action == "set") {
        QJsonObject merged = program_->monitoring().toJson();
        const QJsonObject in = args.value("settings").toObject();
        for (auto it = in.begin(); it != in.end(); ++it) {
            if (it.key() == "cue" && it.value().isObject()) {
                QJsonObject cue = merged.value("cue").toObject();
                const QJsonObject c = it.value().toObject();
                for (auto ct = c.begin(); ct != c.end(); ++ct) cue[ct.key()] = ct.value();
                merged["cue"] = cue;
            } else {
                merged[it.key()] = it.value();
            }
        }
        apply(MonitorSettings::fromJson(merged));
        state();
        return tr("Monitoring set");
    }
    if (action == "calibrate") {
        if (!program_->monitoring().on) {
            isError = true;
            return tr("Turn the monitoring section on first");
        }
        calibrate(args.value("speaker").toInt(-1));
        state();
        return program_->monitorCalibration() >= 0 ? tr("Calibration noise on speaker %1").arg(program_->monitorCalibration() + 1)
                                                   : tr("Calibration noise off");
    }
    if (action == "reading") {
        if (!args.value("spl").isDouble()) {
            isError = true;
            return tr("reading needs spl (the meter's dB SPL)");
        }
        applyReading(args.value("spl").toDouble());
        state();
        return tr("Monitor level %1 dB").arg(program_->monitoring().levelDb, 0, 'f', 1);
    }
    if (action == "talkback") {
        program_->setTalkback(args.value("on").toBool());
        state();
        return program_->talkback() ? tr("Talking") : tr("Talkback off");
    }
    isError = true;
    return tr("Unknown action %1").arg(action);
}

}  // namespace montage
