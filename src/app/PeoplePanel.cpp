#include "PeoplePanel.h"

#include <QEventLoop>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QProgressDialog>
#include <QPushButton>
#include <QSplitter>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>

#include "EditorState.h"
#include "ModelPacks.h"
#include "ThumbnailCache.h"
#include "core/MediaLog.h"
#include "media/Faces.h"

namespace montage {

namespace {
bool facesFindable(const MediaItem& m) {
    return (m.kind == MediaKind::Video || m.kind == MediaKind::Image) && m.hasVideo && !m.path.empty() && !m.subclipOf;
}
}  // namespace

bool indexFacesIn(EditorState* state, const std::vector<Id>& media, QWidget* parent) {
    if (!faceSearchAvailable()) return false;
    struct Job {
        Id id;
        std::string path;
        double duration;
    };
    std::vector<Job> jobs;
    for (Id id : media) {
        const MediaItem* m = state->project().findMedia(id);
        if (m && m->subclipOf) m = state->project().findMedia(m->subclipOf);  // a subclip's media holds the index
        if (!m || !facesFindable(*m) || m->faces) continue;
        if (std::none_of(jobs.begin(), jobs.end(), [&](const Job& j) { return j.id == m->id; }))
            jobs.push_back({m->id, m->path, m->kind == MediaKind::Image ? 0.0 : m->duration});
    }
    if (jobs.empty()) return true;
    if (!ensureModelPack(parent, faceModel(), QObject::tr("Find People"),
                         QObject::tr("Finding people uses YuNet (MIT licence) to find faces and SFace (Apache 2.0) to tell "
                                     "them apart. Both run on this computer; no picture leaves it.")))
        return false;
    QProgressDialog progress(QObject::tr("Looking for faces…"), QObject::tr("Cancel"), 0, 1000, parent);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(300);
    auto done = std::make_shared<std::atomic<double>>(0.0);
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    QObject::connect(&progress, &QProgressDialog::canceled, &progress, [cancel] { *cancel = true; });
    QTimer tick;
    QObject::connect(&tick, &QTimer::timeout, &progress, [&progress, done] { progress.setValue(int(*done * 1000)); });
    tick.start(100);
    using Out = std::pair<std::vector<std::pair<Id, std::shared_ptr<const FaceIndex>>>, std::string>;
    QFutureWatcher<Out> watcher;
    QEventLoop wait;
    QObject::connect(&watcher, &QFutureWatcher<Out>::finished, &wait, &QEventLoop::quit);
    watcher.setFuture(QtConcurrent::run([jobs, done, cancel] {
        Out out;
        for (size_t i = 0; i < jobs.size() && !*cancel; ++i) {
            FaceIndex f;
            std::string err;
            const double n = double(jobs.size());
            if (indexFaces(jobs[i].path, jobs[i].duration, f, 0, 8, 32, [&](double x) { *done = (double(i) + x) / n; }, cancel.get(), &err))
                out.first.emplace_back(jobs[i].id, std::make_shared<const FaceIndex>(std::move(f)));
            else if (!*cancel && out.second.empty())
                out.second = err;
        }
        return out;
    }));
    if (!watcher.isFinished()) wait.exec();
    tick.stop();
    QObject::disconnect(&progress, &QProgressDialog::canceled, nullptr, nullptr);  // closing a progress dialog emits canceled()
    progress.close();
    const Out r = watcher.result();
    if (!r.first.empty()) {
        // Saved with the project, but not an edit to undo.
        const auto indexes = r.first;
        state->amend([indexes](Project& p, Sequence&) {
            for (const auto& [id, f] : indexes)
                if (MediaItem* m = p.findMedia(id)) m->faces = f;
            groupPeople(p);
            return true;
        });
    }
    if (!r.second.empty()) state->message(QString::fromStdString(r.second), 6000);
    return !*cancel && r.second.empty();
}

PeoplePanel::PeoplePanel(EditorState* state, QWidget* parent)
    : QWidget(parent), state_(state), thumbs_(&ThumbnailCache::instance()) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(4, 4, 4, 4);
    auto* row = new QHBoxLayout;
    status_ = new QLabel(this);
    status_->setWordWrap(true);
    status_->setObjectName(QStringLiteral("peopleStatus"));
    findBtn_ = new QPushButton(tr("Find People"), this);
    findBtn_->setObjectName(QStringLiteral("findPeople"));
    findBtn_->setToolTip(tr("Look for faces in the videos and stills not looked through yet, and group them into people"));
    row->addWidget(status_, 1);
    row->addWidget(findBtn_);
    lay->addLayout(row);
    auto* split = new QSplitter(Qt::Vertical, this);
    peopleList_ = new QListWidget(split);
    peopleList_->setObjectName(QStringLiteral("peopleList"));
    peopleList_->setViewMode(QListView::IconMode);
    peopleList_->setIconSize(QSize(72, 72));
    peopleList_->setGridSize(QSize(104, 112));
    peopleList_->setResizeMode(QListView::Adjust);
    peopleList_->setMovement(QListView::Static);
    peopleList_->setWordWrap(true);
    peopleList_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    momentList_ = new QListWidget(split);
    momentList_->setObjectName(QStringLiteral("personMoments"));
    momentList_->setIconSize(QSize(128, 72));
    momentList_->setUniformItemSizes(true);
    split->addWidget(peopleList_);
    split->addWidget(momentList_);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 1);
    lay->addWidget(split, 1);

    connect(findBtn_, &QPushButton::clicked, this, [this] { QTimer::singleShot(0, this, [this] { findPeople(); }); });
    connect(peopleList_, &QListWidget::currentRowChanged, this, [this](int r) {
        if (!filling_) selectPerson(r);
    });
    connect(peopleList_, &QListWidget::itemChanged, this, [this](QListWidgetItem* it) {
        if (filling_) return;
        const int r = peopleList_->row(it);
        if (r < 0 || r >= int(people_.size())) return;
        const QString name = it->text().trimmed();
        const int id = people_[size_t(r)].id;
        QTimer::singleShot(0, this, [this, id, name] { rename(id, name); });
    });
    connect(momentList_, &QListWidget::itemActivated, this, [this](QListWidgetItem* it) { open(momentList_->row(it)); });
    peopleList_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(peopleList_, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        const int r = peopleList_->row(peopleList_->itemAt(pos));
        if (r < 0 || r >= int(people_.size())) return;
        const PersonSummary person = people_[size_t(r)];
        QMenu menu(this);
        menu.addAction(tr("Name…"), this, [this, person] {
            bool ok = false;
            const QString name = QInputDialog::getText(this, tr("Name Person"), tr("Name:"), QLineEdit::Normal,
                                                       QString::fromStdString(person.name), &ok);
            if (ok) rename(person.id, name.trimmed());
        });
        QMenu* mergeMenu = menu.addMenu(tr("Same Person As"));
        mergeMenu->setToolTip(tr("Two groups that are one person: join them"));
        for (const PersonSummary& other : people_)
            if (other.id != person.id)
                mergeMenu->addAction(QString::fromStdString(other.name), this, [this, person, other] { merge(person.id, other.id); });
        mergeMenu->setEnabled(people_.size() > 1);
        menu.addAction(tr("Make Smart Bin"), this, [this, person] { makeSmartBin(person.id); });
        menu.exec(peopleList_->viewport()->mapToGlobal(pos));
    });
    momentList_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(momentList_, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        const int r = momentList_->row(momentList_->itemAt(pos));
        if (r < 0) return;
        QMenu menu(this);
        menu.addAction(tr("Open in Source Monitor"), this, [this, r] { open(r); });
        menu.addAction(tr("Make Subclip"), this, [this, r] { makeSubclip(r); });
        menu.exec(momentList_->viewport()->mapToGlobal(pos));
    });
    connect(state_, &EditorState::projectChanged, this, &PeoplePanel::refresh);
    connect(thumbs_, &ThumbnailCache::ready, this, [this] {
        showPeople();
        showMoments();
    });
    refresh();
}

