#include "SoundLibraryPanel.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QDoubleSpinBox>
#include <QDrag>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMediaDevices>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QProgressDialog>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <algorithm>
#include <cmath>
#include <functional>

#include "Accessibility.h"
#include "EditorState.h"
#include "ModelPacks.h"
#include "Settings.h"
#include "audio/AudioRepair.h"
#include "media/SoundSearch.h"

namespace montage {

QMimeData* soundRangeMime(const SoundRange& r) {
    auto* mime = new QMimeData;
    const QJsonObject o{{"path", r.path}, {"first", double(r.first)}, {"count", double(r.count)}, {"rate", r.rate}};
    mime->setData(kSoundRangeMime, QJsonDocument(o).toJson(QJsonDocument::Compact));
    mime->setUrls({QUrl::fromLocalFile(r.path)});
    return mime;
}

bool soundRangeFromMime(const QMimeData* mime, SoundRange& out) {
    if (!mime || !mime->hasFormat(kSoundRangeMime)) return false;
    const QJsonObject o = QJsonDocument::fromJson(mime->data(kSoundRangeMime)).object();
    SoundRange r;
    r.path = o.value("path").toString();
    r.first = int64_t(o.value("first").toDouble());
    r.count = int64_t(o.value("count").toDouble());
    r.rate = o.value("rate").toInt();
    if (r.path.isEmpty() || r.count <= 0 || r.rate <= 0 || r.first < 0) return false;
    out = r;
    return true;
}

// A sound's waveform: drag across it to choose a stretch, then drag the stretch away (onto the timeline).
class SoundWaveView : public QWidget {
public:
    explicit SoundWaveView(QWidget* parent) : QWidget(parent) {
        setMinimumHeight(90);
        setFocusPolicy(Qt::ClickFocus);
        access::describe(this, {QAccessible::Graphic, [] { return SoundLibraryPanel::tr("Waveform"); }, {}, {}});
        setMouseTracking(false);
    }
    void setSound(AudioBufferPtr sound) {
        sound_ = std::move(sound);
        first_ = count_ = 0;
        update();
    }
    int64_t frames() const { return sound_ ? sound_->frames() : 0; }
    int64_t first() const { return first_; }
    int64_t count() const { return count_; }
    void setSelection(int64_t first, int64_t count) {
        const int64_t n = frames();
        first_ = std::clamp<int64_t>(first, 0, n);
        count_ = std::clamp<int64_t>(count, 0, n - first_);
        update();
        if (changed) changed();
    }
    std::function<void()> changed, dragged;

protected:
    int64_t frameAt(int x) const { return width() > 0 ? std::clamp<int64_t>(int64_t(double(x) / width() * frames()), 0, frames()) : 0; }
    int xAt(int64_t f) const { return frames() > 0 ? int(double(f) / frames() * width()) : 0; }

    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), palette().base());
        if (!sound_ || frames() == 0) {
            p.setPen(palette().color(QPalette::PlaceholderText));
            p.drawText(rect(), Qt::AlignCenter, tr("Choose a sound"));
            return;
        }
        if (count_ > 0) p.fillRect(QRect(QPoint(xAt(first_), 0), QPoint(xAt(first_ + count_), height())), palette().highlight().color().lighter(150));
        p.setPen(palette().color(QPalette::Text));
        const int mid = height() / 2, ch = std::max(1, sound_->channels);
        const int64_t n = frames();
        for (int x = 0; x < width(); ++x) {
            const int64_t a = int64_t(double(x) / width() * n), b = std::max(a + 1, int64_t(double(x + 1) / width() * n));
            float lo = 0, hi = 0;
            // Long sounds are sampled (every frame of a column needs not be looked at to draw it).
            const int64_t step = std::max<int64_t>(1, (b - a) / 512);
            for (int64_t f = a; f < b && f < n; f += step) {
                float v = 0;
                for (int c = 0; c < std::min(ch, 2); ++c) v += sound_->samples[size_t(f) * size_t(ch) + size_t(c)];
                v /= float(std::min(ch, 2));
                lo = std::min(lo, v), hi = std::max(hi, v);
            }
            p.drawLine(x, mid - int(hi * mid), x, mid - int(lo * mid));
        }
    }
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton || !sound_) return;
        press_ = e->position().toPoint();
        const int64_t f = frameAt(press_.x());
        dragPending_ = count_ > 0 && f >= first_ && f < first_ + count_;
        anchor_ = f;
        moved_ = false;
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!(e->buttons() & Qt::LeftButton) || !sound_) return;
        const QPoint pos = e->position().toPoint();
        if ((pos - press_).manhattanLength() < QApplication::startDragDistance() && !moved_) return;
        if (dragPending_) {
            dragPending_ = false;
            if (dragged) dragged();
            return;
        }
        moved_ = true;
        const int64_t f = frameAt(pos.x());
        setSelection(std::min(anchor_, f), std::abs(f - anchor_));
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton && !moved_ && sound_ && !dragPending_) setSelection(0, 0);  // a click: the whole sound
        dragPending_ = false;
    }

