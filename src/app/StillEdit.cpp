#include "StillEdit.h"

#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QImageReader>
#include <QMenu>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

#include "EditorState.h"
#include "Settings.h"
#include "core/Interpretation.h"
#include "media/ImageSequence.h"
#include "media/Relink.h"
#include "core/Preferences.h"

namespace montage {

namespace {
std::function<bool(const QString&, const QString&)>& launcher() {
    static std::function<bool(const QString&, const QString&)> f;
    return f;
}
}  // namespace

void StillEditor::setLauncherForTesting(std::function<bool(const QString& program, const QString& file)> launch) { launcher() = std::move(launch); }

StillEditor* StillEditor::of(EditorState* state) {
    if (auto* found = state->findChild<StillEditor*>(QString(), Qt::FindDirectChildrenOnly)) return found;
    return new StillEditor(state);
}

StillEditor::StillEditor(EditorState* state) : QObject(state), state_(state) {
    watcher_ = new QFileSystemWatcher(this);
    quiet_ = new QTimer(this);
    quiet_->setSingleShot(true);
    quiet_->setInterval(250);  // (editors save in several writes)
    connect(watcher_, &QFileSystemWatcher::fileChanged, this, [this](const QString& path) {
        saved_.insert(path);
        quiet_->start();
    });
    connect(quiet_, &QTimer::timeout, this, [this] { takeSaved(); });
}

QString StillEditor::program() { return prefs::text(QStringLiteral("editing/stillProgram")); }

void StillEditor::setProgram(const QString& path) { prefs::set(QStringLiteral("editing/stillProgram"), path); }

bool StillEditor::canEdit(Id media) const {
    const MediaItem* m = state_->project().findMedia(media);
    if (!m || m->kind != MediaKind::Image || m->path.empty() || m->subclipOf) return false;
    if (isImageSequencePath(uninterpretedPath(m->path))) return false;
    return QFileInfo::exists(QString::fromStdString(mediaFileOnDisk(m->path)));
}

bool StillEditor::ours(const Copy& c) const {
    return c.generation == state_->projectGeneration() || (!c.project.isEmpty() && c.project == state_->filePath());
}

QString StillEditor::workingCopy(Id media) const {
    for (const auto& [copy, c] : copies_)
        if (c.media == media && ours(c)) return copy;
    return {};
}

bool StillEditor::edit(Id media, QString* why) {
    auto fail = [why](const QString& text) {
        if (why) *why = text;
        return false;
    };
    if (!canEdit(media)) return fail(tr("Only a still from a file can be edited in another application"));
    const QString prog = program();
    if (prog.isEmpty()) return fail(tr("Choose the application for stills first"));
    QString copy = workingCopy(media);
    if (copy.isEmpty() || !QFileInfo::exists(copy)) {
        const MediaItem* m = state_->project().findMedia(media);
        const QFileInfo original(QString::fromStdString(mediaFileOnDisk(m->path)));
        const QString base = state_->filePath().isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                                                          : QFileInfo(state_->filePath()).absolutePath();
        const QString folder = base + QStringLiteral("/Edited Stills");
        if (!QDir().mkpath(folder)) return fail(tr("Could not make the folder %1").arg(folder));
        // "Logo (editing).png", or "Logo (editing 2).png" when another still of that name is being edited.
        const QString suffix = original.suffix().isEmpty() ? QString() : QStringLiteral(".") + original.suffix();
        for (int n = 1;; ++n) {
            copy = folder + '/' + original.completeBaseName() + (n == 1 ? QStringLiteral(" (editing)") : QStringLiteral(" (editing %1)").arg(n)) + suffix;
            const auto taken = copies_.find(copy);
            if (taken == copies_.end() || (taken->second.media == media && ours(taken->second))) break;
        }
        if (QFileInfo::exists(copy)) {
            watcher_->removePath(copy);
            QFile::remove(copy);
        }
        if (!QFile::copy(original.absoluteFilePath(), copy)) return fail(tr("Could not copy %1 to edit it").arg(original.fileName()));
        QFile(copy).setPermissions(QFile(copy).permissions() | QFileDevice::WriteOwner);
        for (auto it = copies_.begin(); it != copies_.end();) it = it->second.media == media && ours(it->second) ? copies_.erase(it) : std::next(it);
        copies_[copy] = {media, state_->projectGeneration(), state_->filePath()};
    }
    if (!watcher_->files().contains(copy)) watcher_->addPath(copy);
    // An application bundle is opened by the system (macOS); anything else is run with the file.
    const bool bundle = prog.endsWith(QStringLiteral(".app")) && QFileInfo(prog).isDir();
    const bool started = launcher() ? launcher()(prog, copy)
                         : bundle   ? QProcess::startDetached(QStringLiteral("open"), {QStringLiteral("-a"), prog, copy})
                                    : QProcess::startDetached(prog, {copy});
    if (!started) return fail(tr("Could not start %1").arg(QFileInfo(prog).fileName()));
    return true;
}

int StillEditor::takeSaved() {
    int count = 0;
    const std::set<QString> paths = std::exchange(saved_, {});
    for (const QString& copy : paths) {
        const auto found = copies_.find(copy);
        if (found == copies_.end()) continue;
        // (saved while another project is open: that project is left alone, and its own still changes once it is open
        // again and the copy saved)
        if (!ours(found->second)) continue;
        const Id media = found->second.media;
        // Saved by writing a new file over it: the new file watched.
        if (QFileInfo::exists(copy) && !watcher_->files().contains(copy)) watcher_->addPath(copy);
        // Not a whole picture yet (still being written): looked at again in a moment.
        QImageReader reader(copy);
        if (!QFileInfo::exists(copy) || QFileInfo(copy).size() == 0 || !reader.canRead()) {
            saved_.insert(copy);
            quiet_->start();
            continue;
        }
        const MediaItem* m = state_->project().findMedia(media);
        if (!m) continue;
        // Kept as the next version beside the copy, which the other application goes on editing.
        const QFileInfo fi(copy);
        QString stem = fi.completeBaseName();
        if (const qsizetype cut = stem.lastIndexOf(QStringLiteral(" (editing")); cut > 0) stem.truncate(cut);
        const QString suffix = fi.suffix().isEmpty() ? QString() : QStringLiteral(".") + fi.suffix();
        QString version;
        for (int n = 2;; ++n) {
            version = fi.absolutePath() + '/' + stem + QStringLiteral(" v%1").arg(n) + suffix;
            if (!QFileInfo::exists(version)) break;
        }
        if (!QFile::copy(copy, version)) {
            state_->message(tr("Could not keep the saved still as %1").arg(QFileInfo(version).fileName()));
            continue;
        }
        // The still's media points at the version, as one undo step; its name stays.
        const std::string onDisk = mediaFileOnDisk(m->path);
        const QString name = QString::fromStdString(m->name);
        std::string path = m->path;
        if (const size_t at = path.find(onDisk); at != std::string::npos) path.replace(at, onDisk.size(), version.toStdString());
        else path = version.toStdString();
        std::string reason;
        const bool done = state_->edit(tr("Edit in Another Application"), [&](Project& p, Sequence&) {
            const MediaItem* before = p.findMedia(media);
            if (!before) return false;
            const std::string kept = before->name;
            if (!relinkMedia(p, media, path, RelinkCheck::Replace, &reason)) return false;
            if (MediaItem* after = p.findMedia(media)) after->name = kept;
            return true;
        });
        if (!done) {
            QFile::remove(version);
            state_->message(tr("Could not update the still: %1").arg(QString::fromStdString(reason)));
            continue;
        }
        state_->recheckOffline();
        ++count;
        emit updated(media);
        state_->message(tr("Updated %1 from %2").arg(name, QFileInfo(program()).completeBaseName()), 4000);
    }
    return count;
}

void StillEditor::addActions(QMenu& menu, EditorState* state, Id media, QWidget* parent) {
    StillEditor* editor = of(state);
    if (!editor->canEdit(media)) return;
    auto choose = [parent] {
        const QString path = QFileDialog::getOpenFileName(parent, tr("Choose Application for Stills"), program());
        if (!path.isEmpty()) setProgram(path);
        return !path.isEmpty();
    };
    menu.addAction(tr("Edit in Another Application…"), parent, [editor, state, media, choose] {
        if (program().isEmpty() && !choose()) return;
        QString why;
        if (!editor->edit(media, &why)) state->message(tr("Could not edit the still: %1").arg(why));
    })->setObjectName(QStringLiteral("editStillElsewhere"));
    if (!program().isEmpty())
        menu.addAction(tr("Choose Application for Stills…"), parent, [choose] { choose(); })->setObjectName(QStringLiteral("chooseStillProgram"));
}

}  // namespace montage
