#include "ShotSearchPanel.h"

#include <QComboBox>
#include <QEventLoop>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QProgressDialog>
#include <QPushButton>
#include <QTimer>
#include <algorithm>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <atomic>
#include <memory>

#include "EditorState.h"
#include "ModelPacks.h"
#include "ThumbnailCache.h"
#include "core/History.h"
#include "media/SpeechSearch.h"
#include "media/VisualSearch.h"

namespace montage {

namespace {
bool searchable(const MediaItem& m) { return m.kind == MediaKind::Video && m.hasVideo && !m.path.empty() && !m.subclipOf; }
}  // namespace

ShotSearchPanel::ShotSearchPanel(EditorState* state, QWidget* parent)
    : QWidget(parent), state_(state), thumbs_(&ThumbnailCache::instance()) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(4, 4, 4, 4);
    auto* row = new QHBoxLayout;
    mode_ = new QComboBox(this);
    mode_->setObjectName(QStringLiteral("findMode"));
    mode_->addItem(tr("What's Shown"));
    mode_->addItem(tr("What's Said"));
    mode_->setToolTip(tr("Search the footage by what it shows, or the transcripts by what is said (by meaning, not only the words)"));
    row->addWidget(mode_);
    query_ = new QLineEdit(this);
    query_->setObjectName(QStringLiteral("shotQuery"));
    query_->setPlaceholderText(tr("Describe a shot: \"a dog running on a beach\""));
    query_->setClearButtonEnabled(true);
    searchBtn_ = new QPushButton(tr("Find"), this);
    row->addWidget(query_, 1);
    row->addWidget(searchBtn_);
    lay->addLayout(row);
    auto* statusRow = new QHBoxLayout;
    status_ = new QLabel(this);
    status_->setWordWrap(true);
    status_->setObjectName(QStringLiteral("shotStatus"));
    indexBtn_ = new QPushButton(tr("Index"), this);
    indexBtn_->setObjectName(QStringLiteral("indexShots"));
    indexBtn_->setToolTip(tr("Look through the videos that are not indexed yet, so they can be searched"));
    statusRow->addWidget(status_, 1);
    statusRow->addWidget(indexBtn_);
    lay->addLayout(statusRow);
    list_ = new QListWidget(this);
    list_->setObjectName(QStringLiteral("shotResults"));
    list_->setIconSize(QSize(128, 72));
    list_->setUniformItemSizes(true);
    lay->addWidget(list_, 1);
    connect(mode_, &QComboBox::currentIndexChanged, this, [this](int i) { setMode(Mode(i)); });
    connect(query_, &QLineEdit::returnPressed, this, [this] { search(query_->text()); });
    connect(searchBtn_, &QPushButton::clicked, this, [this] { search(query_->text()); });
    connect(indexBtn_, &QPushButton::clicked, this, [this] { QTimer::singleShot(0, this, [this] { indexMissing(); }); });
    connect(list_, &QListWidget::itemActivated, this, [this](QListWidgetItem* it) { open(list_->row(it)); });
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(list_, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        const int row = list_->row(list_->itemAt(pos));
        if (row < 0) return;
        QMenu menu(this);
        menu.addAction(tr("Open in Source Monitor"), this, [this, row] { open(row); });
        menu.addAction(tr("Make Subclip"), this, [this, row] { makeSubclip(row); })->setObjectName(QStringLiteral("shotSubclip"));
        menu.exec(list_->viewport()->mapToGlobal(pos));
    });
    connect(state_, &EditorState::projectChanged, this, &ShotSearchPanel::refreshStatus);
    connect(thumbs_, &ThumbnailCache::ready, this, &ShotSearchPanel::showResults);
    refreshStatus();
}

ShotSearchPanel::Mode ShotSearchPanel::mode() const { return Mode(mode_->currentIndex()); }

