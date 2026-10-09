#include "CaptionsPanel.h"
#include "Settings.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEventLoop>
#include <QFutureWatcher>
#include <QProgressDialog>
#include <QtConcurrent>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <functional>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>

#include "EditorState.h"
#include "core/ProjectIO.h"
#include "ModelPacks.h"
#include "SpeechDialog.h"
#include "core/EditOps.h"
#include "media/AutoDuck.h"
#include "media/TextToSpeech.h"
#include "media/Translator.h"
#include "TranscribeDialog.h"
#include "core/History.h"

namespace montage {

namespace {

constexpr int kIndexRole = Qt::UserRole;

QToolButton* button(QWidget* parent, const QString& text, const QString& tip) {
    auto* b = new QToolButton(parent);
    b->setText(text);
    b->setToolTip(tip);
    b->setAutoRaise(true);
    return b;
}

}  // namespace

CaptionsPanel::CaptionsPanel(EditorState* state, QWidget* parent) : QWidget(parent), state_(state) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(4, 4, 4, 4);
    auto* top = new QHBoxLayout;
    tracks_ = new QComboBox(this);
    tracks_->setToolTip(tr("Caption track"));
    tracks_->setMinimumContentsLength(10);
    top->addWidget(tracks_, 1);
    visible_ = new QCheckBox(tr("Show"), this);
    visible_->setToolTip(tr("Show this track in the viewer and use it for burn-in and embedded captions"));
    top->addWidget(visible_);
    auto* generate = button(this, tr("Generate"), tr("Make captions from the transcripts of the clips in this sequence"));
    auto* more = button(this, tr("More"), tr("Import, export, style and track options"));
    more->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new QMenu(more);
    menu->addAction(tr("New Caption Track"), this, &CaptionsPanel::addTrack);
    menu->addAction(tr("Import Captions..."), this, &CaptionsPanel::importDialog);
    menu->addAction(tr("Export Captions..."), this, &CaptionsPanel::exportDialog);
    menu->addAction(tr("Translate Track..."), this, &CaptionsPanel::translateDialog)->setObjectName(QStringLiteral("translateCaptions"));
    menu->addAction(tr("Dub into English..."), this, &CaptionsPanel::dubDialog)->setObjectName(QStringLiteral("dubCaptions"));
    menu->addSeparator();
    menu->addAction(tr("Style..."), this, &CaptionsPanel::styleDialog);
    menu->addSeparator();
    menu->addAction(tr("Fix Timing"), this, [this] {
        const int n = fixTiming();
        state_->message(n ? tr("Retimed %1 captions").arg(n) : tr("No caption needed retiming"), 3000);
    })->setObjectName(QStringLiteral("fixCaptionTiming"));
    menu->addAction(tr("Shift Captions..."), this, &CaptionsPanel::shiftDialog)->setObjectName(QStringLiteral("shiftCaptions"));
    menu->addAction(tr("Sync to Two Points..."), this, &CaptionsPanel::syncDialog)->setObjectName(QStringLiteral("syncCaptions"));
    menu->addAction(tr("Find and Replace..."), this, &CaptionsPanel::findReplaceDialog)->setObjectName(QStringLiteral("replaceCaptions"));
    menu->addAction(tr("Reading Limits..."), this, &CaptionsPanel::limitsDialog)->setObjectName(QStringLiteral("captionLimits"));
    menu->addSeparator();
    menu->addAction(tr("Rename Track..."), this, [this] {
        const CaptionTrack* t = track();
        if (!t) return;
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("Rename Track"), tr("Name:"), QLineEdit::Normal,
                                                   QString::fromStdString(t->name), &ok);
        if (ok && !name.trimmed().isEmpty())
            editTrack(tr("Rename Caption Track"), [name](CaptionTrack& tr) {
                tr.name = name.trimmed().toStdString();
                return true;
            });
    });
    menu->addAction(tr("Language..."), this, [this] {
        const CaptionTrack* t = track();
        if (!t) return;
        bool ok = false;
        const QString code = QInputDialog::getText(this, tr("Caption Language"), tr("Language code (en, fr, de...):"),
                                                   QLineEdit::Normal, QString::fromStdString(t->language), &ok);
        if (ok && !code.trimmed().isEmpty())
            editTrack(tr("Caption Language"), [code](CaptionTrack& tr) {
                tr.language = code.trimmed().toLower().toStdString();
                return true;
            });
    });
    menu->addAction(tr("Delete Track"), this, [this] {
        const Id id = track_;
        if (!id) return;
        state_->edit(tr("Delete Caption Track"), [id](Project&, Sequence& s) {
            auto it = std::find_if(s.captionTracks.begin(), s.captionTracks.end(), [id](const CaptionTrack& t) { return t.id == id; });
            if (it == s.captionTracks.end()) return false;
            s.captionTracks.erase(it);
            return true;
        });
    });
    more->setMenu(menu);
    top->addWidget(generate);
    top->addWidget(more);
    lay->addLayout(top);

    table_ = new QTableWidget(this);
    table_->setColumnCount(5);
    table_->setHorizontalHeaderLabels({tr("In"), tr("Out"), tr("Text"), QString(), QString()});
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    table_->horizontalHeaderItem(3)->setToolTip(tr("Captions that break the reading limits (More › Reading Limits)"));
    table_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    table_->horizontalHeaderItem(4)->setToolTip(tr("Where a caption sits when not at the bottom in the middle (Place)"));
    table_->verticalHeader()->hide();
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    table_->setWordWrap(true);
    lay->addWidget(table_, 1);
    summary_ = new QLabel(this);
    summary_->setObjectName(QStringLiteral("captionCheckSummary"));
    lay->addWidget(summary_);

    auto* bottom = new QHBoxLayout;
    auto* add = button(this, tr("Add"), tr("Add a caption at the playhead"));
    auto* split = button(this, tr("Split"), tr("Split the caption under the playhead"));
    auto* merge = button(this, tr("Merge"), tr("Join the selected caption with the next one"));
    auto* del = button(this, tr("Delete"), tr("Delete the selected captions"));
    auto* place = button(this, tr("Place"), tr("Move the selected captions to the top or middle, or line them up left or right"));
    place->setObjectName(QStringLiteral("placeCaptions"));
    place->setPopupMode(QToolButton::InstantPopup);
    auto* placeMenu = new QMenu(place);
    const struct {
        const char* name;
        QString text;
        int vertical, align;
    } places[] = {{"placeBottom", tr("Bottom"), kCaptionBottom, -1}, {"placeTop", tr("Top"), kCaptionTop, -1},
                  {"placeMiddle", tr("Middle"), kCaptionMiddle, -1},  {nullptr, {}, 0, 0},
                  {"placeLeft", tr("Left"), -1, kCaptionLeft},        {"placeCentre", tr("Centre"), -1, kCaptionCentre},
                  {"placeRight", tr("Right"), -1, kCaptionRight}};
    for (const auto& pl : places) {
        if (!pl.name) {
            placeMenu->addSeparator();
            continue;
        }
        const int v = pl.vertical, a = pl.align;
        placeMenu->addAction(pl.text, this, [this, v, a] { placeCaptions(v, a); })->setObjectName(QString::fromLatin1(pl.name));
    }
    placeMenu->addSeparator();
    placeMenu->addAction(tr("Move Above Titles"), this, [this] {
        const int n = raiseOverTitles();
        state_->message(n ? tr("Moved %1 captions to the top, clear of titles").arg(n) : tr("No caption is over a low title"), 3000);
    })->setObjectName(QStringLiteral("raiseCaptions"));
    place->setMenu(placeMenu);
    for (QToolButton* b : {add, split, merge, del, place}) bottom->addWidget(b);
    bottom->addStretch();
    lay->addLayout(bottom);

    connect(generate, &QToolButton::clicked, this, &CaptionsPanel::generateDialog);
    connect(add, &QToolButton::clicked, this, &CaptionsPanel::addAtPlayhead);
    connect(split, &QToolButton::clicked, this, &CaptionsPanel::splitAtPlayhead);
    connect(merge, &QToolButton::clicked, this, &CaptionsPanel::mergeWithNext);
    connect(del, &QToolButton::clicked, this, &CaptionsPanel::deleteSelected);
    connect(tracks_, &QComboBox::activated, this, [this](int i) { setCurrentTrack(tracks_->itemData(i).toULongLong()); });
    connect(visible_, &QCheckBox::clicked, this, [this](bool on) {
        editTrack(on ? tr("Show Captions") : tr("Hide Captions"), [on](CaptionTrack& t) {
            t.visible = on;
            return true;
        });
    });
    connect(table_, &QTableWidget::itemChanged, this, &CaptionsPanel::itemChanged);
    connect(table_, &QTableWidget::cellClicked, this, [this](int row, int) {
        const CaptionTrack* t = track();
        if (t && row >= 0 && size_t(row) < t->captions.size()) state_->setPlayhead(t->captions[size_t(row)].start);
    });
    connect(state_, &EditorState::projectChanged, this, &CaptionsPanel::rebuild);
    connect(state_, &EditorState::sequenceSwitched, this, &CaptionsPanel::rebuild);
    connect(state_, &EditorState::playheadChanged, this, &CaptionsPanel::followPlayhead);
    rebuild();
}

