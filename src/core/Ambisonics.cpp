#include "Ambisonics.h"

#include <algorithm>
#include <cmath>

namespace montage {

namespace {

constexpr double kDeg = M_PI / 180;
constexpr double kHeadRadius = 0.0875;  // metres
constexpr double kSoundSpeed = 343.0;   // metres a second

// Unit direction in ambisonic axes (X ahead, Y to the left, Z up) from Montage's angle (positive to the right).
void direction(double angle, double elevation, double& x, double& y, double& z) {
    const double az = -angle * kDeg, el = elevation * kDeg;
    x = std::cos(az) * std::cos(el);
    y = std::sin(az) * std::cos(el);
    z = std::sin(el);
}

}  // namespace

std::array<float, 4> foaEncode(double angle, double elevation, double focus) {
    double x, y, z;
    direction(angle, elevation, x, y, z);
    const double f = std::clamp(focus, 0.0, 1.0);
    return {1.0f, float(y * f), float(z * f), float(x * f)};
}

FoaRotation foaRotation(double yaw, double pitch, double roll) {
    FoaRotation r;
    r.identity = std::fabs(yaw) < 1e-9 && std::fabs(pitch) < 1e-9 && std::fabs(roll) < 1e-9;
    const double cy = std::cos(yaw * kDeg), sy = std::sin(yaw * kDeg), cp = std::cos(pitch * kDeg), sp = std::sin(pitch * kDeg),
                 cr = std::cos(roll * kDeg), sr = std::sin(roll * kDeg);
    // Undo the view's turn: its yaw (about Z), then its pitch (about Y), then its roll (about X).
    const double Y[3][3] = {{cy, -sy, 0}, {sy, cy, 0}, {0, 0, 1}};
    const double P[3][3] = {{cp, 0, sp}, {0, 1, 0}, {-sp, 0, cp}};
    const double R[3][3] = {{1, 0, 0}, {0, cr, sr}, {0, -sr, cr}};
    double PY[3][3] = {}, M[3][3] = {};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) PY[i][j] += P[i][k] * Y[k][j];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) M[i][j] += R[i][k] * PY[k][j];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i][j] = float(M[i][j]);
    return r;
}

void foaRotate(const FoaRotation& r, float* wyzx) {
    if (r.identity) return;
    const float x = wyzx[3], y = wyzx[1], z = wyzx[2];
    wyzx[3] = r.m[0][0] * x + r.m[0][1] * y + r.m[0][2] * z;
    wyzx[1] = r.m[1][0] * x + r.m[1][1] * y + r.m[1][2] * z;
    wyzx[2] = r.m[2][0] * x + r.m[2][1] * y + r.m[2][2] * z;
}

void foaDecodeStereo(const float* wyzx, float& left, float& right) {
    // Cardioids (half omni, half figure of eight) at 60° left and right.
    constexpr float kx = 0.5f * 0.5f, ky = 0.5f * 0.8660254f;
    left = 0.5f * wyzx[0] + kx * wyzx[3] + ky * wyzx[1];
    right = 0.5f * wyzx[0] + kx * wyzx[3] - ky * wyzx[1];
}

FoaBinaural::FoaBinaural(double sampleRate) : sr_(sampleRate > 0 ? sampleRate : 48000) {
    // A cube of eight virtual speakers, decoded with max-rE weighting (first order in 3D: 0.577).
    const double el = std::atan(1 / std::sqrt(2.0)) / kDeg;  // 35.26°
    const double maxDelay = kHeadRadius / kSoundSpeed * (2 + M_PI / 2) * sr_;
    while (size_ < int(std::ceil(maxDelay)) + 4) size_ *= 2;
    const double tau = kHeadRadius / (2 * kSoundSpeed), K = 2 * sr_;
    for (double up : {el, -el})
        for (double angle : {-45.0, 45.0, 135.0, -135.0}) {
            Speaker s;
            double x, y, z;
            direction(angle, up, x, y, z);
            const double g1 = 0.125 * 3 * 0.57735;
            s.gain[0] = 0.125f;
            s.gain[1] = float(g1 * y);
            s.gain[2] = float(g1 * z);
            s.gain[3] = float(g1 * x);
            for (int e = 0; e < 2; ++e) {
                // The angle between the ear's axis (left ear +Y, right ear -Y) and the speaker.
                const double theta = std::acos(std::clamp(e == 0 ? y : -y, -1.0, 1.0));
                const double t = theta < M_PI / 2 ? -std::cos(theta) : theta - M_PI / 2;
                s.ear[e].delay = float((1 + t) * kHeadRadius / kSoundSpeed * sr_);
                const double alpha = (1 + 0.05) + (1 - 0.05) * std::cos(theta / (150 * kDeg) * M_PI);
                const double norm = 1 + tau * K;
                s.ear[e].b0 = float((1 + alpha * tau * K) / norm);
                s.ear[e].b1 = float((1 - alpha * tau * K) / norm);
                s.ear[e].a1 = float((1 - tau * K) / norm);
            }
            s.history.assign(size_t(size_), 0.0f);
            speakers_.push_back(std::move(s));
        }
}

void FoaBinaural::reset() {
    for (Speaker& s : speakers_) {
        std::fill(s.history.begin(), s.history.end(), 0.0f);
        for (Ear& e : s.ear) e.x1 = e.y1 = 0;
    }
    pos_ = 0;
}

void FoaBinaural::process(const float* in, int frames, float* out) {
    const int mask = size_ - 1;
    for (int i = 0; i < frames; ++i) {
        const float* b = in + size_t(i) * 4;
        float ears[2] = {0, 0};
        pos_ = (pos_ + 1) & mask;
        for (Speaker& s : speakers_) {
            s.history[size_t(pos_)] = s.gain[0] * b[0] + s.gain[1] * b[1] + s.gain[2] * b[2] + s.gain[3] * b[3];
            for (int e = 0; e < 2; ++e) {
                Ear& ear = s.ear[e];
                const int whole = int(ear.delay);
                const float frac = ear.delay - float(whole);
                const float a = s.history[size_t((pos_ - whole) & mask)], c = s.history[size_t((pos_ - whole - 1) & mask)];
                const float x = a + (c - a) * frac;
                const float y = ear.b0 * x + ear.b1 * ear.x1 - ear.a1 * ear.y1;
                ear.x1 = x;
                ear.y1 = y;
                ears[e] += y;
            }
        }
        out[size_t(i) * 2] = ears[0];
        out[size_t(i) * 2 + 1] = ears[1];
    }
}

}  // namespace montage
