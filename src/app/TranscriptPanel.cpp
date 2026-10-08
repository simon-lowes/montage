#include "TranscriptPanel.h"

#include <QComboBox>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSettings>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextEdit>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

#include "EditorState.h"
#include "Theme.h"
#include "core/History.h"
#include "core/Bleep.h"
#include "core/TranscriptEdit.h"

namespace montage {

namespace {

QToolButton* button(QWidget* parent, const QString& text, const QString& tip) {
    auto* b = new QToolButton(parent);
    b->setText(text);
    b->setToolTip(tip);
    b->setAutoRaise(true);
    return b;
}

QString normalized(const QString& s) {
    static const QRegularExpression re(QStringLiteral("[^\\w']"));
    return s.toLower().remove(re);
}

bool endsSentence(const std::string& w) { return !w.empty() && (w.back() == '.' || w.back() == '?' || w.back() == '!'); }

}  // namespace

TranscriptPanel::TranscriptPanel(EditorState* state, QWidget* parent) : QWidget(parent), state_(state) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(4, 4, 4, 4);
    auto* top = new QHBoxLayout;
    modeBox_ = new QComboBox(this);
    modeBox_->addItem(tr("Sequence"), int(Mode::Sequence));
    modeBox_->addItem(tr("Source Clip"), int(Mode::Source));
    modeBox_->setToolTip(tr("Show what the sequence says, or the transcript of the clip in the Source monitor"));
    top->addWidget(modeBox_);
    search_ = new QLineEdit(this);
    search_->setPlaceholderText(tr("Find in transcript"));
    search_->setClearButtonEnabled(true);
    top->addWidget(search_, 1);
    matches_ = new QLabel(this);
    top->addWidget(matches_);
    lay->addLayout(top);

    text_ = new QTextEdit(this);
    text_->setReadOnly(true);
    text_->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    text_->viewport()->installEventFilter(this);
    text_->installEventFilter(this);
    text_->viewport()->setCursor(Qt::IBeamCursor);
    lay->addWidget(text_, 1);

    auto* bottom = new QHBoxLayout;
    deleteBtn_ = button(this, tr("Delete"), tr("Cut the selected words out of the sequence and close the gap (Delete)"));
    fillersBtn_ = button(this, tr("Remove Fillers"), tr("Cut out um, uh, er and similar filler words"));
    retakesBtn_ = button(this, tr("Remove Retakes"), tr("Where the speaker broke off and started again, keep only the last take"));
    retakesBtn_->setObjectName(QStringLiteral("removeRetakes"));
    pausesBtn_ = button(this, tr("Shorten Pauses..."), tr("Shorten silences between words"));
    bleepBtn_ = button(this, tr("Bleep"), tr("Cover words with a bleep tone, and mask them in the captions"));
    bleepBtn_->setObjectName(QStringLiteral("bleepButton"));
    {
        auto* menu = new QMenu(bleepBtn_);
        QAction* sel = menu->addAction(tr("Bleep Selected Words"), this, &TranscriptPanel::bleepSelection);
        sel->setObjectName(QStringLiteral("bleepSelection"));
        QAction* swear = menu->addAction(tr("Bleep Profanity"), this, &TranscriptPanel::bleepProfanity);
        swear->setObjectName(QStringLiteral("bleepProfanity"));
        connect(menu, &QMenu::aboutToShow, this, [this, sel, swear] {
            sel->setEnabled(selectedWords().first >= 0);
            const int n = int(profanity(words_).size());
            swear->setText(n ? tr("Bleep Profanity (%n word(s))", "", n) : tr("Bleep Profanity (none found)"));
            swear->setEnabled(n > 0);
        });
        bleepBtn_->setMenu(menu);
        bleepBtn_->setPopupMode(QToolButton::InstantPopup);
    }
    insertBtn_ = button(this, tr("Insert"), tr("Insert the selected words into the timeline at the playhead"));
    overwriteBtn_ = button(this, tr("Overwrite"), tr("Overwrite the timeline at the playhead with the selected words"));
    smoothBtn_ = button(this, tr("Smooth Cuts"),
                        tr("Put a Smooth Cut at each join that Delete, Remove Fillers and Shorten Pauses leave,\n"
                           "so the jump in the picture morphs across instead of cutting"));
    smoothBtn_->setObjectName(QStringLiteral("smoothCuts"));
    smoothBtn_->setCheckable(true);
    smoothBtn_->setChecked(QSettings().value(QStringLiteral("transcript/smoothCuts"), false).toBool());
    connect(smoothBtn_, &QToolButton::toggled, this, [](bool on) { QSettings().setValue(QStringLiteral("transcript/smoothCuts"), on); });
    paperAddBtn_ = button(this, tr("Add to Paper Edit"), tr("Add the selected words to the Paper Edit list below; lines can come from any clip"));
    paperAddBtn_->setObjectName(QStringLiteral("paperAdd"));
    paperBuildBtn_ = button(this, tr("Assemble"), tr("Lay the Paper Edit's lines out, in the list's order, as a new sequence"));
    paperBuildBtn_->setObjectName(QStringLiteral("paperAssemble"));
    for (QToolButton* b : {deleteBtn_, fillersBtn_, retakesBtn_, pausesBtn_, bleepBtn_, insertBtn_, overwriteBtn_, paperAddBtn_, paperBuildBtn_})
        bottom->addWidget(b);
    bottom->addStretch();
    bottom->addWidget(smoothBtn_);
    lay->addLayout(bottom);
    // The Paper Edit: lines in order, dragged to reorder, Delete to remove.
    paperList_ = new QListWidget(this);
    paperList_->setObjectName(QStringLiteral("paperEdit"));
    paperList_->setDragDropMode(QAbstractItemView::InternalMove);
    paperList_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    paperList_->setMaximumHeight(120);
    paperList_->setToolTip(tr("Paper Edit: drag lines to reorder them, Delete removes them"));
    paperList_->installEventFilter(this);
    lay->addWidget(paperList_);
    status_ = new QLabel(this);
    status_->setStyleSheet(QStringLiteral("color: palette(mid);"));
    status_->setWordWrap(true);
    lay->addWidget(status_);