const CaptionTrack* CaptionsPanel::track() const {
    const Sequence* s = state_->sequence();
    if (!s) return nullptr;
    for (const auto& t : s->captionTracks)
        if (t.id == track_) return &t;
    return nullptr;
}

void CaptionsPanel::setCurrentTrack(Id id) {
    track_ = id;
    rebuild();
}

bool CaptionsPanel::editTrack(const QString& label, const std::function<bool(CaptionTrack&)>& fn, const QString& mergeKey) {
    const Id id = track_;
    return state_->edit(label, [id, fn](Project&, Sequence& s) {
        for (auto& t : s.captionTracks)
            if (t.id == id) {
                if (!fn(t)) return false;
                normalizeCaptions(t.captions);
                return true;
            }
        return false;
    }, mergeKey);
}

void CaptionsPanel::rebuild() {
    rebuilding_ = true;
    const Sequence* s = state_->sequence();
    tracks_->clear();
    if (s) {
        if (!track() && !s->captionTracks.empty()) track_ = s->captionTracks.front().id;
        for (const auto& t : s->captionTracks) {
            tracks_->addItem(QString::fromStdString(t.name) + QStringLiteral(" (%1)").arg(QString::fromStdString(t.language)),
                             QVariant::fromValue<qulonglong>(t.id));
            if (t.id == track_) tracks_->setCurrentIndex(tracks_->count() - 1);
        }
    }
    const CaptionTrack* t = track();
    if (!t) track_ = 0;
    tracks_->setEnabled(t != nullptr);
    visible_->setEnabled(t != nullptr);
    visible_->setChecked(t && t->visible);
    const int keepRow = table_->currentRow();
    table_->setRowCount(0);
    const CaptionLimits lim = limits();
    const std::vector<unsigned> problems = t && s ? checkCaptions(t->captions, s->fps, lim) : std::vector<unsigned>();
    const int flagged = int(std::count_if(problems.begin(), problems.end(), [](unsigned v) { return v != 0; }));
    summary_->setText(!t || t->captions.empty() ? QString()
                      : flagged            ? tr("%1 of %2 captions break the reading limits").arg(flagged).arg(t->captions.size())
                                           : tr("All %1 captions are within the reading limits").arg(t->captions.size()));
    if (t && s) {
        table_->setRowCount(int(t->captions.size()));
        for (size_t i = 0; i < t->captions.size(); ++i) {
            const Caption& c = t->captions[i];
            auto* in = new QTableWidgetItem(QString::fromStdString(formatTimecode(c.start, s->fps)));
            auto* out = new QTableWidgetItem(QString::fromStdString(formatTimecode(c.end, s->fps)));
            auto* text = new QTableWidgetItem(QString::fromStdString(c.text));
            // What it breaks, if anything: a warning sign with the details as its tooltip.
            auto* check = new QTableWidgetItem(problems[i] ? QStringLiteral("\u26A0") : QString());
            check->setFlags(check->flags() & ~Qt::ItemIsEditable);
            if (problems[i]) {
                check->setToolTip(QString::fromStdString(describeCaptionIssues(problems[i], c, s->fps, lim)));
                check->setForeground(QColor(230, 160, 40));
            }
            // Its place, when not the usual: arrows up or to the middle, and to a side.
            QString where = c.vertical == kCaptionTop ? QStringLiteral("\u2191") : c.vertical == kCaptionMiddle ? QStringLiteral("\u2195") : QString();
            where += c.align == kCaptionLeft ? QStringLiteral("\u2190") : c.align == kCaptionRight ? QStringLiteral("\u2192") : QString();
            auto* placeItem = new QTableWidgetItem(where);
            placeItem->setFlags(placeItem->flags() & ~Qt::ItemIsEditable);
            if (!where.isEmpty()) placeItem->setToolTip(QString::fromStdString(captionPlaceName(c)));
            for (auto* item : {in, out, text, check, placeItem}) item->setData(kIndexRole, int(i));
            table_->setItem(int(i), 0, in);
            table_->setItem(int(i), 1, out);
            table_->setItem(int(i), 2, text);
            table_->setItem(int(i), 3, check);
            table_->setItem(int(i), 4, placeItem);
        }
        table_->resizeRowsToContents();
        if (keepRow >= 0 && keepRow < table_->rowCount()) table_->setCurrentCell(keepRow, 2);
    }
    rebuilding_ = false;
    followPlayhead(state_->playhead());
}

void CaptionsPanel::followPlayhead(FrameTime t) {
    const CaptionTrack* tr = track();
    if (!tr) return;
    const QBrush now = palette().brush(QPalette::Highlight);
    const int at = [&] {
        const Caption* c = captionAt(*tr, t);
        return c ? int(c - tr->captions.data()) : -1;
    }();
    for (int r = 0; r < table_->rowCount(); ++r)
        if (QTableWidgetItem* item = table_->item(r, 0)) {
            QFont f = item->font();
            f.setBold(r == at);
            item->setFont(f);
        }
}

