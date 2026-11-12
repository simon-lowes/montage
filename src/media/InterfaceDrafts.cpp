#include "InterfaceDrafts.h"

#include "core/Adr.h"
#include "core/Effects.h"
#include "core/ShapeContents.h"
#include "media/Exr.h"
#include "render/ColorSlice.h"
#include "render/ColorSpace.h"
#include "render/Exporter.h"

#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <algorithm>
#include <cstdlib>
#include <set>

namespace montage {

namespace {

std::string unescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        const size_t semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 10) {
            out += s[i];
            continue;
        }
        const std::string e = s.substr(i + 1, semi - i - 1);
        if (e == "amp") out += '&';
        else if (e == "lt") out += '<';
        else if (e == "gt") out += '>';
        else if (e == "quot") out += '"';
        else if (e == "apos") out += '\'';
        else if (!e.empty() && e[0] == '#') {
            const char32_t code = char32_t(e.size() > 1 && e[1] == 'x' ? std::strtoul(e.c_str() + 2, nullptr, 16) : std::strtoul(e.c_str() + 1, nullptr, 10));
            out += QString::fromUcs4(&code, 1).toStdString();
        } else {
            out += s.substr(i, semi - i + 1);
        }
        i = semi;
    }
    return out;
}

std::string escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '"') out += "&quot;";
        else if (c == '\'') out += "&apos;";
        else out += c;
    }
    return out;
}

// One <message> of the file: where its translation's element is, and what it holds.
struct Message {
    std::string context;
    std::string source;
    bool numerus = false;
    bool vanished = false;  // (obsolete: no longer in the source)
    bool unfinished = false;
    std::string translation;            // the element's content, as written
    size_t elementStart = 0, elementEnd = 0;  // the whole <translation ...>...</translation> (or <translation .../>)
};

bool parse(const std::string& ts, std::vector<Message>& out, std::string* error) {
    size_t pos = 0;
    for (;;) {
        size_t m = ts.find("<message", pos);
        if (m == std::string::npos) return true;
        const size_t tagEnd = ts.find('>', m);
        const size_t end = ts.find("</message>", m);
        if (tagEnd == std::string::npos || end == std::string::npos) {
            if (error) *error = "a <message> with no end";
            return false;
        }
        Message msg;
        if (const size_t name = ts.rfind("<name>", m); name != std::string::npos) {
            const size_t close = ts.find("</name>", name);
            if (close != std::string::npos && close < m) msg.context = unescape(ts.substr(name + 6, close - name - 6));
        }
        msg.numerus = ts.substr(m, tagEnd - m).find("numerus=\"yes\"") != std::string::npos;
        const size_t s0 = ts.find("<source>", tagEnd), s1 = ts.find("</source>", tagEnd);
        const size_t t0 = ts.find("<translation", tagEnd);
        if (s0 > end || s1 > end || t0 > end) {
            if (error) *error = "a <message> with no source or translation";
            return false;
        }
        msg.source = unescape(ts.substr(s0 + 8, s1 - s0 - 8));
        const size_t tTagEnd = ts.find('>', t0);
        const std::string tag = ts.substr(t0, tTagEnd - t0 + 1);
        msg.unfinished = tag.find("type=\"unfinished\"") != std::string::npos;
        msg.vanished = tag.find("type=\"vanished\"") != std::string::npos || tag.find("type=\"obsolete\"") != std::string::npos;
        msg.elementStart = t0;
        if (tag.size() >= 2 && tag[tag.size() - 2] == '/') {
            msg.elementEnd = tTagEnd + 1;
        } else {
            const size_t close = ts.find("</translation>", tTagEnd);
            if (close > end) {
                if (error) *error = "a <translation> with no end";
                return false;
            }
            msg.translation = ts.substr(tTagEnd + 1, close - tTagEnd - 1);
            msg.elementEnd = close + 14;
        }
        out.push_back(std::move(msg));
        pos = end + 10;
    }
}

bool hasText(const Message& m) {
    if (!m.numerus) return m.translation.find_first_not_of(" \t\r\n") != std::string::npos;
    return m.translation.find("<numerusform>") != std::string::npos &&
           m.translation.find("<numerusform></numerusform>") == std::string::npos;
}

