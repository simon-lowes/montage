#include "SpeechDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QPushButton>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <atomic>
#include <memory>

#include "EditorState.h"
#include "ModelPacks.h"
#include "core/Captions.h"
#include "core/EditOps.h"
#include "media/TextToSpeech.h"

namespace montage {

std::vector<SpeechLine> captionLines(const Sequence& s, int captionTrack) {
    std::vector<SpeechLine> lines;
    if (captionTrack < 0 || captionTrack >= int(s.captionTracks.size())) return lines;
    const auto& caps = s.captionTracks[size_t(captionTrack)].captions;
    for (size_t i = 0; i < caps.size(); ++i) {
        SpeechLine l;
        l.text = caps[i].text;
        std::replace(l.text.begin(), l.text.end(), '\n', ' ');
        l.at = caps[i].start;
        // Until the next cue starts (or this one ends, if it is the last).
        l.fit = (i + 1 < caps.size() ? caps[i + 1].start : caps[i].end) - caps[i].start;
        if (!l.text.empty()) lines.push_back(l);
    }
    return lines;
}

std::vector<Id> generateSpeech(EditorState* state, const std::vector<SpeechLine>& lines, const std::string& voice, double speed, int track,
                               QWidget* parent, QString* error) {
    const Sequence* s = state->sequence();
    if (!s || lines.empty()) return {};
    if (!ttsAvailable() || !ensureModelPack(parent, ttsModel(), QObject::tr("Generate Voiceover"),
                                            QObject::tr("Voiceovers from text use Kokoro (Apache-2.0), a speech model that runs on "
                                                        "this computer, with misaki's pronunciation dictionaries.")))
        return {};
    // Beside the project, as recorded takes are.
    const QString project = state->filePath();
    const QString folder = (project.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::MusicLocation) + QStringLiteral("/Montage")
                                              : QFileInfo(project).absolutePath()) +
                           QStringLiteral("/Voiceover");
    QDir().mkpath(folder);
    const QString stem = folder + '/' + QString::fromStdString(s->name) + QStringLiteral(" Speech ");
    std::vector<QString> paths;
    int n = 1;
    for (size_t i = 0; i < lines.size(); ++i) {
        QString path;
        do path = stem + QString::number(n++) + QStringLiteral(".wav");
        while (QFileInfo::exists(path));
        paths.push_back(path);
    }
    const double fps = s->fpsValue();
    QProgressDialog progress(QObject::tr("Speaking…"), QObject::tr("Cancel"), 0, 1000, parent);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(300);
    auto done = std::make_shared<std::atomic<double>>(0.0);
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    QObject::connect(&progress, &QProgressDialog::canceled, &progress, [cancel] { *cancel = true; });
    QTimer tick;
    QObject::connect(&tick, &QTimer::timeout, &progress, [&progress, done] { progress.setValue(int(*done * 1000)); });
    tick.start(100);
    QFutureWatcher<std::string> watcher;
    QEventLoop wait;
    QObject::connect(&watcher, &QFutureWatcher<std::string>::finished, &wait, &QEventLoop::quit);
    watcher.setFuture(QtConcurrent::run([lines, paths, voice, speed, fps, done, cancel]() -> std::string {
        for (size_t i = 0; i < lines.size(); ++i) {
            if (*cancel) return "Cancelled";
            std::vector<float> audio;
            std::string err;
            if (!synthesizeSpeech(lines[i].text, voice, speed, audio, &err, {}, cancel.get())) return err;
            // Too long for its room: said a little faster (up to 1.6 times).
            const double room = lines[i].fit > 0 ? lines[i].fit / fps : 0, said = double(audio.size()) / kTtsSampleRate;
            if (room > 0 && said > room * 1.02) {
                const double faster = std::min(1.6, speed * said / room);
                if (faster > speed * 1.02 && !synthesizeSpeech(lines[i].text, voice, faster, audio, &err, {}, cancel.get())) return err;
            }
            if (!writeSpeechWav(paths[i].toStdString(), audio, &err)) return err;
            *done = double(i + 1) / lines.size();
        }
        return {};
    }));
    if (!watcher.isFinished()) wait.exec();
    tick.stop();
    QObject::disconnect(&progress, &QProgressDialog::canceled, nullptr, nullptr);  // closing a progress dialog emits canceled()
    progress.close();
    const std::string err = watcher.result();
    if (!err.empty()) {
        for (const QString& p : paths) QFile::remove(p);
        if (error) *error = QString::fromStdString(err);
        if (err != "Cancelled") state->message(QString::fromStdString(err), 6000);
        return {};
    }
    QStringList files;
    for (const QString& p : paths) files << p;
    const std::vector<Id> media = state->importFiles(files, nullptr, QStringLiteral("Voiceover"));
    if (media.size() != lines.size()) return {};
    std::vector<Id> created;
    state->apply(QObject::tr("Generate Voiceover"), [&](Project& p, Sequence& sq) {
        edit::Result all;
        if (sq.audioTracks.empty() || track < 0) edit::addTrack(p, sq, TrackKind::Audio);
        const TrackRef a{TrackKind::Audio, track < 0 ? int(sq.audioTracks.size()) - 1 : std::clamp(track, 0, int(sq.audioTracks.size()) - 1)};
        for (size_t i = 0; i < lines.size(); ++i) {
            edit::Result r = edit::placeMedia(p, sq, media[i], lines[i].at, 0, -1, {TrackKind::Video, 0}, a, false);
            if (!r.ok) return r;
            created.insert(created.end(), r.created.begin(), r.created.end());
        }
        all.ok = true;
        all.created = created;
        return all;
    });
    return created;
}