    connect(modeBox_, &QComboBox::activated, this, [this](int i) { setMode(Mode(modeBox_->itemData(i).toInt())); });
    connect(search_, &QLineEdit::textChanged, this, [this](const QString& t) { find(t); });
    connect(search_, &QLineEdit::returnPressed, this, [this] {
        find(search_->text(), QGuiApplication::keyboardModifiers() & Qt::ShiftModifier);
    });
    connect(deleteBtn_, &QToolButton::clicked, this, &TranscriptPanel::deleteSelection);
    connect(fillersBtn_, &QToolButton::clicked, this, &TranscriptPanel::removeFillerWords);
    connect(retakesBtn_, &QToolButton::clicked, this, &TranscriptPanel::removeRetakes);
    connect(pausesBtn_, &QToolButton::clicked, this, [this] {
        QDialog dlg(this);
        dlg.setWindowTitle(tr("Shorten Pauses"));
        auto* form = new QFormLayout(&dlg);
        auto* longer = new QDoubleSpinBox(&dlg);
        longer->setRange(0.2, 10);
        longer->setSingleStep(0.1);
        longer->setSuffix(tr(" s"));
        longer->setValue(1.0);
        auto* keep = new QDoubleSpinBox(&dlg);
        keep->setRange(0, 5);
        keep->setSingleStep(0.05);
        keep->setSuffix(tr(" s"));
        keep->setValue(0.3);
        form->addRow(tr("Pauses longer than:"), longer);
        form->addRow(tr("Shorten them to:"), keep);
        auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
        connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        form->addRow(box);
        if (dlg.exec() == QDialog::Accepted) removePauses(longer->value(), std::min(keep->value(), longer->value()));
    });
    connect(paperAddBtn_, &QToolButton::clicked, this, [this] { addToPaperEdit(); });
    connect(paperBuildBtn_, &QToolButton::clicked, this, [this] { assemblePaperEdit(); });
    connect(insertBtn_, &QToolButton::clicked, this, [this] { insertSelection(false); });
    connect(overwriteBtn_, &QToolButton::clicked, this, [this] { insertSelection(true); });
    connect(text_, &QTextEdit::selectionChanged, this, [this] {
        if (mode_ == Mode::Source) {
            const auto [a, b] = selectedWords();
            insertBtn_->setEnabled(a >= 0);
            overwriteBtn_->setEnabled(a >= 0);
        } else {
            deleteBtn_->setEnabled(selectedWords().first >= 0);
        }
    });
    connect(state_, &EditorState::projectChanged, this, &TranscriptPanel::rebuild);
    connect(state_, &EditorState::sequenceSwitched, this, &TranscriptPanel::rebuild);
    connect(state_, &EditorState::sourceChanged, this, [this] {
        if (mode_ == Mode::Source) rebuild();
    });
    connect(state_, &EditorState::playheadChanged, this, [this](FrameTime t) {
        if (mode_ != Mode::Sequence) return;
        const double sec = double(t) / fps();
        int cur = -1;
        for (size_t i = 0; i < words_.size(); ++i)
            if (sec >= words_[i].start && sec < std::max(words_[i].end, words_[i].start + 0.05)) cur = int(i);
        if (cur != current_) {
            current_ = cur;
            refreshHighlights();
        }
    });
    setMode(Mode::Sequence);
}