void PeoplePanel::refresh() {
    const Project& p = state_->project();
    int findable = 0, looked = 0;
    for (const MediaItem& m : p.media)
        if (facesFindable(m)) {
            ++findable;
            looked += m.faces != nullptr;
        }
    people_ = peopleIn(p);
    if (!faceSearchAvailable()) {
        status_->setText(tr("This build of Montage cannot find people (no ONNX Runtime)."));
        findBtn_->setEnabled(false);
    } else if (findable == 0) {
        status_->setText(tr("Import videos or stills to find the people in them."));
        findBtn_->setEnabled(false);
    } else {
        const QString who = people_.size() == 1 ? tr("1 person") : tr("%1 people").arg(people_.size());
        status_->setText(tr("%1 in %2 of %3 videos and stills.").arg(who).arg(looked).arg(findable));
        findBtn_->setEnabled(looked < findable);
    }
    // The chosen person stays chosen while they exist.
    if (std::none_of(people_.begin(), people_.end(), [&](const PersonSummary& s) { return s.id == person_; })) person_ = 0;
    moments_ = person_ ? findPerson(p, person_) : std::vector<PersonMoment>{};
    showPeople();
    showMoments();
}

bool PeoplePanel::findPeople() {
    std::vector<Id> ids;
    for (const MediaItem& m : state_->project().media)
        if (facesFindable(m)) ids.push_back(m.id);
    const bool ok = indexFacesIn(state_, ids, window());
    refresh();
    if (ok && people_.empty()) state_->message(tr("No faces found"), 5000);
    return ok;
}

