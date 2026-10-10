#include "SmartBinDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

#include "Theme.h"
#include "core/MediaLog.h"

namespace montage {

namespace {
bool opNeedsValue(const QComboBox* field, const QComboBox* op) {
    if (const MediaField* f = mediaField(field->currentData().toString().toStdString()))
        for (const RuleOp& o : ruleOps(f->type))
            if (op->currentData().toString() == QLatin1String(o.id)) return o.needsValue;
    return true;
}
}  // namespace

SmartBinDialog::SmartBinDialog(const Project& project, const SmartBin& bin, QWidget* parent)
    : QDialog(parent), project_(project), id_(bin.id) {
    setWindowTitle(bin.rules.empty() && bin.name.empty() ? tr("New Smart Bin") : tr("Smart Bin"));
    auto* lay = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    name_ = new QLineEdit(QString::fromStdString(bin.name), this);
    name_->setObjectName(QStringLiteral("smartName"));
    form->addRow(tr("Name:"), name_);
    match_ = new QComboBox(this);
    match_->setObjectName(QStringLiteral("smartMatch"));
    match_->addItem(tr("all of these rules"), true);
    match_->addItem(tr("any of these rules"), false);
    match_->setCurrentIndex(bin.matchAll ? 0 : 1);
    form->addRow(tr("Show media matching:"), match_);
    lay->addLayout(form);

    auto* holder = new QWidget(this);
    rules_ = new QVBoxLayout(holder);
    rules_->setContentsMargins(0, 0, 0, 0);
    rules_->addStretch(1);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setWidget(holder);
    scroll->setMinimumSize(560, 180);
    lay->addWidget(scroll, 1);

    auto* bottom = new QHBoxLayout;
    auto* add = new QPushButton(tr("Add Rule"), this);
    add->setObjectName(QStringLiteral("addRule"));
    count_ = new QLabel(this);
    count_->setObjectName(QStringLiteral("smartCount"));
    bottom->addWidget(add);
    bottom->addStretch(1);
    bottom->addWidget(count_);
    lay->addLayout(bottom);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    lay->addWidget(buttons);

    connect(add, &QPushButton::clicked, this, [this] { addRule(); });
    connect(match_, &QComboBox::currentIndexChanged, this, &SmartBinDialog::updateCount);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (name_->text().trimmed().isEmpty()) name_->setText(tr("Smart Bin"));
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    for (const SmartRule& r : bin.rules) addRule(r);
    if (bin.rules.empty()) addRule({"rating", ">=", "3"});
    updateCount();
}

void SmartBinDialog::addRule(const SmartRule& rule) {
    rows_.push_back({});
    Row& row = rows_.back();
    row.box = new QWidget;
    auto* h = new QHBoxLayout(row.box);
    h->setContentsMargins(0, 0, 0, 0);
    row.field = new QComboBox(row.box);
    row.field->setObjectName(QStringLiteral("ruleField"));
    for (const MediaField& f : mediaFields())
        if (f.rule) row.field->addItem(tr(f.label), QString::fromLatin1(f.key));
    row.op = new QComboBox(row.box);
    row.op->setObjectName(QStringLiteral("ruleOp"));
    row.text = new QLineEdit(row.box);
    row.text->setObjectName(QStringLiteral("ruleText"));
    row.choice = new QComboBox(row.box);
    row.choice->setObjectName(QStringLiteral("ruleChoice"));
    auto* remove = new QToolButton(row.box);
    remove->setText(QStringLiteral("−"));
    remove->setToolTip(tr("Remove this rule"));
    remove->setObjectName(QStringLiteral("removeRule"));
    h->addWidget(row.field);
    h->addWidget(row.op);
    h->addWidget(row.text, 1);
    h->addWidget(row.choice, 1);
    h->addWidget(remove);
    rules_->insertWidget(rules_->count() - 1, row.box);

    Row* r = &row;
    const int f = row.field->findData(QString::fromStdString(rule.field.empty() ? "any" : rule.field));
    row.field->setCurrentIndex(std::max(0, f));
    fieldChanged(row);
    if (const int o = row.op->findData(QString::fromStdString(rule.op)); o >= 0) row.op->setCurrentIndex(o);
    opChanged(row);
    if (!rule.value.empty()) {
        if (const int c = row.choice->findData(QString::fromStdString(rule.value)); c >= 0) row.choice->setCurrentIndex(c);
        else if (row.choice->isEditable()) row.choice->setCurrentText(QString::fromStdString(rule.value));
        row.text->setText(QString::fromStdString(rule.value));
    }
    connect(row.field, &QComboBox::currentIndexChanged, this, [this, r] {
        fieldChanged(*r);
        updateCount();
    });
    connect(row.op, &QComboBox::currentIndexChanged, this, [this, r] {
        opChanged(*r);
        updateCount();
    });
    connect(row.text, &QLineEdit::textChanged, this, &SmartBinDialog::updateCount);
    connect(row.choice, &QComboBox::currentTextChanged, this, &SmartBinDialog::updateCount);
    connect(remove, &QToolButton::clicked, this, [this, r] {
        for (auto it = rows_.begin(); it != rows_.end(); ++it)
            if (&*it == r) {
                it->box->deleteLater();
                rows_.erase(it);
                break;
            }
        updateCount();
    });
    updateCount();
}

int SmartBinDialog::ruleCount() const { return int(rows_.size()); }

void SmartBinDialog::fieldChanged(Row& row) {
    const MediaField* f = mediaField(row.field->currentData().toString().toStdString());
    if (!f) return;
    const QString op = row.op->currentData().toString();
    row.op->blockSignals(true);
    row.op->clear();
    for (const RuleOp& o : ruleOps(f->type)) row.op->addItem(tr(o.label), QString::fromLatin1(o.id));
    row.op->setCurrentIndex(std::max(0, row.op->findData(op)));
    row.op->blockSignals(false);

    row.choice->blockSignals(true);
    row.choice->clear();
    row.choice->setEditable(false);
    bool choice = true;
    switch (f->type) {
        case FieldType::Rating:
            row.choice->addItem(tr("Rejected"), QStringLiteral("-1"));
            row.choice->addItem(tr("Unrated"), QStringLiteral("0"));
            for (int i = 1; i <= 5; ++i) row.choice->addItem(QString(i, QChar(0x2605)), QString::number(i));
            row.choice->setCurrentIndex(4);  // three stars
            break;
        case FieldType::Label:
            for (int i = 1; i < theme::labelCount(); ++i) {
                QPixmap sw(10, 10);
                sw.fill(theme::labelColor(i));
                row.choice->addItem(QIcon(sw), tr(labelName(i)), QString::fromLatin1(labelName(i)));
            }
            break;
        case FieldType::Kind:
            row.choice->addItem(tr("Video"), QStringLiteral("video"));
            row.choice->addItem(tr("Audio"), QStringLiteral("audio"));
            row.choice->addItem(tr("Still Image"), QStringLiteral("image"));
            row.choice->addItem(tr("Sequence"), QStringLiteral("sequence"));
            row.choice->addItem(tr("Subclip"), QStringLiteral("subclip"));
            break;
        case FieldType::Keywords:
            row.choice->setEditable(true);
            if (std::string(f->key) == "people")
                for (const PersonSummary& person : peopleIn(project_))
                    row.choice->addItem(QString::fromStdString(person.name), QString::fromStdString(person.name));
            else
                for (const std::string& k : projectKeywords(project_)) row.choice->addItem(QString::fromStdString(k), QString::fromStdString(k));
            break;
        default: choice = false;
    }
    row.choice->blockSignals(false);
    row.choice->setProperty("isChoice", choice);
    row.text->setPlaceholderText(f->type == FieldType::Number ? (std::string(f->key) == "duration" ? tr("seconds, or m:ss") : tr("number"))
                                                              : QString());
    opChanged(row);
}

void SmartBinDialog::opChanged(Row& row) {
    const bool needsValue = opNeedsValue(row.field, row.op);
    const bool choice = row.choice->property("isChoice").toBool();
    row.choice->setVisible(needsValue && choice);
    row.text->setVisible(needsValue && !choice);
}

QString SmartBinDialog::rowValue(const Row& row) const {
    if (!row.choice->property("isChoice").toBool()) return row.text->text().trimmed();
    if (row.choice->isEditable()) return row.choice->currentText().trimmed();
    return row.choice->currentData().toString();
}

SmartBin SmartBinDialog::bin() const {
    SmartBin b;
    b.id = id_;
    b.name = name_->text().trimmed().toStdString();
    b.matchAll = match_->currentData().toBool();
    for (const Row& r : rows_) {
        SmartRule rule{r.field->currentData().toString().toStdString(), r.op->currentData().toString().toStdString(), {}};
        if (opNeedsValue(r.field, r.op)) rule.value = rowValue(r).toStdString();
        b.rules.push_back(std::move(rule));
    }
    return b;
}

void SmartBinDialog::updateCount() {
    const size_t n = smartBinMedia(project_, bin()).size();
    count_->setText(tr("%n item(s) match", "", int(n)));
}

}  // namespace montage
