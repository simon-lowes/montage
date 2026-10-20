#include "FootageWords.h"

#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <algorithm>
#include <cmath>

#include "Compositor.h"
#include "core/Chapters.h"
#include "core/TextStats.h"
#include "core/TranscriptEdit.h"
#include "media/Analysis.h"
#include "media/Decoder.h"
#include "media/LanguageModel.h"
#include "media/Proxy.h"

namespace montage {

namespace {

const char* kEditorsAssistant =
    "You are an assistant to a film and video editor. You answer exactly in the form asked, in plain English, "
    "and say only what the material supports.";

bool stopped(const std::atomic<bool>* cancel) { return cancel && cancel->load(); }

// The sentences said in the sequence, as text, with where each starts and ends in its words.
struct Said {
    std::vector<TranscriptWord> words;
    std::vector<std::pair<size_t, size_t>> spans;
    std::vector<std::string> sentences;
};

Said whatIsSaid(const Project& p, const Sequence& s) {
    Said out;
    out.words = sequenceTranscriptWords(p, s);
    out.spans = sentenceSpans(out.words);
    for (const auto& [a, b] : out.spans) {
        std::string line;
        for (size_t w = a; w <= b; ++w) line += (line.empty() ? "" : " ") + out.words[w].text;
        out.sentences.push_back(line);
    }
    return out;
}

// The sentences one a line, each cut to `words` words (0 = whole).
std::string sentenceLines(const std::vector<std::string>& sentences, int words, bool numbered) {
    std::string out;
    for (size_t i = 0; i < sentences.size(); ++i) {
        QStringList w = QString::fromStdString(sentences[i]).split(' ', Qt::SkipEmptyParts);
        if (words > 0 && w.size() > words) w = w.mid(0, words);
        if (numbered) out += "[" + std::to_string(i + 1) + "] ";
        out += w.join(' ').toStdString() + "\n";
    }
    return out;
}

// The question with the transcript shortened (each sentence cut to fewer words) until it and the answer fit the
// model's context; empty when even that is too long.
std::string fitQuestion(const LanguageModel& lm, int answerTokens, const std::function<std::string(int)>& question) {
    for (const int words : {0, 30, 20, 14, 10, 7, 5}) {
        std::string q = question(words);
        if (lm.tokenCount(std::string(kEditorsAssistant) + q) + answerTokens + 64 <= lm.contextTokens()) return q;
    }
    return {};
}

QString upperFirst(QString s) {
    if (!s.isEmpty()) s[0] = s[0].toUpper();
    return s;
}

}  // namespace

// ---- Shots ---------------------------------------------------------------------------------------------------------

std::vector<ShotDescription> describeShots(const MediaItem& m, const DescribeShotsOptions& o, const WordsProgress& progress,
                                           const std::atomic<bool>* cancel, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return std::vector<ShotDescription>{};
    };
    const std::string path = analysisSource(m);  // (the proxy when there is one: quicker to read, and there offline)
    if (!m.hasVideo || path.empty()) return fail("Only pictures can be described");
    std::shared_ptr<PictureDescriber> model = PictureDescriber::load(error);
    if (!model) return {};
    // The shots: from cut to cut, a cut too soon after another left out.
    std::vector<std::pair<double, double>> shots;
    VideoDecoder dec;
    if (!dec.open(path, error)) return {};
    const double duration = m.kind == MediaKind::Image ? 0 : (m.duration > 0 ? m.duration : dec.duration());
    if (duration <= 0) {
        shots.push_back({0, 0});
    } else {
        std::string err;
        const std::vector<double> cuts = detectSceneCuts(
            path, o.sensitivity, [&](double f) { if (progress) progress(0.2 * f); }, cancel, &err);
        if (stopped(cancel)) return fail("Cancelled");
        if (cuts.empty() && !err.empty()) return fail(err);
        double from = 0;
        for (const double c : cuts)
            if (c - from >= o.shortestShot && duration - c >= o.shortestShot) shots.push_back({from, c}), from = c;
        shots.push_back({from, duration});
    }
    std::vector<ShotDescription> out;
    for (size_t i = 0; i < shots.size(); ++i) {
        if (stopped(cancel)) return fail("Cancelled");
        const double mid = (shots[i].first + shots[i].second) / 2;
        Frame16Ptr f = dec.frameAt(std::max(0.0, std::min(mid, duration - 0.01)), kDescribeInput, kDescribeInput, true);
        if (!f) return fail("Cannot read the frame at " + std::to_string(mid) + " s of " + path);
        std::string err;
        std::string text = model->describe(toImage(*f), o.detail, &err, cancel);
        if (text.empty() && !err.empty()) return fail(err);  // (a picture put into no words is kept as a shot without them)
        out.push_back({shots[i].first, shots[i].second, std::move(text)});
        if (progress) progress(0.2 + 0.8 * double(i + 1) / double(shots.size()));
    }
    return out;
}

