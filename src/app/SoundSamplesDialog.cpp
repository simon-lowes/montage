#include "SoundSamplesDialog.h"
#include "EditorState.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include "core/EditOps.h"
#include "core/History.h"

namespace montage {

namespace {

int rateOf(const Sequence* s) { return s && s->sampleRate > 0 ? s->sampleRate : 48000; }

}  // namespace

SoundSamplesDialog::SoundSamplesDialog(EditorState* state, Id clip, QWidget* parent) : QDialog(parent), state_(state), clip_(clip) {
    setWindowTitle(tr("Edit Sound by Samples"));
    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    span_ = new QLabel(this);
    span_->setObjectName(QStringLiteral("soundSpan"));
    span_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(tr("Sound:"), span_);
    auto field = [&](QSpinBox*& box, const char* name, const QString& label, const QString& tip) {
        box = new QSpinBox(this);
        box->setObjectName(QString::fromLatin1(name));
        box->setRange(-100000000, 100000000);
        box->setSuffix(tr(" samples"));
        box->setToolTip(tip);
        form->addRow(label, box);
    };
    field(move_, "soundMove", tr("Move:"), tr("Move the sound later (or earlier, negative), leaving the picture where it is"));
    field(trimStart_, "soundTrimStart", tr("Trim start:"), tr("Start the sound later (or earlier, negative)"));
    field(trimEnd_, "soundTrimEnd", tr("Trim end:"), tr("End the sound sooner (or later, negative)"));
    field(slip_, "soundSlip", tr("Slip:"), tr("Play later material in the same place (or earlier, negative)"));
    layout->addLayout(form);
    error_ = new QLabel(this);
    error_->setObjectName(QStringLiteral("soundSamplesError"));
    error_->setStyleSheet(QStringLiteral("color: #f85149;"));
    error_->setWordWrap(true);
    layout->addWidget(error_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Close, this);
    buttons->button(QDialogButtonBox::Apply)->setObjectName(QStringLiteral("soundSamplesApply"));
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, [this] { apply(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    refresh();
}

QString SoundSamplesDialog::span() const { return span_->text(); }

QString SoundSamplesDialog::error() const { return error_->text(); }

void SoundSamplesDialog::refresh() {
    const Sequence* s = state_->sequence();
    const Clip* c = s ? edit::clipById(*s, clip_) : nullptr;
    if (!c) {
        span_->setText(tr("The clip is gone"));
        return;
    }
    const int rate = rateOf(s);
    span_->setText(tr("%1 to %2 (%3 Hz)")
                       .arg(QString::fromStdString(formatTimecodeSamples(edit::soundStart(*c), s->fps, rate)),
                            QString::fromStdString(formatTimecodeSamples(edit::soundEnd(*c), s->fps, rate)))
                       .arg(rate));
}

bool SoundSamplesDialog::apply() {
    std::string error;
    const std::pair<QSpinBox*, edit::SampleEdit> ops[] = {{trimStart_, edit::SampleEdit::TrimStart},
                                                          {trimEnd_, edit::SampleEdit::TrimEnd},
                                                          {slip_, edit::SampleEdit::Slip},
                                                          {move_, edit::SampleEdit::Move}};
    bool any = false;
    for (const auto& [box, op] : ops) any = any || box->value() != 0;
    if (!any) return true;
    const bool done = state_->edit(tr("Edit Sound by Samples"), [&](Project& p, Sequence& s) {
        for (const auto& [box, op] : ops) {
            if (box->value() == 0) continue;
            const edit::Result r = edit::editSoundSamples(p, s, clip_, op, box->value(), rateOf(&s));
            if (!r.ok) {
                error = r.error;
                return false;
            }
        }
        return true;
    });
    error_->setText(QString::fromStdString(error));
    if (done)
        for (const auto& [box, op] : ops) box->setValue(0);
    refresh();
    return done;
}

}  // namespace montage
