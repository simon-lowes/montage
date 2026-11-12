#include "Multiviewer.h"

#include <QCheckBox>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <set>

#include "EditorState.h"
#include "Settings.h"
#include "Theme.h"
#include "media/Capture.h"
#include "media/ImageSequence.h"

namespace montage {

// An input: its feed, the recorder filling it (watching, or recording too) and its tile.
struct Multiviewer::Slot {
    quint64 id = 0;
    Input input;
    std::shared_ptr<LiveFeed> feed;
    std::unique_ptr<Recorder> recorder;
    bool recording = false;  // (the recorder records it)
    bool ended = false;      // (the recorder has finished: waited for again)
    qint64 retryAt = 0;
    int generation = 0;
    int opened = 0;          // times its feed was opened
    QString path, importedPath, error;
    Id media = 0;
    int64_t notedFrames = -1;
    MultiviewTile* widget = nullptr;
};

namespace {

QString labelOf(const Multiviewer::Input& in) {
    if (!in.label.trimmed().isEmpty()) return in.label.trimmed();
    if (in.kind == QLatin1String("st2110")) return QStringLiteral("ST 2110");
    QString u = in.url.trimmed();
    if (const int q = int(u.indexOf(QLatin1Char('?'))); q > 0) u.truncate(q);
    return u.isEmpty() ? Multiviewer::tr("Input") : u;
}

bool parseOptions(const QString& text, std::map<std::string, std::string>& out) {
    for (const QString& o : text.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        const int eq = int(o.indexOf(QLatin1Char('=')));
        if (eq <= 0) return false;
        out[o.left(eq).toStdString()] = o.mid(eq + 1).toStdString();
    }
    return true;
}

}  // namespace

// ---- A tile

MultiviewTile::MultiviewTile(QWidget* parent) : QWidget(parent) {
    setMinimumSize(160, 110);
    setAttribute(Qt::WA_OpaquePaintEvent);
    choose_ = new QCheckBox(Multiviewer::tr("Rec"), this);
    choose_->setToolTip(Multiviewer::tr("Record this input while Record is on"));
    choose_->setStyleSheet(QStringLiteral("QCheckBox { color: white; background: rgba(0,0,0,140); padding: 1px 4px; border-radius: 3px; }"));
}

void MultiviewTile::show(const Multiviewer::Tile& t, const QImage& picture) {
    tile_ = t;
    picture_ = picture;
    if (choose_->isChecked() != t.input.record) {
        const QSignalBlocker block(choose_);
        choose_->setChecked(t.input.record);
    }
    update();
}

QColor MultiviewTile::tally() const {
    if (tile_.recording) return QColor(220, 30, 30);
    if (tile_.onSource) return QColor(30, 190, 60);
    return {};
}

void MultiviewTile::resizeEvent(QResizeEvent*) {
    choose_->adjustSize();
    choose_->move(width() - choose_->width() - 8, 8);
}

void MultiviewTile::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(12, 12, 12));
    const int barH = std::max(18, QFontMetrics(theme::monoFont(9)).height() + 4);
    const QRectF area = QRectF(rect()).adjusted(5, 5, -5, -5 - barH);
    if (!picture_.isNull()) {
        const QSizeF sz = QSizeF(picture_.size()).scaled(area.size(), Qt::KeepAspectRatio);
        const QRectF target(area.center().x() - sz.width() / 2, area.center().y() - sz.height() / 2, sz.width(), sz.height());
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(target, picture_);
        // Its sound's meters at the picture's left: green, yellow from -18 dBFS, red from -6.
        const int n = std::min<int>(16, int(tile_.peaks.size()));
        const double w = std::clamp(target.width() / 60, 2.0, 6.0), gap = 1, h = target.height() * 0.7, bottom = target.bottom() - 4;
        double x = target.left() + 4;
        if (n) p.fillRect(QRectF(x - 1, bottom - h - 1, n * (w + gap) + 1, h + 2), QColor(0, 0, 0, 150));
        auto at = [h](double db) { return std::clamp((db + 60) / 60, 0.0, 1.0) * h; };
        for (int c = 0; c < n; ++c, x += w + gap) {
            const double db = tile_.peaks[size_t(c)] > 0 ? 20 * std::log10(double(tile_.peaks[size_t(c)])) : -100;
            const double lit = at(db), yellow = at(-18), red = at(-6);
            p.fillRect(QRectF(x, bottom - std::min(lit, yellow), w, std::min(lit, yellow)), QColor(60, 200, 90));
            if (lit > yellow) p.fillRect(QRectF(x, bottom - std::min(lit, red), w, std::min(lit, red) - yellow), QColor(230, 200, 40));
            if (lit > red) p.fillRect(QRectF(x, bottom - lit, w, lit - red), QColor(230, 50, 40));
        }
    } else {
        p.setPen(theme::kTextDim);
        p.drawText(area.adjusted(8, 8, -8, -8), Qt::AlignCenter | Qt::TextWordWrap,
                   tile_.error.isEmpty() ? Multiviewer::tr("Waiting for the feed…") : tile_.error);
    }
    // The tally round the tile: red while it records, green while it is on the Source monitor (both: red, green within).
    if (const QColor t = tally(); t.isValid()) {
        p.setPen(QPen(t, 4));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(rect()).adjusted(2, 2, -2, -2));
        if (tile_.recording && tile_.onSource) {
            p.setPen(QPen(QColor(30, 190, 60), 2));
            p.drawRect(QRectF(rect()).adjusted(5, 5, -5, -5));
        }
    }
    // Its label, and its timecode.
    const QRectF bar(5, height() - 5 - barH, width() - 10, barH);
    p.fillRect(bar, QColor(28, 28, 28));
    p.setFont(theme::monoFont(9));
    const QString tc = tile_.timecode.isEmpty() ? QStringLiteral("--:--:--:--") : tile_.timecode;
    const int tcW = p.fontMetrics().horizontalAdvance(tc) + 8;
    p.setPen(tile_.running ? QColor(240, 240, 240) : theme::kTextDim);
    p.drawText(bar.adjusted(bar.width() - tcW, 0, -4, 0), Qt::AlignRight | Qt::AlignVCenter, tc);
    QFont f = font();
    f.setBold(true);
    p.setFont(f);
    const QString label = p.fontMetrics().elidedText(labelOf(tile_.input), Qt::ElideRight, int(bar.width()) - tcW - 8);
    p.drawText(bar.adjusted(6, 0, -tcW, 0), Qt::AlignLeft | Qt::AlignVCenter, label);
}

