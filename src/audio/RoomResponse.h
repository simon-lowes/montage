// Montage — rooms: how reverberant a recording is (its decay time in each octave and how much of it is the room), a
// room's response made to measure (or read from an impulse-response file), and convolution with it, both offline
// and in real time (the Convolution Reverb effect). Dialogue Match (render/DialogueMatch.h) puts them together to make
// ADR sound as if it were recorded in the production take's room.
#pragma once

#include <array>
#include <complex>
#include <cstdint>
#include <string>
#include <vector>

#include "media/Decoder.h"

namespace montage {

// Octave bands, 125 Hz to 8 kHz.
constexpr int kRoomBands = 7;
const std::array<double, kRoomBands>& roomBandCentres();

// In place, any power of two; the inverse is unscaled.
void fftInPlace(std::vector<std::complex<float>>& a, bool inverse);

// A room's shape: its decay time (RT60, seconds) in each octave and when the reverberation starts.
struct RoomShape {
    std::array<double, kRoomBands> rt60{{0.6, 0.6, 0.6, 0.6, 0.6, 0.5, 0.4}};
    double predelayMs = 12;
    bool operator==(const RoomShape&) const = default;
};
// The reverberant part of the room's response (no direct sound): noise decaying at each octave's own rate, starting
// after the pre-delay, with unit energy. Each channel (seed) is its own noise, so stereo is wide.
std::vector<float> roomImpulse(int sampleRate, const RoomShape& room, uint32_t seed = 1);
// An impulse response from a sound file, each channel at `sampleRate` (mono files give the same to both), scaled to
// unit energy over both channels. False with `error` if it cannot be read or is silent.
bool loadImpulseResponse(const std::string& path, int sampleRate, std::vector<float>& left, std::vector<float>& right,
                         std::string* error = nullptr);

// Convolution with an impulse response in real time and without latency: its first block of taps directly, the rest
// in uniformly partitioned FFT blocks (overlap-save) that come due one block after their input is complete.
class Convolver {
public:
    void setImpulse(const std::vector<float>& ir, int block = 512);
    bool empty() const { return head_.empty(); }
    void reset();
    // `n` samples of output (the response's alone, no dry sound) for `n` of input; `stride` steps through
    // interleaved buffers. `in` and `out` may be the same.
    void process(const float* in, float* out, int n, int stride = 1);

private:
    int block_ = 0;
    std::vector<float> head_;                                // taps 0 .. block-1
    std::vector<std::vector<std::complex<float>>> parts_;   // the spectra of the later blocks of taps
    std::vector<std::vector<std::complex<float>>> inputs_;  // the spectra of the latest input frames (a ring)
    size_t newest_ = 0;
    std::vector<float> history_;  // the last `block` inputs (a ring), for the direct taps
    size_t at_ = 0;
    std::vector<float> previous_, current_, tail_;  // the input frame before, this one, and the later taps' output now
    int pos_ = 0;
    std::vector<std::complex<float>> work_;
};

// Offline: `signal` (mono) convolved with `ir`, as long as the signal.
std::vector<float> convolve(const std::vector<float>& signal, const std::vector<float>& ir);

// How a recording's room sounds, measured blind from the speech in it.
struct RoomEstimate {
    std::array<double, kRoomBands> rt60{};  // per octave (0 where no decay could be followed)
    double broadband = 0;                   // 500 Hz to 4 kHz, as De-Reverb measures it
    int bandsFound = 0;
    // The bands where none was found, filled from the nearest found (or the broadband time).
    RoomShape shape() const;
};
// The decay time in the octave around `centreHz` (0 if fewer than three falls could be followed).
double bandReverbTime(const std::vector<float>& mono, int sampleRate, double centreHz);
RoomEstimate estimateRoom(const std::vector<float>& mono, int sampleRate);
// How much late reverberation there is (dB): in the quieter moments, the sound against what a room of this decay time
// would hold there with as much reverberant energy as direct (300 Hz to 4 kHz, the noise floor taken out). Adding
// more of a room raises it; dry speech is far below 0 dB. -60 if it cannot be told.
double lateReverbDb(const std::vector<float>& mono, int sampleRate, double rt60);
}  // namespace montage
