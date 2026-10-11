#include "GraphicsDialogs.h"

#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

#include "EditorState.h"
#include "core/EditOps.h"

namespace montage {

QString saveSequenceAsGraphicsTemplate(EditorState* state, QWidget* parent, const QString& name) {
    const Sequence* s = state->sequence();
    if (!s) return {};
    QString n = name.trimmed();
    if (n.isEmpty()) {
        bool ok = false;
        n = QInputDialog::getText(parent, QObject::tr("Save as Graphics Template"),
                                  QObject::tr("Name (its published fields are what each copy can change):"), QLineEdit::Normal,
                                  QString::fromStdString(s->name), &ok)
                .trimmed();
        if (!ok || n.isEmpty()) return {};
        for (const GraphicsTemplateInfo& t : listGraphicsTemplates(graphicsTemplatesDirectory()))
            if (QString::fromStdString(t.name).compare(n, Qt::CaseInsensitive) == 0 &&
                QMessageBox::question(parent, QObject::tr("Save as Graphics Template"),
                                      QObject::tr("Replace the template \"%1\"?").arg(n)) != QMessageBox::Yes)
                return {};
    }
    std::string path, err;
    if (!saveGraphicsTemplate(state->project(), s->id, n.toStdString(), graphicsTemplatesDirectory(), &path, &err)) {
        state->message(QObject::tr("The template was not saved: %1").arg(QString::fromStdString(err)), 6000);
        return {};
    }
    const size_t fields = liveFields(*s).size();
    state->message(fields ? QObject::tr("Saved \"%1\" with %n field(s)", "", int(fields)).arg(n)
                          : QObject::tr("Saved \"%1\" (it has no published fields yet: right-click a setting › Publish to Graphic)").arg(n),
                   6000);
    return QString::fromStdString(path);
}

QString publishPictureField(EditorState* state, Id clip, const QString& label) {
    QString why;
    state->edit(QObject::tr("Publish %1").arg(label), [&](Project& p, Sequence& s) {
        const auto f = mediaFieldFor(p, s, clip, label.toStdString());
        if (!f) {
            why = QObject::tr("Select a clip of an image or video to publish its picture");
            return false;
        }
        const edit::Result r = publishField(s, *f);
        why = QString::fromStdString(r.error);
        return r.ok;
    });
    return why;
}

std::vector<Id> fillGraphicsFromFile(EditorState* state, Id clip, const QString& file) {
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) {
        state->message(QObject::tr("Cannot read %1").arg(file), 6000);
        return {};
    }
    const std::string csv = f.readAll().toStdString();
    std::vector<Id> made;
    std::vector<std::string> unmatched;
    QString why;
    const bool ok = state->apply(QObject::tr("Fill Graphics from Table"), [&](Project& p, Sequence& s) {
        edit::Result r = fillGraphicsFromTable(p, s, clip, csv, &made, &unmatched);
        why = QString::fromStdString(r.error);
        return r;
    });
    if (!ok) {
        state->message(why.isEmpty() ? QObject::tr("The graphics could not be filled") : why, 6000);
        return {};
    }
    QString msg = QObject::tr("Filled %n graphic(s) from the table", "", int(made.size()));
    if (!unmatched.empty()) {
        QStringList cols;
        for (const std::string& c : unmatched) cols << QString::fromStdString(c);
        msg += QObject::tr(" (columns naming no field: %1)").arg(cols.join(QStringLiteral(", ")));
    }
    state->message(msg, 6000);
    return made;
}

// ---- The library ---------------------------------------------------------------------------------------------------

