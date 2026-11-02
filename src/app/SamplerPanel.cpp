// Montage — the Sampler panel (see SamplerPanel.h).
#include "SamplerPanel.h"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTextEdit>
#include <QVBoxLayout>
#include <chrono>
#include <cmath>

#include "EditorState.h"
#include "PlaybackController.h"
#include "SoundLibraryPanel.h"
#include "core/TakeLanes.h"

namespace montage {

namespace {

qint64 steadyNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

QString keyText(int key) {
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return QStringLiteral("%1 %2%3").arg(key).arg(QLatin1String(names[key % 12])).arg(key / 12 - 1);
}

enum Column { Key, Name, Sounds, Gain, Attack, Release, OneShot, Choke, Pitch, Level, Columns };

}  // namespace

SamplerPanel::SamplerPanel(EditorState* state, PlaybackController* program, QWidget* parent)
    : QWidget(parent), state_(state), program_(program) {
    setObjectName(QStringLiteral("samplerPanel"));
    engine_ = std::make_shared<sampler::Engine>(state_->sequence() ? state_->sequence()->sampleRate : 48000);
    auto* lay = new QVBoxLayout(this);
    auto* top = new QHBoxLayout;
    top->addWidget(new QLabel(tr("Track:")));
    trackBox_ = new QComboBox;
    trackBox_->setObjectName(QStringLiteral("samplerTrack"));
    top->addWidget(trackBox_);
    top->addWidget(new QLabel(tr("MIDI:")));
    midiBox_ = new QComboBox;
    midiBox_->setObjectName(QStringLiteral("samplerMidi"));
    midiBox_->addItem(tr("None"));
    for (const MidiPortInfo& p : midiInputs()) midiBox_->addItem(QString::fromStdString(p.name));
    top->addWidget(midiBox_);
    keysBox_ = new QCheckBox(tr("Computer keys"));
    keysBox_->setObjectName(QStringLiteral("samplerKeys"));
    keysBox_->setToolTip(tr("Z to M play C3 to B3 (S, D, G, H, J the sharps); Q to P play C4 to E5"));
    top->addWidget(keysBox_);
    top->addWidget(new QLabel(tr("Velocity:")));
    velocityBox_ = new QSpinBox;
    velocityBox_->setObjectName(QStringLiteral("samplerVelocity"));
    velocityBox_->setRange(1, 127);
    velocityBox_->setValue(100);
    top->addWidget(velocityBox_);
    top->addStretch();
    lay->addLayout(top);

    table_ = new QTableWidget(0, Columns);
    table_->setObjectName(QStringLiteral("samplerSlots"));
    table_->setHorizontalHeaderLabels({tr("Key"), tr("Name"), tr("Sounds"), tr("Gain (dB)"), tr("Attack (s)"), tr("Release (s)"), tr("One-shot"),
                                       tr("Choke"), tr("Pitch ±"), tr("Level ±")});
    table_->verticalHeader()->hide();
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->horizontalHeader()->setSectionResizeMode(Name, QHeaderView::Stretch);
    table_->setAcceptDrops(true);
    table_->viewport()->setAcceptDrops(true);
    table_->viewport()->installEventFilter(this);
    lay->addWidget(table_, 1);

    auto* buttons = new QHBoxLayout;
    auto* add = new QPushButton(tr("Add Sounds…"));
    add->setObjectName(QStringLiteral("samplerAdd"));
    auto* split = new QPushButton(tr("Split by Onsets…"));
    split->setObjectName(QStringLiteral("samplerSplit"));
    split->setToolTip(tr("Cut a recording of hits onto successive keys from the one selected"));
    auto* remove = new QPushButton(tr("Remove"));
    remove->setObjectName(QStringLiteral("samplerRemove"));
    recordButton_ = new QPushButton(tr("Record"));
    recordButton_->setObjectName(QStringLiteral("samplerRecord"));
    recordButton_->setCheckable(true);
    passBox_ = new QComboBox;
    passBox_->setObjectName(QStringLiteral("samplerPass"));
    auto* revoiceButton = new QPushButton(tr("Voice Again"));
    revoiceButton->setObjectName(QStringLiteral("samplerRevoice"));
    revoiceButton->setToolTip(tr("Render the pass's notes again through the slots as they are now"));
    for (QWidget* w : {static_cast<QWidget*>(add), static_cast<QWidget*>(split), static_cast<QWidget*>(remove)}) buttons->addWidget(w);
    buttons->addStretch();
    buttons->addWidget(recordButton_);
    buttons->addWidget(passBox_);
    buttons->addWidget(revoiceButton);
    lay->addLayout(buttons);
    status_ = new QLabel;
    status_->setObjectName(QStringLiteral("samplerStatus"));
    lay->addWidget(status_);

    connect(trackBox_, &QComboBox::activated, this, [this](int i) { setTrack(i); });
    connect(midiBox_, &QComboBox::activated, this, [this](int i) {
        QString why;
        if (!connectMidi(i > 0 ? midiBox_->itemText(i) : QString(), &why)) status_->setText(why);
    });
    connect(keysBox_, &QCheckBox::toggled, this, &SamplerPanel::setComputerKeys);
    connect(table_, &QTableWidget::cellChanged, this, &SamplerPanel::cellEdited);
    connect(add, &QPushButton::clicked, this, [this] {
        const QStringList files = QFileDialog::getOpenFileNames(this, tr("Add Sounds"), QString(), tr("Sounds (*.wav *.aif *.aiff *.flac *.mp3 *.ogg *.m4a *.caf);;All files (*)"));
        if (!files.isEmpty()) addSounds(selectedKey(), files);
    });
    connect(split, &QPushButton::clicked, this, [this] {
        const QString file = QFileDialog::getOpenFileName(this, tr("Split by Onsets"), QString(), tr("Sounds (*.wav *.aif *.aiff *.flac *.mp3 *.ogg *.m4a *.caf);;All files (*)"));
        if (!file.isEmpty()) splitByOnsets(file, selectedKey());
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        const int key = selectedKey();
        state_->apply(tr("Remove Sampler Slot"), [&](Project&, Sequence& s) { return sampler::removeSlot(s, {TrackKind::Audio, track_}, key); });
    });
    connect(recordButton_, &QPushButton::toggled, this, &SamplerPanel::setRecording);
    connect(revoiceButton, &QPushButton::clicked, this, [this] {
        if (passBox_->currentIndex() >= 0) revoice(Id(passBox_->currentData().toLongLong()));
    });
    connect(state_, &EditorState::projectChanged, this, &SamplerPanel::projectChanged);
    connect(state_, &EditorState::sequenceSwitched, this, &SamplerPanel::projectChanged);
    connect(program_, &PlaybackController::playingChanged, this, [this](bool playing) {
        if (!playing && recording_) setRecording(false);  // (the pass ends with the play)
    });
    projectChanged();
}

SamplerPanel::~SamplerPanel() {
    midi_.reset();  // (no more callbacks)
    if (computerKeys_) qApp->removeEventFilter(this);
    if (active_ && program_) program_->setSampler(nullptr);
}

const Track* SamplerPanel::audioTrack() const {
    const Sequence* s = state_->sequence();
    return s && track_ >= 0 && track_ < int(s->audioTracks.size()) ? &s->audioTracks[size_t(track_)] : nullptr;
}

void SamplerPanel::setTrack(int index) {
    if (index == track_) return;
    track_ = index;
    projectChanged();
}

int SamplerPanel::selectedKey() const {
    const int row = table_->currentRow();
    if (row >= 0 && table_->item(row, Key)) return table_->item(row, Key)->data(Qt::UserRole).toInt();
    // (the first key free from C3, where the computer's keys start)
    const Track* t = audioTrack();
    for (int k = 48; k < 128; ++k)
        if (!t || std::none_of(t->sampler.pads.begin(), t->sampler.pads.end(), [&](const SamplerSlot& sl) { return sl.key == k; })) return k;
    return 48;
}

void SamplerPanel::projectChanged() {
    const Sequence* s = state_->sequence();
    const int count = s ? int(s->audioTracks.size()) : 0;
    if (track_ >= count) track_ = std::max(0, count - 1);
    {
        const QSignalBlocker block(trackBox_);
        trackBox_->clear();
        for (int i = 0; i < count; ++i) {
            const std::string& n = s->audioTracks[size_t(i)].name;
            trackBox_->addItem(n.empty() ? QStringLiteral("A%1").arg(i + 1) : QStringLiteral("A%1 %2").arg(i + 1).arg(QString::fromStdString(n)));
        }
        trackBox_->setCurrentIndex(track_);
    }
    reloadKit();
    refreshTable();
    refreshPasses();
}

void SamplerPanel::reloadKit() {
    const Sequence* s = state_->sequence();
    const Track* t = audioTrack();
    const int rate = s ? s->sampleRate : 48000;
    const std::vector<SamplerSlot> pads = t ? t->sampler.pads : std::vector<SamplerSlot>{};
    if (kit_ && pads == loaded_ && rate == loadedRate_) return;
    if (rate != engine_->sampleRate()) {
        auto e = std::make_shared<sampler::Engine>(rate);
        {
            std::lock_guard<std::mutex> lock(inboxMutex_);
            engine_ = e;
        }
        if (active_) program_->setSampler(engine_);
    }
    std::vector<std::string> missing;
    kit_ = sampler::loadKit(t ? t->sampler : SamplerSettings{}, rate, &missing);
    loaded_ = pads;
    loadedRate_ = rate;
    engine_->setKit(kit_);
    status_->setText(missing.empty() ? QString() : tr("Not found: %1").arg(QString::fromStdString(missing.front())));
    updateActive();
}

void SamplerPanel::refreshTable() {
    refreshing_ = true;
    const int key = table_->currentRow() >= 0 && table_->item(table_->currentRow(), Key) ? table_->item(table_->currentRow(), Key)->data(Qt::UserRole).toInt() : -1;
    const Track* t = audioTrack();
    const std::vector<SamplerSlot> pads = t ? t->sampler.pads : std::vector<SamplerSlot>{};
    table_->setRowCount(int(pads.size()));
    for (int r = 0; r < int(pads.size()); ++r) {
        const SamplerSlot& sl = pads[size_t(r)];
        auto set = [&](int c, const QString& text, bool editable = true) {
            auto* it = new QTableWidgetItem(text);
            if (!editable) it->setFlags(it->flags() & ~Qt::ItemIsEditable);
            table_->setItem(r, c, it);
            return it;
        };
        set(Key, keyText(sl.key), false)->setData(Qt::UserRole, sl.key);
        set(Name, QString::fromStdString(sl.name));
        QStringList files;
        for (const SamplerSound& sd : sl.sounds) files << QFileInfo(QString::fromStdString(sd.path)).fileName();
        set(Sounds, QString::number(sl.sounds.size()), false)->setToolTip(files.join('\n'));
        set(Gain, QString::number(sl.gainDb, 'g', 4));
        set(Attack, QString::number(sl.attack, 'g', 4));
        set(Release, QString::number(sl.release, 'g', 4));
        auto* one = set(OneShot, QString(), false);
        one->setFlags(one->flags() | Qt::ItemIsUserCheckable);
        one->setCheckState(sl.oneShot ? Qt::Checked : Qt::Unchecked);
        set(Choke, sl.choke ? QString::number(sl.choke) : QString());
        set(Pitch, QString::number(sl.pitchVariation, 'g', 4));
        set(Level, QString::number(sl.levelVariation, 'g', 4));
        if (sl.key == key) table_->setCurrentCell(r, Name);
    }
    refreshing_ = false;
}

void SamplerPanel::refreshPasses() {
    const Track* t = audioTrack();
    const Id was = passBox_->currentIndex() >= 0 ? Id(passBox_->currentData().toLongLong()) : 0;
    const QSignalBlocker block(passBox_);
    passBox_->clear();
    if (!t) return;
    for (const SamplerPass& ps : t->sampler.passes)
        if (const TakeLane* ln = edit::findTakeLane(*t, ps.lane)) {
            passBox_->addItem(tr("%1 (%2 notes)").arg(QString::fromStdString(ln->name)).arg(ps.notes.size()), qlonglong(ps.lane));
            if (ps.lane == was) passBox_->setCurrentIndex(passBox_->count() - 1);
        }
    if (!was && passBox_->count()) passBox_->setCurrentIndex(passBox_->count() - 1);
}

void SamplerPanel::setSlot(const SamplerSlot& slot, const QString& label) {
    state_->apply(label, [&](Project&, Sequence& s) { return sampler::setSlot(s, {TrackKind::Audio, track_}, slot); });
}

void SamplerPanel::cellEdited(int row, int column) {
    if (refreshing_) return;
    const Track* t = audioTrack();
    if (!t || row < 0 || row >= int(t->sampler.pads.size())) return;
    SamplerSlot sl = t->sampler.pads[size_t(row)];
    const QString text = table_->item(row, column) ? table_->item(row, column)->text().trimmed() : QString();
    bool good = true;
    const double v = text.isEmpty() ? 0.0 : text.toDouble(&good);
    switch (column) {
    case Name: sl.name = text.toStdString(); break;
    case Gain: sl.gainDb = std::clamp(v, -96.0, 24.0); break;
    case Attack: sl.attack = std::clamp(v, 0.0, 10.0); break;
    case Release: sl.release = std::clamp(v, 0.0, 10.0); break;
    case OneShot: sl.oneShot = table_->item(row, column)->checkState() == Qt::Checked; break;
    case Choke: sl.choke = std::clamp(int(std::lround(v)), 0, 16); break;
    case Pitch: sl.pitchVariation = std::clamp(v, 0.0, 12.0); break;
    case Level: sl.levelVariation = std::clamp(v, 0.0, 24.0); break;
    default: return;
    }
    if (!good || sl == t->sampler.pads[size_t(row)]) {
        QMetaObject::invokeMethod(this, [this] { refreshTable(); }, Qt::QueuedConnection);  // (back to what it is)
        return;
    }
    setSlot(sl, tr("Sampler Slot"));
}

bool SamplerPanel::addSounds(int key, const QStringList& files, double start, double end) {
    const Track* t = audioTrack();
    if (!t || files.isEmpty() || key < 0 || key > 127) return false;
    SamplerSlot sl;
    sl.key = key;
    for (const SamplerSlot& e : t->sampler.pads)
        if (e.key == key) sl = e;
    for (const QString& f : files) sl.sounds.push_back({QFileInfo(f).absoluteFilePath().toStdString(), start, end, 1, 127});
    if (sl.name.empty()) sl.name = QFileInfo(files.front()).completeBaseName().toStdString();
    setSlot(sl, tr("Add Sampler Sounds"));
    return true;
}

bool SamplerPanel::splitByOnsets(const QString& file, int firstKey) {
    const Sequence* s = state_->sequence();
    if (!s) return false;
    std::string why;
    AudioBufferPtr buf = decodeAudio(file.toStdString(), s->sampleRate, &why);
    if (!buf) {
        status_->setText(tr("Cannot read %1: %2").arg(file, QString::fromStdString(why)));
        return false;
    }
    const auto slices = sampler::splitByOnsets(*buf);
    const bool done = state_->apply(tr("Split by Onsets"), [&](Project&, Sequence& sq) {
        return sampler::addSlices(sq, {TrackKind::Audio, track_}, QFileInfo(file).absoluteFilePath().toStdString(), slices, firstKey,
                                  QFileInfo(file).completeBaseName().toStdString());
    });
    if (done) status_->setText(tr("%n hit(s) on keys from %1", nullptr, int(slices.size())).arg(keyText(firstKey)));
    return done;
}

QString SamplerPanel::passFolder() const {
    const QString project = state_->filePath();
    const QString base = project.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::MusicLocation) + QStringLiteral("/Montage")
                                           : QFileInfo(project).absolutePath();
    return base + QStringLiteral("/Sampler");
}