void CaptionsPanel::editCaption(Id track, int index) {
    setCurrentTrack(track);
    if (index < 0 || index >= table_->rowCount()) return;
    table_->setCurrentCell(index, 2);
    table_->scrollToItem(table_->item(index, 2));
    table_->editItem(table_->item(index, 2));
}

void CaptionsPanel::itemChanged(QTableWidgetItem* item) {
    if (rebuilding_ || !item) return;
    const int i = item->data(kIndexRole).toInt();
    const Sequence* s = state_->sequence();
    if (!s) return;
    const QString value = item->text();
    if (item->column() >= 3) return;
    if (item->column() == 2) {
        editTrack(tr("Edit Caption"), [i, value](CaptionTrack& t) {
            if (size_t(i) >= t.captions.size()) return false;
            t.captions[size_t(i)].text = value.toStdString();
            return true;
        }, QStringLiteral("caption-text-%1").arg(i));
        return;
    }
    FrameTime f = 0;
    if (!parseTimecode(value.toStdString(), s->fps, f)) {
        state_->message(tr("Not a timecode: %1").arg(value));
        QMetaObject::invokeMethod(this, &CaptionsPanel::rebuild, Qt::QueuedConnection);
        return;
    }
    const bool in = item->column() == 0;
    const bool ok = editTrack(in ? tr("Caption In") : tr("Caption Out"), [i, f, in](CaptionTrack& t) {
        if (size_t(i) >= t.captions.size()) return false;
        Caption& c = t.captions[size_t(i)];
        if (in) c.start = std::min(f, c.end - 1);
        else c.end = std::max(f, c.start + 1);
        return true;
    });
    if (!ok) QMetaObject::invokeMethod(this, &CaptionsPanel::rebuild, Qt::QueuedConnection);
}

