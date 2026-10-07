// Montage — Audio Plugins window: every plugin found, its status, the
// blocklist, and rescanning.
#pragma once

#include <QDialog>

class QLabel;
class QPushButton;
class QTableWidget;

namespace montage {

class PluginManagerDialog : public QDialog {
    Q_OBJECT
public:
    explicit PluginManagerDialog(QWidget* parent = nullptr);

signals:
    // The set of usable plugins may have changed (after a scan).
    void pluginsChanged();

private:
    void refresh();
    void startScan(bool retryBlocked);
    void setScanning(bool on);

    QTableWidget* table_ = nullptr;
    QLabel* summary_ = nullptr;
    QPushButton* rescan_ = nullptr;
    QPushButton* retry_ = nullptr;
    bool scanning_ = false;
};

}  // namespace montage