namespace {

QString nextPassFile(const QString& folder, const QString& stem) {
    QDir().mkpath(folder);
    for (int n = 1;; ++n) {
        const QString f = QStringLiteral("%1/%2 %3.wav").arg(folder, stem).arg(n);
        if (!QFileInfo::exists(f)) return f;
    }
}

}  // namespace

uint32_t SamplerPanel::seedFor(uint32_t index) const { return sampler::noteSeed(passSeed_, index); }

void SamplerPanel::noteOn(int key, int velocity, qint64 nsAgo) {
    if (key < 0 || key > 127) return;
    const uint32_t index = noteIndex_.fetch_add(1);
    if (active_) engine_->noteOn(key, velocity, -1, seedFor(index));
    if (!recording_) return;
    SamplerNote n;
    n.key = key;
    n.velocity = std::clamp(velocity, 1, 127);
    n.at = std::max<int64_t>(0, program_->heardSample(nsAgo));
    // (in the order played, which is the order the pass renders them in)
    auto it = std::upper_bound(notes_.begin(), notes_.end(), index, [](uint32_t i, const auto& e) { return i < e.first; });
    for (auto& [k, at] : open_)
        if (at >= size_t(it - notes_.begin())) ++at;
    open_[key] = size_t(it - notes_.begin());
    notes_.insert(it, {index, n});
}