void setShotDescriptions(MediaItem& m, std::vector<ShotDescription> shots) {
    m.shots = std::move(shots);
    if (m.shots.empty()) return;
    std::string& description = m.metadata["description"];
    if (!QString::fromStdString(description).trimmed().isEmpty()) return;
    const auto longest = std::max_element(m.shots.begin(), m.shots.end(), [](const ShotDescription& a, const ShotDescription& b) {
        return (a.text.empty() ? -1 : a.end - a.start) < (b.text.empty() ? -1 : b.end - b.start);
    });
    description = longest->text;
}

// ---- Chapters ------------------------------------------------------------------------------------------------------

std::string numberedSentences(const std::vector<std::string>& sentences, int wordsPerSentence, int perLine) {
    std::string out;
    const size_t per = size_t(std::max(1, perLine));
    for (size_t i = 0, n = 1; i < sentences.size(); i += per, ++n) {
        out += "[" + std::to_string(n) + "]";
        for (size_t k = i; k < std::min(sentences.size(), i + per); ++k) {
            QStringList w = QString::fromStdString(sentences[k]).split(' ', Qt::SkipEmptyParts);
            if (wordsPerSentence > 0 && w.size() > wordsPerSentence) w = w.mid(0, wordsPerSentence);
            out += " " + w.join(' ').toStdString();
        }
        out += "\n";
    }
    return out;
}

std::string subjectsQuestion(const std::string& numbered) {
    return "Here is a transcript, numbered:\n\n" + numbered +
           "\nWhat broad subjects does the talk move between, in the order they come? Name each in one to three words, one a "
           "line, each subject once. Only a subject that many sentences in a row are about counts: details within a subject "
           "are not subjects of their own.";
}

std::string subjectsGrammar(int maxSubjects) {
    return "root ::= (\"- \" subject \"\\n\"){1," + std::to_string(std::clamp(maxSubjects, 1, 12)) +
           "}\n"
           "subject ::= [A-Z] [^\\n:|\"]{1,30}\n";
}

std::vector<std::string> readSubjects(const std::string& answer) {
    std::vector<std::string> out;
    for (QString line : QString::fromStdString(answer).split('\n')) {
        line = line.trimmed();
        if (line.startsWith('-')) line = line.mid(1).trimmed();
        while (!line.isEmpty() && QStringLiteral(".,;:-").contains(line.back())) line.chop(1);
        line = upperFirst(line.simplified());
        if (line.isEmpty() || line.contains('"') || line.contains('\\')) continue;
        if (std::none_of(out.begin(), out.end(), [&](const std::string& o) { return QString::fromStdString(o).compare(line, Qt::CaseInsensitive) == 0; }))
            out.push_back(line.toStdString());
    }
    return out;
}

std::string labelQuestion(const std::string& numbered, const std::vector<std::string>& subjects) {
    std::string list;
    for (size_t i = 0; i < subjects.size(); ++i) list += (i ? ", " : "") + subjects[i];
    return "Here is a transcript, numbered:\n\n" + numbered + "\nThe talk's subjects are: " + list +
           ". For each numbered line, write its number, then \" | \", then the subject the talk is on there, one of those.";
}

