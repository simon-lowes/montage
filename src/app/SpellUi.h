// Montage — spell checking as you type (core/SpellCheck.h): red wavy underlines under misspelt words in caption and
// title text, and on right-click the suggestions, Add to Dictionary (the project's vocabulary, one undo step) and the
// dictionary in use. Edit › Spelling turns it off or picks US or UK English for titles; captions use their track's
// language.
#pragma once

#include <QStyledItemDelegate>
#include <QSyntaxHighlighter>
#include <functional>
#include <string>
#include <vector>

class QMenu;
class QPlainTextEdit;
class QTextCursor;

namespace montage {

class EditorState;
class SpellChecker;

// Edit › Spelling: whether to check as you type, and the dictionary for titles ("en-US" or "en-GB"; by default the
// system's English, else US).
bool spellCheckingOn();
void setSpellCheckingOn(bool on);
QString titleSpellingLanguage();
void setTitleSpellingLanguage(const QString& language);

class SpellHighlighter : public QSyntaxHighlighter {
    Q_OBJECT
public:
    // `language` gives the dictionary's language each time a block is checked (an empty checker: nothing marked).
    SpellHighlighter(QTextDocument* doc, EditorState* state, std::function<QString()> language);

protected:
    void highlightBlock(const QString& text) override;

private:
    EditorState* state_;
    std::function<QString()> language_;
};

// The checker spelling is on with for `language`, or null (off, or no dictionary).
const SpellChecker* activeChecker(const QString& language);

// At the top of `menu`: the suggestions for the misspelt word under `cursor` (replacing it in `edit`) and Add to
// Dictionary. False when the word there is spelt right (nothing added).
bool addSpellingActions(QMenu* menu, QPlainTextEdit* edit, const QTextCursor& cursor, EditorState* state, const QString& language);

// Spell checking on a plain text editor: the highlighter and the right-click menu.
void enableSpellCheck(QPlainTextEdit* edit, EditorState* state, std::function<QString()> language);

// Learns a word for the project (Add to Dictionary), one undo step.
bool learnSpelling(EditorState* state, const QString& word);

// Table cells of text drawn with their misspelt words underlined (the row's language from `language`).
class SpellDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    SpellDelegate(EditorState* state, std::function<QString(const QModelIndex&)> language, QObject* parent = nullptr);
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;

private:
    EditorState* state_;
    std::function<QString(const QModelIndex&)> language_;
};

}  // namespace montage