void SamplerPanel::noteOff(int key, qint64 nsAgo) {
    if (key < 0 || key > 127) return;
    if (active_) engine_->noteOff(key);
    auto it = open_.find(key);
    if (it == open_.end()) return;
    if (recording_ && it->second < notes_.size()) {
        SamplerNote& n = notes_[it->second].second;
        n.length = std::max<int64_t>(1, program_->heardSample(nsAgo) - n.at);
    }
    open_.erase(it);
}

void SamplerPanel::setRecording(bool on) {
    if (on == recording_) return;
    if (on) {
        const Sequence* s = state_->sequence();
        if (!s || !audioTrack()) {
            const QSignalBlocker block(recordButton_);
            recordButton_->setChecked(false);
            return;
        }
        {
            // (MIDI notes that came before the pass are not in it; its notes are numbered from 0)
            std::lock_guard<std::mutex> lock(inboxMutex_);
            inbox_.clear();
            passSeed_ = QRandomGenerator::global()->bounded(1u, 0x7fffffffu);
            noteIndex_ = 0;
        }
        notes_.clear();
        open_.clear();
        engine_->reset();  // (round robin from the start, as the pass will be rendered)
        passFrom_ = program_->position();
        recording_ = true;
        startedPlay_ = !program_->isPlaying();
        if (startedPlay_) program_->play();
    } else {
        drainMidi();  // (the last MIDI notes, still queued for this thread)
        recording_ = false;
        if (startedPlay_ && program_->isPlaying()) program_->pause();
        startedPlay_ = false;
        finishPass();
    }
    {
        const QSignalBlocker block(recordButton_);
        recordButton_->setChecked(recording_);
    }
    emit recordingChanged(recording_);
}