std::string labelGrammar(int lines, const std::vector<std::string>& subjects) {
    std::string g = "root ::=";
    for (int i = 1; i <= lines; ++i) g += " \"" + std::to_string(i) + " | \" subject \"\\n\"";
    g += "\nsubject ::=";
    for (size_t i = 0; i < subjects.size(); ++i) g += (i ? " | \"" : " \"") + subjects[i] + "\"";
    return g + "\n";
}

std::vector<int> readLabels(const std::string& answer, int lines, const std::vector<std::string>& subjects) {
    static const QRegularExpression line(QStringLiteral("^\\s*(\\d+)\\s*\\|\\s*(.*\\S)\\s*$"));
    std::vector<int> out(size_t(std::max(0, lines)), -1);
    for (const QString& l : QString::fromStdString(answer).split('\n')) {
        const QRegularExpressionMatch m = line.match(l);
        if (!m.hasMatch()) continue;
        const long long n = m.captured(1).toLongLong();
        if (n < 1 || n > lines) continue;
        for (size_t k = 0; k < subjects.size(); ++k)
            if (QString::fromStdString(subjects[k]).compare(m.captured(2), Qt::CaseInsensitive) == 0) out[size_t(n - 1)] = int(k);
    }
    return out;
}

std::vector<std::pair<size_t, int>> chaptersFromLabels(const std::vector<int>& labels, const std::vector<double>& starts, double total,
                                                       double minSeconds, int maxChapters) {
    // Runs of one subject (a sentence with none is on the subject before it).
    std::vector<std::pair<size_t, int>> runs;
    for (size_t i = 0; i < labels.size() && i < starts.size(); ++i) {
        const int l = labels[i] >= 0 ? labels[i] : (runs.empty() ? -1 : runs.back().second);
        if (runs.empty() || runs.back().second != l) runs.push_back({i, l});
    }
    auto length = [&](size_t r) { return (r + 1 < runs.size() ? starts[runs[r + 1].first] : total) - starts[runs[r].first]; };
    auto join = [&](size_t r) {  // run r taken into a neighbour
        const bool intoNext = r + 1 < runs.size() && (r == 0 || runs[r + 1].second == runs[r].second);
        if (intoNext) runs[r + 1].first = runs[r].first;
        runs.erase(runs.begin() + long(r));
        for (size_t k = 1; k < runs.size();)  // neighbours now on one subject become one
            if (runs[k].second == runs[k - 1].second) runs.erase(runs.begin() + long(k));
            else ++k;
    };
    while (runs.size() > 1) {
        size_t shortest = 0;
        for (size_t r = 1; r < runs.size(); ++r)
            if (length(r) < length(shortest)) shortest = r;
        const bool tooMany = maxChapters > 0 && int(runs.size()) > maxChapters;
        if (length(shortest) >= minSeconds && !tooMany) break;
        join(shortest);
    }
    if (!runs.empty()) runs.front().first = 0;
    return runs;
}

