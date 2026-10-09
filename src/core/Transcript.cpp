#include "Transcript.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace montage {

std::string Transcript::text() const {
    std::string out;
    for (const auto& s : segments) {
        if (!out.empty() && !s.text.empty()) out += ' ';
        out += s.text;
    }
    return out;
}

size_t Transcript::wordCount() const {
    size_t n = 0;
    for (const auto& s : segments) n += s.words.size();
    return n;
}

void applySpeakers(Transcript& t, const std::vector<SpeakerTurn>& turns) {
    auto speakerAt = [&](double a, double b) {
        double best = 0;
        int who = -1;
        for (const SpeakerTurn& turn : turns) {
            const double overlap = std::min(b, turn.end) - std::max(a, turn.start);
            if (overlap > best) {
                best = overlap;
                who = turn.speaker;
            }
        }
        if (who >= 0) return who;
        double gap = 1.0;
        for (const SpeakerTurn& turn : turns) {
            const double g = std::max(turn.start - b, a - turn.end);
            if (g < gap) {
                gap = g;
                who = turn.speaker;
            }
        }
        return who;
    };
    std::vector<TranscriptSegment> out;
    for (TranscriptSegment& s : t.segments) {
        if (s.words.empty()) {
            s.speaker = speakerAt(s.start, s.end);
            out.push_back(std::move(s));
            continue;
        }
        const size_t n = s.words.size();
        std::vector<int> who(n);
        for (size_t i = 0; i < n; ++i) who[i] = speakerAt(s.words[i].start, s.words[i].end);
        for (size_t i = 1; i + 1 < n; ++i)
            if (who[i - 1] == who[i + 1] && who[i] != who[i - 1]) who[i] = who[i - 1];
        // Words in nobody's turn go with the speaker before them (or after, at the start).
        for (size_t i = 1; i < n; ++i)
            if (who[i] < 0) who[i] = who[i - 1];
        for (size_t i = n - 1; i-- > 0;)
            if (who[i] < 0) who[i] = who[i + 1];
        if (std::all_of(who.begin(), who.end(), [&](int w) { return w == who[0]; })) {
            s.speaker = who[0];
            out.push_back(std::move(s));
            continue;
        }
        for (size_t i = 0; i < n;) {
            size_t e = i;
            TranscriptSegment part;
            part.speaker = who[i];
            for (; e < n && who[e] == who[i]; ++e) {
                if (!part.text.empty()) part.text += ' ';
                part.text += s.words[e].text;
                part.words.push_back(s.words[e]);
            }
            part.start = part.words.front().start;
            part.end = part.words.back().end;
            out.push_back(std::move(part));
            i = e;
        }
    }
    t.segments = std::move(out);
}

std::string speakerName(const Transcript& t, int speaker) {
    if (speaker < 0) return {};
    if (size_t(speaker) < t.speakerNames.size() && !t.speakerNames[size_t(speaker)].empty()) return t.speakerNames[size_t(speaker)];
    return "Speaker " + std::to_string(speaker + 1);
}

int speakerCount(const Transcript& t) {
    int n = int(t.speakerNames.size());
    for (const auto& s : t.segments) n = std::max(n, s.speaker + 1);
    return n;
}

std::string transcriptToJson(const Transcript& t) {
    QJsonArray segs;
    for (const auto& s : t.segments) {
        QJsonArray words;
        for (const auto& w : s.words)
            words.append(QJsonArray{QString::fromStdString(w.text), w.start, w.end, double(w.probability)});
        QJsonObject o{{"start", s.start}, {"end", s.end}, {"text", QString::fromStdString(s.text)}, {"words", words}};
        if (s.speaker >= 0) o["speaker"] = s.speaker;
        segs.append(o);
    }
    QJsonObject root{{"language", QString::fromStdString(t.language)},
                     {"model", QString::fromStdString(t.model)},
                     {"segments", segs}};
    if (!t.speakerNames.empty()) {
        QJsonArray names;
        for (const auto& name : t.speakerNames) names.append(QString::fromStdString(name));
        root["speakers"] = names;
    }
    return QJsonDocument(root).toJson(QJsonDocument::Compact).toStdString();
}

bool transcriptFromJson(const std::string& json, Transcript& out, std::string* error) {
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(json), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error) *error = "invalid transcript";
        return false;
    }
    const QJsonObject root = doc.object();
    Transcript t;
    t.language = root.value("language").toString().toStdString();
    t.model = root.value("model").toString().toStdString();
    for (const QJsonValue& name : root.value("speakers").toArray()) t.speakerNames.push_back(name.toString().toStdString());
    for (const QJsonValue& sv : root.value("segments").toArray()) {
        const QJsonObject o = sv.toObject();
        TranscriptSegment s;
        s.start = o.value("start").toDouble();
        s.end = o.value("end").toDouble();
        s.text = o.value("text").toString().toStdString();
        s.speaker = o.value("speaker").toInt(-1);
        for (const QJsonValue& wv : o.value("words").toArray()) {
            const QJsonArray a = wv.toArray();
            if (a.size() < 3) continue;
            TranscriptWord w;
            w.text = a.at(0).toString().toStdString();
            w.start = a.at(1).toDouble();
            w.end = a.at(2).toDouble();
            w.probability = float(a.at(3).toDouble(1));
            s.words.push_back(w);
        }
        t.segments.push_back(std::move(s));
    }
    out = std::move(t);
    return true;
}