double TranscriptPanel::fps() const {
    const Sequence* s = state_->sequence();
    return s && s->fpsValue() > 0 ? s->fpsValue() : 30.0;
}

void TranscriptPanel::setMode(Mode m) {
    mode_ = m;
    modeBox_->setCurrentIndex(m == Mode::Sequence ? 0 : 1);
    const bool seq = m == Mode::Sequence;
    for (QToolButton* b : {deleteBtn_, fillersBtn_, retakesBtn_, pausesBtn_, bleepBtn_}) b->setVisible(seq);
    for (QToolButton* b : {insertBtn_, overwriteBtn_, paperAddBtn_, paperBuildBtn_}) b->setVisible(!seq);
    paperList_->setVisible(!seq && paperList_->count() > 0);
    signature_.clear();
    current_ = -1;
    rebuild();
}

void TranscriptPanel::rebuild() {
    std::vector<TranscriptWord> words;
    QString empty;
    if (mode_ == Mode::Sequence) {
        if (const Sequence* s = state_->sequence()) words = sequenceTranscriptWords(state_->project(), *s);
        empty = tr("Nothing in this sequence has been transcribed. Right-click clips in the Media panel and choose "
                   "Transcribe..., then edit the cut by editing its text here.");
    } else {
        const MediaItem* m = state_->project().findMedia(state_->sourceMedia());
        if (m && m->transcript)
            for (const auto& seg : m->transcript->segments)
                for (TranscriptWord w : seg.words) {
                    w.speaker = speakerName(*m->transcript, seg.speaker);
                    words.push_back(std::move(w));
                }
        empty = !m ? tr("Open a clip in the Source monitor to see its transcript.")
                   : tr("\"%1\" has not been transcribed. Right-click it in the Media panel and choose Transcribe...")
                         .arg(QString::fromStdString(m->name));
    }
    // Rebuild the text only when the words or their timing changed.
    QString sig = QString::number(int(mode_)) + QString::number(qulonglong(words.size()));
    for (const auto& w : words) sig += QString::fromStdString(w.text + w.speaker) + QString::number(std::lround(w.start * 100));
    if (sig == signature_) return;
    signature_ = sig;
    words_ = std::move(words);
    spans_.clear();
    found_.clear();
    const int scroll = text_->verticalScrollBar()->value();
    text_->clear();
    text_->setPlaceholderText(empty);
    QTextCursor cur(text_->document());
    QTextCharFormat normal, label, filler, unsure, who;
    label.setForeground(palette().color(QPalette::Disabled, QPalette::Text));
    who.setFontWeight(QFont::Bold);
    who.setFontPointSize(text_->font().pointSizeF() * 0.85);
    label.setFontPointSize(text_->font().pointSizeF() * 0.85);
    filler.setForeground(palette().color(QPalette::Disabled, QPalette::Text));
    filler.setFontItalic(true);
    unsure.setUnderlineStyle(QTextCharFormat::DotLine);
    unsure.setUnderlineColor(QColor(0xd9, 0x9a, 0x2b));
    int paragraphChars = 0, fillers = 0, uncertain = 0;
    Rational rate{int(std::lround(fps() * 1000)), 1000};
    if (const Sequence* s = state_->sequence()) rate = s->fps;
    for (size_t i = 0; i < words_.size(); ++i) {
        const TranscriptWord& w = words_[i];
        const bool newSpeaker = i > 0 && w.speaker != words_[i - 1].speaker;
        const bool newParagraph = i == 0 || newSpeaker || w.start - words_[i - 1].end > 1.5 ||
                                  (endsSentence(words_[i - 1].text) && paragraphChars > 320);
        if (newParagraph) {
            if (i > 0) cur.insertBlock();
            // Who speaks, at each change of speaker.
            if (!w.speaker.empty() && (i == 0 || newSpeaker)) {
                cur.insertText(QString::fromStdString(w.speaker), who);
                cur.insertText(QStringLiteral("  "), normal);
            }
            const QString tc = QString::fromStdString(formatTimecode(FrameTime(std::llround(w.start * fps())), rate));
            spans_.push_back({cur.position(), int(tc.size()), -1, w.start});
            cur.insertText(tc, label);
            cur.insertText(QStringLiteral("  "), normal);
            paragraphChars = 0;
        } else {
            cur.insertText(QStringLiteral(" "), normal);
        }
        const QString t = QString::fromStdString(w.text);
        const bool isFiller = isFillerWord(w.text);
        fillers += isFiller ? 1 : 0;
        uncertain += w.probability < 0.4f ? 1 : 0;
        spans_.push_back({cur.position(), int(t.size()), int(i), w.start});
        cur.insertText(t, isFiller ? filler : w.probability < 0.4f ? unsure : normal);
        paragraphChars += int(t.size()) + 1;
    }
    text_->verticalScrollBar()->setValue(scroll);
    if (words_.empty()) status_->clear();
    else if (mode_ == Mode::Sequence)
        status_->setText(tr("%n word(s)", "", int(words_.size())) +
                         (fillers ? tr(", %n filler word(s)", "", fillers) : QString()) +
                         (uncertain ? tr(", %n uncertain (dotted)", "", uncertain) : QString()) +
                         tr(". Click a word to go there; select words and press Delete to cut them."));
    else
        status_->setText(tr("Select words, then Insert or Overwrite them into the timeline."));
    deleteBtn_->setEnabled(false);
    insertBtn_->setEnabled(false);
    overwriteBtn_->setEnabled(false);
    fillersBtn_->setEnabled(fillers > 0);
    retakesBtn_->setEnabled(mode_ == Mode::Sequence && !retakeRanges(words_, fps()).empty());
    pausesBtn_->setEnabled(words_.size() > 1);
    if (!search_->text().isEmpty()) find(search_->text());
    refreshHighlights();
}