Id CaptionsPanel::translateTrack(const std::string& to, QString* error) {
    const CaptionTrack* src = track();
    if (!src || src->captions.empty()) {
        if (error) *error = tr("There are no captions to translate");
        return 0;
    }
    const std::string from = src->language.empty() ? "en" : src->language;
    if (translationRoute(from, to).empty()) {
        if (error)
            *error = tr("There is no translation from %1 to %2")
                         .arg(QString::fromStdString(translationLanguageName(from)), QString::fromStdString(translationLanguageName(to)));
        return 0;
    }
    const CaptionTrack source = *src;
    const std::vector<std::string> texts = captionTexts(source);
    // Translating takes a while: in the background, with progress.
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    auto done = std::make_shared<std::atomic<double>>(0.0);
    QProgressDialog progress(tr("Translating captions…"), tr("Cancel"), 0, 1000, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(400);
    connect(&progress, &QProgressDialog::canceled, &progress, [cancel] { *cancel = true; });
    QTimer tick;
    connect(&tick, &QTimer::timeout, &progress, [&progress, done] { progress.setValue(int(*done * 1000)); });
    tick.start(100);
    using Out = std::pair<std::vector<std::string>, std::string>;
    QFutureWatcher<Out> watcher;
    QEventLoop wait;
    connect(&watcher, &QFutureWatcher<Out>::finished, &wait, &QEventLoop::quit);
    watcher.setFuture(QtConcurrent::run([texts, from, to, cancel, done] {
        Out out;
        std::string err;
        if (!translateTexts(texts, from, to, out.first, [done](double f) { *done = f; }, cancel.get(), &err)) out.second = err.empty() ? "Cancelled" : err;
        return out;
    }));
    if (!watcher.isFinished()) wait.exec();
    tick.stop();
    disconnect(&progress, &QProgressDialog::canceled, nullptr, nullptr);
    progress.close();
    const Out r = watcher.result();
    if (*cancel || !r.second.empty()) {
        if (error) *error = *cancel ? QString() : QString::fromStdString(r.second);
        return 0;
    }
    Id created = 0;
    state_->edit(tr("Translate Captions"), [&](Project& p, Sequence& sq) {
        CaptionTrack t = translatedTrack(source, r.first, p.newId(), to, translationLanguageName(to));
        t.visible = false;  // the original stays the one shown until this is chosen
        created = t.id;
        sq.captionTracks.push_back(std::move(t));
        return true;
    });
    if (created) setCurrentTrack(created);
    return created;
}

void CaptionsPanel::translateDialog() {
    const CaptionTrack* src = track();
    if (!src || src->captions.empty()) {
        state_->message(tr("There are no captions to translate"));
        return;
    }
    if (!translatorAvailable()) {
        QMessageBox::information(this, tr("Translate Captions"), tr("This build of Montage cannot translate: it was built without ONNX Runtime."));
        return;
    }
    const std::string from = src->language.empty() ? "en" : src->language;
    QStringList names;
    std::vector<std::string> codes;
    for (const TranslationLanguage& l : translationLanguages())
        if (l.code != from && !translationRoute(from, l.code).empty()) {
            names << QString::fromStdString(l.name);
            codes.push_back(l.code);
        }
    if (codes.empty()) {
        state_->message(tr("There is no translation from %1").arg(QString::fromStdString(translationLanguageName(from))));
        return;
    }
    bool ok = false;
    const QString pick = QInputDialog::getItem(this, tr("Translate Captions"),
                                               tr("Translate \"%1\" (%2) into:").arg(QString::fromStdString(src->name),
                                                                                   QString::fromStdString(translationLanguageName(from))),
                                               names, 0, false, &ok);
    if (!ok) return;
    const std::string to = codes[size_t(names.indexOf(pick))];
    for (const ModelPack* pack : translationRoute(from, to))
        if (!ensureModelPack(window(), *pack, tr("Translate Captions"),
                             tr("Translation runs on this computer with Opus-MT (University of Helsinki, CC-BY-4.0), one model per language pair.")))
            return;
    QString error;
    if (!translateTrack(to, &error) && !error.isEmpty()) QMessageBox::warning(this, tr("Translate Captions"), error);
}

CaptionsPanel::DubResult CaptionsPanel::dub(const std::string& voice, double duckDb, QString* error) {
    DubResult r;
    const CaptionTrack* src = track();
    if (!src || src->captions.empty()) {
        if (error) *error = tr("There are no captions to dub");
        return r;
    }
    const std::string from = src->language.empty() ? "en" : src->language;
    r.captions = from == "en" ? src->id : translateTrack("en", error);
    if (!r.captions) return r;
    const Sequence* s = state_->sequence();
    int index = -1;
    for (int i = 0; i < int(s->captionTracks.size()); ++i)
        if (s->captionTracks[size_t(i)].id == r.captions) index = i;
    r.clips = generateSpeech(state_, captionLines(*s, index), voice, 1.0, -1, window(), error);
    if (r.clips.empty()) return r;
    const auto at = edit::locate(*state_->sequence(), r.clips.front());
    if (!at) return r;
    r.audioTrack = at->track.index;
    // Part of the same undo step as the speech.
    state_->amend([&](Project&, Sequence& sq) {
        sq.audioTracks[size_t(r.audioTrack)].name = tr("Dub (English)").toStdString();
        if (duckDb < 0) {
            DuckOptions o;
            o.amountDb = std::max(-60.0, duckDb);
            const Spans spans = clipSpans(sq, r.audioTrack, o.minPause);
            for (int i = 0; i < int(sq.audioTracks.size()); ++i)
                if (i != r.audioTrack)
                    for (Clip& c : sq.audioTracks[size_t(i)].clips) r.ducked += duckClip(c, sq, spans, o) ? 1 : 0;
        }
        return true;
    });
    return r;
}

void CaptionsPanel::dubDialog() {
    const CaptionTrack* src = track();
    if (!src || src->captions.empty()) {
        state_->message(tr("There are no captions to dub"));
        return;
    }
    if (!translatorAvailable() || !ttsAvailable()) {
        QMessageBox::information(this, tr("Dub into English"), tr("This build of Montage cannot dub: it was built without ONNX Runtime."));
        return;
    }
    const std::string from = src->language.empty() ? "en" : src->language;
    const auto route = from == "en" ? std::vector<const ModelPack*>{} : translationRoute(from, "en");
    if (from != "en" && route.empty()) {
        state_->message(tr("There is no translation from %1 to English").arg(QString::fromStdString(translationLanguageName(from))));
        return;
    }
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Dub into English"));
    auto* lay = new QVBoxLayout(&dlg);
    auto* about = new QLabel(from == "en" ? tr("Speaks \"%1\" with an AI voice on a new audio track, each caption at its time, and lowers "
                                               "the other audio under it.")
                                                .arg(QString::fromStdString(src->name))
                                          : tr("Translates \"%1\" from %2 into English, speaks it with an AI voice on a new audio track, "
                                               "each caption at its time, and lowers the other audio under it.")
                                                .arg(QString::fromStdString(src->name), QString::fromStdString(translationLanguageName(from))),
                             &dlg);
    about->setWordWrap(true);
    lay->addWidget(about);
    auto* form = new QFormLayout;
    auto* voice = new QComboBox(&dlg);
    voice->setObjectName(QStringLiteral("dubVoice"));
    for (const TtsVoice& v : ttsVoices()) voice->addItem(QString::fromStdString(v.name), QString::fromStdString(v.id));
    form->addRow(tr("Voice:"), voice);
    auto* duck = new QDoubleSpinBox(&dlg);
    duck->setObjectName(QStringLiteral("dubDuck"));
    duck->setRange(-60, 0);
    duck->setValue(-18);
    duck->setSuffix(tr(" dB"));
    duck->setToolTip(tr("How far the original audio is lowered while the dub speaks (0 leaves it as it is)"));
    form->addRow(tr("Lower the original by:"), duck);
    lay->addLayout(form);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Dub"));
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(buttons);
    if (dlg.exec() != QDialog::Accepted) return;
    for (const ModelPack* pack : route)
        if (!ensureModelPack(window(), *pack, tr("Dub into English"),
                             tr("Translation runs on this computer with Opus-MT (University of Helsinki, CC-BY-4.0), one model per language pair.")))
            return;
    QString error;
    const DubResult r = dub(voice->currentData().toString().toStdString(), duck->value(), &error);
    if (r.clips.empty()) {
        if (!error.isEmpty() && error != QLatin1String("Cancelled")) QMessageBox::warning(this, tr("Dub into English"), error);
        return;
    }
    state_->message(tr("Dubbed %1 captions onto %2").arg(r.clips.size()).arg(QString::fromStdString(state_->sequence()->audioTracks[size_t(r.audioTrack)].name)));
}

void CaptionsPanel::addTrack() {
    const Sequence* s = state_->sequence();
    if (!s) return;
    Id created = 0;
    const int n = int(s->captionTracks.size()) + 1;
    state_->edit(tr("New Caption Track"), [&created, n, this](Project& p, Sequence& sq) {
        CaptionTrack t;
        t.id = p.newId();
        t.name = (n == 1 ? tr("Subtitles") : tr("Subtitles %1").arg(n)).toStdString();
        sq.captionTracks.push_back(t);
        created = t.id;
        return true;
    });
    if (created) setCurrentTrack(created);
}

int CaptionsPanel::generateFromTranscripts(const CaptionRules& rules) {
    const Sequence* s = state_->sequence();
    if (!s) return 0;
    const std::vector<Caption> caps = captionsFromTranscripts(state_->project(), *s, rules);
    if (caps.empty()) return 0;
    Id id = track_;
    state_->edit(tr("Generate Captions"), [&id, &caps, this](Project& p, Sequence& sq) {
        CaptionTrack* t = nullptr;
        for (auto& tr : sq.captionTracks)
            if (tr.id == id) t = &tr;
        if (!t) {
            sq.captionTracks.push_back(CaptionTrack{});
            t = &sq.captionTracks.back();
            t->id = id = p.newId();
            t->name = tr("Subtitles").toStdString();
            // Label the track with the transcripts' language.
            for (const auto& m : p.media)
                if (m.transcript && !m.transcript->language.empty()) {
                    t->language = m.transcript->language;
                    break;
                }
        }
        t->captions = caps;
        return true;
    });
    setCurrentTrack(id);
    return int(caps.size());
}

void CaptionsPanel::generateDialog() {
    const Sequence* s = state_->sequence();
    if (!s) return;
    // Media in the cut that has sound but no transcript yet.
    std::vector<Id> untranscribed;
    bool anyTranscript = false;
    for (const auto* list : {&s->audioTracks, &s->videoTracks})
        for (const auto& tr : *list)
            for (const auto& c : tr.clips)
                if (const MediaItem* m = state_->project().findMedia(c.mediaId); m && m->hasAudio && !m->path.empty()) {
                    if (m->transcript) anyTranscript = true;
                    else if (std::find(untranscribed.begin(), untranscribed.end(), m->id) == untranscribed.end())
                        untranscribed.push_back(m->id);
                }
    if (!untranscribed.empty()) {
        QMessageBox box(QMessageBox::Question, tr("Generate Captions"),
                        tr("%n clip(s) in this sequence have not been transcribed yet. Transcribe them first? "
                           "Click Generate again when it finishes.",
                           "", int(untranscribed.size())),
                        QMessageBox::NoButton, this);
        auto* transcribe = box.addButton(tr("Transcribe..."), QMessageBox::AcceptRole);
        QPushButton* skip = anyTranscript ? box.addButton(tr("Use Existing Transcripts"), QMessageBox::ActionRole) : nullptr;
        box.addButton(QMessageBox::Cancel);
        box.exec();
        if (box.clickedButton() == transcribe) {
            TranscribeDialog dlg(int(untranscribed.size()), this);
            if (dlg.exec() == QDialog::Accepted) startTranscription(state_, untranscribed, dlg.options(), this);
            return;
        }
        if (!skip || box.clickedButton() != skip) return;
    } else if (!anyTranscript) {
        QMessageBox::information(this, tr("Generate Captions"), tr("There is no speech in this sequence to caption."));
        return;
    }
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Generate Captions"));
    auto* form = new QFormLayout(&dlg);
    QSettings settings = appSettings();
    auto* chars = new QSpinBox(&dlg);
    chars->setRange(16, 80);
    chars->setValue(settings.value("captions/lineChars", 42).toInt());
    auto* lines = new QSpinBox(&dlg);
    lines->setRange(1, 3);
    lines->setValue(settings.value("captions/maxLines", 2).toInt());
    auto* secs = new QDoubleSpinBox(&dlg);
    secs->setRange(1, 15);
    secs->setSuffix(tr(" s"));
    secs->setValue(settings.value("captions/maxSeconds", 7.0).toDouble());
    form->addRow(tr("Characters per line:"), chars);
    form->addRow(tr("Lines per caption:"), lines);
    form->addRow(tr("Longest caption:"), secs);
    if (track()) {
        auto* note = new QLabel(tr("Replaces the captions in \"%1\".").arg(QString::fromStdString(track()->name)), &dlg);
        form->addRow(note);
    }
    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(box);
    if (dlg.exec() != QDialog::Accepted) return;
    settings.setValue("captions/lineChars", chars->value());
    settings.setValue("captions/maxLines", lines->value());
    settings.setValue("captions/maxSeconds", secs->value());
    CaptionRules rules;
    rules.lineChars = chars->value();
    rules.maxLines = lines->value();
    rules.maxSeconds = secs->value();
    const int n = generateFromTranscripts(rules);
    state_->message(n ? tr("Generated %n caption(s)", "", n) : tr("No transcribed speech is heard in this sequence"));
}

bool CaptionsPanel::importFile(const QString& path, QString* error) {
    const Sequence* s = state_->sequence();
    QFile f(path);
    if (!s || !f.open(QIODevice::ReadOnly)) {
        if (error) *error = tr("Cannot read %1").arg(path);
        return false;
    }
    std::vector<Caption> caps;
    std::string err;
    if (!parseSubtitles(f.readAll().toStdString(), s->fps, caps, &err)) {
        if (error) *error = QString::fromStdString(err);
        return false;
    }
    Id id = 0;
    const QString name = QFileInfo(path).completeBaseName();
    state_->edit(tr("Import Captions"), [&](Project& p, Sequence& sq) {
        CaptionTrack t;
        t.id = id = p.newId();
        t.name = name.toStdString();
        // "film.fr.srt" names its language.
        const QString suffix = name.section('.', -1);
        if (name.contains('.') && suffix.size() == 2) t.language = suffix.toLower().toStdString();
        t.captions = caps;
        sq.captionTracks.push_back(std::move(t));
        return true;
    });
    setCurrentTrack(id);
    return true;
}

bool CaptionsPanel::exportFile(const QString& path, QString* error) const {
    const CaptionTrack* t = track();
    const Sequence* s = state_->sequence();
    if (!t || !s) {
        if (error) *error = tr("There are no captions to export");
        return false;
    }
    const std::string suffix = QFileInfo(path).suffix().toLower().toStdString();
    const std::string text = exportCaptions(*t, *s, captionFormatKnown(suffix) ? suffix : "srt");
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(text.data(), qint64(text.size())) != qint64(text.size()) || !f.commit()) {
        if (error) *error = tr("Cannot write %1").arg(path);
        return false;
    }
    return true;
}