void ShotSearchPanel::setMode(Mode m) {
    if (mode_->currentIndex() != int(m)) {
        QSignalBlocker block(mode_);
        mode_->setCurrentIndex(int(m));
    }
    query_->setPlaceholderText(m == Said ? tr("What is talked about: \"where they discuss the budget\"")
                                         : tr("Describe a shot: \"a dog running on a beach\""));
    results_.clear();
    texts_.clear();
    showResults();
    refreshStatus();
}

QString ShotSearchPanel::resultText(int i) const {
    return i >= 0 && size_t(i) < texts_.size() ? QString::fromStdString(texts_[size_t(i)]) : QString();
}

int ShotSearchPanel::searchSaid(const QString& queryText) {
    const QString query = queryText.trimmed();
    results_.clear();
    texts_.clear();
    if (query.isEmpty() || !speechSearchAvailable()) {
        showResults();
        return 0;
    }
    if (!ensureModelPack(window(), sentenceModel(), tr("Find What's Said"),
                         tr("Searching what is said by meaning uses multi-qa-MiniLM (sentence-transformers, Apache-2.0), which runs on this computer.")))
        return 0;
    // The first search reads every transcript (later ones are immediate): in the background, with progress.
    QProgressDialog progress(tr("Reading the transcripts…"), tr("Cancel"), 0, 1000, window());
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(500);
    auto done = std::make_shared<std::atomic<double>>(0.0);
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    connect(&progress, &QProgressDialog::canceled, &progress, [cancel] { *cancel = true; });
    QTimer tick;
    connect(&tick, &QTimer::timeout, &progress, [&progress, done] { progress.setValue(int(*done * 1000)); });
    tick.start(100);
    using Out = std::pair<std::vector<SpokenHit>, std::string>;
    QFutureWatcher<Out> watcher;
    QEventLoop wait;
    connect(&watcher, &QFutureWatcher<Out>::finished, &wait, &QEventLoop::quit);
    const Project project = state_->project();
    const std::string q = query.toStdString();
    watcher.setFuture(QtConcurrent::run([project, q, done, cancel] {
        Out out;
        SpokenSearchOptions o;
        o.max = 30;
        out.first = searchSpoken(project, q, o, &out.second, [done](double f) { *done = f; }, cancel.get());
        return out;
    }));
    if (!watcher.isFinished()) wait.exec();
    tick.stop();
    disconnect(&progress, &QProgressDialog::canceled, nullptr, nullptr);
    progress.close();
    const Out r = watcher.result();
    if (r.first.empty() && !r.second.empty() && !*cancel) state_->message(QString::fromStdString(r.second), 6000);
    for (const SpokenHit& h : r.first) {
        ShotMatch m;
        m.media = h.media;
        m.start = h.start, m.end = h.end, m.best = h.start;
        m.score = h.score;
        results_.push_back(m);
        texts_.push_back(h.text);
    }
    lastQuery_ = query;
    showResults();
    return int(results_.size());
}

void ShotSearchPanel::refreshStatus() {
    if (mode() == Said) {
        int transcribed = 0, sounds = 0;
        for (const MediaItem& m : state_->project().media)
            if (!m.subclipOf && (m.hasAudio || m.transcript)) {
                ++sounds;
                transcribed += bool(m.transcript);
            }
        indexBtn_->setVisible(false);
        searchBtn_->setEnabled(speechSearchAvailable());
        status_->setText(!speechSearchAvailable() ? tr("This build of Montage cannot search speech by meaning (no ONNX Runtime).")
                         : sounds == 0            ? tr("Import and transcribe recordings to search what is said.")
                                                  : tr("%1 of %n recording(s) transcribed.", "", sounds).arg(transcribed));
        return;
    }
    indexBtn_->setVisible(true);
    int videos = 0, indexed = 0;
    for (const MediaItem& m : state_->project().media)
        if (searchable(m)) {
            ++videos;
            indexed += m.visual && !m.visual->samples.empty();
        }
    if (!visualSearchAvailable()) {
        status_->setText(tr("This build of Montage cannot search footage by description (no ONNX Runtime)."));
        indexBtn_->setEnabled(false);
        searchBtn_->setEnabled(false);
        return;
    }
    status_->setText(videos == 0 ? tr("Import videos to search them.")
                                 : tr("%1 of %n video(s) indexed.", "", videos).arg(indexed));
    indexBtn_->setEnabled(indexed < videos);
}

