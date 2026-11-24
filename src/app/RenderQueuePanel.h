// Montage — the Render Queue panel: the queued exports with their status and
// progress, and Start, Stop, Remove, Retry, Clear Finished and Show File; Cancel Job for one job, and how many render
// at once. Below them the render farm
// (render/RenderFarm.h): a folder shared with other computers, where Send to Farm puts the picked job for the render
// nodes there, its jobs and nodes shown as they go (read again on a worker every second and a half while the panel shows),
// with Cancel, Retry and Remove. Under the queue's own buttons, while there are any, the tasks the dialogs run in the
// background (DialogJobs.h).
#pragma once

#include <QFutureWatcher>
#include <QWidget>

#include "render/RenderFarm.h"

class QLabel;
class QSpinBox;
class QPushButton;
class QTimer;
class QTreeWidget;
class QVBoxLayout;

namespace montage {

class DialogJobs;
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
    // The dialogs' background tasks (DialogJobs.h) listed under the queue while there are any: those running or
    // waiting, with Pause, Move Up and Stop, and the results kept and offered, with Apply Result and Discard.
    void setDialogJobs(DialogJobs* jobs);

signals:
    void taskSaid(const QString& text);  // what applying a task's result did, for the status bar

private:
    struct FarmView {
        QString farm;
        std::vector<FarmJob> jobs;
        std::vector<FarmNodeInfo> nodes;
    };
    void refresh();
    void refreshTasks();
    QString selectedTask() const;
    void readFarmLater();
    void showFarm(const FarmView& view);
    int selectedJob() const;
    std::string selectedFarmJob() const;

    RenderQueue* queue_;
    DialogJobs* tasks_ = nullptr;
    QWidget* taskBox_ = nullptr;
    QTreeWidget* taskList_ = nullptr;
    QPushButton* taskPause_ = nullptr;
    QPushButton* taskUp_ = nullptr;
    QPushButton* taskStop_ = nullptr;
    QPushButton* taskApply_ = nullptr;
    QPushButton* taskDiscard_ = nullptr;
    QVBoxLayout* localLay_ = nullptr;
    QTreeWidget* list_;
    QPushButton* start_;
    QPushButton* stop_;
    QPushButton* cancel_;
    QSpinBox* atOnce_;
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
