// Montage — effects browser: searchable tree of video / audio effects,
// transitions and generators. Leaves can be dragged onto clips or edit points
// (mime type kEffectMimeType, payload = effect type) or applied with
// double-click / Enter.
#pragma once

#include <QHash>
#include <QString>
#include <QWidget>

#include "core/Effects.h"

class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;

namespace montage {

inline constexpr const char* kEffectMimeType = "application/x-montage-effect";

class EffectsBrowser : public QWidget {
    Q_OBJECT
public:
    explicit EffectsBrowser(QWidget* parent = nullptr);

    // Filters the tree as if `text` had been typed in the search field.
    void setFilterText(const QString& text);
    QString filterText() const;

signals:
    void applyRequested(const QString& type, montage::EffectCategory category);

private:
    void populate();
    void applyFilter(const QString& text);
    void requestApply(QTreeWidgetItem* item);
    void focusFirstMatch();

    QLineEdit* search_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QString activeFilter_;
    QHash<QTreeWidgetItem*, bool> savedExpansion_;  // folder state before a search started
};

}  // namespace montage
