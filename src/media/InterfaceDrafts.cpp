#include "InterfaceDrafts.h"

#include "core/Adr.h"
#include "core/Effects.h"
#include "media/Exr.h"
#include "render/ColorSlice.h"
#include "render/ColorSpace.h"
#include "render/Exporter.h"

#include <QRegularExpression>
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
        if (d.isEmpty() || placeholders(d) != placeholders(src)) {
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
    for (const std::string& m : blendModes()) add(blendModeLabel(m));
    for (const TitleTemplate& t : titleTemplates()) add(t.name);
    for (const HdrZone& z : hdrZones()) add(z.label);
    for (const HueVector& v : colorSliceVectors()) add(v.label);
    for (const ColorSpace& cs : colorSpaces()) add(cs.label);
    for (const ExportPreset& p : exportPresets()) add(p.name), add(p.description);
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
