#include "RemoteGradePanel.h"

#include <QBuffer>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QNetworkInterface>
#include <QPushButton>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

#include "EditorState.h"
#include "PlaybackController.h"
#include "Settings.h"

namespace montage {

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

constexpr qint64 kPointerShownMs = 4000;

}  // namespace

RemoteGradeController::RemoteGradeController(EditorState* state, PlaybackController* program, RemoteGradeHooks hooks, QObject* parent)
    : QObject(parent), state_(state), program_(program), hooks_(std::move(hooks)) {
    timer_ = new QTimer(this);
    timer_->setInterval(20);
    connect(timer_, &QTimer::timeout, this, &RemoteGradeController::tick);
    connect(state_, &EditorState::projectChanged, this, [this] { projectDirty_ = true; });
}

RemoteGradeController::~RemoteGradeController() {
    blockSignals(true);
    hooks_ = {};  // (the window may be going: nothing to hand notes to)
    stop();
}

remote::GradeSettings RemoteGradeController::loadSettings(bool hosting) {
    QSettings s = appSettings();
    remote::GradeSettings g;
    g.address = s.value(hosting ? "grade/hostAddress" : "grade/joinAddress", hosting ? qs(g.address) : QStringLiteral("srt://127.0.0.1:9100"))
                    .toString()
                    .toStdString();
    g.passphrase = s.value(hosting ? "grade/hostPassphrase" : "grade/joinPassphrase").toString().toStdString();
    g.name = s.value("grade/name").toString().toStdString();
    return g;
}

QString RemoteGradeController::loadStream() { return appSettings().value("grade/stream").toString(); }

void RemoteGradeController::saveSettings(const remote::GradeSettings& g, bool hosting, const QString& stream) {
    QSettings s = appSettings();
    s.setValue(hosting ? "grade/hostAddress" : "grade/joinAddress", qs(g.address));
    s.setValue(hosting ? "grade/hostPassphrase" : "grade/joinPassphrase", qs(g.passphrase));
    if (!hosting) {
        s.setValue("grade/name", qs(g.name));
        s.setValue("grade/stream", stream);
    }
}

bool RemoteGradeController::host(const remote::GradeSettings& settings, QString* error) {
    stop();
    std::string why;
    if (!host_.start(settings, &why)) {
        if (error) *error = qs(why);
        return false;
    }
    settings_ = settings;
    saveSettings(settings, true);
    mode_ = Mode::Hosting;
    projectDirty_ = true;
    compareKey_ = -1;
    compareBytes_.clear();
    pointers_.clear();
    timer_->start();
    tick();
    emit modeChanged();
    return true;
}

bool RemoteGradeController::join(const remote::GradeSettings& settings, const QString& stream, QString* error) {
    if (state_->isModified()) {
        if (error) *error = tr("Save this project first (or start a new one): following replaces it with the colourist's");
        return false;
    }
    stop();
    std::string why;
    if (!client_.start(settings, &why)) {
        if (error) *error = qs(why);
        return false;
    }
    if (!stream.isEmpty()) {
        receiver_ = std::make_unique<output::ReviewReceiver>();
        if (!receiver_->start(stream.toStdString(), settings.passphrase, settings.latencyMs, false, &why)) {
            client_.stop();
            receiver_.reset();
            if (error) *error = tr("The stream for missing media: %1").arg(qs(why));
            return false;
        }
    }
    settings_ = settings;
    stream_ = stream;
    saveSettings(settings, false, stream);
    local_ = state_->project();
    revision_ = 0;
    followError_.clear();
    viewerShown_.clear();
    compareShown_.clear();
    drift_ = 0;
    showingStream_ = false;
    streamPicture_ = QImage();
    streamSerial_ = 0;
    mode_ = Mode::Following;
    timer_->start();
    emit modeChanged();
    return true;
}

void RemoteGradeController::stop() {
    if (mode_ == Mode::Off) return;
    timer_->stop();
    if (mode_ == Mode::Hosting) {
        host_.stop();
        // (notes that came in the last moments are still handed on)
        const std::vector<output::RemoteMarker> last = host_.takeNotes();
        const Sequence* s = program_ ? program_->sequence() : nullptr;
        if (!last.empty() && hooks_.notes) hooks_.notes(s ? s->id : 0, last);
        pointers_.clear();
    } else {
        client_.stop();
        receiver_.reset();
        showingStream_ = false;
        streamPicture_ = QImage();
        if (program_ && program_->isPlaying()) program_->pause();
    }
    mode_ = Mode::Off;
    emit pointersChanged();
    emit modeChanged();
}

