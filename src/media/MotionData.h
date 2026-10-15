// Montage — camera motion data: the gyroscope and accelerometer readings a camera (or a logger beside it) records
// while filming, and stabilisation from them. Read from GoPro's GPMF track (HERO5 on; 360° cameras' data is refused), a CAMM track
// (Camera Motion Metadata, as some cameras and phones write it) or a Gyroflow .gcsv log beside the clip (phone
// loggers). The readings are lined up with the picture by
// correlating them with the camera movement the point tracker measures (media/Tracking.h), which also finds which
// way round the sensor sits and the lens's focal length; integrated into the camera's orientation; and stabilised by
// turning each frame (each row, with a rolling shutter) to a smoothed orientation, the horizon levelled from gravity.
#pragma once

#include <array>
#include <atomic>
#include <string>
#include <vector>

#include "media/Tracking.h"

namespace montage {

// One reading, in the sensor's own axes and order.
struct ImuSample {
    double t = 0;                    // media seconds (from the start of the file's first frame)
    std::array<float, 3> gyro{};     // radians a second
    std::array<float, 3> accel{};    // metres a second squared (zeros when the source has none)
    bool accelOnly = false;          // a reading of the accelerometer alone (merged into the gyro's by readMotionData)
};

struct MotionData {
    std::string source;              // "GoPro GPMF", "CAMM", ".gcsv log"
    std::vector<ImuSample> samples;  // in time order
    bool hasAccel = false;
    double readout = -1;             // seconds the sensor takes to read a frame out, top to bottom, when the data says (-1 not)
    bool steadiedInCamera = false;   // the camera turned its picture to steady it (GPMF's image orientation, IORI)
    double rate() const;             // readings a second
};

// The motion data for a video file: its GPMF or CAMM track, else a .gcsv beside it with the same name. False (and why)
// when there is none.
bool readMotionData(const std::string& videoPath, MotionData& out, std::string* error = nullptr);
// Whether the file has any (cheap: looks for the track or the file only).
bool hasMotionData(const std::string& videoPath);

// The parsers, for one payload each (exposed for tests).
// GPMF (KLV): the GYRO and ACCL streams of a payload covering [t0, t1), their samples spread evenly over it.
bool parseGpmf(const uint8_t* data, size_t size, double t0, double t1, MotionData& out);
// CAMM: one sample (gyro, acceleration or exposure) at time t.
bool parseCamm(const uint8_t* data, size_t size, double t, MotionData& out);
// Gyroflow's CSV log (version 1.x): its scales and readout (a readout in another direction than top to bottom is not
// used); its orientation line is not needed (the axes are found by syncMotionData).
bool parseGcsv(const std::string& text, MotionData& out, std::string* error = nullptr);

// How the readings line up with the picture.
struct GyroSync {
    double offset = 0;                 // seconds added to a picture's time to find its readings
    std::array<int, 3> axis{0, 1, 2};  // the sensor channel for the camera's x (right), y (down), z (forward) axes
    std::array<int, 3> sign{1, 1, 1};
    double focal = 0;                  // the lens's focal length in frame widths (0 not measured)
    double readout = -1;               // seconds to read a frame out, when known otherwise (-1: the data's, if it says)
    double score = 0;                  // how well they matched (0..1)
};
// Finds the offset (within ±searchSeconds), the axes and the focal length from the tracked camera movement; false
// (and why) when they do not match well enough to trust.
bool syncMotionData(const MotionData& d, const CameraMotion& tracked, GyroSync& out, std::string* error = nullptr,
                    double searchSeconds = 2.0, const std::atomic<bool>* cancel = nullptr);

// Measures the focal length again from points followed between a dozen pairs of frames in [from, to): the ones that
// turned furthest, the turn between them from the data. The movement the tracker measures is a mean over the picture,
// which overstates the focal length by an amount that depends on where the points are; this is exact for a camera that
// only turns. False when too few points could be followed.
bool refineGyroLens(const std::string& videoPath, const MotionData& d, GyroSync& sync, double fps, double from, double to,
                    std::string* error = nullptr, const std::atomic<bool>* cancel = nullptr);

// The camera's orientation over a stretch of the picture, from synced readings: what a Stabilize effect keeps.
struct GyroPath {
    std::string source;
    double fps = 0;        // the picture's frame rate
    double start = 0;      // media seconds of the first sample (a picture time)
    int perFrame = 2;      // samples a frame
    double focal = 0;      // frame widths
    double readout = -1;   // seconds, from the data (-1 not known)
    struct Sample {
        std::array<double, 4> q{1, 0, 0, 0};  // camera to world (w, x, y, z); the first is the identity
        std::array<double, 3> up{0, 0, 0};    // the way up (against gravity) in the camera's axes, unit (zeros unknown)
    };
    std::vector<Sample> samples;
};
GyroPath gyroPath(const MotionData& d, const GyroSync& sync, double fps, double from, double to, int perFrame = 2);
std::string gyroPathToString(const GyroPath& p);
bool gyroPathFromString(const std::string& s, GyroPath& p);
// Only its header (cheap): where the data came from and the focal length.
bool gyroPathSummary(const std::string& s, std::string& source, double& focal);

// Stabilising from a path: for each frame, the rotation from the stabilised view to the camera at the middle of the
// frame and the camera's turning speed then (radians a frame, camera axes), for rolling shutter.
struct GyroCorrection {
    std::array<double, 9> r{1, 0, 0, 0, 1, 0, 0, 0, 1};  // row-major: camera ray = r * view ray
    std::array<double, 3> spin{0, 0, 0};
};
struct GyroStabilizeSettings {
    double smoothSeconds = 1.5;  // 0 holds the first frame's view
    double horizon = 0;          // 0..1: how far the horizon is levelled
    double offset = 0;           // seconds more than the sync found
};
std::vector<GyroCorrection> gyroCorrections(const GyroPath& p, const GyroStabilizeSettings& s);
// Where output pixel (x, y) (centred, in frame widths, before zoom) is found in the source frame (centred, in widths),
// with `readout` frames between the top and bottom rows read; false when it is behind the camera.
bool gyroSourcePoint(const GyroCorrection& c, double focal, double readout, double aspect, double x, double y, double& sx,
                     double& sy);
// The turn a camera ray takes `dt` frames from the middle of the frame (row-major; camera ray then = it * ray).
std::array<double, 9> gyroTurnAt(const GyroCorrection& c, double dt);
// The zoom that keeps every frame covered.
double gyroZoom(const std::vector<GyroCorrection>& cs, double focal, double readout, double aspect);

}  // namespace montage