std::vector<SuggestedChapter> writeChapters(const Project& p, const Sequence& s, const ChapterOptions& o, const LanguageModel& lm,
                                            std::string* error, const WordsProgress& progress, const std::atomic<bool>* cancel) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return std::vector<SuggestedChapter>{};
    };
    const double fps = s.fpsValue(), total = double(s.duration()) / fps;
    const Said said = whatIsSaid(p, s);
    if (said.words.empty()) return fail("Nothing in this sequence has been transcribed");
    if (total < 2 * o.minSeconds) return fail("The cut is too short for chapters of at least " + std::to_string(int(o.minSeconds)) + " seconds");
    if (said.sentences.size() < 4) return fail("Too little is said to find where the talk moves on");
    const int cap = o.maxChapters > 0 ? o.maxChapters : std::max(3, int(total / 90));
    // At most 150 numbered lines (a line answered for each): sentences share lines in a long talk.
    const int perLine = int((said.sentences.size() + 149) / 150);
    const int lines = int((said.sentences.size() + size_t(perLine) - 1) / size_t(perLine));
    std::string err;
    // The subjects.
    WriteRequest r;
    r.system = kEditorsAssistant;
    r.grammar = subjectsGrammar(std::min(cap, 8));
    r.maxTokens = 16 * std::min(cap, 8) + 8;
    r.prompt = fitQuestion(lm, r.maxTokens, [&](int words) { return subjectsQuestion(numberedSentences(said.sentences, words, perLine)); });
    if (r.prompt.empty()) return fail("The talk is too long for the language model: suggest chapters for part of it at a time");
    // (Only whole lines are read: a last line cut off at the length allowed is not a subject, or a line's subject.)
    auto wholeLines = [](std::string a) { return a.substr(0, a.rfind('\n') == std::string::npos ? 0 : a.rfind('\n') + 1); };
    const std::vector<std::string> subjects =
        readSubjects(wholeLines(lm.write(r, &err, [&](double f) { if (progress) progress(0.25 * f); }, cancel)));
    if (subjects.empty()) return fail(err.empty() ? "The language model named no subjects" : err);
    if (subjects.size() < 2) return fail("The talk stays on one subject: no place to start a new chapter");
    // Each line's subject.
    r.grammar = labelGrammar(lines, subjects);
    r.maxTokens = 12 * lines + 16;
    r.prompt = fitQuestion(lm, r.maxTokens, [&](int words) { return labelQuestion(numberedSentences(said.sentences, words, perLine), subjects); });
    if (r.prompt.empty()) return fail("The talk is too long for the language model: suggest chapters for part of it at a time");
    const std::string answer = lm.write(r, &err, [&](double f) { if (progress) progress(0.25 + 0.75 * f); }, cancel);
    if (answer.empty()) return fail(err.empty() ? "The language model gave no answer" : err);
    const std::vector<int> lineLabels = readLabels(wholeLines(answer), lines, subjects);
    // Each sentence takes its line's subject; each would start its chapter just before its first word.
    std::vector<int> labels(said.sentences.size());
    std::vector<double> starts(said.sentences.size());
    for (size_t i = 0; i < said.sentences.size(); ++i) {
        labels[i] = lineLabels[i / size_t(perLine)];
        starts[i] = i == 0 ? 0.0
                           : std::max(said.words[said.spans[i - 1].second].end, said.words[said.spans[i].first].start - 0.4);
    }
    std::vector<SuggestedChapter> out;
    for (const auto& [sentence, subject] : chaptersFromLabels(labels, starts, total, o.minSeconds, cap))
        out.push_back({sentence == 0 ? 0 : FrameTime(std::llround(starts[sentence] * fps)),
                       subject >= 0 ? subjects[size_t(subject)] : "Chapter " + std::to_string(out.size() + 1), 0});
    if (out.size() < 2) return fail("The talk stays on one subject: no place to start a new chapter");
    return out;
}

// ---- Summaries and notes -------------------------------------------------------------------------------------------

const char* footageWritingName(FootageWriting w) {
    switch (w) {
    case FootageWriting::Summary: return "Summary";
    case FootageWriting::ShowNotes: return "Show Notes";
    case FootageWriting::StoryNotes: return "Story Notes";
    }
    return "";
}

std::string footageWritingGrammar(FootageWriting w) {
    switch (w) {
    // Sentences counted, so a paragraph ends where it was asked to rather than going round again.
    case FootageWriting::Summary:
        return "root ::= sentence (\" \" sentence){0,4} \"\\n\"\n"
               "sentence ::= [A-Z] [^\\n#*.!?]+ [.!?]\n";
    case FootageWriting::ShowNotes:
        return "root ::= sentence (\" \" sentence){0,2} \"\\n\\nTopics:\\n\" item{3,8}\n"
               "sentence ::= [A-Z] [^\\n#*.!?]+ [.!?]\n"
               "item ::= \"- \" [A-Z] [^\\n#*]+ \"\\n\"\n";
    case FootageWriting::StoryNotes:
        return "root ::= \"Logline: \" [A-Z] [^\\n#*]+ \"\\n\\nBeats:\\n\" item{3,12} \"\\nOpen questions:\\n\" item{1,5}\n"
               "item ::= \"- \" [A-Z] [^\\n#*]+ \"\\n\"\n";
    }
    return {};
}