void RemoteGradeController::tick() {
    if (mode_ == Mode::Hosting) hostTick();
    else if (mode_ == Mode::Following) followTick();
}

void RemoteGradeController::hostTick() {
    if (projectDirty_) {
        host_.publish(state_->project());
        projectDirty_ = false;
    }
    remote::ViewState v;
    const Sequence* s = program_ ? program_->sequence() : nullptr;
    v.sequence = s ? s->id : state_->project().activeSequence;
    if (program_) {
        v.playhead = program_->position();
        v.playing = program_->isPlaying();
        v.speed = v.playing ? program_->speed() : 1;
    }
    if (hooks_.viewer) v.viewer = QJsonDocument(hooks_.viewer()).toJson(QJsonDocument::Compact).toStdString();
    const QImage compare = hooks_.compare ? hooks_.compare() : QImage();
    if (compare.isNull()) {
        compareKey_ = -1;
        compareBytes_.clear();
    } else if (compare.cacheKey() != compareKey_) {
        // (a JPEG of it, at most 1920 wide: sent once, when it changes)
        const QImage small = compare.width() > 1920 ? compare.scaledToWidth(1920, Qt::SmoothTransformation) : compare;
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        small.convertToFormat(QImage::Format_RGB32).save(&buffer, "JPG", 90);
        compareBytes_ = bytes.toStdString();
        compareKey_ = compare.cacheKey();
    }
    v.compare = compareBytes_;
    host_.setView(v);
    const std::vector<output::RemoteMarker> notes = host_.takeNotes();
    if (!notes.empty() && hooks_.notes) hooks_.notes(v.sequence, notes);
    bool changed = false;
    for (const remote::Pointer& p : host_.takePointers()) {
        Seen seen{p, {}};
        seen.since.start();
        pointers_.push_back(std::move(seen));
        changed = true;
    }
    const size_t before = pointers_.size();
    pointers_.erase(std::remove_if(pointers_.begin(), pointers_.end(), [](const Seen& p) { return p.since.elapsed() > kPointerShownMs; }), pointers_.end());
    if (changed || before != pointers_.size() || !pointers_.empty()) emit pointersChanged();
}

void RemoteGradeController::followTick() {
    Project got;
    uint64_t rev = 0;
    std::string why;
    if (client_.takeProject(got, &rev, &why)) {
        remote::keepLocalMedia(local_, got);
        state_->followProject(std::move(got));
        projectDirty_ = false;
        revision_ = rev;
        followError_.clear();
    } else if (!why.empty()) {
        followError_ = tr("The colourist's project cannot be read here: %1").arg(qs(why));
    }
    remote::ViewState v;
    int64_t age = 0;
    // (a view from a colourist who has gone, or one playing that has not been heard from for a while, is not followed)
    const bool connected = client_.status().connected;
    const bool haveView = connected && client_.view(v, &age) && !(v.playing && age > 1500);
    if (!haveView && program_ && program_->isPlaying()) program_->pause();
    if (haveView && v.sequence && state_->project().activeSequence != v.sequence) state_->setActiveSequence(v.sequence);
    const Sequence* s = state_->project().active();
    if (haveView && s && program_) {
        const FrameTime end = std::max<FrameTime>(1, s->duration());
        const FrameTime target = std::clamp<FrameTime>(remote::followPlayhead(v, s->fps, age), 0, end - 1);
        if (v.playing) {
            if (!program_->isPlaying() || program_->speed() != v.speed) {
                program_->seek(target);
                program_->setSpeed(v.speed);
                drift_ = 0;
            } else if (double(std::abs(program_->position() - target)) > 1 + std::abs(v.speed) * s->fps.toDouble() * 0.04) {
                // (playing on its own clock: put back only when it keeps drifting)
                if (++drift_ > 10) {
                    program_->seek(target);
                    drift_ = 0;
                }
            } else {
                drift_ = 0;
            }
        } else {
            if (program_->isPlaying()) program_->pause();
            if (program_->position() != target) program_->seek(target);
        }
    }
    if (haveView && (v.viewer != viewerShown_ || v.compare != compareShown_) && hooks_.applyViewer) {
        QImage compare;
        if (!v.compare.empty()) compare.loadFromData(QByteArray::fromStdString(v.compare));
        hooks_.applyViewer(viewerJson(qs(v.viewer)), compare);
        viewerShown_ = v.viewer;
        compareShown_ = v.compare;
    }
    // Where this Montage lacks the media, the colourist's stream.
    const bool was = showingStream_;
    showingStream_ = receiver_ && s && program_ &&
                     remote::needsStream(state_->project(), *s, program_->position(), [this](Id m) { return state_->isMediaOffline(m); });
    bool fresh = false;
    if (receiver_) {
        output::ReviewReceiver::Picture pic;
        if (receiver_->latest(pic, streamSerial_) && pic.width > 0) {
            streamPicture_ = QImage(pic.rgba.data(), pic.width, pic.height, QImage::Format_RGBA8888).copy();
            streamSerial_ = pic.serial;
            fresh = true;
        }
    }
    if (was != showingStream_ || (showingStream_ && fresh)) emit pointersChanged();
}