void MultiviewTile::mouseDoubleClickEvent(QMouseEvent*) {
    if (onOpen) onOpen();
}

void MultiviewTile::contextMenuEvent(QContextMenuEvent* e) {
    if (onMenu) onMenu(e->globalPos());
}

// ---- The window

Multiviewer::Multiviewer(EditorState* state, QWidget* parent) : QWidget(parent, Qt::Window), state_(state) {
    setWindowTitle(tr("Multiviewer"));
    setObjectName(QStringLiteral("multiviewer"));
    resize(1100, 720);
    QSettings settings = appSettings();
    auto* layout = new QVBoxLayout(this);
    auto* top = new QHBoxLayout;
    auto* add = new QPushButton(tr("Add Input…"), this);
    add->setObjectName(QStringLiteral("multiviewAdd"));
    connect(add, &QPushButton::clicked, this, [this] { addInteractive(); });
    top->addWidget(add);
    record_ = new QPushButton(tr("Record"), this);
    record_->setObjectName(QStringLiteral("multiviewRecord"));
    record_->setCheckable(true);
    record_->setToolTip(tr("Record the inputs chosen (Rec on their tiles), each into the folder and the Captures bin"));
    connect(record_, &QPushButton::clicked, this, [this](bool on) {
        QString why;
        if (on && !startRecording(&why)) say(why);
        if (!on) stopRecording();
        const QSignalBlocker block(record_);
        record_->setChecked(recording_);
    });
    top->addWidget(record_);
    top->addSpacing(12);
    top->addWidget(new QLabel(tr("Into:"), this));
    folder_ = new QLineEdit(settings.value(QStringLiteral("capture/folder"), QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)).toString(), this);
    folder_->setObjectName(QStringLiteral("multiviewFolder"));
    top->addWidget(folder_, 1);
    auto* browse = new QPushButton(tr("Choose…"), this);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString d = QFileDialog::getExistingDirectory(this, tr("Record Into"), folder_->text());
        if (!d.isEmpty()) folder_->setText(d);
    });
    top->addWidget(browse);
    codec_ = new QComboBox(this);
    codec_->setObjectName(QStringLiteral("multiviewCodec"));
    for (const auto& [id, name] : captureCodecs()) codec_->addItem(QString::fromStdString(name), QString::fromStdString(id));
    if (const int i = codec_->findData(settings.value(QStringLiteral("capture/codec"), QStringLiteral("prores_hq")).toString()); i >= 0)
        codec_->setCurrentIndex(i);
    top->addWidget(codec_);
    layout->addLayout(top);
    wall_ = new QWidget(this);
    wall_->setObjectName(QStringLiteral("multiviewWall"));
    wall_->setAutoFillBackground(true);
    QPalette pal = wall_->palette();
    pal.setColor(QPalette::Window, QColor(0, 0, 0));
    wall_->setPalette(pal);
    grid_ = new QGridLayout(wall_);
    grid_->setContentsMargins(2, 2, 2, 2);
    grid_->setSpacing(2);
    layout->addWidget(wall_, 1);
    status_ = new QLabel(tr("Add up to %1 inputs: double-click one to watch it on the Source monitor").arg(kMaxInputs), this);
    status_->setObjectName(QStringLiteral("multiviewStatus"));
    layout->addWidget(status_);
    timer_ = new QTimer(this);
    timer_->setInterval(40);
    connect(timer_, &QTimer::timeout, this, &Multiviewer::tick);
    timer_->start();
}

