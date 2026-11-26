#include "PreferencesDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include "Language.h"
#include "core/Preferences.h"

namespace montage {

namespace {

QString shown(const char* text) { return text ? QCoreApplication::translate("montage::Preferences", text) : QString(); }

}  // namespace

PreferencesDialog::PreferencesDialog(QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("preferencesDialog"));
    setWindowTitle(tr("Preferences"));
    auto* layout = new QVBoxLayout(this);
    search_ = new QLineEdit(this);
    search_->setObjectName(QStringLiteral("preferencesSearch"));
    search_->setPlaceholderText(tr("Search the settings"));
    search_->setClearButtonEnabled(true);
    layout->addWidget(search_);
    problem_ = new QLabel(this);
    problem_->setObjectName(QStringLiteral("preferencesProblem"));
    problem_->setWordWrap(true);
    layout->addWidget(problem_);

    auto* middle = new QHBoxLayout;
    pages_ = new QListWidget(this);
    pages_->setObjectName(QStringLiteral("preferencesPages"));
    stack_ = new QStackedWidget(this);
    std::vector<QGridLayout*> grids;
    for (const QString& page : prefs::pages()) {
        auto* item = new QListWidgetItem(shown(page.toLatin1().constData()), pages_);
        item->setData(Qt::UserRole, page);
        auto* scroll = new QScrollArea(stack_);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        auto* inner = new QWidget(scroll);
        inner->setObjectName(QStringLiteral("page:") + page);
        auto* grid = new QGridLayout(inner);
        grid->setColumnStretch(1, 1);
        scroll->setWidget(inner);
        stack_->addWidget(scroll);
        grids.push_back(grid);
    }
    pages_->setMaximumWidth(pages_->sizeHintForColumn(0) + 4 * pages_->frameWidth() + 24);
    middle->addWidget(pages_);
    middle->addWidget(stack_, 1);
    layout->addLayout(middle, 1);
    nothing_ = new QLabel(tr("No setting has those words in its name or description."), this);
    nothing_->setObjectName(QStringLiteral("preferencesNothing"));
    nothing_->hide();
    layout->addWidget(nothing_);

