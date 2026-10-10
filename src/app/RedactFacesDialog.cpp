#include "RedactFacesDialog.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <algorithm>
#include <cmath>

#include "EditorState.h"
#include "core/EditOps.h"
#include "core/FaceIndex.h"
#include "render/Compositor.h"

namespace montage {

RedactFacesDialog::RedactFacesDialog(EditorState* state, Id clip, QWidget* parent)
    : QDialog(parent), state_(state), clip_(clip), cancel_(std::make_shared<std::atomic<bool>>(false)),
      fraction_(std::make_shared<std::atomic<double>>(0)) {
    setWindowTitle(tr("Redact Faces"));
    setObjectName(QStringLiteral("redactFacesDialog"));
    auto* v = new QVBoxLayout(this);
    auto* intro = new QLabel(tr("Finds every face in the clip, follows it from frame to frame and covers it. "
                                "Untick anyone who may be shown. Strength, shape and how long a lost face stays "
                                "covered are in Effect Controls."));
    intro->setWordWrap(true);
    v->addWidget(intro);
    auto* row = new QHBoxLayout;
    find_ = new QPushButton(tr("Find Faces"));
    find_->setObjectName(QStringLiteral("findFaces"));
    progress_ = new QProgressBar;
    progress_->setRange(0, 1000);
    progress_->setVisible(false);
    row->addWidget(find_);
    row->addWidget(progress_, 1);
    v->addLayout(row);
    list_ = new QListWidget;
    list_->setObjectName(QStringLiteral("faceGroups"));
    list_->setViewMode(QListView::IconMode);
    list_->setIconSize(QSize(96, 96));
    list_->setGridSize(QSize(132, 150));
    list_->setResizeMode(QListView::Adjust);
    list_->setMovement(QListView::Static);
    list_->setWordWrap(true);
    list_->setMinimumSize(560, 220);
    v->addWidget(list_, 1);
    auto* look = new QHBoxLayout;
    look->addWidget(new QLabel(tr("Cover with:")));
    style_ = new QComboBox;
    style_->setObjectName(QStringLiteral("redactStyle"));
    style_->addItems({tr("Blur"), tr("Pixelate"), tr("Solid Colour")});
    look->addWidget(style_);
    look->addStretch(1);
    auto* all = new QPushButton(tr("Tick Everyone"));
    auto* none = new QPushButton(tr("Untick Everyone"));
    look->addWidget(all);
    look->addWidget(none);
    v->addLayout(look);
    info_ = new QLabel;
    info_->setWordWrap(true);
    v->addWidget(info_);
    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    apply_ = new QPushButton(tr("Redact"));
    apply_->setObjectName(QStringLiteral("applyRedaction"));
    apply_->setDefault(true);
    auto* close = new QPushButton(tr("Close"));
    buttons->addWidget(apply_);
    buttons->addWidget(close);
    v->addLayout(buttons);

    connect(find_, &QPushButton::clicked, this, [this] {
        if (running_) stop();
        else findFaces();
    });
    connect(all, &QPushButton::clicked, this, [this] {
        for (int i = 0; i < list_->count(); ++i) list_->item(i)->setCheckState(Qt::Checked);
    });
    connect(none, &QPushButton::clicked, this, [this] {
        for (int i = 0; i < list_->count(); ++i) list_->item(i)->setCheckState(Qt::Unchecked);
    });
    connect(apply_, &QPushButton::clicked, this, [this] { apply(); });
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    connect(&watcher_, &QFutureWatcher<bool>::finished, this, [this] { finishAnalysis(); });
    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this] {
        if (running_) progress_->setValue(int(std::lround(fraction_->load() * 1000)));
    });
    timer->start(100);
    connect(state_, &EditorState::sequenceSwitched, this, &QDialog::close);
    loadFromClip();
}

RedactFacesDialog::~RedactFacesDialog() {
    cancel_->store(true);
    watcher_.waitForFinished();
}