void TranscriptPanel::refreshHighlights() {
    QList<QTextEdit::ExtraSelection> sel;
    auto wordRange = [this](int first, int last, const QColor& bg) {
        QTextEdit::ExtraSelection es;
        QTextCursor c(text_->document());
        int a = -1, b = -1;
        for (const Span& s : spans_)
            if (s.word == first) a = s.pos;
        for (const Span& s : spans_)
            if (s.word == last) b = s.pos + s.len;
        if (a < 0 || b < 0) return es;
        c.setPosition(a);
        c.setPosition(b, QTextCursor::KeepAnchor);
        es.cursor = c;
        es.format.setBackground(bg);
        return es;
    };
    for (const auto& [a, b] : found_) sel << wordRange(a, b, QColor(0xd9, 0x9a, 0x2b, 110));
    if (current_ >= 0) sel << wordRange(current_, current_, QColor(theme::kAccent.red(), theme::kAccent.green(), theme::kAccent.blue(), 120));
    text_->setExtraSelections(sel);
}

int TranscriptPanel::wordAtPosition(int pos) const {
    auto it = std::upper_bound(spans_.begin(), spans_.end(), pos, [](int p, const Span& s) { return p < s.pos; });
    if (it == spans_.begin()) return -2;
    --it;
    if (pos > it->pos + it->len) return -2;
    return int(it - spans_.begin());
}

void TranscriptPanel::selectWords(int first, int last) {
    if (first < 0 || last < first || size_t(last) >= words_.size()) return;
    int a = -1, b = -1;
    for (const Span& s : spans_) {
        if (s.word == first) a = s.pos;
        if (s.word == last) b = s.pos + s.len;
    }
    if (a < 0 || b < 0) return;
    QTextCursor c(text_->document());
    c.setPosition(a);
    c.setPosition(b, QTextCursor::KeepAnchor);
    text_->setTextCursor(c);
    text_->ensureCursorVisible();
}

std::pair<int, int> TranscriptPanel::selectedWords() const {
    const QTextCursor c = text_->textCursor();
    if (!c.hasSelection()) return {-1, -1};
    const int a = c.selectionStart(), b = c.selectionEnd();
    int first = -1, last = -1;
    for (const Span& s : spans_) {
        if (s.word < 0 || s.pos + s.len <= a || s.pos >= b) continue;
        if (first < 0) first = s.word;
        last = s.word;
    }
    return {first, last};
}

