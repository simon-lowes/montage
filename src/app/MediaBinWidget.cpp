#include "MediaBinWidget.h"
#include <QCoreApplication>
#include "media/TextReader.h"
#include "Settings.h"

#include <QApplication>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QDialog>
#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QMimeData>
#include <QPointer>
#include <QProgressDialog>
#include <QSettings>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <algorithm>
#include <memory>

#include "EditorState.h"
#include "media/DualSystem.h"
#include "MediaBinModel.h"
#include "ModelPacks.h"
#include "ShotSearchPanel.h"
#include "SmartBinDialog.h"
#include "Theme.h"
#include "ThumbnailCache.h"
#include "media/Relink.h"
#include "TranscribeDialog.h"
#include "core/Slate.h"
#include "core/AudioChannels.h"
#include "media/ImageSequence.h"
#include "media/Psd.h"
#include "core/MediaLog.h"
#include "core/AutoTag.h"
#include "media/Analysis.h"
#include "media/VisualSearch.h"
#include "render/ColorSpace.h"

namespace montage {

namespace {

constexpr const char* kMediaMime = "application/x-montage-media";
constexpr const char* kBinMime = "application/x-montage-bin";

std::vector<Id> mediaFromMime(const QMimeData* mime) {
    std::vector<Id> ids;
    for (const QString& part : QString::fromUtf8(mime->data(kMediaMime)).split(',', Qt::SkipEmptyParts)) ids.push_back(part.toULongLong());
    return ids;
}

// A bin's name: no slashes (they separate nested bins).
QString cleanBinName(const QString& name) { return name.simplified().replace('/', '-'); }

QString starText(int n) { return n < 0 ? QObject::tr("Rejected") : n == 0 ? QObject::tr("Unrated") : QString(n, QChar(0x2605)); }

QStringList defaultColumns() {
    return {"name", "rating", "label", "duration", "kind", "resolution", "fps", "keywords", "usage", "comment"};
}

// Edits a list cell, and every selected row with it when the cell is in the selection.
class LogDelegate : public QStyledItemDelegate {
public:
    LogDelegate(QAbstractItemView* view, MediaBinModel* model) : QStyledItemDelegate(view), view_(view), model_(model) {}

    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        const std::string key = MediaBinModel::columnKeys()[size_t(index.column())];
        if (key == "rating") {
            auto* box = new QComboBox(parent);
            for (int r = -1; r <= 5; ++r) box->addItem(starText(r), r);
            return box;
        }
        if (key == "label") {
            auto* box = new QComboBox(parent);
            box->addItem(QObject::tr("None"), 0);
            for (int i = 1; i < theme::labelCount(); ++i) {
                QPixmap sw(10, 10);
                sw.fill(theme::labelColor(i));
                box->addItem(QIcon(sw), QObject::tr(labelName(i)), i);
            }
            return box;
        }
        return QStyledItemDelegate::createEditor(parent, option, index);
    }

    void setEditorData(QWidget* editor, const QModelIndex& index) const override {
        if (auto* box = qobject_cast<QComboBox*>(editor)) {
            const std::string key = MediaBinModel::columnKeys()[size_t(index.column())];
            const int value = key == "rating" ? index.data(Qt::EditRole).toInt() : index.data(MediaBinModel::SortRole).toInt();
            box->setCurrentIndex(std::max(0, box->findData(value)));
            return;
        }
        QStyledItemDelegate::setEditorData(editor, index);
    }

    void setModelData(QWidget* editor, QAbstractItemModel*, const QModelIndex& index) const override {
        QString value;
        if (auto* box = qobject_cast<QComboBox*>(editor)) value = box->currentData().toString();
        else if (auto* line = qobject_cast<QLineEdit*>(editor)) value = line->text();
        else return;
        std::vector<Id> ids;
        const QItemSelectionModel* sel = view_->selectionModel();
        if (sel->isSelected(index.siblingAtColumn(0)))
            for (const QModelIndex& r : sel->selectedIndexes())
                if (r.column() == 0) ids.push_back(r.data(MediaBinModel::IdRole).toULongLong());
        if (ids.empty()) ids.push_back(index.data(MediaBinModel::IdRole).toULongLong());
        model_->setField(ids, MediaBinModel::columnKeys()[size_t(index.column())], value);
    }

private:
    QAbstractItemView* view_;
    MediaBinModel* model_;
};

}  // namespace

// ---------------------------------------------------------------------------
// BinTree

BinTree::BinTree(QWidget* parent) : QTreeWidget(parent) {
    setHeaderHidden(true);
    setColumnCount(1);
    setDragEnabled(true);
    setAcceptDrops(true);
    viewport()->setAcceptDrops(true);
    setDropIndicatorShown(true);
    setDragDropMode(QAbstractItemView::DragDrop);
    setDefaultDropAction(Qt::MoveAction);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setEditTriggers(QAbstractItemView::EditKeyPressed | QAbstractItemView::SelectedClicked);
    setContextMenuPolicy(Qt::CustomContextMenu);
}

QStringList BinTree::mimeTypes() const { return {kBinMime, kMediaMime, "text/uri-list"}; }

QMimeData* BinTree::mimeData(const QList<QTreeWidgetItem*>& items) const {
    if (items.size() != 1 || items[0]->data(0, KindRole).toInt() != BinItem || items[0]->data(0, PathRole).toString().isEmpty())
        return nullptr;
    auto* m = new QMimeData;
    m->setData(kBinMime, items[0]->data(0, PathRole).toString().toUtf8());
    return m;
}

QTreeWidgetItem* BinTree::binAt(const QPoint& pos) const {
    QTreeWidgetItem* it = itemAt(pos);
    return it && it->data(0, KindRole).toInt() == BinItem ? it : nullptr;
}

void BinTree::dragEnterEvent(QDragEnterEvent* e) {
    const QMimeData* m = e->mimeData();
    if (m->hasFormat(kMediaMime) || m->hasFormat(kBinMime) || m->hasUrls()) e->acceptProposedAction();
    else e->ignore();
}

void BinTree::dragMoveEvent(QDragMoveEvent* e) {
    QTreeWidgetItem* target = binAt(e->position().toPoint());
    const QMimeData* m = e->mimeData();
    if (!target) {
        e->ignore();
        return;
    }
    if (m->hasFormat(kBinMime)) {
        const QString bin = QString::fromUtf8(m->data(kBinMime)), into = target->data(0, PathRole).toString();
        if (binWithin(into.toStdString(), bin.toStdString()) || binParent(bin.toStdString()) == into.toStdString()) {
            e->ignore();
            return;
        }
    }
    e->setDropAction(m->hasUrls() ? Qt::CopyAction : Qt::MoveAction);
    e->accept();
}

void BinTree::dropEvent(QDropEvent* e) {
    QTreeWidgetItem* target = binAt(e->position().toPoint());
    if (!target) {
        e->ignore();
        return;
    }
    const QString into = target->data(0, PathRole).toString();
    const QMimeData* m = e->mimeData();
    if (m->hasFormat(kMediaMime)) emit mediaDropped(mediaFromMime(m), into);
    else if (m->hasFormat(kBinMime)) emit binDropped(QString::fromUtf8(m->data(kBinMime)), into);
    else if (m->hasUrls()) {
        QStringList files;
        for (const QUrl& u : m->urls())
            if (u.isLocalFile()) files << u.toLocalFile();
        emit filesDropped(files, into);
    }
    // The tree is rebuilt from the project; nothing moves here.
    e->setDropAction(Qt::IgnoreAction);
    e->accept();
}

// ---------------------------------------------------------------------------
// MediaBinWidget

