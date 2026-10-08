#include "TranscribeDialog.h"
#include "Settings.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QProgressDialog>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <atomic>
#include <memory>

#include "EditorState.h"
#include "ModelPacks.h"
#include "media/Diarizer.h"

namespace montage {

// ---- Model download ---------------------------------------------------------

ModelDownload::ModelDownload(QObject* parent) : QObject(parent), net_(new QNetworkAccessManager(this)) {}

ModelDownload::~ModelDownload() {
    if (reply_) {
        reply_->disconnect(this);
        reply_->abort();
        reply_->deleteLater();
    }
    if (file_ && file_->isOpen()) {
        file_->close();
        file_->remove();
    }
}

void ModelDownload::start(const std::string& model) {
    if (reply_) return;
    const QString dir = QString::fromStdString(whisperModelsDirectory());
    target_ = dir + "/ggml-" + QString::fromStdString(model) + ".bin";
    delete file_;
    file_ = new QFile(target_ + ".part", this);
    if (!QDir().mkpath(dir) || !file_->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        const QString err = tr("Cannot write to %1").arg(QDir::toNativeSeparators(dir));
        QMetaObject::invokeMethod(this, [this, err] { done(false, err); }, Qt::QueuedConnection);
        return;
    }
    QNetworkRequest req(QUrl(QString::fromStdString(whisperModelUrl(model))));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Montage/" MONTAGE_VERSION));
    reply_ = net_->get(req);
    connect(reply_, &QNetworkReply::readyRead, this, [this] {
        if (reply_ && file_) file_->write(reply_->readAll());
    });
    connect(reply_, &QNetworkReply::downloadProgress, this, &ModelDownload::progress);
    connect(reply_, &QNetworkReply::finished, this, [this] {
        QNetworkReply* r = reply_;
        reply_ = nullptr;
        r->deleteLater();
        file_->write(r->readAll());
        file_->close();
        const QString part = file_->fileName();
        const bool aborted = r->error() == QNetworkReply::OperationCanceledError;
        const QVariant status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        QString err;
        if (aborted) {
            // Cancelled: no message.
        } else if (r->error() != QNetworkReply::NoError) {
            err = tr("The download failed: %1").arg(r->errorString());
        } else if (status.isValid() && status.toInt() >= 400) {
            err = tr("The download failed (HTTP %1)").arg(status.toInt());
        } else if (!isWhisperModelFile(part.toStdString())) {
            err = tr("The downloaded file is not a speech model");
        } else {
            QFile::remove(target_);
            if (!QFile::rename(part, target_)) err = tr("Cannot save %1").arg(QDir::toNativeSeparators(target_));
        }
        if (aborted || !err.isEmpty()) QFile::remove(part);
        done(!aborted && err.isEmpty(), err);
    });
}

void ModelDownload::abort() {
    if (reply_) reply_->abort();
}

void ModelDownload::done(bool ok, const QString& error) { emit finished(ok, error); }

// ---- Options dialog ---------------------------------------------------------

namespace {

QString megabytes(int64_t bytes) { return QLocale().toString(double(bytes) / 1e6, 'f', 0) + QStringLiteral(" MB"); }

const WhisperModel* modelNamed(const std::string& name) {
    for (const auto& m : whisperModels())
        if (m.name == name) return &m;
    return nullptr;
}

}  // namespace

TranscribeDialog::TranscribeDialog(int mediaCount, QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Transcribe"));
    auto* lay = new QVBoxLayout(this);
    auto* intro = new QLabel(tr("Turn the speech in %n clip(s) into a word-timed transcript, for captions and search. "
                                "It runs on this computer; nothing is uploaded.",
                                "", mediaCount),
                             this);
    intro->setWordWrap(true);
    lay->addWidget(intro);
    auto* form = new QFormLayout;
    model_ = new QComboBox(this);
    QSettings settings = appSettings();
    const QString english = QLocale::system().language() == QLocale::English ? "base.en" : "base";
    const QString chosen = settings.value("transcribe/model", english).toString();
    for (const auto& m : whisperModels()) {
        const bool have = !whisperModelPath(m.name).empty();
        model_->addItem(have ? tr("%1 (downloaded)").arg(QString::fromStdString(m.label))
                             : tr("%1, %2 download").arg(QString::fromStdString(m.label), megabytes(m.bytes)),
                        QString::fromStdString(m.name));
        if (QString::fromStdString(m.name) == chosen) model_->setCurrentIndex(model_->count() - 1);
    }
    form->addRow(tr("Model:"), model_);
    language_ = new QComboBox(this);
    language_->addItem(tr("Detect automatically"), "auto");
    static const char* codes[] = {"en", "es", "fr", "de", "it", "pt", "nl", "pl", "sv", "da", "no", "fi", "cs", "el",
                                  "tr", "ru", "uk", "ar", "he", "hi", "id", "vi", "th", "ja", "ko", "zh"};
    for (const char* c : codes)
        language_->addItem(QLocale::languageToString(QLocale(QString::fromLatin1(c)).language()), QString::fromLatin1(c));
    const int lang = language_->findData(settings.value("transcribe/language", "auto"));
    language_->setCurrentIndex(std::max(0, lang));
    form->addRow(tr("Language:"), language_);
    translate_ = new QCheckBox(tr("Translate to English"), this);
    form->addRow(QString(), translate_);
    // Who speaks: for interviews, podcasts and multicam.
    auto* who = new QWidget(this);
    auto* wh = new QHBoxLayout(who);
    wh->setContentsMargins(0, 0, 0, 0);
    speakers_ = new QCheckBox(tr("Label speakers"), who);
    speakers_->setObjectName(QStringLiteral("labelSpeakers"));
    speakerCount_ = new QComboBox(who);
    speakerCount_->setObjectName(QStringLiteral("speakerCount"));
    speakerCount_->addItem(tr("Any number of people"), 0);
    for (int n = 1; n <= 8; ++n) speakerCount_->addItem(tr("%n person(s)", "", n), n);
    wh->addWidget(speakers_);
    wh->addWidget(speakerCount_, 1);
    form->addRow(QString(), who);
    if (!diarizerAvailable()) {
        speakers_->setEnabled(false);
        speakers_->setToolTip(tr("This build of Montage was built without ONNX Runtime"));
    } else {
        speakers_->setChecked(settings.value("transcribe/speakers", false).toBool());
        speakerCount_->setCurrentIndex(std::max(0, speakerCount_->findData(settings.value("transcribe/speakerCount", 0))));
    }
    speakerCount_->setEnabled(speakers_->isChecked());
    connect(speakers_, &QCheckBox::toggled, speakerCount_, &QWidget::setEnabled);
    connect(speakers_, &QCheckBox::toggled, this, &TranscribeDialog::updateState);
    lay->addLayout(form);
    note_ = new QLabel(this);
    note_->setWordWrap(true);
    note_->setStyleSheet(QStringLiteral("color: palette(mid);"));
    lay->addWidget(note_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Transcribe"));
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        QSettings s = appSettings();
        s.setValue("transcribe/model", model_->currentData());
        s.setValue("transcribe/language", language_->currentData());
        s.setValue("transcribe/speakers", speakers_->isChecked());
        s.setValue("transcribe/speakerCount", speakerCount_->currentData());
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(buttons);
    connect(model_, &QComboBox::currentIndexChanged, this, &TranscribeDialog::updateState);
    updateState();
}

void TranscribeDialog::updateState() {
    const WhisperModel* m = modelNamed(model_->currentData().toString().toStdString());
    const bool english = m && m->englishOnly;
    language_->setEnabled(!english);
    translate_->setEnabled(!english);
    if (english) translate_->setChecked(false);
    QString note;
    if (m && whisperModelPath(m->name).empty())
        note = tr("The model (%1) is downloaded once and kept for next time.").arg(megabytes(m->bytes));
    else
        note = english ? tr("English-only models are faster and a little more accurate for English.")
                       : tr("Larger models are more accurate but slower.");
    if (speakers_->isChecked() && !speakerModel().installed())
        note += QStringLiteral(" ") + tr("Labelling speakers needs a one-time %1 download.").arg(megabytes(speakerModel().bytes()));
    note_->setText(note);
}

TranscribeOptions TranscribeDialog::options() const {
    TranscribeOptions o;
    o.model = model_->currentData().toString().toStdString();
    o.language = language_->isEnabled() ? language_->currentData().toString().toStdString() : "en";
    o.translate = translate_->isEnabled() && translate_->isChecked();
    o.speakers = speakers_->isEnabled() && speakers_->isChecked();
    o.speakerCount = speakerCount_->currentData().toInt();
    return o;
}

// ---- Running ----------------------------------------------------------------

namespace {

// Closes a progress dialog and gives the focus back to the window it covered.
void closeProgress(QProgressDialog* dlg, QWidget* parent) {
    // Its update timer must not show it again between closing and deletion.
    for (QTimer* t : dlg->findChildren<QTimer*>()) t->stop();
    dlg->close();
    dlg->deleteLater();
    if (parent) parent->window()->activateWindow();
}

struct TranscribeJob {
    Id id;
    std::string path;
    QString name;
};

struct TranscribeResults {
    std::vector<std::pair<Id, std::shared_ptr<const Transcript>>> done;
    QStringList errors;
    bool cancelled = false;
};

void runTranscription(EditorState* state, const std::vector<TranscribeJob>& jobs, const TranscribeOptions& options,
                      QWidget* parent) {
    auto* dlg = new QProgressDialog(QObject::tr("Transcribing…"), QObject::tr("Cancel"), 0, 1000, parent);
    dlg->setWindowTitle(QObject::tr("Transcribe"));
    dlg->setWindowModality(Qt::WindowModal);
    dlg->setMinimumDuration(300);
    dlg->setAutoClose(false);
    dlg->setAutoReset(false);
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    auto permille = std::make_shared<std::atomic<int>>(0);
    auto current = std::make_shared<std::atomic<int>>(0);
    QObject::connect(dlg, &QProgressDialog::canceled, dlg, [cancel] { *cancel = true; });
    if (parent) QObject::connect(parent, &QObject::destroyed, dlg, [cancel] { *cancel = true; });
    auto* tick = new QTimer(dlg);
    QObject::connect(tick, &QTimer::timeout, dlg, [dlg, permille, current, jobs] {
        dlg->setValue(*permille);
        const int i = std::min(int(jobs.size()) - 1, current->load());
        dlg->setLabelText(jobs.size() == 1 ? QObject::tr("Transcribing %1…").arg(jobs[0].name)
                                           : QObject::tr("Transcribing %1 (%2 of %3)…")
                                                 .arg(jobs[size_t(i)].name)
                                                 .arg(i + 1)
                                                 .arg(jobs.size()));
    });
    tick->start(100);
    auto* watcher = new QFutureWatcher<TranscribeResults>(dlg);
    QPointer<EditorState> st(state);
    QObject::connect(watcher, &QFutureWatcher<TranscribeResults>::finished, dlg, [st, watcher, dlg, parent] {
        const TranscribeResults r = watcher->result();
        closeProgress(dlg, parent);
        if (!st) return;
        size_t words = 0;
        for (const auto& [id, t] : r.done) words += t->wordCount();
        if (!r.done.empty()) {
            auto done = r.done;
            st->edit(QObject::tr("Transcribe"), [done](Project& p, Sequence&) {
                bool any = false;
                for (const auto& [id, t] : done)
                    if (MediaItem* m = p.findMedia(id)) {
                        m->transcript = t;
                        any = true;
                    }
                return any;
            });
            st->message(QObject::tr("Transcribed %n clip(s): %1 words", "", int(r.done.size())).arg(words));
        } else if (r.cancelled) {
            st->message(QObject::tr("Transcription cancelled"));
        }
        if (!r.errors.isEmpty())
            QMessageBox::warning(parent, QObject::tr("Transcribe"),
                                 QObject::tr("Some clips could not be transcribed:\n\n%1").arg(r.errors.join('\n')));
    });
    watcher->setFuture(QtConcurrent::run([jobs, options, cancel, permille, current]() {
        TranscribeResults r;
        const double n = double(jobs.size());
        for (size_t i = 0; i < jobs.size() && !*cancel; ++i) {
            *current = int(i);
            auto t = std::make_shared<Transcript>();
            std::string err;
            const bool ok = transcribeMedia(
                jobs[i].path, options, *t, [&](double f) { *permille = int(1000.0 * (double(i) + f) / n); },
                cancel.get(), &err);
            if (ok) r.done.emplace_back(jobs[i].id, std::move(t));
            else if (!*cancel) r.errors << QStringLiteral("%1: %2").arg(jobs[i].name, QString::fromStdString(err));
        }
        r.cancelled = *cancel;
        return r;
    }));
}

}  // namespace

void startTranscription(EditorState* state, const std::vector<Id>& media, const TranscribeOptions& optionsIn,
                        QWidget* parent) {
    TranscribeOptions options = optionsIn;
    // The speaker model first (asked for and fetched here); without it, words only.
    if (options.speakers &&
        !ensureModelPack(parent, speakerModel(), QObject::tr("Transcribe"),
                         QObject::tr("Labelling speakers uses pyannote's segmentation model (MIT) and the CAM++ voice model "
                                     "(Apache-2.0), which run on this computer.")))
        options.speakers = false;
    std::vector<TranscribeJob> jobs;
    for (Id id : media)
        if (const MediaItem* m = state->project().findMedia(id); m && m->hasAudio && !m->path.empty())
            jobs.push_back({id, m->path, QString::fromStdString(m->name)});
    if (jobs.empty()) {
        state->message(QObject::tr("Nothing to transcribe: choose clips with sound"));
        return;
    }
    if (!whisperModelPath(options.model).empty()) {
        runTranscription(state, jobs, options, parent);
        return;
    }
    // Fetch the model first, then carry on.
    const WhisperModel* model = modelNamed(options.model);
    const QString label = model ? QString::fromStdString(model->label) : QString::fromStdString(options.model);
    auto* dlg = new QProgressDialog(QObject::tr("Downloading the speech model…"), QObject::tr("Cancel"), 0, 1000, parent);
    dlg->setWindowTitle(QObject::tr("Transcribe"));
    dlg->setWindowModality(Qt::WindowModal);
    dlg->setMinimumDuration(0);
    dlg->setAutoClose(false);
    dlg->setAutoReset(false);
    auto* download = new ModelDownload(dlg);
    QObject::connect(download, &ModelDownload::progress, dlg, [dlg, label](qint64 got, qint64 total) {
        if (total > 0) dlg->setValue(int(1000 * got / total));
        dlg->setLabelText(QObject::tr("Downloading the %1 speech model: %2 of %3")
                              .arg(label, megabytes(got), total > 0 ? megabytes(total) : QObject::tr("?")));
    });
    QObject::connect(dlg, &QProgressDialog::canceled, download, &ModelDownload::abort);
    QPointer<EditorState> st(state);
    QObject::connect(download, &ModelDownload::finished, dlg,
                     [st, dlg, jobs, options, parent](bool ok, const QString& error) {
                         closeProgress(dlg, parent);
                         if (!st) return;
                         if (ok) runTranscription(st, jobs, options, parent);
                         else if (!error.isEmpty()) QMessageBox::warning(parent, QObject::tr("Transcribe"), error);
                     });
    download->start(options.model);
}

void exportTranscript(const MediaItem& media, QWidget* parent) {
    if (!media.transcript) return;
    QSettings settings = appSettings();
    const QString dir = settings.value("transcribe/exportDir", QFileInfo(QString::fromStdString(media.path)).absolutePath())
                            .toString();
    const QString srt = QObject::tr("SubRip captions (*.srt)"), vtt = QObject::tr("WebVTT captions (*.vtt)"),
                  txt = QObject::tr("Plain text (*.txt)"), json = QObject::tr("Transcript with word timings (*.json)");
    QString filter = srt;
    QString path = QFileDialog::getSaveFileName(
        parent, QObject::tr("Export Transcript"),
        dir + "/" + QFileInfo(QString::fromStdString(media.name)).completeBaseName() + ".srt",
        QStringList{srt, vtt, txt, json}.join(";;"), &filter);
    if (path.isEmpty()) return;
    QString format = QFileInfo(path).suffix().toLower();
    if (!QStringList{"srt", "vtt", "txt", "json"}.contains(format)) {
        format = filter == vtt ? "vtt" : filter == txt ? "txt" : filter == json ? "json" : "srt";
        path += "." + format;
    }
    settings.setValue("transcribe/exportDir", QFileInfo(path).absolutePath());
    QSaveFile f(path);
    const std::string text = transcriptAs(*media.transcript, format.toStdString());
    if (!f.open(QIODevice::WriteOnly) || f.write(text.data(), qint64(text.size())) != qint64(text.size()) || !f.commit())
        QMessageBox::warning(parent, QObject::tr("Export Transcript"),
                             QObject::tr("Cannot write %1").arg(QDir::toNativeSeparators(path)));
}

}  // namespace montage
