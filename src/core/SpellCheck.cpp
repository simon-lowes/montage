#include "SpellCheck.h"

#include <QFile>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <algorithm>
#include <functional>
#include <map>
#include <mutex>

namespace montage {

namespace {

QString normalised(QString w) { return w.replace(QChar(0x2019), QLatin1Char('\'')).replace(QChar(0x2018), QLatin1Char('\'')); }

enum class Case { Lower, Capitalised, Upper, Mixed };

Case caseOf(const QString& w) {
    bool anyUpper = false, anyLower = false, restLower = true;
    for (int i = 0; i < w.size(); ++i) {
        if (!w[i].isLetter()) continue;
        if (w[i].isUpper()) {
            anyUpper = true;
            if (i > 0) restLower = false;
        } else {
            anyLower = true;
        }
    }
    if (!anyUpper) return Case::Lower;
    if (!anyLower) return Case::Upper;
    return w[0].isUpper() && restLower ? Case::Capitalised : Case::Mixed;
}

QString inCase(const QString& s, Case c) {
    if (c == Case::Upper) return s.toUpper();
    if (c == Case::Capitalised && !s.isEmpty() && s == s.toLower()) return s.left(1).toUpper() + s.mid(1);
    return s;
}

bool loadList(const QString& path, const std::function<void(const QString&, int)>& add) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    int level = 60;
    while (!f.atEnd()) {
        const QString line = QString::fromUtf8(f.readLine()).trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;
        if (line.startsWith('=')) {
            level = line.mid(1).toInt();
            continue;
        }
        add(line, level);
    }
    return true;
}

// The words of `extra` (vocabulary terms), lower case.
std::unordered_set<std::string> extraWords(const std::vector<std::string>& extra) {
    std::unordered_set<std::string> out;
    for (const std::string& term : extra)
        for (const QString& w : normalised(QString::fromStdString(term)).split(QRegularExpression(QStringLiteral("[\\s\\-]+")), Qt::SkipEmptyParts)) {
            QString t = w;
            while (!t.isEmpty() && !t.back().isLetterOrNumber()) t.chop(1);
            while (!t.isEmpty() && !t.front().isLetterOrNumber()) t.remove(0, 1);
            if (!t.isEmpty()) out.insert(t.toLower().toStdString());
        }
    return out;
}

}  // namespace

std::string SpellChecker::dictionaryFor(const std::string& language) {
    QString l = QString::fromStdString(language).trimmed().toLower().replace('_', '-');
    if (l.isEmpty() || l == "en" || l == "eng" || l == "english" || l.startsWith("en-us")) return "en-US";
    if (l.startsWith("en-gb") || l.startsWith("en-uk") || l.startsWith("en-ie") || l.startsWith("en-au") || l.startsWith("en-nz") ||
        l.startsWith("en-za") || l.startsWith("en-in"))
        return "en-GB";
    if (l.startsWith("en-")) return "en-US";
    return "";
}

const SpellChecker* SpellChecker::forLanguage(const std::string& language) {
    static std::mutex m;
    static std::map<std::string, std::unique_ptr<SpellChecker>> loaded;
    const std::string dict = dictionaryFor(language);
    if (dict.empty()) return nullptr;
    std::lock_guard lock(m);
    auto& slot = loaded[dict];
    if (!slot) {
        slot.reset(new SpellChecker(dict));
        if (slot->exact_.empty()) {
            slot.reset();
            loaded.erase(dict);
            return nullptr;
        }
    }
    return slot.get();
}

SpellChecker::SpellChecker(const std::string& dictionary) : language_(dictionary) {
    auto add = [this](const QString& w, int level) {
        const std::string s = w.toStdString();
        if (!exact_.insert(s).second) return;
        upper_.insert(w.toUpper().toStdString());
        if (w == w.toLower()) lowerLevel_.emplace(s, level);
    };
    loadList(QStringLiteral(":/montage/dict/english.txt"), add);
    loadList(dictionary == "en-GB" ? QStringLiteral(":/montage/dict/british.txt") : QStringLiteral(":/montage/dict/american.txt"), add);
}

int SpellChecker::level(const std::string& lower) const {
    const auto it = lowerLevel_.find(lower);
    return it == lowerLevel_.end() ? 0 : it->second;
}

bool SpellChecker::known(const std::string& word) const {
    if (exact_.count(word)) return true;
    const QString w = QString::fromStdString(word);
    switch (caseOf(w)) {
        case Case::Capitalised: return lowerLevel_.count(w.toLower().toStdString()) > 0;  // a sentence's first word
        case Case::Upper: return upper_.count(word) > 0;
        default: return false;
    }
}