void RedactFacesDialog::loadFromClip() {
    const Sequence* s = state_->sequence();
    const Clip* c = s ? edit::clipById(*s, clip_) : nullptr;
    const MediaItem* m = c ? state_->project().findMedia(c->mediaId) : nullptr;
    if (!m || (m->kind != MediaKind::Video && m->kind != MediaKind::Image)) {
        info_->setText(tr("This clip has no picture to look for faces in."));
        find_->setEnabled(false);
        apply_->setEnabled(false);
        return;
    }
    media_ = *m;
    path_ = m->path;
    clipMediaSpan(*s, *c, m->kind == MediaKind::Image, start_, end_);
    for (const Effect& e : c->effects) {
        if (e.type != "redact_faces") continue;
        style_->setCurrentIndex(std::clamp(int(std::lround(e.p("style", 0))), 0, 2));
        auto t = std::make_shared<FaceTracks>();
        if (!e.s("media").empty() && e.s("media") != std::to_string(m->id)) {
            info_->setText(tr("The faces on this clip were found in other footage: find them again here."));
            return;
        }
        if (faceTracksFromString(e.s("tracks"), *t)) {
            tracks_ = t;
            groups_ = groupFaceTracks(*tracks_);
            matchProjectPeople(state_->project(), groups_);
            showGroups();
            const std::set<int> keep = trackIdsFromString(e.s("keep"));
            for (size_t g = 0; g < groups_.size(); ++g) {
                const bool shown = std::all_of(groups_[g].tracks.begin(), groups_[g].tracks.end(), [&](int id) { return keep.count(id) > 0; });
                list_->item(int(g))->setCheckState(shown ? Qt::Unchecked : Qt::Checked);
            }
            const bool covers = t->step <= 0 || (start_ >= t->start - 1 / t->fps && end_ <= t->end + 1 / t->fps);
            info_->setText(covers ? status() : tr("The clip now shows more than was analysed: find the faces again to follow them there."));
        }
        break;
    }
    if (!tracks_) info_->setText(tr("Find the faces in the clip to choose who is covered."));
}

bool RedactFacesDialog::findFaces(bool wait) {
    if (running_ || path_.empty()) return false;
    if (!faceSearchAvailable()) {
        info_->setText(tr("This build cannot find faces (it was made without ONNX Runtime)."));
        return false;
    }
    if (!faceModel().installed()) {
        info_->setText(tr("The face models are not installed yet: download them in Settings > Models (People search)."));
        return false;
    }
    cancel_ = std::make_shared<std::atomic<bool>>(false);
    fraction_->store(0);
    pending_ = std::make_shared<FaceTracks>();
    error_ = std::make_shared<std::string>();
    running_ = true;
    find_->setText(tr("Stop"));
    apply_->setEnabled(false);
    progress_->setValue(0);
    progress_->setVisible(true);
    info_->setText(tr("Finding and following the faces…"));
    const std::string path = path_;
    const double start = start_, end = end_;
    auto cancel = cancel_;
    auto fraction = fraction_;
    auto out = pending_;
    auto error = error_;
    watcher_.setFuture(QtConcurrent::run([path, start, end, cancel, fraction, out, error] {
        return trackFaces(path, start, end, *out, [&](double f) {
            fraction->store(f);
            return !cancel->load();
        }, error.get());
    }));
    if (wait) {
        watcher_.waitForFinished();
        finishAnalysis();
    }
    return true;
}

void RedactFacesDialog::stop() {
    cancel_->store(true);
}

void RedactFacesDialog::finishAnalysis() {
    if (!running_) return;  // (already handled when waited for)
    running_ = false;
    find_->setText(tr("Find Faces"));
    apply_->setEnabled(true);
    progress_->setVisible(false);
    const bool ok = watcher_.future().result();
    if (!ok) {
        info_->setText(cancel_->load() ? tr("Stopped.") : tr("Could not look for faces: %1").arg(QString::fromStdString(*error_)));
        return;
    }
    tracks_ = pending_;
    groups_ = groupFaceTracks(*tracks_);
    matchProjectPeople(state_->project(), groups_);
    showGroups();
    info_->setText(status());
}