QJsonObject RemoteGradeController::viewerJson(const QString& text) { return QJsonDocument::fromJson(text.toUtf8()).object(); }

void RemoteGradeController::watchViewer(QWidget* viewer, std::function<QRectF()> pictureRect) {
    if (viewer_) viewer_->removeEventFilter(this);
    viewer_ = viewer;
    pictureRect_ = std::move(pictureRect);
    if (viewer_) viewer_->installEventFilter(this);
}

bool RemoteGradeController::eventFilter(QObject* watched, QEvent* event) {
    if (watched == viewer_ && mode_ == Mode::Following && event->type() == QEvent::MouseButtonPress) {
        auto* e = static_cast<QMouseEvent*>(event);
        const QRectF r = pictureRect_ ? pictureRect_() : QRectF();
        if (e->button() == Qt::LeftButton && (e->modifiers() & Qt::AltModifier) && r.width() > 0 && r.height() > 0 && r.contains(e->position())) {
            point((e->position().x() - r.left()) / r.width(), (e->position().y() - r.top()) / r.height());
            return true;
        }
    }
    return QObject::eventFilter(watched, event);
}

void RemoteGradeController::paintOverlay(QPainter& p, const QRectF& picture) const {
    if (mode_ == Mode::Following && showingStream_ && !streamPicture_.isNull()) {
        p.drawImage(picture, streamPicture_);
        p.setPen(Qt::white);
        p.drawText(picture.adjusted(8, 8, -8, -8), Qt::AlignTop | Qt::AlignLeft, tr("The colourist's stream (this shot's media is missing here)"));
    }
    if (mode_ != Mode::Hosting) return;
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    for (const ShownPointer& s : pointers()) {
        const QPointF at(picture.left() + s.pointer.x * picture.width(), picture.top() + s.pointer.y * picture.height());
        QColor ring(255, 200, 0);
        ring.setAlphaF(float(s.fade));
        p.setPen(QPen(ring, 3));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(at, 14, 14);
        p.drawEllipse(at, 3, 3);
        if (!s.pointer.author.empty()) p.drawText(at + QPointF(18, -10), qs(s.pointer.author));
    }
    p.restore();
}

void RemoteGradeController::sendNote(const QString& text) {
    if (mode_ != Mode::Following || text.trimmed().isEmpty()) return;
    output::RemoteMarker m;
    const Sequence* s = state_->project().active();
    m.sequence = s ? s->id : 0;
    m.frame = program_ ? program_->position() : 0;
    if (s) m.timecode = formatTimecode(m.frame, s->fps);
    m.text = text.trimmed().toStdString();
    m.author = settings_.name;
    client_.sendNote(m);
}

void RemoteGradeController::point(double x, double y) {
    if (mode_ != Mode::Following) return;
    remote::Pointer p;
    const Sequence* s = state_->project().active();
    p.sequence = s ? s->id : 0;
    p.frame = program_ ? program_->position() : 0;
    p.x = std::clamp(x, 0.0, 1.0);
    p.y = std::clamp(y, 0.0, 1.0);
    p.author = settings_.name;
    client_.sendPointer(p);
}

std::vector<RemoteGradeController::ShownPointer> RemoteGradeController::pointers() const {
    std::vector<ShownPointer> out;
    for (const Seen& p : pointers_) {
        const double age = double(p.since.elapsed()) / kPointerShownMs;
        if (age <= 1) out.push_back({p.pointer, std::clamp(1.5 - 1.5 * age, 0.0, 1.0)});
    }
    return out;
}

