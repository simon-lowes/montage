// Montage — word-timed transcripts of media (speech to text), and the
// caption formats made from them.
#pragma once

#include <string>
#include <vector>

namespace montage {

struct TranscriptWord {
    double start = 0;  // seconds, media time
    double end = 0;
    std::string text;  // without surrounding spaces
    float probability = 1;
    bool operator==(const TranscriptWord&) const = default;
};

struct TranscriptSegment {
    double start = 0;
    double end = 0;
    std::string text;
    std::vector<TranscriptWord> words;
    int speaker = -1;  // -1 = unknown
    bool operator==(const TranscriptSegment&) const = default;
};

struct Transcript {
    std::string language;  // ISO 639-1, e.g. "en"
    std::string model;     // the model that produced it, e.g. "base.en"
    std::vector<TranscriptSegment> segments;

    bool empty() const { return segments.empty(); }
    std::string text() const;  // the whole transcript as plain text
    size_t wordCount() const;
    bool operator==(const Transcript&) const = default;
};

std::string transcriptToJson(const Transcript& t);
bool transcriptFromJson(const std::string& json, Transcript& out, std::string* error = nullptr);

// Caption cues: transcript text split into readable lines, at most
// `maxChars` characters and `maxSeconds` long, breaking at word boundaries.
struct Cue {
    double start = 0;
    double end = 0;
    std::string text;
};
std::vector<Cue> transcriptCues(const Transcript& t, int maxChars = 42, double maxSeconds = 6.0);

// SubRip and WebVTT, with times shifted by `offset` seconds.
std::string cuesToSrt(const std::vector<Cue>& cues, double offset = 0);
std::string cuesToVtt(const std::vector<Cue>& cues, double offset = 0);

// The transcript as a file of the given type: "srt", "vtt", "txt" or "json"
// (empty for anything else).
std::string transcriptAs(const Transcript& t, const std::string& format, double offset = 0);

// Media times where `phrase` is spoken (case- and punctuation-insensitive,
// whole words), as (start, end) pairs.
std::vector<std::pair<double, double>> findPhrase(const Transcript& t, const std::string& phrase);

}  // namespace montage
