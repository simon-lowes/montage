#include "LinkMediaDialog.h"

#include <QCheckBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include "EditorState.h"
#include "media/Relink.h"

namespace montage {

LinkMediaDialog::LinkMediaDialog(EditorState* state, QWidget* parent) : QDialog(parent), state_(state) {
    setObjectName(QStringLiteral("linkMedia"));
    setWindowTitle(tr("Link Media"));
    resize(720, 360);
    auto* lay = new QVBoxLayout(this);
    auto* intro = new QLabel(tr("These files cannot be found. Locate one and the others are looked for beside it, or "
                                "search a folder for all of them. Clips using them show as Media Offline until then."),
                             this);
    intro->setWordWrap(true);
    lay->addWidget(intro);
    table_ = new QTableWidget(0, 2, this);
    table_->setObjectName(QStringLiteral("linkMediaList"));
    table_->setHorizontalHeaderLabels({tr("Media"), tr("Last known place")});
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->verticalHeader()->hide();
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    lay->addWidget(table_, 1);
    others_ = new QCheckBox(tr("Relink others automatically"), this);
    others_->setObjectName(QStringLiteral("linkOthers"));
    others_->setChecked(true);
    lay->addWidget(others_);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("linkStatus"));
    lay->addWidget(status_);
    auto* buttons = new QHBoxLayout;
    auto* locateBtn = new QPushButton(tr("Locate..."), this);
    locateBtn->setObjectName(QStringLiteral("linkLocate"));
    auto* searchBtn = new QPushButton(tr("Search Folder..."), this);
    searchBtn->setObjectName(QStringLiteral("linkSearch"));
    auto* closeBtn = new QPushButton(tr("Leave Offline"), this);
    closeBtn->setObjectName(QStringLiteral("linkClose"));
    buttons->addWidget(locateBtn);
    buttons->addWidget(searchBtn);
    buttons->addStretch(1);
    buttons->addWidget(closeBtn);
    lay->addLayout(buttons);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);
    auto locateSelected = [this] {
        const int row = table_->currentRow() >= 0 ? table_->currentRow() : 0;
        if (row >= int(offline_.size())) return;
        const Id id = offline_[size_t(row)];
        const MediaItem* m = state_->project().findMedia(id);
        if (!m) return;
        const QString name = QFileInfo(QString::fromStdString(m->path)).fileName();
        const QString f = QFileDialog::getOpenFileName(this, tr("Locate %1").arg(name), QString(), tr("%1;;All files (*)").arg(name));
        if (!f.isEmpty()) locate(id, f);
    };
    connect(locateBtn, &QPushButton::clicked, this, locateSelected);
    connect(table_, &QTableWidget::cellDoubleClicked, this, locateSelected);
    connect(searchBtn, &QPushButton::clicked, this, [this] {
        const QString d = QFileDialog::getExistingDirectory(this, tr("Search Folder for Media"));
        if (!d.isEmpty()) searchFolder(d);
    });
    refresh();
}

void LinkMediaDialog::refresh() {
    state_->recheckOffline();
    offline_ = offlineMedia(state_->project());
    table_->setRowCount(int(offline_.size()));
    for (size_t i = 0; i < offline_.size(); ++i) {
        const MediaItem* m = state_->project().findMedia(offline_[i]);
        table_->setItem(int(i), 0, new QTableWidgetItem(QString::fromStdString(m ? m->name : std::string())));
        table_->setItem(int(i), 1, new QTableWidgetItem(QString::fromStdString(m ? m->path : std::string())));
    }
    if (!offline_.empty() && table_->currentRow() < 0) table_->selectRow(0);
    status_->setText(tr("%n file(s) offline", "", int(offline_.size())));
    if (offline_.empty() && isVisible()) {
        state_->message(tr("All media linked"), 4000);
        accept();
    }
}

int LinkMediaDialog::locate(Id id, const QString& file) {
    std::string why;
    int found = 0;
    const bool others = others_->isChecked();
    state_->edit(tr("Link Media"), [&](Project& p, Sequence&) {
        if (!relinkMedia(p, id, QDir::cleanPath(file).toStdString(), RelinkCheck::Strict, &why)) return false;
        found = 1;
        if (others) {
            // Beside it, and in the folder above (Day 1 / Day 2 cards).
            const QDir dir = QFileInfo(file).absoluteDir();
            QDir up = dir;
            found += int(relinkFromFolder(p, dir.absolutePath().toStdString(), {}, 0).size());
            if (up.cdUp()) found += int(relinkFromFolder(p, up.absolutePath().toStdString(), {}, 2).size());
        }
        return true;
    });
    refresh();
    if (!found) status_->setText(tr("That file does not match: %1").arg(QString::fromStdString(why)));
    return found;
}

int LinkMediaDialog::searchFolder(const QString& folder) {
    int found = 0;
    state_->edit(tr("Link Media"), [&](Project& p, Sequence&) {
        found = int(relinkFromFolder(p, QDir::cleanPath(folder).toStdString()).size());
        return found > 0;
    });
    refresh();
    if (!found && !offline_.empty()) status_->setText(tr("None of the files were found in %1").arg(folder));
    return found;
}

}  // namespace montage