MediaBinWidget::MediaBinWidget(EditorState* state, QWidget* parent) : QWidget(parent), state_(state) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(2, 2, 2, 2);
    lay->setSpacing(2);
    auto* bar = new QHBoxLayout;
    auto addButton = [&](const QString& text, const QString& tip) {
        auto* b = new QToolButton(this);
        b->setText(text);
        b->setToolTip(tip);
        b->setAutoRaise(true);
        bar->addWidget(b);
        return b;
    };
    auto* importBtn = addButton(tr("Import"), tr("Import media files (Ctrl+I)"));
    auto* titleBtn = addButton(tr("Title"), tr("New title at the playhead"));
    auto* seqBtn = addButton(tr("Sequence"), tr("New sequence"));
    auto* binBtn = addButton(tr("Bin"), tr("New bin (the arrow: a new smart bin)"));
    binBtn->setObjectName(QStringLiteral("newBin"));
    auto* binMenu = new QMenu(binBtn);
    binMenu->addAction(tr("New Bin"), this, [this] { newBin(bin_); });
    binMenu->addAction(tr("New Smart Bin…"), this, [this] { newSmartBinDialog(); });
    binBtn->setMenu(binMenu);
    binBtn->setPopupMode(QToolButton::MenuButtonPopup);
    binBtn->setMinimumWidth(binBtn->fontMetrics().horizontalAdvance(binBtn->text()) + 30);  // room for the arrow beside it
    iconsBtn_ = addButton(QStringLiteral("▦"), tr("Icon view"));
    listBtn_ = addButton(QStringLiteral("☰"), tr("List view, with metadata columns"));
    iconsBtn_->setObjectName(QStringLiteral("iconViewButton"));
    listBtn_->setObjectName(QStringLiteral("listViewButton"));
    iconsBtn_->setCheckable(true);
    listBtn_->setCheckable(true);
    search_ = new QLineEdit(this);
    search_->setPlaceholderText(tr("Search media"));
    search_->setToolTip(tr("Finds names, keywords, metadata and what is said; \"quotes\" for a phrase"));
    search_->setClearButtonEnabled(true);
    bar->addWidget(search_, 1);
    lay->addLayout(bar);

    model_ = new MediaBinModel(state_, this);
    proxy_ = new QSortFilterProxyModel(this);
    proxy_->setSourceModel(model_);
    proxy_->setSortRole(MediaBinModel::SortRole);
    proxy_->setDynamicSortFilter(true);

    split_ = new QSplitter(Qt::Horizontal, this);
    tree_ = new BinTree(split_);
    tree_->setObjectName(QStringLiteral("binTree"));
    stack_ = new QStackedWidget(split_);
    icons_ = new QListView(stack_);
    icons_->setObjectName(QStringLiteral("mediaIcons"));
    icons_->setModel(proxy_);
    icons_->setViewMode(QListView::IconMode);
    icons_->setIconSize(QSize(MediaBinModel::kThumbW, MediaBinModel::kThumbH));
    icons_->setGridSize(QSize(MediaBinModel::kThumbW + 16, MediaBinModel::kThumbH + 40));
    icons_->setResizeMode(QListView::Adjust);
    icons_->setMovement(QListView::Static);
    icons_->setWordWrap(true);
    icons_->setUniformItemSizes(true);
    icons_->setTextElideMode(Qt::ElideMiddle);
    icons_->setEditTriggers(QAbstractItemView::EditKeyPressed);
    icons_->viewport()->setMouseTracking(true);  // hover scrub
    hoverScrub_ = appSettings().value(QStringLiteral("bin/hoverScrub"), true).toBool();
    list_ = new QTreeView(stack_);
    list_->setObjectName(QStringLiteral("mediaList"));
    list_->setModel(proxy_);
    list_->setRootIsDecorated(false);
    list_->setUniformRowHeights(true);
    list_->setSortingEnabled(true);
    list_->sortByColumn(-1, Qt::AscendingOrder);  // project order until a column is clicked
    list_->setAlternatingRowColors(true);
    list_->setIconSize(QSize(48, 27));
    list_->setEditTriggers(QAbstractItemView::EditKeyPressed | QAbstractItemView::SelectedClicked);
    list_->setItemDelegate(new LogDelegate(list_, model_));
    list_->header()->setSectionsMovable(true);
    list_->header()->setStretchLastSection(false);
    list_->header()->setContextMenuPolicy(Qt::CustomContextMenu);
    list_->header()->resizeSection(0, 200);
    QItemSelectionModel* old = list_->selectionModel();
    list_->setSelectionModel(icons_->selectionModel());  // one selection for both views
    delete old;
    for (QAbstractItemView* v : {static_cast<QAbstractItemView*>(icons_), static_cast<QAbstractItemView*>(list_)}) {
        v->setSelectionMode(QAbstractItemView::ExtendedSelection);
        v->setSelectionBehavior(QAbstractItemView::SelectRows);
        v->setDragEnabled(true);
        v->setDragDropMode(QAbstractItemView::DragOnly);
        v->viewport()->setAcceptDrops(true);  // files, imported into the bin shown
        v->viewport()->installEventFilter(this);
        v->installEventFilter(this);
        v->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(v, &QWidget::customContextMenuRequested, this, [this, v](const QPoint& pos) { showContextMenu(v, pos); });
        connect(v, &QAbstractItemView::activated, this,
                [this](const QModelIndex& i) { open(i.data(MediaBinModel::IdRole).toULongLong()); });
        stack_->addWidget(v);
    }
    split_->addWidget(tree_);
    split_->addWidget(stack_);
    split_->setStretchFactor(1, 1);
    split_->setSizes({130, 400});
    split_->setCollapsible(1, false);
    lay->addWidget(split_, 1);

    QSettings settings = appSettings();
    const QStringList visible = settings.value("mediaBin/columns", defaultColumns()).toStringList();
    for (int c = 0; c < model_->columnCount(); ++c)
        list_->setColumnHidden(c, c != 0 && !visible.contains(QString::fromStdString(MediaBinModel::columnKeys()[size_t(c)])));
    setView(settings.value("mediaBin/view").toString() == "list" ? View::List : View::Icons);

    connect(importBtn, &QToolButton::clicked, this, &MediaBinWidget::importDialog);
    connect(titleBtn, &QToolButton::clicked, this, &MediaBinWidget::newTitleRequested);
    connect(seqBtn, &QToolButton::clicked, this, &MediaBinWidget::newSequenceRequested);
    connect(binBtn, &QToolButton::clicked, this, [this] { newBin(bin_); });
    connect(iconsBtn_, &QToolButton::clicked, this, [this] { setView(View::Icons); });
    connect(listBtn_, &QToolButton::clicked, this, [this] { setView(View::List); });
    connect(search_, &QLineEdit::textChanged, this, &MediaBinWidget::rebuild);
    connect(list_->header(), &QWidget::customContextMenuRequested, this, &MediaBinWidget::showColumnMenu);
    connect(tree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* it) {
        if (treeUpdating_ || !it) return;
        if (it->data(0, BinTree::KindRole).toInt() == BinTree::SmartItem) showSmartBin(it->data(0, BinTree::IdRole).toULongLong());
        else if (it->data(0, BinTree::KindRole).toInt() == BinTree::BinItem) showBin(it->data(0, BinTree::PathRole).toString());
    });
    connect(tree_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* it) {
        if (treeUpdating_) return;
        const QString name = it->text(0).trimmed();
        if (it->data(0, BinTree::KindRole).toInt() == BinTree::BinItem) {
            if (!renameBin(it->data(0, BinTree::PathRole).toString(), name)) QTimer::singleShot(0, this, [this] { resetTree(); });
        } else if (it->data(0, BinTree::KindRole).toInt() == BinTree::SmartItem) {
            if (const SmartBin* b = findSmartBin(state_->project(), it->data(0, BinTree::IdRole).toULongLong())) {
                SmartBin changed = *b;
                changed.name = name.toStdString();
                if (name.isEmpty() || !updateSmartBin(changed)) QTimer::singleShot(0, this, [this] { resetTree(); });
            }
        }
    });
    connect(tree_, &QWidget::customContextMenuRequested, this, &MediaBinWidget::showBinMenu);
    connect(tree_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* it) {
        if (it->data(0, BinTree::KindRole).toInt() == BinTree::SmartItem) editSmartBinDialog(it->data(0, BinTree::IdRole).toULongLong());
    });
    connect(tree_, &BinTree::mediaDropped, this, [this](const std::vector<Id>& ids, const QString& bin) { moveToBin(ids, bin); });
    connect(tree_, &BinTree::binDropped, this, [this](const QString& bin, const QString& into) { moveBin(bin, into); });
    connect(tree_, &BinTree::filesDropped, this, [this](const QStringList& files, const QString& bin) { importInto(files, bin); });
    connect(state_, &EditorState::projectChanged, this, &MediaBinWidget::rebuild);
    connect(state_, &EditorState::sequenceSwitched, this, &MediaBinWidget::rebuild);
    connect(&ThumbnailCache::instance(), &ThumbnailCache::ready, model_, &MediaBinModel::refreshThumbnails);
    connect(state_, &EditorState::mediaFileChanged, model_, &MediaBinModel::forgetThumbnail);
    rebuild();
}

void MediaBinWidget::setView(View v) {
    view_ = v;
    stack_->setCurrentWidget(v == View::Icons ? static_cast<QWidget*>(icons_) : static_cast<QWidget*>(list_));
    iconsBtn_->setChecked(v == View::Icons);
    listBtn_->setChecked(v == View::List);
    QSettings("Montage", "Montage").setValue("mediaBin/view", v == View::List ? "list" : "icons");
}

QAbstractItemView* MediaBinWidget::currentView() const {
    return view_ == View::Icons ? static_cast<QAbstractItemView*>(icons_) : static_cast<QAbstractItemView*>(list_);
}

void MediaBinWidget::showBin(const QString& bin) {
    bin_ = bin;
    smart_ = 0;
    rebuild();
}

void MediaBinWidget::showSmartBin(Id id) {
    smart_ = id;
    rebuild();
}

std::vector<Id> MediaBinWidget::shownMedia() const {
    std::vector<Id> ids;
    for (int r = 0; r < proxy_->rowCount(); ++r) ids.push_back(proxy_->index(r, 0).data(MediaBinModel::IdRole).toULongLong());
    return ids;
}