    for (const prefs::Setting& s : prefs::all()) {
        const int page = int(prefs::pages().indexOf(QString::fromLatin1(s.page)));
        QGridLayout* grid = grids[size_t(std::max(0, page))];
        Row r;
        r.page = std::max(0, page);
        r.row = grid->parentWidget();
        auto* name = new QLabel(shown(s.label), r.row);
        name->setObjectName(QStringLiteral("name:") + s.key);
        r.editor = makeEditor(s.key);
        r.editor->setParent(r.row);
        r.fallback = new QLabel(r.row);
        r.fallback->setObjectName(QStringLiteral("default:") + s.key);
        r.fallback->setWordWrap(true);
        auto* restore = new QToolButton(r.row);
        restore->setObjectName(QStringLiteral("restore:") + s.key);
        restore->setText(tr("Restore"));
        restore->setToolTip(tr("Use the default again"));
        connect(restore, &QToolButton::clicked, this, [this, key = s.key] {
            prefs::restore(key);
            showRow(key);
            emit changed(key);
        });
        r.restore = restore;
        auto* about = new QLabel(shown(s.description) + (s.restart ? QLatin1Char(' ') + tr("Used from when Montage next starts.") : QString()), r.row);
        about->setObjectName(QStringLiteral("about:") + s.key);
        about->setWordWrap(true);
        about->setEnabled(false);  // (drawn quieter)
        r.note = new QLabel(r.row);
        r.note->setObjectName(QStringLiteral("note:") + s.key);
        r.note->setWordWrap(true);
        const int at = grid->rowCount();
        grid->addWidget(name, at, 0);
        grid->addWidget(r.editor, at, 1);
        grid->addWidget(restore, at, 2);
        grid->addWidget(about, at + 1, 1, 1, 2);
        grid->addWidget(r.fallback, at + 2, 1, 1, 2);
        grid->addWidget(r.note, at + 3, 1, 1, 2);
        // (the row's widgets, so the search can hide them together)
        for (QWidget* w : {static_cast<QWidget*>(name), r.editor, static_cast<QWidget*>(restore), static_cast<QWidget*>(about),
                           static_cast<QWidget*>(r.fallback), static_cast<QWidget*>(r.note)})
            w->setProperty("settingKey", s.key);
        r.words = (shown(s.label) + QLatin1Char(' ') + shown(s.description) + QLatin1Char(' ') + QString::fromUtf8(s.label) +
                   QLatin1Char(' ') + QString::fromUtf8(s.description) + QLatin1Char(' ') + s.key)
                      .toLower();
        rows_[s.key] = r;
    }
    for (QGridLayout* grid : grids) grid->setRowStretch(grid->rowCount(), 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto* save = buttons->addButton(tr("Save to File…"), QDialogButtonBox::ActionRole);
    save->setObjectName(QStringLiteral("preferencesSave"));
    auto* load = buttons->addButton(tr("Read from File…"), QDialogButtonBox::ActionRole);
    load->setObjectName(QStringLiteral("preferencesLoad"));
    auto* all = buttons->addButton(tr("Restore All"), QDialogButtonBox::ResetRole);
    all->setObjectName(QStringLiteral("preferencesRestoreAll"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    connect(save, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getSaveFileName(this, tr("Save Settings"), QString(), tr("Montage settings (*.json)"));
        QString why;
        if (!path.isEmpty() && !saveTo(path, &why)) QMessageBox::warning(this, tr("Save Settings"), why);
    });
    connect(load, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("Read Settings"), QString(), tr("Montage settings (*.json)"));
        QString why;
        QStringList passed;
        if (path.isEmpty()) return;
        if (!loadFrom(path, &passed, &why)) QMessageBox::warning(this, tr("Read Settings"), why);
        else if (!passed.isEmpty())
            QMessageBox::information(this, tr("Read Settings"),
                                     tr("These were passed over (locked here, or not Montage's settings): %1").arg(passed.join(QStringLiteral(", "))));
    });
    connect(all, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, tr("Restore All"), tr("Use every setting's default again?")) != QMessageBox::Yes) return;
        for (const prefs::Setting& s : prefs::all()) {
            prefs::restore(s.key);
            emit changed(s.key);
        }
        refresh();
    });
    connect(pages_, &QListWidget::currentRowChanged, stack_, &QStackedWidget::setCurrentIndex);
    connect(search_, &QLineEdit::textChanged, this, &PreferencesDialog::search);
    pages_->setCurrentRow(0);
    refresh();
    resize(860, 600);
}

PreferencesDialog* PreferencesDialog::open(QWidget* parent) {
    QWidget* window = parent ? parent->window() : nullptr;
    auto* d = window ? window->findChild<PreferencesDialog*>(QString(), Qt::FindDirectChildrenOnly) : nullptr;
    if (!d) {
        d = new PreferencesDialog(window);
        d->setAttribute(Qt::WA_DeleteOnClose, false);
    }
    d->refresh();
    d->show();
    d->raise();
    d->activateWindow();
    return d;
}