QString RemoteGradeController::status() const {
    if (mode_ == Mode::Hosting) {
        const remote::GradeHost::Status st = host_.status();
        if (!st.connected) {
            QString text = tr("Waiting for a client to join on port %1 (notes back on %2)").arg(st.port).arg(st.notesPort);
            if (!st.error.empty()) text += QStringLiteral(" (%1)").arg(qs(st.error));
            return text;
        }
        const QString who = st.client.empty() ? tr("A client") : qs(st.client);
        return tr("%1 follows: %n change(s) sent", "", int(std::min<int64_t>(st.patches + st.snapshots, 1 << 30))).arg(who) +
               tr(", %n note(s)", "", st.notes) + (st.resyncs ? tr(", sent whole again %n time(s)", "", int(st.resyncs)) : QString());
    }
    if (mode_ == Mode::Following) {
        const remote::GradeClient::Status st = client_.status();
        if (!st.connected) return st.error.empty() ? tr("Joining %1…").arg(qs(settings_.address)) : qs(st.error);
        if (!followError_.isEmpty()) return followError_;
        QString text = tr("Following the colourist: %n change(s)", "", int(std::min<int64_t>(st.patches + st.snapshots, 1 << 30)));
        if (st.mismatches) text += tr(", caught up again %n time(s)", "", int(st.mismatches));
        if (showingStream_) text += tr("; this shot's media is missing here, so it shows the colourist's stream");
        return text;
    }
    return tr("Off");
}

QByteArray RemoteGradeController::toolJson() {
    return QByteArrayLiteral(R"json({"name":"montage_remote_grade","title":"Remote grading",
"description":"Remote grading: another Montage with its own copy of the media (a client, or a second colourist) follows this one live. \"host\": this Montage sends every change to the project, the Program's playhead and play state, the viewer's exposure check and wipe, and the scope shown, over SRT encrypted with the passphrase (made when there is none; it is given back), to one client at a time; the client's notes come back as markers and its pointers show on the Program monitor. \"join\": this Montage follows the colourist at `address`, its project replaced by theirs (keeping its own media files; refused while this project has unsaved changes; the project is then untitled, so saving asks where), playing as they play; `stream` is their Remote Review stream, shown where a clip's media is missing here. \"note\": following, a note at the playhead. \"status\", \"stop\".",
"inputSchema":{"type":"object","properties":{
"action":{"type":"string","enum":["status","host","join","note","stop"]},
"address":{"type":"string","description":"host: srt://0.0.0.0:9100 (notes back on the next port); join: srt://colourist:9100"},
"passphrase":{"type":"string","description":"10 to 79 characters"},
"name":{"type":"string","description":"join: shown with your notes and pointers"},
"stream":{"type":"string","description":"join: the colourist's Remote Review stream (srt://colourist:9000), for missing media"},
"text":{"type":"string","description":"note: its text"}},
"required":["action"]},
"annotations":{"readOnlyHint":false,"destructiveHint":true,"openWorldHint":true}})json");
}

QString RemoteGradeController::runTool(const QJsonObject& args, QJsonObject& structured, bool& isError) {
    const QString action = args.value("action").toString();
    auto has = [&](const char* k) { return args.contains(QLatin1String(k)); };
    if (action == "stop") {
        stop();
    } else if (action == "host" || action == "join") {
        const bool hosting = action == "host";
        remote::GradeSettings g = loadSettings(hosting);
        if (has("address")) g.address = args.value("address").toString().toStdString();
        if (has("passphrase")) g.passphrase = args.value("passphrase").toString().toStdString();
        if (has("name")) g.name = args.value("name").toString().toStdString();
        if (hosting && g.passphrase.empty()) g.passphrase = output::generatePassphrase();
        QString err;
        if (!(hosting ? host(g, &err) : join(g, has("stream") ? args.value("stream").toString() : loadStream(), &err))) {
            isError = true;
            return err;
        }
        if (hosting) structured["passphrase"] = qs(g.passphrase);
    } else if (action == "note") {
        if (mode_ != Mode::Following) {
            isError = true;
            return tr("Notes are sent while following a colourist (join first)");
        }
        sendNote(args.value("text").toString());
    } else if (action != "status") {
        isError = true;
        return tr("action is status, host, join, note or stop");
    }
    structured["mode"] = mode_ == Mode::Hosting ? "hosting" : mode_ == Mode::Following ? "following" : "off";
    if (mode_ == Mode::Hosting) {
        const remote::GradeHost::Status st = host_.status();
        structured["client_connected"] = st.connected;
        structured["client"] = qs(st.client);
        structured["revision"] = double(st.revision);
        structured["patches"] = double(st.patches);
        structured["snapshots"] = double(st.snapshots);
        structured["notes_received"] = st.notes;
        structured["port"] = st.port;
        if (!st.error.empty()) structured["error"] = qs(st.error);
        QJsonArray addresses;
        std::string scheme, host;
        int port = 0;
        output::parseStreamAddress(settings_.address, scheme, host, port);
        if (host == "0.0.0.0" || host == "::") {
            for (const QHostAddress& a : QNetworkInterface::allAddresses())
                if (a.protocol() == QAbstractSocket::IPv4Protocol && !a.isLoopback() && !a.isLinkLocal())
                    addresses.append(QStringLiteral("srt://%1:%2").arg(a.toString()).arg(port));
        } else {
            addresses.append(qs(settings_.address));
        }
        structured["addresses"] = addresses;
    } else if (mode_ == Mode::Following) {
        const remote::GradeClient::Status st = client_.status();
        structured["connected"] = st.connected;
        structured["revision"] = double(st.revision);
        structured["mismatches"] = double(st.mismatches);
        structured["showing_stream"] = showingStream_;
    }
    return status();
}