void CaptionsPanel::importDialog() {
    QSettings settings = appSettings();
    const QString path = QFileDialog::getOpenFileName(this, tr("Import Captions"), settings.value("captions/dir").toString(),
                                                      tr("Captions (*.srt *.vtt *.scc *.ttml *.xml *.dfxp *.stl *.ass *.ssa);;All files (*)"));
    if (path.isEmpty()) return;
    settings.setValue("captions/dir", QFileInfo(path).absolutePath());
    QString err;
    if (!importFile(path, &err)) QMessageBox::warning(this, tr("Import Captions"), err);
}

void CaptionsPanel::exportDialog() {
    const CaptionTrack* t = track();
    if (!t) return;
    QSettings settings = appSettings();
    // Each format and its extension.
    const QList<QPair<QString, QString>> formats = {{tr("SubRip (*.srt)"), ".srt"},
                                                    {tr("WebVTT (*.vtt)"), ".vtt"},
                                                    {tr("Scenarist SCC, CEA-608 (*.scc)"), ".scc"},
                                                    {tr("TTML, IMSC 1.1 (*.ttml *.xml *.dfxp)"), ".ttml"},
                                                    {tr("EBU STL (*.stl)"), ".stl"},
                                                    {tr("Advanced SubStation Alpha (*.ass *.ssa)"), ".ass"}};
    QStringList filters;
    for (const auto& f : formats) filters << f.first;
    QString filter = filters.front();
    QString path = QFileDialog::getSaveFileName(this, tr("Export Captions"),
                                                settings.value("captions/dir").toString() + "/" +
                                                    QString::fromStdString(t->name) + ".srt",
                                                filters.join(";;"), &filter);
    if (path.isEmpty()) return;
    if (QFileInfo(path).suffix().isEmpty())
        for (const auto& f : formats)
            if (f.first == filter) path += f.second;
    settings.setValue("captions/dir", QFileInfo(path).absolutePath());
    QString err;
    if (!exportFile(path, &err)) QMessageBox::warning(this, tr("Export Captions"), err);
    else state_->message(tr("Exported %1").arg(QFileInfo(path).fileName()));
}

