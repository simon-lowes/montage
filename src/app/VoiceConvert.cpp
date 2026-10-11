#include "VoiceConvert.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QLabel>
#include <QProgressDialog>
#include <QPushButton>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <algorithm>
#include <atomic>
#include <memory>

#include "EditorState.h"
#include "ModelPacks.h"
#include "core/Adr.h"
#include "core/EditOps.h"
#include "core/History.h"
#include "media/VoiceClone.h"

namespace montage {

Id voiceConversionClip(const Project& p, const Sequence& s, const std::vector<Id>& selection) {
    auto sound = [&](Id id) {
        const auto at = edit::locate(s, id);
        const Clip* c = at && at->track.kind == TrackKind::Audio ? edit::clipById(s, id) : nullptr;
        const MediaItem* m = c && c->mediaId ? p.findMedia(c->mediaId) : nullptr;
        return m && m->hasAudio;
    };
    for (const Id id : selection)
        if (sound(id)) return id;
    for (const Id id : selection)
        if (const Clip* c = edit::clipById(s, id))
            for (const Id l : edit::linkedClips(s, id))
                if (const Clip* k = edit::clipById(s, l); k && k->mediaId == c->mediaId && sound(l)) return l;
    return 0;
}

std::string convertedVoiceNote(const std::string& speechFrom, const ClonedVoice& voice) {
    return QObject::tr("Speech from %1 converted to a voice cloned from %2 with Chatterbox in Montage, %3; the speaker's consent "
                       "was confirmed by the editor.")
        .arg(QString::fromStdString(speechFrom), QString::fromStdString(voice.from.empty() ? std::string("a recording") : voice.from),
             QDateTime::currentDateTime().toString(Qt::ISODate))
        .toStdString();
}

bool convertClipVoice(EditorState* state, Id clip, const ClonedVoice& voice, QWidget* parent, QString* error, double handles) {
    auto fail = [&](const QString& why) {
        if (error) *error = why;
        return false;
    };
    const Sequence* s = state->sequence();
    if (!s) return fail(QObject::tr("No sequence"));
    const auto at = edit::locate(*s, clip);
    const Clip* c = edit::clipById(*s, clip);
    double from = 0, to = 0;
    if (!at || at->track.kind != TrackKind::Audio || !edit::voiceConversionRange(state->project(), *s, clip, handles, from, to))
        return fail(QObject::tr("Select a clip with speech on an audio track to convert"));
    if (voice.sample.empty()) return fail(QObject::tr("No voice to convert to"));
    if (!ensureVoiceCloneModel(parent)) return fail(QObject::tr("Cancelled"));
    const MediaItem* m = state->project().findMedia(c->mediaId);
    const std::string path = m->path, name = c->name.empty() ? m->name : c->name;
    const std::vector<int> channels = c->channels;
    const int rate = s->sampleRate > 0 ? s->sampleRate : 48000;
    const std::string note = convertedVoiceNote(name, voice);
    // Beside the project, as generated speech is.
    const QString project = state->filePath();
    const QString folder = (project.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::MusicLocation) + QStringLiteral("/Montage")
                                              : QFileInfo(project).absolutePath()) +
                           QStringLiteral("/Voiceover");
    QDir().mkpath(folder);
    QString out;
    int n = 1;
    do out = folder + '/' + QString::fromStdString(name) + QStringLiteral(" Converted Voice ") + QString::number(n++) + QStringLiteral(".wav");
    while (QFileInfo::exists(out));

    QProgressDialog progress(QObject::tr("Converting the voice…"), QObject::tr("Cancel"), 0, 1000, parent);
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
    const std::vector<float> sample = voice.sample;
    const std::string file = out.toStdString();
    watcher.setFuture(QtConcurrent::run([=]() -> std::string {
        std::vector<float> speech, converted;
        std::string err;
        if (!voiceSoundFrom(path, from, to, speech, &err, channels, rate)) return err;
        if (*cancel) return "Cancelled";
        if (!convertVoice(speech, kCloneSampleRate, sample, kCloneSampleRate, converted, &err, [done](double f) { *done = f; }, cancel.get()))
            return *cancel ? std::string("Cancelled") : err;
        if (!writeClonedSpeechWav(file, converted, note, &err)) return err;
        return {};
    }));
    if (!watcher.isFinished()) wait.exec();
    tick.stop();
    QObject::disconnect(&progress, &QProgressDialog::canceled, nullptr, nullptr);  // closing a progress dialog emits canceled()
    progress.close();
    const std::string err = watcher.result();
    if (!err.empty()) {
        QFile::remove(out);
        if (err != "Cancelled") state->message(QString::fromStdString(err), 6000);
        return fail(QString::fromStdString(err));
    }
    const std::vector<Id> media = state->importFiles(QStringList{out}, nullptr, QStringLiteral("Voiceover"));
    if (media.size() != 1) return fail(QObject::tr("The converted sound could not be imported"));
    // Noted on the media as it comes in (the import's own undo step), so the bin's copy says what it is whatever follows.
    state->amend([&](Project& p, Sequence&) {
        if (MediaItem* made = p.findMedia(media[0])) {
            made->metadata["comment"] = note;
            if (std::find(made->keywords.begin(), made->keywords.end(), "AI voice") == made->keywords.end()) made->keywords.push_back("AI voice");
        }
        return true;
    });
    std::string why;
    const bool ok = state->apply(QObject::tr("Convert Voice"), [&](Project& p, Sequence& sq) {
        const edit::Result r = edit::addConvertedTake(p, sq, clip, media[0], from);
        why = r.error;
        return r;
    });
    if (!ok) return fail(QString::fromStdString(why));
    state->message(QObject::tr("Converted %1 to %2's voice: the original is the clip's previous take")
                       .arg(QString::fromStdString(name), QString::fromStdString(voice.from)),
                   6000);
    return true;
}

