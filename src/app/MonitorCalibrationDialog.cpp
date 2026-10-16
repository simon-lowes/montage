#include "MonitorCalibrationDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScreen>
#include <QSpinBox>
#include <QVBoxLayout>

#include "DisplayCalibration.h"
#include "PatchGenerator.h"
#include "ReferenceOutputPanel.h"
#include "Settings.h"

namespace montage {

MonitorCalibrationDialog::MonitorCalibrationDialog(PatchGenerator* generator, ReferenceOutputController* output, QWidget* parent)
    : QDialog(parent), generator_(generator), output_(output) {
    setWindowTitle(tr("Monitor Calibration"));
    setObjectName(QStringLiteral("monitorCalibration"));
    auto* lay = new QVBoxLayout(this);

    auto* lutBox = new QGroupBox(tr("Calibration LUTs"), this);
    auto* lutLay = new QVBoxLayout(lutBox);
    auto* about = new QLabel(tr("A display's calibration LUT (.cube, .3dl or .clf, made by calibration software) corrects "
                                "the pictures shown on it, after the viewer's colour transform. It never goes into "
                                "renders, exports or exported LUTs."),
                             lutBox);
    about->setWordWrap(true);
    lutLay->addWidget(about);
    luts_ = new QGridLayout;
    lutLay->addLayout(luts_);
    bypass_ = new QCheckBox(tr("Bypass all calibration"), lutBox);
    bypass_->setObjectName(QStringLiteral("bypassCalibration"));
    bypass_->setToolTip(tr("Show every display uncorrected, to compare"));
    bypass_->setChecked(DisplayCalibration::instance().bypassed());
    connect(bypass_, &QCheckBox::toggled, this, [](bool on) { DisplayCalibration::instance().setBypassed(on); });
    lutLay->addWidget(bypass_);
    lay->addWidget(lutBox);

    auto* genBox = new QGroupBox(tr("Patch generator"), this);
    auto* form = new QFormLayout(genBox);
    auto* genAbout = new QLabel(tr("Calibration software measures a display through Montage: set it to drive a network "
                                   "pattern generator, start it listening, then start here. Patches fill the chosen "
                                   "display (Esc stops) or go out on the reference output."),
                                genBox);
    genAbout->setWordWrap(true);
    form->addRow(genAbout);
    QSettings s = appSettings();
    host_ = new QLineEdit(s.value(QStringLiteral("calibration/patchHost"), QStringLiteral("127.0.0.1")).toString(), genBox);
    host_->setObjectName(QStringLiteral("patchHost"));
    host_->setToolTip(tr("The computer running the calibration software"));
    form->addRow(tr("Software at"), host_);
    port_ = new QSpinBox(genBox);
    port_->setObjectName(QStringLiteral("patchPort"));
    port_->setRange(1, 65535);
    port_->setValue(s.value(QStringLiteral("calibration/patchPort"), kPatchGeneratorPort).toInt());
    form->addRow(tr("Port"), port_);
    target_ = new QComboBox(genBox);
    target_->setObjectName(QStringLiteral("patchTarget"));
    form->addRow(tr("Show on"), target_);
    through_ = new QCheckBox(tr("Through the display's calibration (to verify it)"), genBox);
    through_->setObjectName(QStringLiteral("patchThroughCalibration"));
    through_->setToolTip(tr("Off to measure the display for a new calibration; on to check one"));
    through_->setChecked(generator_->throughCalibration());
    form->addRow(through_);
    startStop_ = new QPushButton(genBox);
    startStop_->setObjectName(QStringLiteral("patchStartStop"));
    status_ = new QLabel(genBox);
    status_->setObjectName(QStringLiteral("patchStatus"));
    status_->setWordWrap(true);
    form->addRow(startStop_, status_);
    lay->addWidget(genBox);
    connect(startStop_, &QPushButton::clicked, this, [this] {
        if (generator_->active()) {
            generator_->stop();
            return;
        }
        const QVariant t = target_->currentData();
        generator_->setReferenceOutput(t.toInt() < 0);
        const QList<QScreen*> screens = QGuiApplication::screens();
        generator_->setScreen(t.toInt() >= 0 && t.toInt() < screens.size() ? screens[t.toInt()] : nullptr);
        generator_->setThroughCalibration(through_->isChecked());
        QSettings st = appSettings();
        st.setValue(QStringLiteral("calibration/patchHost"), host_->text().trimmed());
        st.setValue(QStringLiteral("calibration/patchPort"), port_->value());
        QString why;
        if (!generator_->start(host_->text(), port_->value(), &why)) QMessageBox::warning(this, windowTitle(), why);
    });
    connect(generator_, &PatchGenerator::statusChanged, this, &MonitorCalibrationDialog::refreshPatch);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    lay->addWidget(buttons);

    connect(&DisplayCalibration::instance(), &DisplayCalibration::changed, this, &MonitorCalibrationDialog::fillLuts);
    connect(qGuiApp, &QGuiApplication::screenAdded, this, [this] { fillLuts(), fillTargets(); });
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, [this] { fillLuts(), fillTargets(); });
    if (output_) connect(output_, &ReferenceOutputController::runningChanged, this, &MonitorCalibrationDialog::fillLuts);
    fillLuts();
    fillTargets();
    refreshPatch();
}

