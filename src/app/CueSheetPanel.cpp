#include "CueSheetPanel.h"

#include <QApplication>
#include <QComboBox>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <set>

#include "EditorState.h"
#include "core/History.h"
#include "media/MusicTags.h"
#include "media/StemSeparation.h"

namespace montage {

namespace {

// The table's columns: the cue number, the fields (kCueFields' order), the use, then the timing.
constexpr int kFirstField = 1;
constexpr int kFieldCount = int(std::size(kCueFields));
constexpr int kUse = kFirstField + kFieldCount, kVocal = kUse + 1, kTheme = kUse + 2, kIn = kUse + 3, kOut = kUse + 4,
              kDuration = kUse + 5, kColumns = kUse + 6;

struct Choice {
    const char* value;
    const char* label;
};
const std::vector<Choice> kUses{{"background", QT_TRANSLATE_NOOP("CueSheetPanel", "Background")},
                                {"visual", QT_TRANSLATE_NOOP("CueSheetPanel", "Visual")}};
const std::vector<Choice> kVocals{{"", ""}, {"vocal", QT_TRANSLATE_NOOP("CueSheetPanel", "Vocal")},
                                  {"instrumental", QT_TRANSLATE_NOOP("CueSheetPanel", "Instrumental")}};
const std::vector<Choice> kThemes{{"", ""}, {"main title", QT_TRANSLATE_NOOP("CueSheetPanel", "Main title")},
                                  {"end title", QT_TRANSLATE_NOOP("CueSheetPanel", "End title")}};

}  // namespace

CueSheetPanel::CueSheetPanel(EditorState* state, QWidget* parent) : QWidget(parent), state_(state) {
    setObjectName(QStringLiteral("cueSheetPanel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    auto* top = new QHBoxLayout;
    summary_ = new QLabel(this);
    summary_->setObjectName(QStringLiteral("cueSheetSummary"));
    top->addWidget(summary_, 1);
    suggest_ = new QPushButton(tr("Suggest Vocal"), this);
    suggest_->setObjectName(QStringLiteral("cueSheetSuggestVocal"));
    suggest_->setToolTip(tr("Mark each cue not yet marked as vocal or instrumental, from how much of it the vocal stem holds"));
    top->addWidget(suggest_);
    export_ = new QPushButton(tr("Export…"), this);
    export_->setObjectName(QStringLiteral("cueSheetExport"));
    top->addWidget(export_);
    layout->addLayout(top);
    table_ = new QTableWidget(0, kColumns, this);
    table_->setObjectName(QStringLiteral("cueSheetTable"));
    QStringList heads{tr("Cue"), tr("Title"), tr("Composer"), tr("Publisher"), tr("Society"), tr("Library"), tr("Catalogue"),
                      tr("ISRC"), tr("ISWC"), tr("Use"), tr("Vocal"), tr("Theme"), tr("In"), tr("Out"), tr("Duration")};
    table_->setHorizontalHeaderLabels(heads);
    table_->verticalHeader()->hide();
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table_->setColumnWidth(0, 40);
    layout->addWidget(table_, 1);
    connect(table_, &QTableWidget::cellChanged, this, &CueSheetPanel::edited);
    connect(suggest_, &QPushButton::clicked, this, [this] { suggestVocal(); });
    connect(export_, &QPushButton::clicked, this, &CueSheetPanel::exportRequested);
    connect(state_, &EditorState::projectChanged, this, [this] {
        if (isVisible())
            refresh();
        else
            stale_ = true;
    });
}

void CueSheetPanel::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    if (stale_) refresh();
}

std::map<std::string, std::string> CueSheetPanel::tagsOf(const MediaItem& m) {
    auto it = tags_.find(m.path);
    if (it == tags_.end()) it = tags_.emplace(m.path, musicTagsFor(m)).first;
    return it->second;
}

void CueSheetPanel::refresh() {
    stale_ = false;
    const Sequence* s = state_->sequence();
    cues_ = s ? musicCues(state_->project(), *s, [this](const MediaItem& m) { return tagsOf(m); }) : std::vector<MusicCue>{};
    refreshing_ = true;
    table_->setRowCount(int(cues_.size()));
    auto combo = [&](int row, int column, const std::vector<Choice>& choices, const std::string& value) {
        auto* box = qobject_cast<QComboBox*>(table_->cellWidget(row, column));
        if (!box) {
            box = new QComboBox(table_);
            for (const Choice& c : choices) box->addItem(tr(c.label), QString::fromLatin1(c.value));
            table_->setCellWidget(row, column, box);
            connect(box, &QComboBox::activated, this, [this, box, column] {
                for (int r = 0; r < table_->rowCount(); ++r)
                    if (table_->cellWidget(r, column) == box && size_t(r) < cues_.size()) {
                        const char* key = column == kUse ? kCueUse : column == kVocal ? kCueVocal : kCueTheme;
                        setField(cues_[size_t(r)].mediaId, key, box->currentData().toString().toStdString());
                    }
            });
        }
        const QSignalBlocker block(box);
        box->setCurrentIndex(std::max(0, box->findData(QString::fromStdString(value))));
    };
    auto text = [&](int row, int column, const QString& t, bool editable) {
        QTableWidgetItem* item = table_->item(row, column);
        if (!item) {
            item = new QTableWidgetItem;
            table_->setItem(row, column, item);
        }
        item->setText(t);
        item->setFlags(editable ? item->flags() | Qt::ItemIsEditable : item->flags() & ~Qt::ItemIsEditable);
        return item;
    };
    FrameTime total = 0;
    for (size_t i = 0; i < cues_.size(); ++i) {
        const MusicCue& c = cues_[i];
        const int r = int(i);
        text(r, 0, QString::number(c.number), false)->setToolTip(QString::fromStdString(c.file));
        for (int f = 0; f < kFieldCount; ++f) text(r, kFirstField + f, QString::fromStdString(c.fields.at(kCueFields[f])), true);
        combo(r, kUse, kUses, c.use);
        combo(r, kVocal, kVocals, c.vocal);
        combo(r, kTheme, kThemes, c.theme);
        text(r, kIn, QString::fromStdString(formatTimecode(c.in, s->fps)), false);
        text(r, kOut, QString::fromStdString(formatTimecode(c.out, s->fps)), false);
        text(r, kDuration, QString::fromStdString(formatTimecode(c.duration(), s->fps)), false);
        total += c.duration();
    }
    refreshing_ = false;
    summary_->setText(cues_.empty() ? tr("No music in the cut: give music clips the Music role, or tag their files")
                                    : tr("%n cue(s), %1 of music", nullptr, int(cues_.size()))
                                          .arg(QString::fromStdString(formatTimecode(total, s->fps))));
    suggest_->setEnabled(!cues_.empty());
    export_->setEnabled(s != nullptr);
}

void CueSheetPanel::setField(Id media, const std::string& key, const std::string& value) {
    state_->edit(tr("Cue Sheet"), [&](Project& p, Sequence&) {
        MediaItem* m = p.findMedia(media);
        if (!m) return false;
        auto it = m->metadata.find(key);
        if (value.empty() ? it == m->metadata.end() : it != m->metadata.end() && it->second == value) return false;
        if (value.empty())
            m->metadata.erase(it);
        else
            m->metadata[key] = value;
        return true;
    });
}

void CueSheetPanel::edited(int row, int column) {
    if (refreshing_ || row < 0 || size_t(row) >= cues_.size() || column < kFirstField || column >= kFirstField + kFieldCount) return;
    const MusicCue& c = cues_[size_t(row)];
    const std::string field = kCueFields[column - kFirstField];
    const std::string value = table_->item(row, column)->text().trimmed().toStdString();
    // What the file's tag says needs no word of the media's own; anything else is kept.
    std::string tag;
    if (const MediaItem* m = state_->project().findMedia(c.mediaId)) {
        const auto tags = tagsOf(*m);
        if (auto it = tags.find(field); it != tags.end()) tag = it->second;
    }
    setField(c.mediaId, cueMetadataKey(field), value == tag ? std::string() : value);
}

bool CueSheetPanel::suggestVocal() {
    const Sequence* s = state_->sequence();
    if (!s || cues_.empty()) return false;
    if (!stemSeparationAvailable() || !(stemModel(2).installed() || stemModel(4).installed() || stemModel(5).installed())) {
        state_->message(tr("Suggest Vocal needs the music stem model (Separate Stems downloads it)"), 6000);
        return false;
    }
    std::vector<MusicCue> open;
    for (const MusicCue& c : cues_)
        if (c.vocal.empty()) open.push_back(c);
    if (open.empty()) {
        state_->message(tr("Every cue is marked vocal or instrumental"), 4000);
        return true;
    }
    const Project p = state_->project();
    const Sequence seq = *s;
    using Found = std::vector<std::pair<Id, bool>>;
    QApplication::setOverrideCursor(Qt::BusyCursor);
    suggest_->setEnabled(false);
    auto* watcher = new QFutureWatcher<Found>(this);
    connect(watcher, &QFutureWatcher<Found>::finished, this, [this, watcher] {
        QApplication::restoreOverrideCursor();
        const Found found = watcher->result();
        watcher->deleteLater();
        suggest_->setEnabled(true);
        state_->edit(tr("Suggest Vocal"), [&](Project& p, Sequence&) {
            bool any = false;
            for (const auto& [media, vocal] : found)
                if (MediaItem* m = p.findMedia(media); m && !m->metadata.count(kCueVocal)) {
                    m->metadata[kCueVocal] = vocal ? "vocal" : "instrumental";
                    any = true;
                }
            return any;
        });
        state_->message(tr("Vocal or instrumental suggested for %n cue(s)", nullptr, int(found.size())), 5000);
    });
    watcher->setFuture(QtConcurrent::run([p, seq, open] {
        Found found;
        std::set<Id> done;
        for (const MusicCue& c : open) {
            bool vocal = false;
            if (done.insert(c.mediaId).second && cueIsVocal(p, seq, c, vocal)) found.push_back({c.mediaId, vocal});
        }
        return found;
    }));
    return true;
}

}  // namespace montage
