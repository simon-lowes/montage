#include "CaptureDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QJsonArray>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>

#include "EditorState.h"
#include "ReferenceOutputPanel.h"
#include "Settings.h"
#include "audio/AudioDevice.h"
#include "media/ImageSequence.h"
#include "render/Nmos.h"
#include "render/St2110Receive.h"
#ifdef MONTAGE_WITH_OMT
#include "media/Omt.h"
#endif

namespace montage {

namespace {

QString deviceLabel(const QString& kind) {
    if (kind == QLatin1String("v4l2")) return QObject::tr("Video4Linux camera or capture card");
    if (kind == QLatin1String("alsa")) return QObject::tr("ALSA sound card");
    if (kind == QLatin1String("pulse")) return QObject::tr("PulseAudio sound");
    if (kind == QLatin1String("avfoundation")) return QObject::tr("AVFoundation camera or capture card");
    if (kind == QLatin1String("dshow")) return QObject::tr("DirectShow camera or capture card");
    if (kind == QLatin1String("lavfi")) return QObject::tr("Test pattern (FFmpeg filter)");
    if (kind == QLatin1String("omt")) return QObject::tr("Open Media Transport source (on the network)");
    if (kind == QLatin1String("st2110")) return QObject::tr("SMPTE ST 2110 streams (from their SDP files)");
    return kind;
}

QString clock(double seconds) {
    const int s = int(seconds);
    return QStringLiteral("%1:%2:%3").arg(s / 3600, 2, 10, QLatin1Char('0')).arg(s / 60 % 60, 2, 10, QLatin1Char('0')).arg(s % 60, 2, 10, QLatin1Char('0'));
}

}  // namespace

CaptureDialog::CaptureDialog(EditorState* state, QWidget* parent) : QDialog(parent), state_(state) {
    setWindowTitle(tr("Capture"));
    setObjectName(QStringLiteral("captureDialog"));
    QSettings settings = appSettings();
    auto* layout = new QVBoxLayout(this);
    auto* intro = new QLabel(tr("Records a live feed (a network stream or a camera, capture card or sound card on this computer) "
                                "with its timecode. A QuickTime recording is in the Captures bin a second after it starts and "
                                "grows there as it records, so it can be cut in straight away."),
                             this);
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto* form = new QFormLayout;

    kind_ = new QComboBox(this);
    kind_->setObjectName(QStringLiteral("captureKind"));
    kind_->addItem(tr("Network stream (SRT, RIST, UDP, RTMP...)"), QString());
    for (const std::string& k : captureDeviceKinds()) kind_->addItem(deviceLabel(QString::fromStdString(k)), QString::fromStdString(k));
    form->addRow(tr("Feed:"), kind_);
    url_ = new QLineEdit(settings.value(QStringLiteral("capture/url")).toString(), this);
    url_->setObjectName(QStringLiteral("captureUrl"));
    auto* urlRow = new QHBoxLayout;
    urlRow->addWidget(url_, 1);
    // Open Media Transport sources announced on the network, to choose from.
    find_ = new QPushButton(tr("Find…"), this);
    find_->setObjectName(QStringLiteral("captureFindSources"));
    urlRow->addWidget(find_);
    connect(find_, &QPushButton::clicked, this, [this] {
        if (kind_->currentData().toString() == QLatin1String("st2110")) {
            // (the video's SDP, and the sound's and ancillary data's when they come in files of their own)
            const QStringList files = QFileDialog::getOpenFileNames(this, tr("SDP Files"), QString(), tr("SDP files (*.sdp);;All files (*)"));
            if (!files.isEmpty()) url_->setText(files.join(QLatin1Char(';')));
            return;
        }
#ifdef MONTAGE_WITH_OMT
        setCursor(Qt::WaitCursor);
        std::string why;
        const std::vector<omt::Source> sources = omt::discover(1.5, &why);
        unsetCursor();
        QMenu menu(this);
        for (const omt::Source& s : sources) {
            const QString url = QStringLiteral("omt://%1:%2").arg(QString::fromStdString(s.host.find(':') != std::string::npos ? "[" + s.host + "]" : s.host)).arg(s.port);
            menu.addAction(QString::fromStdString(s.name), this, [this, url] { url_->setText(url); });
        }
        if (sources.empty()) menu.addAction(why.empty() ? tr("No sources found") : tr("No sources found (%1)").arg(QString::fromStdString(why)))->setEnabled(false);
        menu.exec(find_->mapToGlobal(QPoint(0, find_->height())));
#endif
    });
    form->addRow(tr("Address or device:"), urlRow);
    sound_ = new QLineEdit(this);
    sound_->setObjectName(QStringLiteral("captureSound"));
    sound_->setPlaceholderText(tr("The feed's own sound"));
    form->addRow(tr("Sound device:"), sound_);
    // The sound's latency, taken off so it lines up with the pictures: the device's input latency as Monitoring's audio
    // device reports it (a measured loopback's half where one was run), or set by hand.
    soundLatency_ = new QDoubleSpinBox(this);
    soundLatency_->setObjectName(QStringLiteral("captureSoundLatency"));
    soundLatency_->setRange(0, 2000);
    soundLatency_->setDecimals(1);
    soundLatency_->setSuffix(tr(" ms"));
    soundLatency_->setToolTip(tr("How late a sound device's sound arrives (its input latency): taken off, so it lines up with the pictures (a feed's own sound keeps its times)"));
    if (const audiodev::Engine& engine = audiodev::Engine::shared(); engine.active()) {
        const audiodev::EngineStatus st = engine.status();
        if (st.rate > 0) soundLatency_->setValue(1000.0 * (st.measured ? st.roundTrip / 2 : st.latency.input) / st.rate);
    }
    form->addRow(tr("Sound latency:"), soundLatency_);
    options_ = new QLineEdit(settings.value(QStringLiteral("capture/options")).toString(), this);
    options_->setObjectName(QStringLiteral("captureOptions"));
    options_->setPlaceholderText(tr("FFmpeg's input options, e.g. video_size=1920x1080 framerate=25"));
    form->addRow(tr("Options:"), options_);
    // ST 2110 receivers for a facility's broadcast controller to connect (its registry as the reference output's).
    nmos_ = new QCheckBox(tr("Offer to NMOS controllers (their connection becomes the feed)"), this);
    nmos_->setObjectName(QStringLiteral("captureNmos"));
    nmosStatus_ = new QLabel(this);
    nmosStatus_->setObjectName(QStringLiteral("captureNmosStatus"));
    nmosStatus_->setWordWrap(true);
    auto* nmosBox = new QVBoxLayout;
    nmosBox->setContentsMargins(0, 0, 0, 0);
    nmosBox->addWidget(nmos_);
    nmosBox->addWidget(nmosStatus_);
    form->addRow(QString(), nmosBox);
    connect(nmos_, &QCheckBox::toggled, this, [this](bool on) {
        if (bool(node_ && node_->running()) == on) return;
        QString why;
        if (!offerNmos(on, &why)) nmosStatus_->setText(why);
    });
    auto placeholders = [this] {
        const QString k = kind_->currentData().toString();
        url_->setPlaceholderText(k.isEmpty() ? tr("srt://0.0.0.0:9000?mode=listener") : k == QLatin1String("v4l2") ? QStringLiteral("/dev/video0")
                                               : k == QLatin1String("avfoundation")                                 ? QStringLiteral("0:0")
                                               : k == QLatin1String("dshow")                                        ? tr("video=Camera name")
                                               : k == QLatin1String("omt")                                          ? tr("omt://host:6400, or HOST (Name)")
                                               : k == QLatin1String("st2110")                                       ? tr("SDP files, apart by ;")
                                                                                                                    : QString());
        const bool omt = k == QLatin1String("omt"), ip = k == QLatin1String("st2110");
        sound_->setEnabled(!k.isEmpty() && !omt && !ip);
        options_->setEnabled(!omt);
        options_->setPlaceholderText(ip ? tr("interface=address interface2=address (ST 2022-7's second path)")
                                        : tr("FFmpeg's input options, e.g. video_size=1920x1080 framerate=25"));
        find_->setText(ip ? tr("Choose…") : tr("Find…"));
        find_->setVisible(omt || ip);
        nmos_->setVisible(ip);
        nmosStatus_->setVisible(ip && !nmosStatus_->text().isEmpty());
    };
    connect(kind_, &QComboBox::currentIndexChanged, this, placeholders);
    if (const int i = kind_->findData(settings.value(QStringLiteral("capture/kind")).toString()); i >= 0) kind_->setCurrentIndex(i);
    placeholders();

    codec_ = new QComboBox(this);
    codec_->setObjectName(QStringLiteral("captureCodec"));
    for (const auto& [id, name] : captureCodecs()) codec_->addItem(QString::fromStdString(name), QString::fromStdString(id));
    if (const int i = codec_->findData(settings.value(QStringLiteral("capture/codec"), QStringLiteral("prores_hq")).toString()); i >= 0)
        codec_->setCurrentIndex(i);
    form->addRow(tr("Record as:"), codec_);
    wrapper_ = new QComboBox(this);
    wrapper_->setObjectName(QStringLiteral("captureWrapper"));
    wrapper_->addItem(tr("QuickTime (edit while it records)"), QStringLiteral("mov"));
    wrapper_->addItem(tr("MXF (imported once it stops)"), QStringLiteral("mxf"));
    if (const int i = wrapper_->findData(settings.value(QStringLiteral("capture/wrapper"), QStringLiteral("mov")).toString()); i >= 0)
        wrapper_->setCurrentIndex(i);
    form->addRow(tr("File:"), wrapper_);
    // (JPEG XS records to MXF only)
    auto codecChosen = [this] {
        const bool xs = codec_->currentData().toString() == QLatin1String("jpeg_xs");
        if (xs) wrapper_->setCurrentIndex(wrapper_->findData(QStringLiteral("mxf")));
        wrapper_->setEnabled(!xs);
    };
    connect(codec_, qOverload<int>(&QComboBox::currentIndexChanged), this, codecChosen);
    codecChosen();

    auto* folderRow = new QHBoxLayout;
    folder_ = new QLineEdit(settings.value(QStringLiteral("capture/folder"), QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)).toString(), this);
    folder_->setObjectName(QStringLiteral("captureFolder"));
    auto* browse = new QPushButton(tr("Choose…"), this);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString d = QFileDialog::getExistingDirectory(this, tr("Record Into"), folder_->text());
        if (!d.isEmpty()) setFolder(d);
    });
    folderRow->addWidget(folder_, 1);
    folderRow->addWidget(browse);
    form->addRow(tr("Folder:"), folderRow);
    name_ = new QLineEdit(tr("Capture 001"), this);
    name_->setObjectName(QStringLiteral("captureName"));
    form->addRow(tr("Clip name:"), name_);
    timecode_ = new QLineEdit(this);
    timecode_->setObjectName(QStringLiteral("captureTimecode"));
    timecode_->setPlaceholderText(tr("The feed's own, else the time of day (hh:mm:ss:ff, ; for drop frame)"));
    timecode_->setInputMask(QString());
    form->addRow(tr("Start timecode:"), timecode_);
    seconds_ = new QDoubleSpinBox(this);
    seconds_->setObjectName(QStringLiteral("captureSeconds"));
    seconds_->setRange(0, 24 * 3600);
    seconds_->setDecimals(1);
    seconds_->setSuffix(tr(" s"));
    seconds_->setSpecialValueText(tr("Until stopped"));
    form->addRow(tr("Length:"), seconds_);
    import_ = new QCheckBox(tr("Import into the Captures bin"), this);
    import_->setObjectName(QStringLiteral("captureImport"));
    import_->setChecked(settings.value(QStringLiteral("capture/import"), true).toBool());
    form->addRow(QString(), import_);
    watch_ = new QCheckBox(tr("Watch it on the Source monitor (mark In and Out as it records)"), this);
    watch_->setObjectName(QStringLiteral("captureWatch"));
    watch_->setChecked(settings.value(QStringLiteral("capture/watch"), true).toBool());
    form->addRow(QString(), watch_);
    layout->addLayout(form);

    // A live package of the feed as it records: HLS and DASH for players, from a folder, served from here if asked.
    live_ = new QGroupBox(tr("Live package while recording"), this);
    live_->setObjectName(QStringLiteral("captureLive"));
    live_->setCheckable(true);
    live_->setChecked(settings.value(QStringLiteral("capture/live"), false).toBool());
    auto* liveForm = new QFormLayout(live_);
    auto* liveFolderRow = new QHBoxLayout;
    liveFolder_ = new QLineEdit(settings.value(QStringLiteral("capture/liveFolder")).toString(), live_);
    liveFolder_->setObjectName(QStringLiteral("captureLiveFolder"));
    liveFolder_->setPlaceholderText(tr("The package's folder (what it held is replaced)"));
    auto* liveBrowse = new QPushButton(tr("Choose…"), live_);
    connect(liveBrowse, &QPushButton::clicked, this, [this] {
        const QString d = QFileDialog::getExistingDirectory(this, tr("Live Package Folder"), liveFolder_->text());
        if (!d.isEmpty()) liveFolder_->setText(d);
    });
    liveFolderRow->addWidget(liveFolder_, 1);
    liveFolderRow->addWidget(liveBrowse);
    liveForm->addRow(tr("Folder:"), liveFolderRow);
    liveFormats_ = new QComboBox(live_);
    liveFormats_->setObjectName(QStringLiteral("captureLiveFormats"));
    liveFormats_->addItem(tr("HLS and DASH"), QStringLiteral("both"));
    liveFormats_->addItem(tr("HLS"), QStringLiteral("hls"));
    liveFormats_->addItem(tr("DASH"), QStringLiteral("dash"));
    liveForm->addRow(tr("For:"), liveFormats_);
    liveSegments_ = new QComboBox(live_);
    liveSegments_->setObjectName(QStringLiteral("captureLiveSegments"));
    liveSegments_->addItem(tr("CMAF"), QStringLiteral("cmaf"));
    liveSegments_->addItem(tr("MPEG-TS for HLS (CMAF for DASH)"), QStringLiteral("ts"));
    liveForm->addRow(tr("Segments:"), liveSegments_);
    liveSound_ = new QComboBox(live_);
    liveSound_->setObjectName(QStringLiteral("captureLiveSound"));
    liveSound_->addItem(tr("AAC"), QStringLiteral("aac"));
    liveSound_->addItem(tr("Opus"), QStringLiteral("libopus"));
    liveSound_->addItem(tr("AC-3 (to 5.1)"), QStringLiteral("ac3"));
    liveSound_->addItem(tr("E-AC-3 (to 7.1)"), QStringLiteral("eac3"));
    liveSound_->addItem(tr("None"), QStringLiteral("none"));
    liveForm->addRow(tr("Sound:"), liveSound_);
    liveLowLatency_ = new QCheckBox(tr("Low latency (segments sent in parts as they are made)"), live_);
    liveLowLatency_->setObjectName(QStringLiteral("captureLiveLowLatency"));
    liveForm->addRow(QString(), liveLowLatency_);
    liveWindow_ = new QSpinBox(live_);
    liveWindow_->setObjectName(QStringLiteral("captureLiveWindow"));
    liveWindow_->setRange(3, 100);
    liveWindow_->setValue(6);
    liveWindow_->setSuffix(tr(" segments of 2 s"));
    liveForm->addRow(tr("Window:"), liveWindow_);
    auto* serveRow = new QHBoxLayout;
    liveServe_ = new QCheckBox(tr("Serve on port"), live_);
    liveServe_->setObjectName(QStringLiteral("captureLiveServe"));
    livePort_ = new QSpinBox(live_);
    livePort_->setObjectName(QStringLiteral("captureLivePort"));
    livePort_->setRange(0, 65535);
    livePort_->setValue(settings.value(QStringLiteral("capture/livePort"), 8080).toInt());
    livePort_->setSpecialValueText(tr("Any free"));
    liveNetwork_ = new QCheckBox(tr("To the network"), live_);
    liveNetwork_->setObjectName(QStringLiteral("captureLiveNetwork"));
    liveNetwork_->setToolTip(tr("Off: players on this computer only"));
    serveRow->addWidget(liveServe_);
    serveRow->addWidget(livePort_);
    serveRow->addWidget(liveNetwork_);
    serveRow->addStretch(1);
    liveForm->addRow(QString(), serveRow);
    liveLanguages_ = new QCheckBox(tr("Every sound stream of the feed, each in its language"), live_);
    liveLanguages_->setObjectName(QStringLiteral("captureLiveLanguages"));
    liveLanguages_->setToolTip(tr("The recording keeps the main sound"));
    liveForm->addRow(QString(), liveLanguages_);
    liveCaptions_ = new QComboBox(live_);
    liveCaptions_->setObjectName(QStringLiteral("captureLiveCaptions"));
    liveCaptions_->addItem(tr("The feed's, when it has them"), QStringLiteral("auto"));
    liveCaptions_->addItem(tr("Always listed"), QStringLiteral("always"));
    liveCaptions_->addItem(tr("None"), QStringLiteral("none"));
    liveForm->addRow(tr("Captions:"), liveCaptions_);
    // (while it records: an ad break or a chapter from the next picture)
    auto* markRow = new QHBoxLayout;
    liveCue_ = new QPushButton(tr("Ad Break"), live_);
    liveCue_->setObjectName(QStringLiteral("captureLiveCue"));
    liveCue_->setToolTip(tr("An ad break from the next picture, on a keyframe: a cue to the players and to what they pass it to"));
    liveBreak_ = new QDoubleSpinBox(live_);
    liveBreak_->setObjectName(QStringLiteral("captureLiveBreak"));
    liveBreak_->setRange(0, 3600);
    liveBreak_->setDecimals(1);
    liveBreak_->setValue(settings.value(QStringLiteral("capture/liveBreak"), 30.0).toDouble());
    liveBreak_->setSuffix(tr(" s"));
    liveBreak_->setSpecialValueText(tr("Length not known"));
    liveChapter_ = new QPushButton(tr("Chapter"), live_);
    liveChapter_->setObjectName(QStringLiteral("captureLiveChapter"));
    liveChapter_->setToolTip(tr("A chapter from the next picture, in the package's chapter list"));
    markRow->addWidget(liveCue_);
    markRow->addWidget(liveBreak_);
    markRow->addWidget(liveChapter_);
    markRow->addStretch(1);
    liveForm->addRow(tr("While recording:"), markRow);
    connect(liveCue_, &QPushButton::clicked, this, [this] { liveCue(liveBreak_->value()); });
    connect(liveChapter_, &QPushButton::clicked, this, [this] { liveChapter(); });
    liveCue_->setEnabled(false);
    liveChapter_->setEnabled(false);
    auto liveEnable = [this] {
        // (low latency's parts are CMAF chunks; MPEG-TS is HLS's alone)
        const bool ts = liveSegments_->currentData().toString() == QLatin1String("ts");
        if (ts) liveLowLatency_->setChecked(false);
        liveLowLatency_->setEnabled(!ts);
        if (ts && liveSound_->currentData().toString() == QLatin1String("libopus")) liveSound_->setCurrentIndex(0);
        livePort_->setEnabled(liveServe_->isChecked());
        liveNetwork_->setEnabled(liveServe_->isChecked());
    };
    connect(liveSegments_, &QComboBox::currentIndexChanged, this, liveEnable);
    connect(liveServe_, &QCheckBox::toggled, this, liveEnable);
    liveEnable();
    layout->addWidget(live_);

    status_ = new QLabel(tr("Not recording"), this);
    status_->setObjectName(QStringLiteral("captureStatus"));
    status_->setWordWrap(true);
    layout->addWidget(status_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    record_ = buttons->addButton(tr("Record"), QDialogButtonBox::ActionRole);
    record_->setObjectName(QStringLiteral("captureRecord"));
    connect(record_, &QPushButton::clicked, this, [this] { isRecording() ? stop() : (void)record(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::hide);  // (recording goes on with it hidden)
    layout->addWidget(buttons);

    tick_ = new QTimer(this);
    tick_->setInterval(250);
    connect(tick_, &QTimer::timeout, this, &CaptureDialog::refresh);
}

CaptureDialog::~CaptureDialog() {
    // (the recorder's notices, queued to this, go with it)
    if (recorder_) recorder_->stop();
    endLive();
    if (liveFinish_.joinable()) liveFinish_.join();
    if (origin_) origin_->stop();
    if (node_) node_->stop();
}

void CaptureDialog::setLive(const LiveOptions& o) {
    live_->setChecked(!o.folder.isEmpty());
    if (!o.folder.isEmpty()) liveFolder_->setText(o.folder);
    liveFormats_->setCurrentIndex(o.hls && o.dash ? 0 : o.hls ? 1 : 2);
    if (const int i = liveSegments_->findData(o.segments); i >= 0) liveSegments_->setCurrentIndex(i);
    if (const int i = liveSound_->findData(o.sound); i >= 0) liveSound_->setCurrentIndex(i);
    liveLowLatency_->setChecked(o.lowLatency);
    liveWindow_->setValue(o.window);
    liveServe_->setChecked(o.serve);
    livePort_->setValue(o.port);
    liveNetwork_->setChecked(o.network);
    liveLanguages_->setChecked(o.languages);
    if (const int i = liveCaptions_->findData(o.captions); i >= 0) liveCaptions_->setCurrentIndex(i);
}

CaptureDialog::LiveOptions CaptureDialog::live() const {
    LiveOptions o;
    if (live_->isChecked()) o.folder = liveFolder_->text().trimmed();
    const QString f = liveFormats_->currentData().toString();
    o.hls = f != QLatin1String("dash"), o.dash = f != QLatin1String("hls");
    o.segments = liveSegments_->currentData().toString();
    o.sound = liveSound_->currentData().toString();
    o.lowLatency = liveLowLatency_->isChecked();
    o.window = liveWindow_->value();
    o.serve = liveServe_->isChecked();
    o.port = livePort_->value();
    o.network = liveNetwork_->isChecked();
    o.languages = liveLanguages_->isChecked();
    o.captions = liveCaptions_->currentData().toString();
    return o;
}

QString CaptureDialog::liveUrl() const { return liveUrl_; }

bool CaptureDialog::liveCue(double duration, QString* error) {
    if (!packager_ || !recording_ || !packager_->status().running) {
        if (error) *error = tr("No live package is being made");
        return false;
    }
    LiveCue c;
    c.duration = duration < 0 ? liveBreak_->value() : duration;
    packager_->cue(c);
    appSettings().setValue(QStringLiteral("capture/liveBreak"), liveBreak_->value());
    return true;
}

bool CaptureDialog::liveChapter(const QString& title, QString* error) {
    if (!packager_ || !recording_ || !packager_->status().running) {
        if (error) *error = tr("No live package is being made");
        return false;
    }
    ++liveChapters_;
    packager_->chapter((title.trimmed().isEmpty() ? tr("Chapter %1").arg(liveChapters_) : title.trimmed()).toStdString());
    return true;
}

bool CaptureDialog::startLive(CaptureSettings& cs) {
    // (the last recording's package, and its serving, end here: its folder may be the same)
    endLive();
    if (liveFinish_.joinable()) liveFinish_.join();
    if (origin_) origin_->stop();
    origin_.reset();
    packager_.reset();
    liveUrl_.clear();
    const LiveOptions o = live();
    if (!live_->isChecked()) return true;
    if (o.folder.isEmpty()) {
        error_ = tr("Choose a folder for the live package");
        return false;
    }
    LiveSettings ls;
    ls.folder = o.folder.toStdString();
    ls.hls = o.hls, ls.dash = o.dash;
    ls.segmentFormat = o.segments.toStdString();
    ls.audioCodec = o.sound.toStdString();
    ls.lowLatency = o.lowLatency;
    ls.window = o.window;
    ls.captions = o.captions.toStdString();
    auto packager = std::make_unique<LivePackager>();
    std::string why;
    if (!packager->start(ls, &why)) {
        error_ = tr("Live package: %1").arg(QString::fromStdString(why));
        return false;
    }
    if (o.serve) {
        auto origin = std::make_unique<LiveOrigin>();
        LivePackager* p = packager.get();
        if (!origin->start(ls.folder, o.port, [p](const std::string& rel) { return p->growing(rel); }, &why,
                           o.network ? "0.0.0.0" : "127.0.0.1")) {
            packager->finish();
            error_ = tr("Live package: %1").arg(QString::fromStdString(why));
            return false;
        }
        liveUrl_ = QStringLiteral("http://127.0.0.1:%1/").arg(origin->port());
        origin_ = std::move(origin);
    }
    LivePackager* p = packager.get();
    cs.onPicture = [p](const AVFrame* f, int num, int den) { p->picture(f, num, den); };
    cs.onSound = [p](const AVFrame* f) { p->sound(f); };
    if (o.languages) {
        cs.allSound = true;
        cs.onTrackSound = [p](int track, const AVFrame* f) { p->sound(f, track); };
        cs.onSoundTracks = [p](const std::vector<CaptureSettings::SoundTrack>& tracks) {
            std::vector<LiveSoundTrack> list;
            for (const auto& t : tracks) list.push_back({t.language, t.title, t.channels});
            p->soundTracks(list);
        };
    }
    // (the feed's own ad cues, an MPEG transport stream's)
    cs.onCue = [p](const std::vector<uint8_t>& section, double zero, double at) { p->cueFromFeed(section, zero, at); };
    liveChapters_ = 0;
    packager_ = std::move(packager);
    QSettings settings = appSettings();
    settings.setValue(QStringLiteral("capture/liveFolder"), o.folder);
    if (o.serve) settings.setValue(QStringLiteral("capture/livePort"), o.port);
    return true;
}

void CaptureDialog::endLive() {
    // (the recording has ended: what was queued is packaged and the playlists end, on a thread of its own so the
    // window does not wait for the encoders; the origin goes on serving them)
    if (!packager_ || !packager_->status().running || liveFinish_.joinable()) return;
    LivePackager* p = packager_.get();
    liveFinish_ = std::thread([p] { p->finish(); });
}

bool CaptureDialog::offerNmos(bool on, QString* error) {
    auto show = [this](const QString& text) {
        nmosStatus_->setText(text);
        nmosStatus_->setVisible(nmos_->isVisible() && !text.isEmpty());
    };
    if (!on) {
        if (node_) node_->stop();
        const QSignalBlocker block(nmos_);
        nmos_->setChecked(false);
        show(QString());
        return true;
    }
    if (!node_) {
        node_ = new nmos::Node(this);
        node_->onReceiverActivated = [this](const nmos::ReceiverConnection&) { nmosConnected(); };
        node_->onRegistered = [this](bool) { nmosStatus_->setText(nmosStatus()); };
    }
    nmos::NodeOptions o = ReferenceOutputController::loadNmos();
    o.label = tr("Montage capture");
    o.port = 0;  // (the reference output's node may hold the one set)
    o.senders = false;
    o.receivers = true;
    // (the paths' interfaces, as the options give them)
    for (const QString& part : options_->text().split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        if (part.startsWith(QLatin1String("interface="))) o.address = part.mid(10);
        if (part.startsWith(QLatin1String("interface2="))) o.interface2 = part.mid(11);
    }
    if (node_->running()) node_->stop();
    QString why;
    if (!node_->start(o, &why)) {
        const QSignalBlocker block(nmos_);
        nmos_->setChecked(false);
        why = tr("NMOS: %1").arg(why);
        show(why);
        if (error) *error = why;
        return false;
    }
    const QSignalBlocker block(nmos_);
    nmos_->setChecked(true);
    show(nmosStatus());
    return true;
}

QString CaptureDialog::nmosStatus() const {
    if (!node_ || !node_->running()) return QString();
    const QString where = node_->registered() ? tr("registered with %1").arg(node_->registry()) : tr("at %1, no registry yet").arg(node_->apiUrl());
    const nmos::ReceiverConnection c = node_->receiverConnection(0);
    if (!c.active) return tr("Offered to NMOS controllers (%1): not connected").arg(where);
    return tr("Connected by an NMOS controller (%1)%2").arg(where, isRecording() ? tr(", for the next recording") : QString());
}

void CaptureDialog::nmosConnected() {
    // What the receivers now take, as the feed: the streams' SDPs (their paths as the controller placed them) and the
    // legs' interfaces.
    QString why;
    const std::vector<st2110::SdpStream> streams = node_->receiverStreams(&why);
    std::string text;
    bool video = false;
    for (const st2110::SdpStream& s : streams) text += st2110::sdpText(s), video = video || s.encoding == "raw";
    if (video) {
        setSource(QStringLiteral("st2110"), QString::fromStdString(text));
        nmosFeed_ = url_->text();
        // (each path's own: with the second leg alone, it is the first path)
        const QStringList interfaces = node_->receiverInterfaces();
        QStringList options;
        if (interfaces.size() > 0) options << QStringLiteral("interface=") + interfaces[0];
        if (interfaces.size() > 1) options << QStringLiteral("interface2=") + interfaces[1];
        // (the sound's and the data's receivers, where a controller joins them elsewhere)
        for (int stream : {1, 2}) {
            const QStringList own = node_->receiverInterfaces(stream);
            const QString kind = stream == 1 ? QStringLiteral("audio_") : QStringLiteral("data_");
            for (int i = 0; i < 2 && i < own.size(); ++i)
                if (own[i] != interfaces.value(i)) options << kind + (i ? QStringLiteral("interface2=") : QStringLiteral("interface=")) + own[i];
        }
        setOptions(options.join(QLatin1Char(' ')));
    } else if (!nmosFeed_.isEmpty() && url_->text() == nmosFeed_) {
        // (disconnected, its picture at least: what it connected is no longer the feed, unless someone has typed
        // another since)
        url_->clear();
        setOptions(QString());
        nmosFeed_.clear();
    }
    nmosStatus_->setText(why.isEmpty() ? nmosStatus() : tr("NMOS: %1").arg(why));
    nmosStatus_->setVisible(nmos_->isVisible());
}

void CaptureDialog::setSource(const QString& kind, const QString& url) {
    if (const int i = kind_->findData(kind); i >= 0) kind_->setCurrentIndex(i);
    url_->setText(url);
}
void CaptureDialog::setSoundDevice(const QString& name) { sound_->setText(name); }
void CaptureDialog::setOptions(const QString& options) { options_->setText(options); }
void CaptureDialog::setCodec(const QString& id) {
    if (const int i = codec_->findData(id); i >= 0) codec_->setCurrentIndex(i);
}
void CaptureDialog::setWrapper(const QString& ext) {
    if (const int i = wrapper_->findData(ext); i >= 0) wrapper_->setCurrentIndex(i);
}
void CaptureDialog::setFolder(const QString& dir) { folder_->setText(dir); }
void CaptureDialog::setClipName(const QString& name) { name_->setText(name); }
void CaptureDialog::setTimecode(const QString& tc) { timecode_->setText(tc); }
void CaptureDialog::setSeconds(double s) { seconds_->setValue(s); }

bool CaptureDialog::isRecording() const { return recording_; }

bool CaptureDialog::record() {
    if (isRecording()) return false;
    error_.clear();
    imported_ = 0;
    const QString tc = timecode_->text().trimmed();
    if (!tc.isEmpty() && !QRegularExpression(QStringLiteral("^\\d{2}:\\d{2}:\\d{2}[:;]\\d{2}$")).match(tc).hasMatch()) {
        error_ = tr("Timecode is hh:mm:ss:ff");
        refresh();
        return false;
    }
    const QString dir = folder_->text().trimmed();
    if (dir.isEmpty() || !QDir().mkpath(dir)) {
        error_ = tr("Choose a folder to record into");
        refresh();
        return false;
    }
    // (a name taken: the next number)
    QString name = name_->text().trimmed();
    if (name.isEmpty()) name = tr("Capture 001");
    name.replace(QRegularExpression(QStringLiteral("[/\\\\:*?\"<>|]")), QStringLiteral("_"));
    const QString ext = codec_->currentData().toString() == QLatin1String("jpeg_xs") ? QStringLiteral("mxf") : wrapper_->currentData().toString();
    while (QFileInfo::exists(QDir(dir).filePath(name + '.' + ext))) {
        const QRegularExpressionMatch m = QRegularExpression(QStringLiteral("^(.*?)(\\d+)$")).match(name);
        name = m.hasMatch() ? m.captured(1) + QStringLiteral("%1").arg(m.captured(2).toInt() + 1, m.captured(2).size(), 10, QLatin1Char('0'))
                            : name + QStringLiteral(" 2");
    }
    path_ = QDir(dir).filePath(name + '.' + ext);

    CaptureSettings cs;
    const QString kind = kind_->currentData().toString();
    cs.video.device = kind.toStdString();
    cs.video.url = url_->text().trimmed().toStdString();
    for (const QString& o : options_->text().split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        const int eq = int(o.indexOf(QLatin1Char('=')));
        if (eq <= 0) {
            error_ = tr("Options are name=value, apart by spaces");
            refresh();
            return false;
        }
        cs.video.options[o.left(eq).toStdString()] = o.mid(eq + 1).toStdString();
    }
    if (!kind.isEmpty() && kind != QLatin1String("omt") && kind != QLatin1String("st2110") && !sound_->text().trimmed().isEmpty()) cs.audio = CaptureSource{sound_->text().trimmed().toStdString(), kind.toStdString(), {}};
    cs.codec = codec_->currentData().toString().toStdString();
    cs.jpegXsBpp = jpegXsBpp_;
    cs.path = path_.toStdString();
    cs.timecode = tc.toStdString();
    cs.seconds = seconds_->value();
    cs.soundLatency = soundLatency_->value() / 1000.0;
    if (recorder_) recorder_->stop();
    // (watched at the Source monitor's size, about)
    std::shared_ptr<LiveFeed> watched = watch_->isChecked() ? std::make_shared<LiveFeed>(1280, 720) : nullptr;
    cs.live = watched;
    if (!startLive(cs)) {
        refresh();
        return false;
    }
    recorder_ = std::make_unique<Recorder>();
    const int generation = ++generation_;
    QPointer<CaptureDialog> self(this);
    recorder_->onFirstFragment = [self, generation] {
        QMetaObject::invokeMethod(self.data(), [self, generation] {
            if (self && self->generation_ == generation) self->importNow();
        }, Qt::QueuedConnection);
    };
    recorder_->onFinished = [self, generation](bool ok, const std::string& message) {
        const QString m = QString::fromStdString(message);
        QMetaObject::invokeMethod(self.data(), [self, generation, ok, m] {
            if (self) self->finished(generation, ok, m);
        }, Qt::QueuedConnection);
    };
    std::string err;
    if (!recorder_->start(cs, &err)) {
        error_ = QString::fromStdString(err);
        recorder_.reset();
        endLive();
        refresh();
        return false;
    }
    QSettings settings = appSettings();
    settings.setValue(QStringLiteral("capture/kind"), kind);
    settings.setValue(QStringLiteral("capture/url"), url_->text().trimmed());
    settings.setValue(QStringLiteral("capture/codec"), codec_->currentData());
    settings.setValue(QStringLiteral("capture/wrapper"), ext);
    settings.setValue(QStringLiteral("capture/folder"), dir);
    settings.setValue(QStringLiteral("capture/import"), import_->isChecked());
    settings.setValue(QStringLiteral("capture/options"), options_->text().trimmed());
    settings.setValue(QStringLiteral("capture/live"), live_->isChecked());
    settings.setValue(QStringLiteral("capture/watch"), watch_->isChecked());
    recording_ = true;
    liveFeed_ = watched;
    liveMarkable_ = import_->isChecked() && ext == QLatin1String("mov");
    notedFrames_ = -1;
    importedPath_.clear();
    record_->setText(tr("Stop"));
    tick_->start();
    refresh();
    emit recordingChanged();
    if (liveFeed_) emit watching(liveFeed_, 0, clipLabel(), true);
    return true;
}

void CaptureDialog::setWatchLive(bool on) { watch_->setChecked(on); }
bool CaptureDialog::watchLive() const { return watch_->isChecked(); }
QString CaptureDialog::clipLabel() const { return QFileInfo(path_).completeBaseName(); }

void CaptureDialog::stop() {
    if (recorder_) recorder_->stop();  // (finished() follows, queued)
}

void CaptureDialog::importNow() {
    if (imported_ || !import_->isChecked() || path_.isEmpty() || !QFileInfo::exists(path_)) return;
    QStringList errors;
    const std::vector<Id> ids = state_->importFiles({path_}, &errors, tr("Captures"));
    if (!ids.empty()) {
        imported_ = ids.front();
        // (as the project names it; kept, as another project opened meanwhile may have a media item of the same id)
        if (const MediaItem* m = state_->project().findMedia(imported_)) importedPath_ = QString::fromStdString(mediaFileOnDisk(m->path));
        if (liveFeed_ && recording_) emit watching(liveFeed_, imported_, clipLabel(), true);
    }
    else if (!errors.isEmpty()) error_ = errors.front();
    refresh();
}

void CaptureDialog::finished(int generation, bool ok, const QString& message) {
    if (generation != generation_) return;  // (an earlier recording's)
    recording_ = false;
    record_->setText(tr("Record"));
    endLive();
    if (!packager_) tick_->stop();  // (else once the package is finished: refresh)
    if (!message.isEmpty()) error_ = message;  // (why it failed, or what went wrong in a recording kept)
    // (an MXF file, or a recording shorter than its first fragment, is imported now it is whole)
    if (ok) importNow();
    if (std::shared_ptr<LiveFeed> ended = std::move(liveFeed_)) emit watching(ended, imported_, clipLabel(), false);
    // The next clip's name: the next number.
    const QRegularExpressionMatch m = QRegularExpression(QStringLiteral("^(.*?)(\\d+)$")).match(QFileInfo(path_).completeBaseName());
    if (ok && m.hasMatch()) name_->setText(m.captured(1) + QStringLiteral("%1").arg(m.captured(2).toInt() + 1, m.captured(2).size(), 10, QLatin1Char('0')));
    refresh();
    emit recordingChanged();
}

QString CaptureDialog::statusText() const { return status_->text(); }

// ---- The agent link's tool

QByteArray CaptureDialog::toolJson() {
    return QByteArrayLiteral(R"json({"name":"montage_capture","title":"Capture (record a live feed)",
"description":"Records a live feed to ProRes, DNxHR or JPEG XS (codec jpeg_xs: 10-bit 4:2:2 in MXF, ST 2124, at jpeg_xs_bpp bits a pixel, default 5) with its timecode, as File > Capture does; a QuickTime recording is imported into the Captures bin a second after it starts and grows there. \"kinds\": the devices this build opens (\"st2110\" for SMPTE ST 2110 streams, \"omt\" for Open Media Transport, FFmpeg's capture devices) and the codecs. \"record\": kind (\"\" a network stream's URL), url (for st2110 the SDP files apart by ';', or the SDP text itself; ST 2022-7's two paths come from the SDP's DUP group), options (\"name=value\" apart by spaces: FFmpeg's input options, or for st2110 interface= and interface2=, the network interfaces of the two paths, and audio_interface=, data_interface= and their 2s for a sound or data stream joined elsewhere), sound (a sound device of the same kind), codec, wrapper (mov or mxf), folder, name, timecode (\"\" the feed's own: ST 2110's from its ancillary data), seconds (0 until stopped), sound_latency_ms (how late the sound arrives, taken off; default Monitoring's audio device's input latency). \"stop\" and \"status\". \"nmos\" (offer true or false): ST 2110 receivers offered to a facility's NMOS controller (IS-04 and IS-05, registered as the reference output's node is); what it connects them to becomes the st2110 feed, recorded by \"record\" with kind st2110 and no url. \"live\" with record: a live package of the feed as it records (HLS and DASH with a sliding window of window segments of 2 s, into folder; low_latency: in parts sent as they are made; segments ts: HLS in MPEG-TS; sound aac, libopus, ac3, eac3 or none; serve: from here over HTTP on port, network for other computers too; languages: every sound stream of the feed, each a rendition in its language; captions auto, always or none: the feed's CEA-608/708 captions as WebVTT and IMSC 1; the feed's SCTE 35 cues placed on keyframes as EXT-X-DATERANGE and DASH events); watch (default true): the recording watched live on the Source monitor. \"live\": the Source monitor watching a feed live (watch true: the recording File > Capture is making; input: a multiviewer input's index; watch false: back to the media), mark_in or mark_out (one a call) at the picture shown now, as frames of its recording (cut in at once from the growing file), and edit (insert, overwrite, append or place_on_top) the marked range into the sequence at its playhead; answers its label, timecode, frame, media, in and out. \"cue\" while a live package is made: an ad break from the next picture recorded, on a keyframe made there (duration: its length in seconds, 0 not known); \"chapter\": a chapter from there (title) in the package's chapters.vtt. \"multiview\": up to 16 inputs watched side by side (inputs: [{label, kind, url, options, record}] added, as \"record\" takes a feed; remove: an index; clear), the chosen ones (choose: indices, or each input's record) recorded while record is true, each into folder (codec as \"record\") and the Captures bin; source: an input's index shown on the Source monitor (-1 none); an input being watched is recorded, and its recording ended, from the same reception (its feed is not opened again); answers each input's label, running, recording, on_source, timecode, pictures, peaks_dbfs, path, media and opened (the times its feed was opened).",
"inputSchema":{"type":"object","properties":{
"action":{"type":"string","enum":["kinds","record","stop","status","nmos","live","multiview","cue","chapter"]},"offer":{"type":"boolean"},
"duration":{"type":"number","minimum":0,"maximum":3600},"title":{"type":"string"},
"watch":{"type":"boolean"},"input":{"type":"integer","minimum":0,"maximum":15},"mark_in":{"type":"boolean"},"mark_out":{"type":"boolean"},
"edit":{"type":"string","enum":["insert","overwrite","append","place_on_top"]},
"inputs":{"type":"array","maxItems":16,"items":{"type":"object","properties":{"label":{"type":"string"},"kind":{"type":"string"},"url":{"type":"string"},"options":{"type":"string"},"record":{"type":"boolean"}}}},
"remove":{"type":"integer","minimum":0},"clear":{"type":"boolean"},"choose":{"type":"array","items":{"type":"integer"}},
"record":{"type":"boolean"},"source":{"type":"integer","minimum":-1,"maximum":15},
"kind":{"type":"string"},"url":{"type":"string"},"options":{"type":"string"},"sound":{"type":"string"},
"codec":{"type":"string"},"jpeg_xs_bpp":{"type":"number","minimum":0.5,"maximum":24},"wrapper":{"type":"string","enum":["mov","mxf"]},"folder":{"type":"string"},"name":{"type":"string"},
"timecode":{"type":"string"},"seconds":{"type":"number","minimum":0},"sound_latency_ms":{"type":"number","minimum":0,"maximum":2000},
"live":{"type":"object","properties":{"folder":{"type":"string"},"hls":{"type":"boolean","default":true},"dash":{"type":"boolean","default":true},
"low_latency":{"type":"boolean","default":false},"window":{"type":"integer","minimum":3,"maximum":100,"default":6},
"segments":{"type":"string","enum":["cmaf","ts"],"default":"cmaf"},"sound":{"type":"string","enum":["aac","libopus","ac3","eac3","none"],"default":"aac"},
"serve":{"type":"boolean","default":false},"port":{"type":"integer","minimum":0,"maximum":65535,"default":0},"network":{"type":"boolean","default":false},
"languages":{"type":"boolean","default":false},"captions":{"type":"string","enum":["auto","always","none"],"default":"auto"}},"required":["folder"]}},"required":["action"]}})json");
}

QString CaptureDialog::runTool(const QJsonObject& args, QJsonObject& structured, bool& isError) {
    isError = false;
    const QString action = args.value("action").toString();
    if (action == QLatin1String("kinds")) {
        QJsonArray kinds, codecs;
        for (const std::string& k : captureDeviceKinds()) kinds.append(QString::fromStdString(k));
        for (const auto& [id, name] : captureCodecs()) codecs.append(QJsonObject{{"id", QString::fromStdString(id)}, {"name", QString::fromStdString(name)}});
        structured["kinds"] = kinds;
        structured["codecs"] = codecs;
        return tr("%n kind(s) of device", nullptr, int(kinds.size()));
    }
    if (action == QLatin1String("record")) {
        if (isRecording()) {
            isError = true;
            return tr("Already recording %1").arg(QDir::toNativeSeparators(path_));
        }
        const QString kind = args.value("kind").toString();
        if (kind_->findData(kind) < 0) {
            isError = true;
            return tr("This build does not open \"%1\" (see \"kinds\")").arg(kind);
        }
        // (no url: the feed as it is, an NMOS controller's connection say)
        const bool same = !args.contains("url") && kind == kind_->currentData().toString();
        setSource(kind, same ? url_->text() : args.value("url").toString());
        setSoundDevice(args.value("sound").toString());
        if (!same || args.contains("options")) setOptions(args.value("options").toString());
        if (args.contains("codec")) {
            if (codec_->findData(args.value("codec").toString()) < 0) {
                isError = true;
                return tr("This build does not record \"%1\" (see \"kinds\")").arg(args.value("codec").toString());
            }
            setCodec(args.value("codec").toString());
        }
        if (args.contains("jpeg_xs_bpp")) {
            const double bpp = args.value("jpeg_xs_bpp").toDouble();
            if (!(bpp >= 0.5 && bpp <= 24)) {
                isError = true;
                return tr("jpeg_xs_bpp is 0.5 to 24 bits a pixel");
            }
            jpegXsBpp_ = bpp;
        }
        if (args.contains("wrapper")) setWrapper(args.value("wrapper").toString());
        if (args.contains("folder")) setFolder(args.value("folder").toString());
        if (args.contains("name")) setClipName(args.value("name").toString());
        setTimecode(args.value("timecode").toString());
        setSeconds(args.value("seconds").toDouble(0));
        if (args.contains("sound_latency_ms")) soundLatency_->setValue(args.value("sound_latency_ms").toDouble());
        if (args.contains("watch")) setWatchLive(args.value("watch").toBool());
        LiveOptions lo;  // (none unless asked)
        if (args.value("live").isObject()) {
            const QJsonObject l = args.value("live").toObject();
            lo.folder = l.value("folder").toString();
            if (lo.folder.isEmpty()) {
                isError = true;
                return tr("A live package needs a folder");
            }
            lo.hls = l.value("hls").toBool(true), lo.dash = l.value("dash").toBool(true);
            lo.lowLatency = l.value("low_latency").toBool();
            lo.window = l.value("window").toInt(6);
            lo.segments = l.value("segments").toString(QStringLiteral("cmaf"));
            lo.sound = l.value("sound").toString(QStringLiteral("aac"));
            lo.serve = l.value("serve").toBool();
            lo.port = l.value("port").toInt(0);
            lo.network = l.value("network").toBool();
            lo.languages = l.value("languages").toBool();
            lo.captions = l.value("captions").toString(QStringLiteral("auto"));
            if (liveCaptions_->findData(lo.captions) < 0) {
                isError = true;
                return tr("The live package's captions are auto, always or none");
            }
            if (lo.window < 3 || lo.window > 100 || (lo.segments != QLatin1String("cmaf") && lo.segments != QLatin1String("ts")) ||
                liveSound_->findData(lo.sound) < 0 || lo.port < 0 || lo.port > 65535) {
                isError = true;
                return tr("The live package's window is 3 to 100, its segments cmaf or ts, its sound aac, libopus, ac3, eac3 or none");
            }
            if (lo.lowLatency && lo.segments == QLatin1String("ts")) {
                isError = true;
                return tr("Low latency needs CMAF segments (its parts are CMAF chunks)");
            }
        }
        setLive(lo);
        if (!record()) {
            isError = true;
            return statusText();
        }
        structured["path"] = path_;
        QString text = tr("Recording to %1").arg(QDir::toNativeSeparators(path_));
        if (packager_) {
            structured["live_folder"] = lo.folder;
            text += QStringLiteral("\n") + tr("Packaged live into %1").arg(QDir::toNativeSeparators(lo.folder));
            if (!liveUrl_.isEmpty()) {
                structured["live_url"] = liveUrl_;
                text += QStringLiteral(", served at %1").arg(liveUrl_);
            }
        }
        return text;
    }
    if (action == QLatin1String("stop")) {
        stop();
        return isRecording() ? tr("Stopping") : tr("Not recording");
    }
    if (action == QLatin1String("status")) {
        refresh();
        structured["recording"] = isRecording();
        structured["path"] = path_;
        structured["status"] = statusText();
        if (imported_) structured["media"] = double(imported_);
        if (packager_) {
            const LiveStatus ls = packager_->status();
            structured["live"] = QJsonObject{{"running", ls.running}, {"segments", double(ls.segments)}, {"first_listed", double(ls.firstListed)},
                                             {"hls", QString::fromStdString(ls.hls)}, {"dash", QString::fromStdString(ls.dash)},
                                             {"url", liveUrl_}, {"error", QString::fromStdString(ls.error)},
                                             {"sound_renditions", ls.soundRenditions}, {"captioned", ls.captioned},
                                             {"cues", int(ls.cues.size())}, {"chapters", ls.chapters}};
        }
        return statusText();
    }
    if (action == QLatin1String("cue") || action == QLatin1String("chapter")) {
        QString why;
        const bool cue = action == QLatin1String("cue");
        const double duration = args.value("duration").toDouble(0);
        if (cue && !(duration >= 0 && duration <= 3600)) {
            isError = true;
            return tr("An ad break's duration is 0 (not known) to 3600 seconds");
        }
        if (!(cue ? liveCue(duration, &why) : liveChapter(args.value("title").toString(), &why))) {
            isError = true;
            return why;
        }
        return cue ? (duration > 0 ? tr("An ad break of %1 s from the next picture").arg(duration) : tr("An ad break from the next picture"))
                   : tr("A chapter from the next picture");
    }
    if (action == QLatin1String("nmos")) {
        QString why;
        if (!offerNmos(args.value("offer").toBool(true), &why)) {
            isError = true;
            return why;
        }
        if (node_ && node_->running()) {
            structured["node"] = node_->apiUrl();
            QJsonArray ids;
            for (int i = 0; i < 3; ++i) ids.append(node_->receiverId(i).toString(QUuid::WithoutBraces));
            structured["receivers"] = ids;
            structured["connected"] = node_->receiverConnection(0).active;
        }
        return node_ && node_->running() ? nmosStatus() : tr("Not offered to NMOS controllers");
    }
    isError = true;
    return tr("Unknown action %1").arg(action);
}

void CaptureDialog::refresh() {
    const bool marking = packager_ && recording_ && packager_->status().running;
    liveCue_->setEnabled(marking);
    liveChapter_->setEnabled(marking);
    if (!recorder_) {
        status_->setText(error_.isEmpty() ? tr("Not recording") : error_);
        return;
    }
    const CaptureStatus st = recorder_->status();
    // The file imported is being written by this program: said so as it records, not left to the system's notice of
    // writes (late on macOS and Windows), so it is followed as growing at once.
    if (st.recording && st.frames != notedFrames_ && !importedPath_.isEmpty()) {
        state_->noteMediaFileWritten(importedPath_);
        notedFrames_ = st.frames;
    }
    QString text;
    if (st.recording && st.frames == 0) {
        text = tr("Waiting for the feed…");
    } else if (st.width > 0) {
        text = (st.recording ? tr("Recording %1") : tr("Recorded %1")).arg(clock(st.seconds));
        text += QStringLiteral(" · ") + tr("%1×%2 at %3 fps").arg(st.width).arg(st.height).arg(st.fps, 0, 'g', 4);
        text += QStringLiteral(" · ") + (st.channels ? tr("%n sound channel(s)", nullptr, st.channels) : tr("no sound"));
        text += QStringLiteral(" · TC ") + QString::fromStdString(st.timecode);
        if (st.dropped || st.repeated) text += QStringLiteral(" · ") + tr("%1 dropped, %2 repeated").arg(st.dropped).arg(st.repeated);
        if (!st.recording) text += QStringLiteral("\n") + QDir::toNativeSeparators(path_);
    }
    if (packager_) {
        // The live package: its segments, and where it is.
        const LiveStatus ls = packager_->status();
        if (!recording_ && !ls.running) {
            // (finished: the thread that finished it ends, and so does the ticking)
            if (liveFinish_.joinable()) liveFinish_.join();
            tick_->stop();
        }
        QString line = ls.running ? tr("Live: %n segment(s)", nullptr, int(ls.segments)) : tr("Live package ended: %n segment(s)", nullptr, int(ls.segments));
        if (ls.soundRenditions > 1) line += QStringLiteral(" · ") + tr("%n sound stream(s)", nullptr, ls.soundRenditions);
        if (ls.captioned) line += QStringLiteral(" · ") + tr("captions");
        if (!ls.cues.empty()) line += QStringLiteral(" · ") + tr("%n ad cue(s)", nullptr, int(ls.cues.size()));
        if (ls.chapters) line += QStringLiteral(" · ") + tr("%n chapter(s)", nullptr, ls.chapters);
        if (ls.dropped) line += QStringLiteral(" · ") + tr("%1 pictures dropped (the encoders fell behind)").arg(ls.dropped);
        if (!liveUrl_.isEmpty()) {
            QStringList at;
            if (!ls.hls.empty()) at << liveUrl_ + QStringLiteral("master.m3u8");
            if (!ls.dash.empty()) at << liveUrl_ + QStringLiteral("manifest.mpd");
            if (!at.isEmpty()) line += QStringLiteral(" · ") + at.join(QStringLiteral(", "));
            if (liveNetwork_->isChecked()) line += QStringLiteral(" ") + tr("(and at this computer's address on the network)");
        } else if (!ls.hls.empty() || !ls.dash.empty()) {
            line += QStringLiteral(" · ") + QDir::toNativeSeparators(QString::fromStdString(ls.hls.empty() ? ls.dash : ls.hls));
        }
        if (!ls.error.empty()) line += QStringLiteral("\n") + QString::fromStdString(ls.error);
        text += (text.isEmpty() ? QString() : QStringLiteral("\n")) + line;
    }
    const QString note = !error_.isEmpty() ? error_ : QString::fromStdString(st.warning);
    if (!note.isEmpty()) text += (text.isEmpty() ? QString() : QStringLiteral("\n")) + note;
    status_->setText(text.isEmpty() ? tr("Not recording") : text);
}

}  // namespace montage