bool ShotSearchPanel::indexMissing() {
    std::vector<Id> ids;
    for (const MediaItem& m : state_->project().media)
        if (searchable(m)) ids.push_back(m.id);
    const bool ok = indexVideos(state_, ids, window());
    refreshStatus();
    return ok;
}

bool indexVideos(EditorState* state, const std::vector<Id>& media, QWidget* parent) {
    if (!visualSearchAvailable()) return false;
    struct Job {
        Id id;
        std::string path;
        double duration;
    };
    std::vector<Job> jobs;
    for (Id id : media) {
        const MediaItem* m = state->project().findMedia(id);
        if (m && m->subclipOf) m = state->project().findMedia(m->subclipOf);  // a subclip's media holds the index
        if (!m || !searchable(*m) || (m->visual && !m->visual->samples.empty())) continue;
        if (std::none_of(jobs.begin(), jobs.end(), [&](const Job& j) { return j.id == m->id; })) jobs.push_back({m->id, m->path, m->duration});
    }
    if (jobs.empty()) return true;
    if (!ensureModelPack(parent, visualModel(), QObject::tr("Find Shots"),
                         QObject::tr("Searching and tagging footage by what it shows uses CLIP (OpenAI, MIT licence), which runs on this computer.")))
        return false;
    QProgressDialog progress(QObject::tr("Indexing videos…"), QObject::tr("Cancel"), 0, 1000, parent);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(300);
    auto done = std::make_shared<std::atomic<double>>(0.0);
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    QObject::connect(&progress, &QProgressDialog::canceled, &progress, [cancel] { *cancel = true; });
    QTimer tick;
    QObject::connect(&tick, &QTimer::timeout, &progress, [&progress, done] { progress.setValue(int(*done * 1000)); });
    tick.start(100);
    using Out = std::pair<std::vector<std::pair<Id, std::shared_ptr<const VisualIndex>>>, std::string>;
    QFutureWatcher<Out> watcher;
    QEventLoop wait;
    QObject::connect(&watcher, &QFutureWatcher<Out>::finished, &wait, &QEventLoop::quit);
    watcher.setFuture(QtConcurrent::run([jobs, done, cancel] {
        Out out;
        for (size_t i = 0; i < jobs.size() && !*cancel; ++i) {
            VisualIndex v;
            std::string err;
            const double n = double(jobs.size());
            if (indexVideo(jobs[i].path, jobs[i].duration, v, 0, [&](double f) { *done = (double(i) + f) / n; }, cancel.get(), &err))
                out.first.emplace_back(jobs[i].id, std::make_shared<const VisualIndex>(std::move(v)));
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
            for (const auto& [id, v] : indexes)
                if (MediaItem* m = p.findMedia(id)) m->visual = v;
            return true;
        });
    }
    if (!r.second.empty()) state->message(QString::fromStdString(r.second), 6000);
    return !*cancel && r.second.empty();
}

int ShotSearchPanel::search(const QString& queryText) {
    if (mode() == Said) return searchSaid(queryText);
    const QString query = queryText.trimmed();
    results_.clear();
    texts_.clear();
    if (query.isEmpty() || !visualSearchAvailable()) {
        showResults();
        return 0;
    }
    if (!indexMissing() && !visualModel().installed()) return 0;
    std::string err;
    auto model = ClipModel::load(&err);
    const std::vector<float> q = model ? model->text(query.toStdString(), &err) : std::vector<float>{};
    if (q.empty()) {
        state_->message(QString::fromStdString(err), 6000);
        return 0;
    }
    results_ = findShots(state_->project(), q, 30);
    lastQuery_ = query;
    showResults();
    return int(results_.size());
}

int ShotSearchPanel::searchSimilar(Id media, double seconds) {
    results_.clear();
    const MediaItem* m = state_->project().findMedia(media);
    if (!m || !visualSearchAvailable()) {
        showResults();
        return 0;
    }
    if (!indexMissing() && !visualModel().installed()) return 0;
    std::string err;
    std::vector<float> q;
    if (!embedFrame(m->path, seconds, q, &err)) {
        state_->message(QString::fromStdString(err), 6000);
        return 0;
    }
    texts_.clear();
    results_ = findSimilarShots(state_->project(), q, media, seconds, 30);
    lastQuery_ = tr("Like %1").arg(QString::fromStdString(m->name));
    query_->setText(QString());
    query_->setPlaceholderText(lastQuery_);
    showResults();
    return int(results_.size());
}

void ShotSearchPanel::showResults() {
    const Sequence* s = state_->sequence();
    const Rational rate = s ? s->fps : Rational{30, 1};
    const int keep = list_->currentRow();
    list_->clear();
    for (const ShotMatch& r : results_) {
        const MediaItem* m = state_->project().findMedia(r.media);
        if (!m) continue;
        const QString range = QString::fromStdString(formatTimecode(FrameTime(std::floor(r.start * rate.toDouble())), rate)) +
                              QStringLiteral(" – ") +
                              QString::fromStdString(formatTimecode(FrameTime(std::ceil(r.end * rate.toDouble())), rate));
        const size_t i = size_t(&r - results_.data());
        const QString said = i < texts_.size() ? QString::fromStdString(texts_[i]) : QString();
        QString label = QStringLiteral("%1\n%2").arg(QString::fromStdString(m->name), range);
        if (!said.isEmpty()) label += QStringLiteral("\n\u201c%1\u201d").arg(said.length() > 90 ? said.left(88) + QStringLiteral("\u2026") : said);
        auto* item = new QListWidgetItem(label, list_);
        if (m->hasVideo) {
            const QImage thumb = thumbs_->get(QString::fromStdString(m->path), r.best, 128, 72);
            if (!thumb.isNull()) item->setIcon(QIcon(QPixmap::fromImage(thumb)));
        }
        item->setToolTip(said.isEmpty() ? tr("Match %1").arg(double(r.score), 0, 'f', 3) : said);
    }
    if (keep >= 0 && keep < list_->count()) list_->setCurrentRow(keep);
}

Id ShotSearchPanel::makeSubclip(int i) {
    if (i < 0 || i >= int(results_.size())) return 0;
    const ShotMatch& r = results_[size_t(i)];
    const Sequence* s = state_->sequence();
    const double fps = s ? s->fpsValue() : 30.0;
    // Named after the search that found it.
    const QString name = lastQuery_.isEmpty() ? QString() : lastQuery_.left(1).toUpper() + lastQuery_.mid(1);
    const Id id = state_->makeSubclip(r.media, FrameTime(std::floor(r.start * fps)), FrameTime(std::ceil(r.end * fps)) - 1, name);
    if (id) state_->message(tr("Subclip \"%1\" added to the media bin").arg(QString::fromStdString(state_->project().findMedia(id)->name)), 5000);
    return id;
}

void ShotSearchPanel::open(int i) {
    if (i < 0 || i >= int(results_.size())) return;
    const ShotMatch& r = results_[size_t(i)];
    const Sequence* s = state_->sequence();
    const double fps = s ? s->fpsValue() : 30.0;
    emit openRequested(r.media, FrameTime(std::floor(r.start * fps)), FrameTime(std::ceil(r.end * fps)), FrameTime(std::lround(r.best * fps)));
}

}  // namespace montage