private:
    AudioBufferPtr sound_;
    int64_t first_ = 0, count_ = 0, anchor_ = 0;
    QPoint press_;
    bool dragPending_ = false, moved_ = false;
};

namespace {

QString lengthText(double seconds) {
    if (seconds < 60) return QStringLiteral("%1 s").arg(seconds, 0, 'f', seconds < 10 ? 2 : 1);
    return QStringLiteral("%1:%2").arg(int(seconds) / 60).arg(int(seconds) % 60, 2, 10, QChar('0'));
}

QString qs(const std::string& s) { return QString::fromStdString(s); }

}  // namespace

SoundLibraryPanel::SoundLibraryPanel(EditorState* state, std::shared_ptr<SoundLibrary> library, QWidget* parent)
    : QWidget(parent), state_(state), library_(library ? std::move(library) : SoundLibrary::shared()) {
    cancel_ = std::make_shared<std::atomic<bool>>(false);
    heard_ = std::make_shared<std::atomic<int>>(0);
    toHear_ = std::make_shared<std::atomic<int>>(0);

    auto* top = new QHBoxLayout;
    query_ = new QLineEdit(this);
    query_->setObjectName("soundLibraryQuery");
    query_->setPlaceholderText(tr("Search sounds: name, category, description"));
    query_->setClearButtonEnabled(true);
    mode_ = new QComboBox(this);
    mode_->setObjectName("soundLibraryMode");
    mode_->addItem(tr("Words"));
    mode_->addItem(tr("Sounds Like"));
    mode_->setToolTip(tr("Words: names, UCS categories, descriptions and tags, as you type.\n"
                         "Sounds Like: what it sounds like (\"a heavy wooden door creaking\"), by CLAP, on this computer."));
    folders_ = new QPushButton(tr("Folders"), this);
    folders_->setObjectName("soundLibraryFolders");
    top->addWidget(query_, 1);
    top->addWidget(mode_);
    top->addWidget(folders_);

    status_ = new QLabel(this);
    status_->setObjectName("soundLibraryStatus");

    table_ = new QTableWidget(0, 6, this);
    table_->setObjectName("soundLibraryResults");
    table_->setHorizontalHeaderLabels({tr("Name"), tr("Category"), tr("Description"), tr("Length"), tr("Channels"), tr("Rate")});
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->verticalHeader()->hide();
    table_->horizontalHeader()->setStretchLastSection(false);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    table_->viewport()->installEventFilter(this);
    table_->installEventFilter(this);

    wave_ = new SoundWaveView(this);
    wave_->setObjectName("soundLibraryWave");
    wave_->setToolTip(tr("Drag across to choose a stretch; Space plays it. Drag the stretch onto the timeline to place it."));
    wave_->installEventFilter(this);

    auto* bottom = new QHBoxLayout;
    auto* playButton = new QPushButton(tr("Play"), this);
    playButton->setObjectName("soundLibraryPlay");
    playButton->setToolTip(tr("Play the stretch, or the whole sound (Space)"));
    pitch_ = new QDoubleSpinBox(this);
    pitch_->setObjectName("soundLibraryPitch");
    pitch_->setRange(-24, 24);
    pitch_->setDecimals(1);
    pitch_->setSingleStep(1);
    pitch_->setSuffix(tr(" st"));
    pitch_->setToolTip(tr("Audition raised or lowered by semitones"));
    varispeed_ = new QCheckBox(tr("Varispeed"), this);
    varispeed_->setObjectName("soundLibraryVarispeed");
    varispeed_->setToolTip(tr("Change speed with pitch, as tape would"));
    copy_ = new QCheckBox(tr("Copy into project folder"), this);
    copy_->setObjectName("soundLibraryCopy");
    copy_->setToolTip(tr("Sounds placed from the library are copied into a \"Sound Effects\" folder beside the project first"));
    copy_->setChecked(appSettings().value(QStringLiteral("soundLibrary/copyToProject"), false).toBool());
    bottom->addWidget(playButton);
    bottom->addWidget(new QLabel(tr("Pitch"), this));
    bottom->addWidget(pitch_);
    bottom->addWidget(varispeed_);
    bottom->addStretch(1);
    bottom->addWidget(copy_);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(4, 4, 4, 4);
    lay->addLayout(top);
    lay->addWidget(status_);
    lay->addWidget(table_, 3);
    lay->addWidget(wave_, 1);
    lay->addLayout(bottom);

    typing_ = new QTimer(this);
    typing_->setSingleShot(true);
    typing_->setInterval(200);
    rescanSoon_ = new QTimer(this);
    rescanSoon_->setSingleShot(true);
    rescanSoon_->setInterval(2000);
    watcher_ = new QFileSystemWatcher(this);

    connect(query_, &QLineEdit::textChanged, this, [this] {
        if (mode_->currentIndex() == Words) typing_->start();
    });
    connect(query_, &QLineEdit::returnPressed, this, [this] { search(query_->text()); });
    connect(typing_, &QTimer::timeout, this, [this] { search(query_->text()); });
    connect(mode_, &QComboBox::currentIndexChanged, this, [this](int m) {
        query_->setPlaceholderText(m == SoundsLike ? tr("Describe the sound, then press Return") : tr("Search sounds: name, category, description"));
        if (m == Words) search(query_->text());
    });
    connect(table_, &QTableWidget::currentCellChanged, this, [this](int row) { select(row); });
    connect(table_, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& at) {
        const int row = table_->rowAt(at.y());
        if (row < 0 || row >= int(results_.size())) return;
        QMenu menu(this);
        const int64_t id = results_[size_t(row)].id;
        const QString path = qs(results_[size_t(row)].path);
        menu.addAction(tr("Find More Like This"), this, [this, id] { moreLike(id); });
        menu.addAction(tr("Show in Folder"), this, [path] { QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath())); });
        menu.exec(table_->viewport()->mapToGlobal(at));
    });
    wave_->dragged = [this] {
        if (!current()) return;
        auto* drag = new QDrag(this);
        drag->setMimeData(soundRangeMime(range()));
        drag->exec(Qt::CopyAction);
    };
    connect(playButton, &QPushButton::clicked, this, [this] { playing() ? stop() : play(); });
    connect(copy_, &QCheckBox::toggled, this, [](bool on) {
        QSettings settings = appSettings();
        settings.setValue(QStringLiteral("soundLibrary/copyToProject"), on);
    });
    connect(rescanSoon_, &QTimer::timeout, this, [this] { rescan(); });
    connect(watcher_, &QFileSystemWatcher::directoryChanged, this, [this] { rescanSoon_->start(); });
    auto* menu = new QMenu(folders_);
    folders_->setMenu(menu);
    connect(menu, &QMenu::aboutToShow, this, [this, menu] {
        menu->clear();
        menu->addAction(tr("Add Folder…"), this, [this] {
            const QString dir = QFileDialog::getExistingDirectory(this, tr("Add a Folder of Sounds"));
            if (!dir.isEmpty()) addFolder(dir);
        });
        if (!library_) return;
        const std::vector<std::string> folders = library_->folders();
        if (!folders.empty()) {
            QMenu* remove = menu->addMenu(tr("Remove Folder"));
            for (const std::string& f : folders) {
                const QString folder = qs(f);
                remove->addAction(QDir::toNativeSeparators(folder), this, [this, folder] { removeFolder(folder); });
            }
            menu->addAction(tr("Rescan"), this, [this] { rescan(); });
        }
    });

    if (!library_) {
        setEnabled(false);
        status_->setText(tr("The sound library could not be opened"));
        return;
    }
    refreshStatus();
    search(QString());
    // Brought up to date once Montage has started (folders may have changed while it was closed); that also starts
    // watching them.
    if (!library_->folders().empty()) QTimer::singleShot(0, this, [this] { rescan(); });
}

