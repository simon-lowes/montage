#include "ProjectManagerDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include "EditorState.h"
#include "Language.h"
#include "Settings.h"
#include "render/ColorSpace.h"

namespace montage {

namespace {

void selectData(QComboBox* box, const QString& data) {
    const int i = box->findData(data);
    if (i >= 0) box->setCurrentIndex(i);
}

}  // namespace

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

    // Presets: the project's, the user's library and the facility's, the narrower overriding the wider.
    auto* presetRow = new QWidget(this);
    auto* ph = new QHBoxLayout(presetRow);
    ph->setContentsMargins(0, 0, 0, 0);
    preset_ = new QComboBox(presetRow);
    preset_->setObjectName(QStringLiteral("pmPreset"));
    auto* save = new QPushButton(tr("Save…"), presetRow);
    save->setObjectName(QStringLiteral("pmSavePreset"));
    auto* saveMenu = new QMenu(save);
    auto saveTo = [this](PresetLevel level) {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("Save Preset"), tr("Preset name:"), QLineEdit::Normal,
                                                   preset_->currentData().toString(), &ok).trimmed();
        if (!ok || name.isEmpty()) return;
        const std::string json = consolidateChoicesJson(options());
        std::string error;
        if (level == PresetLevel::Project) {
            const std::string key = name.toStdString();
            if (!state_->apply(tr("Save Consolidate Preset"), [&](Project& p, Sequence&) {
                    p.consolidatePresets[key] = json;
                    return edit::Result{};
                }))
                error = tr("The project could not be changed").toStdString();
        } else {
            writePreset(userPresetFolder().toStdString(), name.toStdString(), json, &error);
        }
        if (!error.empty()) {
            QMessageBox::warning(this, tr("Save Preset"), QString::fromStdString(error));
            return;
        }
        loadPresets();
        selectData(preset_, name);
    };
    saveMenu->addAction(tr("In the project"), this, [saveTo] { saveTo(PresetLevel::Project); });
    saveMenu->addAction(tr("In my library"), this, [saveTo] { saveTo(PresetLevel::User); });
    save->setMenu(saveMenu);
    auto* compare = new QPushButton(tr("Compare…"), presetRow);
    compare->setObjectName(QStringLiteral("pmComparePreset"));
    compare->setToolTip(tr("What this preset sets differently from the presets of the same name it overrides"));
    connect(compare, &QPushButton::clicked, this, [this] {
        const QString name = preset_->currentData().toString();
        if (name.isEmpty()) return;
        QMessageBox::information(this, tr("Compare Preset"), compareText(name));
    });
    ph->addWidget(preset_, 1);
    ph->addWidget(save);
    ph->addWidget(compare);
    form->addRow(tr("Preset:"), presetRow);

    mode_ = new QComboBox(this);
    mode_->setObjectName(QStringLiteral("pmMode"));
    mode_->addItem(tr("Collect files: copy the media used, whole"));
    mode_->addItem(tr("Consolidate: transcode only the parts used, with handles"));
    mode_->addItem(tr("For finishing: the parts used, in their own formats"));
    mode_->addItem(tr("For finishing: the parts used, as image sequences"));
    mode_->setItemData(2, tr("Cut without coding again where the format allows; other formats are transcoded to ProRes 422 HQ"),
                       Qt::ToolTipRole);
    form->addRow(tr("Media:"), mode_);

    auto* handleRow = new QWidget(this);
    auto* hh = new QHBoxLayout(handleRow);
    hh->setContentsMargins(0, 0, 0, 0);
    handleUnit_ = new QComboBox(handleRow);
    handleUnit_->setObjectName(QStringLiteral("pmHandleUnit"));
    handleUnit_->addItem(tr("Seconds"), QStringLiteral("seconds"));
    handleUnit_->addItem(tr("Frames"), QStringLiteral("frames"));
    handleUnit_->addItem(tr("Timecode"), QStringLiteral("timecode"));
    handleUnit_->addItem(tr("None"), QStringLiteral("none"));
    handleUnit_->addItem(tr("The whole file"), QStringLiteral("whole"));
    handles_ = new QDoubleSpinBox(handleRow);
    handles_->setObjectName(QStringLiteral("pmHandles"));
    handles_->setRange(0, 60);
    handles_->setValue(1.0);
    handles_->setSuffix(tr(" s"));
    handleFrames_ = new QSpinBox(handleRow);
    handleFrames_->setObjectName(QStringLiteral("pmHandleFrames"));
    handleFrames_->setRange(0, 100000);
    handleFrames_->setSuffix(tr(" frames"));
    handleTimecode_ = new QLineEdit(handleRow);
    handleTimecode_->setObjectName(QStringLiteral("pmHandleTimecode"));
    handleTimecode_->setPlaceholderText(QStringLiteral("00:00:00:12"));
    hh->addWidget(handleUnit_);
    hh->addWidget(handles_, 1);
    hh->addWidget(handleFrames_, 1);
    hh->addWidget(handleTimecode_, 1);
    form->addRow(tr("Handles:"), handleRow);

    codec_ = new QComboBox(this);
    codec_->setObjectName(QStringLiteral("pmCodec"));
    codec_->addItem(tr("ProRes 422 HQ (.mov)"), QStringLiteral("prores_ks"));
    codec_->addItem(tr("H.264 (.mp4)"), QStringLiteral("libx264"));
    codec_->addItem(tr("DNxHR HQ (.mov)"), QStringLiteral("dnxhd"));
    codec_->addItem(tr("H.265 (.mp4)"), QStringLiteral("libx265"));
    codec_->addItem(tr("FFV1, lossless (.mkv)"), QStringLiteral("ffv1"));
    codec_->addItem(tr("Each file's own, where it is one of these"), QString());
    form->addRow(tr("Transcode to:"), codec_);
    frames_ = new QComboBox(this);
    frames_->setObjectName(QStringLiteral("pmFrames"));
    frames_->addItem(tr("OpenEXR (half float)"), QStringLiteral("exr"));
    frames_->addItem(tr("DPX (10-bit)"), QStringLiteral("dpx"));
    frames_->addItem(tr("TIFF (16-bit)"), QStringLiteral("tiff"));
    form->addRow(tr("Image sequences:"), frames_);
    colour_ = new QComboBox(this);
    colour_->setObjectName(QStringLiteral("pmColour"));
    colour_->addItem(tr("Each file's own"), QString());
    colour_->addItem(tr("The working space"), QStringLiteral("working"));
    for (const ColorSpace& cs : colorSpaces()) colour_->addItem(language::engine(cs.label), QString::fromStdString(cs.id));
    form->addRow(tr("Colour space:"), colour_);
    names_ = new QLineEdit(this);
    names_->setObjectName(QStringLiteral("pmNames"));
    names_->setPlaceholderText(tr("Each source's own"));
    names_->setToolTip(tr("A pattern of {sequence}, {shot}, {clip}, {reel}, {tape}, {scene}, {take} and, for image sequences, {frame}"));
    form->addRow(tr("Names:"), names_);
    depth_ = new QSpinBox(this);
    depth_->setObjectName(QStringLiteral("pmDepth"));
    depth_->setRange(0, 16);
    depth_->setSpecialValueText(tr("None"));
    depth_->setToolTip(tr("How many of each source's folders are kept above it in the new Media folder"));
    form->addRow(tr("Source folders kept:"), depth_);
    leaveOut_ = new QCheckBox(tr("Leave out clips nothing shows (disabled, or hidden under clips above)"), this);
    leaveOut_->setObjectName(QStringLiteral("pmLeaveOut"));
    form->addRow(QString(), leaveOut_);
    bake_ = new QCheckBox(tr("Bake in grades, effects and speed"), this);
    bake_->setObjectName(QStringLiteral("pmBake"));
    form->addRow(QString(), bake_);
    trimSound_ = new QCheckBox(tr("Sound: only the parts used, as WAV"), this);
    trimSound_->setObjectName(QStringLiteral("pmTrimSound"));
    form->addRow(QString(), trimSound_);
    keepUnused_ = new QCheckBox(tr("Keep media nothing uses"), this);
    keepUnused_->setObjectName(QStringLiteral("pmKeepUnused"));
    form->addRow(QString(), keepUnused_);

    connect(mode_, &QComboBox::currentIndexChanged, this, [this] { update(); });
    connect(handleUnit_, &QComboBox::currentIndexChanged, this, [this] { update(); });
    connect(preset_, &QComboBox::currentIndexChanged, this, [this, compare] {
        const QString name = preset_->currentData().toString();
        compare->setEnabled(!name.isEmpty());
        if (name.isEmpty()) return;
        ConsolidateOptions o;
        if (resolvePreset(presets(), name.toStdString(), o)) setChoices(o);
    });
    loadPresets();
    compare->setEnabled(false);
    update();
    layout->addLayout(form);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Copy Project"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

