// Montage — the media bin's items as a table: one row per media item, one
// column per field of core/MediaLog.h. The icon view shows the Name column
// (thumbnail with rating and label badges); the list view shows the columns,
// sortable and, for logging fields, editable.
#pragma once

#include <QAbstractTableModel>
#include <QPixmap>
#include <map>
#include <string>
#include <vector>

#include "core/Model.h"

namespace montage {

class EditorState;

class MediaBinModel : public QAbstractTableModel {
    Q_OBJECT
public:
    static constexpr int SortRole = Qt::UserRole + 1;
    static constexpr int IdRole = Qt::UserRole + 2;
    static constexpr int KeyRole = Qt::UserRole + 3;  // the column's field key (header data)
    static constexpr int kThumbW = 128;
    static constexpr int kThumbH = 72;

    explicit MediaBinModel(EditorState* state, QObject* parent = nullptr);

    // The field keys of the columns, in order ("name" first).
    static const std::vector<std::string>& columnKeys();
    static int columnOf(const std::string& key);

    // Shows these media items (in this order) and refreshes their data.
    void setMedia(const std::vector<Id>& ids);
    const std::vector<Id>& media() const { return ids_; }
    Id mediaAt(int row) const { return row >= 0 && row < int(ids_.size()) ? ids_[size_t(row)] : 0; }
    int rowOf(Id id) const;
    // Sets a field on several items as one undo step; false if nothing changed.
    bool setField(const std::vector<Id>& ids, const std::string& key, const QString& value);
    // Picks up new thumbnails.
    void refreshThumbnails();
    void forgetThumbnail(Id id);  // its file changed: decode the thumbnail again
    // Hover scrub (Premiere's hover scrub, Final Cut's skimming): the item's thumbnail shows its media's frame at
    // `seconds`, with a playhead line `fraction` of the way across, until clearSkim(). Videos only.
    void setSkim(Id id, double seconds, double fraction);
    void clearSkim();
    Id skimmed() const { return skimId_; }
    double skimSeconds() const { return skimSeconds_; }
    bool skimFrameShown() const { return skimShown_; }  // the frame at skimSeconds() has arrived and is drawn

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QModelIndexList& indexes) const override;
    Qt::DropActions supportedDragActions() const override { return Qt::CopyAction | Qt::MoveAction; }

private:
    QPixmap thumbnail(const MediaItem& m) const;
    QPixmap decorated(const MediaItem& m) const;
    QString toolTip(const MediaItem& m) const;

    EditorState* state_;
    std::vector<Id> ids_;
    std::map<Id, int> usage_;
    mutable std::map<Id, QPixmap> thumbs_;  // real thumbnails, once decoded
    struct Badge {
        int rating, label;
        qint64 thumb;
        bool offline;
        QPixmap pixmap;
    };
    mutable std::map<Id, Badge> decorated_;
    QPixmap skimmedThumbnail(const MediaItem& m) const;
    Id skimId_ = 0;
    double skimSeconds_ = 0, skimFraction_ = 0;
    mutable QImage skimFrame_;  // the latest frame decoded for the skimmed item, shown until the next arrives
    mutable bool skimShown_ = false;
};

}  // namespace montage
