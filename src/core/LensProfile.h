// Montage — lens profiles in the lensfun database's format (lensfun.github.io; the data is CC BY-SA 3.0): cameras,
// lenses and their calibrations, a camera found from the name a recording gives, and the correction a profile makes
// (distortion, transverse chromatic aberration, vignetting, a fisheye's projection) computed as lensfun computes it.
#pragma once

#include <cfloat>
#include <memory>
#include <string>
#include <vector>

namespace montage::lens {

// How a lens projects the scene; lensfun's lens types.
enum class Projection { Unknown, Rectilinear, Fisheye, Panoramic, Equirectangular, Orthographic, Stereographic, Equisolid, Thoby };

// Distortion at one focal length: the undistorted radius ru to the recorded one rd, in units of half the shorter side
// of the calibration sensor. Poly3: rd = ru (1 - k1 + k1 ru²); Poly5: rd = ru (1 + k1 ru² + k2 ru⁴);
// PtLens: rd = ru (a ru³ + b ru² + c ru + 1 - a - b - c).
struct Distortion {
    enum Model { None, Poly3, Poly5, PtLens } model = None;
    double focal = 0;
    double realFocal = 0;  // measured (real-focal), else derived as lensfun does
    bool realFocalMeasured = false;
    double terms[3] = {0, 0, 0};
};

// Transverse chromatic aberration: where red and blue lie against green. Linear: r = k r (terms kr, kb);
// Poly3: r = r (b r² + c r + v) (terms vr, vb, cr, cb, br, bb).
struct Tca {
    enum Model { None, Linear, Poly3 } model = None;
    double focal = 0;
    double terms[6] = {1, 1, 0, 0, 0, 0};
};

// Vignetting (Hugin's pa model): light falls to c = 1 + k1 r² + k2 r⁴ + k3 r⁶ of the middle's, r = 1 at the
// calibration sensor's corner.
struct Vignetting {
    double focal = 0, aperture = 0, distance = 0;
    double k[3] = {0, 0, 0};
};

struct Calibration {
    double cropFactor = 0;     // of the sensor it was measured on
    double aspectRatio = 1.5;  // of that sensor (long side over short)
    std::vector<Distortion> distortion;
    std::vector<Tca> tca;
    std::vector<Vignetting> vignetting;
};

struct Camera {
    std::string maker, model, variant, mount;
    std::vector<std::string> names;   // every maker and model name given (translations too), for finding it
    std::vector<std::string> makers;  // the maker's names among them
    double cropFactor = 0;
};

struct Lens {
    std::string maker, model;
    std::vector<std::string> names;  // every maker and model name given
    std::vector<std::string> mounts;
    Projection type = Projection::Rectilinear;
    double cropFactor = 0, aspectRatio = 1.5;
    double minFocal = 0, maxFocal = 0;  // from <focal>, else the calibrations' range
    double minAperture = 0;             // widest f-number, when given
    // The lens's centre against the frame's middle, in halves of the frame's shorter side (lensfun's <center>; down
    // and right positive), and an anamorphic lens's squeeze (Montage's own <anamorphic>): the recorded picture is that
    // much narrower than the lens drew it, so its distortion is radial only once widened again.
    double centerX = 0, centerY = 0;
    double squeeze = 1;
    std::vector<Calibration> calibrations;
    bool own = false;  // one the user calibrated (read from their lens folder)
    bool calibrated() const;
};

struct Mount {
    std::string name;
    std::vector<std::string> compat;  // other mounts its cameras take (by adapter)
};

class Database {
public:
    // One database file's contents (lensfun's <lensdatabase>, version 1 or 2); false and why when it is not one.
    bool addXml(const std::string& xml, std::string* error = nullptr);
    // Every .xml file in `dir`; the number read. `own`: the user's own lenses (preferred for a fixed-lens camera).
    int addDirectory(const std::string& dir, bool own = false);

    const std::vector<Camera>& cameras() const { return cameras_; }
    const std::vector<Lens>& lenses() const { return lenses_; }
    bool empty() const { return lenses_.empty(); }

