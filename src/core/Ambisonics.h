// Montage — ambisonic (spatial) audio, as Premiere's VR audio, Resolve's Fairlight ambisonics and YouTube's spatial
// audio take it: AmbiX (ACN channel order, SN3D normalisation) from first order (four channels W, Y, Z, X, as 360°
// cameras and ambisonic microphones record) to third order (16 channels, as Zylia's and Eigenmike's arrays and spatial
// mixes carry). The sound field turns with the view (yaw right, pitch up, roll clockwise, as Reframe 360° aims its
// view), sounds are placed in it from a direction, and it is heard as two virtual cardioid microphones (speakers), on
// a surround layout's speakers (an AllRAD decoder), or binaurally (headphones: through head-related impulse responses,
// a spherical-head model's or measured ones from a SOFA file).
//
// Directions follow Montage's surround convention: degrees from straight ahead, positive to the right, and elevation
// degrees above the ear.
#pragma once

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace montage {

constexpr int kFoaChannels = 4;  // W, Y, Z, X
constexpr int kMaxAmbisonicOrder = 3;

// (order + 1)²: 4, 9, 16.
inline int ambisonicChannels(int order) { return (order + 1) * (order + 1); }
// The highest order `channels` hold (1 to 3), or 0 under four.
int ambisonicOrderForChannels(int channels);
// The order a file marked as an ambisonic field of `marked` order is played at: no higher than its `channels` hold;
// 0 if it is not played as a field (not marked, or under four channels).
inline int playedAmbisonicOrder(int marked, int channels) {
    const int held = ambisonicOrderForChannels(channels);
    return marked > 0 && held > 0 ? std::min(std::min(marked, held), kMaxAmbisonicOrder) : 0;
}

// The four channel gains of a sound from a direction (W 1; Y, Z, X its direction scaled by `focus`, 0 heard
// everywhere alike, 1 a point).
std::array<float, 4> foaEncode(double angle, double elevation, double focus = 1);
// The same to any order (ambisonicChannels(order) gains): the real spherical harmonics in ACN order, SN3D; each
// order l scaled by focus^l.
void ambisonicEncode(int order, double angle, double elevation, float* out, double focus = 1);

// The turn that brings the direction a view looks at (yaw degrees right, pitch up, roll clockwise) to the front.
struct FoaRotation {
    float m[3][3];  // applied to (X, Y, Z)
    bool identity = true;
};
FoaRotation foaRotation(double yaw, double pitch, double roll);
// `first` and then `then`, as one turn.
FoaRotation foaCompose(const FoaRotation& first, const FoaRotation& then);
void foaRotate(const FoaRotation& r, float* wyzx);

// A turn of the whole field to an order: each order's channels mixed among themselves (found by fitting the
// harmonics of turned directions, so it is exact whatever the convention's signs).
struct AmbisonicRotation {
    int order = 1;
    bool identity = true;
    std::vector<float> band[kMaxAmbisonicOrder + 1];  // order l: (2l + 1)² gains, row-major
};
AmbisonicRotation ambisonicRotation(int order, const FoaRotation& turn);
void ambisonicRotate(const AmbisonicRotation& r, float* field);

// Two virtual cardioids 60° either side of the front: a stereo picture of the field for speakers (from its first
// order, the four channels every field starts with).
void foaDecodeStereo(const float* wyzx, float& left, float& right);

// An AllRAD decoder (Zotter and Frank's all-round ambisonic decoding: the field sampled on a dense, even grid of
// virtual speakers, each panned onto the real ones by VBAP, with max-rE weighting), for speakers at these directions
// (angle, elevation); LFE speakers get nothing. Speakers × ambisonicChannels(order) gains, row-major, scaled so a
// sound keeps its power. Below the lowest speakers it fades out, as AllRAD's imaginary speaker underneath does.
std::vector<float> allradDecoder(int order, const std::vector<std::array<double, 2>>& directions, const std::vector<bool>& lfe);

// Head-related impulse responses: what reaches each ear from a direction.
struct Hrirs {
    std::string name;  // where they came from, for people ("Spherical head model", a SOFA file's title)
    double sampleRate = 48000;
    int length = 0;    // taps
    std::vector<std::array<double, 2>> directions;  // angle, elevation
    std::vector<float> left, right;                  // directions × length
};
// The virtual speakers a binaural decode hears the field through: 50 directions spread evenly over the sphere.
const std::vector<std::array<double, 2>>& binauralDirections();
// A spherical head (Brown and Duda's model: the interaural delay and the head's shadow as a first-order shelf) at
// those directions.
std::shared_ptr<const Hrirs> sphericalHeadHrirs(double sampleRate);

// The field binaurally for headphones: decoded to binauralDirections() and each heard through its HRIRs, folded into
// one pair of filters per channel and convolved directly (no added latency). Scaled so a diffuse field keeps its
// power (half at each ear) whatever the head and order.
class AmbisonicBinaural {
public:
    // `hrirs` at binauralDirections() and this rate (nullptr: the spherical head).
    explicit AmbisonicBinaural(int order = 1, double sampleRate = 48000, std::shared_ptr<const Hrirs> hrirs = nullptr);
    void reset();
    // `in` has ambisonicChannels(order) channels interleaved, `out` two.
    void process(const float* in, int frames, float* out);
    int order() const { return order_; }
    double sampleRate() const { return sr_; }
    const std::shared_ptr<const Hrirs>& hrirs() const { return hrirs_; }

private:
    int order_, channels_, length_ = 1;
    double sr_;
    std::shared_ptr<const Hrirs> hrirs_;
    std::vector<float> filters_;  // channel × ear × length, reversed
    std::vector<float> history_;  // channel × 2·length: each sample written twice, so a window is always contiguous
    int pos_ = 0;
};

}  // namespace montage
