#include "ReferenceOutputPanel.h"

#include "DisplayCalibration.h"
#include "render/TrimPasses.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QJsonArray>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

#include "PlaybackController.h"
#include "Settings.h"
#include "render/ColorSpace.h"

namespace montage {

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

}  // namespace

ReferenceOutputController::ReferenceOutputController(PlaybackController* program, QObject* parent)
    : QObject(parent), program_(program), config_(loadConfig()) {
    watch_ = new QTimer(this);
    watch_->setInterval(500);
    connect(watch_, &QTimer::timeout, this, [this] {
        if (!scheduler_ || scheduler_->running()) return;
        // Stopped by itself: said so, and let go.
        const std::string why = scheduler_->stats().error;
        stop();
        lastError_ = why.empty() ? tr("The output stopped") : qs(why);
    });
    // A calibration chosen or changed while it runs applies straight away.
    connect(&DisplayCalibration::instance(), &DisplayCalibration::changed, this, [this] {
        if (!scheduler_) return;
        const auto lut = DisplayCalibration::instance().lut(calibrationTarget());
        if (lut == scheduler_->calibration()) return;
        scheduler_->setCalibration(lut);
        if (program_) program_->requestFrame();
    });
}

QString ReferenceOutputController::calibrationTarget() const {
    return backend_ ? DisplayCalibration::outputTarget(qs(backend_->name()), qs(device_.id)) : QString();
}

ReferenceOutputController::~ReferenceOutputController() { stop(); }

output::Config ReferenceOutputController::loadConfig() {
    QSettings s = appSettings();
    output::Config c;
    c.backend = s.value("output/backend").toString().toStdString();
    c.device = s.value("output/device").toString().toStdString();
    c.mode = s.value("output/mode").toString().toStdString();
    c.depth = s.value("output/depth", 10).toInt() == 8 ? 8 : 10;
    c.rec709 = s.value("output/rec709", false).toBool();
    c.hdrMetadata = s.value("output/hdrMetadata", true).toBool();
    c.scaling = output::Config::Scaling(std::clamp(s.value("output/scaling", 0).toInt(), 0, 2));
    c.audioChannels = s.value("output/audioChannels", 2).toInt();
    c.videoOffset = std::clamp(s.value("output/videoOffset", 0).toInt(), -10, 10);
    c.audioOffsetMs = std::clamp(s.value("output/audioOffsetMs", 0).toInt(), -500, 500);
    return c;
}

void ReferenceOutputController::saveConfig(const output::Config& c) {
    QSettings s = appSettings();
    s.setValue("output/backend", qs(c.backend));
    s.setValue("output/device", qs(c.device));
    s.setValue("output/mode", qs(c.mode));
    s.setValue("output/depth", c.depth);
    s.setValue("output/rec709", c.rec709);
    s.setValue("output/hdrMetadata", c.hdrMetadata);
    s.setValue("output/scaling", int(c.scaling));
    s.setValue("output/audioChannels", c.audioChannels);
    s.setValue("output/videoOffset", c.videoOffset);
    s.setValue("output/audioOffsetMs", c.audioOffsetMs);
}

QString ReferenceOutputController::formatKey() const {
    const Sequence* s = program_ ? program_->sequence() : nullptr;
    if (!s) return {};
    return QStringLiteral("%1x%2@%3/%4 %5 %6 %7 %8")
        .arg(s->width)
        .arg(s->height)
        .arg(s->fps.num)
        .arg(s->fps.den)
        .arg(qs(sequenceColorSpace(*s).id))
        .arg(s->sampleRate)
        .arg(s->hdrPeakNits)
        .arg(s->hdrMaxCll) + QStringLiteral(" ") + qs(program_->trimTarget());
}