std::vector<Id> MediaBinWidget::selectedMedia() const {
    std::vector<Id> ids;
    for (const QModelIndex& i : icons_->selectionModel()->selectedIndexes())
        if (i.column() == 0) ids.push_back(i.data(MediaBinModel::IdRole).toULongLong());
    return ids;
}

void MediaBinWidget::selectMedia(const std::vector<Id>& ids) {
    QItemSelection sel;
    for (Id id : ids)
        if (const int row = model_->rowOf(id); row >= 0) {
            const QModelIndex i = proxy_->mapFromSource(model_->index(row, 0));
            sel.select(i, i);
        }
    icons_->selectionModel()->select(sel, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    if (!sel.isEmpty()) icons_->selectionModel()->setCurrentIndex(sel.indexes().first(), QItemSelectionModel::NoUpdate);
}

void MediaBinWidget::importDialog() {
    QSettings settings = appSettings();
    QString dir = settings.value("lastImportDir").toString();
    QStringList files = QFileDialog::getOpenFileNames(
        this, tr("Import Media"), dir,
        tr("Media (*.mp4 *.mov *.mkv *.avi *.webm *.m4v *.mxf *.mts *.m2ts *.ts *.mpg *.mpeg *.wmv *.flv *.gif "
           "*.wav *.mp3 *.aac *.m4a *.flac *.ogg *.opus *.aif *.aiff *.png *.jpg *.jpeg *.tif *.tiff *.bmp *.webp *.exr *.svg *.json "
           "*.psd *.psb *.dng *.cr2 *.cr3 *.nef *.arw *.raf *.rw2 *.orf *.pef *.srw *.3fr *.iiq);;All files (*)"));
    if (files.isEmpty()) return;
    settings.setValue("lastImportDir", QFileInfo(files.first()).absolutePath());
    importInto(files, smart_ ? QString() : bin_);
}

void MediaBinWidget::importImageSequenceDialog() {
    QSettings settings = appSettings();
    const QString file = QFileDialog::getOpenFileName(this, tr("Import Image Sequence"), settings.value("lastImportDir").toString(),
                                                      tr("Frames (*.exr *.dpx *.png *.tif *.tiff *.tga *.bmp *.jpg *.jpeg *.webp);;All files (*)"));
    if (file.isEmpty()) return;
    settings.setValue("lastImportDir", QFileInfo(file).absolutePath());
    const Sequence* s = state_->sequence();
    bool ok = false;
    const double fps = QInputDialog::getDouble(this, tr("Import Image Sequence"), tr("Frames per second:"),
                                               s && s->fps.valid() ? s->fps.toDouble() : 24.0, 1, 240, 3, &ok);
    if (!ok) return;
    QString why;
    if (!state_->importImageSequence(file, rateFor(fps), &why)) QMessageBox::warning(this, tr("Import Image Sequence"), why);
}

bool MediaBinWidget::setImageSequenceRate(Id media, double fps) {
    const Rational rate = rateFor(fps);
    return state_->apply(tr("Interpret Frame Rate"), [media, rate](Project& p, Sequence&) { return edit::setImageSequenceRate(p, media, rate); });
}

void MediaBinWidget::importInto(const QStringList& files, const QString& bin) {
    QStringList errors, rest;
    // Layered Photoshop files: merged, as a still per layer, or as a sequence of their layers (as Premiere asks).
    QStringList psds;
    for (const QString& f : files) (isPsdFile(f.toStdString()) ? psds : rest) << f;
    if (!psds.isEmpty()) {
        QMessageBox ask(QMessageBox::Question, tr("Import Layered File"),
                        tr("How should %1 come in?").arg(psds.size() == 1 ? QFileInfo(psds.first()).fileName() : tr("these Photoshop files")),
                        QMessageBox::Cancel, this);
        ask.setObjectName(QStringLiteral("psdImportQuestion"));
        QPushButton* merged = ask.addButton(tr("Merged"), QMessageBox::AcceptRole);
        QPushButton* layers = ask.addButton(tr("Individual Layers"), QMessageBox::AcceptRole);
        QPushButton* sequence = ask.addButton(tr("Sequence"), QMessageBox::AcceptRole);
        merged->setObjectName(QStringLiteral("psdMerged"));
        layers->setObjectName(QStringLiteral("psdLayers"));
        sequence->setObjectName(QStringLiteral("psdSequence"));
        ask.setDefaultButton(sequence);
        ask.exec();
        if (ask.clickedButton() == merged) rest << psds;
        else if (ask.clickedButton() == layers || ask.clickedButton() == sequence)
            for (const QString& f : psds) {
                QString err;
                if (state_->importPsd(f, ask.clickedButton() == sequence ? PsdImport::Sequence : PsdImport::Layers, &err).empty()) errors << err;
            }
    }
    if (!rest.isEmpty()) state_->importFiles(rest, &errors, bin);
    if (!errors.isEmpty()) QMessageBox::warning(this, tr("Import"), errors.join("\n"));
}

void MediaBinWidget::rebuild() {
    const Project& p = state_->project();
    if (smart_ && !findSmartBin(p, smart_)) smart_ = 0;
    if (!bin_.isEmpty()) {
        const std::vector<std::string> bins = projectBins(p);
        if (std::find(bins.begin(), bins.end(), bin_.toStdString()) == bins.end()) bin_.clear();
    }
    rebuildTree();
    const std::string query = search_->text().trimmed().toStdString(), bin = bin_.toStdString();
    std::vector<Id> ids;
    if (const SmartBin* sb = findSmartBin(p, smart_)) {
        const std::map<Id, int> usage = mediaUsage(p);
        for (const MediaItem& m : p.media)
            if (smartBinMatches(*sb, m, usage, &p) && mediaMatchesSearch(m, query, &p)) ids.push_back(m.id);
    } else {
        // A bin shows what is in it; a search looks inside its bins too.
        for (const MediaItem& m : p.media)
            if (query.empty() ? m.bin == bin : binWithin(m.bin, bin) && mediaMatchesSearch(m, query, &p)) ids.push_back(m.id);
    }
    const std::vector<Id> keep = selectedMedia();
    model_->setMedia(ids);
    if (selectedMedia() != keep) selectMedia(keep);
}

// Rebuilds the tree even if the project's bins are unchanged (to undo a rename that failed).
void MediaBinWidget::resetTree() {
    tree_->setProperty("signature", QStringList());
    rebuildTree();
}

void MediaBinWidget::rebuildTree() {
    const Project& p = state_->project();
    const std::vector<std::string> bins = projectBins(p);
    QStringList signature{bin_, QString::number(smart_)};
    for (const std::string& b : bins) signature << QString::fromStdString(b);
    for (const SmartBin& b : p.smartBins) signature << QString::number(b.id) + ":" + QString::fromStdString(b.name);
    // The tree shows once there is a bin to show.
    const bool showTree = !bins.empty() || !p.smartBins.empty();
    if (showTree && tree_->isHidden()) {
        tree_->show();
        if (split_->sizes().value(0) < 60) split_->setSizes({130, std::max(200, split_->width() - 130)});
    } else if (!showTree) {
        tree_->hide();
    }
    if (tree_->property("signature").toStringList() == signature) return;
    tree_->setProperty("signature", signature);
    treeUpdating_ = true;
    tree_->clear();
    const QIcon folder = style()->standardIcon(QStyle::SP_DirIcon);
    auto* root = new QTreeWidgetItem(tree_, {tr("Project")});
    root->setIcon(0, style()->standardIcon(QStyle::SP_DirHomeIcon));
    root->setData(0, BinTree::KindRole, BinTree::BinItem);
    root->setData(0, BinTree::PathRole, QString());
    root->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDropEnabled);
    std::map<std::string, QTreeWidgetItem*> items{{"", root}};
    QTreeWidgetItem* current = smart_ ? nullptr : root;
    for (const std::string& b : bins) {
        auto* it = new QTreeWidgetItem(items[binParent(b)], {QString::fromStdString(binLeaf(b))});
        it->setIcon(0, folder);
        it->setData(0, BinTree::KindRole, BinTree::BinItem);
        it->setData(0, BinTree::PathRole, QString::fromStdString(b));
        it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled);
        items[b] = it;
        if (!smart_ && b == bin_.toStdString()) current = it;
    }
    if (!p.smartBins.empty()) {
        auto* header = new QTreeWidgetItem(tree_, {tr("Smart Bins")});
        header->setData(0, BinTree::KindRole, BinTree::HeaderItem);
        header->setFlags(Qt::ItemIsEnabled);
        QFont f = header->font(0);
        f.setBold(true);
        header->setFont(0, f);
        for (const SmartBin& b : p.smartBins) {
            auto* it = new QTreeWidgetItem(header, {QString::fromStdString(b.name)});
            it->setIcon(0, style()->standardIcon(QStyle::SP_FileDialogContentsView));
            it->setData(0, BinTree::KindRole, BinTree::SmartItem);
            it->setData(0, BinTree::IdRole, QVariant::fromValue<qulonglong>(b.id));
            it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable);
            it->setToolTip(0, tr("%n item(s); double-click to change its rules", "", int(smartBinMedia(p, b).size())));
            if (b.id == smart_) current = it;
        }
    }
    tree_->expandAll();
    tree_->setCurrentItem(current);
    treeUpdating_ = false;
}

