// Montage — linear timecode (LTC, SMPTE ST 12-1; EBU's is the same at 25 fps): each video frame's timecode sent as an
// 80-bit word in biphase mark code on a sound channel, as lockit boxes, field recorders and cameras record it. Reading
// it at any sample rate (its speed found from the signal and followed as it drifts, either polarity) and writing it.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "core/Model.h"

namespace montage {

// One LTC word.
struct LtcFrame {
    int hours = 0, minutes = 0, seconds = 0, frames = 0;
    bool dropFrame = false;
    bool colourFrame = false;
    uint32_t userBits = 0;  // the eight user-bit groups, group 1 in the lowest four bits
    // Read words: where its first bit starts (the start of the video frame it labels) and where the next word would,
    // in samples since the decoder began, to a fraction of a sample.
    double start = 0, end = 0;
};

// The 80 bits of the word, bit 0 first. `base` (24, 25 or 30) places the bit that keeps each word starting the same
// way up (bit 59 at 25, bit 27 otherwise), set so the word has an even number of zeros.
std::array<uint8_t, 80> ltcBits(const LtcFrame& f, int base);
// A word from its bits (those after the sync word left out); false when a digit is out of range.
bool ltcFromBits(const uint8_t* bits, LtcFrame& out);

// Frames since 00:00:00:00 the label stands for, at `base` frames a second (drop frame skipping labels 0 and 1 at
// each minute but every tenth, at 30), and the label of a frame number (taken round the clock).
int64_t ltcFrameNumber(const LtcFrame& f, int base);
LtcFrame ltcFrameAt(int64_t number, int base, bool dropFrame);
// Frames in a day of labels.
int64_t ltcFramesPerDay(int base, bool dropFrame);
// "HH:MM:SS:FF" (";" before the frames in drop frame).
std::string ltcLabel(const LtcFrame& f);

// `count` words from frame number `start` at `fps` (24000/1001, 24, 25, 30000/1001 or 30; drop frame only at
// 30000/1001), as `sampleRate` samples at `level` peak: sample 0 is where the first word starts, and edges take about
// 25 µs as ST 12-1 asks.
std::vector<float> encodeLtc(int64_t start, int64_t count, Rational fps, bool dropFrame, int sampleRate, float level = 0.5f,
                             uint32_t userBits = 0);

// Reads LTC from a stream of samples (one channel). Bit periods from 0.33 to 0.66 ms are taken (23.976 to 30 fps and
// some way either side); the bit length is followed as the speed drifts.
class LtcDecoder {
public:
    explicit LtcDecoder(int sampleRate);
    void write(const float* samples, size_t n);
    // The words read since the last call.
    std::vector<LtcFrame> take();

private:
    void edge(double t);
    void bit(int value, double from, double to);

    int rate_;
    double minBit_, maxBit_;
    int64_t index_ = 0;          // samples written so far
    float prev_ = 0;             // the last sample
    double envelope_ = 0;        // the signal's recent peak
    double dc_ = 0;              // its slow average, taken away
    int state_ = 0;              // +1 above the threshold, -1 below, 0 not known yet
    double crossing_ = -1;       // the last time the signal passed zero
    double lastEdge_ = -1;       // the last transition
    double period_ = 0;          // the bit length (samples), 0 until two edges agree
    double halfStart_ = -1;      // a short interval waiting for its pair: where its bit began
    std::array<uint8_t, 80> bits_{};
    std::array<double, 81> times_{};  // where each held bit starts, and the last one's end
    int held_ = 0;
    std::vector<LtcFrame> out_;
};

}  // namespace montage