SoundLibraryPanel::~SoundLibraryPanel() {
    stop();
    *cancel_ = true;
    work_.waitForFinished();
}

bool SoundLibraryPanel::busy() const { return scanning_ || listening_; }

bool SoundLibraryPanel::addFolder(const QString& folder) {
    if (!library_) return false;
    std::string err;
    if (!library_->addFolder(QDir::cleanPath(folder).toStdString(), &err)) {
        state_->message(qs(err), 6000);
        return false;
    }
    rescan();
    return true;
}

void SoundLibraryPanel::removeFolder(const QString& folder) {
    if (!library_) return;
    *cancel_ = true;  // (a scan under way would put its sounds back)
    work_.waitForFinished();
    library_->removeFolder(folder.toStdString());
    const QString root = QDir::cleanPath(folder);
    QStringList watched;
    for (const QString& d : watcher_->directories())
        if (d == root || d.startsWith(root + '/')) watched << d;
    if (!watched.isEmpty()) watcher_->removePaths(watched);
    refreshStatus();
    search(query_->text());
}

void SoundLibraryPanel::watchFolders(const QStringList& dirs) {
    if (!watcher_->directories().isEmpty()) watcher_->removePaths(watcher_->directories());
    if (!dirs.isEmpty()) watcher_->addPaths(dirs);
}

