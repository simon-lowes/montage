// Montage — the bar above the timeline that names a plug-in that failed in its host (crashed, stopped responding or
// used too much memory) and was bypassed, with Restart and Disable. It never blocks: playback and editing go on.
#pragma once

#include <QFrame>
#include <cstdint>
#include <string>

class QLabel;
class QPushButton;
class QTimer;

namespace montage {

class PluginFailureBar : public QFrame {
    Q_OBJECT
public:
    explicit PluginFailureBar(QWidget* parent = nullptr);
    // Looks at the failure list again (a timer does, a few times a second).
    void refresh();

signals:
    // The plug-in list may have changed (a plug-in disabled).
    void pluginsChanged();

private:
    QLabel* text_ = nullptr;
    QPushButton* restart_ = nullptr;
    QPushButton* disable_ = nullptr;
    QPushButton* close_ = nullptr;
    QTimer* timer_ = nullptr;
    uint64_t serial_ = ~uint64_t(0);
    int64_t dismissedAt_ = 0;  // the last failure's time when the bar was closed
    std::string pluginId_;     // the plug-in Restart and Disable act on
};

}  // namespace montage
