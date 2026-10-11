#include "SpeechDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include <QFileDialog>
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
#include "media/VoiceClone.h"

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

bool clipVoice(const Project& p, const Sequence& s, Id clip, ClonedVoice& voice, QString* error, double seconds) {
    const auto at = edit::locate(s, clip);
    const Clip* c = at ? edit::clipById(const_cast<Sequence&>(s), clip) : nullptr;
    const MediaItem* m = c && c->mediaId ? p.findMedia(c->mediaId) : nullptr;
    if (!m || !m->hasAudio || m->path.empty()) {
        if (error) *error = QObject::tr("That clip has no sound to take a voice from");
        return false;
    }
    // The source the clip plays (either way round), at most `seconds` of it.
    const double fps = s.fpsValue(), a = c->sourceFrameAt(c->start) / fps, b = c->sourceFrameAt(c->end() - 1) / fps;
    const double from = std::min(a, b), to = std::min(std::max(a, b) + 1 / fps, from + seconds);
    std::string err;
    if (!voiceSampleFrom(m->path, from, to, voice.sample, &err, c->channels, s.sampleRate > 0 ? s.sampleRate : 48000)) {
        if (error) *error = QString::fromStdString(err);
        return false;
    }
    voice.from = (c->name.empty() ? m->name : c->name) + " (" + QFileInfo(QString::fromStdString(m->path)).fileName().toStdString() + ")";
    return true;
}

bool voiceAt(const Project& p, const Sequence& s, FrameTime at, ClonedVoice& voice, QString* error) {
    for (const Track& t : s.audioTracks) {
        if (t.muted) continue;
        for (const Clip& c : t.clips) {
            if (!c.contains(at) || !c.mediaId || !c.enabled) continue;
            const MediaItem* m = p.findMedia(c.mediaId);
            if (m && m->hasAudio) return clipVoice(p, s, c.id, voice, error);
        }
    }
    if (error) *error = QObject::tr("No clip with sound is heard there to take a voice from");
    return false;
}

bool fileVoice(const QString& path, ClonedVoice& voice, QString* error) {
    std::string err;
    if (!voiceSampleFrom(path.toStdString(), 0, 30, voice.sample, &err)) {
        if (error) *error = QString::fromStdString(err);
        return false;
    }
    voice.from = QFileInfo(path).fileName().toStdString();
    return true;
}

std::string clonedVoiceNote(const ClonedVoice& voice) {
    return QObject::tr("Generated speech (%1) in a voice cloned from %2 with Chatterbox Multilingual in Montage, %3; the speaker's "
                       "consent was confirmed by the editor.")
        .arg(QString::fromUtf8(findCloneLanguage(voice.language) ? findCloneLanguage(voice.language)->name : voice.language.c_str()),
             QString::fromStdString(voice.from.empty() ? std::string("a recording") : voice.from),
             QDateTime::currentDateTime().toString(Qt::ISODate))
        .toStdString();
}

