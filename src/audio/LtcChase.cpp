#include "LtcChase.h"

#include <cmath>

namespace montage {

LtcChase::LtcChase(int sampleRate, int base, bool dropFrame)
    : rate_(sampleRate), base_(base), drop_(dropFrame && base == 30), decoder_(sampleRate) {
    const double fps = base_ == 30 && drop_ ? 30000.0 / 1001 : double(base_);
    nominal_ = double(rate_) / fps;
}

void LtcChase::write(const float* samples, size_t n) {
    decoder_.write(samples, n);
    written_ += int64_t(n);
    for (const LtcFrame& f : decoder_.take()) take(f);
}

void LtcChase::take(const LtcFrame& f) {
    const int64_t day = ltcFramesPerDay(base_, drop_);
    int64_t frame = ltcFrameNumber(f, base_);
    if (!words_.empty()) {
        const Word& last = words_.back();
        int64_t k = frame - (last.frame % day + day) % day;
        if (k < -day / 2) k += day;
        if (k > day / 2) k -= day;
        frame = last.frame + k;
        // In sequence: one word on, or several when some were lost, each about a word apart (the speed so far).
        const double spacing = words_.size() >= 2 ? (last.start - words_.front().start) / double(last.frame - words_.front().frame) : nominal_;
        const double gap = f.start - last.start;
        const bool inTime = k >= 1 && std::fabs(gap - double(k) * spacing) < 0.5 * spacing &&
                            (k == 1 || gap <= freewheel_ * rate_ + spacing);
        if (!inTime) {
            words_.clear();
            locked_ = false;
        }
    }
    words_.push_back({frame, f.start});
    while (words_.size() > 16) words_.pop_front();
    if (int(words_.size()) >= lockWords_) locked_ = true;
}

LtcChase::Position LtcChase::at(int64_t sample) const {
    Position p;
    if (words_.empty()) return p;
    // The words' line (frame against start), fitted so a word's jitter moves little.
    const Word& last = words_.back();
    double spacing = nominal_, intercept = last.start;
    if (words_.size() >= 2) {
        const double n = double(words_.size());
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (const Word& w : words_) {
            const double x = double(w.frame - last.frame), y = w.start - last.start;
            sx += x, sy += y, sxx += x * x, sxy += x * y;
        }
        const double d = n * sxx - sx * sx;
        if (d > 0) {
            spacing = (n * sxy - sx * sy) / d;
            intercept = last.start + (sy - spacing * sx) / n;
        }
        if (!(spacing > 0.2 * nominal_ && spacing < 5 * nominal_)) spacing = nominal_, intercept = last.start;
    }
    // Words arrive one after another: half a word past the next one's end with none (the input's pieces deliver them a
    // little late), the signal has dropped out.
    const double since = double(sample) - (last.start + 2.5 * spacing);
    if (since > freewheel_ * rate_) return p;  // (stopped)
    p.state = !locked_ ? (since > 0 ? State::Stopped : State::Locking) : since > 0 ? State::Freewheel : State::Locked;
    if (p.state == State::Stopped) return p;
    p.frame = double(last.frame) + (double(sample) - intercept) / spacing + double(offset_);
    p.speed = nominal_ / spacing;
    return p;
}

}  // namespace montage