void SamplerPanel::finishPass() {
    const int64_t now = program_->heardSample();
    for (auto& [key, at] : open_)  // (keys still held end now)
        if (at < notes_.size()) notes_[at].second.length = std::max<int64_t>(1, now - notes_[at].second.at);
    open_.clear();
    if (notes_.empty()) return;
    std::vector<SamplerNote> notes;
    for (const auto& [i, n] : notes_) notes.push_back(n);
    notes_.clear();
    const Track* t = audioTrack();
    const QString stem = (t && !t->name.empty() ? QString::fromStdString(t->name) : QStringLiteral("A%1").arg(track_ + 1)) + QStringLiteral(" Sampler Pass");
    const QString file = nextPassFile(passFolder(), stem);
    Id lane = 0;
    const sampler::KitPtr kit = kit_;
    const FrameTime from = passFrom_;
    const uint32_t seed = passSeed_;
    if (state_->apply(tr("Record Sampler Pass"), [&](Project& p, Sequence& s) {
            edit::Result r = sampler::addPass(p, s, {TrackKind::Audio, track_}, kit, notes, seed, file.toStdString(), from);
            if (r.ok) lane = r.created.front();
            return r;
        })) {
        status_->setText(tr("%n note(s) recorded to %1", nullptr, int(notes.size())).arg(QFileInfo(file).fileName()));
        emit passRecorded(lane);
    }
}