    // The camera a recording's camera name ("GoPro HERO11 Black", "Sony ILCE-7M4") is, or null: every word of one of
    // a camera's model names found in it (one at least not the maker's), the camera matching most words winning.
    const Camera* findCamera(const std::string& device) const;
    // Lenses for `camera`: those on its mount first (a fixed lens alone there), then those its mount takes by
    // adapter; only calibrated lenses. Every calibrated lens when `camera` is null.
    std::vector<const Lens*> lensesFor(const Camera* camera) const;
    // A fixed-lens camera's lens (a compact's, an action camera's, a drone's, a phone's): its mount is its own (not one
    // the database lists for interchangeable lenses) and one lens is on it (or one of the user's own); else null.
    const Lens* fixedLens(const Camera* camera) const;
    // By name as cameraName and lensName give it (or the words of one).
    const Camera* camera(const std::string& name) const;
    const Lens* lens(const std::string& name, const Camera* on = nullptr) const;

private:
    std::vector<Camera> cameras_;
    std::vector<Lens> lenses_;
    std::vector<Mount> mounts_;
};

std::string cameraName(const Camera& c);  // "Sony ILCE-7M4"
std::string lensName(const Lens& l);      // "Sony FE 24-70mm f/2.8 GM II"
std::string projectionName(Projection p);

// A lens alone as a database file (kept in a lens correction effect, so a project renders the same anywhere), and
// back.
std::string lensXml(const Lens& lens);
std::shared_ptr<const Lens> parseLens(const std::string& xml);

// The calibration lensfun picks for a picture of `crop`: the smallest sensor that is not much smaller than it.
// Then each kind interpolated as lensfun does (a Hermite spline across focal lengths; vignetting weighted by inverse
// distance across focal length, aperture and distance). False when the lens has none for that picture.
bool interpolateDistortion(const Lens& lens, double crop, double focal, Distortion& out, Calibration* from = nullptr);
bool interpolateTca(const Lens& lens, double crop, double focal, Tca& out);
bool interpolateVignetting(const Lens& lens, double crop, double focal, double aperture, double distance,
                           Vignetting& out);

struct Settings {
    double focal = 0;          // mm; 0: the lens's shortest
    double aperture = 0;       // f-number; 0: no vignetting correction
    double distance = 1000;    // focus distance in metres
    double cameraCrop = 0;     // the camera's crop factor (its stills sensor); 0: the calibration's
    double recordingCrop = 1;  // how much smaller than the sensor's width the recording's width is (1: all of it)
    bool distortion = true, tca = true, vignetting = true;
    bool rectilinear = true;   // a fisheye's picture made rectilinear
    enum Fit { Fill, Whole, None } fit = Fill;  // fill the frame, show all of the picture, or neither
    double scale = 1;          // on top of the fit
};

// One lens's correction for a frame of a size.
class Correction {
public:
    Correction(const Lens& lens, const Settings& settings, int width, int height);
    // Something to do.
    bool active() const { return geometric() || vignetting_; }
    bool geometric() const { return distortion_ || tca_ || rectilinear_ || scale_ != 1; }
    bool vignetting() const { return vignetting_; }
    double scale() const { return scale_; }
    double cropFactor() const { return crop_; }  // the recording's crop factor used
    // For the corrected frame's position (x, y) (pixels, their centres at +0.5): where its red, green and blue come
    // from in the recorded frame, as x, y pairs.
    void source(double x, double y, double rgb[6]) const;
    // The other way (green): the corrected frame's position whose picture comes from the recorded frame's (x, y);
    // false where none does.
    bool corrected(double x, double y, double& cx, double& cy) const;
    // The lens's centre in the frame (pixels, as `source`), and calibration units per pixel (down the frame; across,
    // times the squeeze).
    double centerX() const { return cx_; }
    double centerY() const { return cy_; }
    double unit() const { return unit_; }
    double squeeze() const { return squeeze_; }
    // The radius in the recorded frame (calibration units) a corrected radius comes from, for one channel (0..2).
    double sourceRadius(double r, int channel = 1) const;
    // How much darker the recorded frame is at (x, y) than in its middle (divide linear light by it).
    double falloff(double x, double y) const;

private:
    double cx_ = 0, cy_ = 0, unit_ = 1;  // middle in pixels, calibration units per pixel
    double squeeze_ = 1;
    double crop_ = 0;
    bool distortion_ = false, tca_ = false, vignetting_ = false, rectilinear_ = false;
    Distortion dist_;
    Tca tcaCal_;
    Vignetting vig_;
    Projection type_ = Projection::Rectilinear;
    double focal_ = 1;      // in calibration units, for the projection
    double vigUnit_ = 1;    // calibration units per vignetting unit
    double scale_ = 1;
    double rising_ = DBL_MAX;  // the corrected radius the green's source radius rises to (`corrected` stays inside)
    double radius(double r, int channel) const;  // sourceRadius before the scale
    double distort(double ru) const;
    double fisheyeRadius(double rRect) const;
};

}  // namespace montage::lens