void MediaBinWidget::open(Id id) {
    const MediaItem* m = state_->project().findMedia(id);
    if (!m) return;
    if (m->kind == MediaKind::Sequence) state_->setActiveSequence(m->sequenceId);
    else emit openInSource(id);
}

// ---------------------------------------------------------------------------
// Logging

bool MediaBinWidget::setRating(const std::vector<Id>& ids, int rating) {
    rating = std::clamp(rating, -1, 5);
    return state_->edit(rating < 0 ? tr("Reject") : tr("Rate %1").arg(starText(rating)), [ids, rating](Project& p, Sequence&) {
        bool any = false;
        for (Id id : ids)
            if (MediaItem* m = p.findMedia(id); m && m->rating != rating) {
                m->rating = rating;
                any = true;
            }
        return any;
    });
}

bool MediaBinWidget::setLabel(const std::vector<Id>& ids, int label) {
    return state_->edit(tr("Set Label"), [ids, label](Project& p, Sequence&) {
        bool any = false;
        for (Id id : ids)
            if (MediaItem* m = p.findMedia(id); m && m->label != label) {
                m->label = label;
                any = true;
            }
        return any;
    });
}

bool MediaBinWidget::addKeywords(const std::vector<Id>& ids, const std::vector<std::string>& keywords) {
    return state_->edit(tr("Add Keywords"), [ids, keywords](Project& p, Sequence&) {
        bool any = false;
        for (Id id : ids)
            if (MediaItem* m = p.findMedia(id)) any |= montage::addKeywords(m->keywords, keywords);
        return any;
    });
}

bool MediaBinWidget::removeKeyword(const std::vector<Id>& ids, const std::string& keyword) {
    return state_->edit(tr("Remove Keyword"), [ids, keyword](Project& p, Sequence&) {
        bool any = false;
        for (Id id : ids)
            if (MediaItem* m = p.findMedia(id)) any |= montage::removeKeywords(m->keywords, {keyword});
        return any;
    });
}

bool MediaBinWidget::moveToBin(const std::vector<Id>& ids, const QString& bin) {
    const std::string b = bin.toStdString();
    return state_->edit(tr("Move to Bin"), [ids, b](Project& p, Sequence&) { return moveMediaToBin(p, ids, b); });
}

QString MediaBinWidget::newBin(const QString& parent) {
    const std::string par = parent.toStdString();
    const std::string path = joinBin(par, uniqueBinName(state_->project(), par, tr("Bin").toStdString()));
    if (!state_->edit(tr("New Bin"), [path](Project& p, Sequence&) { return addBin(p, path); })) return {};
    rebuild();
    // Name it straight away.
    for (QTreeWidgetItemIterator it(tree_); *it; ++it)
        if ((*it)->data(0, BinTree::KindRole).toInt() == BinTree::BinItem && (*it)->data(0, BinTree::PathRole).toString().toStdString() == path) {
            tree_->scrollToItem(*it);
            if (tree_->isVisible()) tree_->editItem(*it);
        }
    return QString::fromStdString(path);
}

bool MediaBinWidget::renameBin(const QString& bin, const QString& name) {
    const QString leaf = cleanBinName(name);
    if (bin.isEmpty() || leaf.isEmpty()) return false;
    const std::string from = bin.toStdString(), to = joinBin(binParent(from), leaf.toStdString());
    if (to == from) return false;
    const bool showing = !smart_ && binWithin(bin_.toStdString(), from);
    if (!state_->edit(tr("Rename Bin"), [from, to](Project& p, Sequence&) { return montage::renameBin(p, from, to); })) return false;
    if (showing) showBin(QString::fromStdString(to + bin_.toStdString().substr(from.size())));
    return true;
}

bool MediaBinWidget::moveBin(const QString& bin, const QString& into) {
    const std::string from = bin.toStdString(), to = joinBin(into.toStdString(), binLeaf(from));
    const bool showing = !smart_ && binWithin(bin_.toStdString(), from);
    if (!state_->edit(tr("Move Bin"), [from, into = into.toStdString()](Project& p, Sequence&) { return montage::moveBin(p, from, into); }))
        return false;
    if (showing) showBin(QString::fromStdString(to + bin_.toStdString().substr(from.size())));
    return true;
}

bool MediaBinWidget::deleteBin(const QString& bin) {
    const std::string b = bin.toStdString();
    if (!state_->edit(tr("Delete Bin"), [b](Project& p, Sequence&) { return removeBin(p, b); })) return false;
    if (!smart_ && binWithin(bin_.toStdString(), b)) showBin(QString::fromStdString(binParent(b)));
    return true;
}

Id MediaBinWidget::addSmartBin(const SmartBin& bin) {
    Id id = 0;
    state_->edit(tr("New Smart Bin"), [&](Project& p, Sequence&) {
        SmartBin b = bin;
        b.id = id = p.newId();
        if (b.name.empty()) b.name = tr("Smart Bin").toStdString();
        p.smartBins.push_back(std::move(b));
        return true;
    });
    if (id) showSmartBin(id);
    return id;
}

bool MediaBinWidget::updateSmartBin(const SmartBin& bin) {
    return state_->edit(tr("Change Smart Bin"), [bin](Project& p, Sequence&) {
        SmartBin* b = findSmartBin(p, bin.id);
        if (!b || *b == bin) return false;
        *b = bin;
        return true;
    });
}

bool MediaBinWidget::deleteSmartBin(Id id) {
    return state_->edit(tr("Delete Smart Bin"), [id](Project& p, Sequence&) {
        return std::erase_if(p.smartBins, [id](const SmartBin& b) { return b.id == id; }) > 0;
    });
}

Id MediaBinWidget::newSmartBinDialog() {
    SmartBin b;
    b.name = tr("Smart Bin").toStdString();
    SmartBinDialog dlg(state_->project(), b, this);
    if (dlg.exec() != QDialog::Accepted) return 0;
    return addSmartBin(dlg.bin());
}

bool MediaBinWidget::editSmartBinDialog(Id id) {
    const SmartBin* b = findSmartBin(state_->project(), id);
    if (!b) return false;
    SmartBinDialog dlg(state_->project(), *b, this);
    if (dlg.exec() != QDialog::Accepted) return false;
    return updateSmartBin(dlg.bin());
}

int MediaBinWidget::autoTag(const std::vector<Id>& ids) {
    if (!visualSearchAvailable()) return 0;
    if (!ensureModelPack(window(), visualModel(), tr("Auto-Tag"),
                         tr("Tagging footage by what it shows uses CLIP (OpenAI, MIT licence), which runs on this computer.")))
        return 0;
    if (!indexVideos(state_, ids, window())) return 0;
    std::string err;
    const auto model = ClipModel::load(&err);
    const LabelEmbeddings labels = model ? model->labels(&err) : LabelEmbeddings{};
    if (labels.empty()) {
        state_->message(QString::fromStdString(err), 6000);
        return 0;
    }
    std::vector<std::pair<Id, std::vector<std::string>>> tags;
    for (Id id : ids)
        if (const MediaItem* m = state_->project().findMedia(id)) tags.emplace_back(id, autoTagMedia(state_->project(), *m, labels).keywords);
    int changed = 0;
    state_->edit(tr("Auto-Tag"), [&](Project& p, Sequence&) {
        for (const auto& [id, keywords] : tags)
            if (MediaItem* m = p.findMedia(id)) changed += montage::addKeywords(m->keywords, keywords) ? 1 : 0;
        return changed > 0;
    });
    state_->message(changed ? tr("Tagged %n item(s) with what they show", "", changed) : tr("No new tags"), 5000);
    return changed;
}

void MediaBinWidget::addKeywordsDialog(const std::vector<Id>& ids) {
    QInputDialog dlg(this);
    dlg.setWindowTitle(tr("Add Keywords"));
    dlg.setLabelText(tr("Keywords, separated by commas:"));
    if (auto* line = dlg.findChild<QLineEdit*>()) {
        QStringList known;
        for (const std::string& k : projectKeywords(state_->project())) known << QString::fromStdString(k);
        auto* completer = new QCompleter(known, line);
        completer->setCaseSensitivity(Qt::CaseInsensitive);
        line->setCompleter(completer);
    }
    if (dlg.exec() != QDialog::Accepted) return;
    addKeywords(ids, parseKeywords(dlg.textValue().toStdString()));
}