Sequence ReferenceOutputController::signalSequence(const Sequence& s) const {
    // A trim pass goes out as its target's signal (its space and peak; light levels unmeasured).
    Sequence out = s;
    const std::string trim = program_ ? program_->trimTarget() : std::string();
    if (findTrimTarget(s, trim)) {
        out.colorSpace = trimTargetSpace(s, trim).id;
        out.hdrPeakNits = trimTargetPeak(s, trim);
        out.hdrMaxCll = out.hdrMaxFall = 0;
    }
    return out;
}

bool ReferenceOutputController::start(const output::Config& config, QString* error) {
    stop();
    config_ = config;
    saveConfig(config);
    auto fail = [&](const QString& why) {
        lastError_ = why;
        if (error) *error = why;
        emit runningChanged(false);
        return false;
    };
    const Sequence* s = program_ ? program_->sequence() : nullptr;
    if (!s) return fail(tr("Open a sequence first"));
    // The interface, else the first one there; its device, else its first.
    std::shared_ptr<output::Backend> b = config.backend.empty() ? nullptr : output::findBackend(config.backend);
    if (!b)
        for (const auto& x : output::backends())
            if (x->available() && !x->devices().empty()) {
                b = x;
                break;
            }
    std::string why;
    if (!b) return fail(tr("No video output device: connect a DeckLink card (Blackmagic Desktop Video) or install the NDI Runtime"));
    if (!b->available(&why)) return fail(qs(why));
    const std::vector<output::Device> devices = b->devices();
    const output::Device* d = nullptr;
    for (const auto& x : devices)
        if (x.id == config.device) d = &x;
    // (one named and not there is not swapped for another)
    if (!d && !config.device.empty()) return fail(tr("No %1 device \"%2\"").arg(qs(b->name()), qs(config.device)));
    if (!d && !devices.empty()) d = &devices.front();
    if (!d) return fail(tr("No %1 device found").arg(qs(b->name())));
    // The mode: chosen, else the sequence's (any size and rate on NDI).
    output::Mode mode;
    if (d->modes.empty()) {
        mode = {output::modeName(s->width, s->height, s->fps, false), s->width, s->height, s->fps, false, 0};
    } else if (!config.mode.empty()) {
        const output::Mode* m = output::findMode(d->modes, config.mode);
        if (!m) return fail(tr("%1 has no %2 mode").arg(qs(d->name), qs(config.mode)));
        mode = *m;
    } else {
        const output::Mode* m = output::matchMode(d->modes, s->width, s->height, s->fps);
        if (!m)
            return fail(tr("%1 has no mode at %2 frames a second: choose one").arg(qs(d->name)).arg(s->fpsValue(), 0, 'g', 5));
        mode = *m;
    }
    auto sched = std::make_shared<output::Scheduler>(b);
    // The monitor's calibration (app/DisplayCalibration.h), last.
    sched->setCalibration(DisplayCalibration::instance().lut(DisplayCalibration::outputTarget(qs(b->name()), qs(d->id))));
    if (!sched->start(*d, mode, signalSequence(*s), config, &why)) return fail(qs(why));
    backend_ = b;
    device_ = *d;
    scheduler_ = sched;
    startedFor_ = formatKey();
    lastError_.clear();
    program_->setReferenceOutput(scheduler_);
    watch_->start();
    emit runningChanged(true);
    return true;
}

void ReferenceOutputController::stop() {
    if (!scheduler_) return;
    watch_->stop();
    if (program_) program_->setReferenceOutput(nullptr);
    scheduler_->stop();
    scheduler_.reset();
    emit runningChanged(false);
}

bool ReferenceOutputController::running() const { return scheduler_ && scheduler_->running(); }

void ReferenceOutputController::sequenceChanged() {
    if (!scheduler_ || formatKey() == startedFor_) return;
    QString why;
    if (!start(config_, &why)) lastError_ = why;
}

