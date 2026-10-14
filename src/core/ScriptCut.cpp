#include "ScriptCut.h"

#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QXmlStreamReader>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "EditOps.h"
#include "MediaLog.h"
#include "Transcript.h"

namespace montage {

namespace {

QString norm(const QString& w) {
    QString n = w.toLower();
    n.remove(QRegularExpression(QStringLiteral("[^\\w]")));
    return n;
}

std::vector<QString> words(const QString& text) {
    std::vector<QString> out;
    for (const QString& w : text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts))
        if (QString n = norm(w); !n.isEmpty()) out.push_back(n);
    return out;
}

// ---------------------------------------------------------------------------
// Parsing

bool isSceneHeading(const QString& l) {
    static const QRegularExpression re(QStringLiteral("^(INT|EXT|INT\\.?/EXT|I/E|EST)[\\. ]"));
    return re.match(l).hasMatch();
}

bool isTransition(const QString& l) {
    if (l != l.toUpper()) return false;
    return l.endsWith(QStringLiteral("TO:")) || l == QStringLiteral("FADE IN:") || l.startsWith(QStringLiteral("FADE OUT")) ||
           l.startsWith(QStringLiteral("FADE TO"));
}

bool isDirection(const QString& l) {
    return (l.startsWith('(') && l.endsWith(')')) || (l.startsWith('[') && l.endsWith(']')) || l.startsWith('#');
}

QString withoutAsides(QString s) {
    s.remove(QRegularExpression(QStringLiteral("\\([^)]*\\)")));
    s.remove(QRegularExpression(QStringLiteral("\\[[^\\]]*\\]")));
    return s.simplified();
}

// A character cue: a short name in capitals, maybe with (V.O.) or (CONT'D).
bool isCue(const QString& l) {
    const QString s = withoutAsides(l);
    if (s.isEmpty() || s.size() > 40 || s != s.toUpper() || !s.contains(QRegularExpression(QStringLiteral("[A-Z]")))) return false;
    if (s.contains(QRegularExpression(QStringLiteral("[!?,;:]"))) || s.endsWith('.')) return false;
    return s.split(' ', Qt::SkipEmptyParts).size() <= 4;
}

QString speakerOf(const QString& cue) {
    QString s = withoutAsides(cue);
    // "JOHN SMITH" reads better as "John Smith".
    QStringList parts = s.split(' ', Qt::SkipEmptyParts);
    for (QString& p : parts) p = p.left(1).toUpper() + p.mid(1).toLower();
    return parts.join(' ');
}

void addSpeech(std::vector<ScriptLine>& out, const QString& speaker, const QString& body) {
    const QStringList sentences = body.split(QRegularExpression(QStringLiteral("(?<=[.!?])\\s+")), Qt::SkipEmptyParts);
    // Short paragraphs stay whole; long ones become groups of sentences of about 25 words.
    QString cur;
    int curWords = 0;
    auto flush = [&] {
        if (!cur.trimmed().isEmpty()) out.push_back({speaker.toStdString(), cur.trimmed().toStdString()});
        cur.clear();
        curWords = 0;
    };
    const int total = int(words(body).size());
    if (total <= 30) {
        out.push_back({speaker.toStdString(), body.toStdString()});
        return;
    }
    for (const QString& s : sentences) {
        const int n = int(words(s).size());
        if (curWords > 0 && curWords + n > 25) flush();
        cur += (cur.isEmpty() ? QString() : QStringLiteral(" ")) + s;
        curWords += n;
    }
    flush();
}

// ---------------------------------------------------------------------------
// Matching

int levenshtein(const QString& a, const QString& b, int cap) {
    if (std::abs(a.size() - b.size()) > cap) return cap + 1;
    std::vector<int> prev(size_t(b.size()) + 1), cur(prev.size());
    for (int j = 0; j <= b.size(); ++j) prev[size_t(j)] = j;
    for (int i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        int best = cur[0];
        for (int j = 1; j <= b.size(); ++j) {
            cur[size_t(j)] = std::min({prev[size_t(j)] + 1, cur[size_t(j) - 1] + 1, prev[size_t(j) - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
            best = std::min(best, cur[size_t(j)]);
        }
        if (best > cap) return cap + 1;
        std::swap(prev, cur);
    }
    return prev[size_t(b.size())];
}

// 2 for the same word, 1 for a near miss (a mishearing), -1 otherwise.
int similarity(const QString& a, const QString& b) {
    if (a == b) return 2;
    if (a.size() >= 4 && b.size() >= 4 && levenshtein(a, b, 1) <= 1) return 1;
    return -1;
}

bool isStopword(const QString& w) {
    static const std::set<QString> stop = {"the", "a", "an", "and", "or", "but", "of", "to", "in", "on", "at", "is", "it",
                                           "i", "you", "he", "she", "we", "they", "my", "your", "this", "that", "be", "was",
                                           "were", "are", "for", "with", "as", "so", "do", "not", "no", "yes", "oh", "um",
                                           "uh", "im", "its", "me", "his", "her", "our", "if", "just"};
    return stop.count(w) > 0;
}

struct Flat {
    Id mediaId = 0;
    int order = 0;  // position in the project's media
    const Transcript* t = nullptr;
    std::vector<QString> norm;
    std::vector<const TranscriptWord*> words;
    std::vector<int> speaker;
    std::map<QString, std::vector<int>> index;
};

struct Hit {
    int first = -1, last = -1, matched = 0;
};

// The best local alignment of `line` within t[a, b), not passing through blocked words.
Hit align(const std::vector<QString>& line, const Flat& f, int a, int b, const std::vector<char>& blocked) {
    const int m = int(line.size()), w = b - a;
    std::vector<int> h(size_t(m + 1) * size_t(w + 1), 0);
    std::vector<unsigned char> dir(h.size(), 0);  // 0 stop, 1 diagonal, 2 skip a line word, 3 skip a spoken word
    auto at = [w](int i, int j) { return size_t(i) * size_t(w + 1) + size_t(j); };
    int best = 0, bi = 0, bj = 0;
    for (int i = 1; i <= m; ++i)
        for (int j = 1; j <= w; ++j) {
            if (blocked[size_t(a + j - 1)]) continue;
            const int d = h[at(i - 1, j - 1)] + similarity(line[size_t(i - 1)], f.norm[size_t(a + j - 1)]);
            const int up = h[at(i - 1, j)] - 1, left = h[at(i, j - 1)] - 1;
            int v = 0;
            unsigned char k = 0;
            if (d > v) v = d, k = 1;
            if (up > v) v = up, k = 2;
            if (left > v) v = left, k = 3;
            h[at(i, j)] = v;
            dir[at(i, j)] = k;
            if (v > best) best = v, bi = i, bj = j;
        }
    Hit hit;
    if (best <= 0) return hit;
    int i = bi, j = bj;
    while (i > 0 && j > 0 && dir[at(i, j)] != 0) {
        const unsigned char k = dir[at(i, j)];
        if (k == 1) {
            if (similarity(line[size_t(i - 1)], f.norm[size_t(a + j - 1)]) > 0) {
                ++hit.matched;
                if (hit.last < 0) hit.last = a + j - 1;
                hit.first = a + j - 1;
            }
            --i, --j;
        } else if (k == 2) {
            --i;
        } else {
            --j;
        }
    }
    return hit;
}

QString nameKey(const std::string& s) { return norm(QString::fromStdString(s)); }

}  // namespace

Screenplay plainScreenplay(const std::string& text) {
    using K = ScriptBlock::Kind;
    Screenplay sp;
    // Paragraphs (separated by blank lines) and, where they come, the lines left out of them.
    struct Entry {
        QStringList lines;  // a paragraph
        QStringList asides;  // the parentheticals left out of it, shown under its cue
        K kind = K::Dialogue;
        QString skipped;    // or a line left out (kind: what it is)
        bool left = false;  // (which of the two)
    };
    std::vector<Entry> entries;
    bool open = false;  // a paragraph is being read
    for (QString l : QString::fromStdString(text).split('\n')) {
        l = l.trimmed();
        if (l.isEmpty()) {
            open = false;
            continue;
        }
        if (isSceneHeading(l) || isTransition(l) || isDirection(l)) {
            if (open && l.startsWith('(')) {
                std::find_if(entries.rbegin(), entries.rend(), [](const Entry& e) { return !e.left; })->asides << l;
                continue;
            }
            Entry e;
            e.kind = isSceneHeading(l) ? K::Heading : isTransition(l) ? K::Transition : l.startsWith('(') ? K::Parenthetical : K::Action;
            e.skipped = l.startsWith('#') ? l.mid(1).trimmed() : l;
            e.left = true;
            entries.push_back(e);
            continue;
        }
        if (!open) {
            entries.emplace_back();
            open = true;
        }
        // (a paragraph goes on after a line left out of it)
        auto it = std::find_if(entries.rbegin(), entries.rend(), [](const Entry& e) { return !e.left; });
        it->lines << l;
    }
    QString pending;  // a cue alone in its paragraph names the next one's speaker
    for (Entry& e : entries) {
        if (e.left) {
            if (!e.skipped.isEmpty()) sp.blocks.push_back({e.kind, e.skipped.toStdString()});
            continue;
        }
        QStringList& lines = e.lines;
        if (lines.isEmpty()) continue;
        QString speaker = pending;
        pending.clear();
        if (isCue(lines.front())) {
            sp.blocks.push_back({K::Character, lines.front().toStdString()});
            if (lines.size() == 1) {
                pending = speakerOf(lines.front());
                for (const QString& a : e.asides) sp.blocks.push_back({K::Parenthetical, a.toStdString()});
                continue;
            }
            speaker = speakerOf(lines.takeFirst());
        }
        QString body = withoutAsides(lines.join(' '));
        static const QRegularExpression named(QStringLiteral("^([A-Za-z][\\w .'’-]{0,29}):\\s+(.+)$"));
        if (const auto m = named.match(body); m.hasMatch() && m.captured(1).split(' ', Qt::SkipEmptyParts).size() <= 4) {
            const QString who = m.captured(1).trimmed();
            speaker = who == who.toUpper() ? speakerOf(who) : who;
            body = m.captured(2).trimmed();
            sp.blocks.push_back({K::Character, who.toUpper().toStdString()});
        }
        for (const QString& a : e.asides) sp.blocks.push_back({K::Parenthetical, a.toStdString()});
        if (words(body).empty()) continue;
        ScriptBlock b{K::Dialogue, body.toStdString(), int(sp.lines.size())};
        addSpeech(sp.lines, speaker, body);
        b.lineCount = int(sp.lines.size()) - b.firstLine;
        sp.blocks.push_back(b);
    }
    return sp;
}

std::vector<ScriptLine> parseScript(const std::string& text) { return plainScreenplay(text).lines; }

std::string fdxToScript(const std::string& xml) {
    QXmlStreamReader r(QByteArray::fromStdString(xml));
    QString out, type, text;
    bool inParagraph = false;
    while (!r.atEnd()) {
        r.readNext();
        if (r.isStartElement() && r.name() == QLatin1String("Paragraph")) {
            inParagraph = true;
            type = r.attributes().value(QLatin1String("Type")).toString();
            text.clear();
        } else if (inParagraph && r.isStartElement() && r.name() == QLatin1String("Text")) {
            text += r.readElementText(QXmlStreamReader::IncludeChildElements);
        } else if (r.isEndElement() && r.name() == QLatin1String("Paragraph")) {
            inParagraph = false;
            text = text.simplified();
            if (text.isEmpty()) continue;
            if (type == QLatin1String("Character")) out += QStringLiteral("\n") + text.toUpper() + QStringLiteral("\n");
            else if (type == QLatin1String("Dialogue")) out += text + QStringLiteral("\n");
        }
    }
    return out.toStdString();
}

namespace {

// Fountain's emphasis (*italic*, **bold**, _underline_) shown plain; \* stays a star.
QString plainFountain(QString s) {
    s.replace(QStringLiteral("\\*"), QStringLiteral("\x01")).replace(QStringLiteral("\\_"), QStringLiteral("\x02"));
    static const QRegularExpression stars(QStringLiteral("\\*{1,3}([^*\\n]+?)\\*{1,3}")), under(QStringLiteral("_([^_\\n]+?)_"));
    s.replace(stars, QStringLiteral("\\1")).replace(under, QStringLiteral("\\1"));
    return s.replace(QChar(1), QChar('*')).replace(QChar(2), QChar('_')).trimmed();
}

bool fountainHeading(const QString& l) {
    static const QRegularExpression re(QStringLiteral("^(INT|EXT|EST|INT\\.?/EXT|I/E)[\\. ]"), QRegularExpression::CaseInsensitiveOption);
    return re.match(l).hasMatch();
}

// A character cue: capitals (an extension in brackets may be lower case), at least one letter.
bool fountainCue(QString l) {
    static const QRegularExpression extension(QStringLiteral("\\([^)]*\\)")), letter(QStringLiteral("[A-Z]"));
    if (l.endsWith('^')) l.chop(1);
    l.remove(extension);
    l = l.trimmed();
    return !l.isEmpty() && l == l.toUpper() && l.contains(letter);
}

}  // namespace

Screenplay parseFountain(const std::string& source) {
    using K = ScriptBlock::Kind;
    Screenplay sp;
    QString text = QString::fromStdString(source);
    text.remove('\r');
    text.remove(QRegularExpression(QStringLiteral("/\\*.*?\\*/"), QRegularExpression::DotMatchesEverythingOption));
    // (a note on a line of its own goes with its line, so it does not end a speech)
    text.remove(QRegularExpression(QStringLiteral("^[ \\t]*\\[\\[(?:(?!\\]\\]).)*\\]\\][ \\t]*\\n"),
                                   QRegularExpression::DotMatchesEverythingOption | QRegularExpression::MultilineOption));
    text.remove(QRegularExpression(QStringLiteral("\\[\\[.*?\\]\\]"), QRegularExpression::DotMatchesEverythingOption));
    QStringList raw = text.split('\n');
    size_t i = 0;
    // The title page: "Key: value" lines (values may go on, indented, below) up to the first blank line.
    static const QRegularExpression key(QStringLiteral("^([A-Za-z][A-Za-z ]*):\\s*(.*)$"));
    while (i < size_t(raw.size()) && raw[qsizetype(i)].trimmed().isEmpty()) ++i;
    static const QRegularExpression titleKey(
        QStringLiteral("^(Title|Credit|Authors?|Source|Draft date|Date|Contact|Copyright|Notes|Revision):"), QRegularExpression::CaseInsensitiveOption);
    // (any "Key: value" opens one, but not a transition such as FADE IN:)
    auto opensTitlePage = [&](const QString& l) {
        if (titleKey.match(l).hasMatch()) return true;
        const auto m = key.match(l);
        return m.hasMatch() && m.captured(1) != m.captured(1).toUpper() && !fountainHeading(l);
    };
    if (i < size_t(raw.size()) && opensTitlePage(raw[qsizetype(i)])) {
        QString current;
        for (; i < size_t(raw.size()) && !raw[qsizetype(i)].trimmed().isEmpty(); ++i) {
            const QString& l = raw[qsizetype(i)];
            if (const auto m = key.match(l); m.hasMatch() && !l.startsWith(' ') && !l.startsWith('\t')) {
                current = m.captured(1).trimmed().toLower();
                if (current == QLatin1String("title") && !m.captured(2).trimmed().isEmpty()) sp.title = plainFountain(m.captured(2)).toStdString();
            } else if (current == QLatin1String("title")) {
                sp.title += (sp.title.empty() ? "" : " ") + plainFountain(l).toStdString();
            }
        }
    }
    // (a line of two spaces is Fountain's empty line inside a speech, not a blank one)
    auto blankAt = [&](size_t k) {
        return k >= size_t(raw.size()) || (raw[qsizetype(k)].trimmed().isEmpty() && !raw[qsizetype(k)].startsWith(QLatin1String("  ")));
    };
    bool prevBlank = true;
    for (; i < size_t(raw.size()); ++i) {
        const QString l = raw[qsizetype(i)].trimmed();
        if (l.isEmpty()) {
            prevBlank = true;
            continue;
        }
        const bool wasBlank = prevBlank;
        prevBlank = false;
        if (l.size() >= 3 && l.count('=') == l.size()) {
            sp.blocks.push_back({K::PageBreak, {}});
            continue;
        }
        if (l.startsWith('#') || l.startsWith('=')) continue;  // sections and synopses: not printed
        if (l.startsWith('~')) {
            sp.blocks.push_back({K::Lyric, plainFountain(l.mid(1)).toStdString()});
            continue;
        }
        if ((l.startsWith('.') && !l.startsWith(QLatin1String(".."))) || (wasBlank && fountainHeading(l))) {
            QString h = l.startsWith('.') ? l.mid(1) : l;
            h.remove(QRegularExpression(QStringLiteral("\\s*#[^#]*#\\s*$")));  // a scene number
            sp.blocks.push_back({K::Heading, plainFountain(h).toUpper().toStdString()});
            continue;
        }
        if (l.startsWith('>') && l.endsWith('<')) {
            sp.blocks.push_back({K::Centered, plainFountain(l.mid(1, l.size() - 2)).toStdString()});
            continue;
        }
        if (l.startsWith('>') || (wasBlank && blankAt(i + 1) && l == l.toUpper() && l.endsWith(QLatin1String("TO:")))) {
            sp.blocks.push_back({K::Transition, plainFountain(l.startsWith('>') ? l.mid(1) : l).toUpper().toStdString()});
            continue;
        }
        if (!l.startsWith('!') && (l.startsWith('@') || (wasBlank && !blankAt(i + 1) && fountainCue(l)))) {
            QString cue = l.startsWith('@') ? l.mid(1).trimmed() : l;
            if (cue.endsWith('^')) cue.chop(1);
            cue = cue.trimmed();
            sp.blocks.push_back({K::Character, plainFountain(cue).toStdString()});
            const QString speaker = speakerOf(cue);
            // Its dialogue and parentheticals, up to the next blank line.
            QString speech;
            auto flush = [&] {
                const QString body = withoutAsides(plainFountain(speech));
                speech.clear();
                if (words(body).empty()) return;
                ScriptBlock b{K::Dialogue, body.toStdString(), int(sp.lines.size())};
                addSpeech(sp.lines, speaker, body);
                b.lineCount = int(sp.lines.size()) - b.firstLine;
                sp.blocks.push_back(b);
            };
            for (; !blankAt(i + 1); ++i) {
                const QString d = raw[qsizetype(i + 1)].trimmed();
                if (d.startsWith('(') && d.endsWith(')')) {
                    flush();
                    sp.blocks.push_back({K::Parenthetical, plainFountain(d).toStdString()});
                } else {
                    speech += (speech.isEmpty() ? QString() : QStringLiteral(" ")) + d;
                }
            }
            flush();
            continue;
        }
        // Action: this line and those up to the next blank one.
        QString action = l.startsWith('!') ? l.mid(1) : l;
        for (; !blankAt(i + 1); ++i) action += '\n' + raw[qsizetype(i + 1)].trimmed();
        sp.blocks.push_back({K::Action, plainFountain(action).toStdString()});
    }
    return sp;
}

std::string fdxToFountain(const std::string& xml) {
    QXmlStreamReader r(QByteArray::fromStdString(xml));
    QString out, type, text;
    bool inParagraph = false;
    while (!r.atEnd()) {
        r.readNext();
        if (r.isStartElement() && r.name() == QLatin1String("Paragraph")) {
            inParagraph = true;
            type = r.attributes().value(QLatin1String("Type")).toString();
            text.clear();
        } else if (inParagraph && r.isStartElement() && r.name() == QLatin1String("Text")) {
            text += r.readElementText(QXmlStreamReader::IncludeChildElements);
        } else if (r.isEndElement() && r.name() == QLatin1String("Paragraph")) {
            inParagraph = false;
            text = text.simplified();
            if (text.isEmpty()) continue;
            // (each forced, so Fountain reads it as Final Draft says it is)
            if (type == QLatin1String("Scene Heading")) out += QStringLiteral("\n.") + text + '\n';
            else if (type == QLatin1String("Character")) out += QStringLiteral("\n@") + text + '\n';
            else if (type == QLatin1String("Parenthetical")) out += (text.startsWith('(') ? text : '(' + text + ')') + '\n';
            else if (type == QLatin1String("Dialogue")) out += text + '\n';
            else if (type == QLatin1String("Transition")) out += QStringLiteral("\n>") + text + '\n';
            else out += QStringLiteral("\n!") + text + '\n';
        }
    }
    return out.toStdString();
}

std::string scriptFormat(const std::string& fileName, const std::string& text) {
    const QString name = QString::fromStdString(fileName).toLower();
    const QString head = QString::fromStdString(text.substr(0, 4096));
    if (name.endsWith(QLatin1String(".fdx")) || head.contains(QLatin1String("<FinalDraft"))) return "fdx";
    if (name.endsWith(QLatin1String(".fountain")) || name.endsWith(QLatin1String(".spmd"))) return "fountain";
    static const QRegularExpression titlePage(QStringLiteral("^\\s*Title:"), QRegularExpression::CaseInsensitiveOption),
        forcedCue(QStringLiteral("(^|\\n)@\\S"));
    return titlePage.match(head).hasMatch() || forcedCue.match(head).hasMatch() ? "fountain" : "text";
}

std::vector<ScriptLine> scriptLines(const std::string& text, const std::string& fileName) {
    ProjectScript s;
    s.format = scriptFormat(fileName, text);
    s.text = text;
    return readScreenplay(s).lines;
}

Screenplay readScreenplay(const ProjectScript& s) {
    if (s.format == "fdx") return parseFountain(fdxToFountain(s.text));
    if (s.format == "fountain") return parseFountain(s.text);
    return plainScreenplay(s.text);
}

std::vector<ScriptMatch> matchScript(const Project& p, const std::vector<ScriptLine>& lines, const ScriptCutOptions& o) {
    std::vector<Flat> flats;
    for (size_t mi = 0; mi < p.media.size(); ++mi) {
        const MediaItem& m = p.media[mi];
        if (!m.transcript || m.transcript->empty()) continue;
        if (!o.media.empty() && std::find(o.media.begin(), o.media.end(), m.id) == o.media.end()) continue;
        Flat f;
        f.mediaId = m.id;
        f.order = int(mi);
        f.t = m.transcript.get();
        for (const auto& seg : m.transcript->segments)
            for (const auto& w : seg.words) {
                const QString n = norm(QString::fromStdString(w.text));
                if (n.isEmpty()) continue;
                f.index[n].push_back(int(f.norm.size()));
                f.norm.push_back(n);
                f.words.push_back(&w);
                f.speaker.push_back(seg.speaker);
            }
        if (!f.norm.empty()) flats.push_back(std::move(f));
    }

    std::vector<ScriptMatch> out;
    for (const ScriptLine& line : lines) {
        ScriptMatch match;
        match.line = line;
        const std::vector<QString> want = words(QString::fromStdString(line.text));
        if (want.empty()) {
            out.push_back(std::move(match));
            continue;
        }
        const int m = int(want.size());
        std::vector<QString> anchors;
        for (const QString& w : want)
            if (!isStopword(w)) anchors.push_back(w);
        if (anchors.empty()) anchors = want;
        const QString wantSpeaker = nameKey(line.speaker);
        for (const Flat& f : flats) {
            const int n = int(f.norm.size());
            // Windows around the places the line's telling words are said.
            std::vector<std::pair<int, int>> windows;
            const int pad = m + 4;
            for (const QString& a : anchors)
                if (auto it = f.index.find(a); it != f.index.end())
                    for (int j : it->second) windows.push_back({std::max(0, j - pad), std::min(n, j + pad + 1)});
            std::sort(windows.begin(), windows.end());
            std::vector<std::pair<int, int>> merged;
            for (const auto& w : windows) {
                if (!merged.empty() && w.first <= merged.back().second) merged.back().second = std::max(merged.back().second, w.second);
                else merged.push_back(w);
            }
            std::vector<char> blocked(size_t(n), 0);
            for (const auto& [a, b] : merged) {
                // Every reading in the window: the best, then the best of what is left.
                for (int guard = 0; guard < 16; ++guard) {
                    const Hit hit = align(want, f, a, b, blocked);
                    if (hit.first < 0) break;
                    const double coverage = double(hit.matched) / m;
                    if (coverage < o.minCoverage) break;
                    std::fill(blocked.begin() + hit.first, blocked.begin() + hit.last + 1, char(1));
                    ScriptTake take;
                    take.mediaId = f.mediaId;
                    take.start = f.words[size_t(hit.first)]->start;
                    take.end = f.words[size_t(hit.last)]->end;
                    take.coverage = coverage;
                    take.extraWords = hit.last - hit.first + 1 - hit.matched;
                    take.score = coverage * 100 - take.extraWords * 4;
                    // Who says it: the speaker of most of its words.
                    std::map<int, int> votes;
                    for (int j = hit.first; j <= hit.last; ++j) ++votes[f.speaker[size_t(j)]];
                    const int spk = std::max_element(votes.begin(), votes.end(), [](auto& x, auto& y) { return x.second < y.second; })->first;
                    take.speaker = speakerName(*f.t, spk);
                    const bool named = spk >= 0 && size_t(spk) < f.t->speakerNames.size() && !f.t->speakerNames[size_t(spk)].empty();
                    if (named && !wantSpeaker.isEmpty()) take.score += nameKey(take.speaker) == wantSpeaker ? 5 : -10;
                    match.takes.push_back(take);
                }
            }
        }
        // Best first; among equals, the later take (later media, later in the media).
        std::map<Id, int> order;
        for (const Flat& f : flats) order[f.mediaId] = f.order;
        std::stable_sort(match.takes.begin(), match.takes.end(), [&](const ScriptTake& x, const ScriptTake& y) {
            if (std::fabs(x.score - y.score) > 1e-9) return x.score > y.score;
            if (order[x.mediaId] != order[y.mediaId]) return order[x.mediaId] > order[y.mediaId];
            return x.start > y.start;
        });
        out.push_back(std::move(match));
    }
    return out;
}

ScriptCutResult buildScriptCut(Project& p, const std::vector<ScriptMatch>& matches, const std::string& name, const ScriptCutOptions& o) {
    ScriptCutResult res;
    const ScriptTake* firstTake = nullptr;
    for (const ScriptMatch& m : matches)
        if (!m.takes.empty()) {
            firstTake = &m.takes.front();
            break;
        }
    if (!firstTake) {
        res.missing = int(matches.size());
        return res;
    }
    // Sized like the active sequence, or like the first take.
    int w = 1920, h = 1080;
    Rational fps{30, 1};
    const Sequence* like = p.active();
    if (like) {
        w = like->width, h = like->height, fps = like->fps;
    } else if (const MediaItem* m = p.findMedia(firstTake->mediaId); m && m->width > 0) {
        w = m->width, h = m->height;
        if (m->fps.valid()) fps = m->fps;
    }
    Sequence s = makeSequence(p, name, w, h, fps, 1, 1);
    if (like) {
        s.sampleRate = like->sampleRate;
        s.colorSpace = like->colorSpace;
        s.hdrPeakNits = like->hdrPeakNits;
    }
    const double rate = s.fpsValue();
    const int red = std::max(0, labelFromName("Red"));

    auto range = [&](const ScriptTake& t, double& in, double& out) {
        in = std::max(0.0, t.start - o.handle) * rate;
        out = (t.end + o.handle) * rate;
        if (const MediaItem* m = p.findMedia(t.mediaId); m && m->duration > 0) out = std::min(out, m->duration * rate);
    };
    auto freeOn = [](const Track& t, FrameTime a, FrameTime b) {
        for (const Clip& c : t.clips)
            if (c.start < b && a < c.start + c.duration) return false;
        return true;
    };
    auto shortText = [](const ScriptLine& l) {
        QString t = QString::fromStdString(l.text).simplified();
        if (t.size() > 48) t = t.left(47) + QChar(0x2026);
        return (l.speaker.empty() ? QString() : QString::fromStdString(l.speaker) + QStringLiteral(": ")) + t;
    };

    FrameTime cursor = 0;
    for (const ScriptMatch& m : matches) {
        if (m.takes.empty()) {
            ++res.missing;
            if (o.markers) s.markers.push_back({cursor, 0, "Missing: " + shortText(m.line).toStdString(), m.line.text, red});
            continue;
        }
        double in = 0, out = 0;
        range(m.takes.front(), in, out);
        const edit::Result r = edit::placeMedia(p, s, m.takes.front().mediaId, cursor, in, out, {TrackKind::Video, 0},
                                                {TrackKind::Audio, 0}, false);
        if (!r.ok || r.created.empty()) {
            ++res.missing;
            continue;
        }
        const Clip* placed = edit::clipById(s, r.created.front());
        const FrameTime len = placed ? placed->duration : 1;
        ++res.placed;
        if (o.markers) {
            std::string comment = m.line.text;
            if (m.takes.size() > 1) comment += "\n" + std::to_string(m.takes.size()) + " takes";
            s.markers.push_back({cursor, len, shortText(m.line).toStdString(), comment, 0});
        }
        // Alternates on the lowest tracks above that are free for their whole length.
        const int alts = std::min<int>(o.maxAlternates, int(m.takes.size()) - 1);
        for (int k = 1; k <= alts; ++k) {
            const ScriptTake& t = m.takes[size_t(k)];
            double ain = 0, aout = 0;
            range(t, ain, aout);
            const FrameTime alen = std::max<FrameTime>(1, FrameTime(std::llround(aout - ain)));
            int ti = 1;
            while (ti < int(s.videoTracks.size()) &&
                   !(freeOn(s.videoTracks[size_t(ti)], cursor, cursor + alen) &&
                     (ti >= int(s.audioTracks.size()) || freeOn(s.audioTracks[size_t(ti)], cursor, cursor + alen))))
                ++ti;
            while (int(s.videoTracks.size()) <= ti)
                s.videoTracks.push_back(makeTrack(p, TrackKind::Video, "V" + std::to_string(s.videoTracks.size() + 1)));
            while (int(s.audioTracks.size()) <= ti)
                s.audioTracks.push_back(makeTrack(p, TrackKind::Audio, "A" + std::to_string(s.audioTracks.size() + 1)));
            const edit::Result ar = edit::placeMedia(p, s, t.mediaId, cursor, ain, aout, {TrackKind::Video, ti},
                                                     {TrackKind::Audio, ti}, false);
            if (!ar.ok) continue;
            for (Id id : ar.created)
                if (Clip* c = edit::clipById(s, id)) c->enabled = false;
            ++res.alternates;
        }
        cursor += len;
    }
    if (res.placed == 0) return res;
    res.sequence = s.id;
    MediaItem item;
    item.id = p.newId();
    item.kind = MediaKind::Sequence;
    item.name = s.name;
    item.sequenceId = s.id;
    item.hasVideo = item.hasAudio = true;
    item.width = s.width;
    item.height = s.height;
    item.fps = s.fps;
    p.sequences.push_back(std::move(s));
    p.media.push_back(std::move(item));
    return res;
}

}  // namespace montage

namespace montage {

int matchProjectScript(Project& p, const ScriptCutOptions& o) {
    p.script.readings.clear();
    if (p.script.text.empty()) return 0;
    const Screenplay sp = readScreenplay(p.script);
    int found = 0;
    for (const ScriptMatch& m : matchScript(p, sp.lines, o)) {
        // Every reading, best first (a take may say the line more than once: a reset without a cut).
        std::vector<ScriptReading> line;
        for (const ScriptTake& t : m.takes) {
            line.push_back({t.mediaId, t.start, t.end, t.coverage, t.extraWords, t.score});
            if (line.size() >= 32) break;
        }
        found += line.empty() ? 0 : 1;
        p.script.readings.push_back(std::move(line));
    }
    return found;
}

int setProjectScript(Project& p, const std::string& name, const std::string& format, const std::string& text, const ScriptCutOptions& o) {
    p.script = {};
    if (QString::fromStdString(text).trimmed().isEmpty()) return 0;
    p.script.name = name;
    p.script.format = format.empty() ? scriptFormat(name, text) : format;
    p.script.text = text;
    return matchProjectScript(p, o);
}

namespace {

using Played = std::map<Id, std::vector<std::pair<double, double>>>;

// What the sequence's enabled clips play of each media item between `from` and `to` (its own seconds), in media
// seconds; through compound clips into their sequences, and through multicam clips into the angle they show.
void collectPlayed(const Project& p, const Sequence& s, double from, double to, Played& out, int depth, int onlyVideo = -2,
                   int onlyAudio = -2) {
    const double fps = s.fpsValue() > 0 ? s.fpsValue() : 30.0;
    auto walk = [&](const std::vector<Track>& tracks, bool video) {
        for (size_t ti = 0; ti < tracks.size(); ++ti) {
            const int only = video ? onlyVideo : onlyAudio;
            if (only == -1 || (only >= 0 && int(ti) != only)) continue;
            for (const Clip& c : tracks[ti].clips) {
                if (!c.enabled || !c.mediaId) continue;
                const double a = std::max(from, c.start / fps), b = std::min(to, c.end() / fps);
                if (b <= a) continue;
                double ma = c.sourceAt(a * fps - double(c.start)) / fps, mb = c.sourceAt(b * fps - double(c.start)) / fps;
                if (mb < ma) std::swap(ma, mb);
                const MediaItem* m = p.findMedia(c.mediaId);
                const Sequence* inner = m && m->sequenceId ? p.findSequence(m->sequenceId) : nullptr;
                if (inner && inner != &s && depth < 4) {
                    if (!inner->multicam) collectPlayed(p, *inner, ma, mb, out, depth + 1);
                    else if (video) collectPlayed(p, *inner, ma, mb, out, depth + 1, c.angle, -1);
                    else collectPlayed(p, *inner, ma, mb, out, depth + 1, -1, c.audioAngle >= 0 ? c.audioAngle : -2);
                    continue;
                }
                out[c.mediaId].push_back({ma, mb});
            }
        }
    };
    walk(s.videoTracks, true);
    walk(s.audioTracks, false);
}

}  // namespace

std::vector<int> scriptLinesInCut(const Project& p, const Sequence& s) {
    std::vector<int> out(p.script.readings.size(), -1);
    Played played;
    collectPlayed(p, s, -1e18, 1e18, played, 0);
    for (size_t line = 0; line < out.size(); ++line) {
        const auto& readings = p.script.readings[line];
        for (size_t k = 0; k < readings.size() && out[line] < 0; ++k) {
            const ScriptReading& r = readings[k];
            const auto it = played.find(r.media);
            if (it == played.end()) continue;
            const double len = std::max(1e-3, r.end - r.start);
            for (const auto& [a, b] : it->second)
                if (std::min(b, r.end) - std::max(a, r.start) >= len * 0.5) {
                    out[line] = int(k);
                    break;
                }
        }
    }
    return out;
}

bool isCircledTake(const MediaItem& m) {
    const auto it = m.metadata.find("circled");
    if (it == m.metadata.end()) return false;
    const QString v = QString::fromStdString(it->second).trimmed().toLower();
    return v == QLatin1String("yes") || v == QLatin1String("true") || v == QLatin1String("1");
}

}  // namespace montage
