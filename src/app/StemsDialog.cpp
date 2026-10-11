#include "StemsDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QEventLoop>
#include <QFile>
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
#include <atomic>
#include <memory>

#include "EditorState.h"
#include "ModelPacks.h"
#include "core/EditOps.h"
#include "media/StemSeparation.h"

namespace montage {

namespace {

struct Job {
    Id clip = 0;
    std::string path;            // the media file (with any interpretation)
    std::vector<int> channels;   // the clip's channels
    double from = 0, to = 0;     // source seconds separated
    std::vector<QString> outs;   // one WAV per stem
    std::string name;            // the clip's name, for the files and their note
};

}  // namespace

std::vector<Id> separateStems(EditorState* state, const std::vector<Id>& clips, const StemOptions& o, QWidget* parent, QString* error) {
    auto fail = [&](const QString& why) {
        if (error) *error = why;
        if (!why.isEmpty()) state->message(why, 6000);
        return std::vector<Id>{};
    };
    const Sequence* s = state->sequence();
    const std::vector<std::string>& names = stemNames(o.stems);
    if (!s || names.empty()) return fail(QObject::tr("Separation is into 2, 4 or 5 stems"));
    std::vector<Job> jobs;
    QString why;
    for (const Id id : clips) {
        Job j;
        j.clip = id;
        std::string err;
        if (!stemSource(state->project(), *s, id, o.handles, j.from, j.to, &err)) {
            why = QString::fromStdString(err);
            continue;
        }
        const Clip* c = edit::clipById(const_cast<Sequence&>(*s), id);
        const MediaItem* m = state->project().findMedia(c->mediaId);
        j.path = m->path;
        j.channels = c->channels;
        j.name = c->name.empty() ? m->name : c->name;
        jobs.push_back(std::move(j));
    }
    if (jobs.empty()) return fail(why.isEmpty() ? QObject::tr("Select the audio clips to separate") : why);
    if (!stemSeparationAvailable()) return fail(QObject::tr("This build of Montage has no ONNX Runtime"));
    if (!ensureModelPack(parent, stemModel(o.stems), QObject::tr("Separate Stems"),
                         QObject::tr("Separating stems uses Spleeter (Deezer Research, MIT), which runs on this computer.")))
        return {};
    // Beside the project, as recorded takes are.
    const QString project = state->filePath();
    const QString folder = (project.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::MusicLocation) + QStringLiteral("/Montage")
                                              : QFileInfo(project).absolutePath()) +
                           QStringLiteral("/Stems");
    QDir().mkpath(folder);
    QStringList files;
    for (Job& j : jobs) {
        QString base = QString::fromStdString(j.name);
        for (const QChar bad : QStringLiteral("/\\:*?\"<>|")) base.replace(bad, '_');
        for (const std::string& n : names) {
            QString path = folder + '/' + base + ' ' + QString::fromStdString(stemTitle(n)) + QStringLiteral(".wav");
            for (int k = 2; QFileInfo::exists(path) || files.contains(path); ++k)
                path = folder + '/' + base + ' ' + QString::fromStdString(stemTitle(n)) + ' ' + QString::number(k) + QStringLiteral(".wav");
            j.outs.push_back(path);
            files << path;
        }
    }
    QProgressDialog progress(QObject::tr("Separating stems…"), QObject::tr("Cancel"), 0, 1000, parent);
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
    const int stems = o.stems;
    watcher.setFuture(QtConcurrent::run([jobs, stems, done, cancel]() -> std::string {
        double total = 0, before = 0;
        for (const Job& j : jobs) total += j.to - j.from;
        for (const Job& j : jobs) {
            std::vector<std::string> outs;
            for (const QString& q : j.outs) outs.push_back(q.toStdString());
            std::string err;
            if (!separateStemsToFiles(
                    j.path, j.from, j.to, j.channels, stems, outs, [&](double f) { *done = (before + f * (j.to - j.from)) / std::max(1e-9, total); },
                    cancel.get(), &err))
                return err.empty() ? "The separation failed" : err;
            before += j.to - j.from;
        }
        return {};
    }));
    if (!watcher.isFinished()) wait.exec();
    tick.stop();
    QObject::disconnect(&progress, &QProgressDialog::canceled, nullptr, nullptr);  // closing a progress dialog emits canceled()
    progress.close();
    const std::string err = watcher.result();
    if (!err.empty()) {
        for (const QString& f : files) QFile::remove(f);
        return fail(err == "Cancelled" ? QString() : QString::fromStdString(err));
    }
    const std::vector<Id> media = state->importFiles(files, nullptr, QStringLiteral("Stems"));
    if (media.size() != size_t(files.size())) return fail(QObject::tr("The stems could not be imported"));
    // Where each came from, noted on the media as it comes in (the import's own undo step).
    state->amend([&](Project& p, Sequence&) {
        size_t k = 0;
        for (const Job& j : jobs)
            for (const std::string& n : names)
                if (MediaItem* m = p.findMedia(media[k++])) {
                    m->metadata["comment"] = stemTitle(n) + " separated from " + j.name + " (" +
                                             QFileInfo(QString::fromStdString(j.path)).fileName().toStdString() + ") with Spleeter in Montage";
                    if (std::find(m->keywords.begin(), m->keywords.end(), "Stem") == m->keywords.end()) m->keywords.push_back("Stem");
                }
        return true;
    });
    std::vector<std::string> titles;
    for (const std::string& n : names) titles.push_back(stemTitle(n));
    std::vector<Id> created;
    state->apply(QObject::tr("Separate Stems"), [&](Project& p, Sequence& sq) {
        size_t k = 0;
        for (const Job& j : jobs) {
            const std::vector<Id> mine(media.begin() + std::ptrdiff_t(k), media.begin() + std::ptrdiff_t(k + names.size()));
            k += names.size();
            const edit::Result r = placeStems(p, sq, j.clip, mine, titles, j.from, o.muteOriginal);
            if (!r.ok) return r;
            created.insert(created.end(), r.created.begin(), r.created.end());
        }
        edit::Result all;
        all.ok = true;
        all.created = created;
        return all;
    });
    if (!created.empty())
        state->message(QObject::tr("Separated %n clip(s) into %1 stems each", "", int(jobs.size())).arg(o.stems), 5000);
    return created;
}

StemsDialog::StemsDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Separate Stems"));
    auto* form = new QFormLayout;
    stems_ = new QComboBox(this);
    stems_->setObjectName(QStringLiteral("stems"));
    stems_->addItem(tr("Vocals, drums, bass and other (4 stems)"), 4);
    stems_->addItem(tr("Vocals, piano, drums, bass and other (5 stems)"), 5);
    stems_->addItem(tr("Vocals and accompaniment (2 stems)"), 2);
    form->addRow(tr("Separate into:"), stems_);
    mute_ = new QCheckBox(tr("Mute the original clip"), this);
    mute_->setObjectName(QStringLiteral("muteOriginal"));
    mute_->setChecked(true);
    form->addRow(QString(), mute_);
    auto* note = new QLabel(tr("Each stem goes on its own track, linked with the clip. Spleeter (Deezer Research) runs on this "
                               "computer; the model is downloaded the first time (73 to 183 MB)."),
                            this);
    note->setWordWrap(true);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Separate"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(note);
    layout->addWidget(buttons);
}

StemOptions StemsDialog::options() const {
    StemOptions o;
    o.stems = stems_->currentData().toInt();
    o.muteOriginal = mute_->isChecked();
    return o;
}

}  // namespace montage