VoiceConvertDialog::VoiceConvertDialog(EditorState* state, Id clip, Id voiceClip, QWidget* parent)
    : QDialog(parent), state_(state), clip_(clip) {
    setWindowTitle(tr("Convert Voice"));
    auto* lay = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    const Sequence* s = state_->sequence();
    const Clip* c = s ? edit::clipById(*s, clip) : nullptr;
    auto* what = new QLabel(c ? QString::fromStdString(c->name) : tr("Select a clip with speech first"), this);
    what->setObjectName(QStringLiteral("convertClip"));
    form->addRow(tr("Convert:"), what);
    // Every other clip with sound, those with speech (a transcript) first, then a recording.
    voice_ = new QComboBox(this);
    voice_->setObjectName(QStringLiteral("convertVoice"));
    if (s) {
        const Project& p = state_->project();
        for (const bool spoken : {true, false})
            for (size_t t = 0; t < s->audioTracks.size(); ++t)
                for (const Clip& k : s->audioTracks[t].clips) {
                    const MediaItem* m = k.mediaId && k.id != clip ? p.findMedia(k.mediaId) : nullptr;
                    if (!m || !m->hasAudio || bool(m->transcript) != spoken || voice_->count() >= 200) continue;
                    const QString track = s->audioTracks[t].name.empty() ? tr("A%1").arg(t + 1) : QString::fromStdString(s->audioTracks[t].name);
                    voice_->addItem(tr("%1 (%2, %3)").arg(QString::fromStdString(k.name.empty() ? m->name : k.name), track,
                                                          QString::fromStdString(formatTimecode(k.start, s->fps))),
                                    QVariant::fromValue(qint64(k.id)));
                }
    }
    voice_->addItem(tr("A recording…"), QVariant::fromValue(qint64(0)));
    if (const int i = voiceClip ? voice_->findData(QVariant::fromValue(qint64(voiceClip))) : -1; i >= 0) voice_->setCurrentIndex(i);
    form->addRow(tr("Into the voice of:"), voice_);
    consent_ = new QCheckBox(tr("I have this person's consent to use their voice (or it is my own)"), this);
    consent_->setObjectName(QStringLiteral("convertConsent"));
    form->addRow(QString(), consent_);
    lay->addLayout(form);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("convertStatus"));
    status_->setWordWrap(true);
    lay->addWidget(status_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    go_ = buttons->addButton(tr("Convert"), QDialogButtonBox::AcceptRole);
    go_->setObjectName(QStringLiteral("convertGo"));
    connect(go_, &QPushButton::clicked, this, [this] {
        if (convert()) accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(buttons);
    connect(voice_, &QComboBox::activated, this, [this] {
        // Picking "A recording…" asks for the file each time.
        if (voice_->currentData().toLongLong() == 0) {
            const QString file = QFileDialog::getOpenFileName(this, tr("A Voice from a Recording"), recording_,
                                                              tr("Sound or video (*.wav *.mp3 *.m4a *.aac *.flac *.ogg *.opus *.aif *.aiff "
                                                                 "*.mov *.mp4 *.mkv *.mxf);;All files (*)"));
            if (!file.isEmpty()) recording_ = file;
        }
        voiceChanged();
    });
    connect(consent_, &QCheckBox::toggled, this, &VoiceConvertDialog::voiceChanged);
    voiceChanged();
}

void VoiceConvertDialog::setRecording(const QString& path) {
    recording_ = path;
    voice_->setCurrentIndex(voice_->findData(QVariant::fromValue(qint64(0))));
    voiceChanged();
}

void VoiceConvertDialog::voiceChanged() {
    const Sequence* s = state_->sequence();
    bool ready = voiceCloneAvailable() && consent_->isChecked() && s && edit::clipById(*s, clip_);
    if (voice_->currentData().toLongLong() == 0) ready = ready && !recording_.isEmpty();
    if (!voiceCloneAvailable())
        status_->setText(tr("This build of Montage cannot convert voices (no ONNX Runtime)."));
    else
        status_->setText(voice_->currentData().toLongLong() == 0 && !recording_.isEmpty()
                             ? tr("The voice in %1.").arg(QFileInfo(recording_).fileName())
                             : tr("The clip's words and timing in the other person's voice (5 to 10 clean seconds of them speaking "
                                  "is best). Runs on this computer. The result becomes a take of the clip, so the original is a "
                                  "take away, and its file notes what it is."));
    go_->setEnabled(ready);
}

bool VoiceConvertDialog::convert() {
    const Sequence* s = state_->sequence();
    if (!s) return false;
    if (!consent_->isChecked()) {
        status_->setText(tr("Confirm that you have the speaker's consent to use their voice."));
        return false;
    }
    ClonedVoice voice;
    QString err;
    const Id from = Id(voice_->currentData().toLongLong());
    const bool got = from ? clipVoice(state_->project(), *s, from, voice, &err)
                          : !recording_.isEmpty() && fileVoice(recording_, voice, &err);
    if (!got) {
        status_->setText(err.isEmpty() ? tr("Pick a recording of the voice") : err);
        return false;
    }
    if (!convertClipVoice(state_, clip_, voice, this, &err)) {
        if (err != QLatin1String("Cancelled")) status_->setText(err);
        return false;
    }
    return true;
}

}  // namespace montage
