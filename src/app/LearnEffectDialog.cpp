#include "LearnEffectDialog.h"

#include <QCheckBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <algorithm>
#include <cmath>

#include "EditorState.h"
#include "core/EditOps.h"
#include "media/SuperScale.h"
#include "render/LearnFromClip.h"

namespace montage {

namespace {

QImage toQImage(const Image& img) {
    QImage q(img.width, img.height, QImage::Format_RGBA8888);
    toRgba8(img, q.bits(), size_t(q.bytesPerLine()));
    return q;
}

// What a training run hands back.
struct Job {
    std::shared_ptr<learn::Trainer> trainer;
    std::vector<unsigned char> state;  // to go on from
    std::vector<FrameTime> frames;
};

}  // namespace

LearnEffectDialog::LearnEffectDialog(EditorState* state, Id clip, QWidget* parent)
    : QDialog(parent), state_(state), clip_(clip), cancel_(std::make_shared<std::atomic<bool>>(false)) {
    const Sequence* s = state_->sequence();
    const Clip* c = s ? edit::clipById(*s, clip) : nullptr;
    setWindowTitle(tr("Learn Effect from Examples"));
    auto* v = new QVBoxLayout(this);
    auto* intro = new QLabel(tr("The frames below are the examples: the clip as it is with its effects (Paint, a grade, "
                                "masks, a matte) against the same frames without them. A network learns what was done, "
                                "and does it to the rest of the shot, or to other clips."));
    intro->setWordWrap(true);
    v->addWidget(intro);
    list_ = new QListWidget;
    list_->setObjectName(QStringLiteral("learnFrames"));
    list_->setAccessibleName(tr("Example frames"));
    list_->setMaximumHeight(110);
    v->addWidget(list_);
    auto* frameButtons = new QHBoxLayout;
    auto* add = new QPushButton(tr("Add Playhead Frame"));
    add->setObjectName(QStringLiteral("addLearnFrame"));
    auto* remove = new QPushButton(tr("Remove"));
    remove->setObjectName(QStringLiteral("removeLearnFrame"));
    frameButtons->addWidget(add);
    frameButtons->addWidget(remove);
    frameButtons->addStretch(1);
    v->addLayout(frameButtons);
    auto* stepsRow = new QHBoxLayout;
    auto* stepsLabel = new QLabel(tr("Training steps:"));
    steps_ = new QSpinBox;
    steps_->setObjectName(QStringLiteral("learnSteps"));
    steps_->setRange(50, 200000);
    steps_->setSingleStep(500);
    steps_->setValue(2000);
    stepsLabel->setBuddy(steps_);
    stepsRow->addWidget(stepsLabel);
    stepsRow->addWidget(steps_);
    stepsRow->addStretch(1);
    v->addLayout(stepsRow);
    progress_ = new QProgressBar;
    progress_->setAccessibleName(tr("Training"));
    progress_->setRange(0, 1);
    progress_->setValue(0);
    v->addWidget(progress_);
    preview_ = new QLabel;
    preview_->setObjectName(QStringLiteral("learnPreview"));
    preview_->setAccessibleName(tr("The first example: as it was, and as the network does it"));
    preview_->setMinimumHeight(120);
    preview_->setAlignment(Qt::AlignCenter);
    v->addWidget(preview_);
    info_ = new QLabel;
    info_->setWordWrap(true);
    v->addWidget(info_);
    replace_ = new QCheckBox(tr("Turn off the clip's own effects (the network does their work)"));
    replace_->setObjectName(QStringLiteral("learnReplace"));
    replace_->setChecked(true);
    v->addWidget(replace_);
    selected_ = new QCheckBox(tr("Give it to the selected clips too"));
    selected_->setObjectName(QStringLiteral("learnSelected"));
    v->addWidget(selected_);
    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    train_ = new QPushButton(tr("Train"));
    train_->setObjectName(QStringLiteral("learnTrain"));
    apply_ = new QPushButton(tr("Apply"));
    apply_->setObjectName(QStringLiteral("learnApply"));
    auto* close = new QPushButton(tr("Close"));
    buttons->addWidget(train_);
    buttons->addWidget(apply_);
    buttons->addWidget(close);
    v->addLayout(buttons);

    connect(add, &QPushButton::clicked, this, [this] {
        std::vector<FrameTime> f = frames_;
        f.push_back(state_->playhead());
        setFrames(f);
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        const int row = list_->currentRow();
        if (row < 0 || row >= int(frames_.size())) return;
        std::vector<FrameTime> f = frames_;
        f.erase(f.begin() + row);
        setFrames(f);
    });
    connect(train_, &QPushButton::clicked, this, [this] {
        if (running_) stop();
        else train();
    });
    connect(apply_, &QPushButton::clicked, this, [this] { apply(selected_->isChecked(), replace_->isChecked()); });
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    connect(&watcher_, &QFutureWatcher<QString>::finished, this, [this] { finished(); });
    connect(state_, &EditorState::sequenceSwitched, this, &QDialog::close);

    if (!c) {
        info_->setText(tr("The clip is gone."));
        train_->setEnabled(false);
        apply_->setEnabled(false);
        return;
    }
    // Trained before (the network its Learned Effect runs): its frames, to go on from.
    file_ = learnedNetworkOf(*c);
    if (file_.empty()) file_ = newLearnedNetworkPath(state_->filePath().toStdString(), state_->project(), *c, true);
    learn::Saved saved;
    if (QFileInfo::exists(QString::fromStdString(file_)) && learn::load(file_, saved)) {
        steps_done_ = int(learn::Trainer::stepsIn(saved.state));
        frames_.assign(saved.frames.begin(), saved.frames.end());
        info_->setText(tr("Trained before on these frames: Train goes on from there."));
    } else {
        frames_ = exampleFrames(*s, *c, state_->playhead());
        info_->setText(frames_.empty() ? tr("Put the playhead on a frame you worked on and add it.") : status());
    }
    std::erase_if(frames_, [c](FrameTime f) { return !c->contains(f); });
    showFrames();
    apply_->setEnabled(QFileInfo::exists(QString::fromStdString(file_)));
}

LearnEffectDialog::~LearnEffectDialog() {
    // (Not waited for: a render of an example can take a while. The work holds copies of all it uses and stops at the
    // next step; what it would tell the dialog goes nowhere.)
    cancel_->store(true);
}

void LearnEffectDialog::setFrames(std::vector<FrameTime> frames) {
    const Sequence* s = state_->sequence();
    const Clip* c = s ? edit::clipById(*s, clip_) : nullptr;
    std::sort(frames.begin(), frames.end());
    frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
    if (c) std::erase_if(frames, [c](FrameTime f) { return !c->contains(f); });
    frames_ = std::move(frames);
    showFrames();
    info_->setText(status());
}

void LearnEffectDialog::setSteps(int steps) { steps_->setValue(steps); }

void LearnEffectDialog::showFrames() {
    list_->clear();
    for (FrameTime f : frames_) list_->addItem(timecodeString(state_->sequence(), f));
}

int LearnEffectDialog::stepsTrained() const { return steps_done_; }

QString LearnEffectDialog::status() const {
    if (running_) return tr("Training…");
    if (frames_.empty()) return tr("No example frames: put the playhead on a frame you worked on and add it.");
    if (trainer_)
        return tr("Trained %n step(s) on %1 frame(s); the last error %2.", nullptr, steps_done_)
            .arg(trainedOn_.size())
            .arg(loss_, 0, 'g', 3);
    return tr("%n example frame(s).", nullptr, int(frames_.size()));
}

bool LearnEffectDialog::train(bool wait) {
    if (running_ || frames_.empty() || file_.empty()) return false;
    const Sequence* s = state_->sequence();
    const Clip* c = s ? edit::clipById(*s, clip_) : nullptr;
    if (!c) return false;
    running_ = true;
    cancel_->store(false);
    train_->setText(tr("Stop"));
    apply_->setEnabled(false);
    const int steps = steps_->value();
    progress_->setRange(0, steps);
    progress_->setValue(0);
    info_->setText(status());
    // The examples are made again (the clip's effects may have changed); the network goes on when the frames are the
    // ones it was trained on.
    auto job = std::make_shared<Job>();
    job->frames = frames_;
    if (trainer_ && trainedOn_ == frames_) job->state = trainer_->state();
    const Project project = state_->project();
    const Id seq = s->id;
    const Id clip = clip_;
    const std::string from = file_;
    // (Clips running the network keep it: what is trained now goes to a new file, which Apply gives them.)
    const std::string file = networkInUse(project, file_) ? newLearnedNetworkPath(state_->filePath().toStdString(), project, *c) : file_;
    const std::string name = c->name;
    auto cancel = cancel_;
    const learn::TrainOptions options = options_;
    QPointer<LearnEffectDialog> self(this);
    watcher_.setFuture(QtConcurrent::run([=]() -> QString {
      try {
        const Sequence* sq = project.findSequence(seq);
        const Clip* cc = sq ? edit::clipById(*sq, clip) : nullptr;
        if (!sq || !cc) return tr("The clip is gone.");
        std::vector<learn::Pair> pairs;
        for (FrameTime f : job->frames) {
            if (cancel->load()) return tr("Stopped.");
            learn::Pair p;
            std::string err;
            if (!learningPair(project, *sq, *cc, f, p, &err)) return QString::fromStdString(err);
            fitLearningPair(p, learningPixelsEach(job->frames.size()));
            pairs.push_back(std::move(p));
        }
        job->trainer = std::make_shared<learn::Trainer>(learn::NetShape{}, std::move(pairs), options);
        if (!job->state.empty()) job->trainer->restore(job->state);
        else {
            // Trained before on the same frames: from there.
            learn::Saved saved;
            if (learn::load(from, saved) && std::vector<FrameTime>(saved.frames.begin(), saved.frames.end()) == job->frames && !saved.state.empty())
                job->trainer->restore(saved.state);
        }
        learn::Trainer& t = *job->trainer;
        // The first example, small, for the preview.
        const learn::Pair& first = t.pairs().front();
        const double scale = std::min(1.0, 240.0 / std::max(1, first.before.height));
        const Image small = resizeImage(first.before, std::max(1, int(std::lround(first.before.width * scale))),
                                        std::max(1, int(std::lround(first.before.height * scale))));
        const int start = t.steps();
        t.run(steps, cancel.get(), [&](int step, double loss) {
            const int done = step - start;
            if (done % 100 != 0 && done != steps) return;
            const Image learned = learn::applyNet(t.net(), small);
            QMetaObject::invokeMethod(
                QCoreApplication::instance(),  // (not the dialog, which may be gone by the time it is posted: it is checked there)
                [self, done, loss, small, learned] {
                    if (!self) return;
                    self->progress_->setValue(done);
                    self->info_->setText(tr("Training: step %1, error %2").arg(done).arg(loss, 0, 'g', 3));
                    self->showPreview(small, learned);
                },
                Qt::QueuedConnection);
        });
        learn::Saved saved;
        saved.net = t.net();
        saved.frames.assign(job->frames.begin(), job->frames.end());
        saved.options = t.options();
        saved.state = t.state();
        saved.source = name;
        std::string err;
        if (!learn::save(file, saved, &err)) return QString::fromStdString(err);
        const int trained = t.steps();
        const double loss = t.loss();
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [self, job, file, trained, loss] {
                if (!self) return;
                self->trainer_ = job->trainer;
                self->trainedOn_ = job->frames;
                self->file_ = file;
                self->steps_done_ = trained;
                self->loss_ = loss;
            },
            Qt::QueuedConnection);
        return QString();
      } catch (const std::exception& e) {  // (out of memory, most likely: pictures too large)
        return tr("Training stopped: %1").arg(QString::fromLocal8Bit(e.what()));
      }
    }));
    if (wait) {
        watcher_.waitForFinished();
        QCoreApplication::processEvents();  // (the queued updates)
        finished();
    }
    return true;
}