int TranscriptPanel::find(const QString& textIn, bool backwards) {
    found_.clear();
    QStringList want;
    for (const QString& p : textIn.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts))
        if (const QString n = normalized(p); !n.isEmpty()) want << n;
    if (!want.isEmpty()) {
        std::vector<QString> norm;
        norm.reserve(words_.size());
        for (const auto& w : words_) norm.push_back(normalized(QString::fromStdString(w.text)));
        for (size_t i = 0; i + size_t(want.size()) <= norm.size(); ++i) {
            bool match = true;
            for (int k = 0; k < want.size() && match; ++k) {
                // The last word may be partly typed.
                const QString& n = norm[i + size_t(k)];
                match = k + 1 == want.size() ? n.startsWith(want[k]) : n == want[k];
            }
            if (match) found_.emplace_back(int(i), int(i) + int(want.size()) - 1);
        }
    }
    matches_->setText(want.isEmpty() ? QString() : tr("%n match(es)", "", int(found_.size())));
    refreshHighlights();
    if (found_.empty()) return 0;
    // Select the next (or previous) match after the current selection.
    const int from = selectedWords().first;
    auto pick = found_.front();
    if (backwards) {
        pick = found_.back();
        for (auto it = found_.rbegin(); it != found_.rend(); ++it)
            if (it->first < from) {
                pick = *it;
                break;
            }
    } else {
        for (const auto& f : found_)
            if (f.first > from) {
                pick = f;
                break;
            }
    }
    selectWords(pick.first, pick.second);
    return int(found_.size());
}

bool TranscriptPanel::renameSpeaker(const QString& from, const QString& to) {
    const Id media = state_->sourceMedia();
    const std::string a = from.toStdString(), b = to.trimmed().toStdString();
    if (mode_ != Mode::Source || !media || b.empty() || a == b) return false;
    return state_->edit(tr("Rename Speaker"), [media, a, b](Project& p, Sequence&) {
        MediaItem* m = p.findMedia(media);
        if (!m || !m->transcript) return false;
        for (int i = 0; i < speakerCount(*m->transcript); ++i)
            if (speakerName(*m->transcript, i) == a) {
                auto t = std::make_shared<Transcript>(*m->transcript);
                t->speakerNames.resize(std::max(t->speakerNames.size(), size_t(i) + 1));
                t->speakerNames[size_t(i)] = b;
                m->transcript = t;
                return true;
            }
        return false;
    });
}

bool TranscriptPanel::eventFilter(QObject* obj, QEvent* e) {
    if (obj == paperList_ && e->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent*>(e)->key();
        if (key == Qt::Key_Delete || key == Qt::Key_Backspace) {
            for (QListWidgetItem* item : paperList_->selectedItems()) delete item;
            if (paperList_->count() == 0) paperList_->setVisible(false);
            return true;
        }
    }
    if (obj == text_->viewport() && e->type() == QEvent::ContextMenu && mode_ == Mode::Source) {
        auto* ce = static_cast<QContextMenuEvent*>(e);
        const int i = wordAtPosition(text_->cursorForPosition(ce->pos()).position());
        const int w = i >= 0 ? spans_[size_t(i)].word : -1;
        if (w >= 0 && !words_[size_t(w)].speaker.empty()) {
            const QString name = QString::fromStdString(words_[size_t(w)].speaker);
            QMenu menu(this);
            QAction* rename = menu.addAction(tr("Rename Speaker \u201c%1\u201d…").arg(name));
            if (menu.exec(ce->globalPos()) == rename) {
                bool ok = false;
                const QString to = QInputDialog::getText(this, tr("Rename Speaker"), tr("Name:"), QLineEdit::Normal, name, &ok);
                if (ok) renameSpeaker(name, to);
            }
            return true;
        }
    }
    if (obj == text_->viewport() && e->type() == QEvent::MouseButtonRelease) {
        auto* me = static_cast<QMouseEvent*>(e);
        if (me->button() == Qt::LeftButton && !text_->textCursor().hasSelection()) {
            const int i = wordAtPosition(text_->cursorForPosition(me->pos()).position());
            if (i >= 0) {
                const double sec = spans_[size_t(i)].seconds;
                const FrameTime f = FrameTime(std::llround(sec * fps()));
                if (mode_ == Mode::Sequence) state_->setPlayhead(f);
                else emit sourceSeekRequested(f);
            }
        }
    } else if (obj == text_ && e->type() == QEvent::KeyPress && mode_ == Mode::Sequence) {
        auto* ke = static_cast<QKeyEvent*>(e);
        if (ke->key() == Qt::Key_Delete || ke->key() == Qt::Key_Backspace) {
            deleteSelection();
            return true;
        }
    }
    return QWidget::eventFilter(obj, e);
}