QString ReferenceOutputController::status() const {
    if (!scheduler_) return lastError_.isEmpty() ? tr("Off") : lastError_;
    const output::Scheduler::Stats st = scheduler_->stats();
    if (!scheduler_->running()) return st.error.empty() ? tr("Stopped") : qs(st.error);
    const output::Signal& sig = scheduler_->signal();
    const QString depth = sig.format == output::PixelFormat::Uyvy8 ? tr("8-bit") : sig.format == output::PixelFormat::V210 ? tr("10-bit") : tr("16-bit");
    const QString colour = sig.eotf == output::Eotf::Pq ? QStringLiteral("PQ") : sig.eotf == output::Eotf::Hlg ? QStringLiteral("HLG")
                         : sig.bt2020 ? QStringLiteral("BT.2020") : QStringLiteral("BT.709");
    QString text = tr("%1, %2 %3, on %4: %5 frames, %6 late")
                       .arg(qs(scheduler_->mode().name), depth, colour, qs(device_.name))
                       .arg(st.sent)
                       .arg(st.late);
    if (st.receivers >= 0) text += tr(", %n receiver(s)", "", st.receivers);
    if (backend_ && !backend_->address().empty()) text += QStringLiteral(" (%1)").arg(qs(backend_->address()));
    if (scheduler_->calibration()) text += tr(", calibrated");
    return text;
}

QByteArray ReferenceOutputController::toolJson() {
    return QByteArrayLiteral(R"json({"name":"montage_reference_output","title":"Reference monitor output",
"description":"The Program monitor sent to a broadcast monitor: SDI or HDMI through a Blackmagic DeckLink card, or over the network as an NDI or Open Media Transport (OMT) source (10-bit 4:2:2, HDR signalled, timecode, embedded sound); it follows the playhead, playing or parked. \"list\": the interfaces, whether each can be used (and why not), their devices and modes. \"status\": what is going out (mode, frames sent, late frames, NDI or OMT receivers). \"start\": opens it (settings left out are the last used: the first interface and device found, the sequence's own mode). \"stop\".",
"inputSchema":{"type":"object","properties":{
"action":{"type":"string","enum":["list","status","start","stop"]},
"interface":{"type":"string","description":"\"DeckLink\", \"NDI\" or \"OMT\""},
"device":{"type":"string","description":"A device's id or name from \"list\" (on NDI and OMT, the source's name)"},
"mode":{"type":"string","description":"A mode from \"list\", such as \"1080p25\" or \"2160p23.98\"; \"\" for the sequence's"},
"depth":{"type":"number","enum":[8,10],"description":"Bits (10 for HDR)"},
"rec709":{"type":"boolean","description":"Send the picture as the viewer shows it (HDR tone mapped to Rec. 709) instead of the sequence's own colour"},
"hdr_metadata":{"type":"boolean","description":"Send HDR10 mastering display and light level metadata"},
"scaling":{"type":"string","enum":["fit","fill","none"],"description":"A picture not the mode's size: fitted, cropped to fill, or 1:1"},
"audio_channels":{"type":"number","enum":[0,2,8,16],"description":"Embedded sound channels (0: none)"},
"video_offset":{"type":"number","description":"Frames the picture runs ahead of the computer's speakers (-10 to 10), for a monitor's delay"},
"audio_offset_ms":{"type":"number","description":"The embedded sound's delay against its picture (-500 to 500 ms)"}},
"required":["action"]},
"annotations":{"readOnlyHint":false,"destructiveHint":false,"openWorldHint":true}})json");
}

