// Montage — effects browser: searchable tree of video / audio effects,
// transitions, generators and installed audio plugins. Leaves can be dragged onto clips or edit points
// (mime type kEffectMimeType, payload = effect type) or applied with
// double-click / Enter.
#pragma once

#include <QHash>
#include <QString>
#include <QWidget>

#include "core/Effects.h"

class QLineEdit;
class QTimer;
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
    // Rebuilds the tree, e.g. after a plugin scan.
    void reload() { populate(); }
    // As if the pointer had come to rest on the item of this type ("preset:<file>" for a preset), or had left the
    // list (an empty type): what is pointed at is shown on the Program monitor. False when no item has that type.
    bool pointAt(const QString& type);
    // The type pointed at now ("" = none).
    QString pointedAt() const { return pointed_; }

signals:
    void applyRequested(const QString& type, montage::EffectCategory category);
    // The pointer has rested on an effect or a preset, or has left what it was on.
    void previewRequested(const QString& type, montage::EffectCategory category);
    void previewEnded();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void populate();
    void applyFilter(const QString& text);
    void requestApply(QTreeWidgetItem* item);
    void focusFirstMatch();
    void point(const QString& type, EffectCategory category);

    QLineEdit* search_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QString activeFilter_;
    QString pointed_;            // the type whose preview was asked for
    QString resting_;            // the type the pointer is on, shown once it has rested there
    EffectCategory restingCategory_ = EffectCategory::VideoFilter;
    QTimer* rest_ = nullptr;
    QHash<QTreeWidgetItem*, bool> savedExpansion_;  // folder state before a search started
};

}  // namespace montage