// ---------------------------------------------------------------------------

RemoteGradePanel::RemoteGradePanel(RemoteGradeController* controller, QWidget* parent) : QDialog(parent), controller_(controller) {
    setObjectName(QStringLiteral("remoteGradeDialog"));
    setWindowTitle(tr("Remote Grading"));
    setAttribute(Qt::WA_DeleteOnClose);
    auto* layout = new QVBoxLayout(this);
    auto* intro = new QLabel(tr("Another Montage with its own copy of the media follows this one live: a client watching the grade on their own "
                                "calibrated display, or a second colourist. What goes over is the work, not the picture, encrypted with the "
                                "passphrase; the client renders every frame itself."),
                             this);
    intro->setWordWrap(true);
    layout->addWidget(intro);
    tabs_ = new QTabWidget(this);
    tabs_->setObjectName(QStringLiteral("remoteGradeTabs"));
    // Hosting.
    auto* hostPage = new QWidget(tabs_);
    auto* hostForm = new QFormLayout(hostPage);
    const remote::GradeSettings h = RemoteGradeController::loadSettings(true);
    hostAddress_ = new QLineEdit(qs(h.address), hostPage);
    hostAddress_->setObjectName(QStringLiteral("remoteGradeHostAddress"));
    hostAddress_->setToolTip(tr("srt://0.0.0.0:port takes a client on any of this computer's addresses; notes come back on the next port"));
    hostForm->addRow(tr("Listen on:"), hostAddress_);
    hostPassphrase_ = new QLineEdit(qs(h.passphrase.empty() ? output::generatePassphrase() : h.passphrase), hostPage);
    hostPassphrase_->setObjectName(QStringLiteral("remoteGradeHostPassphrase"));
    auto* generate = new QPushButton(tr("New"), hostPage);
    connect(generate, &QPushButton::clicked, this, [this] { hostPassphrase_->setText(qs(output::generatePassphrase())); });
    auto* passRow = new QHBoxLayout;
    passRow->addWidget(hostPassphrase_, 1);
    passRow->addWidget(generate);
    hostForm->addRow(tr("Passphrase:"), passRow);
    auto* hostHint = new QLabel(tr("Give the client this computer's address and the passphrase. Their notes land as markers; their pointers "
                                   "show on the Program monitor."),
                                hostPage);
    hostHint->setWordWrap(true);
    hostForm->addRow(hostHint);
    tabs_->addTab(hostPage, tr("Host"));
    // Following.
    auto* joinPage = new QWidget(tabs_);
    auto* joinForm = new QFormLayout(joinPage);
    const remote::GradeSettings j = RemoteGradeController::loadSettings(false);
    joinAddress_ = new QLineEdit(qs(j.address), joinPage);
    joinAddress_->setObjectName(QStringLiteral("remoteGradeJoinAddress"));
    joinForm->addRow(tr("Colourist:"), joinAddress_);
    joinPassphrase_ = new QLineEdit(qs(j.passphrase), joinPage);
    joinPassphrase_->setObjectName(QStringLiteral("remoteGradeJoinPassphrase"));
    joinForm->addRow(tr("Passphrase:"), joinPassphrase_);
    name_ = new QLineEdit(qs(j.name), joinPage);
    name_->setObjectName(QStringLiteral("remoteGradeName"));
    name_->setPlaceholderText(tr("Shown with your notes"));
    joinForm->addRow(tr("Your name:"), name_);
    stream_ = new QLineEdit(RemoteGradeController::loadStream(), joinPage);
    stream_->setObjectName(QStringLiteral("remoteGradeStream"));
    stream_->setPlaceholderText(tr("srt://colourist:9000 (optional)"));
    stream_->setToolTip(tr("The colourist's Remote Review stream, with the same passphrase: shown where a shot's media is missing here"));
    joinForm->addRow(tr("Their stream:"), stream_);
    note_ = new QLineEdit(joinPage);
    note_->setObjectName(QStringLiteral("remoteGradeNote"));
    note_->setPlaceholderText(tr("A note at the playhead"));
    send_ = new QPushButton(tr("Send"), joinPage);
    send_->setObjectName(QStringLiteral("remoteGradeSend"));
    auto sendNote = [this] {
        controller_->sendNote(note_->text());
        note_->clear();
    };
    connect(send_, &QPushButton::clicked, this, sendNote);
    connect(note_, &QLineEdit::returnPressed, this, sendNote);
    auto* noteRow = new QHBoxLayout;
    noteRow->addWidget(note_, 1);
    noteRow->addWidget(send_);
    joinForm->addRow(tr("Note:"), noteRow);
    auto* joinHint = new QLabel(tr("Your project is replaced by the colourist's while you follow (your media files are kept). Alt+click the "
                                   "Program monitor to point at something."),
                                joinPage);
    joinHint->setWordWrap(true);
    joinForm->addRow(joinHint);
    tabs_->addTab(joinPage, tr("Join"));
    if (controller_->mode() == RemoteGradeController::Mode::Following) tabs_->setCurrentIndex(1);
    layout->addWidget(tabs_);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("remoteGradeStatus"));
    status_->setWordWrap(true);
    status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(status_);
    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    start_ = new QPushButton(this);
    start_->setObjectName(QStringLiteral("remoteGradeStart"));
    connect(start_, &QPushButton::clicked, this, [this] {
        if (controller_->mode() != RemoteGradeController::Mode::Off) {
            controller_->stop();
            refresh();
            return;
        }
        QString err;
        bool ok;
        if (tabs_->currentIndex() == 0) {
            remote::GradeSettings g;
            g.address = hostAddress_->text().trimmed().toStdString();
            g.passphrase = hostPassphrase_->text().toStdString();
            ok = controller_->host(g, &err);
        } else {
            remote::GradeSettings g;
            g.address = joinAddress_->text().trimmed().toStdString();
            g.passphrase = joinPassphrase_->text().toStdString();
            g.name = name_->text().trimmed().toStdString();
            ok = controller_->join(g, stream_->text().trimmed(), &err);
        }
        if (!ok) status_->setText(err);
        else refresh();
    });
    buttons->addWidget(start_);
    auto* close = new QPushButton(tr("Close"), this);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    buttons->addWidget(close);
    layout->addLayout(buttons);
    timer_ = new QTimer(this);
    timer_->setInterval(500);
    connect(timer_, &QTimer::timeout, this, &RemoteGradePanel::refresh);
    timer_->start();
    connect(controller_, &RemoteGradeController::modeChanged, this, &RemoteGradePanel::refresh);
    connect(tabs_, &QTabWidget::currentChanged, this, &RemoteGradePanel::refresh);
    refresh();
}

void RemoteGradePanel::refresh() {
    const RemoteGradeController::Mode m = controller_->mode();
    start_->setText(m == RemoteGradeController::Mode::Off ? (tabs_->currentIndex() == 0 ? tr("Host") : tr("Join")) : tr("Stop"));
    tabs_->setTabEnabled(0, m != RemoteGradeController::Mode::Following);
    tabs_->setTabEnabled(1, m != RemoteGradeController::Mode::Hosting);
    for (QLineEdit* e : {hostAddress_, hostPassphrase_, joinAddress_, joinPassphrase_, name_, stream_}) e->setEnabled(m == RemoteGradeController::Mode::Off);
    note_->setEnabled(m == RemoteGradeController::Mode::Following);
    send_->setEnabled(m == RemoteGradeController::Mode::Following);
    if (m != RemoteGradeController::Mode::Off || status_->text().isEmpty()) status_->setText(controller_->status());
}

}  // namespace montage
