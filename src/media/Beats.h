// Montage — the beat of a piece of music, and re-editing music to a length
// (like Final Cut's beat detection, Premiere's Remix and Resolve's music
// editor). Plain signal processing, no model:
// - beats: an onset envelope (spectral flux of a log-mel spectrogram), the
//   tempo from its autocorrelation (weighted towards 120 BPM), and Ellis'
//   dynamic-programming beat tracker; bars start where bass onsets and
//   chord changes line up (4 beats to the bar);
// - fitting: beat-synchronous chroma and timbre, and jumps between bars that
//   sound alike, so the music is shortened or lengthened by whole bars while
//   its start and ending stay as they are.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace montage {

struct BeatGrid {
    double tempo = 0;                // beats per minute
    std::vector<double> beats;       // seconds
    std::vector<double> downbeats;   // the first beat of each bar (a subset of beats)
    int beatsPerBar = 4;
    double duration = 0;             // of the analysed audio, seconds
    bool empty() const { return beats.empty(); }
};

// Mono audio at `rate` (any; analysed at 22.05 kHz).
BeatGrid detectBeats(const std::vector<float>& mono, int rate, const std::atomic<bool>* cancel = nullptr);
// Decodes the file's audio first.
BeatGrid detectBeatsInFile(const std::string& path, const std::atomic<bool>* cancel = nullptr, std::string* error = nullptr);

// A stretch of the source, in seconds; a fitted piece plays them in order.
struct MusicSegment {
    double in = 0, out = 0;
    bool operator==(const MusicSegment&) const = default;
};
struct MusicFit {
    std::vector<MusicSegment> segments;
    double duration = 0;    // sum of the segments
    double similarity = 0;  // of the worst join, 0..1 (1: indistinguishable)
    bool ok() const { return !segments.empty(); }
};

struct FitOptions {
    double keepStart = 8;  // seconds at the start (at least) never cut
    double keepEnd = 8;    // and at the end
    int maxJumps = 4;
};

// Re-edits the music (mono at `rate`, with its beat grid) to last about
// `target` seconds by skipping or repeating whole bars where the music
// matches itself best. Returns the source stretches in order; the result is
// within about half a bar of the target. Fails (no segments) when the music
// is too short to cut or has no beat.
MusicFit fitMusic(const std::vector<float>& mono, int rate, const BeatGrid& grid, double target, const FitOptions& o = {},
                  const std::atomic<bool>* cancel = nullptr);

// Decodes mono audio at `rate` for the functions above.
bool decodeMono(const std::string& path, int rate, std::vector<float>& out, const std::atomic<bool>* cancel = nullptr,
                std::string* error = nullptr);

}  // namespace montage