void CaptionsPanel::styleDialog() {
    const CaptionTrack* t = track();
    if (!t) return;
    CaptionStyle st = t->style;
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Caption Style"));
    auto* form = new QFormLayout(&dlg);
    // A ready-made look to start from (or one saved earlier), then any changes.
    auto* look = new QComboBox(&dlg);
    look->setObjectName(QStringLiteral("captionLook"));
    look->addItem(tr("Custom"), QString());
    for (const CaptionLook& l : captionLooks()) look->addItem(tr(l.name.c_str()), QString::fromStdString(l.id));
    for (const QString& name : savedLooks()) look->addItem(name, QStringLiteral("saved:") + name);
    form->addRow(tr("Look:"), look);
    std::vector<std::function<void()>> refresh;  // puts the controls back to `st`
    auto* font = new QFontComboBox(&dlg);
    font->setCurrentFont(QFont(QString::fromStdString(st.font)));
    auto* size = new QDoubleSpinBox(&dlg);
    size->setRange(2, 20);
    size->setSingleStep(0.5);
    size->setSuffix(tr("% of height"));
    size->setValue(st.size * 100);
    auto* bold = new QCheckBox(tr("Bold"), &dlg);
    bold->setChecked(st.bold);
    auto colourButton = [&](double& r, double& g, double& b) {
        auto* btn = new QPushButton(&dlg);
        auto paint = [btn, &r, &g, &b] {
            btn->setStyleSheet(QStringLiteral("background-color: %1;").arg(QColor::fromRgbF(float(r), float(g), float(b)).name()));
        };
        paint();
        refresh.push_back(paint);
        connect(btn, &QPushButton::clicked, &dlg, [&, paint] {
            const QColor c = QColorDialog::getColor(QColor::fromRgbF(float(r), float(g), float(b)), &dlg);
            if (!c.isValid()) return;
            r = c.redF();
            g = c.greenF();
            b = c.blueF();
            paint();
        });
        return btn;
    };
    auto* box = new QSpinBox(&dlg);
    box->setRange(0, 100);
    box->setSuffix(QStringLiteral("%"));
    box->setValue(int(std::lround(st.boxOpacity * 100)));
    auto* outline = new QDoubleSpinBox(&dlg);
    outline->setRange(0, 20);
    outline->setSuffix(tr("% of text"));
    outline->setValue(st.outline * 100);
    auto* position = new QSpinBox(&dlg);
    position->setRange(10, 100);
    position->setSuffix(tr("% down"));
    position->setValue(int(std::lround(st.position * 100)));
    form->addRow(tr("Font:"), font);
    form->addRow(tr("Size:"), size);
    form->addRow(QString(), bold);
    form->addRow(tr("Text colour:"), colourButton(st.textR, st.textG, st.textB));
    form->addRow(tr("Background:"), box);
    form->addRow(tr("Background colour:"), colourButton(st.boxR, st.boxG, st.boxB));
    form->addRow(tr("Outline:"), outline);
    form->addRow(tr("Outline colour:"), colourButton(st.outlineR, st.outlineG, st.outlineB));
    auto* shadow = new QDoubleSpinBox(&dlg);
    shadow->setObjectName(QStringLiteral("captionShadow"));
    shadow->setRange(0, 30);
    shadow->setSuffix(tr("% of text"));
    shadow->setValue(st.shadow * 100);
    form->addRow(tr("Shadow:"), shadow);
    auto* caps = new QCheckBox(tr("All capitals"), &dlg);
    caps->setObjectName(QStringLiteral("captionAllCaps"));
    caps->setChecked(st.allCaps);
    form->addRow(QString(), caps);
    form->addRow(tr("Bottom edge:"), position);
    // Word animation (from the words' timings when the captions came from a transcript).
    auto* animation = new QComboBox(&dlg);
    animation->setObjectName(QStringLiteral("captionAnimation"));
    animation->addItems({tr("None"), tr("Word by Word"), tr("Highlight the Spoken Word"), tr("Pop the Spoken Word"), tr("One Word at a Time")});
    animation->setCurrentIndex(std::clamp(st.animation, 0, 4));
    animation->setToolTip(tr("Animate captions word by word, as social video does"));
    form->addRow(tr("Animation:"), animation);
    form->addRow(tr("Highlight colour:"), colourButton(st.hiR, st.hiG, st.hiB));
    refresh.push_back([&] {
        font->setCurrentFont(QFont(QString::fromStdString(st.font)));
        size->setValue(st.size * 100);
        bold->setChecked(st.bold);
        box->setValue(int(std::lround(st.boxOpacity * 100)));
        outline->setValue(st.outline * 100);
        shadow->setValue(st.shadow * 100);
        caps->setChecked(st.allCaps);
        position->setValue(int(std::lround(st.position * 100)));
        animation->setCurrentIndex(std::clamp(st.animation, 0, 4));
    });
    connect(look, &QComboBox::activated, &dlg, [&](int i) {
        const QString id = look->itemData(i).toString();
        CaptionStyle chosen;
        if (id.startsWith(QStringLiteral("saved:"))) {
            if (!captionStyleFromJsonString(appSettings().value("captions/looks/" + id.mid(6)).toString().toStdString(), chosen)) return;
        } else if (const CaptionLook* l = findCaptionLook(id.toStdString())) {
            chosen = l->style;
        } else {
            return;
        }
        st = chosen;
        for (auto& f : refresh) f();
    });
    auto* save = new QPushButton(tr("Save Look…"), &dlg);
    save->setToolTip(tr("Keep these settings as a look of your own, for other tracks and projects"));
    connect(save, &QPushButton::clicked, &dlg, [&] {
        bool ok = false;
        const QString name = QInputDialog::getText(&dlg, tr("Save Look"), tr("Name:"), QLineEdit::Normal, QString(), &ok).trimmed();
        if (!ok || name.isEmpty()) return;
        CaptionStyle now = st;
        now.font = font->currentFont().family().toStdString();
        now.size = size->value() / 100.0;
        now.bold = bold->isChecked();
        now.boxOpacity = box->value() / 100.0;
        now.outline = outline->value() / 100.0;
        now.shadow = shadow->value() / 100.0;
        now.allCaps = caps->isChecked();
        now.position = position->value() / 100.0;
        now.animation = animation->currentIndex();
        appSettings().setValue("captions/looks/" + name, QString::fromStdString(captionStyleToJsonString(now)));
        look->addItem(name, QStringLiteral("saved:") + name);
    });
    form->addRow(QString(), save);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);
    if (dlg.exec() != QDialog::Accepted) return;
    st.font = font->currentFont().family().toStdString();
    st.size = size->value() / 100.0;
    st.bold = bold->isChecked();
    st.boxOpacity = box->value() / 100.0;
    st.outline = outline->value() / 100.0;
    st.shadow = shadow->value() / 100.0;
    st.allCaps = caps->isChecked();
    st.position = position->value() / 100.0;
    st.animation = animation->currentIndex();
    editTrack(tr("Caption Style"), [st](CaptionTrack& tr) {
        if (tr.style == st) return false;
        tr.style = st;
        return true;
    });
}

void CaptionsPanel::addAtPlayhead() {
    if (!track()) addTrack();
    const Sequence* s = state_->sequence();
    if (!s || !track()) return;
    const FrameTime at = state_->playhead();
    const FrameTime len = FrameTime(std::llround(s->fpsValue() * 2));
    editTrack(tr("Add Caption"), [at, len, this](CaptionTrack& t) {
        if (captionAt(t, at)) return false;
        const size_t next = captionIndexAt(t, at);
        const FrameTime end = next < t.captions.size() ? std::min(at + len, t.captions[next].start) : at + len;
        if (end <= at) return false;
        Caption c;
        c.start = at;
        c.end = end;
        c.text = tr("Caption").toStdString();
        t.captions.push_back(std::move(c));
        return true;
    });
    if (const CaptionTrack* t = track())
        if (const Caption* c = captionAt(*t, at)) {
            const int row = int(c - t->captions.data());
            table_->setCurrentCell(row, 2);
            table_->editItem(table_->item(row, 2));
        }
}

void CaptionsPanel::deleteSelected() {
    std::vector<int> rows;
    for (QTableWidgetItem* item : table_->selectedItems()) rows.push_back(item->data(kIndexRole).toInt());
    if (rows.empty()) return;
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    editTrack(tr("Delete Captions"), [rows](CaptionTrack& t) {
        for (auto it = rows.rbegin(); it != rows.rend(); ++it)
            if (size_t(*it) < t.captions.size()) t.captions.erase(t.captions.begin() + *it);
        return true;
    });
}

