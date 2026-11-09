// Montage — DNG opcode lists (the Digital Negative specification, chapter 7): the corrections a camera records for its
// raw developer to make, which LibRaw does not apply. Phones and CinemaDNG cameras use them for lens shading (a gain map
// per colour of the mosaic), vignetting, distortion and lateral chromatic aberration (warps), and bad pixels.
//
// OpcodeList1 is applied to the raw values as stored, OpcodeList2 to them once mapped to linear (0 to 1 between the black
// and white levels), OpcodeList3 to the picture once demosaiced. Montage applies WarpRectilinear, WarpFisheye,
// FixVignetteRadial, GainMap, FixBadPixelsConstant and FixBadPixelsList; the others (and WarpRectilinear2) are passed
// over, and the warp after an optional WarpRectilinear2 is applied as the specification's skip rule has it (Montage does
// not apply WarpRectilinear2 itself).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace montage {

struct DngOpcode {
    uint32_t id = 0;
    uint32_t version = 0;
    uint32_t flags = 0;  // bit 0 optional, bit 1 may be skipped for previews
    std::vector<uint8_t> params;  // big-endian, as stored
};
using DngOpcodeList = std::vector<DngOpcode>;

enum DngOpcodeId : uint32_t {
    kWarpRectilinear = 1,
    kWarpFisheye = 2,
    kFixVignetteRadial = 3,
    kFixBadPixelsConstant = 4,
    kFixBadPixelsList = 5,
    kGainMap = 9,
    kWarpRectilinear2 = 14,
};

// An opcode list's bytes into its opcodes (false for a list that runs past its end).
bool parseDngOpcodeList(const uint8_t* data, size_t size, DngOpcodeList& out);
// Its bytes back (for tests and for writing DNGs).
std::vector<uint8_t> dngOpcodeListBytes(const DngOpcodeList& list);

// A DNG's three opcode lists (from its raw IFD: the one whose NewSubFileType is 0, in IFD 0 or its SubIFDs); all empty
// for a file without any, or one that is not a TIFF-based DNG.
struct DngOpcodeLists {
    DngOpcodeList list[3];
    bool empty() const { return list[0].empty() && list[1].empty() && list[2].empty(); }
};
bool readDngOpcodeLists(const std::string& path, DngOpcodeLists& out);

// The opcodes of list 1 or 2 (`stage`) applied to a Bayer mosaic of raw values (`stride` values a row): `black` and
// `white` its levels (list 2's opcodes work between them, as 0 to 1), with `blackPattern` (`patternRows` by
// `patternCols`, repeating from the top left) added to the black where the sensor's black differs by position. Returns
// how many were applied, or -1 when `cancel` stopped it (heard between opcodes and between rows; the mosaic then part
// done). Each opcode's pixels are worked across the cores; `progress` (0 to 1) is told after each opcode.
int applyDngMosaicOpcodes(const DngOpcodeList& list, int stage, uint16_t* raw, int width, int height, int stride, double black,
                          double white, const double* blackPattern = nullptr, int patternRows = 0, int patternCols = 0,
                          const std::atomic<bool>* cancel = nullptr, const std::function<void(double)>& progress = {});
// The opcodes of list 3 applied to a demosaiced picture: `planes` floats a pixel (linear, 0 to 1), interleaved. A picture
// developed smaller than the raw (half size) gives the full size, which gain maps' areas are given in. Returns how many
// were applied, or -1 when cancelled (as applyDngMosaicOpcodes).
int applyDngPictureOpcodes(const DngOpcodeList& list, float* pixels, int width, int height, int planes, int fullWidth = 0,
                           int fullHeight = 0, const std::atomic<bool>* cancel = nullptr, const std::function<void(double)>& progress = {});

// Where a warp takes the picture's pixel (x, y) from in the picture before it (the specification's x', y'), for one
// plane; false for an opcode that is not a warp, or a plane it has no coefficients for and no single set.
bool dngWarpSource(const DngOpcode& op, int plane, double x, double y, int width, int height, double& sx, double& sy);

}  // namespace montage
