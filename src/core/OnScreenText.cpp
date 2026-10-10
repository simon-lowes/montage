#include "OnScreenText.h"

#include <QList>
#include <QString>
#include <algorithm>
#include <cmath>
#include <map>

namespace montage {

namespace {

std::u32string letters(const std::string& s) {
    std::u32string out;
    for (const uint c : QString::fromStdString(s).toLower().toUcs4())
        if (QChar::isLetterOrNumber(char32_t(c))) out.push_back(char32_t(c));
    return out;
}

}  // namespace

double readingSimilarity(const std::string& a, const std::string& b) {
    const std::u32string x = letters(a), y = letters(b);
    if (x.empty() && y.empty()) return 1;
    if (x.empty() || y.empty()) return 0;
    std::vector<size_t> prev(y.size() + 1), cur(y.size() + 1);
    for (size_t j = 0; j <= y.size(); ++j) prev[j] = j;
    for (size_t i = 1; i <= x.size(); ++i) {
        cur[0] = i;
        for (size_t j = 1; j <= y.size(); ++j) cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (x[i - 1] == y[j - 1] ? 0 : 1)});
        std::swap(prev, cur);
    }
    return 1.0 - double(prev[y.size()]) / double(std::max(x.size(), y.size()));
}

std::vector<Caption> captionsFromReadings(const std::vector<std::pair<double, std::string>>& readings, double step, Rational fps,
                                          double minSeconds, double sameAbove) {
    std::vector<Caption> out;
    const double rate = fps.toDouble();
    size_t i = 0;
    while (i < readings.size()) {
        if (QString::fromStdString(readings[i].second).trimmed().isEmpty()) {
            ++i;
            continue;
        }
        // The run of readings alike to its first (and to the one before, so a slow change is followed).
        size_t j = i + 1;
        while (j < readings.size() && !QString::fromStdString(readings[j].second).trimmed().isEmpty() &&
               (readingSimilarity(readings[j].second, readings[i].second) >= sameAbove ||
                readingSimilarity(readings[j].second, readings[j - 1].second) >= std::max(sameAbove, 0.9)))
            ++j;
        // Shown as the reading seen most often (the longest of equals).
        std::map<std::string, int> counts;
        for (size_t k = i; k < j; ++k) ++counts[QString::fromStdString(readings[k].second).simplified().toStdString()];
        std::string best;
        int most = 0;
        for (const auto& [text, n] : counts)
            if (n > most || (n == most && text.size() > best.size())) best = text, most = n;
        const double start = readings[i].first;
        const double end = j < readings.size() ? readings[j].first : readings[j - 1].first + step;
        if (end - start >= minSeconds - 1e-9) {
            Caption c;
            c.start = FrameTime(std::llround(start * rate));
            c.end = std::max(c.start + 1, FrameTime(std::llround(end * rate)));
            c.text = best;
            // Lines read as separate rows keep their breaks.
            for (size_t k = i; k < j; ++k)
                if (QString::fromStdString(readings[k].second).simplified().toStdString() == best && readings[k].second.find('\n') != std::string::npos) {
                    c.text = QString::fromStdString(readings[k].second).trimmed().toStdString();
                    break;
                }
            out.push_back(std::move(c));
        }
        i = j;
    }
    normalizeCaptions(out);
    return out;
}

}  // namespace montage
