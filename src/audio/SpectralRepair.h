// Montage — spectral repair (Adobe Audition's Spectral Frequency Display and healing, iZotope RX's Spectral Repair):
// a sound that shares its moment with the dialogue, but not its frequencies (a phone ringing, a squeak, a whistle, a
// cough's hiss, a siren), is selected on the spectrogram as a box of time and frequency and taken out, leaving
// everything outside the box as it was.
//
// Worked on a clip's whole source audio, like Noise Reduction (SpeechCleanup.h), so it is cached and costs nothing
// while scrubbing. Each region is processed in a short-time Fourier transform (Hann windows of about 43 ms, 75 %
// overlap) over a stretch around it only; the rest of the file is copied untouched. Inside the box:
//  - Heal: each frequency is brought down to the level heard just before and just after the region (interpolated
//    across it on a log scale), never raised, so the background carries on through the gap and the unwanted sound
//    goes;
//  - Attenuate: turned down by a fixed number of decibels.
// The original phase is kept, and the box's edges are feathered in time and frequency so nothing clicks.
#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "media/Decoder.h"

namespace montage {

struct SpectralRegion {
    double start = 0, end = 0;  // seconds of the source audio
    double low = 0, high = 0;   // Hz (high 0: up to the top)
    std::string mode = "heal";  // "heal" or "attenuate"
    double gainDb = -20;        // attenuate: how far down (-96 or less: silenced)
    int channel = -1;           // -1 both, 0 left, 1 right
    bool operator==(const SpectralRegion&) const = default;
};

// The effect's "regions" string: start,end,low,high,mode,gain,channel per region, separated by ';'.
std::string spectralRegionsToString(const std::vector<SpectralRegion>& regions);
std::vector<SpectralRegion> spectralRegionsFromString(const std::string& text);
bool validSpectralMode(const std::string& mode);

// `out` is `in` with the regions repaired, in order.
void spectralRepair(const AudioBuffer& in, AudioBuffer& out, const std::vector<SpectralRegion>& regions,
                    const std::atomic<bool>* cancel = nullptr);

// Level in decibels (0 dB: a full-scale sine) of each frequency bin over time, for drawing: `columns` evenly spaced
// times from `start` to `end` seconds, `bins` linear frequency bins from 0 to half the sample rate. `channel` -1
// mixes both.
struct Spectrogram {
    int columns = 0, bins = 0;
    double start = 0, end = 0, nyquist = 0;
    std::vector<float> db;  // columns * bins, column by column
    float at(int column, int bin) const { return db[size_t(column) * size_t(bins) + size_t(bin)]; }
    double hzOf(int bin) const { return bins > 1 ? nyquist * bin / (bins - 1) : 0; }
};
Spectrogram computeSpectrogram(const AudioBuffer& in, double start, double end, int columns, int channel = -1);

// Frequency bands that stand out between `start` and `end` seconds against the second either side (or, at the edges
// of the file, against the stretch's own median): what a whistle, ring or squeak occupies. Loudest excess first.
struct SpectralBand {
    double low = 0, high = 0;  // Hz
    double excessDb = 0;       // above its surroundings, at its peak
};
std::vector<SpectralBand> prominentBands(const AudioBuffer& in, double start, double end, int maxBands = 3, int channel = -1);

// The clip's Spectral Repair effect, made first among its effects when `create` (so later restoration works on the
// repaired sound); null if it has none.
Effect* spectralRepairEffect(Project& p, Clip& c, bool create);
std::vector<SpectralRegion> spectralRegionsOf(const Clip& c);
// Seconds of the clip's source at timeline second `t` (regions are kept in source time, so trims keep them on the sound).
double clipSourceSeconds(const Sequence& s, const Clip& c, double t);

}  // namespace montage