QString ReferenceOutputController::runTool(const QJsonObject& args, QJsonObject& structured, bool& isError) {
    const QString action = args.value("action").toString();
    if (action == "list") {
        QJsonArray list;
        QStringList lines;
        for (const auto& b : output::backends()) {
            std::string why;
            const bool ok = b->available(&why);
            QJsonObject o{{"interface", qs(b->name())}, {"available", ok}};
            if (!ok) o["why"] = qs(why);
            lines << (ok ? qs(b->name()) + ":" : tr("%1: not available (%2)").arg(qs(b->name()), qs(why)));
            QJsonArray devs;
            if (ok)
                for (const auto& d : b->devices()) {
                    QJsonArray modes;
                    for (const auto& m : d.modes) modes.append(qs(m.name));
                    devs.append(QJsonObject{{"id", qs(d.id)}, {"name", qs(d.name)}, {"modes", modes}, {"hdr", d.hdr}});
                    lines << tr("  %1 (%2): %3").arg(qs(d.name), qs(d.id), d.modes.empty() ? tr("any size and rate") : tr("%n mode(s)", "", int(d.modes.size())));
                }
            o["devices"] = devs;
            list.append(o);
        }
        structured["interfaces"] = list;
        return lines.join('\n');
    }
    if (action == "stop") {
        stop();
        lastError_.clear();
        return tr("Reference output off");
    }
    if (action == "start") {
        output::Config c = config_;
        auto has = [&](const char* k) { return args.contains(QLatin1String(k)); };
        if (has("interface")) c.backend = args.value("interface").toString().toStdString();
        if (has("device")) {
            // By id or by name, on the interface named or any.
            c.device = args.value("device").toString().toStdString();
            for (const auto& b : output::backends()) {
                if ((!c.backend.empty() && b->name() != c.backend) || !b->available()) continue;
                for (const auto& d : b->devices())
                    if (d.id == c.device || d.name == c.device) {
                        c.device = d.id;
                        c.backend = b->name();
                    }
            }
        } else if (const auto b = output::findBackend(c.backend)) {
            // The last used, if it is still there (else the first).
            bool there = false;
            for (const auto& d : b->available() ? b->devices() : std::vector<output::Device>{}) there = there || d.id == c.device;
            if (!there) c.device.clear();
        } else {
            c.device.clear();
        }
        if (has("mode")) c.mode = args.value("mode").toString().toStdString();
        if (has("depth")) c.depth = args.value("depth").toInt() == 8 ? 8 : 10;
        if (has("rec709")) c.rec709 = args.value("rec709").toBool();
        if (has("hdr_metadata")) c.hdrMetadata = args.value("hdr_metadata").toBool();
        if (has("scaling")) {
            const QString sc = args.value("scaling").toString();
            c.scaling = sc == "fill" ? output::Config::Scaling::Fill : sc == "none" ? output::Config::Scaling::None : output::Config::Scaling::Fit;
        }
        if (has("audio_channels")) {
            const int n = args.value("audio_channels").toInt();
            if (n != 0 && n != 2 && n != 8 && n != 16) {
                isError = true;
                return tr("audio_channels is 0, 2, 8 or 16");
            }
            c.audioChannels = n;
        }
        if (has("video_offset")) c.videoOffset = std::clamp(args.value("video_offset").toInt(), -10, 10);
        if (has("audio_offset_ms")) c.audioOffsetMs = std::clamp(args.value("audio_offset_ms").toInt(), -500, 500);
        if (!c.backend.empty() && !output::findBackend(c.backend)) {
            isError = true;
            return tr("No interface \"%1\" (DeckLink, NDI or OMT)").arg(qs(c.backend));
        }
        QString why;
        if (!start(c, &why)) {
            isError = true;
            return why;
        }
    } else if (action != "status") {
        isError = true;
        return tr("action is list, status, start or stop");
    }
    structured["running"] = running();
    if (scheduler_) {
        const output::Scheduler::Stats st = scheduler_->stats();
        structured["interface"] = qs(backend_->name());
        structured["device"] = qs(device_.name);
        structured["mode"] = qs(scheduler_->mode().name);
        structured["sent"] = double(st.sent);
        structured["late"] = double(st.late);
        if (st.receivers >= 0) structured["receivers"] = st.receivers;
        if (!backend_->address().empty()) structured["address"] = qs(backend_->address());
    }
    return status();
}

// ---------------------------------------------------------------------------

