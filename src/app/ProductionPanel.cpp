#include "ProductionPanel.h"

#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "EditorState.h"
#include "core/Production.h"

namespace montage {

ProductionPanel::ProductionPanel(EditorState* state, QWidget* parent) : QWidget(parent), state_(state) {
    setObjectName(QStringLiteral("productionPanel"));
    auto* v = new QVBoxLayout(this);
    title_ = new QLabel(tr("No production open (File › Production › Open Production)"), this);
    title_->setObjectName(QStringLiteral("productionTitle"));
    title_->setWordWrap(true);
    v->addWidget(title_);
    list_ = new QTreeWidget(this);
    list_->setObjectName(QStringLiteral("productionProjects"));
    list_->setHeaderLabels({tr("Project"), tr("Status"), tr("Saved")});
    list_->setRootIsDecorated(false);
    list_->header()->setStretchLastSection(false);
    list_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    v->addWidget(list_, 1);
    auto* row = new QHBoxLayout;
    open_ = new QPushButton(tr("Open"), this);
    openReadOnly_ = new QPushButton(tr("Open Read-Only"), this);
    import_ = new QPushButton(tr("Import Sequences…"), this);
    import_->setToolTip(tr("Bring sequences from the selected project into the open one"));
    newProject_ = new QPushButton(tr("New Project…"), this);
    for (QPushButton* b : {open_, openReadOnly_, import_, newProject_}) row->addWidget(b);
    row->addStretch(1);
    v->addLayout(row);
    connect(open_, &QPushButton::clicked, this, [this] {
        if (const QString p = selectedProject(); !p.isEmpty()) emit openRequested(p, false);
    });
    connect(openReadOnly_, &QPushButton::clicked, this, [this] {
        if (const QString p = selectedProject(); !p.isEmpty()) emit openRequested(p, true);
    });
    connect(import_, &QPushButton::clicked, this, [this] {
        if (const QString p = selectedProject(); !p.isEmpty()) emit importRequested(p);
    });
    connect(newProject_, &QPushButton::clicked, this, [this] {
        if (!folder_.isEmpty()) emit newProjectRequested(folder_);
    });
    connect(list_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) {
        if (item) emit openRequested(item->data(0, Qt::UserRole).toString(), false);
    });
    connect(list_, &QTreeWidget::itemSelectionChanged, this, &ProductionPanel::updateButtons);
    // Others open and close projects: the list follows every few seconds while it is shown.
    timer_ = new QTimer(this);
    timer_->setInterval(5000);
    connect(timer_, &QTimer::timeout, this, [this] {
        if (isVisible()) refresh();
    });
    timer_->start();
    connect(state_, &EditorState::lockStateChanged, this, &ProductionPanel::refresh);
    connect(state_, &EditorState::fileStateChanged, this, [this] {
        // A project of another production opened: follow it.
        const QString of = QString::fromStdString(productionOf(state_->filePath().toStdString()));
        if (!of.isEmpty() && of != folder_) setFolder(of);
        else refresh();
    });
    updateButtons();
}

void ProductionPanel::setFolder(const QString& folder) {
    folder_ = folder.isEmpty() ? QString() : QDir(folder).absolutePath();
    refresh();
}

QString ProductionPanel::selectedProject() const {
    const QTreeWidgetItem* item = list_->currentItem();
    return item ? item->data(0, Qt::UserRole).toString() : QString();
}

void ProductionPanel::refresh() {
    const QString keep = selectedProject();
    list_->clear();
    if (folder_.isEmpty()) {
        title_->setText(tr("No production open (File › Production › Open Production)"));
        updateButtons();
        return;
    }
    title_->setText(tr("Production: %1  (%2)").arg(QString::fromStdString(productionName(folder_.toStdString())),
                                                   QDir::toNativeSeparators(folder_)));
    const QString open = state_->filePath().isEmpty() ? QString() : QFileInfo(state_->filePath()).absoluteFilePath();
    for (const ProductionProject& pp : listProduction(folder_.toStdString())) {
        auto* item = new QTreeWidgetItem(list_);
        const QString path = QString::fromStdString(pp.path);
        QString rel = QString::fromStdString(pp.relative);
        if (rel.endsWith(QLatin1String(".montage"))) rel.chop(8);
        item->setText(0, rel);
        item->setData(0, Qt::UserRole, path);
        QString status;
        switch (pp.lock.state) {
            case LockState::Mine: status = tr("Editing (you)"); break;
            case LockState::Theirs: status = tr("Editing: %1").arg(QString::fromStdString(pp.lock.owner.describe())); break;
            case LockState::Stale: status = tr("Free (left open by %1)").arg(QString::fromStdString(pp.lock.owner.describe())); break;
            case LockState::Free: status = tr("Free"); break;
        }
        if (path == open && state_->readOnly()) status += tr(" — open here read-only");
        item->setText(1, status);
        item->setText(2, QLocale().toString(pp.modified, QLocale::ShortFormat));
        if (path == open) {
            QFont f = item->font(0);
            f.setBold(true);
            item->setFont(0, f);
        }
        if (path == keep) list_->setCurrentItem(item);
    }
    updateButtons();
}

void ProductionPanel::updateButtons() {
    const bool any = !selectedProject().isEmpty();
    open_->setEnabled(any);
    openReadOnly_->setEnabled(any);
    import_->setEnabled(any && selectedProject() != state_->filePath());
    newProject_->setEnabled(!folder_.isEmpty());
}

}  // namespace montage
