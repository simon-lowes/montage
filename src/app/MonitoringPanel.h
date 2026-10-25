// Montage — the Monitoring panel (Window › Monitoring): the monitoring section (render/MonitorSection.h) for the room
// this computer is in. The output device and speaker set, each speaker's channel, trim and delay, bass management,
// Listen As, Dim, Mono, Mute, monitor solos, calibration to a reference level and the cue mix with talkback. Kept in
// the app's settings (not the project): solos and talkback last only while Montage runs.
#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QWidget>

#include "render/MonitorSection.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QListWidget;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QToolButton;

namespace montage {

class EditorState;
class PlaybackController;

class MonitoringPanel : public QWidget {
    Q_OBJECT
public:
    MonitoringPanel(EditorState* state, PlaybackController* program, QWidget* parent = nullptr);

    // The settings kept in the app's settings, put to the player (at startup).
    static void restore(PlaybackController* program);
    // Applies `s` (as the panel's controls do) and keeps it.
    void apply(const MonitorSettings& s);
    // Calibration: noise on `speaker` (-1 off); then the meter's reading sets the monitor level for the reference.
    void calibrate(int speaker);
    void applyReading(double measuredSpl);

    // MCP montage_monitoring (LiveLink app tool): read or change the section, calibrate, talk back.
    static QByteArray toolJson();
    QString runTool(const QJsonObject& args, QJsonObject& structured, bool& isError);

    // Mackie Control and HUI monitor keys (F1 Dim, F2 Mono, F3 Mute, F4 talkback while held).
    void toggleDim();
    void toggleMono();
    void toggleMute();

private:
    void refresh();
    void edited();  // the controls changed: their settings applied

    EditorState* state_;
    PlaybackController* program_;
    bool refreshing_ = false;
    QCheckBox* on_;
    QComboBox* output_;
    QComboBox* speakers_;
    QTableWidget* table_;
    QCheckBox* bass_;
    QDoubleSpinBox* crossover_;
    QComboBox* listen_;
    QToolButton* dim_;
    QToolButton* mono_;
    QToolButton* mute_;
    QDoubleSpinBox* level_;
    QComboBox* reference_;
    QPushButton* noise_;
    QDoubleSpinBox* reading_;
    QPushButton* applyReading_;
    QComboBox* soloMode_;
    QPushButton* clearSolos_;
    QCheckBox* cueOn_;
    QSpinBox* cueOutput_;
    QDoubleSpinBox* cueLevel_;
    QListWidget* cueTracks_;
    QCheckBox* cueBeeps_;
    QCheckBox* cueProgram_;
    QDoubleSpinBox* cueProgramLevel_;
    QComboBox* talkInput_;
    QToolButton* talk_;
};

}  // namespace montage