bool SamplerPanel::revoice(Id lane) {
    const Track* t = audioTrack();
    if (!t) return false;
    const QString stem = (!t->name.empty() ? QString::fromStdString(t->name) : QStringLiteral("A%1").arg(track_ + 1)) + QStringLiteral(" Sampler Pass");
    const QString file = nextPassFile(passFolder(), stem);
    const sampler::KitPtr kit = kit_;
    return state_->apply(tr("Voice Pass Again"), [&](Project& p, Sequence& s) {
        return sampler::revoicePass(p, s, {TrackKind::Audio, track_}, lane, kit, file.toStdString());
    });
}

// ---- Playing --------------------------------------------------------------------------------------------------

bool SamplerPanel::connectMidi(const QString& input, QString* error) {
    midi_.reset();
    if (input.isEmpty()) return true;
    std::string id;
    for (const MidiPortInfo& p : midiInputs())
        if (QString::fromStdString(p.name) == input) id = p.id;
    if (id.empty()) {
        if (error) *error = tr("No MIDI input %1").arg(input);
        return false;
    }
    std::string why;
    midi_ = openMidi(id, {}, [this](const MidiMessage& m) { received(m); }, &why);
    if (!midi_ && error) *error = tr("Cannot open %1: %2").arg(input, QString::fromStdString(why));
    return midi_ != nullptr;
}

