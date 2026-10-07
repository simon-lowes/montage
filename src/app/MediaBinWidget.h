// Montage — project media bin: import, thumbnails, search and drag to timeline.
#pragma once

#include <QListWidget>
#include <QWidget>

#include "core/Model.h"

class QLineEdit;
class QToolButton;

namespace montage {

class EditorState;

class MediaList : public QListWidget {
    Q_OBJECT
public:
    explicit MediaList(QWidget* parent = nullptr);

signals:
    void filesDropped(const QStringList& paths);

protected:
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override;
    QStringList mimeTypes() const override;
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dragMoveEvent(QDragMoveEvent* e) override;
    void dropEvent(QDropEvent* e) override;
};

class MediaBinWidget : public QWidget {
    Q_OBJECT
public:
    explicit MediaBinWidget(EditorState* state, QWidget* parent = nullptr);

public slots:
    void importDialog();
    void rebuild();

signals:
    void openInSource(montage::Id media);
    void newTitleRequested();
    void newSequenceRequested();
    void createMulticamRequested(const std::vector<montage::Id>& media);

private:
    void refreshThumbnails();
    void showContextMenu(const QPoint& pos);
    std::vector<Id> selectedMedia() const;
    void createProxies(const std::vector<Id>& ids);
    void transcribe(const std::vector<Id>& ids);

    EditorState* state_;
    MediaList* list_;
    QLineEdit* search_;
};

}  // namespace montage