void MonitorCalibrationDialog::addLutRow(int row, const QString& label, const QString& target, const QString& tip) {
    auto* name = new QLabel(label, this);
    name->setToolTip(tip);
    luts_->addWidget(name, row, 0);
    const QString path = DisplayCalibration::instance().lutPath(target);
    const QString error = DisplayCalibration::instance().error(target);
    auto* file = new QLabel(path.isEmpty() ? tr("None") : QFileInfo(path).fileName(), this);
    file->setObjectName(QStringLiteral("lutFile:") + target);
    file->setToolTip(path.isEmpty() ? tip : QDir::toNativeSeparators(path) + (error.isEmpty() ? QString() : QStringLiteral("\n") + error));
    if (!error.isEmpty()) {
        file->setText(tr("%1 (cannot be read)").arg(file->text()));
        file->setStyleSheet(QStringLiteral("color: #e05050;"));
    }
    luts_->addWidget(file, row, 1);
    auto* choose = new QPushButton(tr("Choose…"), this);
    choose->setObjectName(QStringLiteral("lutChoose:") + target);
    connect(choose, &QPushButton::clicked, this, [this, target, path] {
        const QString dir = path.isEmpty() ? QDir::homePath() : QFileInfo(path).absolutePath();
        const QString f = QFileDialog::getOpenFileName(this, tr("Calibration LUT"), dir,
                                                       tr("Calibration LUTs (*.cube *.3dl *.clf *.ctf);;All files (*)"));
        if (f.isEmpty()) return;
        QString why;
        if (!DisplayCalibration::instance().setLutPath(target, f, &why)) QMessageBox::warning(this, windowTitle(), why);
    });
    luts_->addWidget(choose, row, 2);
    auto* clear = new QPushButton(tr("None"), this);
    clear->setObjectName(QStringLiteral("lutClear:") + target);
    clear->setEnabled(!path.isEmpty());
    connect(clear, &QPushButton::clicked, this, [target] { DisplayCalibration::instance().setLutPath(target, QString()); });
    luts_->addWidget(clear, row, 3);
}

void MonitorCalibrationDialog::fillLuts() {
    while (QLayoutItem* item = luts_->takeAt(0)) {
        if (QWidget* w = item->widget()) w->deleteLater();
        delete item;
    }
    int row = 0;
    const QList<QScreen*> screens = QGuiApplication::screens();
    for (int i = 0; i < screens.size(); ++i) {
        const QRect g = screens[i]->geometry();
        const QString name = tr("Screen %1: %2 (%3 × %4)").arg(i + 1).arg(screens[i]->name()).arg(g.width()).arg(g.height());
        addLutRow(row++, name, DisplayCalibration::screenTarget(screens[i]),
                  tr("The viewers and full-screen output on this display (their Rec. 709 pictures)"));
        addLutRow(row++, tr("%1, HDR viewer").arg(name), DisplayCalibration::screenTarget(screens[i], true),
                  tr("The HDR viewer on this display: a LUT made for a PQ signal in BT.2020 primaries"));
    }
    if (output_ && output_->running()) {
        addLutRow(row++, tr("Reference output: %1").arg(QString::fromStdString(output_->device().name)), output_->calibrationTarget(),
                  tr("The signal sent to this device (in its own colour: BT.709, BT.2020, PQ or HLG)"));
    } else {
        auto* note = new QLabel(tr("Start the reference output to choose its LUT."), this);
        note->setEnabled(false);
        luts_->addWidget(note, row++, 0, 1, 4);
    }
    if (bypass_) bypass_->setChecked(DisplayCalibration::instance().bypassed());
}

void MonitorCalibrationDialog::fillTargets() {
    const QVariant was = target_->currentData();
    target_->clear();
    const QList<QScreen*> screens = QGuiApplication::screens();
    for (int i = 0; i < screens.size(); ++i) target_->addItem(tr("Screen %1: %2").arg(i + 1).arg(screens[i]->name()), i);
    target_->addItem(tr("Reference output"), -1);
    const int at = was.isValid() ? target_->findData(was) : -1;
    if (at >= 0) target_->setCurrentIndex(at);
}

void MonitorCalibrationDialog::refreshPatch() {
    const bool on = generator_->active();
    startStop_->setText(on ? tr("Stop") : tr("Start"));
    host_->setEnabled(!on);
    port_->setEnabled(!on);
    target_->setEnabled(!on);
    through_->setEnabled(!on);
    status_->setText(generator_->status());
}

}  // namespace montage
