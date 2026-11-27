// Montage — Edit › Preferences…: every setting in core/Preferences.h, on its page, with its default and a way back to
// it, what overrides or locks it, a search through names and descriptions, and the settings saved to or read from a
// file. A change is kept as it is made.
#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>
#include <map>

class QLabel;
class QLineEdit;
class QListWidget;
class QStackedWidget;
class QWidget;

namespace montage {

class PreferencesDialog : public QDialog {
    Q_OBJECT
public:
    explicit PreferencesDialog(QWidget* parent = nullptr);

    // The parent's window (made the first time), shown and brought forward.
    static PreferencesDialog* open(QWidget* parent);

    // Every row shown again as the settings now are.
    void refresh();
    // Shows the rows whose name or description has the words in it (all of them for none).
    void search(const QString& words);
    // The page shown (its name as pages() has it).
    QString currentPage() const;
    void showPage(const QString& page);
    // The settings written to a file, or read from one (each row shown again); false, with why, when it cannot be.
    bool saveTo(const QString& path, QString* why = nullptr);
    bool loadFrom(const QString& path, QStringList* passed = nullptr, QString* why = nullptr);

signals:
    // A setting set or restored here.
    void changed(const QString& key);

private:
    struct Row {
        QWidget* row = nullptr;
        QWidget* editor = nullptr;
        QLabel* fallback = nullptr;
        QLabel* note = nullptr;
        QWidget* restore = nullptr;
        int page = 0;
        QString words;  // what the search looks in
        bool matched = true;
    };
    QWidget* makeEditor(const QString& key);
    void showRow(const QString& key);
    void setFrom(const QString& key, const QVariant& value);
    QString shownValue(const QString& key, const QVariant& value) const;

    QLineEdit* search_ = nullptr;
    QListWidget* pages_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    QLabel* problem_ = nullptr;
    QLabel* nothing_ = nullptr;
    std::map<QString, Row> rows_;
    bool filling_ = false;
};

}  // namespace montage