void CaptionsPanel::mergeWithNext() {
    const int row = table_->currentRow();
    if (row < 0) return;
    editTrack(tr("Merge Captions"), [row](CaptionTrack& t) {
        if (size_t(row) + 1 >= t.captions.size()) return false;
        Caption& a = t.captions[size_t(row)];
        const Caption& b = t.captions[size_t(row) + 1];
        a.text = wrapCaptionText(a.text + " " + b.text, 42, 2);
        a.end = b.end;
        t.captions.erase(t.captions.begin() + row + 1);
        return true;
    });
}

void CaptionsPanel::splitAtPlayhead() {
    const FrameTime at = state_->playhead();
    editTrack(tr("Split Caption"), [at](CaptionTrack& t) {
        const Caption* c = captionAt(t, at);
        if (!c || at <= c->start) return false;
        const size_t i = size_t(c - t.captions.data());
        // Divide the words in proportion to the time on each side.
        QStringList words = QString::fromStdString(t.captions[i].text).simplified().split(' ');
        const double frac = double(at - c->start) / double(c->end - c->start);
        const int n = std::clamp(int(std::lround(words.size() * frac)), words.size() > 1 ? 1 : 0, int(words.size()));
        Caption second = *c;  // in the same place
        second.start = at;
        second.text = words.mid(n).join(' ').toStdString();
        second.wordTimes.clear();
        t.captions[i].end = at;
        t.captions[i].text = wrapCaptionText(words.mid(0, n).join(' ').toStdString());
        second.text = wrapCaptionText(second.text);
        if (second.text.empty()) second.text = t.captions[i].text;
        t.captions.insert(t.captions.begin() + long(i) + 1, second);
        return true;
    });
}

// ---- Subtitle tools ---------------------------------------------------------------

CaptionLimits CaptionsPanel::limits() const {
    QSettings st = appSettings();
    CaptionLimits l;
    l.maxCps = st.value("captions/maxCps", l.maxCps).toDouble();
    l.maxLineChars = st.value("captions/maxLineChars", l.maxLineChars).toInt();
    l.maxLines = st.value("captions/maxLines", l.maxLines).toInt();
    l.minSeconds = st.value("captions/minSeconds", l.minSeconds).toDouble();
    l.maxSeconds = st.value("captions/maxSeconds", l.maxSeconds).toDouble();
    l.minGapFrames = st.value("captions/minGapFrames", l.minGapFrames).toInt();
    return l;
}

void CaptionsPanel::setLimits(const CaptionLimits& l) {
    QSettings st = appSettings();
    st.setValue("captions/maxCps", l.maxCps);
    st.setValue("captions/maxLineChars", l.maxLineChars);
    st.setValue("captions/maxLines", l.maxLines);
    st.setValue("captions/minSeconds", l.minSeconds);
    st.setValue("captions/maxSeconds", l.maxSeconds);
    st.setValue("captions/minGapFrames", l.minGapFrames);
    rebuild();
}

std::vector<unsigned> CaptionsPanel::issues() const {
    const CaptionTrack* t = track();
    const Sequence* s = state_->sequence();
    return t && s ? checkCaptions(t->captions, s->fps, limits()) : std::vector<unsigned>();
}

std::vector<size_t> CaptionsPanel::selectedCaptions() const {
    std::vector<size_t> rows;
    for (const QModelIndex& i : table_->selectionModel()->selectedRows()) rows.push_back(size_t(i.row()));
    std::sort(rows.begin(), rows.end());
    return rows;
}

int CaptionsPanel::fixTiming() {
    const Sequence* s = state_->sequence();
    if (!s) return 0;
    const Rational fps = s->fps;
    const CaptionLimits l = limits();
    int changed = 0;
    editTrack(tr("Fix Caption Timing"), [&](CaptionTrack& t) { return (changed = fixCaptionTiming(t.captions, fps, l)) > 0; });
    return changed;
}

bool CaptionsPanel::shiftCaptions(FrameTime delta) {
    const std::vector<size_t> rows = selectedCaptions();
    return editTrack(tr("Shift Captions"), [&](CaptionTrack& t) { return montage::shiftCaptions(t.captions, rows, delta); });
}

bool CaptionsPanel::syncToTwoPoints(FrameTime first, FrameTime last) {
    const CaptionTrack* t = track();
    if (!t || t->captions.size() < 2) return false;
    std::vector<size_t> rows = selectedCaptions();
    if (rows.size() < 2) rows = {0, t->captions.size() - 1};
    const FrameTime fromA = t->captions[rows.front()].start, fromB = t->captions[rows.back()].start;
    return editTrack(tr("Sync Captions"), [&](CaptionTrack& ct) { return syncCaptions(ct.captions, fromA, first, fromB, last); });
}

int CaptionsPanel::findReplace(const QString& find, const QString& replace, bool caseSensitive, bool wholeWords) {
    const std::vector<size_t> rows = selectedCaptions();
    int n = 0;
    editTrack(tr("Replace in Captions"), [&](CaptionTrack& t) {
        return (n = replaceInCaptions(t.captions, rows, find.toStdString(), replace.toStdString(), caseSensitive, wholeWords)) > 0;
    });
    return n;
}

QStringList CaptionsPanel::savedLooks() const {
    QSettings st = appSettings();
    st.beginGroup(QStringLiteral("captions/looks"));
    QStringList names = st.childKeys();
    names.sort(Qt::CaseInsensitive);
    return names;
}

bool CaptionsPanel::applyLook(const QString& idOrName) {
    CaptionStyle chosen;
    if (const CaptionLook* l = findCaptionLook(idOrName.toStdString())) chosen = l->style;
    else if (!captionStyleFromJsonString(appSettings().value("captions/looks/" + idOrName).toString().toStdString(), chosen) ||
             !savedLooks().contains(idOrName))
        return false;
    return editTrack(tr("Caption Look"), [chosen](CaptionTrack& t) {
        if (t.style == chosen) return false;
        t.style = chosen;
        return true;
    });
}

bool CaptionsPanel::saveLook(const QString& name) {
    const CaptionTrack* t = track();
    if (!t || name.trimmed().isEmpty() || name.contains('/') || findCaptionLook(name.trimmed().toStdString())) return false;
    appSettings().setValue("captions/looks/" + name.trimmed(), QString::fromStdString(captionStyleToJsonString(t->style)));
    return true;
}

bool CaptionsPanel::placeCaptions(int vertical, int align) {
    const CaptionTrack* t = track();
    if (!t) return false;
    std::vector<size_t> rows = selectedCaptions();
    if (rows.empty()) {
        const Caption* at = captionAt(*t, state_->playhead());
        if (!at) return false;
        rows = {size_t(at - t->captions.data())};
    }
    const QString label = vertical == kCaptionTop ? tr("Captions to Top") : vertical >= 0 ? tr("Place Captions") : tr("Line Up Captions");
    return editTrack(label, [&](CaptionTrack& ct) { return montage::placeCaptions(ct.captions, rows, vertical, align); });
}