Multiviewer::~Multiviewer() {
    // (their notices, queued to this, go with it)
    for (auto& s : inputs_)
        if (s->recorder) s->recorder->stop();
}

Multiviewer::Slot* Multiviewer::find(quint64 id) const {
    for (const auto& s : inputs_)
        if (s->id == id) return s.get();
    return nullptr;
}

int Multiviewer::indexOf(const Slot* s) const {
    for (size_t i = 0; i < inputs_.size(); ++i)
        if (inputs_[i].get() == s) return int(i);
    return -1;
}

std::shared_ptr<LiveFeed> Multiviewer::feed(int i) const { return i >= 0 && i < inputCount() ? inputs_[size_t(i)]->feed : nullptr; }

MultiviewTile* Multiviewer::tileWidget(int i) const { return i >= 0 && i < inputCount() ? inputs_[size_t(i)]->widget : nullptr; }

Multiviewer::Tile Multiviewer::tile(int i) const {
    Tile t;
    if (i < 0 || i >= inputCount()) return t;
    const Slot& s = *inputs_[size_t(i)];
    t.input = s.input;
    t.input.label = labelOf(s.input);
    const LiveFeed::Now now = s.feed->look();
    t.running = now.running && now.pictures > 0;
    t.recording = s.recording && !s.ended;
    t.onSource = i == onSource_;
    t.timecode = QString::fromStdString(now.timecode);
    t.pictures = now.pictures;
    t.width = now.width, t.height = now.height;
    t.peaks = now.peaks;
    t.path = s.path;
    if (s.recorder) t.repeated = s.recorder->status().repeated;
    t.opened = s.opened;
    t.media = s.media;
    t.error = s.error;
    return t;
}

QString Multiviewer::folder() const { return folder_->text().trimmed(); }
void Multiviewer::setFolder(const QString& dir) { folder_->setText(dir); }
void Multiviewer::setCodec(const QString& id) {
    if (const int i = codec_->findData(id); i >= 0) codec_->setCurrentIndex(i);
}

QString Multiviewer::nextPath(const QString& label) const {
    QString name = label;
    name.replace(QRegularExpression(QStringLiteral("[/\\\\:*?\"<>|]")), QStringLiteral("_"));
    name = name.trimmed().isEmpty() ? tr("Input") : name.trimmed();
    std::set<QString> taken;
    for (const auto& s : inputs_)
        if (!s->path.isEmpty()) taken.insert(s->path);
    // (JPEG XS records to MXF, the rest to QuickTime)
    const QString ext = codec_->currentData().toString() == QLatin1String("jpeg_xs") ? QStringLiteral("mxf") : QStringLiteral("mov");
    for (int n = 1;; ++n) {
        const QString path = QDir(folder()).filePath(QStringLiteral("%1 %2.%3").arg(name).arg(n, 3, 10, QLatin1Char('0')).arg(ext));
        if (!QFileInfo::exists(path) && !taken.count(path)) return path;
    }
}

