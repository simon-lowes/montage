#include "RenderQueuePanel.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTimer>
#include <QtConcurrent>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>

#include "RenderQueue.h"
#include "Settings.h"

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
    auto* split = new QSplitter(Qt::Vertical, this);
    auto* local = new QWidget(split);
    auto* localLay = new QVBoxLayout(local);
    localLay->setContentsMargins(0, 0, 0, 0);
    localLay->addWidget(list_, 1);
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
    send_ = button(tr("Send to Farm"), "queueSendToFarm");
    send_->setToolTip(tr("Send the job to the render farm, for its render nodes, and take it off this queue"));
    row->addStretch(1);
    localLay->addLayout(row);

    // The render farm.
    auto* farmBox = new QWidget(split);
    auto* farmLay = new QVBoxLayout(farmBox);
    farmLay->setContentsMargins(0, 0, 0, 0);
    auto* farmRow = new QHBoxLayout;
    farmLabel_ = new QLabel(farmBox);
    farmLabel_->setObjectName(QStringLiteral("farmFolder"));
    auto* choose = new QPushButton(tr("Farm Folder…"), farmBox);
    choose->setObjectName(QStringLiteral("farmChoose"));
    choose->setToolTip(tr("A folder every computer reaches (a network share); start a render node on each with\n"
                          "montage-cli node <folder>"));
    nodes_ = new QLabel(farmBox);
    nodes_->setObjectName(QStringLiteral("farmNodes"));
    farmRow->addWidget(farmLabel_, 1);
    farmRow->addWidget(nodes_);
    farmRow->addWidget(choose);
    farmLay->addLayout(farmRow);
    farmList_ = new QTreeWidget(farmBox);
    farmList_->setObjectName(QStringLiteral("farmJobs"));
    farmList_->setColumnCount(4);
    farmList_->setHeaderLabels({tr("Farm Job"), tr("Node"), tr("Output"), tr("Status")});
    farmList_->setRootIsDecorated(false);
    farmList_->setUniformRowHeights(true);
    farmList_->header()->setStretchLastSection(true);
    farmList_->header()->resizeSection(0, 180);
    farmList_->header()->resizeSection(1, 110);
    farmList_->header()->resizeSection(2, 220);
    farmLay->addWidget(farmList_, 1);
    auto* farmButtons = new QHBoxLayout;
    auto farmButton = [&](const QString& text, const char* name) {
        auto* b = new QPushButton(text, farmBox);
        b->setObjectName(QString::fromLatin1(name));
        farmButtons->addWidget(b);
        return b;
    };
    farmCancel_ = farmButton(tr("Cancel"), "farmCancel");
    farmRetry_ = farmButton(tr("Retry"), "farmRetry");
    farmRemove_ = farmButton(tr("Remove"), "farmRemove");
    // (beside the farm's own buttons: the queue's row is already the panel's widest)
    segments_ = new QSpinBox(farmBox);
    segments_->setObjectName(QStringLiteral("queueSegments"));
    segments_->setRange(1, 64);
    segments_->setSpecialValueText(tr("Whole"));
    segments_->setSuffix(tr(" segments"));
    segments_->setToolTip(tr("Split a job sent to the farm into segments (at cuts near equal lengths) that several nodes\n"
                             "render at once, then one joins: ProRes, DNxHR and FFV1 frame for frame, H.264, H.265 and AV1\n"
                             "with rate control per segment, and image sequences. The sound is rendered once, by the node that joins."));
    segments_->setValue(std::clamp(appSettings().value(QStringLiteral("renderFarm/segments"), 1).toInt(), 1, 64));
    connect(segments_, &QSpinBox::valueChanged, this, [](int v) { appSettings().setValue(QStringLiteral("renderFarm/segments"), v); });
    farmButtons->addWidget(segments_);
    farmButtons->addStretch(1);
    farmLay->addLayout(farmButtons);
    split->addWidget(local);
    split->addWidget(farmBox);
    lay->addWidget(split, 1);
    farmTimer_ = new QTimer(this);
    farmTimer_->setInterval(1500);
    connect(farmTimer_, &QTimer::timeout, this, &RenderQueuePanel::readFarmLater);
    connect(&reading_, &QFutureWatcher<FarmView>::finished, this, [this] {
        // (A read of a farm since changed is dropped.)
        if (reading_.future().resultCount() && reading_.result().farm == farm_) showFarm(reading_.result());
    });
    connect(choose, &QPushButton::clicked, this, [this] {
        const QString f = QFileDialog::getExistingDirectory(this, tr("Render Farm Folder"), farm_);
        if (!f.isEmpty()) setFarmFolder(f);
    });
    connect(send_, &QPushButton::clicked, this, [this] { sendToFarm(selectedJob()); });
    auto farmAction = [this](bool (*act)(const std::string&, const std::string&, std::string*)) {
        std::string err;
        const std::string id = selectedFarmJob();
        if (!id.empty() && !act(farm_.toStdString(), id, &err)) QMessageBox::warning(this, tr("Render Farm"), QString::fromStdString(err));
        refreshFarm();
    };
    connect(farmCancel_, &QPushButton::clicked, this, [farmAction] { farmAction(&cancelFarmJob); });
    connect(farmRetry_, &QPushButton::clicked, this, [farmAction] { farmAction(&retryFarmJob); });
    connect(farmRemove_, &QPushButton::clicked, this, [farmAction] { farmAction(&removeFarmJob); });
    connect(farmList_, &QTreeWidget::itemSelectionChanged, this, &RenderQueuePanel::refreshFarm);
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
    setFarmFolder(appSettings().value(QStringLiteral("renderFarm/folder")).toString());
}