Id MediaBinWidget::mergeClips(Id video, const std::vector<Id>& sounds, int syncBy, bool keepCameraAudio, const QString& name, QString* why) {
    const Project& proj = state_->project();
    std::vector<double> offsets;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    for (Id a : sounds) {
        SoundSync found;
        if (syncBy == 3) found.found = true;  // the starts together
        else found = syncSound(proj, video, a, syncBy == 1 ? SyncBy::Timecode : syncBy == 2 ? SyncBy::Waveform : SyncBy::Auto);
        if (!found.found) {
            QApplication::restoreOverrideCursor();
            const MediaItem* m = proj.findMedia(a);
            if (why)
                *why = syncBy == 1 ? tr("%1 and the picture have no timecode in common").arg(m ? QString::fromStdString(m->name) : QString())
                                   : tr("Could not line up %1 with the picture by its sound").arg(m ? QString::fromStdString(m->name) : QString());
            return 0;
        }
        offsets.push_back(found.offset);
    }
    QApplication::restoreOverrideCursor();
    Id made = 0;
    std::string err;
    MergeOptions o;
    o.name = name.trimmed().toStdString();
    o.keepCameraAudio = keepCameraAudio;
    state_->edit(tr("Merge Clips"), [&](Project& p, Sequence&) { return (made = montage::mergeClips(p, video, sounds, offsets, o, &err)) != 0; });
    if (!made && why) *why = QString::fromStdString(err);
    return made;
}

bool MediaBinWidget::setMediaProjection(const std::vector<Id>& media, const std::string& projection) {
    return state_->edit(tr("360° Footage"), [media, projection](Project& p, Sequence&) {
        bool any = false;
        for (Id id : media)
            if (MediaItem* m = p.findMedia(id); m && m->hasVideo && m->kind != MediaKind::Sequence && m->projection != projection) {
                m->projection = projection;
                any = true;
            }
        return any;
    });
}

bool MediaBinWidget::setAudioChannelMode(const std::vector<Id>& media, const std::string& mode) {
    return state_->edit(tr("Audio Channels"), [media, mode](Project& p, Sequence&) {
        bool any = false;
        for (Id id : media)
            if (MediaItem* m = p.findMedia(id); m && sourceChannelCount(*m) > 1 && m->audioChannelMode != mode) {
                m->audioChannelMode = mode;
                any = true;
            }
        return any;
    });
}

std::vector<Id> MediaBinWidget::syncDailies(const std::vector<Id>& media, bool keepCameraAudio, QStringList* report) {
    std::vector<std::string> lines;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const std::vector<DailiesMatch> matches = matchDailies(state_->project(), media, &lines);
    QApplication::restoreOverrideCursor();
    std::vector<Id> made;
    if (!matches.empty())
        state_->edit(tr("Sync Dailies"), [&](Project& p, Sequence&) {
            made = mergeDailies(p, matches, keepCameraAudio, &lines);
            return !made.empty();
        });
    if (report)
        for (const std::string& l : lines) *report << QString::fromStdString(l);
    return made;
}

void MediaBinWidget::mergeClipsDialog(Id video, const std::vector<Id>& sounds) {
    const MediaItem* v = state_->project().findMedia(video);
    if (!v) return;
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Merge Clips"));
    auto* form = new QFormLayout(&dlg);
    auto* name = new QLineEdit(QString::fromStdString(v->name) + tr(" (merged)"), &dlg);
    form->addRow(tr("Name:"), name);
    auto* by = new QComboBox(&dlg);
    by->addItems({tr("Timecode, or else sound"), tr("Timecode"), tr("Sound (waveform)"), tr("Their starts")});
    by->setToolTip(tr("How to line up the recorder's sound with the picture"));
    form->addRow(tr("Synchronise by:"), by);
    auto* keep = new QCheckBox(tr("Keep the camera's sound (muted)"), &dlg);
    keep->setChecked(true);
    form->addRow(QString(), keep);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);
    if (dlg.exec() != QDialog::Accepted) return;
    QString why;
    if (!mergeClips(video, sounds, by->currentIndex(), keep->isChecked(), name->text(), &why))
        state_->message(tr("Could not merge the clips: %1").arg(why), 8000);
}

bool MediaBinWidget::replaceFootage(Id id, const QString& path, QString* why) {
    std::string reason;
    const bool ok = state_->edit(tr("Replace Footage"), [&](Project& p, Sequence&) {
        return relinkMedia(p, id, QDir::cleanPath(path).toStdString(), RelinkCheck::Replace, &reason);
    });
    state_->recheckOffline();
    if (!ok && why) *why = QString::fromStdString(reason);
    return ok;
}

void MediaBinWidget::setHoverScrub(bool on) {
    hoverScrub_ = on;
    if (!on) model_->clearSkim();
    appSettings().setValue(QStringLiteral("bin/hoverScrub"), on);
}

void MediaBinWidget::skimAt(const QPoint& pos) {
    const QModelIndex vi = icons_->indexAt(pos);
    const Id id = vi.isValid() ? vi.data(MediaBinModel::IdRole).toULongLong() : 0;
    const MediaItem* m = id ? state_->project().findMedia(id) : nullptr;
    if (!m || m->kind != MediaKind::Video || m->duration <= 0 || state_->isMediaOffline(id)) {
        model_->clearSkim();
        return;
    }
    // Across the thumbnail, centred at the top of the item's cell.
    const QRect cell = icons_->visualRect(vi);
    const double x0 = cell.center().x() - MediaBinModel::kThumbW / 2.0;
    const double u = std::clamp((pos.x() - x0) / MediaBinModel::kThumbW, 0.0, 1.0);
    const bool sub = m->subclipOut > m->subclipIn;
    const double start = sub ? m->subclipIn : 0.0, span = sub ? m->subclipOut - m->subclipIn : m->duration;
    model_->setSkim(id, start + u * span * 0.999, u);
}

