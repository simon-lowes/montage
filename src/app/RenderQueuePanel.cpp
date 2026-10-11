#include "RenderQueuePanel.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QPushButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>

#include "RenderQueue.h"

namespace montage {

RenderQueuePanel::RenderQueuePanel(RenderQueue* queue, QWidget* parent) : QWidget(parent), queue_(queue) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(4, 4, 4, 4);
    list_ = new QTreeWidget(this);
    list_->setObjectName(QStringLiteral("renderQueue"));
    list_->setColumnCount(4);
    list_->setHeaderLabels({tr("Job"), tr("Preset"), tr("Output"), tr("Status")});
    list_->setRootIsDecorated(false);
    list_->setUniformRowHeights(true);
    list_->header()->setStretchLastSection(true);
    list_->header()->resizeSection(0, 180);
    list_->header()->resizeSection(1, 150);
    list_->header()->resizeSection(2, 220);
    lay->addWidget(list_, 1);
    auto* row = new QHBoxLayout;
    auto button = [&](const QString& text, const char* name) {
        auto* b = new QPushButton(text, this);
        b->setObjectName(QString::fromLatin1(name));
        row->addWidget(b);
        return b;
    };
    start_ = button(tr("Start Queue"), "queueStart");
    stop_ = button(tr("Stop"), "queueStop");
    remove_ = button(tr("Remove"), "queueRemove");
    retry_ = button(tr("Retry"), "queueRetry");
    clear_ = button(tr("Clear Finished"), "queueClear");
    reveal_ = button(tr("Show File"), "queueReveal");
    row->addStretch(1);
    lay->addLayout(row);
    connect(start_, &QPushButton::clicked, queue_, &RenderQueue::start);
    connect(stop_, &QPushButton::clicked, queue_, &RenderQueue::stop);
    connect(remove_, &QPushButton::clicked, this, [this] { queue_->remove(selectedJob()); });
    connect(retry_, &QPushButton::clicked, this, [this] { queue_->retry(selectedJob()); });
    connect(clear_, &QPushButton::clicked, queue_, &RenderQueue::clearFinished);
    connect(reveal_, &QPushButton::clicked, this, [this] {
        if (const RenderQueue::Job* j = queue_->job(selectedJob()))
            QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(QString::fromStdString(j->settings.path)).absolutePath()));
    });
    connect(list_, &QTreeWidget::itemSelectionChanged, this, &RenderQueuePanel::refresh);
    connect(queue_, &RenderQueue::changed, this, &RenderQueuePanel::refresh);
    refresh();
}

int RenderQueuePanel::selectedJob() const {
    const QTreeWidgetItem* it = list_->currentItem();
    return it && it->isSelected() ? it->data(0, Qt::UserRole).toInt() : 0;
}

void RenderQueuePanel::refresh() {
    const int keep = selectedJob();
    const auto& jobs = queue_->jobs();
    // Rebuild only when the jobs change; otherwise just update the status column.
    bool same = list_->topLevelItemCount() == int(jobs.size());
    for (int i = 0; same && i < int(jobs.size()); ++i) same = list_->topLevelItem(i)->data(0, Qt::UserRole).toInt() == jobs[size_t(i)].id;
    if (!same) {
        list_->clear();
        for (const auto& j : jobs) {
            auto* it = new QTreeWidgetItem(list_, {j.name, j.preset, QDir::toNativeSeparators(QString::fromStdString(j.settings.path)), {}});
            it->setData(0, Qt::UserRole, j.id);
            it->setToolTip(2, it->text(2));
            if (j.id == keep) {
                list_->setCurrentItem(it);
                it->setSelected(true);
            }
        }
    }
    for (int i = 0; i < int(jobs.size()); ++i) {
        const auto& j = jobs[size_t(i)];
        QString status;
        switch (j.status) {
            case RenderQueue::Status::Waiting: status = tr("Waiting"); break;
            case RenderQueue::Status::Rendering: status = tr("Rendering %1%").arg(int(j.progress * 100)); break;
            case RenderQueue::Status::Done:
                status = tr("Done %1").arg(j.finished.toString("HH:mm")) + (j.encoder.isEmpty() ? QString() : " (" + j.encoder + ")");
                break;
            case RenderQueue::Status::Failed: status = tr("Failed: %1").arg(j.error); break;
            case RenderQueue::Status::Cancelled: status = tr("Cancelled"); break;
        }
        list_->topLevelItem(i)->setText(3, status);
        list_->topLevelItem(i)->setToolTip(3, status);
    }
    const RenderQueue::Job* sel = queue_->job(selectedJob());
    const bool waiting = std::any_of(jobs.begin(), jobs.end(), [](const auto& j) { return j.status == RenderQueue::Status::Waiting; });
    start_->setEnabled(!queue_->running() && waiting);
    stop_->setEnabled(queue_->running());
    remove_->setEnabled(sel && sel->status != RenderQueue::Status::Rendering);
    retry_->setEnabled(sel && (sel->status == RenderQueue::Status::Failed || sel->status == RenderQueue::Status::Cancelled ||
                               sel->status == RenderQueue::Status::Done));
    clear_->setEnabled(std::any_of(jobs.begin(), jobs.end(), [](const auto& j) { return j.status == RenderQueue::Status::Done; }));
    reveal_->setEnabled(sel != nullptr);
}

}  // namespace montage