void SamplerPanel::received(const MidiMessage& m) {
    // The MIDI thread: the note sounds at once; the GUI records it with the moment it came.
    if (m.size() < 3) return;
    const int type = m[0] & 0xF0, key = m[1] & 0x7F, velocity = m[2] & 0x7F;
    if (type != 0x90 && type != 0x80) return;
    const bool on = type == 0x90 && velocity > 0;
    std::lock_guard<std::mutex> lock(inboxMutex_);
    Pressed p{key, velocity, on, steadyNs(), 0};
    if (on) {
        p.index = noteIndex_.fetch_add(1);
        if (active_) engine_->noteOn(key, velocity, -1, seedFor(p.index));
    } else if (active_) {
        engine_->noteOff(key);
    }
    inbox_.push_back(p);
    if (!drainPending_) {
        drainPending_ = true;
        QMetaObject::invokeMethod(this, &SamplerPanel::drainMidi, Qt::QueuedConnection);
    }
}

void SamplerPanel::drainMidi() {
    std::vector<Pressed> got;
    {
        std::lock_guard<std::mutex> lock(inboxMutex_);
        got.swap(inbox_);
        drainPending_ = false;
    }
    const qint64 now = steadyNs();
    for (const Pressed& p : got) {
        if (!recording_) continue;
        if (p.on) {
            SamplerNote n;
            n.key = p.key, n.velocity = std::max(1, p.velocity);
            n.at = std::max<int64_t>(0, program_->heardSample(now - p.at));
            auto it = std::upper_bound(notes_.begin(), notes_.end(), p.index, [](uint32_t i, const auto& e) { return i < e.first; });
            for (auto& [k, at] : open_)
                if (at >= size_t(it - notes_.begin())) ++at;
            open_[p.key] = size_t(it - notes_.begin());
            notes_.insert(it, {p.index, n});
        } else if (auto it = open_.find(p.key); it != open_.end()) {
            if (it->second < notes_.size()) {
                SamplerNote& n = notes_[it->second].second;
                n.length = std::max<int64_t>(1, program_->heardSample(now - p.at) - n.at);
            }
            open_.erase(it);
        }
    }
}