std::vector<Id> generateSpeech(EditorState* state, const std::vector<SpeechLine>& lines, const std::string& voice, double speed, int track,
                               QWidget* parent, QString* error, const ClonedVoice* clone) {
    const Sequence* s = state->sequence();
    if (!s || lines.empty()) return {};
    if (clone) {
        if (clone->sample.empty() || !findCloneLanguage(clone->language)) return {};
        if (!ensureVoiceCloneModel(parent)) return {};
    } else if (!ttsAvailable() || !ensureModelPack(parent, ttsModel(), QObject::tr("Generate Voiceover"),
                                                   QObject::tr("Voiceovers from text use Kokoro (Apache-2.0), a speech model that runs on "
                                                               "this computer, with misaki's pronunciation dictionaries."))) {
        return {};
    }
    const std::shared_ptr<const ClonedVoice> cloned = clone ? std::make_shared<const ClonedVoice>(*clone) : nullptr;
    const std::string note = clone ? clonedVoiceNote(*clone) : std::string();
    // Beside the project, as recorded takes are.
    const QString project = state->filePath();
    const QString folder = (project.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::MusicLocation) + QStringLiteral("/Montage")
                                              : QFileInfo(project).absolutePath()) +
                           QStringLiteral("/Voiceover");
    QDir().mkpath(folder);
    const QString stem = folder + '/' + QString::fromStdString(s->name) + (clone ? QStringLiteral(" Cloned Speech ") : QStringLiteral(" Speech "));
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
    watcher.setFuture(QtConcurrent::run([lines, paths, voice, speed, fps, done, cancel, cloned, note]() -> std::string {
        for (size_t i = 0; i < lines.size(); ++i) {
            if (*cancel) return "Cancelled";
            std::vector<float> audio;
            std::string err;
            if (cloned) {
                // Time-stretched to its speed, and to fit its room (Chatterbox has no speed of its own).
                CloneOptions o;
                o.language = cloned->language;
                if (!cloneSpeechToFit(lines[i].text, cloned->sample, kCloneSampleRate, o, speed, lines[i].fit > 0 ? lines[i].fit / fps : 0,
                                      false, audio, &err, cancel.get()) ||
                    !writeClonedSpeechWav(paths[i].toStdString(), audio, note, &err))
                    return err;
                *done = double(i + 1) / lines.size();
                continue;
            }
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
        // Generated, and from whose voice: in the bin as in the files.
        if (clone)
            for (const Id id : media)
                if (MediaItem* m = p.findMedia(id)) {
                    m->metadata["comment"] = note;
                    if (std::find(m->keywords.begin(), m->keywords.end(), "AI voice") == m->keywords.end()) m->keywords.push_back("AI voice");
                }
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
    // A voice cloned from the selected clip's sound, or from a recording.
    voice_->insertSeparator(voice_->count());
    voice_->addItem(tr("Clone the voice in the selected clip"), QStringLiteral("clone:clip"));
    voice_->addItem(tr("Clone a voice from a recording…"), QStringLiteral("clone:file"));
    form->addRow(tr("Voice:"), voice_);
    language_ = new QComboBox(this);
    language_->setObjectName(QStringLiteral("speechLanguage"));
    for (const CloneLanguage& l : voiceCloneLanguages()) language_->addItem(QString::fromUtf8(l.name), QString::fromUtf8(l.code));
    form->addRow(tr("Language:"), language_);
    sample_ = new QLabel(this);
    sample_->setObjectName(QStringLiteral("speechSample"));
    sample_->setWordWrap(true);
    form->addRow(tr("Voice from:"), sample_);
    consent_ = new QCheckBox(tr("I have this person's consent to clone their voice (or it is my own)"), this);
    consent_->setObjectName(QStringLiteral("speechConsent"));
    form->addRow(QString(), consent_);
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
    status_->setObjectName(QStringLiteral("speechStatus"));
    status_->setWordWrap(true);
    lay->addWidget(status_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    go_ = buttons->addButton(tr("Generate"), QDialogButtonBox::AcceptRole);
    go_->setObjectName(QStringLiteral("speechGenerate"));
    connect(go_, &QPushButton::clicked, this, [this] {
        if (!generate().empty()) accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(buttons);
    connect(source_, &QComboBox::currentIndexChanged, this, &SpeechDialog::sourceChanged);
    connect(voice_, &QComboBox::activated, this, [this] {
        // Picking "from a recording" asks for the file each time.
        if (voice_->currentData().toString() == QLatin1String("clone:file")) {
            const QString file = QFileDialog::getOpenFileName(this, tr("Clone a Voice from a Recording"), sampleFile_,
                                                              tr("Sound or video (*.wav *.mp3 *.m4a *.aac *.flac *.ogg *.opus *.aif *.aiff "
                                                                 "*.mov *.mp4 *.mkv *.mxf);;All files (*)"));
            if (!file.isEmpty()) sampleFile_ = file;
        }
        voiceChanged();
    });
    connect(consent_, &QCheckBox::toggled, this, &SpeechDialog::voiceChanged);
    voiceChanged();
}

void SpeechDialog::sourceChanged() { text_->setEnabled(source_->currentData().toInt() < 0); }

bool SpeechDialog::cloning() const { return voice_->currentData().toString().startsWith(QLatin1String("clone:")); }

void SpeechDialog::voiceChanged() {
    const bool clone = cloning();
    for (QWidget* w : {static_cast<QWidget*>(language_), static_cast<QWidget*>(sample_), static_cast<QWidget*>(consent_)}) {
        w->setVisible(clone);
        if (auto* form = qobject_cast<QFormLayout*>(layout()->itemAt(0)->layout()))
            if (QWidget* label = form->labelForField(w)) label->setVisible(clone);
    }
    bool ready = clone ? voiceCloneAvailable() && consent_->isChecked() : ttsAvailable();
    if (clone) {
        QString from;
        if (voice_->currentData().toString() == QLatin1String("clone:file")) {
            from = sampleFile_.isEmpty() ? tr("No recording picked") : QFileInfo(sampleFile_).fileName();
            ready = ready && !sampleFile_.isEmpty();
        } else {
            const Clip* c = nullptr;
            if (const Sequence* s = state_->sequence())
                for (const Id id : state_->selectedClips())
                    if (const auto at = edit::locate(*s, id); at && at->track.kind == TrackKind::Audio)
                        if (const Clip* k = edit::clipById(const_cast<Sequence&>(*s), id); k && k->mediaId) {
                            c = k;
                            break;
                        }
            from = c ? QString::fromStdString(c->name) : tr("Select a clip with the voice in the timeline first");
            ready = ready && c;
        }
        sample_->setText(from);
        status_->setText(voiceCloneAvailable() ? tr("Speech in the voice of a few seconds of someone speaking (5 to 10 clean seconds is best), "
                                                    "in 23 languages. Runs on this computer. Each file made notes that it is "
                                                    "generated, and from whose voice.")
                                               : tr("This build of Montage cannot clone voices (no ONNX Runtime)."));
    } else {
        status_->setText(ttsAvailable() ? tr("English, in American or British voices. Runs on this computer.")
                                        : tr("This build of Montage cannot speak (no ONNX Runtime)."));
    }
    speed_->setToolTip(clone ? tr("A cloned voice is sped up or slowed down by time-stretching, its pitch kept") : QString());
    go_->setEnabled(ready);
}

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
    ClonedVoice clone;
    if (cloning()) {
        if (!consent_->isChecked()) {
            status_->setText(tr("Confirm that you have the speaker's consent to clone their voice."));
            return {};
        }
        bool got = false;
        if (voice_->currentData().toString() == QLatin1String("clone:file")) {
            got = fileVoice(sampleFile_, clone, &err);
        } else {
            for (const Id id : state_->selectedClips())
                if (const auto at = edit::locate(*s, id); at && at->track.kind == TrackKind::Audio && clipVoice(state_->project(), *s, id, clone, &err)) {
                    got = true;
                    break;
                }
            if (!got && err.isEmpty()) err = tr("Select a clip with the voice in the timeline first");
        }
        if (!got) {
            status_->setText(err);
            return {};
        }
        clone.language = language_->currentData().toString().toStdString();
    }
    const std::vector<Id> clips = generateSpeech(state_, lines, voice_->currentData().toString().toStdString(), speed_->value(),
                                                 track_->currentData().toInt(), this, &err, cloning() ? &clone : nullptr);
    if (clips.empty() && !err.isEmpty() && err != "Cancelled") status_->setText(err);
    return clips;
}

}  // namespace montage
