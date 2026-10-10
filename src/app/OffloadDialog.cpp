#include "OffloadDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDirIterator>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMutex>
#include <QSettings>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <atomic>
#include <memory>

#include "Settings.h"
#include "EditorState.h"

namespace montage {

namespace {

// Runs `work` off the interface thread behind a progress dialog that can cancel it.
template <typename R, typename Work>
R withProgress(QWidget* parent, const QString& label, Work work) {
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    auto done = std::make_shared<std::atomic<int>>(0);
    auto file = std::make_shared<QString>();
    auto lock = std::make_shared<QMutex>();
    QProgressDialog progress(label, QObject::tr("Cancel"), 0, 1000, parent);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(300);
    QObject::connect(&progress, &QProgressDialog::canceled, &progress, [cancel] { *cancel = true; });
    QTimer tick;
    QObject::connect(&tick, &QTimer::timeout, &progress, [&progress, done, file, lock, label] {
        progress.setValue(done->load());
        QMutexLocker l(lock.get());
        if (!file->isEmpty()) progress.setLabelText(label + QStringLiteral("\n") + *file);
    });
    tick.start(100);
    QFutureWatcher<R> watcher;
    QEventLoop wait;
    QObject::connect(&watcher, &QFutureWatcher<R>::finished, &wait, &QEventLoop::quit);
    watcher.setFuture(QtConcurrent::run([work, cancel, done, file, lock] {
        return work([cancel, done, file, lock](double f, const QString& at) {
            *done = int(f * 999);
            if (!at.isEmpty()) {
                QMutexLocker l(lock.get());
                *file = at;
            }
            return !cancel->load();
        });
    }));
    if (!watcher.isFinished()) wait.exec();
    tick.stop();
    QObject::disconnect(&progress, &QProgressDialog::canceled, nullptr, nullptr);
    progress.close();
    return watcher.result();
}

QString sizeText(qint64 bytes) {
    if (bytes >= qint64(1) << 30) return QObject::tr("%1 GB").arg(double(bytes) / double(qint64(1) << 30), 0, 'f', 2);
    if (bytes >= qint64(1) << 20) return QObject::tr("%1 MB").arg(double(bytes) / double(1 << 20), 0, 'f', 1);
    return QObject::tr("%1 KB").arg(double(bytes) / 1024.0, 0, 'f', 0);
}

}  // namespace

OffloadDialog::OffloadDialog(EditorState* state, QWidget* parent) : QDialog(parent), state_(state) {
    setWindowTitle(tr("Offload Card"));
    setObjectName(QStringLiteral("offloadDialog"));
    auto* layout = new QVBoxLayout(this);
    auto* intro = new QLabel(tr("Copies a camera card to one or more drives at once. Every file is checksummed (XXH64) as it is "
                                "read and each copy is read back and compared; an ASC MHL hash list goes with each copy so it "
                                "can be checked again later by any tool."),
                             this);
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto* form = new QFormLayout;
    QSettings settings = appSettings();

    auto* sourceRow = new QHBoxLayout;
    source_ = new QLineEdit(this);
    source_->setObjectName(QStringLiteral("offloadSource"));
    source_->setPlaceholderText(tr("The card (or any folder)"));
    auto* browse = new QPushButton(tr("Choose…"), this);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString d = QFileDialog::getExistingDirectory(this, tr("Card to Offload"), source_->text());
        if (!d.isEmpty()) setSource(d);
    });
    sourceRow->addWidget(source_, 1);
    sourceRow->addWidget(browse);
    form->addRow(tr("Card:"), sourceRow);

    destinations_ = new QListWidget(this);
    destinations_->setObjectName(QStringLiteral("offloadDestinations"));
    destinations_->setMaximumHeight(90);
    for (const QString& d : settings.value(QStringLiteral("offload/destinations")).toStringList()) addDestination(d);
    auto* destButtons = new QHBoxLayout;
    auto* add = new QPushButton(tr("Add…"), this);
    auto* remove = new QPushButton(tr("Remove"), this);
    connect(add, &QPushButton::clicked, this, [this] {
        const QString d = QFileDialog::getExistingDirectory(this, tr("Copy To"));
        if (!d.isEmpty()) addDestination(d);
    });
    connect(remove, &QPushButton::clicked, this, [this] { delete destinations_->currentItem(); });
    destButtons->addWidget(add);
    destButtons->addWidget(remove);
    destButtons->addStretch(1);
    auto* destBox = new QVBoxLayout;
    destBox->addWidget(destinations_);
    destBox->addLayout(destButtons);
    form->addRow(tr("Copy to:"), destBox);

    verify_ = new QCheckBox(tr("Read every copy back and compare it with the card"), this);
    verify_->setObjectName(QStringLiteral("offloadVerify"));
    verify_->setChecked(settings.value(QStringLiteral("offload/verify"), true).toBool());
    form->addRow(QString(), verify_);
    mhl_ = new QCheckBox(tr("Write an ASC MHL hash list with each copy"), this);
    mhl_->setObjectName(QStringLiteral("offloadMhl"));
    mhl_->setChecked(settings.value(QStringLiteral("offload/mhl"), true).toBool());
    form->addRow(QString(), mhl_);
    import_ = new QCheckBox(tr("Import the copied media into a bin named after the card"), this);
    import_->setObjectName(QStringLiteral("offloadImport"));
    import_->setChecked(settings.value(QStringLiteral("offload/import"), true).toBool());
    form->addRow(QString(), import_);
    author_ = new QLineEdit(settings.value(QStringLiteral("offload/author")).toString(), this);
    author_->setObjectName(QStringLiteral("offloadAuthor"));
    author_->setPlaceholderText(tr("Who offloaded it (in the hash list)"));
    form->addRow(tr("Name:"), author_);
    layout->addLayout(form);

    report_ = new QPlainTextEdit(this);
    report_->setObjectName(QStringLiteral("offloadReport"));
    report_->setReadOnly(true);
    report_->setMinimumSize(480, 120);
    layout->addWidget(report_, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto* start = buttons->addButton(tr("Offload"), QDialogButtonBox::AcceptRole);
    start->setObjectName(QStringLiteral("offloadStart"));
    connect(start, &QPushButton::clicked, this, [this] { run(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

void OffloadDialog::setSource(const QString& folder) { source_->setText(QDir::toNativeSeparators(folder)); }

void OffloadDialog::addDestination(const QString& folder) {
    const QString d = QDir::toNativeSeparators(folder);
    if (destinations_->findItems(d, Qt::MatchExactly).isEmpty()) destinations_->addItem(d);
}

OffloadResult OffloadDialog::run() {
    const QString source = QDir::fromNativeSeparators(source_->text().trimmed());
    QStringList dests;
    for (int i = 0; i < destinations_->count(); ++i) dests << QDir::fromNativeSeparators(destinations_->item(i)->text());
    OffloadSettings os;
    os.verify = verify_->isChecked();
    os.mhl = mhl_->isChecked();
    os.author = author_->text().trimmed();
    QSettings settings = appSettings();
    settings.setValue(QStringLiteral("offload/destinations"), dests);
    settings.setValue(QStringLiteral("offload/verify"), os.verify);
    settings.setValue(QStringLiteral("offload/mhl"), os.mhl);
    settings.setValue(QStringLiteral("offload/import"), import_->isChecked());
    settings.setValue(QStringLiteral("offload/author"), os.author);

    const OffloadResult r = withProgress<OffloadResult>(this, tr("Offloading %1…").arg(QFileInfo(source).fileName()),
                                                        [source, dests, os](const std::function<bool(double, const QString&)>& p) {
                                                            return offloadCard(source, dests, os, p);
                                                        });
    QStringList lines;
    if (!r.error.isEmpty()) {
        lines << tr("Stopped: %1").arg(r.error);
    } else {
        lines << (r.ok ? tr("Offloaded %n file(s), %1, to %2 drive(s); every copy matches the card.", nullptr, r.files)
                       : tr("Offloaded %n file(s), %1, to %2 drive(s), with problems:", nullptr, r.files))
                         .arg(sizeText(r.bytes))
                         .arg(r.copies.size());
        if (r.alreadyThere) lines << tr("%n file(s) were already there, identical, and were kept.", nullptr, r.alreadyThere);
        for (const OffloadIssue& i : r.issues) lines << QStringLiteral("  %1: %2").arg(i.path.isEmpty() ? tr("Hash list") : i.path, i.problem);
        if (os.mhl) lines << tr("ASC MHL hash lists are in each copy's ascmhl folder.");
    }
    // The first copy's media in a bin named after the card.
    if (r.error.isEmpty() && import_->isChecked() && !r.copies.isEmpty()) {
        QStringList files;
        QDirIterator it(r.copies.front(), QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString f = it.next();
            if (!f.contains(QStringLiteral("/ascmhl/"))) files << f;
        }
        files.sort();
        QStringList errors;
        const auto ids = state_->importFiles(files, &errors, QFileInfo(source).fileName());
        lines << tr("Imported %n clip(s) into the bin \"%1\".", nullptr, int(ids.size())).arg(QFileInfo(source).fileName());
    }
    reportText_ = lines.join('\n');
    report_->setPlainText(reportText_);
    return r;
}

MhlVerifyResult verifyMhlWithProgress(QWidget* parent, const QString& folder, bool writeGeneration, QString* report) {
    const MhlVerifyResult v = withProgress<MhlVerifyResult>(parent, QObject::tr("Verifying %1…").arg(QFileInfo(folder).fileName()),
                                                            [folder, writeGeneration](const std::function<bool(double, const QString&)>& p) {
                                                                return verifyMhl(folder, writeGeneration, {}, p);
                                                            });
    if (report) {
        QStringList lines;
        if (!v.error.isEmpty()) {
            lines << v.error;
        } else {
            lines << (v.ok ? QObject::tr("All %n file(s) the hash list records are there and match.", nullptr, v.verified)
                           : QObject::tr("%n file(s) match; problems found:", nullptr, v.verified));
            auto list = [&](const QString& what, const QStringList& files) {
                if (files.isEmpty()) return;
                QStringList shown = files.mid(0, 8);
                if (files.size() > 8) shown << QObject::tr("and %n more", nullptr, int(files.size() - 8));
                lines << what.arg(files.size()) + QStringLiteral(" ") + shown.join(QStringLiteral(", "));
            };
            list(QObject::tr("%1 missing:"), v.missing);
            list(QObject::tr("%1 changed:"), v.changed);
            list(QObject::tr("%1 not in the hash list:"), v.added);
            list(QObject::tr("%1 recorded only in a hash Montage does not compute:"), v.unchecked);
            if (!v.generation.isEmpty()) lines << QObject::tr("Recorded in a new generation, %1.").arg(v.generation);
        }
        *report = lines.join('\n');
    }
    return v;
}

}  // namespace montage
