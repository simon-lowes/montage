// Montage — Audio Plugins window: every plugin found with an on/off switch
// and its status, the blocklist, search folders, rescanning and the scan log.
#pragma once

#include <QDialog>
#include <functional>

#include "audio/Plugins.h"

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class QTableWidgetItem;

namespace montage {

class PluginManagerDialog : public QDialog {
    Q_OBJECT
public:
    explicit PluginManagerDialog(QWidget* parent = nullptr);

    // Applies the extra plugin folders saved in the settings to the registry.
    static void applySavedFolders();

signals:
    // The set of offered plugins may have changed (scan, enable/disable).
    void pluginsChanged();

private:
    void refresh();
    void startScan(const std::function<plugins::ScanReport()>& job);
    void setScanning(bool on);
    void editFolders();
    void itemChanged(QTableWidgetItem* item);

    QTableWidget* table_ = nullptr;
    QLabel* summary_ = nullptr;
    QPlainTextEdit* log_ = nullptr;
    QPushButton* rescan_ = nullptr;
    QPushButton* rescanSelected_ = nullptr;
    QPushButton* retry_ = nullptr;
    QPushButton* folders_ = nullptr;
    bool scanning_ = false;
    bool refreshing_ = false;
};

}  // namespace montage