bool MediaBinWidget::eventFilter(QObject* watched, QEvent* event) {
    // Hover scrub over the icon view's thumbnails (not while dragging).
    if (watched == icons_->viewport()) {
        if (event->type() == QEvent::MouseMove && hoverScrub_) {
            auto* me = static_cast<QMouseEvent*>(event);
            if (!(me->buttons() & Qt::LeftButton)) skimAt(me->position().toPoint());
        } else if (event->type() == QEvent::Leave) {
            model_->clearSkim();
        }
    }
    const bool view = watched == icons_ || watched == list_;
    // Rating keys: 0–5 and X (reject), over the window's shortcuts while a view has the focus.
    if (view && (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress)) {
        auto* ke = static_cast<QKeyEvent*>(event);
        const Qt::KeyboardModifiers mods = ke->modifiers() & ~Qt::KeypadModifier;
        const int key = ke->key();
        const bool rating = mods == Qt::NoModifier && ((key >= Qt::Key_0 && key <= Qt::Key_5) || key == Qt::Key_X);
        if (rating && !selectedMedia().empty()) {  // (an open editor has the focus, not the view)
            ke->accept();
            if (event->type() == QEvent::KeyPress) {
                const std::vector<Id> ids = selectedMedia();
                if (key == Qt::Key_X) {
                    bool allRejected = true;
                    for (Id id : ids)
                        if (const MediaItem* m = state_->project().findMedia(id)) allRejected &= m->rating < 0;
                    setRating(ids, allRejected ? 0 : -1);
                } else {
                    setRating(ids, key - Qt::Key_0);
                }
            }
            return true;
        }
    }
    // Files dropped on a view are imported into the bin shown.
    if (!view && (watched == icons_->viewport() || watched == list_->viewport())) {
        if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove) {
            auto* e = static_cast<QDragMoveEvent*>(event);
            if (e->mimeData()->hasUrls()) {
                e->acceptProposedAction();
                return true;
            }
        } else if (event->type() == QEvent::Drop) {
            auto* e = static_cast<QDropEvent*>(event);
            if (e->mimeData()->hasUrls()) {
                QStringList files;
                for (const QUrl& u : e->mimeData()->urls())
                    if (u.isLocalFile()) files << u.toLocalFile();
                e->acceptProposedAction();
                importInto(files, smart_ ? QString() : bin_);
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

// ---------------------------------------------------------------------------
// Menus

void MediaBinWidget::showColumnMenu(const QPoint& pos) {
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("binColumns"));
    for (int c = 1; c < model_->columnCount(); ++c) {
        QAction* a = menu.addAction(model_->headerData(c, Qt::Horizontal).toString());
        a->setCheckable(true);
        a->setChecked(!list_->isColumnHidden(c));
        a->setData(c);
        connect(a, &QAction::toggled, this, [this, c](bool on) {
            list_->setColumnHidden(c, !on);
            saveColumns();
        });
    }
    menu.addSeparator();
    menu.addAction(tr("Default Columns"), this, [this] {
        const QStringList d = defaultColumns();
        for (int c = 1; c < model_->columnCount(); ++c)
            list_->setColumnHidden(c, !d.contains(QString::fromStdString(MediaBinModel::columnKeys()[size_t(c)])));
        saveColumns();
    });
    menu.exec(list_->header()->mapToGlobal(pos));
}

void MediaBinWidget::saveColumns() {
    QStringList visible;
    for (int c = 0; c < model_->columnCount(); ++c)
        if (!list_->isColumnHidden(c)) visible << QString::fromStdString(MediaBinModel::columnKeys()[size_t(c)]);
    QSettings("Montage", "Montage").setValue("mediaBin/columns", visible);
}

void MediaBinWidget::showBinMenu(const QPoint& pos) {
    QTreeWidgetItem* it = tree_->itemAt(pos);
    const int kind = it ? it->data(0, BinTree::KindRole).toInt() : 0;
    const QString path = kind == BinTree::BinItem ? it->data(0, BinTree::PathRole).toString() : QString();
    QMenu menu(this);
    menu.addAction(tr("New Bin"), this, [this, path] { newBin(path); });
    menu.addAction(tr("New Smart Bin…"), this, [this] { newSmartBinDialog(); });
    if (kind == BinTree::BinItem && !path.isEmpty()) {
        menu.addSeparator();
        menu.addAction(tr("Rename"), this, [this, it] { tree_->editItem(it); });
        menu.addAction(tr("Delete Bin (Keep Its Media)"), this, [this, path] { deleteBin(path); })
            ->setObjectName(QStringLiteral("deleteBin"));
    } else if (kind == BinTree::SmartItem) {
        const Id id = it->data(0, BinTree::IdRole).toULongLong();
        menu.addSeparator();
        menu.addAction(tr("Edit Smart Bin…"), this, [this, id] { editSmartBinDialog(id); });
        menu.addAction(tr("Rename"), this, [this, it] { tree_->editItem(it); });
        menu.addAction(tr("Delete Smart Bin"), this, [this, id] { deleteSmartBin(id); });
    }
    menu.exec(tree_->viewport()->mapToGlobal(pos));
}

void MediaBinWidget::showContextMenu(QAbstractItemView* view, const QPoint& pos) {
    auto ids = selectedMedia();
    QMenu menu(this);
    if (ids.size() == 1) {
        Id id = ids.front();
        const MediaItem* m = state_->project().findMedia(id);
        if (m && m->kind == MediaKind::Sequence) {
            menu.addAction(tr("Open Sequence"), this, [this, m] { state_->setActiveSequence(m->sequenceId); });
        } else {
            menu.addAction(tr("Open in Source Monitor"), this, [this, id] { emit openInSource(id); });
        }
        if (m && !m->path.empty() && m->kind != MediaKind::Sequence)
            menu.addAction(tr("Replace Footage..."), this, [this, id] {
                const QString f = QFileDialog::getOpenFileName(this, tr("Replace Footage"));
                QString why;
                if (!f.isEmpty() && !replaceFootage(id, f, &why)) state_->message(tr("Could not replace the footage: %1").arg(why));
            });
        if (m && !m->path.empty() && m->kind != MediaKind::Sequence)
            menu.addAction(tr("Edit Original"), this, [m] {  // in its own application; saved changes reload by themselves
                QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(m->path)));
            });
        if (m && !m->path.empty())
            menu.addAction(tr("Reveal in File Manager"), this, [m] {
                QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(QString::fromStdString(m->path)).absolutePath()));
            });
        menu.addAction(tr("Rename..."), this, [this, id, m] {
            bool ok = false;
            QString name = QInputDialog::getText(this, tr("Rename"), tr("Name:"), QLineEdit::Normal,
                                                 QString::fromStdString(m->name), &ok);
            if (!ok || name.isEmpty()) return;
            model_->setField({id}, "name", name);
        });
    }
    if (std::any_of(ids.begin(), ids.end(), [this](Id i) { return state_->isMediaOffline(i); }))
        menu.addAction(tr("Link Media..."), this, [this] { emit linkMediaRequested(); });
    {
        // Dual-system sound: camera clips with a recorder's files.
        std::vector<Id> pictures, sounds;
        for (Id id : ids)
            if (const MediaItem* m = state_->project().findMedia(id)) {
                if (m->kind == MediaKind::Video && m->hasVideo) pictures.push_back(id);
                else if (m->kind == MediaKind::Audio && m->hasAudio) sounds.push_back(id);
            }
        if (pictures.size() == 1 && !sounds.empty())
            menu.addAction(tr("Merge Clips…"), this, [this, pictures, sounds] { mergeClipsDialog(pictures.front(), sounds); })
                ->setObjectName(QStringLiteral("mergeClips"));
        if (!pictures.empty() && !sounds.empty())
            menu.addAction(tr("Sync Dailies"), this, [this, ids] {
                QStringList report;
                const auto made = syncDailies(ids, true, &report);
                state_->message(made.empty() ? tr("No picture could be matched with a sound file")
                                             : tr("Merged %n clip(s): %1", "", int(made.size())).arg(report.join(QStringLiteral("; "))),
                                8000);
            })->setObjectName(QStringLiteral("syncDailies"));
    }
    if (!ids.empty()) {
        // Logging.
        menu.addSeparator();
        const MediaItem* first = state_->project().findMedia(ids.front());
        auto same = [&](auto get) {
            for (Id id : ids)
                if (const MediaItem* m = state_->project().findMedia(id); m && first && get(*m) != get(*first)) return false;
            return first != nullptr;
        };
        QMenu* rating = menu.addMenu(tr("Rating"));
        rating->setObjectName(QStringLiteral("ratingMenu"));
        const bool sameRating = same([](const MediaItem& m) { return m.rating; });
        for (int r : {-1, 0, 1, 2, 3, 4, 5}) {
            QAction* a = rating->addAction(starText(r), this, [this, ids, r] { setRating(ids, r); });
            a->setCheckable(true);
            a->setChecked(sameRating && first->rating == r);
            a->setShortcut(r < 0 ? QKeySequence(Qt::Key_X) : QKeySequence(Qt::Key_0 + r));
            a->setData(r);
        }
        QMenu* label = menu.addMenu(tr("Label"));
        label->setObjectName(QStringLiteral("labelMenu"));
        const bool sameLabel = same([](const MediaItem& m) { return m.label; });
        for (int i = 0; i < theme::labelCount(); ++i) {
            QAction* a = label->addAction(i == 0 ? tr("None") : tr(labelName(i)), this, [this, ids, i] { setLabel(ids, i); });
            if (i > 0) {
                QPixmap sw(12, 12);
                sw.fill(theme::labelColor(i));
                a->setIcon(QIcon(sw));
            }
            a->setCheckable(true);
            a->setChecked(sameLabel && first->label == i);
            a->setData(i);
        }
        menu.addAction(tr("Add Keywords…"), this, [this, ids] { addKeywordsDialog(ids); })->setObjectName(QStringLiteral("addKeywords"));
        std::vector<Id> taggable;
        for (Id id : ids)
            if (const MediaItem* m = state_->project().findMedia(id); m && m->kind == MediaKind::Video && m->hasVideo) taggable.push_back(id);
        if (!taggable.empty() && visualSearchAvailable())
            menu.addAction(tr("Auto-Tag Shots"), this, [this, taggable] { autoTag(taggable); })
                ->setToolTip(tr("Add keywords for what each shot shows: close-up, medium or wide, interior or exterior, day or night, people"));
        std::vector<std::string> present;
        for (Id id : ids)
            if (const MediaItem* m = state_->project().findMedia(id)) montage::addKeywords(present, m->keywords);
        if (!present.empty()) {
            QMenu* remove = menu.addMenu(tr("Remove Keyword"));
            remove->setObjectName(QStringLiteral("removeKeyword"));
            for (const std::string& k : present)
                remove->addAction(QString::fromStdString(k), this, [this, ids, k] { removeKeyword(ids, k); });
        }
        QMenu* move = menu.addMenu(tr("Move to Bin"));
        move->setObjectName(QStringLiteral("moveToBin"));
        move->addAction(tr("Project"), this, [this, ids] { moveToBin(ids, {}); })->setData(QString());
        for (const std::string& b : projectBins(state_->project()))
            move->addAction(QString(int(std::count(b.begin(), b.end(), '/')) * 2, ' ') + QString::fromStdString(binLeaf(b)), this,
                            [this, ids, b] { moveToBin(ids, QString::fromStdString(b)); })
                ->setData(QString::fromStdString(b));
        move->addSeparator();
        move->addAction(tr("New Bin…"), this, [this, ids] {
            const QString bin = newBin(smart_ ? QString() : bin_);
            if (!bin.isEmpty()) moveToBin(ids, bin);
        });
    }
    // Transcripts, proxies and colour belong to a subclip's media.
    std::vector<Id> files;
    for (Id id : ids)
        if (const MediaItem* m = state_->project().findMedia(id)) {
            const Id f = m->subclipOf ? m->subclipOf : id;
            if (std::find(files.begin(), files.end(), f) == files.end()) files.push_back(f);
        }
    std::vector<Id> withSound;
    for (Id id : files)
        if (const MediaItem* m = state_->project().findMedia(id); m && m->hasAudio && !m->path.empty()) withSound.push_back(id);
    if (!withSound.empty()) {
        menu.addSeparator();
        menu.addAction(tr("Transcribe..."), this, [this, withSound] { transcribe(withSound); });
        if (files.size() == 1)
            if (const MediaItem* m = state_->project().findMedia(files.front()); m && m->transcript) {
                menu.addAction(tr("Export Transcript..."), this, [this, id = files.front()] {
                    if (const MediaItem* mi = state_->project().findMedia(id)) exportTranscript(*mi, this);
                });
            }
        bool anyTranscript = false;
        for (Id id : withSound)
            if (const MediaItem* m = state_->project().findMedia(id)) anyTranscript |= bool(m->transcript);
        if (anyTranscript)
            menu.addAction(tr("Log from Spoken Slate"), this, [this, withSound] {
                int n = 0;
                state_->edit(tr("Log from Spoken Slate"), [withSound, &n](Project& p, Sequence&) {
                    n = logFromSlates(p, withSound);
                    return n > 0;
                });
                state_->message(n ? tr("Logged scene, shot and take for %n clip(s) from their spoken slates", nullptr, n)
                                  : tr("No slate (\"Scene 12 apple, take 3\") was heard at the head of these clips"),
                                5000);
            })->setObjectName(QStringLiteral("logFromSlate"));
        bool anyVideo = false;
        for (Id id : files)
            if (const MediaItem* m = state_->project().findMedia(id)) anyVideo |= m->kind == MediaKind::Video;
        if (anyVideo)
            menu.addAction(tr("Log Slate from Picture"), this, [this, files] { logSlatesFromPicture(files); })
                ->setObjectName(QStringLiteral("logSlateFromPicture"));
        if (anyTranscript)
            menu.addAction(tr("Remove Transcript"), this, [this, withSound] {
                state_->edit(tr("Remove Transcript"), [withSound](Project& p, Sequence&) {
                    bool any = false;
                    for (Id id : withSound)
                        if (MediaItem* m = p.findMedia(id); m && m->transcript) {
                            m->transcript.reset();
                            any = true;
                        }
                    return any;
                });
            });
    }
    std::vector<Id> videos;
    for (Id id : files)
        if (const MediaItem* m = state_->project().findMedia(id); m && m->kind == MediaKind::Video && m->hasVideo) videos.push_back(id);
    std::vector<Id> pictures;
    for (Id id : files)
        if (const MediaItem* m = state_->project().findMedia(id); m && m->hasVideo && m->kind != MediaKind::Sequence)
            pictures.push_back(id);
    if (!pictures.empty()) {
        menu.addSeparator();
        // Interpret Colour: read the media as another colour space (camera log, HDR without tags...).
        QMenu* interpret = menu.addMenu(tr("Interpret Colour"));
        interpret->setObjectName(QStringLiteral("interpretColour"));
        const MediaItem* first = state_->project().findMedia(pictures.front());
        std::string current = first ? first->colorOverride : std::string();
        for (Id id : pictures)
            if (const MediaItem* m = state_->project().findMedia(id); m && m->colorOverride != current) current = "?";
        auto setOverride = [this, pictures](const std::string& space) {
            state_->edit(tr("Interpret Colour"), [pictures, space](Project& p, Sequence&) {
                bool any = false;
                for (Id id : pictures)
                    if (MediaItem* m = p.findMedia(id); m && m->colorOverride != space) {
                        m->colorOverride = space;
                        any = true;
                    }
                return any;
            });
        };
        QAction* detected = interpret->addAction(tr("As Detected"), this, [setOverride] { setOverride({}); });
        detected->setCheckable(true);
        detected->setChecked(current.empty());
        if (first && pictures.size() == 1) {
            MediaItem plain = *first;
            plain.colorOverride.clear();
            detected->setText(tr("As Detected (%1)").arg(QString::fromStdString(mediaColorSpace(plain).label)));
        }
        interpret->addSeparator();
        for (const ColorSpace& cs : colorSpaces()) {
            QAction* a = interpret->addAction(QString::fromStdString(cs.label), this, [setOverride, id = cs.id] { setOverride(id); });
            a->setCheckable(true);
            a->setChecked(current == cs.id);
            a->setData(QString::fromStdString(cs.id));
        }
        // An image sequence's frame rate (Interpret Footage).
        if (pictures.size() == 1)
            if (const MediaItem* m = state_->project().findMedia(pictures.front()); m && isImageSequencePath(m->path))
                menu.addAction(tr("Interpret Frame Rate…"), this, [this, id = m->id, fps = m->fps.toDouble()] {
                    bool ok = false;
                    const double v = QInputDialog::getDouble(this, tr("Interpret Frame Rate"), tr("Frames per second:"), fps, 1, 240, 3, &ok);
                    if (ok) setImageSequenceRate(id, v);
                })->setObjectName(QStringLiteral("interpretFrameRate"));
        // A merged Photoshop file: its layers as a sequence, or each a still.
        if (pictures.size() == 1)
            if (const MediaItem* m = state_->project().findMedia(pictures.front()); m && isPsdFile(m->path)) {
                menu.addAction(tr("Import Layers as Sequence"), this, [this, path = QString::fromStdString(m->path)] {
                    QString err;
                    if (state_->importPsd(path, PsdImport::Sequence, &err).empty()) QMessageBox::warning(this, tr("Import Layers"), err);
                })->setObjectName(QStringLiteral("psdLayersAsSequence"));
                menu.addAction(tr("Import Layers as Stills"), this, [this, path = QString::fromStdString(m->path)] {
                    QString err;
                    if (state_->importPsd(path, PsdImport::Layers, &err).empty()) QMessageBox::warning(this, tr("Import Layers"), err);
                })->setObjectName(QStringLiteral("psdLayersAsStills"));
            }
        // 360° footage: placed in a flat sequence as a view out of the sphere (Reframe 360°).
        bool all360 = true;
        for (Id id : pictures) all360 = all360 && state_->project().findMedia(id)->projection == "equirect";
        QAction* sphere = menu.addAction(tr("360° Footage"), this, [this, pictures, all360] {
            setMediaProjection(pictures, all360 ? std::string() : std::string("equirect"));
        });
        sphere->setObjectName(QStringLiteral("mediaSpherical"));
        sphere->setCheckable(true);
        sphere->setChecked(all360);
    }
    // Audio Channels (Premiere's Modify > Audio Channels): how new clips of files with several channels take them.
    std::vector<Id> multichannel;
    for (Id id : files)
        if (const MediaItem* m = state_->project().findMedia(id); m && sourceChannelCount(*m) > 1) multichannel.push_back(id);
    if (!multichannel.empty()) {
        QMenu* channels = menu.addMenu(tr("Audio Channels"));
        channels->setObjectName(QStringLiteral("mediaAudioChannels"));
        const std::string current = state_->project().findMedia(multichannel.front())->audioChannelMode;
        bool mixed = false;
        for (Id id : multichannel) mixed = mixed || state_->project().findMedia(id)->audioChannelMode != current;
        const std::pair<QString, const char*> modes[] = {{tr("Stereo Mix"), kChannelsMix},
                                                         {tr("Mono Clip per Channel"), kChannelsMono},
                                                         {tr("Stereo Clip per Pair"), kChannelsPairs}};
        for (const auto& [label, mode] : modes) {
            QAction* a = channels->addAction(label, this, [this, multichannel, mode = std::string(mode)] { setAudioChannelMode(multichannel, mode); });
            a->setCheckable(true);
            a->setChecked(!mixed && current == mode);
            a->setData(QString::fromLatin1(mode));
        }
    }
    std::vector<Id> sources;  // files a multicam clip can be made of
    for (Id id : ids)
        if (const MediaItem* m = state_->project().findMedia(id);
            m && !m->subclipOf && m->kind != MediaKind::Sequence && (m->hasVideo || m->hasAudio))
            sources.push_back(id);
    if (sources.size() >= 2 && std::any_of(sources.begin(), sources.end(), [this](Id id) {
            const MediaItem* m = state_->project().findMedia(id);
            return m && m->hasVideo;
        })) {
        menu.addSeparator();
        menu.addAction(tr("Create Multicam Clip..."), this, [this, sources] { emit createMulticamRequested(sources); })
            ->setObjectName(QStringLiteral("createMulticam"));
    }
    if (videos.size() == 1)
        menu.addAction(tr("Find Similar Shots"), this, [this, v = videos.front()] { emit findSimilarRequested(v); })
            ->setObjectName(QStringLiteral("binFindSimilar"));
    std::vector<Id> scalable;  // footage and stills, not graphics drawn at any size
    for (Id id : pictures)
        if (const MediaItem* m = state_->project().findMedia(id); m && m->videoCodec != "svg" && m->videoCodec != "lottie")
            scalable.push_back(id);
    if (!scalable.empty()) {
        menu.addSeparator();
        QMenu* ss = menu.addMenu(tr("Create Super Scale Copy"));
        ss->setObjectName(QStringLiteral("superScaleMenu"));
        for (int f : {2, 3, 4}) ss->addAction(tr("%1x Larger").arg(f), this, [this, scalable, f] { createSuperScaleCopies(scalable, f); });
    }
    if (!videos.empty()) {
        menu.addSeparator();
        menu.addAction(tr("Create Proxy Media"), this, [this, videos] { createProxies(videos); });
        menu.addAction(tr("Detach Proxy Media"), this, [this, videos] {
            state_->edit(tr("Detach Proxies"), [videos](Project& p, Sequence&) {
                for (Id id : videos)
                    if (MediaItem* m = p.findMedia(id)) m->proxyPath.clear();
                return true;
            });
        });
    }
    if (!ids.empty()) {
        menu.addSeparator();
        menu.addAction(tr("Remove from Project"), this, [this, ids] {
            for (Id id : ids) {
                QString err;
                if (!state_->removeMedia(id, &err) && !err.isEmpty()) state_->message(err);
            }
        });
    }
    menu.addSeparator();
    menu.addAction(tr("New Bin"), this, [this] { newBin(smart_ ? QString() : bin_); });
    menu.addAction(tr("Import..."), this, &MediaBinWidget::importDialog);
    QAction* scrub = menu.addAction(tr("Hover Scrub"), this, [this](bool on) { setHoverScrub(on); });
    scrub->setCheckable(true);
    scrub->setChecked(hoverScrub_);
    menu.exec(view->viewport()->mapToGlobal(pos));
}

