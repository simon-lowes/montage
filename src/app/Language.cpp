#include "Language.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QLibraryInfo>
#include <QLocale>
#include <cmath>
#include <cstring>

#include "Settings.h"

namespace montage::language {

namespace {

const char* const kKey = "ui/language";
const char* const kUntranslated = "montageUntranslated";

std::unique_ptr<QTranslator> gMontage, gQt;
QString gInstalled = QStringLiteral("en");

QString qmName(const QString& code) { return QStringLiteral(":/montage/i18n/montage_%1.qm").arg(code); }

}  // namespace

std::vector<Language> available() {
    std::vector<Language> out{{QStringLiteral("en"), displayName(QStringLiteral("en"))}};
    QStringList codes;
    for (const QString& f : QDir(QStringLiteral(":/montage/i18n")).entryList({QStringLiteral("montage_*.qm")}, QDir::Files, QDir::Name))
        codes << f.mid(8, f.size() - 8 - 3);
    for (const QString& c : codes) {
        Language l{c, displayName(c)};
        QFile status(QStringLiteral(":/montage/i18n/montage_%1.status").arg(c));
        if (status.open(QIODevice::ReadOnly)) {
            const QList<QByteArray> n = status.readAll().simplified().split(' ');
            if (n.size() == 3) l.finished = n[0].toInt(), l.drafts = n[1].toInt(), l.texts = n[2].toInt();
        }
        out.push_back(l);
    }
    out.push_back({QStringLiteral("pseudo"), displayName(QStringLiteral("pseudo"))});
    return out;
}

QString Language::label() const {
    if (texts <= 0 || finished >= texts) return name;
    const int checked = int(std::floor(100.0 * finished / texts));
    const int drafted = int(std::floor(100.0 * drafts / texts));
    return drafts > 0 ? QCoreApplication::translate("montage::language", "%1 (%2 % checked, %3 % machine drafts)").arg(name).arg(checked).arg(drafted)
                      : QCoreApplication::translate("montage::language", "%1 (%2 % translated)").arg(name).arg(checked);
}

QString displayName(const QString& code) {
    if (code == QLatin1String("en")) return QStringLiteral("English");
    if (code == QLatin1String("pseudo")) return QCoreApplication::translate("montage::language", "Test Language (Finds Untranslated Text)");
    const QLocale l(code);
    QString n = l.nativeLanguageName();
    if (n.isEmpty()) return code;
    n[0] = n[0].toUpper();
    if (code.contains('_')) n += QStringLiteral(" (%1)").arg(l.nativeTerritoryName());
    return n;
}

QString chosen() { return appSettings().value(QLatin1String(kKey)).toString(); }

void choose(const QString& code) {
    QSettings s = appSettings();
    if (code.isEmpty()) s.remove(QLatin1String(kKey));
    else s.setValue(QLatin1String(kKey), code);
}

QString resolve(const QString& choice) {
    if (!choice.isEmpty()) return choice;
    // Of the system's languages, one at least half checked by a translator: machine drafts only when chosen.
    QStringList ready;
    for (const Language& l : available())
        if (l.texts > 0 && l.finished * 2 >= l.texts) ready << l.code;
    for (QString ui : QLocale::system().uiLanguages()) {
        // ("zh-Hans-CN" as Qt names it: zh_CN)
        ui = QLocale(ui).name();
        if (ui.startsWith(QLatin1String("en"))) return QStringLiteral("en");
        if (ready.contains(ui)) return ui;
        if (const QString lang = ui.section('_', 0, 0); ready.contains(lang)) return lang;
    }
    return QStringLiteral("en");
}

QString install(const QString& choice) {
    uninstall();
    const QString code = resolve(choice);
    if (code == QLatin1String("pseudo")) {
        gMontage = std::make_unique<PseudoTranslator>();
        QCoreApplication::installTranslator(gMontage.get());
    } else if (code != QLatin1String("en")) {
        auto m = std::make_unique<QTranslator>();
        if (m->load(qmName(code)) && QCoreApplication::installTranslator(m.get())) gMontage = std::move(m);
        // Qt's own: its standard dialogs and buttons, from where Qt keeps them or beside Montage in a package.
        const QString app = QCoreApplication::applicationDirPath();
        for (const QString& dir : {QLibraryInfo::path(QLibraryInfo::TranslationsPath), app + QStringLiteral("/translations"),
                                   app + QStringLiteral("/../Resources/translations"), app + QStringLiteral("/../share/qt6/translations")}) {
            auto q = std::make_unique<QTranslator>();
            if (q->load(QLocale(code), QStringLiteral("qtbase"), QStringLiteral("_"), dir) && QCoreApplication::installTranslator(q.get())) {
                gQt = std::move(q);
                break;
            }
        }
    }
    gInstalled = code;
    QGuiApplication::setLayoutDirection(rightToLeft(code) ? Qt::RightToLeft : Qt::LeftToRight);
    return code;
}

void uninstall() {
    for (auto* t : {&gMontage, &gQt})
        if (*t) {
            QCoreApplication::removeTranslator(t->get());
            t->reset();
        }
    gInstalled = QStringLiteral("en");
    QGuiApplication::setLayoutDirection(Qt::LeftToRight);
}

QString installed() { return gInstalled; }

bool rightToLeft(const QString& code) {
    if (code == QLatin1String("pseudo") || code == QLatin1String("en")) return false;
    return QLocale(code).textDirection() == Qt::RightToLeft;
}

QString engine(const std::string& text) {
    if (text.empty()) return {};
    return QCoreApplication::translate("Engine", text.c_str());
}

void keepUntranslated(QObject* o) {
    if (o) o->setProperty(kUntranslated, true);
}

bool keptUntranslated(const QObject* o) {
    for (; o; o = o->parent())
        if (o->property(kUntranslated).toBool()) return true;
    return false;
}

// ---- The test language ------------------------------------------------------

QString PseudoTranslator::translate(const char* context, const char* sourceText, const char*, int) const {
    if (!sourceText || !*sourceText) return {};
    // Qt's names for keys (read back from settings) and its layout direction stay as they are.
    if (context && (!std::strcmp(context, "QShortcut") || !std::strcmp(context, "QKeySequence") ||
                    (!std::strcmp(context, "QGuiApplication") && !std::strcmp(sourceText, "QT_LAYOUT_DIRECTION"))))
        return QString::fromUtf8(sourceText);
    return pseudo(QString::fromUtf8(sourceText));
}

QString PseudoTranslator::pseudo(const QString& text) {
    static const QString plain = QStringLiteral("aceinouyAEINOUCY");
    static const QString accented = QStringLiteral("áçéíñóúýÁÉÍÑÓÚÇÝ");
    // A shortcut after a tab (in a menu's text) stays as it is.
    const qsizetype tab = text.indexOf('\t');
    const QString body = tab < 0 ? text : text.left(tab), tail = tab < 0 ? QString() : text.mid(tab);
    QString out;
    out.reserve(body.size() * 2);
    int visible = 0;
    for (qsizetype i = 0; i < body.size(); ++i) {
        const QChar c = body[i];
        if (c == '%') {  // %1, %L1, %n, %%: left for arg() and the plural forms
            qsizetype j = i + 1;
            if (j < body.size() && (body[j] == 'L')) ++j;
            if (j < body.size() && (body[j] == 'n' || body[j] == '%')) ++j;
            else
                while (j < body.size() && body[j].isDigit()) ++j;
            out += body.mid(i, j - i);
            i = j - 1;
            continue;
        }
        if (c == '<') {  // a rich-text tag
            const qsizetype close = body.indexOf('>', i);
            if (close > i && !body.mid(i + 1, close - i - 1).contains('<')) {
                out += body.mid(i, close - i + 1);
                i = close;
                continue;
            }
        }
        if (c == '&') {  // an entity (&amp;), a literal ampersand (&&), or a mnemonic (&File, kept on its letter)
            qsizetype j = i + 1;
            while (j < body.size() && (body[j].isLetterOrNumber() || body[j] == '#')) ++j;
            if (j < body.size() && body[j] == ';' && j > i + 1) {
                out += body.mid(i, j - i + 1);
                ++visible;
                i = j;
                continue;
            }
            if (i + 1 < body.size() && body[i + 1] == '&') {
                out += QStringLiteral("&&");
                ++visible;
                ++i;
                continue;
            }
            out += c;
            continue;
        }
        const qsizetype k = plain.indexOf(c);
        out += k >= 0 ? accented[k] : c;
        ++visible;
    }
    // About 40 % longer than the English (with the marks), as German and Finnish often are.
    const QString pad(std::max(1, int(std::lround(visible * 0.4)) - 2), QChar(u'~'));
    // Inside a rich-text document's outer tags, where the text shows.
    qsizetype open = 0, end = out.size();
    if (out.startsWith('<') && out.endsWith('>')) {
        open = out.indexOf('>') + 1;
        end = out.lastIndexOf('<');
        if (end < open) end = open;
    }
    return out.left(open) + QChar(0x27E6) + out.mid(open, end - open) + pad + QChar(0x27E7) + out.mid(end) + tail;
}

bool PseudoTranslator::marked(const QString& text) { return text.contains(QChar(0x27E6)); }

}  // namespace montage::language