void TranscriptPanel::deleteSelection() {
    if (mode_ != Mode::Sequence) return;
    const auto [first, last] = selectedWords();
    if (first < 0) return;
    const double f = fps();
    // From the first word to the next word, so the pause after the cut goes too
    // (unless that pause is long; then up to just after the last word).
    const double a = words_[size_t(first)].start;
    double b = words_[size_t(last)].end + 0.1;
    if (size_t(last) + 1 < words_.size()) {
        const double next = words_[size_t(last) + 1].start;
        b = next - b < 0.5 ? next : std::min(b, next);
    }
    const FrameRange range{FrameTime(std::llround(a * f)), FrameTime(std::llround(b * f))};
    const int n = last - first + 1;
    if (state_->apply(tr("Delete Words"), [range, smooth = smoothCutFrames()](Project& p, Sequence& s) { return rippleDeleteRanges(p, s, {range}, smooth); }))
        state_->message(tr("Cut %n word(s)", "", n));
}

void TranscriptPanel::bleepSelection() {
    if (mode_ != Mode::Sequence) return;
    const auto [first, last] = selectedWords();
    if (first < 0) return;
    const std::vector<TranscriptWord> chosen(words_.begin() + first, words_.begin() + last + 1);
    QString why;
    if (state_->apply(tr("Bleep Words"), [chosen, &why](Project& p, Sequence& s) {
            edit::Result r = bleepWords(p, s, chosen);
            if (!r.ok) why = QString::fromStdString(r.error);
            return r;
        }))
        state_->message(tr("Bleeped %n word(s)", "", int(chosen.size())));
    else if (!why.isEmpty())
        state_->message(why);
}

void TranscriptPanel::bleepProfanity() {
    if (mode_ != Mode::Sequence) return;
    const std::vector<TranscriptWord> chosen = profanity(words_);
    if (chosen.empty()) {
        state_->message(tr("No profanity found"));
        return;
    }
    if (state_->apply(tr("Bleep Profanity"), [chosen](Project& p, Sequence& s) { return bleepWords(p, s, chosen); }))
        state_->message(tr("Bleeped %n word(s)", "", int(chosen.size())));
}

FrameTime TranscriptPanel::smoothCutFrames() const {
    return smoothBtn_->isChecked() ? std::max<FrameTime>(2, FrameTime(std::lround(fps() * 0.2))) : 0;
}

void TranscriptPanel::removeFillerWords() {
    if (mode_ != Mode::Sequence) return;
    const auto ranges = fillerWordRanges(words_, fps());
    if (ranges.empty()) {
        state_->message(tr("No filler words found"));
        return;
    }
    const int n = int(std::count_if(words_.begin(), words_.end(), [](const TranscriptWord& w) { return isFillerWord(w.text); }));
    if (state_->apply(tr("Remove Filler Words"), [ranges, smooth = smoothCutFrames()](Project& p, Sequence& s) { return rippleDeleteRanges(p, s, ranges, smooth); }))
        state_->message(tr("Removed %n filler word(s)", "", n));
}

void TranscriptPanel::removeRetakes() {
    if (mode_ != Mode::Sequence) return;
    std::vector<std::pair<size_t, size_t>> takes;
    const auto ranges = retakeRanges(words_, fps(), 3, 30, &takes);
    if (ranges.empty()) {
        state_->message(tr("No retakes found"));
        return;
    }
    if (state_->apply(tr("Remove Retakes"), [ranges, smooth = smoothCutFrames()](Project& p, Sequence& s) { return rippleDeleteRanges(p, s, ranges, smooth); }))
        state_->message(tr("Removed %n broken-off take(s)", "", int(takes.size())));
}