GraphicsTemplatesDialog::GraphicsTemplatesDialog(EditorState* state, QWidget* parent) : QDialog(parent), state_(state) {
    setWindowTitle(tr("Graphics Templates"));
    resize(520, 380);
    list_ = new QListWidget(this);
    list_->setObjectName(QStringLiteral("templates"));
    detail_ = new QLabel(this);
    detail_->setWordWrap(true);
    auto* insertBtn = new QPushButton(tr("Insert"), this);
    insertBtn->setObjectName(QStringLiteral("insertTemplate"));
    auto* deleteBtn = new QPushButton(tr("Delete…"), this);
    auto* folderBtn = new QPushButton(tr("Show Folder"), this);
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(close, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(list_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row < 0 || row >= int(templates_.size())) {
            detail_->setText(tr("Save a sequence as a template with Sequence › Save as Graphics Template; the settings you "
                                "publish (right-click one in the Inspector) are what each copy can change."));
            return;
        }
        const GraphicsTemplateInfo& t = templates_[size_t(row)];
        QStringList fields;
        for (const std::string& f : t.fields) fields << QString::fromStdString(f);
        detail_->setText(tr("%1 x %2, %3 s. Fields: %4").arg(t.width).arg(t.height).arg(t.seconds, 0, 'f', 1)
                             .arg(fields.isEmpty() ? tr("none") : fields.join(QStringLiteral(", "))));
    });
    connect(list_, &QListWidget::itemDoubleClicked, this, [this] {
        if (insert(list_->currentRow())) accept();
    });
    connect(insertBtn, &QPushButton::clicked, this, [this] {
        if (insert(list_->currentRow())) accept();
    });
    connect(deleteBtn, &QPushButton::clicked, this, [this] {
        const int row = list_->currentRow();
        if (row < 0 || row >= int(templates_.size())) return;
        const QString name = QString::fromStdString(templates_[size_t(row)].name);
        if (QMessageBox::question(this, tr("Delete Template"), tr("Delete the template \"%1\" and its files?").arg(name)) != QMessageBox::Yes)
            return;
        QDir(QFileInfo(QString::fromStdString(templates_[size_t(row)].path)).absolutePath()).removeRecursively();
        reload();
    });
    connect(folderBtn, &QPushButton::clicked, this, [] {
        QDir().mkpath(QString::fromStdString(graphicsTemplatesDirectory()));
        QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(graphicsTemplatesDirectory())));
    });
    auto* buttons = new QHBoxLayout;
    buttons->addWidget(insertBtn);
    buttons->addWidget(deleteBtn);
    buttons->addWidget(folderBtn);
    buttons->addStretch(1);
    buttons->addWidget(close);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(list_, 1);
    layout->addWidget(detail_);
    layout->addLayout(buttons);
    reload();
}

void GraphicsTemplatesDialog::reload() {
    templates_ = listGraphicsTemplates(graphicsTemplatesDirectory());
    list_->clear();
    for (const GraphicsTemplateInfo& t : templates_)
        list_->addItem(tr("%1  (%n field(s))", "", int(t.fields.size())).arg(QString::fromStdString(t.name)));
    list_->setCurrentRow(templates_.empty() ? -1 : 0);
    if (templates_.empty()) emit list_->currentRowChanged(-1);
}

Id GraphicsTemplatesDialog::insert(int row) {
    if (row < 0 || row >= int(templates_.size()) || !state_->sequence()) return 0;
    const std::string path = templates_[size_t(row)].path;
    const FrameTime at = state_->playhead();
    Id made = 0;
    QString why;
    state_->apply(tr("Insert %1").arg(QString::fromStdString(templates_[size_t(row)].name)), [&](Project& p, Sequence& s) {
        edit::Result r = insertGraphicsTemplate(p, s, path, at, -1);
        if (r.ok && !r.created.empty()) made = r.created[0];
        why = QString::fromStdString(r.error);
        return r;
    });
    if (!made) state_->message(why.isEmpty() ? tr("The template could not be inserted") : why, 6000);
    return made;
}

// ---- Fields ----------------------------------------------------------------------------------------------------------

