// Montage — effects browser.
#include "EffectsBrowser.h"

#include <QDrag>
#include <QFontMetrics>
#include <QHash>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QStyle>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <functional>

#include "Theme.h"

namespace montage {

namespace {

constexpr int kTypeRole = Qt::UserRole;
constexpr int kCategoryRole = Qt::UserRole + 1;
constexpr int kSearchRole = Qt::UserRole + 2;  // text the search matches against (leaves only)

bool isLeaf(const QTreeWidgetItem* item) { return item && !item->data(0, kTypeRole).toString().isEmpty(); }

// Small swatch: square for effects and generators, diagonally split for transitions.
QIcon categoryIcon(EffectCategory c) {
    QColor color = theme::kVideoClip;
    bool transition = false;
    switch (c) {
        case EffectCategory::VideoFilter: color = theme::kVideoClip; break;
        case EffectCategory::AudioFilter: color = theme::kAudioClip; break;
        case EffectCategory::VideoTransition: color = theme::kVideoClip; transition = true; break;
        case EffectCategory::AudioTransition: color = theme::kAudioClip; transition = true; break;
        case EffectCategory::Generator: color = theme::kGeneratorClip; break;
        case EffectCategory::Fixed: break;
    }
    QIcon icon;
    for (int scale = 1; scale <= 2; ++scale) {
        QPixmap pm(14 * scale, 14 * scale);
        pm.setDevicePixelRatio(scale);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r(2.5, 2.5, 9, 9);
        QPainterPath box;
        box.addRoundedRect(r, 2, 2);
        p.fillPath(box, transition ? color.darker(160) : color);
        if (transition) {
            QPainterPath half;
            half.moveTo(r.topLeft());
            half.lineTo(r.topRight());
            half.lineTo(r.bottomLeft());
            half.closeSubpath();
            p.fillPath(half.intersected(box), color);
        }
        p.setPen(QPen(color.lighter(140), 1));
        p.drawPath(box);
        p.end();
        icon.addPixmap(pm);
    }
    return icon;
}

QString tooltipFor(const EffectInfo& info) {
    QStringList lines;
    lines << QString::fromStdString(info.displayName);
    if (!info.group.empty()) lines << QString::fromStdString(info.group);
    QStringList params;
    for (const ParamInfo& p : info.params) params << QString::fromStdString(p.label);
    for (const StringParamInfo& s : info.strings) params << QString::fromStdString(s.label);
    if (params.size() > 6) {
        params = params.mid(0, 6);
        params << QStringLiteral("…");
    }
    if (!params.isEmpty()) lines << EffectsBrowser::tr("Parameters: %1").arg(params.join(QStringLiteral(", ")));
    lines << EffectsBrowser::tr("Drag into the timeline, or double-click to apply");
    return lines.join('\n');
}

QPixmap dragPixmap(const QTreeWidgetItem* item, const QWidget* widget) {
    const QString text = item->text(0);
    const QFontMetrics fm(widget->font());
    const QSize size(fm.horizontalAdvance(text) + 34, fm.height() + 12);
    const qreal dpr = widget->devicePixelRatioF();
    QPixmap pm(size * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(theme::kAccent, 1));
    p.setBrush(theme::kPanelAlt);
    p.drawRoundedRect(QRectF(0.5, 0.5, size.width() - 1, size.height() - 1), 4, 4);
    item->icon(0).paint(&p, QRect(6, (size.height() - 14) / 2, 14, 14));
    p.setPen(theme::kText);
    p.setFont(widget->font());
    p.drawText(QRect(26, 0, size.width() - 30, size.height()), Qt::AlignVCenter | Qt::AlignLeft, text);
    return pm;
}

// Hides leaves not matching `needle` and folders left empty; expands folders with matches.
bool filterItem(QTreeWidgetItem* item, const QString& needle) {
    if (isLeaf(item)) {
        const bool match = needle.isEmpty() || item->data(0, kSearchRole).toString().contains(needle, Qt::CaseInsensitive);
        item->setHidden(!match);
        return match;
    }
    bool any = false;
    for (int i = 0; i < item->childCount(); ++i) any = filterItem(item->child(i), needle) || any;
    item->setHidden(!needle.isEmpty() && !any);
    if (!needle.isEmpty() && any) item->setExpanded(true);
    return any;
}

QTreeWidgetItem* firstVisibleLeaf(QTreeWidgetItem* item) {
    if (item->isHidden()) return nullptr;
    if (isLeaf(item)) return item;
    for (int i = 0; i < item->childCount(); ++i)
        if (QTreeWidgetItem* leaf = firstVisibleLeaf(item->child(i))) return leaf;
    return nullptr;
}

void forEachFolder(QTreeWidgetItem* item, const std::function<void(QTreeWidgetItem*)>& fn) {
    if (isLeaf(item)) return;
    fn(item);
    for (int i = 0; i < item->childCount(); ++i) forEachFolder(item->child(i), fn);
}

// Tree with a custom drag payload and Enter-to-apply.
class EffectTree : public QTreeWidget {
public:
    using QTreeWidget::QTreeWidget;
    std::function<void(QTreeWidgetItem*)> onApply;

protected:
    void startDrag(Qt::DropActions) override {
        QTreeWidgetItem* item = currentItem();
        if (!isLeaf(item)) return;
        const QString type = item->data(0, kTypeRole).toString();
        auto* mime = new QMimeData;
        mime->setData(QString::fromLatin1(kEffectMimeType), type.toUtf8());
        mime->setText(type);
        auto* drag = new QDrag(this);
        drag->setMimeData(mime);
        const QPixmap pm = dragPixmap(item, this);
        drag->setPixmap(pm);
        drag->setHotSpot(QPoint(8, int(pm.deviceIndependentSize().height() / 2)));
        drag->exec(Qt::CopyAction, Qt::CopyAction);
    }

    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            if (QTreeWidgetItem* item = currentItem()) {
                if (isLeaf(item)) {
                    if (onApply) onApply(item);
                } else {
                    item->setExpanded(!item->isExpanded());
                }
            }
            event->accept();
            return;
        }
        QTreeWidget::keyPressEvent(event);
    }
};

}  // namespace

