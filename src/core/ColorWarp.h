// Montage — Colour Warper (Resolve's): a mesh laid over hue and saturation,
// twelve spokes (every 30°, from red) by four rings (saturation 25, 50, 75 and
// 100 %) round a fixed centre. Dragging a point pulls the colours there, and
// those near it in proportion, to another hue and saturation; each point can
// also lighten or darken its colours. Between points the move is blended
// smoothly (bilinearly over hue and saturation), so greys never change.
//
// Only moved points are stored, as "spoke,ring,hue,sat,luma" groups separated
// by ';' (hue in degrees, saturation in 0..1 units, luma in stops).
#pragma once

#include <string>
#include <vector>

namespace montage {

constexpr int kWarpSpokes = 12;
constexpr int kWarpRings = 4;

struct WarpPoint {
    int spoke = 0;   // 0..11: hue spoke * 30°
    int ring = 1;    // 1..4: saturation ring / 4
    double dh = 0;   // hue moved, degrees
    double ds = 0;   // saturation moved
    double dl = 0;   // brightness, stops
    bool operator==(const WarpPoint&) const = default;
};

struct ColorWarp {
    std::vector<WarpPoint> points;  // moved points only
    bool empty() const { return points.empty(); }
    // The point's move (zero if it has not moved).
    WarpPoint at(int spoke, int ring) const;
    // Sets a point's move; a zero move removes it.
    void set(const WarpPoint& p);
};

bool parseColorWarp(const std::string& text, ColorWarp& out);
std::string colorWarpToString(const ColorWarp& w);

// Where a point of the mesh sits: hue in turns (0..1), saturation 0..1.
inline double warpHue(int spoke) { return double(spoke) / kWarpSpokes; }
inline double warpSat(int ring) { return double(ring) / kWarpRings; }

// Where a colour (hue in turns, saturation 0..1) goes: the new hue (turns,
// wrapped) and saturation, and the factor its value is multiplied by.
void warpHueSat(const ColorWarp& w, double hue, double sat, double& newHue, double& newSat, double& valueScale);

// The same with every point's move laid out in a table, for doing it per pixel.
struct WarpField {
    explicit WarpField(const ColorWarp& w);
    void apply(double hue, double sat, double& newHue, double& newSat, double& valueScale) const;
    double dh[kWarpSpokes][kWarpRings + 1] = {}, ds[kWarpSpokes][kWarpRings + 1] = {}, dl[kWarpSpokes][kWarpRings + 1] = {};
};

}  // namespace montage