ReferenceOutputPanel::ReferenceOutputPanel(ReferenceOutputController* controller, QWidget* parent)
    : QDialog(parent), controller_(controller) {
    setObjectName(QStringLiteral("referenceOutputDialog"));
    setWindowTitle(tr("Reference Output"));
    setAttribute(Qt::WA_DeleteOnClose);
    auto* v = new QVBoxLayout(this);
    auto* intro = new QLabel(tr("The Program sent to a broadcast monitor: SDI or HDMI through a Blackmagic DeckLink or UltraStudio, "
                                "or over the network as an NDI® or Open Media Transport source. 10-bit 4:2:2, HDR signalled, timecode and "
                                "embedded sound."),
                             this);
    intro->setWordWrap(true);
    v->addWidget(intro);
    auto* form = new QFormLayout;
    v->addLayout(form);
    const output::Config c = controller->config();
    auto combo = [&](const char* name) {
        auto* b = new QComboBox(this);
        b->setObjectName(QString::fromLatin1(name));
        return b;
    };
    backend_ = combo("outputBackend");
    for (const auto& b : output::backends()) backend_->addItem(qs(b->name()), qs(b->name()));
    backend_->setCurrentIndex(std::max(0, backend_->findData(qs(c.backend))));
    form->addRow(tr("Interface:"), backend_);
    why_ = new QLabel(this);
    why_->setObjectName(QStringLiteral("outputWhy"));
    why_->setWordWrap(true);
    form->addRow(why_);
    device_ = combo("outputDevice");
    form->addRow(tr("Device:"), device_);
    mode_ = combo("outputMode");
    form->addRow(tr("Mode:"), mode_);
    depth_ = combo("outputDepth");
    depth_->addItem(tr("10-bit 4:2:2"), 10);
    depth_->addItem(tr("8-bit 4:2:2"), 8);
    depth_->setCurrentIndex(c.depth == 8 ? 1 : 0);
    form->addRow(tr("Bit depth:"), depth_);
    colour_ = combo("outputColour");
    colour_->addItem(tr("The sequence's (HDR as HDR)"), false);
    colour_->addItem(tr("Rec. 709, as the viewer shows it"), true);
    colour_->setCurrentIndex(c.rec709 ? 1 : 0);
    form->addRow(tr("Colour:"), colour_);
    hdr_ = new QCheckBox(tr("Send HDR10 metadata (mastering display and light levels)"), this);
    hdr_->setObjectName(QStringLiteral("outputHdrMetadata"));
    hdr_->setChecked(c.hdrMetadata);
    form->addRow(QString(), hdr_);
    scaling_ = combo("outputScaling");
    scaling_->addItem(tr("Fit (letterbox or pillarbox)"));
    scaling_->addItem(tr("Fill (crop)"));
    scaling_->addItem(tr("1:1 (centred)"));
    scaling_->setCurrentIndex(int(c.scaling));
    form->addRow(tr("Framing:"), scaling_);
    audio_ = combo("outputAudio");
    audio_->addItem(tr("None"), 0);
    audio_->addItem(tr("2 channels"), 2);
    audio_->addItem(tr("8 channels"), 8);
    audio_->addItem(tr("16 channels"), 16);
    audio_->setCurrentIndex(std::max(0, audio_->findData(c.audioChannels)));
    form->addRow(tr("Embedded sound:"), audio_);
    videoOffset_ = new QSpinBox(this);
    videoOffset_->setObjectName(QStringLiteral("outputVideoOffset"));
    videoOffset_->setRange(-10, 10);
    videoOffset_->setSuffix(tr(" frames"));
    videoOffset_->setValue(c.videoOffset);
    videoOffset_->setToolTip(tr("Sends the picture this many frames ahead of the computer's speakers, for a monitor's processing delay"));
    form->addRow(tr("Picture offset:"), videoOffset_);
    audioOffset_ = new QSpinBox(this);
    audioOffset_->setObjectName(QStringLiteral("outputAudioOffset"));
    audioOffset_->setRange(-500, 500);
    audioOffset_->setSuffix(tr(" ms"));
    audioOffset_->setValue(c.audioOffsetMs);
    audioOffset_->setToolTip(tr("Delays the embedded sound against its picture (negative: earlier)"));
    form->addRow(tr("Sound offset:"), audioOffset_);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("outputStatus"));
    status_->setWordWrap(true);
    v->addWidget(status_);
    auto* credit = new QLabel(tr("NDI® is a registered trademark of Vizrt NDI AB (<a href=\"https://ndi.video\">ndi.video</a>)."), this);
    credit->setOpenExternalLinks(true);
    credit->setEnabled(false);
    v->addWidget(credit);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    start_ = buttons->addButton(tr("Start"), QDialogButtonBox::ActionRole);
    start_->setObjectName(QStringLiteral("outputStart"));
    v->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(start_, &QPushButton::clicked, this, [this] {
        if (controller_->running()) {
            controller_->stop();
        } else {
            QString why;
            controller_->start(chosen(), &why);
        }
        refresh();
    });
    connect(backend_, &QComboBox::currentIndexChanged, this, &ReferenceOutputPanel::fillDevices);
    connect(device_, &QComboBox::currentIndexChanged, this, &ReferenceOutputPanel::fillModes);
    fillDevices();
    timer_ = new QTimer(this);
    timer_->setInterval(500);
    connect(timer_, &QTimer::timeout, this, &ReferenceOutputPanel::refresh);
    timer_->start();
    refresh();
}

