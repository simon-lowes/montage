#include "MonitoringPanel.h"
#include "Language.h"

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
#include <QLineEdit>
#include <QListWidget>
#include <QMediaDevices>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "EditorState.h"
#include "PlaybackController.h"
#include "Settings.h"
#include "core/Surround.h"

namespace montage {

namespace {

const char* kSettingsKey = "monitoring/settings";
const char* kDeviceKey = "monitoring/device";

QString hostName(audiodev::Host h) {
    switch (h) {
        case audiodev::Host::Qt: return MonitoringPanel::tr("System (Qt Multimedia)");
        case audiodev::Host::Jack: return MonitoringPanel::tr("JACK");
        case audiodev::Host::Alsa: return MonitoringPanel::tr("ALSA");
        case audiodev::Host::CoreAudio: return MonitoringPanel::tr("Core Audio");
        case audiodev::Host::Wasapi: return MonitoringPanel::tr("WASAPI");
        case audiodev::Host::WasapiExclusive: return MonitoringPanel::tr("WASAPI (exclusive)");
        case audiodev::Host::Asio: return MonitoringPanel::tr("ASIO");
        case audiodev::Host::Loopback: return MonitoringPanel::tr("Loopback (a test device)");
    }
    return {};
}

QJsonObject statusJson(const audiodev::EngineStatus& st) {
    QJsonArray ports, clocks;
    for (const std::string& p : st.ports) ports.append(QString::fromStdString(p));
    for (const std::string& c : st.clocks) clocks.append(QString::fromStdString(c));
    QJsonObject o{{"open", st.open},
                  {"host", audiodev::hostId(st.host)},
                  {"device", QString::fromStdString(st.device)},
                  {"sample_rate", st.rate},
                  {"buffer", st.buffer},
                  {"inputs", st.inputs},
                  {"outputs", st.outputs},
                  {"input_latency", st.latency.input},
                  {"output_latency", st.latency.output},
                  {"round_trip", st.roundTrip},
                  {"round_trip_measured", st.measured},
                  {"xruns", double(st.xruns)},
                  {"rate_changes", st.rateChanges},
                  {"ports", ports},
                  {"clock", QString::fromStdString(st.clock)},
                  {"clocks", clocks}};
    if (!st.error.empty()) o["error"] = QString::fromStdString(st.error);
    if (st.load >= 0) o["load"] = st.load;
    return o;
}

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
    // (the device first: the section's outputs are then named on it)
    const QJsonObject device = QJsonDocument::fromJson(st.value(kDeviceKey).toString().toUtf8()).object();
    if (!device.isEmpty()) audiodev::Engine::shared().setSettings(audiodev::DeviceSettings::fromJson(device));
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

