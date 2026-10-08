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
    menu->addAction(tr("Import SubRip / WebVTT..."), this, &CaptionsPanel::importDialog);
    menu->addAction(tr("Export Captions..."), this, &CaptionsPanel::exportDialog);
    menu->addAction(tr("Translate Track..."), this, &CaptionsPanel::translateDialog)->setObjectName(QStringLiteral("translateCaptions"));
    menu->addAction(tr("Dub into English..."), this, &CaptionsPanel::dubDialog)->setObjectName(QStringLiteral("dubCaptions"));
    menu->addSeparator();
    menu->addAction(tr("Style..."), this, &CaptionsPanel::styleDialog);
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
    table_->setColumnCount(3);
    table_->setHorizontalHeaderLabels({tr("In"), tr("Out"), tr("Text")});
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table_->verticalHeader()->hide();
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    table_->setWordWrap(true);
    lay->addWidget(table_, 1);

    auto* bottom = new QHBoxLayout;
    auto* add = button(this, tr("Add"), tr("Add a caption at the playhead"));
    auto* split = button(this, tr("Split"), tr("Split the caption under the playhead"));
    auto* merge = button(this, tr("Merge"), tr("Join the selected caption with the next one"));
    auto* del = button(this, tr("Delete"), tr("Delete the selected captions"));
    for (QToolButton* b : {add, split, merge, del}) bottom->addWidget(b);
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
    if (t && s) {
        table_->setRowCount(int(t->captions.size()));
        for (size_t i = 0; i < t->captions.size(); ++i) {
            const Caption& c = t->captions[i];
            auto* in = new QTableWidgetItem(QString::fromStdString(formatTimecode(c.start, s->fps)));
            auto* out = new QTableWidgetItem(QString::fromStdString(formatTimecode(c.end, s->fps)));
            auto* text = new QTableWidgetItem(QString::fromStdString(c.text));
            for (auto* item : {in, out, text}) item->setData(kIndexRole, int(i));
            table_->setItem(int(i), 0, in);
            table_->setItem(int(i), 1, out);
            table_->setItem(int(i), 2, text);
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
    const QString suffix = QFileInfo(path).suffix().toLower();
    const std::string text = suffix == "vtt"   ? captionsToVtt(t->captions, s->fps)
                             : suffix == "scc" ? captionsToScc(t->captions, s->fps)
                                               : captionsToSrt(t->captions, s->fps);
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
                                                      tr("Captions (*.srt *.vtt);;All files (*)"));
    if (path.isEmpty()) return;
    settings.setValue("captions/dir", QFileInfo(path).absolutePath());
    QString err;
    if (!importFile(path, &err)) QMessageBox::warning(this, tr("Import Captions"), err);
}

void CaptionsPanel::exportDialog() {
    const CaptionTrack* t = track();
    if (!t) return;
    QSettings settings = appSettings();
    const QString srt = tr("SubRip (*.srt)"), vtt = tr("WebVTT (*.vtt)"), scc = tr("Scenarist SCC, CEA-608 (*.scc)");
    QString filter = srt;
    QString path = QFileDialog::getSaveFileName(this, tr("Export Captions"),
                                                settings.value("captions/dir").toString() + "/" +
                                                    QString::fromStdString(t->name) + ".srt",
                                                QStringList{srt, vtt, scc}.join(";;"), &filter);
    if (path.isEmpty()) return;
    if (QFileInfo(path).suffix().isEmpty()) path += filter == vtt ? ".vtt" : filter == scc ? ".scc" : ".srt";
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
    form->addRow(tr("Bottom edge:"), position);
    // Word animation (from the words' timings when the captions came from a transcript).
    auto* animation = new QComboBox(&dlg);
    animation->setObjectName(QStringLiteral("captionAnimation"));
    animation->addItems({tr("None"), tr("Word by Word"), tr("Highlight the Spoken Word"), tr("Pop the Spoken Word"), tr("One Word at a Time")});
    animation->setCurrentIndex(std::clamp(st.animation, 0, 4));
    animation->setToolTip(tr("Animate captions word by word, as social video does"));
    form->addRow(tr("Animation:"), animation);
    form->addRow(tr("Highlight colour:"), colourButton(st.hiR, st.hiG, st.hiB));
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
        t.captions.push_back({at, end, tr("Caption").toStdString()});
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
        Caption second{at, c->end, words.mid(n).join(' ').toStdString()};
        t.captions[i].end = at;
        t.captions[i].text = wrapCaptionText(words.mid(0, n).join(' ').toStdString());
        second.text = wrapCaptionText(second.text);
        if (second.text.empty()) second.text = t.captions[i].text;
        t.captions.insert(t.captions.begin() + long(i) + 1, second);
        return true;
    });
}

}  // namespace montage
