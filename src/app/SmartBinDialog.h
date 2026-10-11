// Montage — editing a smart bin: a name, whether all or any rules must match,
// and the rules (field, test, value), with a live count of matching media.
#pragma once

#include <QDialog>
#include <list>

#include "core/Model.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QVBoxLayout;

namespace montage {

class SmartBinDialog : public QDialog {
    Q_OBJECT
public:
    SmartBinDialog(const Project& project, const SmartBin& bin, QWidget* parent = nullptr);

    SmartBin bin() const;
    // Adds a rule row (for a new bin, one rule is there to start with).
    void addRule(const SmartRule& rule = {});
    int ruleCount() const;

private:
    struct Row {
        QWidget* box;
        QComboBox* field;
        QComboBox* op;
        QLineEdit* text;    // text and numbers
        QComboBox* choice;  // ratings, labels, kinds and keywords
    };
    void fieldChanged(Row& row);
    void opChanged(Row& row);
    QString rowValue(const Row& row) const;
    void updateCount();

    const Project& project_;
    Id id_;
    QLineEdit* name_;
    QComboBox* match_;
    QVBoxLayout* rules_;
    QLabel* count_;
    std::list<Row> rows_;  // stable addresses: the rows' widgets refer to them
};

}  // namespace montage