    // The audio device: its driver, devices, rate, buffer and clock, the round trip and xruns.
    auto* deviceBox = new QGroupBox(tr("Audio device"), body);
    deviceBox->setObjectName(QStringLiteral("monitorDeviceBox"));
    auto* df = new QFormLayout(deviceBox);
    driver_ = new QComboBox(deviceBox);
    driver_->setObjectName(QStringLiteral("monitorDriver"));
    driver_->setToolTip(tr("What playback and recording go through: the system's devices through Qt Multimedia, or a driver "
                           "for professional interfaces with its latency reported and taken off every recording"));
    for (audiodev::Host h : audiodev::availableHosts()) driver_->addItem(hostName(h), QString::fromLatin1(audiodev::hostId(h)));
    df->addRow(tr("Driver"), driver_);
    deviceOut_ = new QComboBox(deviceBox);
    deviceOut_->setObjectName(QStringLiteral("monitorDeviceOutput"));
    language::keepUntranslated(deviceOut_);
    df->addRow(tr("Output device"), deviceOut_);
    deviceIn_ = new QComboBox(deviceBox);
    deviceIn_->setObjectName(QStringLiteral("monitorDeviceInput"));
    language::keepUntranslated(deviceIn_);
    df->addRow(tr("Input device"), deviceIn_);
    deviceRate_ = new QComboBox(deviceBox);
    deviceRate_->setObjectName(QStringLiteral("monitorDeviceRate"));
    deviceRate_->addItem(tr("The sequence's"), 0);
    for (int r : {44100, 48000, 88200, 96000, 176400, 192000}) deviceRate_->addItem(tr("%1 Hz").arg(r), r);
    deviceBuffer_ = new QComboBox(deviceBox);
    deviceBuffer_->setObjectName(QStringLiteral("monitorDeviceBuffer"));
    for (int b = audiodev::kMinBuffer; b <= audiodev::kMaxBuffer; b *= 2) deviceBuffer_->addItem(tr("%1 samples").arg(b), b);
    auto* rateRow = new QHBoxLayout;
    rateRow->addWidget(deviceRate_, 1);
    rateRow->addWidget(deviceBuffer_, 1);
    df->addRow(tr("Rate and buffer"), rateRow);
    deviceClock_ = new QComboBox(deviceBox);
    deviceClock_->setObjectName(QStringLiteral("monitorDeviceClock"));
    df->addRow(tr("Clock"), deviceClock_);
    deviceInputs_ = new QSpinBox(deviceBox);
    deviceInputs_->setObjectName(QStringLiteral("monitorDeviceInputs"));
    deviceInputs_->setRange(0, 64);
    inputNames_ = new QLineEdit(deviceBox);
    inputNames_->setObjectName(QStringLiteral("monitorDeviceInputNames"));
    inputNames_->setPlaceholderText(tr("Their names, apart by commas (In 1, In 2...)"));
    auto* inRow = new QHBoxLayout;
    inRow->addWidget(deviceInputs_);
    inRow->addWidget(inputNames_, 1);
    df->addRow(tr("Inputs"), inRow);
    loopOut_ = new QSpinBox(deviceBox);
    loopOut_->setObjectName(QStringLiteral("monitorLoopOutput"));
    loopOut_->setRange(1, 64);
    loopOut_->setPrefix(tr("Out "));
    loopIn_ = new QSpinBox(deviceBox);
    loopIn_->setObjectName(QStringLiteral("monitorLoopInput"));
    loopIn_->setRange(1, 64);
    loopIn_->setPrefix(tr("In "));
    measure_ = new QPushButton(tr("Measure Loopback"), deviceBox);
    measure_->setObjectName(QStringLiteral("monitorMeasureLoopback"));
    measure_->setToolTip(tr("With a cable from the output to the input, a click is played and listened for: the round trip it "
                            "takes is then taken off recordings instead of the latencies the device reports"));
    forget_ = new QPushButton(tr("Forget"), deviceBox);
    forget_->setObjectName(QStringLiteral("monitorForgetLoopback"));
    auto* loopRow = new QHBoxLayout;
    loopRow->addWidget(loopOut_);
    loopRow->addWidget(loopIn_);
    loopRow->addWidget(measure_, 1);
    loopRow->addWidget(forget_);
    df->addRow(tr("Loopback"), loopRow);
    deviceStatus_ = new QLabel(deviceBox);
    deviceStatus_->setObjectName(QStringLiteral("monitorDeviceStatus"));
    deviceStatus_->setWordWrap(true);
    deviceStatus_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    resetXruns_ = new QPushButton(tr("Reset Xruns"), deviceBox);
    resetXruns_->setObjectName(QStringLiteral("monitorResetXruns"));
    auto* statusRow = new QHBoxLayout;
    statusRow->addWidget(deviceStatus_, 1);
    statusRow->addWidget(resetXruns_);
    df->addRow(statusRow);
    v->addWidget(deviceBox);
    statusTimer_ = new QTimer(this);
    statusTimer_->setInterval(500);
    connect(statusTimer_, &QTimer::timeout, this, &MonitoringPanel::showDeviceStatus);
    statusTimer_->start();