void MediaBinWidget::transcribe(const std::vector<Id>& ids) {
    TranscribeDialog dlg(int(ids.size()), this);
    if (dlg.exec() != QDialog::Accepted) return;
    startTranscription(state_, ids, dlg.options(), this);
}

void MediaBinWidget::createSuperScaleCopies(const std::vector<Id>& ids, int factor) {
    if (!ensureEffectModel(window(), "super_scale")) return;
    struct Job {
        std::string src, dst;
    };
    std::vector<Job> jobs;
    for (Id id : ids) {
        const MediaItem* m = state_->project().findMedia(id);
        if (!m) continue;
        // Beside the original: "<name> (Super Scale 2x).mov", or .png for a still.
        const QFileInfo fi(QString::fromStdString(m->path));
        const QString dst = fi.absolutePath() + "/" + fi.completeBaseName() + tr(" (Super Scale %1x)").arg(factor) +
                            (m->kind == MediaKind::Image ? ".png" : ".mov");
        jobs.push_back({m->path, dst.toStdString()});
    }
    if (jobs.empty()) return;
    auto* dlg = new QProgressDialog(tr("Super Scale..."), tr("Cancel"), 0, 1000, this);
    dlg->setWindowModality(Qt::WindowModal);
    dlg->setMinimumDuration(300);
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    connect(dlg, &QProgressDialog::canceled, this, [cancel] { *cancel = true; });
    auto* watcher = new QFutureWatcher<QStringList>(this);
    const QString bin = smart_ ? QString() : bin_;
    connect(watcher, &QFutureWatcher<QStringList>::finished, this, [this, watcher, dlg, jobs, bin] {
        const QStringList errors = watcher->result();
        dlg->close();
        dlg->deleteLater();
        watcher->deleteLater();
        QStringList made;
        for (const auto& j : jobs)
            if (QFileInfo::exists(QString::fromStdString(j.dst))) made << QString::fromStdString(j.dst);
        if (!made.isEmpty()) importInto(made, bin);
        if (!errors.isEmpty()) QMessageBox::warning(this, tr("Super Scale"), errors.join("\n"));
        else state_->message(tr("Super Scale: %n copy/copies added to the bin", "", int(made.size())), 6000);
    });
    QPointer<QProgressDialog> guard(dlg);
    watcher->setFuture(QtConcurrent::run([jobs, cancel, guard, factor]() {
        QStringList errors;
        for (size_t i = 0; i < jobs.size(); ++i) {
            std::string err;
            auto progress = [&](double f) {
                const int v = int((double(i) + f) / double(jobs.size()) * 1000);
                QMetaObject::invokeMethod(qApp, [guard, v] {
                    if (guard) guard->setValue(v);
                }, Qt::QueuedConnection);
            };
            if (!createSuperScaled(jobs[i].src, jobs[i].dst, factor, 1.0, progress, cancel.get(), &err)) {
                QFile::remove(QString::fromStdString(jobs[i].dst));
                if (*cancel) break;
                errors << QString::fromStdString(err);
            }
        }
        return errors;
    }));
}