QImage PeoplePanel::faceImage(const PersonSummary& s) {
    const MediaItem* m = state_->project().findMedia(s.bestMedia);
    if (!m || m->width <= 0 || m->height <= 0) return {};
    // A frame big enough for the face to fill the icon, cropped square around it with some margin.
    const double longSide = 72.0 / std::max(0.05f, std::max(s.w, s.h)) * 0.6;
    const double k = std::min(1.0, std::min(1920.0, longSide) / std::max(m->width, m->height));
    const int w = std::max(16, int(std::lround(m->width * k))), h = std::max(16, int(std::lround(m->height * k)));
    const QImage frame = thumbs_->get(QString::fromStdString(m->path), s.bestTime, w, h);
    if (frame.isNull()) return {};
    const double side = std::max(s.w * w, s.h * h) * 1.5;
    const double cx = (s.x + s.w / 2) * w, cy = (s.y + s.h / 2) * h;
    QImage out(72, 72, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    QPainterPath circle;
    circle.addEllipse(QRectF(0, 0, 72, 72));
    painter.setClipPath(circle);
    painter.drawImage(QRectF(0, 0, 72, 72), frame, QRectF(cx - side / 2, cy - side / 2, side, side));
    return out;
}

void PeoplePanel::showPeople() {
    filling_ = true;
    peopleList_->clear();
    int current = -1;
    for (size_t i = 0; i < people_.size(); ++i) {
        const PersonSummary& s = people_[i];
        auto* item = new QListWidgetItem(QString::fromStdString(s.name), peopleList_);
        item->setFlags(item->flags() | Qt::ItemIsEditable);
        item->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);
        item->setData(Qt::UserRole, s.id);
        const QImage face = faceImage(s);
        if (!face.isNull()) item->setIcon(QIcon(QPixmap::fromImage(face)));
        item->setToolTip(tr("%1: in %n clip(s). Double-click to name.", "", s.media).arg(QString::fromStdString(s.name)));
        if (s.id == person_) current = int(i);
    }
    if (current >= 0) peopleList_->setCurrentRow(current);
    filling_ = false;
}