    // The room: the device, the speaker set and each speaker.
    on_ = new QCheckBox(tr("Monitoring section"), body);
    on_->setObjectName(QStringLiteral("monitorOn"));
    on_->setToolTip(tr("Speakers, Listen As, Dim, Mono, Mute, monitor solos, calibration and the cue mix, after the mix in "
                       "playback alone (exports never hear them)"));
    v->addWidget(on_);
    auto* room = new QFormLayout;
    output_ = new QComboBox(body);
    output_->setObjectName(QStringLiteral("monitorOutput"));
    language::keepUntranslated(output_);  // (the computer's own names for its devices)
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
    applyReading_->setToolTip(tr("The meter's reading of the noise: on the first speaker it sets the monitor level, on each "
                                 "other speaker its trim, so the noise plays at the reference on every one"));
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
    cueOutput_->setRange(1, kMaxMonitorChannels - 1);
    cueOutput_->setToolTip(tr("Its first channel (the second is the next); on channels a speaker uses, it is added to that speaker"));
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
    language::keepUntranslated(talkInput_);
    qf->addRow(tr("Talkback mic"), talkInput_);  // (its choices: refreshDevice)
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
    for (QComboBox* c : {driver_, deviceOut_, deviceIn_, deviceRate_, deviceBuffer_, deviceClock_})
        connect(c, qOverload<int>(&QComboBox::currentIndexChanged), this, &MonitoringPanel::deviceEdited);
    connect(deviceInputs_, qOverload<int>(&QSpinBox::valueChanged), this, &MonitoringPanel::deviceEdited);
    connect(inputNames_, &QLineEdit::editingFinished, this, &MonitoringPanel::deviceEdited);
    connect(measure_, &QPushButton::clicked, this, [this] {
        QString why;
        if (!measureLoopback(loopOut_->value() - 1, loopIn_->value() - 1, &why)) state_->message(why, 6000);
    });
    connect(forget_, &QPushButton::clicked, this, [this] {
        audiodev::DeviceSettings s = audiodev::Engine::shared().settings();
        s.loopbackFrames = s.loopbackRate = s.loopbackBuffer = 0;
        applyDevice(s);
    });
    connect(resetXruns_, &QPushButton::clicked, this, [this] {
        audiodev::Engine::shared().resetXruns();
        showDeviceStatus();
    });
    refreshDevice();
    refresh();
}

void MonitoringPanel::applyDevice(audiodev::DeviceSettings s) {
    audiodev::Engine& engine = audiodev::Engine::shared();
    const audiodev::DeviceSettings old = engine.settings();
    // (a loopback measured on one device says nothing of another's: forgotten with it, unless a new one is given)
    if ((s.host != old.host || s.output != old.output || s.input != old.input) && s.loopbackFrames == old.loopbackFrames &&
        s.loopbackRate == old.loopbackRate && s.loopbackBuffer == old.loopbackBuffer)
        s.loopbackFrames = s.loopbackRate = s.loopbackBuffer = 0;
    engine.setSettings(s);
    QSettings st = appSettings();
    st.setValue(kDeviceKey, QString::fromUtf8(QJsonDocument(engine.settings().toJson()).toJson(QJsonDocument::Compact)));
    PlaybackController::reopenAllAudio();
    refreshDevice();
}

bool MonitoringPanel::measureLoopback(int output, int input, QString* error) {
    if (program_->isPlaying()) program_->pause();  // (the programme would drown the click)
    std::string why;
    const int frames = audiodev::Engine::shared().measureLoopback(output, input, 1.0, &why);
    if (frames < 0) {
        if (error) *error = tr("Loopback not measured: %1").arg(QString::fromStdString(why));
        showDeviceStatus();
        return false;
    }
    applyDevice(audiodev::Engine::shared().settings());  // (kept)
    return true;
}

void MonitoringPanel::deviceEdited() {
    if (refreshing_) return;
    audiodev::DeviceSettings s = audiodev::Engine::shared().settings();
    const QString host = driver_->currentData().toString();
    s.host = audiodev::hostFromId(host.toStdString()).value_or(audiodev::Host::Qt);
    if (host != listedHost_) {
        // (another driver: its default devices, not the last one's names)
        s.output.clear();
        s.input.clear();
        s.clock.clear();
    } else {
        s.output = deviceOut_->currentData().toString().toStdString();
        s.input = deviceIn_->currentData().toString().toStdString();
        s.clock = deviceClock_->currentData().toString().toStdString();
    }
    s.sampleRate = deviceRate_->currentData().toInt();
    s.bufferFrames = deviceBuffer_->currentData().toInt();
    s.inputs = deviceInputs_->value();
    s.inputNames.clear();
    for (const QString& n : inputNames_->text().split(QLatin1Char(','), Qt::SkipEmptyParts)) s.inputNames.push_back(n.trimmed().toStdString());
    if (s == audiodev::Engine::shared().settings()) return;
    applyDevice(s);
}

void MonitoringPanel::refreshDevice() {
    const bool was = refreshing_;
    refreshing_ = true;
    audiodev::Engine& engine = audiodev::Engine::shared();
    const audiodev::DeviceSettings s = engine.settings();
    const QString host = QString::fromLatin1(audiodev::hostId(s.host));
    if (driver_->findData(host) < 0) driver_->addItem(hostName(s.host), host);  // (set over MCP: the loopback, say)
    driver_->setCurrentIndex(std::max(0, driver_->findData(host)));
    const bool device = s.host != audiodev::Host::Qt;
    if (host != listedHost_) {
        listedHost_ = host;
        deviceOut_->clear();
        deviceIn_->clear();
        deviceOut_->addItem(tr("Default"), QString());
        deviceIn_->addItem(tr("Default"), QString());
        for (const audiodev::DeviceInfo& d : audiodev::devices(s.host)) {
            if (d.outputs > 0) deviceOut_->addItem(QString::fromStdString(d.name), QString::fromStdString(d.id));
            if (d.inputs > 0) deviceIn_->addItem(QString::fromStdString(d.name), QString::fromStdString(d.id));
        }
    }
    for (auto [combo, id] : {std::pair{deviceOut_, s.output}, std::pair{deviceIn_, s.input}}) {
        const QString want = QString::fromStdString(id);
        if (combo->findData(want) < 0) combo->addItem(want, want);  // (a device not here now: kept for when it is)
        combo->setCurrentIndex(std::max(0, combo->findData(want)));
    }
    deviceRate_->setCurrentIndex(std::max(0, deviceRate_->findData(s.sampleRate)));
    deviceBuffer_->setCurrentIndex(std::max(0, deviceBuffer_->findData(s.bufferFrames)));
    deviceClock_->clear();
    deviceClock_->addItem(tr("The device's own"), QString());
    for (const std::string& c : engine.status().clocks) deviceClock_->addItem(QString::fromStdString(c), QString::fromStdString(c));
    if (!s.clock.empty() && deviceClock_->findData(QString::fromStdString(s.clock)) < 0)
        deviceClock_->addItem(QString::fromStdString(s.clock), QString::fromStdString(s.clock));
    deviceClock_->setCurrentIndex(std::max(0, deviceClock_->findData(QString::fromStdString(s.clock))));
    deviceInputs_->setValue(s.inputs);
    QStringList names;
    for (const std::string& n : s.inputNames) names << QString::fromStdString(n);
    inputNames_->setText(names.join(QStringLiteral(", ")));
    for (QWidget* w : std::initializer_list<QWidget*>{deviceOut_, deviceIn_, deviceRate_, deviceBuffer_, deviceClock_, deviceInputs_, inputNames_,
                                                       loopOut_, loopIn_, measure_, forget_, resetXruns_})
        w->setEnabled(device);
    forget_->setEnabled(device && s.loopbackFrames > 0);
    // The talkback microphone: one of the device's inputs, or of the system's.
    const QString talk = program_->talkbackInput();
    talkInput_->clear();
    talkInput_->addItem(device ? tr("First input") : tr("System default"), QString());
    if (device)
        for (int k = 0; k < s.inputs; ++k) talkInput_->addItem(QString::fromStdString(audiodev::inputName(s, k)), QString::fromStdString(audiodev::inputName(s, k)));
    else
        for (const QAudioDevice& d : QMediaDevices::audioInputs()) talkInput_->addItem(d.description(), d.description());
    talkInput_->setCurrentIndex(std::max(0, talkInput_->findData(talk)));
    refreshing_ = was;
    showDeviceStatus();
}

void MonitoringPanel::showDeviceStatus() {
    if (!isVisible() && deviceStatus_->text().size() > 0 && sender() == statusTimer_) return;
    const audiodev::EngineStatus st = audiodev::Engine::shared().status();
    QString text;
    if (st.host == audiodev::Host::Qt) text = tr("Through the system's devices (Qt Multimedia): their latency is not known");
    else if (!st.open) text = tr("Not open: %1").arg(QString::fromStdString(st.error));
    else {
        auto ms = [&](int frames) { return QString::number(1000.0 * frames / std::max(1, st.rate), 'f', 1); };
        text = tr("%1: %2 Hz, %3 samples; latency in %4 ms, out %5 ms; recordings moved %6 ms (%7); xruns %8")
                   .arg(QString::fromStdString(st.device))
                   .arg(st.rate)
                   .arg(st.buffer)
                   .arg(ms(st.latency.input), ms(st.latency.output), ms(st.roundTrip), st.measured ? tr("measured") : tr("as reported"))
                   .arg(st.xruns);
        if (st.rateChanges > 0) text += tr("; the rate changed %n time(s)", nullptr, st.rateChanges);
    }
    if (deviceStatus_->text() != text) deviceStatus_->setText(text);
}

QByteArray MonitoringPanel::deviceToolJson() {
    return QJsonDocument(QJsonObject{
        {"name", "montage_audio_device"},
        {"title", "Audio device"},
        {"description",
         "The audio device playback and recording go through (Window > Monitoring, Audio device): \"get\" its settings and "
         "state (sample_rate, buffer, input_latency and output_latency in frames as the device reports them, round_trip: what "
         "every recording is moved earlier by, from a measured loopback where one was run; xruns, rate_changes, ports: JACK's "
         "port names) and the hosts this build has; \"devices\" of a host; \"set\" (settings merged: host qt/jack/alsa/"
         "coreaudio/wasapi/wasapi_exclusive, output, input (device ids from \"devices\"; JACK: the client whose physical ports "
         "are joined), sample_rate (0 the sequence's), buffer (32 to 2048), clock, inputs, input_names, auto_connect); "
         "\"measure\" a loopback (a cable from output to input, channels from 0: a click played and listened for); "
         "\"reset_xruns\"."},
        {"inputSchema",
         QJsonObject{{"type", "object"},
                     {"properties", QJsonObject{{"action", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"get", "devices", "set", "measure", "reset_xruns"}}}},
                                                {"host", QJsonObject{{"type", "string"}}},
                                                {"settings", QJsonObject{{"type", "object"}}},
                                                {"output", QJsonObject{{"type", "integer"}, {"minimum", 0}}},
                                                {"input", QJsonObject{{"type", "integer"}, {"minimum", 0}}}}},
                     {"required", QJsonArray{"action"}}}}})
        .toJson(QJsonDocument::Compact);
}