GraphicFieldsDialog::GraphicFieldsDialog(EditorState* state, QWidget* parent) : QDialog(parent), state_(state) {
    setWindowTitle(tr("Graphic Fields"));
    resize(460, 320);
    list_ = new QListWidget(this);
    list_->setObjectName(QStringLiteral("fields"));
    auto* note = new QLabel(tr("The settings this sequence publishes: every clip that plays it can set them in the Inspector. "
                               "Publish more by right-clicking a setting of a clip in the Inspector."),
                            this);
    note->setWordWrap(true);
    auto* renameBtn = new QPushButton(tr("Rename…"), this);
    auto* up = new QPushButton(tr("Move Up"), this);
    auto* down = new QPushButton(tr("Move Down"), this);
    auto* removeBtn = new QPushButton(tr("Remove"), this);
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(close, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(renameBtn, &QPushButton::clicked, this, [this] {
        const int row = list_->currentRow();
        const Sequence* s = state_->sequence();
        if (!s || row < 0 || row >= int(s->fields.size())) return;
        bool ok = false;
        const QString label = QInputDialog::getText(this, tr("Rename Field"), tr("Label:"), QLineEdit::Normal,
                                                    QString::fromStdString(s->fields[size_t(row)].label), &ok);
        if (ok) rename(row, label);
    });
    connect(up, &QPushButton::clicked, this, [this] { move(list_->currentRow(), -1); });
    connect(down, &QPushButton::clicked, this, [this] { move(list_->currentRow(), 1); });
    connect(removeBtn, &QPushButton::clicked, this, [this] { remove(list_->currentRow()); });
    auto* buttons = new QHBoxLayout;
    for (QPushButton* b : {renameBtn, up, down, removeBtn}) buttons->addWidget(b);
    buttons->addStretch(1);
    buttons->addWidget(close);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(note);
    layout->addWidget(list_, 1);
    layout->addLayout(buttons);
    reload();
}

void GraphicFieldsDialog::reload() {
    const int keep = list_->currentRow();
    list_->clear();
    const Sequence* s = state_->sequence();
    if (!s) return;
    const std::vector<TemplateField> live = liveFields(*s);
    for (const TemplateField& f : s->fields) {
        const Clip* c = edit::clipById(*s, f.clip);
        const bool there = std::any_of(live.begin(), live.end(), [&](const TemplateField& l) { return l.id == f.id; });
        list_->addItem(tr("%1  (%2, %3)%4")
                           .arg(QString::fromStdString(f.label), QString::fromStdString(f.kind),
                                c ? QString::fromStdString(c->name.empty() ? std::string("clip") : c->name) : tr("clip gone"),
                                there ? QString() : tr(" — no longer in the graphic")));
    }
    list_->setCurrentRow(std::min(keep, list_->count() - 1));
}

bool GraphicFieldsDialog::rename(int row, const QString& label) {
    const QString l = label.trimmed();
    if (l.isEmpty()) return false;
    const bool ok = state_->edit(tr("Rename Field"), [row, l](Project&, Sequence& s) {
        if (row < 0 || row >= int(s.fields.size())) return false;
        s.fields[size_t(row)].label = l.toStdString();
        return true;
    });
    reload();
    return ok;
}

bool GraphicFieldsDialog::remove(int row) {
    const bool ok = state_->edit(tr("Remove Field"), [row](Project&, Sequence& s) {
        if (row < 0 || row >= int(s.fields.size())) return false;
        s.fields.erase(s.fields.begin() + row);
        return true;
    });
    reload();
    return ok;
}

bool GraphicFieldsDialog::move(int row, int by) {
    const bool ok = state_->edit(tr("Move Field"), [row, by](Project&, Sequence& s) {
        const int to = row + by;
        if (row < 0 || row >= int(s.fields.size()) || to < 0 || to >= int(s.fields.size())) return false;
        std::swap(s.fields[size_t(row)], s.fields[size_t(to)]);
        return true;
    });
    if (ok) list_->setCurrentRow(row + by);
    reload();
    if (ok) list_->setCurrentRow(row + by);
    return ok;
}

}  // namespace montage