SpeechDialog::SpeechDialog(EditorState* state, QWidget* parent) : QDialog(parent), state_(state) {
    setWindowTitle(tr("Generate Voiceover"));
    auto* lay = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    source_ = new QComboBox(this);
    source_->setObjectName(QStringLiteral("speechSource"));
    source_->addItem(tr("Text below, at the playhead"), -1);
    if (const Sequence* s = state_->sequence())
        for (int i = 0; i < int(s->captionTracks.size()); ++i)
            source_->addItem(tr("Captions: %1 (each cue at its time)").arg(QString::fromStdString(s->captionTracks[size_t(i)].name)), i);
    form->addRow(tr("Speak:"), source_);
    text_ = new QPlainTextEdit(this);
    text_->setObjectName(QStringLiteral("speechText"));
    text_->setPlaceholderText(tr("Type or paste what the voice should say."));
    text_->setMinimumSize(420, 140);
    form->addRow(tr("Text:"), text_);
    voice_ = new QComboBox(this);
    voice_->setObjectName(QStringLiteral("speechVoice"));
    for (const TtsVoice& v : ttsVoices()) voice_->addItem(QString::fromStdString(v.name), QString::fromStdString(v.id));
    form->addRow(tr("Voice:"), voice_);
    speed_ = new QDoubleSpinBox(this);
    speed_->setObjectName(QStringLiteral("speechSpeed"));
    speed_->setRange(0.5, 2.0);
    speed_->setSingleStep(0.05);
    speed_->setValue(1.0);
    speed_->setSuffix(QStringLiteral("x"));
    form->addRow(tr("Speed:"), speed_);
    track_ = new QComboBox(this);
    track_->setObjectName(QStringLiteral("speechTrack"));
    if (const Sequence* s = state_->sequence())
        for (int i = 0; i < int(s->audioTracks.size()); ++i)
            track_->addItem(s->audioTracks[size_t(i)].name.empty() ? tr("A%1").arg(i + 1) : QString::fromStdString(s->audioTracks[size_t(i)].name), i);
    track_->setCurrentIndex(std::clamp(state_->targetAudioTrack(), 0, std::max(0, track_->count() - 1)));
    form->addRow(tr("Track:"), track_);
    lay->addLayout(form);
    status_ = new QLabel(this);
    status_->setWordWrap(true);
    status_->setText(ttsAvailable() ? tr("English, in American or British voices. Runs on this computer.")
                                    : tr("This build of Montage cannot speak (no ONNX Runtime)."));
    lay->addWidget(status_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    QPushButton* go = buttons->addButton(tr("Generate"), QDialogButtonBox::AcceptRole);
    go->setObjectName(QStringLiteral("speechGenerate"));
    go->setEnabled(ttsAvailable());
    connect(go, &QPushButton::clicked, this, [this] {
        if (!generate().empty()) accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(buttons);
    connect(source_, &QComboBox::currentIndexChanged, this, &SpeechDialog::sourceChanged);
}

void SpeechDialog::sourceChanged() { text_->setEnabled(source_->currentData().toInt() < 0); }

std::vector<Id> SpeechDialog::generate() {
    const Sequence* s = state_->sequence();
    if (!s) return {};
    std::vector<SpeechLine> lines;
    const int captions = source_->currentData().toInt();
    if (captions >= 0) {
        lines = captionLines(*s, captions);
    } else if (!text_->toPlainText().trimmed().isEmpty()) {
        lines.push_back({text_->toPlainText().trimmed().toStdString(), state_->playhead(), -1});
    }
    if (lines.empty()) {
        status_->setText(tr("Nothing to say."));
        return {};
    }
    QString err;
    const std::vector<Id> clips = generateSpeech(state_, lines, voice_->currentData().toString().toStdString(), speed_->value(),
                                                 track_->currentData().toInt(), this, &err);
    if (clips.empty() && !err.isEmpty() && err != "Cancelled") status_->setText(err);
    return clips;
}

}  // namespace montage