void MediaBinWidget::createProxies(const std::vector<Id>& ids) {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/proxies";
    QDir().mkpath(dir);
    struct Job {
        Id id;
        std::string src, dst;
    };
    std::vector<Job> jobs;
    for (Id id : ids) {
        const MediaItem* m = state_->project().findMedia(id);
        if (!m) continue;
        QByteArray hash = QCryptographicHash::hash(QByteArray::fromStdString(m->path), QCryptographicHash::Sha1).toHex().left(16);
        jobs.push_back({id, m->path, (dir + "/" + QString::fromLatin1(hash) + "_proxy.mp4").toStdString()});
    }
    if (jobs.empty()) return;
    auto* dlg = new QProgressDialog(tr("Creating proxy media..."), tr("Cancel"), 0, 1000, this);
    dlg->setWindowModality(Qt::WindowModal);
    dlg->setMinimumDuration(300);
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    connect(dlg, &QProgressDialog::canceled, this, [cancel] { *cancel = true; });
    auto* watcher = new QFutureWatcher<QStringList>(this);
    connect(watcher, &QFutureWatcher<QStringList>::finished, this, [this, watcher, dlg, jobs] {
        QStringList errors = watcher->result();
        dlg->close();
        dlg->deleteLater();
        watcher->deleteLater();
        // Attach the proxies that were written.
        state_->edit(tr("Attach Proxies"), [jobs](Project& p, Sequence&) {
            bool any = false;
            for (const auto& j : jobs)
                if (QFileInfo::exists(QString::fromStdString(j.dst)))
                    if (MediaItem* m = p.findMedia(j.id)) {
                        m->proxyPath = j.dst;
                        any = true;
                    }
            return any;
        });
        if (!errors.isEmpty()) QMessageBox::warning(this, tr("Proxy Media"), errors.join("\n"));
        else state_->message(tr("Proxy media ready — toggle \"Proxy\" in the Program monitor to use it"), 6000);
    });
    QPointer<QProgressDialog> guard(dlg);
    watcher->setFuture(QtConcurrent::run([jobs, cancel, guard]() {
        QStringList errors;
        for (size_t i = 0; i < jobs.size(); ++i) {
            std::string err;
            auto progress = [&](double f) {
                int v = int((double(i) + f) / double(jobs.size()) * 1000);
                QMetaObject::invokeMethod(qApp, [guard, v] {
                    if (guard) guard->setValue(v);
                }, Qt::QueuedConnection);
            };
            if (!createProxy(jobs[i].src, jobs[i].dst, 960, progress, cancel.get(), &err)) {
                QFile::remove(QString::fromStdString(jobs[i].dst));
                if (*cancel) break;
                errors << QString::fromStdString(err);
            }
        }
        return errors;
    }));
}

int MediaBinWidget::logSlatesFromPicture(const std::vector<Id>& media) {
    std::vector<std::pair<Id, std::string>> videos;
    for (Id id : media)
        if (const MediaItem* m = state_->project().findMedia(id); m && m->kind == MediaKind::Video && !m->path.empty()) videos.push_back({id, m->path});
    if (videos.empty()) return 0;
    if (!ensureModelPack(window(), ocrModel(), tr("Log Slate from Picture"),
                         tr("Reading the slate uses PP-OCR (PaddlePaddle, Apache-2.0), which runs on this computer.")))
        return 0;
    QProgressDialog progress(tr("Reading slates…"), tr("Cancel"), 0, int(videos.size()), window());
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(400);
    std::vector<std::pair<Id, SlateInfo>> found;
    for (size_t i = 0; i < videos.size() && !progress.wasCanceled(); ++i) {
        progress.setValue(int(i));
        QCoreApplication::processEvents();
        SlateInfo s;
        if (readSlateFromPicture(videos[i].second, s)) found.push_back({videos[i].first, s});
    }
    progress.setValue(int(videos.size()));
    if (!found.empty())
        state_->edit(tr("Log Slate from Picture"), [found](Project& p, Sequence&) {
            for (const auto& [id, s] : found)
                if (MediaItem* m = p.findMedia(id)) {
                    if (!s.scene.empty()) setMediaField(*m, "scene", s.scene);
                    if (!s.shot.empty()) setMediaField(*m, "shot", s.shot);
                    if (!s.take.empty()) setMediaField(*m, "take", s.take);
                }
            return true;
        });
    state_->message(found.empty() ? tr("No slate was found in the first seconds of these clips' pictures")
                                  : tr("Logged scene, shot and take for %n clip(s) from their slates", nullptr, int(found.size())),
                    5000);
    return int(found.size());
}

}  // namespace montage
