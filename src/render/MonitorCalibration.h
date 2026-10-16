// Montage — monitor calibration. A display's calibration LUT (made by calibration software from a probe's
// measurements) corrects what reaches it: applied last, after the viewer's or the output's colour transform, to the
// pictures a display shows, and never to anything rendered, exported or baked. Read from a .cube (3D or 1D), an
// Autodesk .3dl or a CLF, chosen per display and per reference output (app/DisplayCalibration.h).
//
// The patch generator lets calibration software measure a display through Montage: the software listens on a TCP
// port (20002 by convention) and Montage connects to it; each message is a 4-byte big-endian length followed by an
// XML document naming the colour to show, in code values at a bit depth, with a background and the patch's place on
// the screen — the network pattern protocol that DisplayCAL (GPL-3.0) and other calibration packages speak to
// grading applications and to PGenerator.
#pragma once

#include <QImage>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "media/Image.h"

namespace montage {

struct Lut3D;  // (render/Processing.h)

// A calibration LUT from `path`: .cube, .3dl, or .clf/.ctf (baked to a 65^3 lattice over 0..1). Kept while the file is
// unchanged; null and why when it cannot be read.
std::shared_ptr<const Lut3D> loadCalibrationLut(const std::string& path, std::string* error = nullptr);
// An Autodesk .3dl (Lustre or Flame): an optional "3DMESH" / "Mesh <log2 intervals> <output bits>" header, a line of
// the input lattice's code values (10-, 12- or 16-bit, from the largest), then size^3 integer RGB triplets with blue
// varying fastest (10-, 12- or 16-bit, from the header or the largest value).
std::shared_ptr<const Lut3D> parse3dl(const std::string& text, std::string* error = nullptr);

// `img` through `lut` (8-bit RGB; alpha kept), as Lut3D::apply interpolates it. Returns RGBA8888.
QImage calibratedImage(const QImage& img, const Lut3D& lut);
// A frame through `lut` in place, as shown over black (opaque after).
void calibrateImage(Image& img, const Lut3D& lut);
// An HDR viewer picture (render/HdrView.h: RGBA half floats, linear light in Rec. 709 primaries relative to SDR
// white, shown at `sdrWhiteNits`) through a LUT made for a PQ signal in BT.2020 primaries, as an HDR display calibrated
// for PQ is measured.
void calibrateHdrPicture(std::vector<uint16_t>& rgba, int width, int height, const Lut3D& lut, double sdrWhiteNits = 203);

// One message from calibration software: the colour to show (code values at `bits`) over `background`, in the
// rectangle (x, y, width, height) given as fractions of the screen. A message may carry several rectangles
// (`shapes`), drawn in order.
struct CalibrationPatch {
    struct Shape {
        int rgb[3] = {0, 0, 0};
        double x = 0, y = 0, width = 1, height = 1;
    };
    std::vector<Shape> shapes;
    int background[3] = {0, 0, 0};
    int bits = 8;
    bool bitsGiven = false;  // else the smallest depth holding the values
    // A colour at `bits` as 0..1.
    double value(int code) const;
};
bool parseCalibrationPatch(const std::string& xml, CalibrationPatch& out, std::string* error = nullptr);
// The messages in a byte stream: `feed` takes what arrives and returns the patches completed by it.
class PatchStream {
public:
    std::vector<CalibrationPatch> feed(const char* data, size_t size);
    // The last message that could not be read (empty when none).
    const std::string& error() const { return error_; }
    // Out of step (a length no message has): the connection is to be dropped and made again.
    bool broken() const { return broken_; }

private:
    std::string buffer_;
    std::string error_;
    bool broken_ = false;
    int guessedBits_ = 8;
};
// The patch as a W x H picture: 8-bit for a screen (through `lut` when given, to verify a calibration), or as code
// values 0..1 for the reference output (sent as they are, not converted).
QImage patchImage(const CalibrationPatch& p, int width, int height, const Lut3D* lut = nullptr);
Image patchFrame(const CalibrationPatch& p, int width, int height, const Lut3D* lut = nullptr);

constexpr int kPatchGeneratorPort = 20002;

}  // namespace montage
