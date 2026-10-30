// Montage — the Render Queue panel: the queued exports with their status and
// progress, and Start, Stop, Remove, Retry, Clear Finished and Show File. Below them the render farm
// (render/RenderFarm.h): a folder shared with other computers, where Send to Farm puts the picked job for the render
// nodes there, its jobs and nodes shown as they go (read again on a worker every second and a half while the panel shows),
// with Cancel, Retry and Remove.
#pragma once

#include <QFutureWatcher>
#include <QWidget>

#include "render/RenderFarm.h"

class QLabel;
class QSpinBox;
class QPushButton;
class QTimer;
class QTreeWidget;

namespace montage {

class RenderQueue;

class RenderQueuePanel : public QWidget {
    Q_OBJECT
public:
    explicit RenderQueuePanel(RenderQueue* queue, QWidget* parent = nullptr);

    // The farm's folder ("" none), kept in the settings.
    QString farmFolder() const { return farm_; }
    void setFarmFolder(const QString& folder);
    // The local job `id` (waiting, failed or cancelled) sent to the farm and taken off the queue; false with the error
    // shown.
    bool sendToFarm(int id);
    void refreshFarm();

private:
    struct FarmView {
        QString farm;
        std::vector<FarmJob> jobs;
        std::vector<FarmNodeInfo> nodes;
    };
    void refresh();
    void readFarmLater();
    void showFarm(const FarmView& view);
    int selectedJob() const;
    std::string selectedFarmJob() const;

    RenderQueue* queue_;
    QTreeWidget* list_;
    QPushButton* start_;
    QPushButton* stop_;
    QPushButton* remove_;
    QPushButton* retry_;
    QPushButton* clear_;
    QPushButton* reveal_;
    QPushButton* send_;
    QSpinBox* segments_;  // a job sent to the farm in this many (1: whole)
    QString farm_;
    QLabel* farmLabel_;
    QLabel* nodes_;
    QTreeWidget* farmList_;
    QPushButton* farmCancel_;
    QPushButton* farmRetry_;
    QPushButton* farmRemove_;
    QTimer* farmTimer_;
    FarmHeartbeats beats_;  // (rendering jobs and nodes that stopped answering)
    QFutureWatcher<FarmView> reading_;
};

}  // namespace montage
