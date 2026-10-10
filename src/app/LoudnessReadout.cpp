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
const double kTargets[] = {-23, -24, -14, -16};
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
    row(4, tr("TP"), tr("Highest true peak (dBTP); keep it below -1"), tp_);
    target_ = new QComboBox(this);
    target_->setObjectName(QStringLiteral("loudnessTarget"));
    target_->addItems({tr("-23 (EBU R128)"), tr("-24 (ATSC A/85)"), tr("-14 (Streaming)"), tr("-16 (Apple, podcasts)")});
    target_->setToolTip(tr("The integrated loudness to aim for"));
    target_->setCurrentIndex(std::clamp(appSettings().value(QStringLiteral("loudness/target"), 0).toInt(), 0, 3));
    reset_ = new QToolButton(this);
    reset_->setObjectName(QStringLiteral("loudnessReset"));
    reset_->setText(tr("Reset"));
    reset_->setToolTip(tr("Measure again from now"));
    grid->addWidget(target_, 5, 0, 1, 2);
    grid->addWidget(reset_, 6, 0, 1, 2);
    grid->setRowStretch(7, 1);
    connect(target_, &QComboBox::currentIndexChanged, this, [this](int i) {
        appSettings().setValue(QStringLiteral("loudness/target"), i);
        setReading(std::nan(""), std::nan(""), integrated_, std::nan(""), std::nan(""));
    });
    connect(reset_, &QToolButton::clicked, this, [this] {
        clear();
        emit resetRequested();
    });
}

double LoudnessReadout::target() const { return kTargets[std::clamp(target_->currentIndex(), 0, 3)]; }

void LoudnessReadout::setTargetIndex(int index) { target_->setCurrentIndex(std::clamp(index, 0, 3)); }

QString LoudnessReadout::text(const char* field) const {
    const QString f = QString::fromLatin1(field);
    const QLabel* l = f == "M" ? m_ : f == "S" ? s_ : f == "I" ? i_ : f == "LRA" ? lra_ : tp_;
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
    integrated_ = integrated;
    // Integrated against the target: green on it, amber near, red off.
    const double off = integrated > -70 ? std::fabs(integrated - target()) : 1e9;
    status_ = off <= 1 ? 0 : off <= 2 ? 1 : 2;
    static const char* const colours[] = {"#3fb950", "#d29922", "#f85149"};
    i_->setStyleSheet(integrated > -70 ? QStringLiteral("color: %1;").arg(QLatin1String(colours[status_])) : QString());
    if (!std::isnan(truePeak)) tp_->setStyleSheet(truePeak > -1 ? QStringLiteral("color: #f85149;") : QString());
}

void LoudnessReadout::clear() {
    for (QLabel* l : {m_, s_, i_, lra_, tp_}) {
        l->setText(QStringLiteral("—"));
        l->setStyleSheet(QString());
    }
    integrated_ = -200;
    status_ = 2;
}

}  // namespace montage