QString MonitoringPanel::runDeviceTool(const QJsonObject& args, QJsonObject& structured, bool& isError) {
    const QString action = args.value("action").toString();
    audiodev::Engine& engine = audiodev::Engine::shared();
    auto state = [&] {
        structured = QJsonObject{{"settings", engine.settings().toJson()}, {"status", statusJson(engine.status())}};
        QJsonArray hosts;
        for (audiodev::Host h : audiodev::availableHosts()) hosts.append(QString::fromLatin1(audiodev::hostId(h)));
        structured["hosts"] = hosts;
        structured["jack_library"] = QString::fromStdString(audiodev::jackLibrary());
    };
    if (action == "get") {
        state();
        const audiodev::EngineStatus st = engine.status();
        return st.open ? tr("%1 at %2 Hz, %3 samples, xruns %4").arg(QString::fromStdString(st.device)).arg(st.rate).arg(st.buffer).arg(st.xruns)
                       : tr("Through the system's devices (Qt Multimedia)");
    }
    if (action == "devices") {
        const auto host = audiodev::hostFromId(args.value("host").toString().toStdString());
        if (!host) {
            isError = true;
            return tr("devices needs a host (see \"get\")");
        }
        std::string why;
        QJsonArray list;
        for (const audiodev::DeviceInfo& d : audiodev::devices(*host, &why))
            list.append(QJsonObject{{"id", QString::fromStdString(d.id)}, {"name", QString::fromStdString(d.name)}, {"inputs", d.inputs},
                                    {"outputs", d.outputs}, {"default_rate", d.defaultRate}});
        structured = QJsonObject{{"devices", list}};
        if (!why.empty()) structured["error"] = QString::fromStdString(why);
        return tr("%n device(s)", nullptr, int(list.size()));
    }
    if (action == "set") {
        QJsonObject merged = engine.settings().toJson();
        const QJsonObject in = args.value("settings").toObject();
        for (auto it = in.begin(); it != in.end(); ++it) merged[it.key()] = it.value();
        const audiodev::DeviceSettings s = audiodev::DeviceSettings::fromJson(merged);
        if (s.host != audiodev::Host::Loopback && !audiodev::hostBuilt(s.host)) {
            isError = true;
            return tr("This build or computer has no %1").arg(QString::fromLatin1(audiodev::hostId(s.host)));
        }
        applyDevice(s);
        state();
        const audiodev::EngineStatus st = engine.status();
        if (s.host != audiodev::Host::Qt && !st.open) {
            isError = true;
            return tr("The device could not be opened: %1").arg(QString::fromStdString(st.error));
        }
        return tr("Audio device set");
    }
    if (action == "measure") {
        QString why;
        if (!measureLoopback(args.value("output").toInt(0), args.value("input").toInt(0), &why)) {
            isError = true;
            state();
            return why;
        }
        state();
        return tr("Round trip %1 frames").arg(engine.settings().loopbackFrames);
    }
    if (action == "reset_xruns") {
        engine.resetXruns();
        state();
        return tr("Xruns reset");
    }
    isError = true;
    return tr("Unknown action %1").arg(action);
}

