// Montage — chasing linear timecode: a sound input carrying LTC from a DAW, a sync box or a tape machine (read by
// audio/Ltc) followed as a position on the Program, so the picture follows another system's transport. It locks
// after a few words in sequence, follows the master's speed, freewheels through drop-outs (for a set time, at the
// last speed), and adds an offset.
#pragma once

#include <cstdint>
#include <deque>
#include <memory>

#include "audio/Ltc.h"

namespace montage {

class LtcChase {
public:
    // `base` 24, 25 or 30 (30 with `dropFrame` for 29.97 drop frame).
    LtcChase(int sampleRate, int base, bool dropFrame);

    void setOffset(int64_t frames) { offset_ = frames; }    // added to the master's frame
    void setFreewheel(double seconds) { freewheel_ = seconds; }  // how long a drop-out is ridden through
    void setLockWords(int n) { lockWords_ = n; }             // words in sequence before it locks (4)

    // The input's samples, in order, from sample 0.
    void write(const float* samples, size_t n);

    enum class State { Stopped, Locking, Locked, Freewheel };
    struct Position {
        State state = State::Stopped;
        double frame = 0;  // where the master is (plus the offset), in frames on its timecode; 0 when stopped
        double speed = 0;  // times play
    };
    // Where the master is at input sample `sample` (as written so far; later ones are extrapolated).
    Position at(int64_t sample) const;
    Position now() const { return at(written_); }
    int64_t written() const { return written_; }

private:
    struct Word {
        int64_t frame;  // its label's frame number (with the day wrapped out against the last)
        double start;   // where it began, in samples
    };
    void take(const LtcFrame& f);

    int rate_, base_;
    bool drop_;
    int64_t offset_ = 0;
    double freewheel_ = 2;
    int lockWords_ = 4;
    LtcDecoder decoder_;
    int64_t written_ = 0;
    std::deque<Word> words_;  // the run in sequence so far (the latest few)
    bool locked_ = false;
    double nominal_;          // samples a word at play speed
};

}  // namespace montage
