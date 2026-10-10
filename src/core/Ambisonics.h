// Montage — ambisonic (spatial) audio, as Premiere's VR audio, Resolve's Fairlight ambisonics and YouTube's spatial
// audio take it: first-order AmbiX (four channels W, Y, Z, X in ACN order with SN3D normalisation) recorded by 360°
// cameras and ambisonic microphones. The sound field turns with the view (yaw right, pitch up, roll clockwise, as
// Reframe 360° aims its view), sounds are placed in it from a direction, and it is heard as two virtual cardioid
// microphones (speakers) or binaurally (headphones: a spherical-head model over a cube of virtual speakers, with the
// time and level differences between the ears).
//
// Directions follow Montage's surround convention: degrees from straight ahead, positive to the right, and elevation
// degrees above the ear.
#pragma once

#include <array>
#include <vector>

namespace montage {

constexpr int kFoaChannels = 4;  // W, Y, Z, X

// The four channel gains of a sound from a direction (W 1; Y, Z, X its direction scaled by `focus`, 0 heard
// everywhere alike, 1 a point).
std::array<float, 4> foaEncode(double angle, double elevation, double focus = 1);

// The turn that brings the direction a view looks at (yaw degrees right, pitch up, roll clockwise) to the front.
struct FoaRotation {
    float m[3][3];  // applied to (X, Y, Z)
    bool identity = true;
};
FoaRotation foaRotation(double yaw, double pitch, double roll);
void foaRotate(const FoaRotation& r, float* wyzx);

// Two virtual cardioids 60° either side of the front: a stereo picture of the field for speakers.
void foaDecodeStereo(const float* wyzx, float& left, float& right);

// The field binaurally for headphones: decoded to a cube of virtual speakers, each heard at both ears through a
// spherical head (Brown and Duda's model: the interaural delay and the head's shadow as a first-order shelf).
class FoaBinaural {
public:
    explicit FoaBinaural(double sampleRate = 48000);
    void reset();
    // `in` has four channels interleaved, `out` two.
    void process(const float* in, int frames, float* out);
    double sampleRate() const { return sr_; }

private:
    struct Ear {
        float delay = 0;            // samples
        float b0 = 1, b1 = 0, a1 = 0;  // the head shadow (first order)
        float x1 = 0, y1 = 0;
    };
    struct Speaker {
        float gain[4];  // decoding gains for W, Y, Z, X
        Ear ear[2];
        std::vector<float> history;  // the speaker's feed, for the ears' delays
    };
    double sr_;
    std::vector<Speaker> speakers_;
    int pos_ = 0;
    int size_ = 64;
};

}  // namespace montage