bool Multiviewer::startSlot(Slot& s, bool record, QString* error) {
    QString refused;
    if (record && (folder().isEmpty() || !QDir().mkpath(folder()))) {
        // (watched on, unrecorded, saying why)
        refused = tr("Not recorded: choose a folder to record into");
        if (error) *error = refused;
        record = false;
    }
    // A feed being received is recorded, or no longer, from the same reception: nothing is opened again (an SRT
    // caller does not have to call again, and no picture is missed between watching and recording).
    // (a feed still being opened takes the file once it runs: not waited for, as this thread draws the window)
    if (s.recorder && s.recorder->recording() && !s.ended) {
        const bool running = s.feed->look().running;
        CaptureFile to;
        to.codec = codec_->currentData().toString().toStdString();
        const QString path = record ? nextPath(labelOf(s.input)) : QString();
        to.path = path.toStdString();
        std::string why;
        if (s.recorder->recordTo(to, &why, running)) {
            if (s.recording) importSlot(s);  // (a recording shorter than its first fragment, whole now)
            s.recording = record;
            s.error = refused;
            if (record) {
                s.path = path;
                s.media = 0;
                s.importedPath.clear();
                s.notedFrames = -1;
            }
            announce(s);
            return true;
        }
        if (s.recorder->recording()) {
            // (a file it cannot record: said, the feed watched on as it was)
            s.error = QString::fromStdString(why);
            if (error) *error = s.error;
            return false;
        }
        // (the feed ended meanwhile: opened again)
    }
    if (s.recorder) s.recorder->stop();  // (its notices are an earlier generation's: passed over)
    if (s.recording) importSlot(s);       // (a recording shorter than its first fragment, whole now)
    s.recorder.reset();
    s.recording = false;
    s.ended = false;
    s.error = refused;
    CaptureSettings cs;
    cs.video.device = s.input.kind.toStdString();
    cs.video.url = s.input.url.trimmed().toStdString();
    if (!parseOptions(s.input.options, cs.video.options)) {
        s.error = tr("Options are name=value, apart by spaces");
        if (error) *error = s.error;
        return false;
    }
    cs.live = s.feed;
    cs.codec = codec_->currentData().toString().toStdString();
    cs.openTimeout = 10;
    if (record) {
        s.path = nextPath(labelOf(s.input));
        s.media = 0;
        s.importedPath.clear();
        s.notedFrames = -1;
        cs.path = s.path.toStdString();
    }
    auto r = std::make_unique<Recorder>();
    const int generation = ++s.generation;
    const quint64 id = s.id;
    QPointer<Multiviewer> self(this);
    // (the file whose first second is written, as the recorder says on its own thread: an input's files follow one
    // another on one recorder)
    Recorder* recorder = r.get();
    r->onFirstFragment = [self, id, generation, recorder] {
        const QString path = QString::fromStdString(recorder->status().path);
        QMetaObject::invokeMethod(self.data(), [self, id, generation, path] {
            if (!self) return;
            if (Slot* s = self->find(id); s && s->generation == generation && s->path == path) self->importSlot(*s);
        }, Qt::QueuedConnection);
    };
    // A file named while the feed was received that could not be begun: said on its tile, the feed watched on.
    r->onFileFinished = [self, id, generation](const std::string& file, bool ok, const std::string& message) {
        const QString path = QString::fromStdString(file), m = QString::fromStdString(message);
        QMetaObject::invokeMethod(self.data(), [self, id, generation, path, ok, m] {
            if (!self) return;
            Slot* s = self->find(id);
            if (!s || s->generation != generation || s->path != path) return;
            if (!m.isEmpty()) s->error = m;
            if (!ok && s->recording) {
                s->recording = false;
                self->announce(*s);
            }
            emit self->changed();
        }, Qt::QueuedConnection);
    };
    r->onFinished = [self, id, generation](bool ok, const std::string& message) {
        const QString m = QString::fromStdString(message);
        QMetaObject::invokeMethod(self.data(), [self, id, generation, ok, m] {
            if (self) self->slotFinished(id, generation, ok, m);
        }, Qt::QueuedConnection);
    };
    std::string err;
    if (!r->start(cs, &err)) {
        s.error = QString::fromStdString(err);
        if (error) *error = s.error;
        s.ended = true;
        s.retryAt = QDateTime::currentMSecsSinceEpoch() + 5000;
        return false;
    }
    s.recorder = std::move(r);
    s.recording = record;
    ++s.opened;
    announce(s);
    return true;
}

