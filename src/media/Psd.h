// Montage — layered Photoshop files (Premiere's merged, single-layer or sequence import; Final Cut's layered graphics):
// lower thirds, thumbnails and graphics come from designers as PSDs. The file is read here with no library: PSD and
// PSB, RGB, greyscale and CMYK at 8 or 16 bits, layer names, bounds, blend modes, opacity and fill, visibility,
// clipping masks, groups and layer masks, channels raw, PackBits or ZIP. A single layer is media of its own, named
// "dir/art.psd#layer=3": a still the size of the canvas with the layer where it sits and transparent elsewhere.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/Model.h"

namespace montage {

struct PsdLayer {
    int index = 0;                               // in the file's order, bottom first
    std::string name;
    int left = 0, top = 0, right = 0, bottom = 0;  // its pixels' bounds on the canvas
    std::string blend = "norm";                  // Photoshop's blend key ("mul ", "scrn", "pass"...)
    double opacity = 1;                          // opacity times fill, 0..1
    bool visible = true;
    bool clipped = false;                        // clipped to the layer below (a clipping mask)
    int group = -1;                              // the index of the group's own record it sits in, -1 for none
    bool isGroup = false;                        // a group's own record: its name, opacity and visibility
    bool isGroupEnd = false;                     // the hidden record closing a group
    bool adjustment = false;                     // an adjustment or fill layer (its pixels are not what it does)
    bool hasPixels() const { return !isGroup && !isGroupEnd && right > left && bottom > top; }
};

struct PsdInfo {
    int width = 0, height = 0;
    int depth = 8;     // bits per channel
    int mode = 3;      // 1 greyscale, 3 RGB, 4 CMYK
    int channels = 3;  // of the merged picture
    bool psb = false;
    std::vector<PsdLayer> layers;
};

bool readPsdInfo(const std::string& path, PsdInfo& info, std::string* error = nullptr);
// The picture of layer `layer` (its index; -1 = the merged picture) over the whole canvas as 16-bit RGBA with straight
// alpha, transparent outside the layer and with its layer mask applied (not its opacity, blend mode or visibility).
bool readPsdPixels(const std::string& path, int layer, PsdInfo& info, std::vector<uint16_t>& rgba, std::string* error = nullptr);

bool isPsdFile(const std::string& path);  // .psd or .psb
bool parsePsdLayerPath(const std::string& path, std::string& file, int& layer);
std::string psdLayerPath(const std::string& file, int layer);
// A Photoshop blend key as one of Montage's blend modes (core/Effects.h blendModes()); "normal" where none is close.
std::string psdBlendMode(const std::string& key);

enum class PsdImport { Merged, Layers, Sequence };
// Brings a PSD into the project: merged as one still; or a still per layer with pixels, in a bin "<name> Layers"; or,
// besides those, a sequence at the canvas size holding them, a track per layer bottom up named for it, with its blend
// mode, opacity (times its groups'), visibility (hidden layers' clips disabled), group (a track folder) and clipping
// mask (Track Matte Key on the layer it is clipped to), `seconds` long. Adjustment layers are left out. Returns the
// media ids made (the sequence's last); empty with `error` if the file cannot be read.
std::vector<Id> importPsd(Project& p, const std::string& path, PsdImport mode, double seconds = 5, std::string* error = nullptr);

}  // namespace montage