void RedactFacesDialog::showGroups() {
    list_->clear();
    const double step = tracks_ && tracks_->step > 0 ? tracks_->step : 0;
    for (size_t g = 0; g < groups_.size(); ++g) {
        const FaceGroup& G = groups_[g];
        QString name = G.person ? QString::fromStdString(personName(state_->project(), G.person)) : tr("Face %1").arg(g + 1);
        QString text = step > 0 ? tr("%1\n%2 s").arg(name).arg(G.seconds, 0, 'f', 1) : name;
        auto* item = new QListWidgetItem(text, list_);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Checked);
        item->setToolTip(tr("%n track(s)", "", int(G.tracks.size())));
        // Their picture: the largest sighting, with some room round it.
        const double frameW = std::max(1, media_.width), frameH = std::max(1, media_.height);
        const double scale = std::min(1.0, 640.0 / std::max(frameW, frameH));
        const int w = std::max(1, int(frameW * scale)), h = std::max(1, int(frameH * scale));
        const Image frame = renderMediaFrame(state_->project(), media_, G.bestTime, w, h);
        if (!frame.empty()) {
            QImage img(frame.width, frame.height, QImage::Format_RGBA8888);
            toRgba8(frame, img.bits(), size_t(img.bytesPerLine()));
            const double side = std::max(G.best.w * frame.width, G.best.h * frame.height) * 1.6;
            const double cx = (G.best.x + G.best.w / 2) * frame.width, cy = (G.best.y + G.best.h / 2) * frame.height;
            const QRect r = QRect(int(cx - side / 2), int(cy - side / 2), int(side), int(side)).intersected(img.rect());
            item->setIcon(QIcon(QPixmap::fromImage(img.copy(r).scaled(96, 96, Qt::KeepAspectRatio, Qt::SmoothTransformation))));
        }
    }
}

bool RedactFacesDialog::redacted(int group) const {
    return group >= 0 && group < list_->count() && list_->item(group)->checkState() == Qt::Checked;
}

void RedactFacesDialog::setRedacted(int group, bool on) {
    if (group >= 0 && group < list_->count()) list_->item(group)->setCheckState(on ? Qt::Checked : Qt::Unchecked);
}

void RedactFacesDialog::setStyle(int style) { style_->setCurrentIndex(std::clamp(style, 0, 2)); }

QString RedactFacesDialog::status() const {
    if (!tracks_) return tr("Find the faces in the clip to choose who is covered.");
    if (groups_.empty()) return tr("No faces were found in the clip; any found while it plays are still covered.");
    int covered = 0;
    for (int g = 0; g < int(groups_.size()); ++g) covered += redacted(g) ? 1 : 0;
    return tr("%n face(s) found;", "", int(groups_.size())) + QLatin1Char(' ') + tr("%1 covered.").arg(covered);
}

bool RedactFacesDialog::apply() {
    std::set<int> keep;
    for (int g = 0; g < int(groups_.size()); ++g)
        if (!redacted(g)) keep.insert(groups_[size_t(g)].tracks.begin(), groups_[size_t(g)].tracks.end());
    const std::string tracks = tracks_ ? faceTracksToString(*tracks_) : std::string();
    const std::string keepText = trackIdsToString(keep);
    const int style = style_->currentIndex();
    const Id id = clip_;
    const bool done = state_->edit(tr("Redact Faces"), [&](Project& p, Sequence& s) {
        Clip* c = edit::clipById(s, id);
        if (!c) return false;
        const bool created = !redactFacesEffectOf(p, *c, false);
        Effect* e = redactFacesEffectOf(p, *c, true);
        const Effect before = *e;
        e->enabled = true;
        e->strings["tracks"] = tracks;
        e->strings["keep"] = keepText;
        e->strings["media"] = std::to_string(c->mediaId);
        if (tracks.empty()) e->strings.erase("tracks"), e->strings.erase("media");
        if (keepText.empty()) e->strings.erase("keep");
        e->params["style"].set(0, style);
        return created || !(before == *e);
    });
    if (done) info_->setText(status() + QLatin1Char(' ') + tr("Applied to the clip."));
    return done;
}

}  // namespace montage
