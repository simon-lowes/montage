// Montage — AV1 film grain: a picture's grain carried as parameters. An AV1 stream can say how its pictures' grain
// looked (how strong it is at each level of the picture, per plane; how neighbouring samples go together, as
// autoregression coefficients; how the chroma's follows the luma's; a seed for each frame) and the decoder makes grain
// to that measure on each picture it shows, so the encoder codes the picture without its grain. Here: the parameters
// as a frame header carries them, made from a measured grain (render/GrainMatch.h's profile); the synthesis itself, as
// the AV1 specification's film grain synthesis process describes it (section 7.18.3), for the Program monitor's
// preview and for checking a decoder's; and the parameters as a film grain table (the text file aomenc's
// --film-grain-table and SvtAv1EncApp's --fgs-table read).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace montage {

struct GrainProfile;
struct ColorSpace;

// film_grain_params() of a frame header (AV1 specification, 5.9.30 and 6.8.20), by the names used there.
struct Av1FilmGrain {
    bool apply = false;           // apply_grain
    uint16_t seed = 0;            // grain_seed
    int numY = 0;                 // num_y_points (to 14): the luma's scaling function, a point a pair
    uint8_t yValue[14] = {}, yScaling[14] = {};
    bool chromaFromLuma = false;  // chroma_scaling_from_luma: the chroma scaled by the luma's function
    int numCb = 0, numCr = 0;     // num_cb_points, num_cr_points (to 10)
    uint8_t cbValue[10] = {}, cbScaling[10] = {}, crValue[10] = {}, crScaling[10] = {};
    int scalingShift = 8;         // grain_scaling_minus_8 + 8 (8 to 11)
    int arLag = 0;                // ar_coeff_lag (0 to 3)
    int8_t arY[24] = {}, arCb[25] = {}, arCr[25] = {};  // the coefficients themselves (ar_coeffs_*_plus_128 less 128)
    int arShift = 6;              // ar_coeff_shift_minus_6 + 6 (6 to 9)
    int grainScaleShift = 0;      // grain_scale_shift (0 to 3)
    int cbMult = 128, cbLumaMult = 192, cbOffset = 256;  // as coded: what the chroma's scaling function is looked up by
    int crMult = 128, crLumaMult = 192, crOffset = 256;
    bool overlap = false;         // overlap_flag
    bool clipRestricted = false;  // clip_to_restricted_range

    int lumaCoefficients() const { return 2 * arLag * (arLag + 1); }
    // Every parameter but the seed is the same.
    bool sameParameters(const Av1FilmGrain& o) const;
    // Within what a frame header may carry (counts, shifts, each plane's points rising; in 4:2:0, `subsampled`,
    // points for both Cb and Cr or for neither, as decoders require).
    bool valid(std::string* why = nullptr, bool subsampled = false) const;
};

// A picture's planes for the synthesis: samples of `bitDepth` bits in 16-bit words, `stride` in samples.
struct Av1GrainPicture {
    uint16_t* plane[3] = {};
    ptrdiff_t stride[3] = {};
    int width = 0, height = 0;    // the luma's
    int bitDepth = 8;
    bool subX = true, subY = true;  // the chroma's subsampling
    bool mono = false;
    bool identityMatrix = false;  // matrix_coefficients is MC_IDENTITY (the chroma then clips as the luma does)
};
// The film grain synthesis process on a decoded picture, in place.
void av1ApplyFilmGrain(const Av1FilmGrain& g, Av1GrainPicture& pic);

// The standard deviation of the grain the synthesis draws for each plane before it is scaled (its 73 by 82 template,
// samples of `bitDepth` bits), and how the chroma's goes with the luma's under it (their correlation), over many seeds.
struct Av1GrainTemplate {
    double sigma[3] = {};
    double chromaWithLuma[2] = {};
    // Its autocorrelation at one sample's distance across, for the luma: how soft the grain is.
    double lumaNeighbour = 0;
};
Av1GrainTemplate av1GrainTemplate(const Av1FilmGrain& g, int bitDepth, bool subX, bool subY);

// What the coded picture is, for making parameters from a measured grain.
struct Av1GrainTarget {
    int width = 1920;             // of the coded picture (the grain scales with the picture)
    int bitDepth = 8;
    bool subX = true, subY = true;
    bool fullRange = false;
    double kr = 0.2126, kb = 0.0722;  // the matrix's luma weights (BT.709's; BT.2020's 0.2627 and 0.0593)
};
// A measured grain (a profile of pictures in `space`, the space the coded picture is in) as film grain parameters:
// the luma's and the chroma's scaling functions from the grain's strength at each light, through the space's transfer
// function and the matrix; the autoregression coefficients from its spectrum, at the coded picture's size; the chroma's
// share of the luma's grain from how the grain's colours go together. `amount` scales its strength. Not applied
// (apply false) when the profile holds no grain.
Av1FilmGrain av1FilmGrainFrom(const GrainProfile& g, const ColorSpace& space, const Av1GrainTarget& t, double amount = 1);

// The seed of frame `frame` of a stream: a new one every frame, never 0.
uint16_t av1GrainSeed(int64_t frame, uint32_t stream = 0);

// A film grain table: each entry the parameters for the pictures shown from `start` to `end` (in units of 100 ns).
// The format has no place for clip_to_restricted_range: read back, it is off.
struct Av1GrainTableEntry {
    int64_t start = 0, end = 0;
    Av1FilmGrain grain;
};
std::string av1FilmGrainTable(const std::vector<Av1GrainTableEntry>& entries);
bool parseAv1FilmGrainTable(const std::string& text, std::vector<Av1GrainTableEntry>& out, std::string* error = nullptr);

}  // namespace montage
