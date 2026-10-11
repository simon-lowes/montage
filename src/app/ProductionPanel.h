// Montage — the Production panel (Premiere's Production panel, Avid's shared project window): the projects of a
// production folder on a shared drive, who is editing each, opening one to edit or read-only, starting a new one, and
// bringing sequences from one into the project that is open (core/Production.h).
#pragma once

#include <QWidget>

class QLabel;
class QPushButton;
class QTimer;
class QTreeWidget;

namespace montage {

class EditorState;

class ProductionPanel : public QWidget {
    Q_OBJECT
public:
    explicit ProductionPanel(EditorState* state, QWidget* parent = nullptr);

    void setFolder(const QString& folder);  // "" = none
    const QString& folder() const { return folder_; }
    void refresh();
    QString selectedProject() const;

signals:
    void openRequested(const QString& project, bool readOnly);
    void newProjectRequested(const QString& folder);
    void importRequested(const QString& project);

private:
    EditorState* state_;
    QString folder_;
    QString shownFor_;  // the open project when the list was last brought up to date
    QLabel* title_;
    QTreeWidget* list_;
    QPushButton* open_;
    QPushButton* openReadOnly_;
    QPushButton* import_;
    QPushButton* newProject_;
    QTimer* timer_;
    void updateButtons();
};

}  // namespace montage