QStringList placeholders(const QString& s) {
    static const QRegularExpression re(QStringLiteral("%(L?\\d+|n)"));
    QStringList out;
    for (auto it = re.globalMatch(s); it.hasNext();) out << it.next().captured(0);
    out.sort();
    return out;
}

}  // namespace

InterfaceDraftStats interfaceTranslationStatus(const std::string& ts) {
    InterfaceDraftStats st;
    std::vector<Message> msgs;
    if (!parse(ts, msgs, nullptr)) return st;
    for (const Message& m : msgs) {
        if (m.vanished) continue;
        ++st.messages;
        if (!hasText(m)) continue;
        if (m.unfinished) ++st.existing;
        else ++st.finished;
    }
    return st;
}

// A file dialog's filter list ("Captions (*.srt *.vtt);;All files (*)") keeps the source's patterns, whatever the
// model made of them ("(*.Montage)", "(*. srt)"): only each filter's name is the translation's, less any separator the
// model doubled or wrote its own way (";;;", "؛"). A draft that lost or merged a filter, or kept no name for one or only
// pieces of the patterns ("*.otio *.edl)", "WebVTT*.vtt"), is not kept.
bool garbledDraft(const std::string& source, const std::string& draft) {
    const QString src = QString::fromStdString(source), d = QString::fromStdString(draft);
    if (d.contains(QStringLiteral("<unk>")) || d.count('\\') > src.count('\\') || d.count('+') > src.count('+')) return true;
    // (a letter of another script than Latin: Greek and on, but not Latin's own extended letters)
    const bool latin = std::none_of(d.begin(), d.end(), [](QChar c) {
        return c.isLetter() && c.unicode() >= 0x370 && !(c.unicode() >= 0x1e00 && c.unicode() <= 0x1eff);
    });
    if (latin) return false;
    static const QRegularExpression english(QStringLiteral("[A-Za-z]+"));
    QSet<QString> words;
    for (auto it = english.globalMatch(src); it.hasNext();) words.insert(it.next().captured());
    // Each run of letters in the draft that is three English letters or more and no word of the source, but one with
    // its first letter gone.
    for (qsizetype i = 0; i < d.size();) {
        if (!d[i].isLetter()) {
            ++i;
            continue;
        }
        qsizetype j = i;
        while (j < d.size() && d[j].isLetter()) ++j;
        const QString w = d.mid(i, j - i);
        i = j;
        if (w.size() < 3 || words.contains(w) || !english.match(w).hasMatch() || english.match(w).capturedLength() != w.size()) continue;
        for (const QString& x : words)
            if (x.size() == w.size() + 1 && x.mid(1) == w) return true;
    }
    return false;
}

static bool keepFilters(const QString& src, QString& d) {
    static const QRegularExpression patterns(QStringLiteral("\\(([^()]*\\*[^()]*)\\)\\s*$"));
    const QStringList from = src.split(QStringLiteral(";;")), to = d.split(QStringLiteral(";;"));
    if (std::none_of(from.begin(), from.end(), [](const QString& f) { return patterns.match(f.trimmed()).hasMatch(); })) return true;
    if (from.size() != to.size()) return false;
    QStringList out;
    for (qsizetype i = 0; i < from.size(); ++i) {
        const auto m = patterns.match(from[i].trimmed());
        if (!m.hasMatch()) {
            out << to[i];
            continue;
        }
        QString name = to[i].trimmed();
        if (const qsizetype open = name.lastIndexOf('('); open >= 0) name = name.left(open).trimmed();
        auto separator = [](QChar c) { return c.isSpace() || c == ';' || c == QChar(0x061B) || c == QChar(0xFF1B); };
        while (!name.isEmpty() && separator(name.front())) name.remove(0, 1);
        while (!name.isEmpty() && separator(name.back())) name.chop(1);
        if (name.isEmpty() || name.contains('*') || name.count('(') != name.count(')')) return false;
        out << name + QStringLiteral(" (") + m.captured(1) + ')';
    }
    d = out.join(QStringLiteral(";;"));
    return true;
}

