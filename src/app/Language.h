// Montage — the interface in other languages: Window › Language, Montage's translations (translations/montage_*.ts,
// built in), Qt's own for its dialogs, and right-to-left layout for Arabic, Hebrew and Persian. A language takes effect
// at the next start, where main() installs it before the window is built.
//
// The test language ("pseudo") turns every translated text into an accented one about 40 % longer between ⟦ and ⟧, so
// text that never went through tr() shows up without the marks, and text with too little room shows up clipped.
//
// Projects, exports and the agent link's results do not change with the language or the system's locale: numbers in
// them keep a full stop for the decimal mark.
#pragma once

#include <QString>
#include <QStringList>
#include <QTranslator>
#include <memory>
#include <string>
#include <vector>

class QObject;

namespace montage::language {

struct Language {
    QString code;  // "de", "pt_BR", "ar"; "en" for the interface as written; "pseudo" for the test language
    QString name;  // in itself ("Deutsch")
    // How far its translation is done: texts checked by a translator, and machine drafts not yet checked, of all.
    int finished = 0, drafts = 0, texts = 0;
    // The name, with how far it is done when not finished ("Deutsch (10 % checked, the rest drafts)").
    QString label() const;
};
// English, the languages Montage has a translation for, and the test language last.
std::vector<Language> available();
QString displayName(const QString& code);

// The language chosen in Window › Language ("" the system's), kept in the settings ("ui/language").
QString chosen();
void choose(const QString& code);
// The language a choice comes to: the system's first language Montage has a translation for, or English.
QString resolve(const QString& choice);

// Installs the language's translations (Montage's and Qt's) in the application and sets its layout direction; once, at
// startup, before any window is built. Returns the language installed.
QString install(const QString& choice);
// Takes them out again (the interface as written, left to right).
void uninstall();
QString installed();
// Whether the language is written right to left.
bool rightToLeft(const QString& code);

// A text the engine gives the interface (an effect's or a setting's name, a preset's, a colour space's), in the
// interface's language: translated under the name "Engine".
QString engine(const std::string& text);
// The engine's texts listed for lupdate (app/EngineTexts.cpp, written by `montage-cli interface-texts`).
std::vector<std::string> listedEngineTexts();

// Marks a widget or menu entry whose text is not to be translated (a name from the project, a language's own name, a
// format's name); the test language's check passes over it and its children.
void keepUntranslated(QObject* o);
bool keptUntranslated(const QObject* o);
// The same for an item of a list or tree (a plug-in's or a saved preset's name): true in this role.
constexpr int kUntranslatedRole = Qt::UserRole + 0x4c47;

// The test language's translator.
class PseudoTranslator : public QTranslator {
public:
    QString translate(const char* context, const char* sourceText, const char* disambiguation = nullptr, int n = -1) const override;
    bool isEmpty() const override { return false; }
    static QString pseudo(const QString& text);
    static bool marked(const QString& text);  // contains a translated text
};

}  // namespace montage::language