void Multiviewer::importSlot(Slot& s) {
    if (s.media || !s.recording || s.path.isEmpty() || !QFileInfo::exists(s.path)) return;
    QStringList errors;
    const std::vector<Id> ids = state_->importFiles({s.path}, &errors, tr("Captures"));
    if (!ids.empty()) {
        s.media = ids.front();
        if (const MediaItem* m = state_->project().findMedia(s.media)) s.importedPath = QString::fromStdString(mediaFileOnDisk(m->path));
        announce(s);
    } else if (!errors.isEmpty()) {
        s.error = errors.front();
    }
    emit changed();
}

void Multiviewer::announce(Slot& s) {
    // (its media only while it is the recording being made: one made earlier is not what the feed now numbers)
    const bool recording = s.recording && !s.ended;
    if (indexOf(&s) == onSource_ && onSource_ >= 0) emit showOnSource(s.feed, recording ? s.media : 0, labelOf(s.input), recording, true);
}

void Multiviewer::slotFinished(quint64 id, int generation, bool ok, const QString& message) {
    Slot* s = find(id);
    if (!s || s->generation != generation) return;  // (an earlier recorder's)
    if (ok) importSlot(*s);
    s->ended = true;
    if (!message.isEmpty()) s->error = message;
    // (waited for again: watched, and recorded again if it still should be, from when it comes back)
    s->retryAt = QDateTime::currentMSecsSinceEpoch() + 2000;
    announce(*s);
    emit changed();
}

bool Multiviewer::addInput(const Input& in, QString* error) {
    if (inputCount() >= kMaxInputs) {
        if (error) *error = tr("A multiviewer shows up to %1 inputs").arg(kMaxInputs);
        return false;
    }
    if (in.url.trimmed().isEmpty() && in.kind != QLatin1String("lavfi")) {
        if (error) *error = tr("Name the input's address or device");
        return false;
    }
    std::map<std::string, std::string> check;
    if (!parseOptions(in.options, check)) {
        if (error) *error = tr("Options are name=value, apart by spaces");
        return false;
    }
    auto s = std::make_unique<Slot>();
    s->id = nextId_++;
    s->input = in;
    s->feed = std::make_shared<LiveFeed>(640, 360);
    s->widget = new MultiviewTile(wall_);
    s->widget->setObjectName(QStringLiteral("multiviewTile%1").arg(s->id));
    const quint64 id = s->id;
    s->widget->onOpen = [this, id] { setOnSource(indexOf(find(id))); };
    s->widget->onMenu = [this, id](QPoint at) {
        QMenu menu(this);
        const int i = indexOf(find(id));
        menu.addAction(i == onSource_ ? tr("Off the Source Monitor") : tr("Watch on the Source Monitor"), this,
                       [this, id] {
                           const int j = indexOf(find(id));
                           setOnSource(j == onSource_ ? -1 : j);
                       });
        menu.addAction(tr("Remove"), this, [this, id] { removeInput(indexOf(find(id))); });
        menu.exec(at);
    };
    connect(s->widget->chooser(), &QCheckBox::toggled, this, [this, id](bool on) { setChosen(indexOf(find(id)), on); });
    Slot& slot = *s;
    inputs_.push_back(std::move(s));
    relayout();
    startSlot(slot, recording_ && in.record);  // (a feed that cannot be opened says so on its tile, and is waited for)
    emit changed();
    return true;
}

bool Multiviewer::removeInput(int i) {
    if (i < 0 || i >= inputCount()) return false;
    std::unique_ptr<Slot> s = std::move(inputs_[size_t(i)]);
    inputs_.erase(inputs_.begin() + i);
    if (s->recorder) s->recorder->stop();
    if (s->recording) importSlot(*s);
    if (i == onSource_) {
        onSource_ = -1;
        emit showOnSource(s->feed, s->media, labelOf(s->input), false, false);
    } else if (i < onSource_) {
        --onSource_;
    }
    // (later: the tile's own context menu may be what removes it, its handler still running)
    s->widget->hide();
    s->widget->deleteLater();
    relayout();
    emit changed();
    return true;
}

