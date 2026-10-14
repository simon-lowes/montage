// Montage — the project's media: bins, smart bins, and the media in the one
// shown, as thumbnails or as a list with metadata columns. Media are logged
// here: ratings (0–5 keys, X rejects), colour labels, keywords and fields
// such as scene and take, all searchable and usable in smart bin rules.
#pragma once

#include <QTreeWidget>
#include <QWidget>
#include <functional>
#include <optional>
#include <vector>

#include "core/Interpretation.h"
#include "core/Model.h"
#include "media/Proxy.h"

class QAbstractItemView;
class QLineEdit;
class QListView;
class QSortFilterProxyModel;
class QSplitter;
class QStackedWidget;
class QToolButton;
class QTreeView;

class QDialog;

namespace montage {

class EditorState;
class MediaBinModel;
class StoryboardView;

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
    // Proxies made as `options` say (where: the setting the dialog keeps), attached when written; the settings asked
    // first, and camera proxies found in a folder and attached (how many).
    void createProxies(const std::vector<Id>& ids, const ProxyOptions& options);
    std::optional<ProxyOptions> proxyOptionsDialog();
    int attachProxiesFrom(const std::vector<Id>& ids, const QString& folder);
    // (Tests: called with the options dialog before it runs, to fill it and accept it.)
    std::function<void(QDialog*)> proxyDialogHook_;
    enum class View { Icons, List, Board };

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
    // Log Slate from Picture: the scene, shot and take on a slate in the first seconds of each video read off its
    // picture (media/TextReader.h) into its metadata, one undo step. How many were logged.
    int logSlatesFromPicture(const std::vector<Id>& media);
    // Timecode from LTC: the LTC recorded on a sound channel of each file (media/LtcTimecode.h) becomes its timecode,
    // the LTC channel no longer played; one undo step. How many had LTC.
    int timecodeFromLtc(const std::vector<Id>& media);

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
    // Dual-system sound (media/DualSystem.h). Merge Clips: a camera clip and recorder files joined into one clip,
    // lined up by `syncBy` (0 timecode or else sound, 1 timecode, 2 sound, 3 their starts), the camera's own sound
    // kept muted under them when asked; one undo step; the merged clip, or 0 and why. Sync Dailies: every picture
    // among `media` merged with the sound file that belongs to it; one undo step; the merged clips.
    Id mergeClips(Id video, const std::vector<Id>& sounds, int syncBy, bool keepCameraAudio, const QString& name = {},
                  QString* why = nullptr);
    std::vector<Id> syncDailies(const std::vector<Id>& media, bool keepCameraAudio, QStringList* report = nullptr);
    // How new clips of the media take their channels (MediaItem::audioChannelMode: "", "mono", "pairs"); media with
    // a single channel keep the mix. One undo step; false if nothing changed.
    bool setAudioChannelMode(const std::vector<Id>& media, const std::string& mode);
    // Marks pictures as 360° footage ("equirect") or flat (""), as Premiere's Modify > VR Properties; one undo step.
    // Marks media as ambisonic sound (to `order`, or the highest its channels hold when lower) or channels for speakers
    // (0); false if nothing changed.
    bool setMediaAmbisonic(const std::vector<Id>& media, int order);
    bool setMediaProjection(const std::vector<Id>& media, const std::string& projection);
    // Interpret Footage on an image sequence (media/ImageSequence.h): its frames at `fps`; one undo step.
    bool setImageSequenceRate(Id media, double fps);
    // Interpret Footage (core/Interpretation.h) on each of `media`; one undo step. False (with a message) if none changed.
    bool interpretFootage(const std::vector<Id>& media, const Interpretation& how, bool keepHighlights = false);
    // The Interpret Footage dialog for `media`, applied when accepted.
    void interpretFootageDialog(const std::vector<Id>& media);
    // The Board view (core/Storyboard.h; Premiere's Freeform view): the shown media put in a grid in storyboard order,
    // one undo step; and the selection (or everything shown) assembled into the open sequence at the playhead on the
    // targeted tracks in storyboard order, inserted or over what is there, one undo step (the clips made).
    bool arrangeBoard();
    std::vector<Id> assembleStoryboard(bool insert);
    StoryboardView* board() const { return board_; }

public slots:
    void importDialog();
    void importImageSequenceDialog();
    void mergeClipsDialog(Id video, const std::vector<Id>& sounds);
    void rebuild();

signals:
    void openInSource(montage::Id media);
    void openInStackedTimeline(montage::Id sequence);
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
    StoryboardView* board_;
    QLineEdit* search_;
    QToolButton* iconsBtn_;
    QToolButton* listBtn_;
    QToolButton* boardBtn_;
    View view_ = View::Icons;
    bool hoverScrub_ = true;
    QString bin_;
    Id smart_ = 0;
    bool treeUpdating_ = false;
};

}  // namespace montage
