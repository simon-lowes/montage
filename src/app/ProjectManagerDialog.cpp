#include "ProjectManagerDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "EditorState.h"

namespace montage {

ProjectManagerDialog::ProjectManagerDialog(EditorState* state, QWidget* parent) : QDialog(parent), state_(state) {
    setWindowTitle(tr("Project Manager"));
    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    auto* where = new QWidget(this);
    auto* h = new QHBoxLayout(where);
    h->setContentsMargins(0, 0, 0, 0);
    folder_ = new QLineEdit(where);
    folder_->setObjectName(QStringLiteral("pmFolder"));
    auto* browse = new QPushButton(tr("Browse…"), where);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Copy the Project To"), folder_->text());
        if (!dir.isEmpty()) folder_->setText(dir);
    });
    h->addWidget(folder_, 1);
    h->addWidget(browse);
    form->addRow(tr("Destination:"), where);
    name_ = new QLineEdit(this);
    name_->setObjectName(QStringLiteral("pmName"));
    const QString current = QFileInfo(state_->filePath()).completeBaseName();
    name_->setText(current.isEmpty() ? tr("Project") : current + tr(" (copy)"));
    form->addRow(tr("Project name:"), name_);
    sequences_ = new QComboBox(this);
    sequences_->setObjectName(QStringLiteral("pmSequences"));
    sequences_->addItem(tr("All sequences"));
    sequences_->addItem(tr("The active sequence (and what it nests)"));
    form->addRow(tr("Keep:"), sequences_);
    mode_ = new QComboBox(this);
    mode_->setObjectName(QStringLiteral("pmMode"));
    mode_->addItem(tr("Collect files: copy the media used, whole"));
    mode_->addItem(tr("Consolidate: transcode only the parts used, with handles"));
    form->addRow(tr("Media:"), mode_);
    handles_ = new QDoubleSpinBox(this);
    handles_->setObjectName(QStringLiteral("pmHandles"));
    handles_->setRange(0, 60);
    handles_->setValue(1.0);
    handles_->setSuffix(tr(" s"));
    form->addRow(tr("Handles:"), handles_);
    codec_ = new QComboBox(this);
    codec_->setObjectName(QStringLiteral("pmCodec"));
    codec_->addItem(tr("ProRes 422 HQ (.mov)"), QStringLiteral("prores_ks"));
    codec_->addItem(tr("H.264 (.mp4)"), QStringLiteral("libx264"));
    form->addRow(tr("Transcode to:"), codec_);
    keepUnused_ = new QCheckBox(tr("Keep media nothing uses"), this);
    keepUnused_->setObjectName(QStringLiteral("pmKeepUnused"));
    form->addRow(QString(), keepUnused_);
    auto update = [this] {
        const bool trim = mode_->currentIndex() == 1;
        handles_->setEnabled(trim);
        codec_->setEnabled(trim);
    };
    connect(mode_, &QComboBox::currentIndexChanged, this, update);
    update();
    layout->addLayout(form);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Copy Project"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

ConsolidateOptions ProjectManagerDialog::options() const {
    ConsolidateOptions o;
    o.folder = folder_->text().trimmed().toStdString();
    o.name = name_->text().trimmed().isEmpty() ? std::string("Project") : name_->text().trimmed().toStdString();
    if (sequences_->currentIndex() == 1 && state_->sequence()) o.sequences = {state_->sequence()->id};
    o.trim = mode_->currentIndex() == 1;
    o.handles = handles_->value();
    o.codec = codec_->currentData().toString().toStdString();
    o.keepUnused = keepUnused_->isChecked();
    return o;
}

}  // namespace montage
