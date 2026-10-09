#include "SpellUi.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QLocale>
#include <QMenu>
#include <QPainter>
#include <QPlainTextEdit>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

#include "EditorState.h"
#include "Settings.h"
#include "core/SpellCheck.h"

namespace montage {

namespace {

QTextCharFormat misspeltFormat() {
    QTextCharFormat f;
    f.setUnderlineStyle(QTextCharFormat::SpellCheckUnderline);
    f.setUnderlineColor(QColor(235, 70, 60));
    return f;
}

}  // namespace

bool spellCheckingOn() { return appSettings().value(QStringLiteral("spelling/enabled"), true).toBool(); }
void setSpellCheckingOn(bool on) { appSettings().setValue(QStringLiteral("spelling/enabled"), on); }

QString titleSpellingLanguage() {
    const QString system = QLocale::system().name().replace('_', '-');
    const QString fallback = SpellChecker::dictionaryFor(system.toStdString()) == "en-GB" ? QStringLiteral("en-GB") : QStringLiteral("en-US");
    return appSettings().value(QStringLiteral("spelling/language"), fallback).toString();
}
void setTitleSpellingLanguage(const QString& language) { appSettings().setValue(QStringLiteral("spelling/language"), language); }

const SpellChecker* activeChecker(const QString& language) {
    if (!spellCheckingOn()) return nullptr;
    return SpellChecker::forLanguage(language.toStdString());
}

SpellHighlighter::SpellHighlighter(QTextDocument* doc, EditorState* state, std::function<QString()> language)
    : QSyntaxHighlighter(doc), state_(state), language_(std::move(language)) {
    // A word learned (or forgotten) or the dictionary changed: checked again.
    connect(state_, &EditorState::projectChanged, this, [this] { rehighlight(); });
}

void SpellHighlighter::highlightBlock(const QString& text) {
    const SpellChecker* sc = activeChecker(language_());
    if (!sc) return;
    const QTextCharFormat f = misspeltFormat();
    for (const Misspelling& m : sc->check(text, state_->project().vocabulary)) setFormat(m.start, m.length, f);
}

bool learnSpelling(EditorState* state, const QString& word) {
    const std::string w = word.toStdString();
    return state->edit(QObject::tr("Add to Dictionary"), [w](Project& p, Sequence&) { return learnWord(p, w); });
}

bool addSpellingActions(QMenu* menu, QPlainTextEdit* edit, const QTextCursor& at, EditorState* state, const QString& language) {
    const SpellChecker* sc = activeChecker(language);
    if (!sc) return false;
    // The misspelling the cursor is in, if any.
    const QTextBlock block = at.block();
    const int pos = at.position() - block.position();
    for (const Misspelling& m : sc->check(block.text(), state->project().vocabulary)) {
        if (pos < m.start || pos > m.start + m.length) continue;
        QAction* first = menu->actions().isEmpty() ? nullptr : menu->actions().front();
        const std::vector<std::string> suggestions = sc->suggest(m.word);
        QList<QAction*> added;
        for (const std::string& s : suggestions) {
            auto* a = new QAction(QString::fromStdString(s), menu);
            QFont bold = a->font();
            bold.setBold(true);
            a->setFont(bold);
            const int from = block.position() + m.start, len = m.length;
            QObject::connect(a, &QAction::triggered, edit, [edit, from, len, s] {
                QTextCursor c(edit->document());
                c.setPosition(from);
                c.setPosition(from + len, QTextCursor::KeepAnchor);
                c.insertText(QString::fromStdString(s));
            });
            added << a;
        }
        if (suggestions.empty()) {
            auto* none = new QAction(QObject::tr("No suggestions"), menu);
            none->setEnabled(false);
            added << none;
        }
        const QString word = QString::fromStdString(m.word);
        auto* learn = new QAction(QObject::tr("Add \"%1\" to Dictionary").arg(word), menu);
        learn->setObjectName(QStringLiteral("learnSpelling"));
        QObject::connect(learn, &QAction::triggered, state, [state, word] { learnSpelling(state, word); });
        added << learn;
        menu->insertActions(first, added);
        menu->insertSeparator(first);
        return true;
    }
    return false;
}

void enableSpellCheck(QPlainTextEdit* edit, EditorState* state, std::function<QString()> language) {
    new SpellHighlighter(edit->document(), state, language);
    edit->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(edit, &QWidget::customContextMenuRequested, edit, [edit, state, language](const QPoint& p) {
        QMenu* menu = edit->createStandardContextMenu(p);
        addSpellingActions(menu, edit, edit->cursorForPosition(p), state, language());
        menu->setAttribute(Qt::WA_DeleteOnClose);
        menu->popup(edit->mapToGlobal(p));
    });
}

SpellDelegate::SpellDelegate(EditorState* state, std::function<QString(const QModelIndex&)> language, QObject* parent)
    : QStyledItemDelegate(parent), state_(state), language_(std::move(language)) {}

void SpellDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const {
    const QString text = index.data(Qt::DisplayRole).toString();
    const SpellChecker* sc = activeChecker(language_(index));
    const std::vector<Misspelling> bad = sc ? sc->check(text, state_->project().vocabulary) : std::vector<Misspelling>{};
    if (bad.empty()) {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }
    // The cell without its text, then the text with the misspelt words underlined.
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    opt.text.clear();
    const QWidget* w = opt.widget;
    QStyle* style = w ? w->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, w);
    const QRect r = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, w).adjusted(2, 0, -2, 0);
    QTextDocument doc;
    doc.setDefaultFont(opt.font);
    doc.setDocumentMargin(0);
    doc.setPlainText(text);
    doc.setTextWidth(r.width());
    for (const Misspelling& m : bad) {
        QTextCursor c(&doc);
        c.setPosition(m.start);
        c.setPosition(m.start + m.length, QTextCursor::KeepAnchor);
        c.mergeCharFormat(misspeltFormat());
    }
    painter->save();
    painter->translate(r.left(), r.top() + std::max(0.0, (r.height() - doc.size().height()) / 2));
    painter->setClipRect(QRect(0, 0, r.width(), r.height()));
    QAbstractTextDocumentLayout::PaintContext ctx;
    ctx.palette = opt.palette;
    ctx.palette.setColor(QPalette::Text, opt.state & QStyle::State_Selected ? opt.palette.color(QPalette::HighlightedText)
                                                                            : opt.palette.color(QPalette::Text));
    doc.documentLayout()->draw(painter, ctx);
    painter->restore();
}

}  // namespace montage