bool draftInterfaceTranslation(const std::string& ts, const std::function<bool(const std::vector<std::string>&, std::vector<std::string>&)>& translate,
                               std::string& out, InterfaceDraftStats* stats, int limit, std::string* error) {
    std::vector<Message> msgs;
    if (!parse(ts, msgs, error)) return false;
    InterfaceDraftStats st = interfaceTranslationStatus(ts);
    // What goes to the translator: the text without its mnemonic, and with a literal ampersand as one. An ampersand is a
    // menu's mnemonic only before a letter or digit and never in the engine's names ("Black & White").
    struct Job {
        size_t message;
        bool mnemonic, doubled;
    };
    std::vector<Job> jobs;
    std::vector<std::string> texts;
    for (size_t i = 0; i < msgs.size(); ++i) {
        const Message& m = msgs[i];
        if (m.vanished || hasText(m)) continue;
        const QString src = QString::fromStdString(m.source);
        if (m.numerus || src.contains('<') || src.trimmed().isEmpty()) {
            ++st.skipped;
            continue;
        }
        if (limit > 0 && int(jobs.size()) >= limit) continue;
        QString plain;
        bool mnemonic = false, doubled = false;
        for (qsizetype k = 0; k < src.size(); ++k) {
            if (src[k] == '&' && k + 1 < src.size() && src[k + 1] == '&') {
                plain += '&';
                doubled = true;
                ++k;
            } else if (src[k] == '&' && m.context != "Engine" && k + 1 < src.size() && src[k + 1].isLetterOrNumber()) {
                mnemonic = true;
            } else {
                plain += src[k];
            }
        }
        jobs.push_back({i, mnemonic, doubled});
        texts.push_back(plain.toStdString());
    }
    std::vector<std::string> drafts;
    if (!texts.empty() && (!translate(texts, drafts) || drafts.size() != texts.size())) {
        if (error) *error = "the translation failed";
        return false;
    }
    // Written back from the end, so the earlier positions stay where they were.
    std::vector<std::pair<size_t, std::string>> edits;
    for (size_t j = 0; j < jobs.size(); ++j) {
        const Message& m = msgs[jobs[j].message];
        const QString src = QString::fromStdString(m.source);
        QString d = QString::fromStdString(drafts[j]).trimmed();
        if (placeholders(d) != placeholders(src)) {
            // Some models space a placeholder out ("Удалить% 1"): put it back together, after a space as in English.
            static const QRegularExpression spaced(QStringLiteral("%\\s+(L?\\d+|n)\\b"));
            for (auto hit = spaced.match(d); hit.hasMatch(); hit = spaced.match(d, hit.capturedStart() + 1)) {
                const qsizetype at = hit.capturedStart();
                const bool joined = at > 0 && !d[at - 1].isSpace() && !QStringLiteral("([«\"'„“").contains(d[at - 1]);
                d.replace(at, hit.capturedLength(), (joined ? QStringLiteral(" %") : QStringLiteral("%")) + hit.captured(1));
            }
        }
        // A model that met a word or sign it has no piece for writes "<unk>" in its place: that draft is garbled, so the
        // text stays in English.
        // (and one that wrote backslashes for words it could not place, a run or a single one the source has not, is
        // garbled the same way)
        const bool garbled = garbledDraft(m.source, d.toStdString());
        if (d.isEmpty() || garbled || placeholders(d) != placeholders(src) || !keepFilters(src, d)) {
            ++st.skipped;
            continue;
        }
        // A text joined to others (", %1 selected", " LUFS") keeps its leading comma or space and its trailing space.
        auto joiner = [](QChar c) { return c.isSpace() || c == ',' || c == ';'; };
        qsizetype a = 0, z = src.size();
        while (a < z && joiner(src[a])) ++a;
        while (z > a && src[z - 1].isSpace()) --z;
        const QString core = src.mid(a, z - a);
        while (!d.isEmpty() && joiner(d.front())) d.remove(0, 1);
        for (const QString& end : {QStringLiteral("…"), QStringLiteral(":")})
            if (core.endsWith(end) && !d.endsWith(end)) {
                while (!d.isEmpty() && (d.back() == '.' || d.back() == ':' || d.back().isSpace())) d.chop(1);
                d += end;
            }
        if (!core.endsWith('.') && d.endsWith('.') && !d.endsWith(QStringLiteral("..."))) d.chop(1);
        // An ampersand the translation brought ("Suchen & Ersetzen") is a word, not a mnemonic, so before a letter it is
        // doubled; in the engine's names it is always shown as it is.
        if (jobs[j].doubled) d.replace('&', QStringLiteral("&&"));
        else if (m.context != "Engine")
            for (qsizetype k = d.size() - 2; k >= 0; --k)
                if (d[k] == '&' && d[k + 1].isLetterOrNumber() && (k == 0 || d[k - 1] != '&')) d.insert(k, '&');
        if (jobs[j].mnemonic) {
            const auto letter = std::find_if(d.begin(), d.end(), [](QChar c) { return c.isLetter(); });
            if (letter != d.end()) d.insert(letter - d.begin(), '&');
        }
        d = src.left(a) + d + src.mid(z);
        edits.push_back({jobs[j].message, "<translation type=\"unfinished\">" + escape(d.toStdString()) + "</translation>"});
        ++st.drafted;
    }
    out = ts;
    for (auto it = edits.rbegin(); it != edits.rend(); ++it) {
        const Message& m = msgs[it->first];
        out.replace(m.elementStart, m.elementEnd - m.elementStart, it->second);
    }
    if (stats) *stats = st;
    return true;
}