void MonitoringPanel::apply(const MonitorSettings& s) {
    program_->setMonitoring(s);
    keep(program_);
}

void MonitoringPanel::calibrate(int speaker) {
    program_->setMonitorCalibration(speaker);
}

void MonitoringPanel::applyReading(double measuredSpl) {
    // The first speaker's reading (or one taken without the noise) sets the monitor level; each other speaker's, its
    // own trim, so every speaker reads the reference in turn.
    MonitorSettings s = program_->monitoring();
    const int k = program_->monitorCalibration();
    if (k <= 0) {
        s.levelDb = std::clamp(calibratedLevel(s.levelDb, measuredSpl, s.referenceSpl), -60.0, 20.0);
    } else {
        const int n = int(monitorSpeakers(s.speakers).size());
        for (int i = int(s.map.size()); i < n; ++i) s.map.push_back(monitorSpeaker(s, i));
        MonitorSpeaker& m = s.map[size_t(k)];
        m.trimDb = std::clamp(std::round(calibratedLevel(m.trimDb, measuredSpl, s.referenceSpl) * 10) / 10, -40.0, 20.0);
    }
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
            m.output = std::clamp(int(num(1)) - 1, -1, kMaxMonitorChannels - 1);
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
    const bool cueWasOn = s.cue.on;
    s.cue.on = cueOn_->isChecked();
    s.cue.output = cueOutput_->value() - 1;
    // Turned on where speakers are, it moves to the first channels after them (it can still be put back by hand).
    if (s.cue.on && !cueWasOn && s.cue.output < monitorFreeOutput(s)) s.cue.output = monitorFreeOutput(s);
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
    if (listen_->findData(QString::fromStdString(s.listenAs)) < 0)  // (another layout, set over MCP)
        listen_->addItem(setName(QString::fromStdString(s.listenAs)), QString::fromStdString(s.listenAs));
    listen_->setCurrentIndex(std::max(0, listen_->findData(QString::fromStdString(s.listenAs))));
    dim_->setChecked(s.dim);
    mono_->setChecked(s.mono);
    mute_->setChecked(s.mute);
    level_->setValue(s.levelDb);
    if (reference_->findData(int(std::lround(s.referenceSpl))) < 0)  // (another reference, set over MCP)
        reference_->addItem(tr("%1 dB SPL").arg(int(std::lround(s.referenceSpl))), int(std::lround(s.referenceSpl)));
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
    output_->setEnabled(s.on && !audiodev::Engine::shared().active());  // (the system's device; the driver's is above)
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