bool SpellChecker::correct(const std::string& word, const std::vector<std::string>& extra) const {
    const QString w = normalised(QString::fromStdString(word));
    if (w.isEmpty()) return true;
    const std::string s = w.toStdString();
    if (known(s)) return true;
    if (!extra.empty() && extraWords(extra).count(w.toLower().toStdString())) return true;
    // Possessives: "editor's", "James's", "editors'".
    if (w.endsWith(QLatin1String("'s"), Qt::CaseInsensitive) && w.size() > 2) return correct(w.chopped(2).toStdString(), extra);
    if (w.endsWith('\'') && w.size() > 1) return correct(w.chopped(1).toStdString(), extra);
    return false;
}

std::vector<std::string> SpellChecker::suggest(const std::string& word, int max) const {
    const QString w = normalised(QString::fromStdString(word));
    const Case c = caseOf(w);
    const std::string lw = w.toLower().toStdString();
    // Every entry by its lower case: its listed form and size.
    static std::mutex m;
    std::lock_guard lock(m);
    if (anyCase_.empty())
        for (const std::string& e : exact_) {
            const std::string lo = QString::fromStdString(e).toLower().toStdString();
            const int lv = lowerLevel_.count(e) ? lowerLevel_.at(e) : 60;
            auto it = anyCase_.find(lo);
            if (it == anyCase_.end() || lowerLevel_.count(e)) anyCase_[lo] = {e, lv};
        }
    std::map<std::string, double> score;  // listed form -> cost (lower is better)
    auto offer = [&](const std::string& cand, double cost) {
        const auto it = anyCase_.find(cand);
        if (it == anyCase_.end()) return;
        const double total = cost + it->second.second / 200.0;  // commoner words first
        auto [s, fresh] = score.emplace(it->second.first, total);
        if (!fresh) s->second = std::min(s->second, total);
    };
    static const std::string letters = "abcdefghijklmnopqrstuvwxyz'";
    // One edit, each kind weighed: swapped neighbours and doubled or undoubled letters are the commonest slips.
    auto edits = [&](const std::string& s, double base, const std::function<void(const std::string&, double)>& visit) {
        const size_t n = s.size();
        for (size_t i = 0; i < n; ++i) {  // deletes
            const bool doubled = (i > 0 && s[i - 1] == s[i]) || (i + 1 < n && s[i + 1] == s[i]);
            visit(s.substr(0, i) + s.substr(i + 1), base + (doubled ? 0.8 : 1.0));
        }
        for (size_t i = 0; i + 1 < n; ++i)  // transposes
            if (s[i] != s[i + 1]) {
                std::string t = s;
                std::swap(t[i], t[i + 1]);
                visit(t, base + 0.7);
            }
        for (size_t i = 0; i < n; ++i)  // replaces
            for (char ch : letters)
                if (ch != s[i]) {
                    std::string t = s;
                    t[i] = ch;
                    visit(t, base + 1.0);
                }
        for (size_t i = 0; i <= n; ++i)  // inserts
            for (char ch : letters) {
                const bool doubling = (i > 0 && s[i - 1] == ch) || (i < n && s[i] == ch);
                visit(s.substr(0, i) + ch + s.substr(i), base + (doubling ? 0.8 : 1.0));
            }
    };
    offer(lw, 0.0);  // the right word in the wrong case ("paris")
    edits(lw, 0.0, [&](const std::string& t, double cost) { offer(t, cost); });
    // Run together: "alot", "eachother".
    for (size_t i = 1; i < lw.size(); ++i) {
        const std::string a = lw.substr(0, i), b = lw.substr(i);
        const int la = level(a), lb = level(b);
        if (la && lb && la <= 50 && lb <= 50 && (a.size() > 1 || a == "a")) score.emplace(a + " " + b, 0.9 + std::max(la, lb) / 200.0);
    }
    bool close = false;
    for (const auto& [s, cost] : score) close = close || cost < 1.5;
    if (!close && lw.size() >= 3 && lw.size() <= 24)
        edits(lw, 0.0, [&](const std::string& t, double cost) { edits(t, cost, [&](const std::string& u, double cost2) { offer(u, cost2 + 0.2); }); });
    std::vector<std::pair<double, std::string>> ranked;
    for (const auto& [s, cost] : score)
        if (s != w.toStdString()) ranked.push_back({cost, s});
    std::sort(ranked.begin(), ranked.end());
    std::vector<std::string> out;
    for (const auto& [cost, s] : ranked) {
        const std::string shown = inCase(QString::fromStdString(s), c).toStdString();
        if (std::find(out.begin(), out.end(), shown) == out.end()) out.push_back(shown);
        if (int(out.size()) >= max) break;
    }
    return out;
}