void TranscriptPanel::removePauses(double minPause, double keep) {
    if (mode_ != Mode::Sequence) return;
    const auto ranges = pauseRanges(words_, fps(), minPause, keep);
    if (ranges.empty()) {
        state_->message(tr("No pauses longer than %1 s").arg(minPause));
        return;
    }
    FrameTime total = 0;
    for (const auto& r : ranges) total += r.second - r.first;
    if (state_->apply(tr("Shorten Pauses"), [ranges, smooth = smoothCutFrames()](Project& p, Sequence& s) { return rippleDeleteRanges(p, s, ranges, smooth); }))
        state_->message(tr("Shortened %n pause(s), %1 s shorter", "", int(ranges.size())).arg(double(total) / fps(), 0, 'f', 1));
}

void TranscriptPanel::markSelection() {
    if (mode_ != Mode::Source) return;
    const auto [first, last] = selectedWords();
    if (first < 0) return;
    const double f = fps();
    const FrameTime in = FrameTime(std::floor(words_[size_t(first)].start * f));
    const FrameTime out = std::max(in, FrameTime(std::ceil(words_[size_t(last)].end * f)) - 1);
    state_->setSourceOut(-1);
    state_->setSourceIn(in);
    state_->setSourceOut(out);
    emit sourceSeekRequested(in);
}

void TranscriptPanel::insertSelection(bool overwrite) {
    if (mode_ != Mode::Source || selectedWords().first < 0) return;
    markSelection();
    state_->insertFromSource(overwrite);
}

int TranscriptPanel::addToPaperEdit() {
    if (mode_ != Mode::Source) return paperList_->count();
    const auto [first, last] = selectedWords();
    const Id media = state_->sourceMedia();
    const MediaItem* m = state_->project().findMedia(media);
    if (first < 0 || !m) {
        state_->message(tr("Select the words to add in the source clip's transcript"));
        return paperList_->count();
    }
    QString text;
    for (int i = first; i <= last; ++i) text += (i > first ? " " : "") + QString::fromStdString(words_[size_t(i)].text);
    auto* item = new QListWidgetItem(QStringLiteral("%1 — %2").arg(QString::fromStdString(m->name), text), paperList_);
    item->setData(Qt::UserRole, QVariant::fromValue<qlonglong>(qlonglong(media)));
    item->setData(Qt::UserRole + 1, words_[size_t(first)].start);
    item->setData(Qt::UserRole + 2, words_[size_t(last)].end);
    item->setData(Qt::UserRole + 3, text);
    item->setToolTip(text);
    paperList_->setVisible(true);
    status_->setText(tr("Paper Edit: %n line(s). Drag to reorder, then Assemble.", "", paperList_->count()));
    return paperList_->count();
}

std::vector<PaperLine> TranscriptPanel::paperEdit() const {
    std::vector<PaperLine> lines;
    for (int i = 0; i < paperList_->count(); ++i) {
        const QListWidgetItem* item = paperList_->item(i);
        PaperLine l;
        l.media = Id(item->data(Qt::UserRole).toLongLong());
        l.in = item->data(Qt::UserRole + 1).toDouble();
        l.out = item->data(Qt::UserRole + 2).toDouble();
        l.text = item->data(Qt::UserRole + 3).toString().toStdString();
        lines.push_back(l);
    }
    return lines;
}

void TranscriptPanel::clearPaperEdit() {
    paperList_->clear();
    paperList_->setVisible(false);
}

Id TranscriptPanel::assemblePaperEdit(const QString& name) {
    const std::vector<PaperLine> lines = paperEdit();
    if (lines.empty()) {
        state_->message(tr("Add lines to the Paper Edit first: select words in a source clip's transcript"));
        return 0;
    }
    Id made = 0;
    const std::string title = (name.isEmpty() ? tr("Paper Edit") : name).toStdString();
    state_->edit(tr("Assemble Paper Edit"), [&](Project& p, Sequence&) {
        made = makePaperEdit(p, lines, title);
        return made != 0;
    });
    if (made) {
        state_->setActiveSequence(made);
        state_->message(tr("Assembled %n line(s) into a new sequence", "", int(lines.size())));
    }
    return made;
}

void TranscriptPanel::setSourcePosition(FrameTime frame) {
    sourcePosition_ = frame;
    if (mode_ != Mode::Source) return;
    const double sec = double(frame) / fps();
    int cur = -1;
    for (size_t i = 0; i < words_.size(); ++i)
        if (sec >= words_[i].start && sec < std::max(words_[i].end, words_[i].start + 0.05)) cur = int(i);
    if (cur != current_) {
        current_ = cur;
        refreshHighlights();
    }
}

}  // namespace montage
