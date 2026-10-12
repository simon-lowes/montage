#include "LoudnessReadout.h"
#include "Settings.h"

#include <QComboBox>
#include <QGridLayout>
#include <QLabel>
#include <QSettings>
#include <QToolButton>
#include <cmath>

namespace montage {

namespace {
const double kTargets[] = {-23, -24, -14, -16, -27};
constexpr int kNetflix = 4;
constexpr int kLastTarget = 4;
}

LoudnessReadout::LoudnessReadout(QWidget* parent) : QWidget(parent) {
    auto* grid = new QGridLayout(this);
    grid->setContentsMargins(4, 2, 4, 2);
    grid->setHorizontalSpacing(6);
    grid->setVerticalSpacing(1);
    auto row = [&](int r, const QString& name, const QString& tip, QLabel*& out) {
        auto* label = new QLabel(name, this);
        label->setToolTip(tip);
        out = new QLabel(QStringLiteral("—"), this);
        out->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        out->setToolTip(tip);
        QFont f = out->font();
        f.setFamily(QStringLiteral("Monospace"));
        f.setStyleHint(QFont::TypeWriter);
        out->setFont(f);
        grid->addWidget(label, r, 0);
        grid->addWidget(out, r, 1);
    };
    row(0, tr("M"), tr("Momentary loudness: the last 400 ms (LUFS)"), m_);
    row(1, tr("S"), tr("Short-term loudness: the last 3 s (LUFS)"), s_);
    row(2, tr("I"), tr("Integrated loudness since the last reset (LUFS)"), i_);
    row(3, tr("LRA"), tr("Loudness range: how much the loudness varies (LU)"), lra_);
    row(4, tr("TP"), tr("Highest true peak (dBTP); keep it below -1 (-2 for Netflix and US broadcast)"), tp_);
    const QString dialogueTip = tr("Dialogue loudness: integrated over speech only (LKFS), as Netflix measures it. Montage's own "
                                   "estimate of Dolby's dialogue gating, from the Dialogue clips (or transcribed words) playing. Live, whole "
                                   "Dialogue clips count, pauses too; Quality Check and exports measure only where they sound");
    row(5, tr("D"), dialogueTip, d_);
    row(6, tr("SP"), tr("How much of the sound is speech; under 15 % the integrated loudness counts instead"), sp_);
    target_ = new QComboBox(this);
    target_->setObjectName(QStringLiteral("loudnessTarget"));
    target_->addItems({tr("-23 (EBU R128)"), tr("-24 (ATSC A/85)"), tr("-14 (Streaming)"), tr("-16 (Apple, podcasts)"),
                       tr("-27 dialogue (Netflix)")});
    target_->setToolTip(tr("The integrated loudness to aim for"));
    target_->setCurrentIndex(std::clamp(appSettings().value(QStringLiteral("loudness/target"), 0).toInt(), 0, kLastTarget));
    reset_ = new QToolButton(this);
    reset_->setObjectName(QStringLiteral("loudnessReset"));
    reset_->setText(tr("Reset"));
    reset_->setToolTip(tr("Measure again from now"));
    grid->addWidget(target_, 7, 0, 1, 2);
    grid->addWidget(reset_, 8, 0, 1, 2);
    grid->setRowStretch(9, 1);
    connect(target_, &QComboBox::currentIndexChanged, this, [this](int i) {
        appSettings().setValue(QStringLiteral("loudness/target"), i);
        colour();
    });
    connect(reset_, &QToolButton::clicked, this, [this] {
        clear();
        emit resetRequested();
    });
}

double LoudnessReadout::target() const { return kTargets[std::clamp(target_->currentIndex(), 0, kLastTarget)]; }

bool LoudnessReadout::dialogueTarget() const { return target_->currentIndex() == kNetflix; }

void LoudnessReadout::setTargetIndex(int index) { target_->setCurrentIndex(std::clamp(index, 0, kLastTarget)); }

QString LoudnessReadout::text(const char* field) const {
    const QString f = QString::fromLatin1(field);
    const QLabel* l = f == "M" ? m_ : f == "S" ? s_ : f == "I" ? i_ : f == "LRA" ? lra_ : f == "D" ? d_ : f == "SP" ? sp_ : tp_;
    return l->text();
}

void LoudnessReadout::setReading(double momentary, double shortTerm, double integrated, double range, double truePeak) {
    auto show = [](QLabel* l, double v, double floor) {
        if (std::isnan(v)) return;  // unchanged
        l->setText(v <= floor ? QStringLiteral("—") : QString::number(v, 'f', 1));
    };
    show(m_, momentary, -70);
    show(s_, shortTerm, -70);
    show(i_, integrated, -70);
    show(lra_, range, -1);  // 0.0 is a real range
    show(tp_, truePeak, -95);
    if (!std::isnan(integrated)) integrated_ = integrated;
    if (!std::isnan(truePeak)) truePeak_ = truePeak;
    colour();
}

void LoudnessReadout::setDialogue(double integrated, double speechShare) {
    dialogue_ = integrated;
    share_ = speechShare;
    d_->setText(speechShare >= 0 && integrated > -70 ? QString::number(integrated, 'f', 1) : QStringLiteral("—"));
    sp_->setText(speechShare >= 0 ? QStringLiteral("%1 %").arg(std::lround(speechShare * 100)) : QStringLiteral("—"));
    colour();
}

// The loudness that counts against the target, green on it, amber near, red off; true peaks over the ceiling red.
void LoudnessReadout::colour() {
    const bool dialogue = dialogueTarget() && share_ >= 0.15 && dialogue_ > -70;
    const double counts = dialogue ? dialogue_ : integrated_;
    const double tolerance = dialogueTarget() ? 2 : 1;
    const double off = counts > -70 ? std::fabs(counts - target()) : 1e9;
    status_ = off <= tolerance ? 0 : off <= tolerance + 1 ? 1 : 2;
    static const char* const colours[] = {"#3fb950", "#d29922", "#f85149"};
    const QString style = counts > -70 ? QStringLiteral("color: %1;").arg(QLatin1String(colours[status_])) : QString();
    i_->setStyleSheet(dialogue ? QString() : style);
    d_->setStyleSheet(dialogue ? style : QString());
    const double ceiling = target() <= -24 ? -2 : -1;  // ATSC A/85 and Netflix: -2 dBTP
    tp_->setStyleSheet(truePeak_ > ceiling ? QStringLiteral("color: #f85149;") : QString());
}

void LoudnessReadout::clear() {
    for (QLabel* l : {m_, s_, i_, lra_, tp_, d_, sp_}) {
        l->setText(QStringLiteral("—"));
        l->setStyleSheet(QString());
    }
    integrated_ = truePeak_ = dialogue_ = -200;
    share_ = -1;
    status_ = 2;
}

}  // namespace montage