QWidget* PreferencesDialog::makeEditor(const QString& key) {
    const prefs::Setting& s = *prefs::find(key);
    QWidget* w = nullptr;
    switch (s.kind) {
        case prefs::Kind::Toggle: {
            auto* box = new QCheckBox(this);
            connect(box, &QCheckBox::toggled, this, [this, key](bool on) { setFrom(key, on); });
            w = box;
            break;
        }
        case prefs::Kind::Whole: {
            auto* box = new QSpinBox(this);
            box->setRange(int(s.min), int(s.max));
            if (s.unit) box->setSuffix(QLatin1Char(' ') + shown(s.unit));
            box->setKeyboardTracking(false);
            connect(box, &QSpinBox::valueChanged, this, [this, key](int v) { setFrom(key, v); });
            w = box;
            break;
        }
        case prefs::Kind::Number: {
            auto* box = new QDoubleSpinBox(this);
            box->setRange(s.min, s.max);
            box->setDecimals(2);
            box->setSingleStep(0.05);
            if (s.unit) box->setSuffix(QLatin1Char(' ') + shown(s.unit));
            box->setKeyboardTracking(false);
            connect(box, &QDoubleSpinBox::valueChanged, this, [this, key](double v) { setFrom(key, v); });
            w = box;
            break;
        }
        case prefs::Kind::Choice: {
            auto* box = new QComboBox(this);
            if (key == QLatin1String("ui/language")) {
                box->addItem(tr("The system's"), QString());
                for (const language::Language& l : language::available()) box->addItem(l.name, l.code);
            } else {
                for (const prefs::Choice& c : s.choices) box->addItem(c.label ? shown(c.label) : c.value, c.value);
            }
            connect(box, &QComboBox::activated, this, [this, key, box](int i) { setFrom(key, box->itemData(i)); });
            w = box;
            break;
        }
        default: {
            auto* holder = new QWidget(this);
            auto* h = new QHBoxLayout(holder);
            h->setContentsMargins(0, 0, 0, 0);
            auto* line = new QLineEdit(holder);
            line->setObjectName(QStringLiteral("text:") + key);
            line->setPlaceholderText(s.kind == prefs::Kind::Text ? tr("Montage's own") : tr("The one Montage finds"));
            h->addWidget(line, 1);
            connect(line, &QLineEdit::editingFinished, this, [this, key, line] { setFrom(key, line->text().trimmed()); });
            if (s.kind != prefs::Kind::Text) {
                auto* choose = new QToolButton(holder);
                choose->setObjectName(QStringLiteral("choose:") + key);
                choose->setText(tr("Choose…"));
                const prefs::Kind kind = s.kind;
                connect(choose, &QToolButton::clicked, this, [this, key, line, kind] {
                    const QString now = line->text();
                    const QString path = kind == prefs::Kind::Folder ? QFileDialog::getExistingDirectory(this, tr("Choose a Folder"), now)
                                                                     : QFileDialog::getOpenFileName(this, tr("Choose a File"), now);
                    if (path.isEmpty()) return;
                    line->setText(path);
                    setFrom(key, path);
                });
                h->addWidget(choose);
            }
            w = holder;
        }
    }
    w->setObjectName(QStringLiteral("edit:") + key);
    return w;
}

void PreferencesDialog::setFrom(const QString& key, const QVariant& value) {
    if (filling_) return;
    if (prefs::get(key) == value && prefs::isSet(key)) return;
    QString why;
    if (!prefs::set(key, value, &why)) QMessageBox::warning(this, tr("Preferences"), why);
    showRow(key);
    emit changed(key);
}

QString PreferencesDialog::shownValue(const QString& key, const QVariant& value) const {
    const prefs::Setting& s = *prefs::find(key);
    switch (s.kind) {
        case prefs::Kind::Toggle:
            return value.toBool() ? tr("on") : tr("off");
        case prefs::Kind::Whole:
            return s.unit ? QStringLiteral("%1 %2").arg(value.toInt()).arg(shown(s.unit)) : QString::number(value.toInt());
        case prefs::Kind::Number:
            return s.unit ? QStringLiteral("%1 %2").arg(locale().toString(value.toDouble()), shown(s.unit))
                          : locale().toString(value.toDouble());
        case prefs::Kind::Choice:
            if (key == QLatin1String("ui/language"))
                return value.toString().isEmpty() ? tr("the system's") : language::displayName(value.toString());
            for (const prefs::Choice& c : s.choices)
                if (c.value == value.toString()) return c.label ? shown(c.label) : c.value;
            return value.toString();
        case prefs::Kind::Text:
            return value.toString().isEmpty() ? tr("Montage's own") : value.toString();
        default:
            return value.toString().isEmpty() ? tr("the one Montage finds") : value.toString();
    }
}