std::vector<std::string> engineInterfaceTexts() {
    std::set<std::string> texts;
    auto add = [&](const std::string& t) {
        if (std::any_of(t.begin(), t.end(), [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); })) texts.insert(t);
    };
    for (const EffectInfo& e : effectCatalog()) {
        add(e.displayName);
        add(e.group);
        for (const ParamInfo& p : e.params) {
            add(p.label);
            for (const std::string& c : p.choices) add(c);
        }
        for (const StringParamInfo& sp : e.strings) {
            add(sp.label);
            // (A choice of words; a Track setting's first is its "none". Fonts and files are names, not words.)
            if (sp.kind == StringKind::Choice) for (const std::string& c : sp.choices) add(c);
            if (sp.kind == StringKind::Track && !sp.choices.empty()) add(sp.choices.front());
        }
    }
    // (a Parametric EQ's band types and slopes, shown in its bands' settings; and the settings' names, the numbered
    // bands' as patterns, "Band %1 (Hz)", which app/Language.h engine() fills in)
    for (const std::string& t : eqBandTypes()) add(t);
    for (const std::string& t : eqSlopes()) add(t);
    for (const char* band : {"low", "high", "b%1"})
        for (const ParamInfo& p : eqBandParams(band)) add(p.label);
    for (const ShapeItemKind& k : shapeItemKinds()) {
        add(k.label);
        for (const ParamInfo& p : k.params) {
            add(p.label);
            for (const std::string& c : p.choices) add(c);
        }
    }
    for (const std::string& m : blendModes()) add(blendModeLabel(m));
    for (const TitleTemplate& t : titleTemplates()) add(t.name);
    for (const HdrZone& z : hdrZones()) add(z.label);
    for (const HueVector& v : colorSliceVectors()) add(v.label);
    for (const ColorSpace& cs : colorSpaces()) add(cs.label);
    for (const ExportPreset& p : exportPresetsOfEveryBuild()) add(p.name), add(p.description);
    for (const std::string& c : exrCompressions()) add(exrCompressionLabel(c));
    for (int k = kAdrToRecord; k <= kAdrOmitted; ++k) add(adrStatusName(k));
    return {texts.begin(), texts.end()};
}

std::string engineInterfaceTextsSource(const std::vector<std::string>& texts) {
    std::string out =
        "// Written by `montage-cli interface-texts -o src/app/EngineTexts.cpp` (media/InterfaceDrafts.h): the texts the\n"
        "// engine gives the interface to show, for lupdate to find under the name \"Engine\". Not to be edited by hand.\n"
        "#include \"Language.h\"\n"
        "\n"
        "#include <QtGlobal>\n"
        "#include <iterator>\n"
        "\n"
        "namespace montage::language {\n"
        "\n"
        "namespace {\n"
        "\n"
        "const char* const kTexts[] = {\n";
    for (const std::string& t : texts) {
        out += "    QT_TRANSLATE_NOOP(\"Engine\", \"";
        for (char c : t) {
            if (c == '"' || c == '\\') out += '\\', out += c;
            else if (c == '\n') out += "\\n";
            else if (c == '\t') out += "\\t";
            else out += c;
        }
        out += "\"),\n";
    }
    out +=
        "};\n"
        "\n"
        "}  // namespace\n"
        "\n"
        "std::vector<std::string> listedEngineTexts() { return {std::begin(kTexts), std::end(kTexts)}; }\n"
        "\n"
        "}  // namespace montage::language\n";
    return out;
}

}  // namespace montage