void ReferenceOutputPanel::fillDevices() {
    const output::Config c = controller_->config();
    device_->blockSignals(true);
    device_->clear();
    devices_.clear();
    const auto b = output::findBackend(backend_->currentData().toString().toStdString());
    std::string why;
    if (b && b->available(&why)) devices_ = b->devices();
    for (const auto& d : devices_) device_->addItem(qs(d.name), qs(d.id));
    device_->setCurrentIndex(std::max(0, device_->findData(qs(c.device))));
    device_->blockSignals(false);
    why_->setText(!b ? QString() : !why.empty() ? qs(why) : devices_.empty() ? tr("No %1 device found").arg(qs(b->name())) : QString());
    why_->setVisible(!why_->text().isEmpty());
    const bool network = b && (b->name() == "NDI" || b->name() == "OMT");
    hdr_->setEnabled(!network);  // (NDI and OMT carry the colour, not HDR10's mastering metadata)
    device_->setToolTip(network ? tr("The %1 source's name (other computers see \"<this computer> (Montage)\")").arg(qs(b->name())) : QString());
    fillModes();    refresh();  // (Start, as there is a device now or not)
}

void ReferenceOutputPanel::fillModes() {
    const output::Config c = controller_->config();
    mode_->clear();
    mode_->addItem(tr("The sequence's"), QString());
    const int i = device_->currentIndex();
    if (i >= 0 && i < int(devices_.size()))
        for (const auto& m : devices_[size_t(i)].modes) mode_->addItem(qs(m.name), qs(m.name));
    mode_->setCurrentIndex(std::max(0, mode_->findData(qs(c.mode))));
    mode_->setEnabled(mode_->count() > 1);
}

output::Config ReferenceOutputPanel::chosen() const {
    output::Config c;
    c.backend = backend_->currentData().toString().toStdString();
    c.device = device_->currentData().toString().toStdString();
    c.mode = mode_->currentData().toString().toStdString();
    c.depth = depth_->currentData().toInt();
    c.rec709 = colour_->currentData().toBool();
    c.hdrMetadata = hdr_->isChecked();
    c.scaling = output::Config::Scaling(scaling_->currentIndex());
    c.audioChannels = audio_->currentData().toInt();
    c.videoOffset = videoOffset_->value();
    c.audioOffsetMs = audioOffset_->value();
    return c;
}

void ReferenceOutputPanel::refresh() {
    const bool on = controller_->running();
    start_->setText(on ? tr("Stop") : tr("Start"));
    start_->setEnabled(on || !devices_.empty());
    status_->setText(controller_->status());
}

}  // namespace montage
