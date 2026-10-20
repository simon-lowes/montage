// Montage — words from the footage, on this computer: each shot described for logging and search, chapters,
// summaries, show notes and story notes from what is said, and audio description drafted to fit the gaps between the
// dialogue. Pictures are put into words by Florence-2 (media/DescribePicture.h), and the writing is done by a small
// language model (media/LanguageModel.h) held to a grammar, so chapter starts and lists always parse; nothing leaves
// the computer. The prompts, grammars and the reading of answers are here, apart from the models, so they can be
// checked without them.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "core/AudioDescription.h"
#include "core/ChapterSuggest.h"
#include "core/Model.h"
#include "media/DescribePicture.h"

namespace montage {

class LanguageModel;
class PictureDescriber;
using WordsProgress = std::function<void(double fraction)>;

// ---- Shots ---------------------------------------------------------------------------------------------------------

struct DescribeShotsOptions {
    double sensitivity = 0.5;   // scene-cut sensitivity (media/Analysis.h)
    double shortestShot = 1.0;  // seconds; cuts closer than this are taken as one shot
    PictureDetail detail = PictureDetail::Detailed;
};

// The shots of a video (from its scene cuts; a still is one shot), each described from its middle frame.
std::vector<ShotDescription> describeShots(const MediaItem& m, const DescribeShotsOptions& o = {}, const WordsProgress& progress = {},
                                           const std::atomic<bool>* cancel = nullptr, std::string* error = nullptr);

// Keeps the shots with the media item, and fills its Description (core/MediaLog.h) with the longest shot's when it
// has none.
void setShotDescriptions(MediaItem& m, std::vector<ShotDescription> shots);

// ---- Chapters ------------------------------------------------------------------------------------------------------
// A small model asked in one go where chapters start answers unevenly (a chapter a sentence, or details as chapters),
// so it is asked two easier things: the broad subjects the talk moves between, then which of them each sentence is on
// (each line's subject held by the grammar to one of those). Runs of sentences on one subject are the chapters.

// The sentences one a line, numbered ("[3] ..."), each cut to `wordsPerSentence` words (0 = whole); `perLine`
// sentences share a line (and a number) when a long talk would otherwise give too many lines.
std::string numberedSentences(const std::vector<std::string>& sentences, int wordsPerSentence = 0, int perLine = 1);
// The question for the subjects, and its grammar: a list ("- Cooking"), at most `maxSubjects` lines.
std::string subjectsQuestion(const std::string& numbered);
std::string subjectsGrammar(int maxSubjects);
// The subjects an answer names, in order, each once (compared without case), tidied.
std::vector<std::string> readSubjects(const std::string& answer);
// The question for each line's subject, and its grammar: "<n> | <subject>" for every line 1 to `lines`.
std::string labelQuestion(const std::string& numbered, const std::vector<std::string>& subjects);
std::string labelGrammar(int lines, const std::vector<std::string>& subjects);
// Each line's subject (an index into `subjects`; -1 where the answer gives none) from an answer.
std::vector<int> readLabels(const std::string& answer, int lines, const std::vector<std::string>& subjects);

// Chapters from each sentence's subject and when each sentence's chapter would start (seconds; the first 0) in a talk
// `total` seconds long: runs of one subject, a run shorter than `minSeconds` taken into its neighbour (the one on the
// same subject, else the one before), at most `maxChapters` (the shortest taken in until so; 0 = any number). Returns
// (first sentence, subject) for each.
std::vector<std::pair<size_t, int>> chaptersFromLabels(const std::vector<int>& labels, const std::vector<double>& starts,
                                                       double total, double minSeconds, int maxChapters = 0);

// Suggest Chapters with the language model: where the talk moves on, as the model reads the transcript, each chapter
// titled with its subject; chapters start just before a sentence and are at least o.minSeconds long.
std::vector<SuggestedChapter> writeChapters(const Project& p, const Sequence& s, const ChapterOptions& o, const LanguageModel& lm,
                                            std::string* error = nullptr, const WordsProgress& progress = {},
                                            const std::atomic<bool>* cancel = nullptr);

// ---- Summaries and notes -------------------------------------------------------------------------------------------

enum class FootageWriting {
    Summary,     // a paragraph on what is said
    ShowNotes,   // a paragraph, the topics as a list, and the chapter markers with their times (for a podcast or video page)
    StoryNotes,  // for the editor: a logline, the story's beats and the questions it leaves open
};
const char* footageWritingName(FootageWriting w);
std::string footageWritingGrammar(FootageWriting w);

// Written from the sequence's transcript.
std::string writeAboutSequence(const Project& p, const Sequence& s, FootageWriting what, const LanguageModel& lm,
                               std::string* error = nullptr, const WordsProgress& progress = {}, const std::atomic<bool>* cancel = nullptr);

// ---- Audio description ---------------------------------------------------------------------------------------------

struct DescriptionDraft {
    FrameTime start = 0, end = 0;  // the gap (timeline frames)
    std::string text;              // the line, fitting the gap at the pace asked
    std::string seen;              // what the picture was described as, in brief
};

struct DraftOptions {
    double wordsPerMinute = 160;
};

// Words that fit `seconds` at `wordsPerMinute` (at least two).
int descriptionWordBudget(double seconds, double wordsPerMinute = 160);
// The grammar of a description line of at most `words` words: one sentence, ending in a full stop.
std::string descriptionGrammar(int words);
// `text` cut to at most `words` words, ending in a full stop.
std::string fitDescription(const std::string& text, int words);

// A line drafted for each gap from the picture in its middle: described, then written as a describer would (present
// tense, what is seen, no "we see", people by how they look, not by name) by the language model when one is given,
// else the picture's brief description (as also when the model's line opens with a name); every line fits its gap.
std::vector<DescriptionDraft> draftDescriptions(const Project& p, const Sequence& s, const std::vector<DescriptionGap>& gaps,
                                                const PictureDescriber& describer, const LanguageModel* lm, const DraftOptions& o = {},
                                                const WordsProgress& progress = {}, const std::atomic<bool>* cancel = nullptr,
                                                std::string* error = nullptr);

}  // namespace montage