EffectsBrowser::EffectsBrowser(QWidget* parent) : QWidget(parent) {
    search_ = new QLineEdit(this);
    search_->setPlaceholderText(tr("Search effects"));
    search_->setClearButtonEnabled(true);

    auto* tree = new EffectTree(this);
    tree->onApply = [this](QTreeWidgetItem* item) { requestApply(item); };
    tree_ = tree;
    tree_->setHeaderHidden(true);
    tree_->setColumnCount(1);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_->setDragEnabled(true);
    tree_->setDragDropMode(QAbstractItemView::DragOnly);
    tree_->setUniformRowHeights(true);
    tree_->setAnimated(true);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);
    layout->addWidget(search_);
    layout->addWidget(tree_, 1);

    populate();

    connect(search_, &QLineEdit::textChanged, this, [this](const QString& text) { applyFilter(text); });
    connect(search_, &QLineEdit::returnPressed, this, [this] { focusFirstMatch(); });
    connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) {
        if (isLeaf(item)) requestApply(item);
    });
}

void EffectsBrowser::setFilterText(const QString& text) { search_->setText(text); }

QString EffectsBrowser::filterText() const { return search_->text(); }

void EffectsBrowser::populate() {
    struct Section {
        EffectCategory category;
        QString title;
        bool grouped;  // sub-folders by EffectInfo::group
    };
    const Section sections[] = {
        {EffectCategory::VideoFilter, tr("Video Effects"), true},
        {EffectCategory::AudioFilter, tr("Audio Effects"), true},
        {EffectCategory::VideoTransition, tr("Video Transitions"), false},
        {EffectCategory::AudioTransition, tr("Audio Transitions"), false},
        {EffectCategory::Generator, tr("Generators & Titles"), false},
    };
    const QIcon folderIcon = style()->standardIcon(QStyle::SP_DirIcon);
    const Qt::ItemFlags folderFlags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    const Qt::ItemFlags leafFlags = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;

    tree_->clear();
    savedExpansion_.clear();
    for (const Section& section : sections) {
        auto* top = new QTreeWidgetItem(tree_, QStringList{section.title});
        top->setIcon(0, folderIcon);
        top->setFlags(folderFlags);
        const QIcon leafIcon = categoryIcon(section.category);
        QHash<QString, QTreeWidgetItem*> groups;  // catalogue order is kept for groups and leaves

        for (const EffectInfo* info : effectsInCategory(section.category)) {
            QTreeWidgetItem* parent = top;
            const QString group = info->group.empty() ? tr("Other") : QString::fromStdString(info->group);
            if (section.grouped) {
                parent = groups.value(group);
                if (!parent) {
                    parent = new QTreeWidgetItem(top, QStringList{group});
                    parent->setIcon(0, folderIcon);
                    parent->setFlags(folderFlags);
                    groups.insert(group, parent);
                }
            }
            const QString name = QString::fromStdString(info->displayName);
            const QString type = QString::fromStdString(info->type);
            auto* leaf = new QTreeWidgetItem(parent, QStringList{name});
            leaf->setFlags(leafFlags);
            leaf->setIcon(0, leafIcon);
            leaf->setToolTip(0, tooltipFor(*info));
            leaf->setData(0, kTypeRole, type);
            leaf->setData(0, kCategoryRole, int(info->category));
            leaf->setData(0, kSearchRole, QStringList{name, group, type}.join(' '));
        }
        top->setExpanded(true);
    }
    if (!activeFilter_.isEmpty()) {
        const QString filter = activeFilter_;
        activeFilter_.clear();
        applyFilter(filter);
    }
}

void EffectsBrowser::applyFilter(const QString& text) {
    const QString needle = text.trimmed();
    if (needle == activeFilter_) return;
    const bool wasFiltering = !activeFilter_.isEmpty();
    // Remember the user's folder layout when a search starts, restore it when cleared.
    if (!wasFiltering) {
        savedExpansion_.clear();
        for (int i = 0; i < tree_->topLevelItemCount(); ++i)
            forEachFolder(tree_->topLevelItem(i), [this](QTreeWidgetItem* f) { savedExpansion_.insert(f, f->isExpanded()); });
    }
    activeFilter_ = needle;

    tree_->setUpdatesEnabled(false);
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) filterItem(tree_->topLevelItem(i), needle);
    if (needle.isEmpty()) {
        for (auto it = savedExpansion_.cbegin(); it != savedExpansion_.cend(); ++it) it.key()->setExpanded(it.value());
        savedExpansion_.clear();
        if (QTreeWidgetItem* current = tree_->currentItem()) tree_->scrollToItem(current);
    }
    tree_->setUpdatesEnabled(true);
}

void EffectsBrowser::focusFirstMatch() {
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        if (QTreeWidgetItem* leaf = firstVisibleLeaf(tree_->topLevelItem(i))) {
            tree_->setCurrentItem(leaf);
            tree_->scrollToItem(leaf);
            tree_->setFocus(Qt::ShortcutFocusReason);
            return;
        }
    }
}

void EffectsBrowser::requestApply(QTreeWidgetItem* item) {
    if (!isLeaf(item)) return;
    emit applyRequested(item->data(0, kTypeRole).toString(), EffectCategory(item->data(0, kCategoryRole).toInt()));
}

}  // namespace montage