QString ProjectManagerDialog::userPresetFolder() { return QString::fromStdString(montage::userPresetFolder()); }

ConsolidatePresets ProjectManagerDialog::presets() const {
    return loadConsolidatePresets(state_->project(), userPresetFolder().toStdString());
}

void ProjectManagerDialog::loadPresets() {
    const QSignalBlocker block(preset_);
    const QString was = preset_->currentData().toString();
    preset_->clear();
    preset_->addItem(tr("None"), QString());
    const ConsolidatePresets all = presets();
    for (const std::string& n : all.names()) {
        QStringList where;
        for (int l = 2; l >= 0; --l)
            if (all.levels[l].count(n)) where << (l == 2 ? tr("project") : l == 1 ? tr("library") : tr("facility"));
        preset_->addItem(tr("%1 (%2)").arg(QString::fromStdString(n), where.join(QStringLiteral(", "))), QString::fromStdString(n));
    }
    selectData(preset_, was);
}

void ProjectManagerDialog::update() {
    const int mode = mode_->currentIndex();
    const bool part = mode != 0;
    handleUnit_->setEnabled(part);
    const QString unit = handleUnit_->currentData().toString();
    handles_->setVisible(unit == QLatin1String("seconds"));
    handleFrames_->setVisible(unit == QLatin1String("frames"));
    handleTimecode_->setVisible(unit == QLatin1String("timecode"));
    handles_->setEnabled(part);
    handleFrames_->setEnabled(part);
    handleTimecode_->setEnabled(part);
    codec_->setEnabled(mode == 1);
    frames_->setEnabled(mode == 3);
    colour_->setEnabled(mode == 1 || mode == 3);
    trimSound_->setEnabled(part);
}