void RenderQueuePanel::setFarmFolder(const QString& folder) {
    farm_ = folder;
    appSettings().setValue(QStringLiteral("renderFarm/folder"), folder);
    farmLabel_->setText(folder.isEmpty() ? tr("No render farm folder") : tr("Farm: %1").arg(QDir::toNativeSeparators(folder)));
    farmLabel_->setToolTip(farmLabel_->text());
    if (folder.isEmpty()) farmTimer_->stop();
    else farmTimer_->start();
    refresh();
    refreshFarm();
}

bool RenderQueuePanel::sendToFarm(int id) {
    const RenderQueue::Job* j = queue_->job(id);
    if (!j || farm_.isEmpty() || j->status == RenderQueue::Status::Rendering) return false;
    std::string err;
    if (!submitFarmJob(farm_.toStdString(), j->name.toStdString(), *j->project, j->sequence, j->settings, j->stems, nullptr, &err, segments_->value())) {
        QMessageBox::warning(this, tr("Render Farm"), tr("Not sent to the render farm: %1").arg(QString::fromStdString(err)));
        return false;
    }
    queue_->remove(id);
    refreshFarm();
    return true;
}

std::string RenderQueuePanel::selectedFarmJob() const {
    const QTreeWidgetItem* it = farmList_->currentItem();
    return it && it->isSelected() ? it->data(0, Qt::UserRole).toString().toStdString() : std::string();
}

void RenderQueuePanel::refreshFarm() {
    FarmView v;
    if (!farm_.isEmpty()) v = {farm_, farmJobs(farm_.toStdString()), farmNodes(farm_.toStdString())};
    showFarm(v);
}

void RenderQueuePanel::readFarmLater() {
    // (A share can be slow: read on a worker, shown when read; skipped while the panel is hidden or a read is going.)
    if (farm_.isEmpty() || !isVisible() || reading_.isRunning()) return;
    const QString farm = farm_;
    reading_.setFuture(QtConcurrent::run([farm] { return FarmView{farm, farmJobs(farm.toStdString()), farmNodes(farm.toStdString())}; }));
}

void RenderQueuePanel::showFarm(const FarmView& view) {
    const std::string keep = selectedFarmJob();
    const std::vector<FarmJob>& jobs = view.jobs;
    bool same = farmList_->topLevelItemCount() == int(jobs.size());
    for (int i = 0; same && i < int(jobs.size()); ++i)
        same = farmList_->topLevelItem(i)->data(0, Qt::UserRole).toString().toStdString() == jobs[size_t(i)].id;
    if (!same) {
        const QSignalBlocker block(farmList_);
        farmList_->clear();
        for (const FarmJob& j : jobs) {
            auto* it = new QTreeWidgetItem(farmList_, {QString::fromStdString(j.name), {}, QDir::toNativeSeparators(QString::fromStdString(j.output)), {}});
            it->setData(0, Qt::UserRole, QString::fromStdString(j.id));
            it->setToolTip(0, tr("%1, sent by %2").arg(QString::fromStdString(j.id), QString::fromStdString(j.submittedBy)));
            it->setToolTip(2, it->text(2));
            if (j.id == keep) {
                farmList_->setCurrentItem(it);
                it->setSelected(true);
            }
        }
    }
    const FarmJob* sel = nullptr;
    for (int i = 0; i < int(jobs.size()); ++i) {
        const FarmJob& j = jobs[size_t(i)];
        if (j.id == keep) sel = &j;
        QString status;
        switch (j.state) {
            case FarmState::Waiting: status = tr("Waiting for a node"); break;
            case FarmState::Rendering:
                status = j.cancelRequested ? tr("Cancelling") : tr("Rendering %1%").arg(int(j.progress * 100));
                // (Its heartbeat unchanged for a minute here: a node watching the farm will take the job back.)
                if (beats_.stale(j.id + "|" + j.claim, j.beat)) status += tr(" (its node is not answering)");
                break;
            case FarmState::Done:
                status = tr("Done") + (j.encoder.empty() ? QString() : QStringLiteral(" (") + QString::fromStdString(j.encoder) + ')');
                break;
            case FarmState::Failed: status = tr("Failed: %1").arg(QString::fromStdString(j.error)); break;
            case FarmState::Cancelled: status = tr("Cancelled"); break;
        }
        QTreeWidgetItem* it = farmList_->topLevelItem(i);
        it->setText(1, QString::fromStdString(j.node));
        it->setText(3, status);
        it->setToolTip(3, status);
    }
    QStringList nodes;
    int alive = 0;
    for (const FarmNodeInfo& n : view.nodes) {
        // (A node whose heartbeat has not moved for a minute here is not counted: it crashed, or its computer is off.)
        if (n.state == "stopped" || beats_.stale("node|" + n.name, n.beat)) continue;
        ++alive;
        nodes << QString::fromStdString(n.name) + ": " + (n.state == "rendering" ? tr("rendering") : tr("idle"));
    }
    nodes_->setText(farm_.isEmpty() ? QString() : alive ? tr("%n node(s)", nullptr, alive) : tr("No nodes running"));
    nodes_->setToolTip(nodes.join('\n'));
    farmCancel_->setEnabled(sel && (sel->state == FarmState::Waiting || (sel->state == FarmState::Rendering && !sel->cancelRequested)));
    farmRetry_->setEnabled(sel && (sel->state == FarmState::Failed || sel->state == FarmState::Cancelled || sel->state == FarmState::Done));
    farmRemove_->setEnabled(sel && sel->state != FarmState::Rendering);
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
        // (Not refreshed again from inside the rebuild by its selection signals.)
        const QSignalBlocker block(list_);
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
    send_->setEnabled(sel && !farm_.isEmpty() && sel->status != RenderQueue::Status::Rendering && sel->status != RenderQueue::Status::Done);
}

}  // namespace montage
