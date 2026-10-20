#include "FootageNotesDialog.h"

#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "EditorState.h"
#include "ModelPacks.h"
#include "WorkProgress.h"
#include "media/LanguageModel.h"

namespace montage {

namespace {
QString writingName(FootageWriting w) {
    switch (w) {
    case FootageWriting::Summary: return FootageNotesDialog::tr("Summary");
    case FootageWriting::ShowNotes: return FootageNotesDialog::tr("Show Notes");
    case FootageWriting::StoryNotes: return FootageNotesDialog::tr("Story Notes");
    }
    return {};
}
}  // namespace

FootageNotesDialog::FootageNotesDialog(EditorState* state, QWidget* parent) : QDialog(parent), state_(state) {
    setWindowTitle(tr("Summary and Notes"));
    setObjectName(QStringLiteral("footageNotesDialog"));
    resize(620, 480);
    auto* root = new QVBoxLayout(this);
    auto* intro = new QLabel(tr("Written from what is said in the sequence by a language model on this computer. Read it through "
                                "before using it: it can get things wrong."),
                             this);
    intro->setWordWrap(true);
    root->addWidget(intro);
    auto* row = new QHBoxLayout;
    kind_ = new QComboBox(this);
    kind_->setObjectName(QStringLiteral("footageNotesKind"));
    for (FootageWriting w : {FootageWriting::Summary, FootageWriting::ShowNotes, FootageWriting::StoryNotes})
        kind_->addItem(writingName(w), int(w));
    auto* writeButton = new QPushButton(tr("Write"), this);
    writeButton->setObjectName(QStringLiteral("footageNotesWrite"));
    row->addWidget(kind_);
    row->addWidget(writeButton);
    row->addStretch(1);
    root->addLayout(row);
    text_ = new QPlainTextEdit(this);
    text_->setObjectName(QStringLiteral("footageNotesText"));
    root->addWidget(text_, 1);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("footageNotesStatus"));
    root->addWidget(status_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto* copy = buttons->addButton(tr("Copy"), QDialogButtonBox::ActionRole);
    auto* save = buttons->addButton(tr("Save…"), QDialogButtonBox::ActionRole);
    root->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(writeButton, &QPushButton::clicked, this, [this] {
        QString err;
        if (!write(FootageWriting(kind_->currentData().toInt()), &err)) status_->setText(err);
    });
    connect(copy, &QPushButton::clicked, this, [this] {
        QGuiApplication::clipboard()->setText(text_->toPlainText());
        status_->setText(tr("Copied"));
    });
    connect(save, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getSaveFileName(this, tr("Save Notes"), kind_->currentText() + QStringLiteral(".txt"),
                                                          tr("Text (*.txt *.md)"));
        if (path.isEmpty()) return;
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(text_->toPlainText().toUtf8()) < 0)
            status_->setText(tr("Could not write %1").arg(path));
        else
            status_->setText(tr("Saved %1").arg(path));
    });
}

QString FootageNotesDialog::text() const { return text_->toPlainText(); }

bool FootageNotesDialog::write(FootageWriting what, QString* error) {
    const Sequence* s = state_->sequence();
    if (!s) return false;
    if (!languageModelAvailable()) {
        if (error) *error = tr("This build of Montage cannot write notes: it was built without llama.cpp.");
        return false;
    }
    if (!ensureModelPack(this, languageModel(), tr("Summary and Notes"),
                         tr("Summaries and notes are written by Granite 4.0 Micro (IBM, Apache-2.0 licence), which runs on this computer.")))
        return false;
    struct Outcome {
        std::string text, error;
    };
    const Project project = state_->project();
    const Sequence seq = *s;
    const Outcome o = withProgress<Outcome>(this, tr("Writing the %1…").arg(writingName(what).toLower()), [&](const auto& progress) {
        Outcome out;
        std::atomic<bool> stop{false};
        auto lm = LanguageModel::load({}, &out.error);
        if (lm)
            out.text = writeAboutSequence(project, seq, what, *lm, &out.error, [&](double f) { stop = !progress(f, QString()); }, &stop);
        return out;
    });
    if (o.text.empty()) {
        if (error) *error = o.error == "Cancelled" ? QString() : QString::fromStdString(o.error);
        return false;
    }
    kind_->setCurrentIndex(kind_->findData(int(what)));
    text_->setPlainText(QString::fromStdString(o.text));
    status_->setText(tr("Written on this computer: read it through before using it"));
    return true;
}

}  // namespace montage
