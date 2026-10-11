// Montage — graphics templates in the app (core/GraphicsTemplate.h): saving a sequence to the library, the library
// itself (insert, delete), a sequence's published fields (rename, reorder, remove), publishing a clip's picture, and
// filling graphics from a table.
#pragma once

#include <QDialog>
#include <QString>
#include <vector>

#include "core/GraphicsTemplate.h"

class QLabel;
class QListWidget;

namespace montage {

class EditorState;

// Saves the open sequence to the template library as `name` (asked for when empty; replacing one of that name asks
// first unless `name` was given). Returns the template's file, "" if not saved.
QString saveSequenceAsGraphicsTemplate(EditorState* state, QWidget* parent, const QString& name = {});
// Publishes the picture clip `clip` plays (an image or video) as a field of the open sequence; "" or why not.
QString publishPictureField(EditorState* state, Id clip, const QString& label);
// Fills graphic `clip` and copies of it from the table in `file`, one undo step; the clips made (none on failure, with
// the reason shown).
std::vector<Id> fillGraphicsFromFile(EditorState* state, Id clip, const QString& file);

// The library: insert a template at the playhead (on a new track on top), delete one, open the folder.
class GraphicsTemplatesDialog : public QDialog {
    Q_OBJECT
public:
    explicit GraphicsTemplatesDialog(EditorState* state, QWidget* parent = nullptr);
    void reload();
    const std::vector<GraphicsTemplateInfo>& templates() const { return templates_; }
    // Inserts template `row`; the graphic's clip, 0 if not.
    Id insert(int row);

private:
    EditorState* state_;
    QListWidget* list_;
    QLabel* detail_;
    std::vector<GraphicsTemplateInfo> templates_;
};

// The open sequence's published fields: rename, move, remove.
class GraphicFieldsDialog : public QDialog {
    Q_OBJECT
public:
    explicit GraphicFieldsDialog(EditorState* state, QWidget* parent = nullptr);
    void reload();
    bool rename(int row, const QString& label);
    bool remove(int row);
    bool move(int row, int by);

private:
    EditorState* state_;
    QListWidget* list_;
};

}  // namespace montage