std::vector<Misspelling> SpellChecker::check(const QString& textIn, const std::vector<std::string>& extra, bool suggestions,
                                             bool ignoreUppercase) const {
    std::vector<Misspelling> out;
    const QString text = normalised(textIn);
    const std::unordered_set<std::string> mine = extraWords(extra);
    const int n = int(text.size());
    auto isApostrophe = [](QChar ch) { return ch == '\''; };
    auto judge = [&](int start, int len) {
        const QString w = text.mid(start, len);
        if (w.size() < 2) return;
        if (ignoreUppercase && caseOf(w) == Case::Upper) return;  // acronyms
        if (mine.count(w.toLower().toStdString())) return;
        QString stem = w;
        if (stem.endsWith(QLatin1String("'s"), Qt::CaseInsensitive)) stem.chop(2);
        if (mine.count(stem.toLower().toStdString())) return;
        if (correct(w.toStdString())) return;
        Misspelling ms;
        ms.start = start, ms.length = len, ms.word = w.toStdString();
        if (suggestions) ms.suggestions = suggest(ms.word);
        out.push_back(std::move(ms));
    };
    int i = 0;
    while (i < n) {
        if (!text[i].isLetterOrNumber()) {
            ++i;
            continue;
        }
        // A token: letters and digits, with apostrophes and hyphens between them.
        int e = i;
        bool digit = false;
        while (e < n) {
            if (text[e].isLetterOrNumber()) {
                digit = digit || text[e].isDigit();
                ++e;
            } else if ((isApostrophe(text[e]) || text[e] == '-') && e + 1 < n && text[e + 1].isLetter()) {
                ++e;
            } else {
                break;
            }
        }
        if (e < n && isApostrophe(text[e]) && e > i && (text[e - 1] == 's' || text[e - 1] == 'S')) ++e;  // "editors'"
        // The whitespace-separated chunk it is part of: web and mail addresses are left alone.
        int cs = i, ce = e;
        while (cs > 0 && !text[cs - 1].isSpace()) --cs;
        while (ce < n && !text[ce].isSpace()) ++ce;
        const QString chunk = text.mid(cs, ce - cs);
        const bool address = chunk.contains(QLatin1String("://")) || chunk.contains('@') || chunk.startsWith(QLatin1String("www."), Qt::CaseInsensitive) ||
                             chunk.contains(QRegularExpression(QStringLiteral("\\.(com|org|net|io|co|uk|tv)\\b"), QRegularExpression::CaseInsensitiveOption));
        if (!digit && !address) {
            const QString token = text.mid(i, e - i);
            if (token.contains('-') && !correct(token.toStdString(), extra)) {
                // Hyphenated: each part on its own.
                int p = i;
                for (const QString& part : token.split('-')) {
                    judge(p, int(part.size()));
                    p += int(part.size()) + 1;
                }
            } else if (!token.contains('-')) {
                judge(i, e - i);
            }
        }
        i = e;
    }
    return out;
}

std::vector<Misspelling> SpellChecker::check(const std::string& text, const std::vector<std::string>& extra, bool suggestions,
                                             bool ignoreUppercase) const {
    return check(QString::fromStdString(text), extra, suggestions, ignoreUppercase);
}

bool learnWord(Project& p, const std::string& word) {
    const QString w = normalised(QString::fromStdString(word)).trimmed();
    if (w.isEmpty()) return false;
    for (const std::string& v : p.vocabulary)
        if (QString::fromStdString(v).compare(w, Qt::CaseInsensitive) == 0) return false;
    p.vocabulary.push_back(w.toStdString());
    return true;
}

bool forgetWord(Project& p, const std::string& word) {
    const QString w = normalised(QString::fromStdString(word)).trimmed();
    const size_t before = p.vocabulary.size();
    p.vocabulary.erase(std::remove_if(p.vocabulary.begin(), p.vocabulary.end(),
                                      [&](const std::string& v) { return QString::fromStdString(v).compare(w, Qt::CaseInsensitive) == 0; }),
                       p.vocabulary.end());
    return p.vocabulary.size() != before;
}

}  // namespace montage