void Multiviewer::clear() {
    while (inputCount()) removeInput(inputCount() - 1);
}

void Multiviewer::relayout() {
    while (grid_->count()) grid_->takeAt(0);
    const int n = inputCount();
    const int cols = n <= 1 ? 1 : n <= 4 ? 2 : n <= 9 ? 3 : 4;
    for (int i = 0; i < n; ++i) grid_->addWidget(inputs_[size_t(i)]->widget, i / cols, i % cols);
    for (int c = 0; c < 4; ++c) grid_->setColumnStretch(c, c < cols ? 1 : 0);
    for (int r = 0; r < 4; ++r) grid_->setRowStretch(r, r < (n + cols - 1) / std::max(1, cols) ? 1 : 0);
}

void Multiviewer::setChosen(int i, bool record) {
    if (i < 0 || i >= inputCount()) return;
    Slot& s = *inputs_[size_t(i)];
    if (s.input.record == record) return;
    s.input.record = record;
    if (recording_ && s.recording != record) startSlot(s, record);
    emit changed();
}

bool Multiviewer::startRecording(QString* error) {
    if (recording_) return true;
    if (folder().isEmpty() || !QDir().mkpath(folder())) {
        if (error) *error = tr("Choose a folder to record into");
        return false;
    }
    recording_ = true;
    for (auto& s : inputs_)
        if (s->input.record && !s->recording) startSlot(*s, true);
    appSettings().setValue(QStringLiteral("capture/folder"), folder());
    {
        const QSignalBlocker block(record_);
        record_->setChecked(true);
    }
    emit changed();
    return true;
}

void Multiviewer::stopRecording() {
    if (!recording_) return;
    recording_ = false;
    for (auto& s : inputs_)
        if (s->recording) startSlot(*s, false);  // (finished and imported; watched on)
    {
        const QSignalBlocker block(record_);
        record_->setChecked(false);
    }
    emit changed();
}

void Multiviewer::setOnSource(int i) {
    if (i < -1 || i >= inputCount()) i = -1;
    if (i == onSource_) {
        if (i >= 0) announce(*inputs_[size_t(i)]);  // (shown again)
        return;
    }
    if (onSource_ >= 0) {
        Slot& was = *inputs_[size_t(onSource_)];
        onSource_ = -1;
        emit showOnSource(was.feed, was.media, labelOf(was.input), false, false);
    }
    onSource_ = i;
    if (i >= 0) announce(*inputs_[size_t(i)]);
    emit changed();
}

void Multiviewer::sourceShowing(const std::shared_ptr<LiveFeed>& feed) {
    int at = -1;
    for (int i = 0; i < inputCount(); ++i)
        if (feed && inputs_[size_t(i)]->feed == feed) at = i;
    onSource_ = at;
}

void Multiviewer::tick() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool note = ++ticks_ % 6 == 0;  // (about four times a second)
    for (int i = 0; i < inputCount(); ++i) {
        Slot& s = *inputs_[size_t(i)];
        // (a feed that stopped, or never came, waited for again)
        if (s.ended && now >= s.retryAt) startSlot(s, recording_ && s.input.record);
        s.widget->show(tile(i), s.feed->look().picture);  // (drawn only while the window is shown)
        // The file imported is being written here: said so as it records, so it is followed as growing at once.
        if (note && s.recorder && s.recording && !s.importedPath.isEmpty()) {
            const CaptureStatus st = s.recorder->status();
            if (st.recording && st.frames != s.notedFrames) {
                state_->noteMediaFileWritten(s.importedPath);
                s.notedFrames = st.frames;
            }
        }
    }
    // (a message said for a few seconds, then how the inputs are)
    if (note && inputCount() && now >= messageUntil_) status_->setText(statusText());
}

void Multiviewer::say(const QString& text) {
    status_->setText(text);
    messageUntil_ = QDateTime::currentMSecsSinceEpoch() + 5000;
}

QString Multiviewer::statusText() const {
    int recordingCount = 0, running = 0;
    for (int i = 0; i < inputCount(); ++i) {
        const Tile t = tile(i);
        recordingCount += t.recording;
        running += t.running;
    }
    QString text = tr("%n input(s)", nullptr, inputCount()) + QStringLiteral(" · ") + tr("%1 live").arg(running);
    if (recording_) text += QStringLiteral(" · ") + tr("recording %n", nullptr, recordingCount);
    if (onSource_ >= 0) text += QStringLiteral(" · ") + tr("%1 on the Source monitor").arg(labelOf(inputs_[size_t(onSource_)]->input));
    return text;
}

