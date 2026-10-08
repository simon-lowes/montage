// Montage — the Render Queue panel: the queued exports with their status and
// progress, and Start, Stop, Remove, Retry, Clear Finished and Show File.
#pragma once

#include <QWidget>

class QPushButton;
class QTreeWidget;

namespace montage {

class RenderQueue;

class RenderQueuePanel : public QWidget {
    Q_OBJECT
public:
    explicit RenderQueuePanel(RenderQueue* queue, QWidget* parent = nullptr);

private:
    void refresh();
    int selectedJob() const;

    RenderQueue* queue_;
    QTreeWidget* list_;
    QPushButton* start_;
    QPushButton* stop_;
    QPushButton* remove_;
    QPushButton* retry_;
    QPushButton* clear_;
    QPushButton* reveal_;
};

}  // namespace montage
