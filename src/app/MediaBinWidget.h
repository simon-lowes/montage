// Montage — the project's media: bins, smart bins, and the media in the one
// shown, as thumbnails or as a list with metadata columns. Media are logged
// here: ratings (0–5 keys, X rejects), colour labels, keywords and fields
// such as scene and take, all searchable and usable in smart bin rules.
#pragma once

#include <QTreeWidget>
#include <QWidget>
#include <vector>

#include "core/Model.h"

class QAbstractItemView;
class QLineEdit;
class QListView;
class QSortFilterProxyModel;
class QSplitter;
class QStackedWidget;
class QToolButton;
class QTreeView;

namespace montage {

class EditorState;
class MediaBinModel;

// The bin tree: media dropped on a bin move into it, bins dropped on a bin
// move inside it, and files dropped on a bin are imported into it.
class BinTree : public QTreeWidget {
    Q_OBJECT
public:
    enum ItemKind { BinItem = 1, SmartItem, HeaderItem };
    static constexpr int KindRole = Qt::UserRole;
    static constexpr int PathRole = Qt::UserRole + 1;  // bin path
    static constexpr int IdRole = Qt::UserRole + 2;    // smart bin id

    explicit BinTree(QWidget* parent = nullptr);

signals:
    void mediaDropped(const std::vector<montage::Id>& media, const QString& bin);
    void binDropped(const QString& bin, const QString& into);
    void filesDropped(const QStringList& files, const QString& bin);

protected:
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override;
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dragMoveEvent(QDragMoveEvent* e) override;
    void dropEvent(QDropEvent* e) override;

private:
    QTreeWidgetItem* binAt(const QPoint& pos) const;
};

class MediaBinWidget : public QWidget {
    Q_OBJECT
public:
    enum class View { Icons, List };

    explicit MediaBinWidget(EditorState* state, QWidget* parent = nullptr);

    void setView(View v);
    View view() const { return view_; }
    QAbstractItemView* currentView() const;
    MediaBinModel* model() const { return model_; }

    // Where the media shown come from: a bin ("" = the project root), or a smart bin.
    void showBin(const QString& bin);
    void showSmartBin(Id id);
    QString currentBin() const { return bin_; }
    Id currentSmartBin() const { return smart_; }
    std::vector<Id> shownMedia() const;  // in the order shown
    std::vector<Id> selectedMedia() const;
    void selectMedia(const std::vector<Id>& ids);

    // Logging, each one undo step.
    bool setRating(const std::vector<Id>& ids, int rating);
    bool setLabel(const std::vector<Id>& ids, int label);
    bool addKeywords(const std::vector<Id>& ids, const std::vector<std::string>& keywords);
    bool removeKeyword(const std::vector<Id>& ids, const std::string& keyword);
    bool moveToBin(const std::vector<Id>& ids, const QString& bin);
    // Bins: a new bin inside `parent` (returned), rename, move into another, delete (contents move up).
    QString newBin(const QString& parent);
    bool renameBin(const QString& bin, const QString& name);
    bool moveBin(const QString& bin, const QString& into);
    bool deleteBin(const QString& bin);
    // Smart bins: add one (returns its id), change one, delete one; or edit in the dialog.
    Id addSmartBin(const SmartBin& bin);
    bool updateSmartBin(const SmartBin& bin);
    bool deleteSmartBin(Id id);
    Id newSmartBinDialog();
    bool editSmartBinDialog(Id id);
    // Tags videos and subclips with keywords for what they show (shot size,
    // interior or exterior, day or night, people), indexing them first if
    // needed; one undo step. Returns how many items got new keywords.
    int autoTag(const std::vector<Id>& ids);
    // Hover scrub (Premiere's, Final Cut's skimming): moving the pointer across a video's thumbnail in the icon view
    // shows its frames, left to right. On unless turned off (the bin's context menu); remembered.
    void setHoverScrub(bool on);
    bool hoverScrub() const { return hoverScrub_; }
    // Replace Footage (Premiere's): the item takes another file, its clips keeping their edits; one undo step.
    bool replaceFootage(Id id, const QString& path, QString* why = nullptr);

public slots:
    void importDialog();
    void rebuild();

signals:
    void openInSource(montage::Id media);
    void newTitleRequested();
    void newSequenceRequested();
    void createMulticamRequested(const std::vector<montage::Id>& media);
    void linkMediaRequested();  // Link Media… for the offline items
    // Find Shots for moments like this video (its middle frame).
    void findSimilarRequested(montage::Id media);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void rebuildTree();
    void resetTree();
    void open(Id id);
    void showContextMenu(QAbstractItemView* view, const QPoint& pos);
    void showBinMenu(const QPoint& pos);
    void showColumnMenu(const QPoint& pos);
    void addKeywordsDialog(const std::vector<Id>& ids);
    void importInto(const QStringList& files, const QString& bin);
    void createProxies(const std::vector<Id>& ids);
    // Enlarged copies (2-4x) of videos and stills, written beside them and added to the bin.
    void createSuperScaleCopies(const std::vector<Id>& ids, int factor);
    void transcribe(const std::vector<Id>& ids);
    void saveColumns();
    void skimAt(const QPoint& viewportPos);

    EditorState* state_;
    MediaBinModel* model_;
    QSortFilterProxyModel* proxy_;
    QSplitter* split_;
    BinTree* tree_;
    QStackedWidget* stack_;
    QListView* icons_;
    QTreeView* list_;
    QLineEdit* search_;
    QToolButton* iconsBtn_;
    QToolButton* listBtn_;
    View view_ = View::Icons;
    bool hoverScrub_ = true;
    QString bin_;
    Id smart_ = 0;
    bool treeUpdating_ = false;
};

}  // namespace montage