void LearnEffectDialog::stop() { cancel_->store(true); }

void LearnEffectDialog::finished() {
    if (!running_) return;
    running_ = false;
    const QString why = watcher_.result();
    train_->setText(tr("Train"));
    apply_->setEnabled(QFileInfo::exists(QString::fromStdString(file_)));
    info_->setText(why.isEmpty() ? status() : why);
}

void LearnEffectDialog::showPreview(const Image& before, const Image& learned) {
    const QImage a = toQImage(before), b = toQImage(learned);
    QImage both(a.width() * 2 + 4, a.height(), QImage::Format_RGBA8888);
    both.fill(Qt::transparent);
    QPainter p(&both);
    p.drawImage(0, 0, a);
    p.drawImage(a.width() + 4, 0, b);
    p.end();
    preview_->setPixmap(QPixmap::fromImage(both));
}

bool LearnEffectDialog::apply(bool selected, bool replace) {
    if (running_ || !QFileInfo::exists(QString::fromStdString(file_))) return false;
    std::vector<Id> clips = {clip_};
    if (selected)
        for (Id id : state_->selectedClips())
            if (id != clip_) clips.push_back(id);
    const std::string file = file_;
    const Id examples = clip_;
    const bool done = state_->edit(tr("Learned Effect"), [&](Project& p, Sequence& s) {
        return applyLearnedNetwork(p, s, clips, file, examples, replace) > 0;
    });
    if (done) info_->setText(tr("Given to %n clip(s) as a Learned Effect.", nullptr, int(clips.size())));
    return done;
}

}  // namespace montage