bool Multiviewer::addInteractive() {
    QDialog d(this);
    d.setWindowTitle(tr("Add Input"));
    auto* form = new QFormLayout(&d);
    auto* kind = new QComboBox(&d);
    kind->addItem(tr("Network stream (SRT, RIST, UDP...)"), QString());
    for (const std::string& k : captureDeviceKinds()) kind->addItem(QString::fromStdString(k), QString::fromStdString(k));
    form->addRow(tr("Feed:"), kind);
    auto* url = new QLineEdit(&d);
    url->setPlaceholderText(tr("srt://0.0.0.0:9000?mode=listener, omt://host:6400, SDP files apart by ;"));
    form->addRow(tr("Address or device:"), url);
    auto* options = new QLineEdit(&d);
    options->setPlaceholderText(tr("name=value apart by spaces"));
    form->addRow(tr("Options:"), options);
    auto* label = new QLineEdit(&d);
    label->setPlaceholderText(tr("Camera 1"));
    form->addRow(tr("Label:"), label);
    auto* rec = new QCheckBox(tr("Record it"), &d);
    form->addRow(QString(), rec);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
    connect(buttons, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    form->addRow(buttons);
    if (d.exec() != QDialog::Accepted) return false;
    QString why;
    if (!addInput({label->text(), kind->currentData().toString(), url->text(), options->text(), rec->isChecked()}, &why)) {
        say(why);
        return false;
    }
    return true;
}

// ---- montage_capture's "multiview"

QString Multiviewer::runTool(const QJsonObject& args, QJsonObject& structured, bool& isError) {
    isError = false;
    QStringList said;
    if (args.contains("folder")) setFolder(args.value("folder").toString());
    if (args.contains("codec")) setCodec(args.value("codec").toString());
    if (args.value("clear").toBool()) clear();
    if (args.contains("remove")) {
        if (!removeInput(args.value("remove").toInt(-1))) {
            isError = true;
            return tr("No input %1").arg(args.value("remove").toInt(-1));
        }
    }
    for (const QJsonValue v : args.value("inputs").toArray()) {
        const QJsonObject o = v.toObject();
        QString why;
        if (!addInput({o.value("label").toString(), o.value("kind").toString(), o.value("url").toString(), o.value("options").toString(),
                       o.value("record").toBool()},
                      &why)) {
            isError = true;
            return why;
        }
    }
    if (args.value("choose").isArray()) {
        std::set<int> chosen;
        for (const QJsonValue v : args.value("choose").toArray()) chosen.insert(v.toInt(-1));
        for (int i = 0; i < inputCount(); ++i) setChosen(i, chosen.count(i) > 0);
    }
    if (args.contains("record")) {
        if (args.value("record").toBool()) {
            QString why;
            if (!startRecording(&why)) {
                isError = true;
                return why;
            }
        } else {
            stopRecording();
        }
    }
    if (args.contains("source")) setOnSource(args.value("source").toInt(-1));
    QJsonArray tiles;
    for (int i = 0; i < inputCount(); ++i) {
        const Tile t = tile(i);
        QJsonArray peaks;
        for (float p : t.peaks) peaks.append(p > 0 ? std::round(20 * std::log10(double(p)) * 10) / 10 : -100.0);
        QJsonObject o{{"label", t.input.label}, {"kind", t.input.kind}, {"url", t.input.url}, {"record", t.input.record}, {"running", t.running},
                      {"recording", t.recording}, {"on_source", t.onSource}, {"timecode", t.timecode}, {"pictures", double(t.pictures)},
                      {"width", t.width}, {"height", t.height}, {"peaks_dbfs", peaks}, {"opened", t.opened}};
        if (!t.path.isEmpty()) o["path"] = t.path;
        if (t.media) o["media"] = double(t.media);
        if (!t.error.isEmpty()) o["error"] = t.error;
        tiles.append(o);
    }
    structured["inputs"] = tiles;
    structured["recording"] = recording_;
    structured["source"] = onSource_;
    structured["folder"] = folder();
    return statusText();
}

}  // namespace montage