int SamplerPanel::keyForComputerKey(int qtKey) {
    static const std::map<int, int> keys = {
        {Qt::Key_Z, 48}, {Qt::Key_S, 49}, {Qt::Key_X, 50}, {Qt::Key_D, 51}, {Qt::Key_C, 52}, {Qt::Key_V, 53}, {Qt::Key_G, 54},
        {Qt::Key_B, 55}, {Qt::Key_H, 56}, {Qt::Key_N, 57}, {Qt::Key_J, 58}, {Qt::Key_M, 59}, {Qt::Key_Comma, 60},
        {Qt::Key_Q, 60}, {Qt::Key_2, 61}, {Qt::Key_W, 62}, {Qt::Key_3, 63}, {Qt::Key_E, 64}, {Qt::Key_R, 65}, {Qt::Key_5, 66},
        {Qt::Key_T, 67}, {Qt::Key_6, 68}, {Qt::Key_Y, 69}, {Qt::Key_7, 70}, {Qt::Key_U, 71}, {Qt::Key_I, 72}, {Qt::Key_9, 73},
        {Qt::Key_O, 74}, {Qt::Key_0, 75}, {Qt::Key_P, 76}};
    const auto it = keys.find(qtKey);
    return it == keys.end() ? -1 : it->second;
}

void SamplerPanel::setComputerKeys(bool on) {
    if (on == computerKeys_) return;
    computerKeys_ = on;
    if (on) qApp->installEventFilter(this);
    else qApp->removeEventFilter(this);
    const QSignalBlocker block(keysBox_);
    keysBox_->setChecked(on);
}

void SamplerPanel::setActive(bool on) {
    if (on == active_) return;
    active_ = on;
    if (!on) engine_->reset();  // (not heard again where it stopped when the panel comes back)
    if (program_) program_->setSampler(on ? engine_ : nullptr);
}

void SamplerPanel::updateActive() { setActive(isVisible() && kit_ && !kit_->pads.empty()); }

void SamplerPanel::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    updateActive();
}

void SamplerPanel::hideEvent(QHideEvent* e) {
    QWidget::hideEvent(e);
    updateActive();
}

bool SamplerPanel::eventFilter(QObject* watched, QEvent* event) {
    // Sounds dropped on the table: on the row's key, or the first free one.
    if (watched == table_->viewport()) {
        if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove) {
            auto* d = static_cast<QDropEvent*>(event);
            if (d->mimeData()->hasUrls() || d->mimeData()->hasFormat(kSoundRangeMime)) {
                d->acceptProposedAction();
                return true;
            }
        } else if (event->type() == QEvent::Drop) {
            auto* d = static_cast<QDropEvent*>(event);
            const int row = table_->rowAt(int(d->position().y()));
            const int key = row >= 0 && table_->item(row, Key) ? table_->item(row, Key)->data(Qt::UserRole).toInt() : selectedKey();
            SoundRange r;
            if (soundRangeFromMime(d->mimeData(), r) && r.rate > 0) {
                addSounds(key, {r.path}, double(r.first) / r.rate, r.count > 0 ? double(r.first + r.count) / r.rate : 0.0);
            } else {
                QStringList files;
                for (const QUrl& u : d->mimeData()->urls())
                    if (u.isLocalFile()) files << u.toLocalFile();
                addSounds(key, files);
            }
            d->acceptProposedAction();
            return true;
        }
        return false;
    }
    // The computer's keys, while they play the sampler (not while typing in a field).
    if (computerKeys_ && isVisible() && !QApplication::activePopupWidget() && (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease || event->type() == QEvent::ShortcutOverride)) {
        auto* k = static_cast<QKeyEvent*>(event);
        const int key = keyForComputerKey(k->key());
        QWidget* focus = QApplication::focusWidget();
        const bool typing = qobject_cast<QLineEdit*>(focus) || qobject_cast<QAbstractSpinBox*>(focus) || qobject_cast<QTextEdit*>(focus) ||
                            qobject_cast<QPlainTextEdit*>(focus) || (focus && focus->inherits("QComboBox") && static_cast<QComboBox*>(focus)->isEditable());
        if (key < 0 || typing || (k->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) || QApplication::activeModalWidget())
            return false;
        if (event->type() == QEvent::ShortcutOverride) {
            k->accept();  // (the key plays, rather than running its shortcut)
            return true;
        }
        if (!k->isAutoRepeat()) {
            if (event->type() == QEvent::KeyPress) noteOn(key, velocityBox_->value());
            else noteOff(key);
        }
        return true;
    }
    return false;
}

}  // namespace montage
