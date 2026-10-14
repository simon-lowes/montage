// Montage — timecode from a recording's sound: which channel of a file carries LTC (audio/Ltc.h), the timecode it
// gives the file's start, using that as the media's timecode for syncing (the file's own kept) with the LTC channel no
// longer played, and LTC written for a sequence as a sound file.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "audio/Ltc.h"
#include "core/Model.h"

namespace montage {

struct LtcReading {
    int channel = -1;            // the source channel carrying it (numbered as Clip::channels numbers them)
    Rational fps{0, 1};          // 24000/1001, 24, 25, 30000/1001 or 30
    bool dropFrame = false;
    double start = -1;           // the timecode at the start of the file: seconds since midnight (its frame number / fps)
    std::string label;           // the first word read ("HH:MM:SS:FF")
    double at = 0;               // where in the file that word starts, seconds
    int words = 0;               // words read that agree with each other
    double speed = 1;            // the timecode's rate against the sound's clock (1: they agree)
    uint32_t userBits = 0;       // the first word's
    std::string startLabel() const;  // `start` as a label at `fps`
};

// The timecode words read from one channel (LtcDecoder) make a reading: the longest run of words that follow on from
// each other. Its whole frames a second come from how long a word lasts; whether it is pulled down (23.976, 29.97)
// from the rate the words came at, over 1.5 s or more, else from `hint` (the picture's rate, say: 23.976 or 47.952
// for 23.976, 29.97 or 59.94 for 29.97) or drop frame. False with fewer than 8 in a run.
bool ltcFromWords(const std::vector<LtcFrame>& words, int sampleRate, LtcReading& out, Rational hint = {0, 1});
// The same from one channel's samples.
bool findLtc(const std::vector<float>& samples, int sampleRate, LtcReading& out, Rational hint = {0, 1});
// Reads the first `seconds` of every channel of the file; the one with the longest run of LTC. False with `error`
// when none carries it.
bool readLtc(const std::string& path, LtcReading& out, std::string* error = nullptr, double seconds = 30,
             const std::atomic<bool>* cancel = nullptr, Rational hint = {0, 1});

// Whether the media's timecode is the one its LTC gives.
bool usesLtcTimecode(const MediaItem& m);
// The media (and its subclips) take the reading's start as their timecode, the file's own kept in
// MediaItem::fileTimecode, and the reading's rate as their timecode rate. When the file has other channels, the LTC
// channel is set apart: clips of it on audio tracks of every sequence stop playing it (one playing only it is
// disabled), and new clips leave it out.
void useLtcTimecode(Project& p, Id media, const LtcReading& r);
// Back to the file's own timecode and its rate (the LTC channel stays left out). False when it does not use its LTC's.
bool useFileTimecode(Project& p, Id media);
// Forgets the media's LTC (another recording in its place): its own timecode's rate back, the LTC fields cleared.
void forgetLtc(MediaItem& m);

// LTC from frame number `startFrame` (at the rate's whole frames a second: 24, 25 or 30) for `frames` frames as a
// mono 16-bit WAV at `sampleRate`, at -12 dBFS; its first word starts at its first sample.
bool writeLtcWav(const std::string& path, int64_t startFrame, int64_t frames, Rational fps, bool dropFrame, int sampleRate,
                 std::string* error = nullptr);

// The LTC rate for a sequence's frame rate: its own (23.976 to 30), or half of it (47.952 to 60, a word every two
// frames). False for other rates.
bool ltcRateFor(Rational fps, Rational& ltc, int& framesPerWord);
// LTC for frames [from, to) of a sequence at `fps`, each word labelled with the sequence's timecode there (drop frame
// at 29.97 and 59.94, as the timecode display shows it), written as writeLtcWav writes. At doubled rates the file
// starts at `from` rounded down to an even frame.
bool writeSequenceLtc(const std::string& path, Rational fps, FrameTime from, FrameTime to, int sampleRate, std::string* error = nullptr);
// The media (a sequence's LTC) placed at `at` on a new audio track named LTC at the bottom, muted (unmute it to send
// the timecode out), its clip given the Timecode role. The clip's id, 0 if it could not be placed.
Id placeLtcTrack(Project& p, Sequence& s, Id media, FrameTime at);

}  // namespace montage