std::string writeAboutSequence(const Project& p, const Sequence& s, FootageWriting what, const LanguageModel& lm, std::string* error,
                               const WordsProgress& progress, const std::atomic<bool>* cancel) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return std::string();
    };
    const Said said = whatIsSaid(p, s);
    if (said.words.empty()) return fail("Nothing in this sequence has been transcribed");
    if (said.words.size() < 20) return fail("Too little is said to write about");
    std::string ask;
    WriteRequest r;
    r.system = kEditorsAssistant;
    r.grammar = footageWritingGrammar(what);
    switch (what) {
    case FootageWriting::Summary:
        ask = "Summarise what is said in one paragraph of three to five sentences.";
        r.maxTokens = 320;
        break;
    case FootageWriting::ShowNotes:
        ask = "Write show notes for it: a paragraph of two or three sentences saying what it is about, then a blank line, "
              "then \"Topics:\" and three to eight topics it covers, one a line, each starting with \"- \".";
        r.maxTokens = 520;
        break;
    case FootageWriting::StoryNotes:
        ask = "Write story notes for the editor: \"Logline:\" and one sentence on the story it tells; a blank line; "
              "\"Beats:\" and the story's beats in order, one a line, each starting with \"- \"; a blank line; "
              "\"Open questions:\" and what it leaves unanswered or would need to make the story clear, one a line, each "
              "starting with \"- \".";
        r.maxTokens = 720;
        break;
    }
    r.prompt = fitQuestion(lm, r.maxTokens, [&](int words) {
        return "Here is a transcript of what is said in a cut, one sentence a line:\n\n" + sentenceLines(said.sentences, words, false) +
               "\n" + ask;
    });
    if (r.prompt.empty()) return fail("The talk is too long for the language model");
    std::string err;
    QString text = QString::fromStdString(lm.write(r, &err, progress, cancel)).trimmed();
    if (text.isEmpty()) return fail(err.empty() ? "The language model gave no answer" : err);
    if (what == FootageWriting::Summary && !QStringLiteral(".!?").contains(text.back()) && text.contains(QStringLiteral(". ")))
        text.truncate(int(text.lastIndexOf(QStringLiteral(". "))) + 1);  // (a last sentence cut off at the length allowed)
    std::string out = text.toStdString();
    if (what == FootageWriting::ShowNotes && std::any_of(s.markers.begin(), s.markers.end(), [](const Marker& m) { return m.chapter; }))
        out += "\n\nChapters:\n" + youtubeChapters(s);
    return out;
}

// ---- Audio description ---------------------------------------------------------------------------------------------

int descriptionWordBudget(double seconds, double wordsPerMinute) {
    return std::max(2, int(std::floor(seconds * wordsPerMinute / 60.0 + 1e-9)));
}

std::string descriptionGrammar(int words) {
    // Only the first word capitalised: the line says who is there by what they look like, not by a name the picture's
    // description may have got wrong.
    return "root ::= first (\",\"? \" \" word){0," + std::to_string(std::max(1, words) - 1) +
           "} \".\"\n"
           "first ::= [A-Z] [a-z0-9']* (\"-\" [a-z0-9']+)?\n"
           "word ::= [a-z0-9']+ (\"-\" [a-z0-9']+)?\n";
}

std::string fitDescription(const std::string& text, int words) {
    QStringList w = QString::fromStdString(text).split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    if (w.size() > words) w = w.mid(0, std::max(1, words));
    // Not left hanging on a word that leads into more ("A man in." is "A man.").
    static const QRegularExpression dangling(QStringLiteral("^(a|an|the|of|in|on|at|to|by|for|from|with|and|or|but|his|her|their|its|is|are)[,;:.]?$"),
                                             QRegularExpression::CaseInsensitiveOption);
    while (w.size() > 1 && dangling.match(w.back()).hasMatch()) w.removeLast();
    QString out = w.join(' ');
    while (!out.isEmpty() && QStringLiteral(",;:-").contains(out.back())) out.chop(1);
    if (!out.isEmpty() && !QStringLiteral(".!?").contains(out.back())) out += '.';
    return upperFirst(out).toStdString();
}

