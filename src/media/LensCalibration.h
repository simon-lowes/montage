// Montage — lens calibration: a lens profile made from footage of a checkerboard, or from lines in a picture that
// should be straight (the plumb-line method), anamorphic lenses included. A board's inner corners are found as saddle
// points of the picture and refined to a fraction of a pixel; Zhang's closed form (a planar target's homographies)
// gives the focal length and the board's poses, and Levenberg–Marquardt fits the focal length, the lens's centre, the
// distortion (lensfun's poly3, poly5 or ptlens), an anamorphic squeeze and every pose together, so the board's
// corners are reprojected where they were seen. The result is a lensfun lens (core/LensProfile.h), saved to the user's
// lens folder so Lens Correction offers it, and the ST maps VFX vendors undistort and redistort plates with.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "core/LensProfile.h"
#include "media/Tracking.h"

namespace montage {

// Pixel positions here are as lens::Correction's: their centres at +0.5, y down.

// A checkerboard's `cols` x `rows` inner corners in `img` (a board of cols + 1 by rows + 1 squares), row by row from the
// corner where the board's first row and column meet (so board position (i, j) is corners[j * cols + i]); false when
// the whole board is not seen. A board with as many rows as columns can be found turned a quarter either way.
bool findCheckerboard(const GrayImage& img, int cols, int rows, std::vector<Point2>& corners);

struct LensFitOptions {
    lens::Distortion::Model model = lens::Distortion::Poly5;
    bool fitCenter = true;   // the lens's centre fitted (else the frame's middle)
    bool fitSqueeze = false;  // an anamorphic squeeze fitted (from `squeeze`), else `squeeze` kept
    double squeeze = 1;
    double centerX = 0, centerY = 0;  // the centre when it is not fitted (lensfun's units)
};

struct LensFit {
    lens::Distortion::Model model = lens::Distortion::Poly5;
    double terms[3] = {0, 0, 0};
    double focal = 0;                 // the focal length in halves of the frame's shorter side (0: not known)
    double centerX = 0, centerY = 0;  // lensfun's <center>: halves of the frame's shorter side from its middle
    double squeeze = 1;
    double rms = 0, worst = 0;        // pixels: the corners' reprojection (a board), or the lines' straightness
    int views = 0, points = 0;
    std::vector<double> viewRms;      // each board's (or line's)
};

// Fits a lens from boards seen in a width x height frame (each view a findCheckerboard result). Needs three views
// or more (two with the centre and squeeze held).
bool calibrateFromBoards(const std::vector<std::vector<Point2>>& views, int cols, int rows, int width, int height,
                         const LensFitOptions& options, LensFit& out, std::string* error = nullptr);
// Fits a lens's distortion from lines that should be straight (each three points or more, preferably far from the
// middle and many directions); the focal length is not known from lines.
bool calibrateFromLines(const std::vector<std::vector<Point2>>& lines, int width, int height,
                        const LensFitOptions& options, LensFit& out, std::string* error = nullptr);
// A line drawn on the picture (two points or more) as edge points: every `step` pixels along it, each moved along the
// line's normal (up to `reach` pixels) to the strongest edge there; points with no clear edge are left out.
std::vector<Point2> snapLineToEdge(const GrayImage& img, const std::vector<Point2>& drawn, double reach = 6, double step = 8);

struct LensIdentity {
    std::string maker, model;  // the lens's names in the database
    double focalMm = 0;        // its focal length (0: the fit's, from `crop`)
    double cropFactor = 0;     // the camera's (0: full frame)
    std::string mount;         // the camera's mount (else the lens is offered only when no camera is known)
};
// The fit as a lensfun lens for a width x height recording, in the units of the camera's sensor (3:2, of `cropFactor`)
// the recording is as large as fits on, so the camera's recordings of other shapes are corrected with it too.
lens::Lens calibratedLens(const LensFit& fit, int width, int height, const LensIdentity& id);
// The fit's real focal length in millimetres for a recording on a 3:2 sensor of `crop` (0: full frame).
double fitFocalMm(const LensFit& fit, int width, int height, double crop);

struct ClipCalibration {
    LensFit fit;
    int width = 0, height = 0;  // the frames' size
    int framesRead = 0, boardsFound = 0;
    std::vector<double> times;  // the views' times (seconds into the file)
};
// Calibrates from a clip of a checkerboard (media time `from` to `to`; all of it when `to` is not after `from`): up to
// `maxViews` frames where the board is seen and has moved since the last one kept.
bool calibrateClip(const std::string& path, double from, double to, int cols, int rows, const LensFitOptions& options,
                   ClipCalibration& out, std::string* error = nullptr, const std::function<void(double)>& progress = {},
                   const std::atomic<bool>* cancel = nullptr, int maxViews = 40);

// ST maps (Nuke's convention: red the source's x over its width, green 1 minus its y over its height, at the output
// pixel's centre) for a lens's distortion on a width x height recording, as Lens Correction draws it with no fit:
// `undistort` (overscanWidth x overscanHeight, the recording's frame in its middle) takes the recorded frame to the
// corrected picture, all of it; `redistort` (width x height) takes such a corrected plate back to the recorded frame.
// Each is red, green (and blue 0), width * height * 3 floats.
struct StMaps {
    int width = 0, height = 0, overscanWidth = 0, overscanHeight = 0;
    std::vector<float> undistort, redistort;
};
bool makeStMaps(const lens::Lens& lens, const lens::Settings& settings, int width, int height, StMaps& out,
                std::string* error = nullptr);
// As 32-bit EXRs (R, G, B); the overscan size is in each file's header (montage:overscan).
bool writeStMaps(const StMaps& maps, const std::string& undistortFile, const std::string& redistortFile,
                 std::string* error = nullptr);
// Where a source position (pixels) lands through an ST map (bilinear in the map), as a compositor applying it samples.
bool stMapLookup(const std::vector<float>& map, int width, int height, double x, double y, int sourceWidth,
                 int sourceHeight, double& sx, double& sy);

}  // namespace montage