int CaptionsPanel::raiseOverTitles() {
    const Sequence* s = state_->sequence();
    if (!s) return 0;
    const Sequence seq = *s;
    int n = 0;
    editTrack(tr("Move Captions Above Titles"), [&](CaptionTrack& t) { return (n = raiseCaptionsOverTitles(t.captions, seq)) > 0; });
    return n;
}

void CaptionsPanel::shiftDialog() {
    const Sequence* s = state_->sequence();
    if (!track() || !s) return;
    bool ok = false;
    const QString scope = selectedCaptions().empty() ? tr("all captions") : tr("the %1 selected captions").arg(selectedCaptions().size());
    const QString text = QInputDialog::getText(this, tr("Shift Captions"),
                                               tr("Move %1 by (seconds, or frames, negative for earlier):").arg(scope),
                                               QLineEdit::Normal, QStringLiteral("0.5s"), &ok);
    if (!ok || text.trimmed().isEmpty()) return;
    FrameTime delta = 0;
    if (!parseTimecode(text.trimmed().toStdString(), s->fps, delta)) {
        // "-1.5s": parseTimecode takes seconds unsigned.
        const QString t = text.trimmed();
        bool okNum = false;
        const double sec = (t.endsWith('s') ? t.chopped(1) : t).toDouble(&okNum);
        if (!okNum) {
            state_->message(tr("Not a time: %1").arg(text));
            return;
        }
        delta = FrameTime(std::llround(sec * s->fpsValue()));
    }
    if (!shiftCaptions(delta)) state_->message(tr("Nothing to shift"), 3000);
}

void CaptionsPanel::syncDialog() {
    const CaptionTrack* t = track();
    const Sequence* s = state_->sequence();
    if (!t || !s || t->captions.size() < 2) {
        state_->message(tr("Syncing needs at least two captions"), 3000);
        return;
    }
    std::vector<size_t> rows = selectedCaptions();
    if (rows.size() < 2) rows = {0, t->captions.size() - 1};
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Sync to Two Points"));
    auto* form = new QFormLayout(&dlg);
    auto* help = new QLabel(tr("Say where two captions should start; the others are moved and stretched to match "
                               "(subtitles timed for another cut or frame rate). Tip: put the playhead on the line and "
                               "copy the timecode."), &dlg);
    help->setWordWrap(true);
    form->addRow(help);
    const Caption& a = t->captions[rows.front()];
    const Caption& b = t->captions[rows.back()];
    auto* first = new QLineEdit(QString::fromStdString(formatTimecode(a.start, s->fps)), &dlg);
    auto* last = new QLineEdit(QString::fromStdString(formatTimecode(b.start, s->fps)), &dlg);
    auto clip = [](const std::string& text) { return QString::fromStdString(text).replace('\n', ' ').left(40); };
    form->addRow(tr("\"%1\" starts at:").arg(clip(a.text)), first);
    form->addRow(tr("\"%1\" starts at:").arg(clip(b.text)), last);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return;
    FrameTime fa = 0, fb = 0;
    if (!parseTimecode(first->text().toStdString(), s->fps, fa) || !parseTimecode(last->text().toStdString(), s->fps, fb)) {
        state_->message(tr("Give both times as timecodes"));
        return;
    }
    syncToTwoPoints(fa, fb);
}

void CaptionsPanel::findReplaceDialog() {
    if (!track()) return;
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Find and Replace in Captions"));
    auto* form = new QFormLayout(&dlg);
    auto* find = new QLineEdit(&dlg);
    auto* with = new QLineEdit(&dlg);
    auto* matchCase = new QCheckBox(tr("Match case"), &dlg);
    auto* whole = new QCheckBox(tr("Whole words"), &dlg);
    form->addRow(tr("Find:"), find);
    form->addRow(tr("Replace with:"), with);
    form->addRow(QString(), matchCase);
    form->addRow(QString(), whole);
    form->addRow(new QLabel(selectedCaptions().empty() ? tr("In every caption of the track.")
                                                        : tr("In the %1 selected captions.").arg(selectedCaptions().size()),
                            &dlg));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Replace All"));
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted || find->text().isEmpty()) return;
    const int n = findReplace(find->text(), with->text(), matchCase->isChecked(), whole->isChecked());
    state_->message(n ? tr("Replaced %1").arg(n) : tr("\"%1\" was not found").arg(find->text()), 3000);
}

void CaptionsPanel::limitsDialog() {
    CaptionLimits l = limits();
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Reading Limits"));
    auto* form = new QFormLayout(&dlg);
    auto* preset = new QComboBox(&dlg);
    preset->addItems({tr("Custom"), tr("Netflix (adults)"), tr("Netflix (children)"), tr("BBC")});
    auto* cps = new QDoubleSpinBox(&dlg);
    cps->setRange(5, 40);
    cps->setDecimals(1);
    auto* chars = new QSpinBox(&dlg);
    chars->setRange(10, 80);
    auto* lines = new QSpinBox(&dlg);
    lines->setRange(1, 4);
    auto* minDur = new QDoubleSpinBox(&dlg);
    minDur->setRange(0.1, 5);
    minDur->setDecimals(2);
    minDur->setSuffix(tr(" s"));
    auto* maxDur = new QDoubleSpinBox(&dlg);
    maxDur->setRange(1, 30);
    maxDur->setDecimals(1);
    maxDur->setSuffix(tr(" s"));
    auto* gap = new QSpinBox(&dlg);
    gap->setRange(0, 25);
    gap->setSuffix(tr(" frames"));
    auto show = [&](const CaptionLimits& v) {
        cps->setValue(v.maxCps);
        chars->setValue(v.maxLineChars);
        lines->setValue(v.maxLines);
        minDur->setValue(v.minSeconds);
        maxDur->setValue(v.maxSeconds);
        gap->setValue(v.minGapFrames);
    };
    show(l);
    connect(preset, &QComboBox::activated, &dlg, [&](int i) {
        CaptionLimits v;  // Netflix adults by default
        if (i == 2) v.maxCps = 17;
        if (i == 3) v.maxCps = 15, v.maxLineChars = 37, v.minSeconds = 1.0, v.maxSeconds = 8.0;  // about 180 words a minute
        if (i > 0) show(v);
    });
    form->addRow(tr("Preset:"), preset);
    form->addRow(tr("Characters a second:"), cps);
    form->addRow(tr("Characters a line:"), chars);
    form->addRow(tr("Lines:"), lines);
    form->addRow(tr("Shortest on screen:"), minDur);
    form->addRow(tr("Longest on screen:"), maxDur);
    form->addRow(tr("Gap between captions:"), gap);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return;
    l.maxCps = cps->value();
    l.maxLineChars = chars->value();
    l.maxLines = lines->value();
    l.minSeconds = minDur->value();
    l.maxSeconds = maxDur->value();
    l.minGapFrames = gap->value();
    setLimits(l);
}

}  // namespace montage