std::vector<DescriptionDraft> draftDescriptions(const Project& p, const Sequence& s, const std::vector<DescriptionGap>& gaps,
                                                const PictureDescriber& describer, const LanguageModel* lm, const DraftOptions& o,
                                                const WordsProgress& progress, const std::atomic<bool>* cancel, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return std::vector<DescriptionDraft>{};
    };
    const double fps = s.fpsValue();
    RenderOptions ro;
    ro.scale = std::min(1.0, double(kDescribeInput) / std::max(1, std::max(s.width, s.height)));
    ro.cancel = cancel;
    std::vector<DescriptionDraft> out;
    std::string before;  // the line before, not to be said again
    for (size_t i = 0; i < gaps.size(); ++i) {
        if (stopped(cancel)) return fail("Cancelled");
        const DescriptionGap& g = gaps[i];
        const Image picture = renderProgramFrame(p, s, (g.start + g.end) / 2, ro);
        std::string err;
        DescriptionDraft d{g.start, g.end, {}, describer.describe(picture, PictureDetail::Brief, &err, cancel)};
        if (d.seen.empty()) return fail(err.empty() ? "The picture could not be described" : err);
        // Progress within the gap too, so a cancel is seen between the models' runs.
        auto partly = [&](double f) {
            if (progress) progress((double(i) + f) / double(gaps.size()));
        };
        partly(lm ? 0.2 : 0.9);
        const int budget = descriptionWordBudget(double(g.length()) / fps, o.wordsPerMinute);
        if (lm) {
            // The detailed description says more, but names people (often wrongly): the brief one says who is there.
            const std::string detail = describer.describe(picture, PictureDetail::Detailed, &err, cancel);
            if (detail.empty()) return fail(err.empty() ? "The picture could not be described" : err);
            partly(0.5);
            WriteRequest r;
            r.system = "You write audio description for blind and partially sighted viewers of films and television.";
            r.prompt = "In brief, the shot is " + d.seen + ".\nIn detail: " + detail + "\n\n" +
                       (before.empty() ? "" : "The line before this one was: \"" + before + "\"\n\n") +
                       "Write one short sentence of audio description for the shot, " + std::to_string(budget) +
                       " words or fewer, with a verb: present tense, only what can be seen, the most important thing first, "
                       "no \"we see\". Say who is there by what they look like, never by name (\"A man in a dark suit "
                       "smiles.\"). Do not repeat the line before.";
            r.grammar = descriptionGrammar(budget);
            r.maxTokens = 4 * budget + 8;
            d.text = lm->write(r, &err, [&](double f) { partly(0.5 + 0.5 * f); }, cancel);
            if (d.text.empty()) return fail(err.empty() ? "The language model gave no answer" : err);
            // A line opening with a name from the detailed description is not used: the brief description is.
            static const QRegularExpression name(QStringLiteral("(?<=[a-z,;] )[A-Z][a-z]+"));  // capitalised mid-sentence
            const QString first = QString::fromStdString(d.text).section(QRegularExpression(QStringLiteral("[^A-Za-z']")), 0, 0);
            for (auto it = name.globalMatch(QString::fromStdString(detail)); it.hasNext();)
                if (it.next().captured() == first) d.text.clear();
        }
        if (d.text.empty()) {
            // The brief description as a line ("a man in a suit and tie" -> "A man in a suit and tie.").
            static const QRegularExpression lead(QStringLiteral("^(the|this) (image|picture|photo) (shows|is of|depicts)\\s+"),
                                                 QRegularExpression::CaseInsensitiveOption);
            d.text = QString::fromStdString(d.seen).remove(lead).toStdString();
        }
        d.text = fitDescription(d.text, budget);
        before = d.text;
        out.push_back(std::move(d));
        if (progress) progress(double(i + 1) / double(gaps.size()));
    }
    return out;
}

}  // namespace montage