void PreferencesDialog::showRow(const QString& key) {
    const auto it = rows_.find(key);
    if (it == rows_.end()) return;
    const Row& r = it->second;
    const prefs::Setting& s = *prefs::find(key);
    const QVariant v = prefs::get(key);
    filling_ = true;
    switch (s.kind) {
        case prefs::Kind::Toggle:
            static_cast<QCheckBox*>(r.editor)->setChecked(v.toBool());
            break;
        case prefs::Kind::Whole:
            static_cast<QSpinBox*>(r.editor)->setValue(v.toInt());
            break;
        case prefs::Kind::Number:
            static_cast<QDoubleSpinBox*>(r.editor)->setValue(v.toDouble());
            break;
        case prefs::Kind::Choice: {
            auto* box = static_cast<QComboBox*>(r.editor);
            box->setCurrentIndex(std::max(0, box->findData(v.toString())));
            break;
        }
        default:
            r.editor->findChild<QLineEdit*>()->setText(v.toString());
    }
    filling_ = false;
    r.fallback->setText(tr("Default: %1").arg(shownValue(key, prefs::defaultFor(key))));
    const QString by = prefs::overriddenBy(key);
    const QString lockedBy = prefs::lockedBy(key);
    QStringList notes;
    if (!by.isEmpty()) notes << tr("Overridden by %1 in the environment: that is used, whatever is set here.").arg(by);
    if (!lockedBy.isEmpty()) notes << tr("Locked by the facility's settings file %1, so it cannot be changed here.").arg(lockedBy);
    r.note->setText(notes.join(QLatin1Char(' ')));
    r.note->setVisible(!notes.isEmpty() && r.matched);
    r.editor->setEnabled(by.isEmpty() && lockedBy.isEmpty());
    r.restore->setEnabled(lockedBy.isEmpty() && prefs::isSet(key));
}

void PreferencesDialog::refresh() {
    prefs::reloadFacility();
    const QString problem = prefs::facilityProblem();
    problem_->setText(problem);
    problem_->setVisible(!problem.isEmpty());
    for (const auto& [key, row] : rows_) showRow(key);
    search(search_->text());
}

void PreferencesDialog::search(const QString& words) {
    const QStringList each = words.toLower().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    std::vector<int> found(size_t(pages_->count()), 0);
    for (auto& [key, r] : rows_) {
        bool match = true;
        for (const QString& w : each) match = match && r.words.contains(w);
        r.matched = match;
        if (match) ++found[size_t(r.page)];
        for (QWidget* w : r.row->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) {
            if (w->property("settingKey").toString() != key) continue;
            w->setVisible(match && (w != r.note || !r.note->text().isEmpty()));
        }
    }
    int first = -1;
    for (int i = 0; i < pages_->count(); ++i) {
        pages_->item(i)->setHidden(found[size_t(i)] == 0);
        if (found[size_t(i)] > 0 && first < 0) first = i;
    }
    nothing_->setVisible(first < 0);
    if (first >= 0 && (pages_->currentRow() < 0 || pages_->item(pages_->currentRow())->isHidden())) pages_->setCurrentRow(first);
}

QString PreferencesDialog::currentPage() const {
    const QListWidgetItem* item = pages_->currentItem();
    return item ? item->data(Qt::UserRole).toString() : QString();
}

void PreferencesDialog::showPage(const QString& page) {
    for (int i = 0; i < pages_->count(); ++i)
        if (pages_->item(i)->data(Qt::UserRole).toString() == page) pages_->setCurrentRow(i);
}

bool PreferencesDialog::saveTo(const QString& path, QString* why) { return prefs::saveTo(path, why); }

bool PreferencesDialog::loadFrom(const QString& path, QStringList* passed, QString* why) {
    const bool ok = prefs::loadFrom(path, passed, why);
    refresh();
    for (const prefs::Setting& s : prefs::all()) emit changed(s.key);
    return ok;
}

}  // namespace montage