std::vector<Cue> transcriptCues(const Transcript& t, int maxChars, double maxSeconds) {
    std::vector<Cue> cues;
    Cue cur;
    bool open = false;
    double lastEnd = 0;
    int speaker = -1;
    auto flush = [&] {
        if (open && !cur.text.empty()) cues.push_back(cur);
        open = false;
        cur = Cue{};
    };
    for (const auto& s : t.segments) {
        // A new speaker starts a new cue.
        if (open && s.speaker != speaker) flush();
        speaker = s.speaker;
        // Segments without word timings become words spread evenly over the segment.
        std::vector<TranscriptWord> words = s.words;
        if (words.empty()) {
            const QStringList parts = QString::fromStdString(s.text).split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
            const double step = parts.isEmpty() ? 0 : (s.end - s.start) / parts.size();
            for (int i = 0; i < parts.size(); ++i)
                words.push_back({s.start + i * step, s.start + (i + 1) * step, parts[i].toStdString(), 1});
        }
        for (const auto& w : words) {
            if (w.text.empty()) continue;
            const int len = int(QString::fromStdString(cur.text).size()) + (cur.text.empty() ? 0 : 1) +
                            int(QString::fromStdString(w.text).size());
            const bool pause = open && w.start - lastEnd > 1.0;
            if (open && (len > maxChars || w.end - cur.start > maxSeconds || pause)) flush();
            if (!open) {
                cur.start = w.start;
                cur.voice = speakerName(t, s.speaker);
                open = true;
            }
            if (!cur.text.empty()) cur.text += ' ';
            cur.text += w.text;
            cur.end = w.end;
            lastEnd = w.end;
            // End a cue at the end of a sentence once it has some length.
            const char last = w.text.back();
            if ((last == '.' || last == '?' || last == '!') && int(cur.text.size()) >= maxChars / 2) flush();
        }
    }
    flush();
    return cues;
}

namespace {
std::string stamp(double t, char sep) {
    if (t < 0) t = 0;
    const long long ms = std::llround(t * 1000);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02lld:%02lld:%02lld%c%03lld", ms / 3600000, (ms / 60000) % 60, (ms / 1000) % 60, sep,
                  ms % 1000);
    return buf;
}
}  // namespace

std::string cuesToSrt(const std::vector<Cue>& cues, double offset) {
    std::string out;
    int n = 1;
    for (const Cue& c : cues) {
        out += std::to_string(n++) + "\n" + stamp(c.start + offset, ',') + " --> " + stamp(c.end + offset, ',') + "\n" + c.text +
               "\n\n";
    }
    return out;
}

std::string cuesToVtt(const std::vector<Cue>& cues, double offset) {
    std::string out = "WEBVTT\n\n";
    for (const Cue& c : cues)
        out += stamp(c.start + offset, '.') + " --> " + stamp(c.end + offset, '.') + (c.settings.empty() ? "" : " " + c.settings) + "\n" +
               (c.voice.empty() ? std::string() : "<v " + c.voice + ">") + c.text + "\n\n";
    return out;
}

std::string transcriptAs(const Transcript& t, const std::string& format, double offset) {
    const std::string f = QString::fromStdString(format).toLower().toStdString();
    if (f == "srt") return cuesToSrt(transcriptCues(t), offset);
    if (f == "vtt") return cuesToVtt(transcriptCues(t), offset);
    if (f == "json") return transcriptToJson(t);
    if (f == "txt") {
        // With speakers: a paragraph per turn, under the speaker's name.
        std::string out;
        int last = -2;
        for (const auto& s : t.segments) {
            if (s.speaker >= 0 && s.speaker != last) out += (out.empty() ? "" : "\n") + speakerName(t, s.speaker) + ":\n";
            last = s.speaker;
            out += s.text + "\n";
        }
        return out;
    }
    return {};
}

std::vector<std::pair<double, double>> findPhrase(const Transcript& t, const std::string& phrase) {
    auto norm = [](const QString& s) {
        QString n = s.toLower();
        n.remove(QRegularExpression(QStringLiteral("[^\\w']")));
        return n;
    };
    QStringList want;
    for (const QString& p : QString::fromStdString(phrase).split(QRegularExpression("\\s+"), Qt::SkipEmptyParts))
        if (const QString n = norm(p); !n.isEmpty()) want << n;
    std::vector<std::pair<double, double>> hits;
    if (want.isEmpty()) return hits;
    std::vector<const TranscriptWord*> words;
    for (const auto& s : t.segments)
        for (const auto& w : s.words) words.push_back(&w);
    std::vector<QString> normed;
    normed.reserve(words.size());
    for (const auto* w : words) normed.push_back(norm(QString::fromStdString(w->text)));
    for (size_t i = 0; i + size_t(want.size()) <= words.size(); ++i) {
        bool match = true;
        for (int k = 0; k < want.size() && match; ++k) match = normed[i + size_t(k)] == want[k];
        if (match) hits.emplace_back(words[i]->start, words[i + size_t(want.size()) - 1]->end);
    }
    return hits;
}

}  // namespace montage
