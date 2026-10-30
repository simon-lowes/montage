#include "ReferenceOutputPanel.h"

#include "DisplayCalibration.h"
#include "render/TrimPasses.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
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
    st2110::setOutputOptions(loadSt2110());
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
    c.captions = s.value("output/captions", true).toBool();
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
    s.setValue("output/captions", c.captions);
}

st2110::SenderOptions ReferenceOutputController::loadSt2110() {
    QSettings s = appSettings();
    st2110::SenderOptions o;
    auto endpoint = [&](const char* key, st2110::Endpoint& e) {
        st2110::Endpoint read;
        if (st2110::parseEndpoint(s.value(key).toString().toStdString(), read)) e = read;
    };
    endpoint("output/st2110/video", o.video);
    endpoint("output/st2110/audio", o.audio);
    endpoint("output/st2110/data", o.anc);
    o.ttl = std::clamp(s.value("output/st2110/ttl", o.ttl).toInt(), 1, 255);
    const QString clock = s.value("output/st2110/clock").toString().trimmed();
    if (!clock.isEmpty()) o.refclk = clock.toStdString();
    o.sendAnc = s.value("output/st2110/sendData", true).toBool();
    return o;
}

void ReferenceOutputController::saveSt2110(const st2110::SenderOptions& o) {
    QSettings s = appSettings();
    s.setValue("output/st2110/video", qs(st2110::endpointText(o.video)));
    s.setValue("output/st2110/audio", qs(st2110::endpointText(o.audio)));
    s.setValue("output/st2110/data", qs(st2110::endpointText(o.anc)));
    s.setValue("output/st2110/ttl", o.ttl);
    s.setValue("output/st2110/clock", qs(o.refclk));
    s.setValue("output/st2110/sendData", o.sendAnc);
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

nmos::NodeOptions ReferenceOutputController::loadNmos(bool* enabled) {
    QSettings s = appSettings();
    nmos::NodeOptions o;
    if (enabled) *enabled = s.value("output/nmos/enabled", true).toBool();
    o.registry = s.value("output/nmos/registry").toString().trimmed();
    o.domain = s.value("output/nmos/domain").toString().trimmed();
    o.dnsServer = s.value("output/nmos/dnsServer").toString().trimmed();
    o.port = std::clamp(s.value("output/nmos/port", 0).toInt(), 0, 65535);
    return o;
}

void ReferenceOutputController::saveNmos(const nmos::NodeOptions& o, bool enabled) {
    QSettings s = appSettings();
    s.setValue("output/nmos/enabled", enabled);
    s.setValue("output/nmos/registry", o.registry);
    s.setValue("output/nmos/domain", o.domain);
    s.setValue("output/nmos/dnsServer", o.dnsServer);
    s.setValue("output/nmos/port", o.port);
}

bool ReferenceOutputController::start(const output::Config& config, QString* error) {
    stopOutput();
    config_ = config;
    saveConfig(config);
    if (!activating_ && !keepStreams_) {
        // (started here, every stream goes again from the device's interface: what a controller switched off or
        // chose was for that run)
        st2110::SenderOptions o = st2110::outputOptions();
        o.sendVideo = o.sendAudio = true;
        if (routed_) o.sendAnc = loadSt2110().sendAnc;
        o.sourcePort = 0;
        o.interfaceAddress.clear();
        st2110::setOutputOptions(o);
        routed_ = false;
        saveSt2110(o);  // (a controller's routing is not kept as the user's settings)
    }
    auto fail = [&](const QString& why) {
        lastError_ = why;
        if (error) *error = why;
        if (nmos_ && !activating_) nmos_->stop();  // (one a controller asked for stays offered, to be put right)
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
    updateNmos();
    emit runningChanged(true);
    return true;
}

void ReferenceOutputController::updateNmos() {
    bool enabled = true;
    const nmos::NodeOptions no = loadNmos(&enabled);
    st2110::VideoFormat video;
    st2110::AudioFormat audio;
    st2110::SenderOptions sending;
    if (!enabled || !backend_ || !st2110::openStreams(*backend_, video, audio, sending)) {
        if (nmos_) nmos_->stop();
        return;
    }
    if (!nmos_) {
        nmos_ = new nmos::Node(this);
        nmos_->onActivated = [this](const st2110::SenderOptions& to) {
            // A broadcast controller sent the streams elsewhere (or stopped some): opened again to match.
            st2110::SenderOptions o = st2110::outputOptions();
            o.video = to.video;
            o.audio = to.audio;
            o.anc = to.anc;
            o.sendVideo = to.sendVideo;
            o.sendAnc = to.sendAnc;
            o.sourcePort = to.sourcePort;
            o.interfaceAddress = to.interfaceAddress;  // (its source_ip; a device chosen in the panel still decides)
            if (config_.audioChannels > 0) o.sendAudio = to.sendAudio;  // (none embedded: not sent, whatever it asks)
            st2110::setOutputOptions(o);
            routed_ = true;
            activating_ = true;
            QString why;
            start(config_, &why);
            activating_ = false;
        };
    }
    // (the resources as the streams are, before registering them; an activation's own restart already says so)
    if (!activating_) nmos_->setStreams(video, audio, sending, true);
    nmos::NodeOptions o = no;
    if (!device_.id.empty()) o.address = qs(device_.id);  // (offered on the interface it sends from)
    // (settings changed while it runs: offered again with them)
    const auto key = [](const nmos::NodeOptions& x) { return QStringList{x.registry, x.domain, x.dnsServer, x.address, QString::number(x.port)}; };
    if (nmos_->running() && !activating_ && key(o) != key(nmosStarted_)) nmos_->stop();
    if (!nmos_->running()) {
        QString why;
        if (!nmos_->start(o, &why)) lastError_ = tr("NMOS: %1").arg(why);
        nmosStarted_ = o;
    }
}

void ReferenceOutputController::stop() {
    stopOutput();
    if (nmos_) nmos_->stop();
}

void ReferenceOutputController::stopOutput() {
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
    keepStreams_ = true;
    if (!start(config_, &why)) lastError_ = why;
    keepStreams_ = false;
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
    if (nmosNode())
        text += nmos_->registered() ? tr(", registered with %1").arg(nmos_->registry()) : tr(", offered over NMOS at %1").arg(nmos_->apiUrl());
    return text;
}

QByteArray ReferenceOutputController::toolJson() {
    return QByteArrayLiteral(R"json({"name":"montage_reference_output","title":"Reference monitor output",
"description":"The Program monitor sent to a broadcast monitor (10-bit 4:2:2, HDR signalled, timecode, embedded sound): SDI or HDMI through a Blackmagic DeckLink card; over the network as an NDI or Open Media Transport (OMT) source; or as SMPTE ST 2110 streams (uncompressed video, L24 sound, and ancillary data with time code and captions, over RTP timed from PTP, each described by an SDP file). It follows the playhead, playing or parked. \"list\": the interfaces, whether each can be used (and why not), their devices and modes. \"status\": what is going out (mode, frames sent, late frames, NDI or OMT receivers; ST 2110's SDP files). \"start\": opens it (settings left out are the last used: the first interface and device found, the sequence's own mode). \"stop\".",
"inputSchema":{"type":"object","properties":{
"action":{"type":"string","enum":["list","status","start","stop"]},
"interface":{"type":"string","description":"\"DeckLink\", \"NDI\", \"OMT\" or \"ST 2110\""},
"device":{"type":"string","description":"A device's id or name from \"list\" (on NDI and OMT, the source's name; on ST 2110, the address of the interface to send from, \"\" for the one routing chooses)"},
"mode":{"type":"string","description":"A mode from \"list\", such as \"1080p25\" or \"2160p23.98\"; \"\" for the sequence's"},
"depth":{"type":"number","enum":[8,10],"description":"Bits (10 for HDR)"},
"rec709":{"type":"boolean","description":"Send the picture as the viewer shows it (HDR tone mapped to Rec. 709) instead of the sequence's own colour"},
"hdr_metadata":{"type":"boolean","description":"Send HDR10 mastering display and light level metadata"},
"scaling":{"type":"string","enum":["fit","fill","none"],"description":"A picture not the mode's size: fitted, cropped to fill, or 1:1"},
"audio_channels":{"type":"number","enum":[0,2,8,16],"description":"Embedded sound channels (0: none)"},
"video_offset":{"type":"number","description":"Frames the picture runs ahead of the computer's speakers (-10 to 10), for a monitor's delay"},
"audio_offset_ms":{"type":"number","description":"The embedded sound's delay against its picture (-500 to 500 ms)"},
"captions":{"type":"boolean","description":"Send the sequence's captions as CEA-608/708 (ST 2110's ancillary data) while playing"},
"st2110":{"type":"object","description":"ST 2110's streams (settings left out are the last used)","properties":{
"video":{"type":"string","description":"Where the video goes: \"239.100.0.1:5004\" (multicast or unicast IPv4)"},
"audio":{"type":"string","description":"Where the sound goes (audio_channels 0: none)"},
"data":{"type":"string","description":"Where the ancillary data goes (time code and captions)"},
"send_data":{"type":"boolean"},
"ttl":{"type":"number","description":"Multicast hops (1 to 255)"},
"clock":{"type":"string","description":"The SDP's ts-refclk: \"ptp=IEEE1588-2008:traceable\" or the grandmaster's \"ptp=IEEE1588-2008:<id>:<domain>\""}}},
"nmos":{"type":"object","description":"ST 2110's streams offered to broadcast controllers as an AMWA NMOS Node (IS-04 and IS-05; settings left out are the last used)","properties":{
"enabled":{"type":"boolean"},
"registry":{"type":"string","description":"The registry's Registration API, \"http://host:port\" (empty: found by DNS-SD)"},
"domain":{"type":"string","description":"The DNS-SD domain to look in (empty: the system's)"},
"dns_server":{"type":"string","description":"The DNS server to ask (empty: the system's)"},
"port":{"type":"number","description":"The Node and Connection APIs' port (0: any free)"}}}},
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
            for (const auto& b : c.device.empty() ? std::vector<std::shared_ptr<output::Backend>>{} : output::backends()) {
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
            return tr("No interface \"%1\" (DeckLink, NDI, OMT or ST 2110)").arg(qs(c.backend));
        }
        if (has("captions")) c.captions = args.value("captions").toBool();
        if (has("st2110")) {
            const QJsonObject x = args.value("st2110").toObject();
            st2110::SenderOptions o = st2110::outputOptions();
            for (const auto& [key, endpoint] : {std::pair{"video", &o.video}, std::pair{"audio", &o.audio}, std::pair{"data", &o.anc}}) {
                if (!x.contains(QLatin1String(key))) continue;
                if (!st2110::parseEndpoint(x.value(QLatin1String(key)).toString().toStdString(), *endpoint)) {
                    isError = true;
                    return tr("st2110.%1 is an IPv4 address and port, such as 239.100.0.1:5004").arg(QLatin1String(key));
                }
            }
            if (x.contains("send_data")) {
                o.sendAnc = x.value("send_data").toBool();
                routed_ = false;  // (asked for here: kept, whatever a controller chose)
            }
            if (x.contains("ttl")) o.ttl = std::clamp(x.value("ttl").toInt(), 1, 255);
            if (x.contains("clock")) o.refclk = x.value("clock").toString().trimmed().toStdString();
            st2110::setOutputOptions(o);
        }
        if (has("nmos")) {
            const QJsonObject x = args.value("nmos").toObject();
            bool enabled = true;
            nmos::NodeOptions o = loadNmos(&enabled);
            if (x.contains("enabled")) enabled = x.value("enabled").toBool();
            if (x.contains("registry")) o.registry = x.value("registry").toString().trimmed();
            if (x.contains("domain")) o.domain = x.value("domain").toString().trimmed();
            if (x.contains("dns_server")) o.dnsServer = x.value("dns_server").toString().trimmed();
            if (x.contains("port")) o.port = std::clamp(x.value("port").toInt(), 0, 65535);
            saveNmos(o, enabled);
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
        QJsonObject sdp;
        for (const auto& [stream, text] : backend_->sdp()) sdp[qs(stream)] = qs(text);
        if (!sdp.isEmpty()) structured["sdp"] = sdp;
        if (nmosNode())
            structured["nmos"] = QJsonObject{{"node_api", nmos_->apiUrl()}, {"registry", nmos_->registry()}, {"registered", nmos_->registered()}};
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
                                "or over the network as an NDI® or Open Media Transport source or as SMPTE ST 2110 streams. 10-bit "
                                "4:2:2, HDR signalled, timecode and embedded sound."),
                             this);
    intro->setWordWrap(true);
    v->addWidget(intro);
    auto* form = new QFormLayout;
    form_ = form;
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
    captions_ = new QCheckBox(tr("Send the captions (CEA-608 and 708) while playing"), this);
    captions_->setObjectName(QStringLiteral("outputCaptions"));
    captions_->setChecked(c.captions);
    form->addRow(QString(), captions_);
    // ST 2110: where each stream goes, and the SDP's clock reference.
    const st2110::SenderOptions so = st2110::outputOptions();
    auto endpoint = [&](const char* name, const st2110::Endpoint& e, const QString& tip) {
        auto* edit = new QLineEdit(qs(st2110::endpointText(e)), this);
        edit->setObjectName(QString::fromLatin1(name));
        edit->setPlaceholderText(QStringLiteral("239.100.0.1:5004"));
        edit->setToolTip(tip);
        return edit;
    };
    st2110Video_ = endpoint("outputSt2110Video", so.video, tr("The video stream's address and port (multicast or unicast IPv4)"));
    form->addRow(tr("Video stream:"), st2110Video_);
    st2110Audio_ = endpoint("outputSt2110Audio", so.audio, tr("The sound stream's address and port"));
    form->addRow(tr("Sound stream:"), st2110Audio_);
    st2110Data_ = endpoint("outputSt2110Data", so.anc, tr("The ancillary data stream's address and port: time code, captions and HDR metadata"));
    form->addRow(tr("Data stream:"), st2110Data_);
    st2110Ttl_ = new QSpinBox(this);
    st2110Ttl_->setObjectName(QStringLiteral("outputSt2110Ttl"));
    st2110Ttl_->setRange(1, 255);
    st2110Ttl_->setValue(so.ttl);
    st2110Ttl_->setToolTip(tr("How many routers multicast packets may cross"));
    form->addRow(tr("Multicast TTL:"), st2110Ttl_);
    st2110Clock_ = new QLineEdit(qs(so.refclk), this);
    st2110Clock_->setObjectName(QStringLiteral("outputSt2110Clock"));
    st2110Clock_->setToolTip(tr("The PTP clock the SDP files name (ts-refclk): \"ptp=IEEE1588-2008:traceable\", or the grandmaster's "
                                 "\"ptp=IEEE1588-2008:<id>:<domain>\". Montage reads PTP time from the system clock, which linuxptp "
                                 "(ptp4l and phc2sys) or the operating system keeps in step."));
    form->addRow(tr("PTP reference:"), st2110Clock_);
    bool nmosOn = true;
    const nmos::NodeOptions no = ReferenceOutputController::loadNmos(&nmosOn);
    nmos_ = new QCheckBox(tr("Offer to broadcast controllers (NMOS)"), this);
    nmos_->setObjectName(QStringLiteral("outputNmos"));
    nmos_->setChecked(nmosOn);
    nmos_->setToolTip(tr("Registers the streams with the facility's NMOS registry (IS-04), so a broadcast controller lists them and "
                         "can send them elsewhere (IS-05)"));
    form->addRow(QString(), nmos_);
    nmosRegistry_ = new QLineEdit(no.registry, this);
    nmosRegistry_->setObjectName(QStringLiteral("outputNmosRegistry"));
    nmosRegistry_->setPlaceholderText(tr("Found by DNS-SD"));
    nmosRegistry_->setToolTip(tr("The registry's address, such as http://registry.example:8010 (empty: the one the network's DNS offers)"));
    form->addRow(tr("NMOS registry:"), nmosRegistry_);
    connect(nmos_, &QCheckBox::toggled, nmosRegistry_, &QWidget::setEnabled);
    nmosRegistry_->setEnabled(nmosOn);
    saveSdp_ = new QPushButton(tr("Save SDP Files…"), this);
    saveSdp_->setObjectName(QStringLiteral("outputSaveSdp"));
    saveSdp_->setToolTip(tr("Writes each stream's session description, which receivers and broadcast controllers read"));
    form->addRow(QString(), saveSdp_);
    connect(saveSdp_, &QPushButton::clicked, this, [this] {
        const auto b = controller_->backend();
        const auto sdps = b && controller_->running() ? b->sdp() : std::map<std::string, std::string>{};
        if (sdps.empty()) return;
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Save SDP Files"));
        if (dir.isEmpty()) return;
        for (const auto& [stream, text] : sdps) {
            QFile f(QDir(dir).filePath(QStringLiteral("montage-%1.sdp").arg(qs(stream))));
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(QByteArray::fromStdString(text));
        }
    });
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
            st2110::setOutputOptions(chosenSt2110());
            nmos::NodeOptions no = ReferenceOutputController::loadNmos();
            no.registry = nmosRegistry_->text().trimmed();
            ReferenceOutputController::saveNmos(no, nmos_->isChecked());
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
    const bool ip = b && b->name() == "ST 2110";
    for (QWidget* w : std::initializer_list<QWidget*>{st2110Video_, st2110Audio_, st2110Data_, st2110Ttl_, st2110Clock_, nmos_, nmosRegistry_, saveSdp_})
        form_->setRowVisible(w, ip);
    captions_->setEnabled(std::any_of(devices_.begin(), devices_.end(), [](const output::Device& d) { return d.captions; }));
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
    c.captions = captions_->isChecked();
    return c;
}

st2110::SenderOptions ReferenceOutputPanel::chosenSt2110() const {
    // (an address that does not read is kept as typed, so starting says what is wrong with it)
    st2110::SenderOptions o = st2110::outputOptions();
    for (const auto& [edit, endpoint] : {std::pair{st2110Video_, &o.video}, std::pair{st2110Audio_, &o.audio}, std::pair{st2110Data_, &o.anc}})
        if (!st2110::parseEndpoint(edit->text().trimmed().toStdString(), *endpoint)) *endpoint = {edit->text().trimmed().toStdString(), 0};
    o.ttl = st2110Ttl_->value();
    const QString clock = st2110Clock_->text().trimmed();
    o.refclk = clock.isEmpty() ? st2110::SenderOptions{}.refclk : clock.toStdString();
    return o;
}

void ReferenceOutputPanel::refresh() {
    const bool on = controller_->running();
    start_->setText(on ? tr("Stop") : tr("Start"));
    start_->setEnabled(on || !devices_.empty());
    saveSdp_->setEnabled(on && controller_->backend() && !controller_->backend()->sdp().empty());
    status_->setText(controller_->status());
}

}  // namespace montage