void PeoplePanel::showMoments() {
    const Sequence* seq = state_->sequence();
    const Rational rate = seq ? seq->fps : Rational{30, 1};
    const int keep = momentList_->currentRow();
    momentList_->clear();
    for (const PersonMoment& pm : moments_) {
        const MediaItem* m = state_->project().findMedia(pm.media);
        if (!m) continue;
        QString where;
        if (m->kind == MediaKind::Image)
            where = tr("Still");
        else
            where = QString::fromStdString(formatTimecode(FrameTime(std::floor(pm.start * rate.toDouble())), rate)) + QStringLiteral(" – ") +
                    QString::fromStdString(formatTimecode(FrameTime(std::ceil(pm.end * rate.toDouble())), rate));
        auto* item = new QListWidgetItem(QStringLiteral("%1\n%2").arg(QString::fromStdString(m->name), where), momentList_);
        // Within 128 x 72 at the media's shape.
        const double aspect = m->width > 0 && m->height > 0 ? double(m->width) / m->height : 16.0 / 9.0;
        const int tw = std::min(128, int(std::lround(72 * aspect))), th = std::min(72, int(std::lround(128 / aspect)));
        const QImage thumb = thumbs_->get(QString::fromStdString(m->path), pm.best, std::max(8, tw), std::max(8, th));
        if (!thumb.isNull()) item->setIcon(QIcon(QPixmap::fromImage(thumb)));
    }
    if (keep >= 0 && keep < momentList_->count()) momentList_->setCurrentRow(keep);
}

int PeoplePanel::selectPerson(int row) {
    person_ = row >= 0 && row < int(people_.size()) ? people_[size_t(row)].id : 0;
    moments_ = person_ ? findPerson(state_->project(), person_) : std::vector<PersonMoment>{};
    if (peopleList_->currentRow() != row) {
        filling_ = true;
        peopleList_->setCurrentRow(row);
        filling_ = false;
    }
    showMoments();
    return int(moments_.size());
}

bool PeoplePanel::rename(int person, const QString& name) {
    const std::string n = name.trimmed().toStdString();
    const bool ok = state_->edit(tr("Name Person"), [person, n](Project& p, Sequence&) { return renamePerson(p, person, n); });
    if (!ok) showPeople();  // an unchanged name: put the label back
    return ok;
}

bool PeoplePanel::merge(int from, int into) {
    const bool ok = state_->edit(tr("Same Person"), [from, into](Project& p, Sequence&) { return mergePeople(p, from, into); });
    if (ok && person_ == from) {
        person_ = into;
        refresh();
    }
    return ok;
}

Id PeoplePanel::makeSmartBin(int person) {
    Id id = 0;
    state_->edit(tr("New Smart Bin"), [&](Project& p, Sequence&) {
        SmartBin b;
        b.id = id = p.newId();
        b.name = personName(p, person);
        b.rules.push_back({"people", "includes", b.name});
        p.smartBins.push_back(std::move(b));
        return true;
    });
    if (id) emit smartBinCreated(id);
    return id;
}

void PeoplePanel::open(int i) {
    if (i < 0 || i >= int(moments_.size())) return;
    const PersonMoment& pm = moments_[size_t(i)];
    const MediaItem* m = state_->project().findMedia(pm.media);
    if (!m) return;
    if (m->kind == MediaKind::Image) {
        emit openRequested(pm.media, -1, -1, 0);
        return;
    }
    const Sequence* s = state_->sequence();
    const double fps = s ? s->fpsValue() : 30.0;
    emit openRequested(pm.media, FrameTime(std::floor(pm.start * fps)), FrameTime(std::ceil(pm.end * fps)), FrameTime(std::lround(pm.best * fps)));
}

Id PeoplePanel::makeSubclip(int i) {
    if (i < 0 || i >= int(moments_.size())) return 0;
    const PersonMoment& pm = moments_[size_t(i)];
    const MediaItem* m = state_->project().findMedia(pm.media);
    if (!m || m->kind == MediaKind::Image) return 0;
    const Sequence* s = state_->sequence();
    const double fps = s ? s->fpsValue() : 30.0;
    const QString name = QString::fromStdString(personName(state_->project(), person_));
    const Id id = state_->makeSubclip(pm.media, FrameTime(std::floor(pm.start * fps)), FrameTime(std::ceil(pm.end * fps)) - 1, name);
    if (id) state_->message(tr("Subclip \"%1\" added to the media bin").arg(QString::fromStdString(state_->project().findMedia(id)->name)), 5000);
    return id;
}

}  // namespace montage
