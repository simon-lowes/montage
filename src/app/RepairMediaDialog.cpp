#include "RepairMediaDialog.h"

#include <QCheckBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

#include "EditorState.h"
#include "WorkProgress.h"

namespace montage {

QString guessReferenceClip(const QString& damaged) {
    const QFileInfo fi(damaged);
    QFileInfoList others = fi.dir().entryInfoList(QDir::Files, QDir::Time);
    int tried = 0;
    for (const QFileInfo& o : others) {
        if (o.suffix().compare(fi.suffix(), Qt::CaseInsensitive) || o.absoluteFilePath() == fi.absoluteFilePath() ||
            o.completeBaseName().contains(QStringLiteral("_repaired")))
            continue;
        if (++tried > 8) break;
        const RepairCheck c = checkRecording(o.absoluteFilePath());
        if (c.opens && c.kind == RepairKind::None) return o.absoluteFilePath();
    }
    return {};
}

RepairMediaDialog::RepairMediaDialog(EditorState* state, QWidget* parent) : QDialog(parent), state_(state) {
    setWindowTitle(tr("Repair Media"));
    setObjectName(QStringLiteral("repairMediaDialog"));
    auto* layout = new QVBoxLayout(this);
    auto* intro = new QLabel(tr("Rebuilds a recording the camera or recorder never finished (a flat battery, a pulled card) into a "
                                "new file beside it. An MP4 or MOV is rebuilt with a good clip from the same camera, recorded "
                                "with the same settings. The damaged file is not changed."),
                             this);
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto* form = new QFormLayout;
    auto row = [this](QLineEdit* edit, QPushButton* button) {
        auto* h = new QHBoxLayout;
        h->addWidget(edit, 1);
        h->addWidget(button);
        return h;
    };
    damaged_ = new QLineEdit(this);
    damaged_->setObjectName(QStringLiteral("repairDamaged"));
    auto* chooseDamaged = new QPushButton(tr("Choose…"), this);
    connect(chooseDamaged, &QPushButton::clicked, this, [this] {
        const QString f = QFileDialog::getOpenFileName(this, tr("Damaged Recording"), damaged_->text(),
                                                       tr("Recordings (*.mp4 *.mov *.m4v *.mxf *.wav *.bwf);;All Files (*)"));
        if (!f.isEmpty()) setDamaged(f);
    });
    connect(damaged_, &QLineEdit::editingFinished, this, [this] { setDamaged(damaged_->text()); });
    form->addRow(tr("Recording:"), row(damaged_, chooseDamaged));
    problem_ = new QLabel(this);
    problem_->setObjectName(QStringLiteral("repairProblem"));
    problem_->setWordWrap(true);
    form->addRow(QString(), problem_);
    reference_ = new QLineEdit(this);
    reference_->setObjectName(QStringLiteral("repairReference"));
    reference_->setPlaceholderText(tr("A clip from the same camera that plays"));
    chooseReference_ = new QPushButton(tr("Choose…"), this);
    connect(chooseReference_, &QPushButton::clicked, this, [this] {
        const QString f = QFileDialog::getOpenFileName(this, tr("Good Clip from the Same Camera"),
                                                       reference_->text().isEmpty() ? QFileInfo(damaged_->text()).absolutePath() : reference_->text());
        if (!f.isEmpty()) setReference(f);
    });
    form->addRow(tr("Good clip:"), row(reference_, chooseReference_));
    output_ = new QLineEdit(this);
    output_->setObjectName(QStringLiteral("repairOutput"));
    auto* chooseOutput = new QPushButton(tr("Choose…"), this);
    connect(chooseOutput, &QPushButton::clicked, this, [this] {
        const QString f = QFileDialog::getSaveFileName(this, tr("Repaired File"), output_->text());
        if (!f.isEmpty()) output_->setText(QDir::toNativeSeparators(f));
    });
    form->addRow(tr("Save as:"), row(output_, chooseOutput));
    import_ = new QCheckBox(tr("Import the repaired file, labelled Recovered"), this);
    import_->setObjectName(QStringLiteral("repairImport"));
    import_->setChecked(true);
    form->addRow(QString(), import_);
    layout->addLayout(form);
    report_ = new QPlainTextEdit(this);
    report_->setObjectName(QStringLiteral("repairReport"));
    report_->setReadOnly(true);
    report_->setMinimumSize(460, 90);
    layout->addWidget(report_, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    start_ = buttons->addButton(tr("Repair"), QDialogButtonBox::AcceptRole);
    start_->setObjectName(QStringLiteral("repairStart"));
    start_->setEnabled(false);
    connect(start_, &QPushButton::clicked, this, [this] { run(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    check();
}

void RepairMediaDialog::setDamaged(const QString& file) {
    const QString f = QDir::toNativeSeparators(file.trimmed());
    damaged_->setText(f);
    checked_ = f.isEmpty() ? RepairCheck{} : checkRecording(QDir::fromNativeSeparators(f));
    if (!f.isEmpty()) output_->setText(QDir::toNativeSeparators(repairedPath(QDir::fromNativeSeparators(f))));
    if (checked_.needsReference && reference_->text().isEmpty()) setReference(guessReferenceClip(QDir::fromNativeSeparators(f)));
    check();
}

void RepairMediaDialog::setReference(const QString& file) {
    reference_->setText(QDir::toNativeSeparators(file));
    check();
}

void RepairMediaDialog::check() {
    const bool any = !damaged_->text().isEmpty();
    if (!any)
        problem_->setText(tr("Choose the recording that does not open."));
    else if (checked_.kind == RepairKind::None)
        problem_->setText(checked_.problem.isEmpty() ? tr("It opens: there is nothing to repair.") : checked_.problem + ".");
    else
        problem_->setText(checked_.problem + ".");
    reference_->setEnabled(checked_.needsReference);
    chooseReference_->setEnabled(checked_.needsReference);
    start_->setEnabled(checked_.kind != RepairKind::None && (!checked_.needsReference || !reference_->text().isEmpty()));
}

bool RepairMediaDialog::run() {
    imported_ = 0;
    check();
    if (!start_->isEnabled()) return false;
    const QString damaged = QDir::fromNativeSeparators(damaged_->text()),
                  reference = checked_.needsReference ? QDir::fromNativeSeparators(reference_->text()) : QString(),
                  output = QDir::fromNativeSeparators(output_->text().trimmed());
    QStringList lines;
    if (output.isEmpty() || QFileInfo(output).absoluteFilePath() == QFileInfo(damaged).absoluteFilePath()) {
        reportText_ = tr("Save it as a new file: the damaged one is kept as it is.");
        report_->setPlainText(reportText_);
        return false;
    }
    struct Outcome {
        bool ok = false;
        RepairReport report;
        QString error;
    };
    start_->setEnabled(false);
    const Outcome o = withProgress<Outcome>(this, tr("Repairing %1…").arg(QFileInfo(damaged).fileName()),
                                            [damaged, reference, output](const std::function<bool(double, const QString&)>& p) {
                                                Outcome r;
                                                r.ok = repairRecording(damaged, reference, output, &r.report, &r.error,
                                                                       [&p](double f) { return p(f, QString()); });
                                                return r;
                                            });
    start_->setEnabled(true);
    if (!o.ok) {
        reportText_ = tr("Not repaired: %1.").arg(o.error);
        report_->setPlainText(reportText_);
        return false;
    }
    const RepairReport& r = o.report;
    QStringList what;
    if (r.videoFrames) what << tr("%n frame(s)", nullptr, r.videoFrames);
    if (r.audioSamples && r.kind != RepairKind::Mxf) what << tr("sound");
    lines << tr("Repaired into %1: %2 s%3.")
                 .arg(QFileInfo(output).fileName())
                 .arg(r.seconds, 0, 'f', 2)
                 .arg(what.isEmpty() ? QString() : QStringLiteral(", ") + what.join(tr(" and ")));
    if (r.bytesLeft > 0)
        lines << tr("The last %1 KB were not whole (the recording's last moment) and are left out.").arg((r.bytesLeft + 1023) / 1024);
    for (const QString& n : r.notes) lines << n + ".";
    if (r.timecode >= 0) lines << tr("Start timecode from the camera's sidecar file.");
    if (import_->isChecked()) {
        QStringList errors;
        const std::vector<Id> ids = state_->importFiles({output}, &errors, bin_);
        if (ids.empty()) {
            lines << tr("It could not be imported: %1").arg(errors.join(QStringLiteral("; ")));
        } else {
            imported_ = ids.front();
            const std::string note = tr("Recovered from %1 by Repair Media").arg(QFileInfo(damaged).fileName()).toStdString();
            const double timecode = r.timecode;
            state_->amend([&](Project& p, Sequence&) {
                MediaItem* m = p.findMedia(imported_);
                if (!m) return false;
                m->metadata["comment"] = note;
                if (std::find(m->keywords.begin(), m->keywords.end(), "Recovered") == m->keywords.end()) m->keywords.push_back("Recovered");
                if (timecode >= 0) m->timecode = timecode;
                return true;
            });
            lines << tr("Imported, with the keyword Recovered.");
        }
    }
    reportText_ = lines.join('\n');
    report_->setPlainText(reportText_);
    return true;
}

}  // namespace montage