void ProjectManagerDialog::setChoices(const ConsolidateOptions& o) {
    const std::string out = o.output.empty() ? (o.trim ? "movie" : "whole") : o.output;
    mode_->setCurrentIndex(out == "movie" ? 1 : out == "trimmed" ? 2 : out == "frames" ? 3 : 0);
    selectData(handleUnit_, QString::fromStdString(o.handleUnit));
    handles_->setValue(o.handles);
    handleFrames_->setValue(o.handleFrames);
    handleTimecode_->setText(QString::fromStdString(o.handleTimecode));
    selectData(codec_, QString::fromStdString(o.codec));
    selectData(frames_, QString::fromStdString(o.frames));
    selectData(colour_, QString::fromStdString(o.colorSpace));
    names_->setText(QString::fromStdString(o.namePattern));
    depth_->setValue(o.folderDepth);
    leaveOut_->setChecked(o.leaveOutHidden);
    bake_->setChecked(o.bake);
    trimSound_->setChecked(o.trimSound);
    keepUnused_->setChecked(o.keepUnused);
    update();
}

ConsolidateOptions ProjectManagerDialog::options() const {
    ConsolidateOptions o;
    o.folder = folder_->text().trimmed().toStdString();
    o.name = name_->text().trimmed().isEmpty() ? std::string("Project") : name_->text().trimmed().toStdString();
    if (sequences_->currentIndex() == 1 && state_->sequence()) o.sequences = {state_->sequence()->id};
    const int mode = mode_->currentIndex();
    o.trim = mode != 0;
    o.output = mode == 1 ? "movie" : mode == 2 ? "trimmed" : mode == 3 ? "frames" : "";
    o.handleUnit = handleUnit_->currentData().toString().toStdString();
    o.handles = handles_->value();
    o.handleFrames = handleFrames_->value();
    o.handleTimecode = handleTimecode_->text().trimmed().toStdString();
    o.codec = codec_->currentData().toString().toStdString();
    o.frames = frames_->currentData().toString().toStdString();
    o.colorSpace = colour_->currentData().toString().toStdString();
    o.namePattern = names_->text().trimmed().toStdString();
    o.folderDepth = depth_->value();
    o.leaveOutHidden = leaveOut_->isChecked();
    o.bake = bake_->isChecked();
    o.trimSound = trimSound_->isChecked() && mode != 0;
    o.keepUnused = keepUnused_->isChecked();
    // Signing on (the Export dialog's Content Credentials, with an identity): the sources' credentials are kept on the
    // copies made from them.
    const QString certificate = appSettings().value(QStringLiteral("credentials/certificate")).toString(),
                  key = appSettings().value(QStringLiteral("credentials/key")).toString();
    if (appSettings().value(QStringLiteral("export/credentials"), false).toBool() && !certificate.isEmpty() && !key.isEmpty())
        o.credentialCertificate = certificate.toStdString(), o.credentialKey = key.toStdString();
    return o;
}

QString ProjectManagerDialog::compareText(const QString& preset) const {
    const ConsolidatePresets all = presets();
    const std::string name = preset.toStdString();
    int level = -1;
    for (int l = 2; l >= 0 && level < 0; --l)
        if (all.levels[l].count(name)) level = l;
    if (level < 0) return tr("There is no preset called %1.").arg(preset);
    const auto diffs = comparePreset(all, name, PresetLevel(level));
    const QString here = QString::fromLatin1(presetLevelName(PresetLevel(level)));
    if (diffs.empty()) return tr("The %1 preset %2 sets nothing differently from the levels above it.").arg(here, preset);
    QStringList lines;
    lines << tr("The %1 preset %2 sets:").arg(here, preset);
    for (const PresetDifference& d : diffs)
        lines << tr("%1: %2 (the %3 gives %4)")
                     .arg(QString::fromStdString(d.key), QString::fromStdString(d.value), QString::fromStdString(d.widerLevel),
                          d.wider.empty() ? tr("nothing") : QString::fromStdString(d.wider));
    return lines.join(QLatin1Char('\n'));
}

}  // namespace montage