void SoundLibraryPanel::rescan(bool wait) {
    if (!library_) return;
    if (busy()) {
        if (scanning_ && !wait) {
            rescanSoon_->start();  // after this one
            return;
        }
        // Listening (which can take a long while) stops for the scan, which listens again after.
        if (listening_) *cancel_ = true;
        work_.waitForFinished();
    }
    const int run = ++run_;
    scanning_ = true;
    *cancel_ = false;
    const bool hear = soundSearchAvailable() && soundModel().installed();
    listening_ = false;
    refreshStatus();
    auto lib = library_;
    auto cancel = cancel_;
    auto heard = heard_, toHear = toHear_;
    auto dirs = std::make_shared<QStringList>();
    QPointer<SoundLibraryPanel> self(this);
    work_ = QtConcurrent::run([lib, cancel, heard, toHear, hear, self, run, dirs] {
        lib->scan({}, cancel.get());
        // The folders to watch: each and those inside it (a watcher sees only a folder's own entries), up to a limit.
        for (const std::string& f : lib->folders()) {
            const QString root = QString::fromStdString(f);
            if (cancel->load() || !QFileInfo(root).isDir()) continue;
            *dirs << root;
            QDirIterator it(root, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
            while (it.hasNext() && dirs->size() < 2000) *dirs << it.next();
        }
        if (!hear || *cancel || lib->unheard() == 0) return;
        QMetaObject::invokeMethod(qApp, [self, run] {
            if (!self || self->run_ != run) return;
            self->scanning_ = false;
            self->listening_ = true;
            if (self->mode_->currentIndex() == Words) self->search(self->query_->text());
            self->refreshStatus();
        });
        *heard = 0;
        *toHear = lib->unheard();
        lib->listen([heard](int done, int) { *heard = done; }, cancel.get());
    });
    auto* watch = new QFutureWatcher<void>(this);
    connect(watch, &QFutureWatcher<void>::finished, this, [this, watch, run, dirs] {
        watch->deleteLater();
        if (run != run_) return;  // (one started since)
        scanning_ = listening_ = false;
        refreshStatus();
        if (mode_->currentIndex() == Words) search(query_->text());
        watchFolders(*dirs);
    });
    watch->setFuture(work_);
    if (wait) {
        work_.waitForFinished();
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents();
    } else {
        // Progress while it listens.
        auto* tick = new QTimer(watch);
        connect(tick, &QTimer::timeout, this, [this] { refreshStatus(); });
        tick->start(500);
    }
}

void SoundLibraryPanel::refreshStatus() {
    if (!library_) return;
    const int n = library_->count();
    const size_t folders = library_->folders().size();
    QString text = folders == 0 ? tr("No folders yet: Folders › Add Folder… to bring in a library of sounds")
                                : tr("%n sound(s)", nullptr, n) + tr(" in %n folder(s)", nullptr, int(folders));
    if (scanning_) text += tr(" · reading the folders…");
    else if (listening_) text += tr(" · listening %1 of %2").arg(int(*heard_)).arg(int(*toHear_));
    status_->setText(text);
}

void SoundLibraryPanel::setMode(Mode m) { mode_->setCurrentIndex(int(m)); }

int SoundLibraryPanel::search(const QString& text) {
    if (!library_) return 0;
    scores_.clear();
    if (mode_->currentIndex() == SoundsLike && !text.trimmed().isEmpty()) {
        if (!ensureHeard()) return 0;
        std::string err;
        auto model = SoundModel::load(&err);
        const std::vector<float> q = model ? model->text(text.trimmed().toStdString(), &err) : std::vector<float>{};
        if (q.empty()) {
            state_->message(qs(err), 6000);
            return 0;
        }
        results_.clear();
        for (const SoundLibrary::Match& m : library_->searchByMeaning(q, 100)) {
            results_.push_back(m.entry);
            scores_.push_back(m.score);
        }
    } else {
        results_ = library_->search(text.toStdString(), 1000);
    }
    showResults();
    return int(results_.size());
}

int SoundLibraryPanel::moreLike(int64_t id) {
    if (!library_ || !ensureHeard()) return 0;
    results_.clear();
    scores_.clear();
    for (const SoundLibrary::Match& m : library_->moreLike(id, 100)) {
        results_.push_back(m.entry);
        scores_.push_back(m.score);
    }
    showResults();
    return int(results_.size());
}

bool SoundLibraryPanel::ensureHeard() {
    if (!soundSearchAvailable()) {
        state_->message(tr("This build of Montage cannot search sound by what it sounds like (no ONNX Runtime)"), 6000);
        return false;
    }
    if (!ensureModelPack(window(), soundModel(), tr("Sound Library"),
                         tr("Searching sounds by what they sound like uses CLAP (LAION, Apache-2.0), which runs on this computer.")))
        return false;
    for (int round = 0; round < 2 && (busy() || library_->unheard() > 0); ++round) {
        if (!busy()) rescan();
        QProgressDialog progress(tr("Listening to the library…"), tr("Cancel"), 0, 0, this);
        progress.setWindowModality(Qt::WindowModal);
        progress.setMinimumDuration(300);
        QTimer tick;
        connect(&tick, &QTimer::timeout, &progress, [this, &progress] {
            if (listening_ && *toHear_ > 0) progress.setMaximum(*toHear_), progress.setValue(*heard_);
        });
        tick.start(100);
        QEventLoop wait;
        QFutureWatcher<void> watch;
        connect(&watch, &QFutureWatcher<void>::finished, &wait, &QEventLoop::quit);
        connect(&progress, &QProgressDialog::canceled, &wait, [this, &wait] {
            *cancel_ = true;
            wait.quit();
        });
        watch.setFuture(work_);
        if (!watch.isFinished()) wait.exec();
        if (*cancel_) return false;
        QCoreApplication::sendPostedEvents();
    }
    return true;
}

void SoundLibraryPanel::showResults() {
    const QSignalBlocker block(table_);
    table_->setRowCount(int(results_.size()));
    for (size_t i = 0; i < results_.size(); ++i) {
        const SoundLibraryEntry& e = results_[i];
        const QString category = e.category.empty() ? QString() : qs(e.category) + (e.subCategory.empty() ? QString() : "-" + qs(e.subCategory));
        const QStringList cells = {qs(e.name), category, qs(e.description), lengthText(e.duration), QString::number(e.channels),
                                   QStringLiteral("%1 kHz").arg(e.sampleRate / 1000.0, 0, 'g', 4)};
        for (int c = 0; c < cells.size(); ++c) {
            auto* item = new QTableWidgetItem(cells[c]);
            if (c == 0) item->setToolTip(QDir::toNativeSeparators(qs(e.path)));
            if (c == 2) item->setToolTip(qs(e.description));
            table_->setItem(int(i), c, item);
        }
    }
    table_->resizeColumnToContents(0);
    table_->resizeColumnToContents(1);
    currentRow_ = -1;
    table_->setCurrentCell(-1, -1);
    select(-1);
}

void SoundLibraryPanel::select(int row) {
    stop();
    if (row < 0 || row >= int(results_.size())) {
        currentRow_ = -1;
        sound_.reset();
        wave_->setSound(nullptr);
        return;
    }
    currentRow_ = row;
    if (table_->currentRow() != row) {
        const QSignalBlocker block(table_);
        table_->setCurrentCell(row, 0);
    }
    const SoundLibraryEntry& e = results_[size_t(row)];
    const int rate = e.sampleRate > 0 ? e.sampleRate : 48000;
    // At the file's own rate, so a stretch is counted in its samples.
    if (e.duration <= 120) {
        sound_ = decodeAudio(e.path, rate);
        wave_->setSound(sound_);
        return;
    }
    sound_.reset();
    wave_->setSound(nullptr);
    const std::string path = e.path;
    auto* watch = new QFutureWatcher<AudioBufferPtr>(this);
    connect(watch, &QFutureWatcher<AudioBufferPtr>::finished, this, [this, watch, row, path] {
        watch->deleteLater();
        if (currentRow_ != row || !current() || current()->path != path) return;
        sound_ = watch->result();
        wave_->setSound(sound_);
    });
    watch->setFuture(QtConcurrent::run([path, rate] { return decodeAudio(path, rate); }));
}

const SoundLibraryEntry* SoundLibraryPanel::current() const {
    return currentRow_ >= 0 && currentRow_ < int(results_.size()) ? &results_[size_t(currentRow_)] : nullptr;
}

SoundRange SoundLibraryPanel::range() const {
    SoundRange r;
    const SoundLibraryEntry* e = current();
    if (!e) return r;
    r.path = qs(e->path);
    r.rate = sound_ ? sound_->sampleRate : e->sampleRate;
    if (wave_->count() > 0) {
        r.first = wave_->first();
        r.count = wave_->count();
    } else {
        r.count = sound_ ? sound_->frames() : int64_t(std::llround(e->duration * r.rate));
    }
    return r;
}

void SoundLibraryPanel::setRange(int64_t first, int64_t count) { wave_->setSelection(first, count); }

void SoundLibraryPanel::setPitch(double semitones, bool varispeed) {
    pitch_->setValue(semitones);
    varispeed_->setChecked(varispeed);
}

std::vector<float> SoundLibraryPanel::auditionSound() const {
    if (!sound_ || sound_->frames() == 0) return {};
    const SoundRange r = range();
    const int ch = std::max(1, sound_->channels);
    const double semitones = pitch_->value();
    // Read through at this many of the sound's samples for each one played (its rate to 48 kHz, and varispeed's).
    double step = double(sound_->sampleRate) / 48000.0;
    if (varispeed_->isChecked()) step *= std::pow(2.0, semitones / 12.0);
    const int64_t end = std::min(r.first + r.count, sound_->frames());
    const int64_t n = int64_t(std::floor(double(end - r.first) / step));
    AudioBuffer out;
    out.sampleRate = 48000;
    out.channels = 2;
    out.samples.resize(size_t(std::max<int64_t>(0, n)) * 2);
    for (int64_t i = 0; i < n; ++i) {
        const double at = double(r.first) + double(i) * step;
        const int64_t a = int64_t(at);
        const int64_t b = std::min(a + 1, end - 1);
        const float t = float(at - double(a));
        for (int c = 0; c < 2; ++c) {
            const int src = std::min(c, ch - 1);
            const float va = sound_->samples[size_t(a) * size_t(ch) + size_t(src)], vb = sound_->samples[size_t(b) * size_t(ch) + size_t(src)];
            out.samples[size_t(i) * 2 + size_t(c)] = va + (vb - va) * t;
        }
    }
    if (!varispeed_->isChecked() && std::fabs(semitones) > 1e-6 && n > 0) {
        AudioBuffer shifted;
        pitchShift(out, shifted, semitones);
        return shifted.samples;
    }
    return out.samples;
}

void SoundLibraryPanel::play() {
    stop();
    const std::vector<float> sound = auditionSound();
    if (sound.empty()) return;
    const QAudioDevice dev = QMediaDevices::defaultAudioOutput();
    QAudioFormat fmt;
    fmt.setSampleRate(48000);
    fmt.setChannelCount(2);
    fmt.setSampleFormat(QAudioFormat::Float);
    if (dev.isNull() || !dev.isFormatSupported(fmt)) {
        state_->message(tr("No sound output to play on"), 4000);
        return;
    }
    playing_ = new QBuffer(this);
    playing_->setData(reinterpret_cast<const char*>(sound.data()), qsizetype(sound.size() * sizeof(float)));
    playing_->open(QIODevice::ReadOnly);
    sink_ = new QAudioSink(dev, fmt, this);
    connect(sink_, &QAudioSink::stateChanged, this, [this](QAudio::State s) {
        if (s == QAudio::IdleState) QTimer::singleShot(0, this, [this] { stop(); });
    });
    sink_->start(playing_);
}

void SoundLibraryPanel::stop() {
    if (sink_) {
        sink_->disconnect(this);
        sink_->stop();
        sink_->deleteLater();
        sink_ = nullptr;
    }
    if (playing_) {
        playing_->deleteLater();
        playing_ = nullptr;
    }
}

bool SoundLibraryPanel::playing() const { return sink_ != nullptr; }

bool SoundLibraryPanel::eventFilter(QObject* o, QEvent* e) {
    if (o == table_ || o == wave_) {
        // Space plays here, not the timeline.
        if (e->type() == QEvent::ShortcutOverride && static_cast<QKeyEvent*>(e)->key() == Qt::Key_Space) {
            e->accept();
            return true;
        }
        if (e->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(e)->key() == Qt::Key_Space) {
            playing() ? stop() : play();
            return true;
        }
    }
    if (o == table_->viewport()) {
        // A row dragged out: the whole sound.
        const QPoint& press = press_;
        if (e->type() == QEvent::MouseButtonPress) press_ = static_cast<QMouseEvent*>(e)->position().toPoint();
        if (e->type() == QEvent::MouseMove && (static_cast<QMouseEvent*>(e)->buttons() & Qt::LeftButton) &&
            (static_cast<QMouseEvent*>(e)->position().toPoint() - press).manhattanLength() >= QApplication::startDragDistance()) {
            const int row = table_->rowAt(press.y());
            if (row >= 0 && row < int(results_.size())) {
                if (row != currentRow_) select(row);
                const SoundRange whole = [&] {
                    SoundRange r = range();
                    if (sound_) r.first = 0, r.count = sound_->frames();
                    return r;
                }();
                if (whole.count > 0) {
                    auto* drag = new QDrag(this);
                    drag->setMimeData(soundRangeMime(whole));
                    drag->exec(Qt::CopyAction);
                }
                return true;
            }
        }
    }
    return QWidget::eventFilter(o, e);
}

}  // namespace montage
