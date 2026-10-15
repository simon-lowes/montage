// Montage — OpenEXR beyond one picture: the layers (AOVs) a render writes into one file (diffuse, specular, normals,
// depth...), Cryptomatte ID mattes, and writing several layers at once with any of OpenEXR's compressions (HTJ2K and
// zstd included, which FFmpeg's reader and writer don't have). Built on the OpenEXR library (BSD-3).
//
// A layer is the channels sharing a name before their last dot ("diffuse.R", "diffuse.G" -> "diffuse"); the main
// picture's channels have no prefix (its name is ""). In a multi-part file each part's layers are named after the part
// too ("part.layer"). Cryptomatte layers (spec 1.2: "<name>00".."<name>NN", each holding two id/coverage ranks, named
// in the header's cryptomatte/<key>/ metadata with a manifest of object names) are listed apart, not as pictures.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace montage {

bool exrAvailable();  // built with OpenEXR
bool isExrPath(const std::string& file);
// The OpenEXR file holding the frame `seconds` into a media file (a sequence's own frame), "" when it isn't OpenEXR.
std::string exrFrameFile(const std::string& mediaPath, double seconds);

struct ExrLayer {
    std::string name;                   // "" for the main picture
    std::vector<std::string> channels;  // full channel names, as R, G, B, A where it has them
};

struct Cryptomatte {
    std::string name;                                // e.g. CryptoObject
    std::vector<std::string> channels;               // id, coverage, id, coverage... by rank
    std::vector<std::pair<std::string, float>> objects;  // the manifest: names and their ids, by name
    std::string objectName(float id) const;          // "" when the manifest doesn't have it
};

struct ExrInfo {
    int width = 0, height = 0;  // the display window
    double pixelAspect = 1;
    std::string compression;    // the first part's ("zip", "piz", "htj2k256"...)
    std::vector<ExrLayer> layers;
    std::vector<Cryptomatte> cryptomattes;
    bool hasLayer(const std::string& name) const;
    const Cryptomatte* cryptomatte(const std::string& name) const;  // "" = the first
};

bool readExrInfo(const std::string& file, ExrInfo& out, std::string* error = nullptr);
// The same, kept for the few files last asked for (null when unreadable); read again once the file changes.
std::shared_ptr<const ExrInfo> cachedExrInfo(const std::string& file);

// A layer as RGBA floats (as stored: OpenEXR's premultiplied alpha), the display window's size; the data window placed
// in it, black and (where the layer has alpha) transparent round it. One channel shows grey; X, Y, Z (normals,
// positions) show as R, G, B; a layer with no alpha is opaque.
bool readExrLayer(const std::string& file, const std::string& layer, int& width, int& height, std::vector<float>& rgba,
                  std::string* error = nullptr);

// Whether FFmpeg's EXR reader can't read this compression (HTJ2K, zstd), so OpenEXR reads it instead.
bool exrNeedsLibrary(const std::string& compression);

// A Cryptomatte's ranks for each pixel: `ranks` id/coverage pairs a pixel, the display window's size.
struct CryptomatteFrame {
    int width = 0, height = 0, ranks = 0;
    std::vector<float> pairs;  // width * height * ranks * 2
    Cryptomatte crypto;        // which one, and its manifest
    float idAt(int x, int y) const;  // the id covering most of the pixel (0 = none)
    // The matte of these ids: their coverage summed (0..1), width * height.
    std::vector<float> matte(const std::vector<float>& ids) const;
};
bool readCryptomatte(const std::string& file, const std::string& name, CryptomatteFrame& out, std::string* error = nullptr);
// The same, kept for the few frames last asked for (a mask reads the same frame for every effect that uses it); with
// `read` false, only one already kept (null otherwise), for the GUI thread.
std::shared_ptr<const CryptomatteFrame> cachedCryptomatte(const std::string& file, const std::string& name, bool read = true);

// Cryptomatte's ids: MurmurHash3 (x86, 32-bit, seed 0) of the name, its bits a float (never a NaN, infinity or
// denormal); written in manifests and in Montage's lists as 8 hex digits.
float cryptomatteId(const std::string& name);
std::string cryptomatteHex(float id);
bool parseCryptomatteHex(const std::string& hex, float& id);
std::vector<float> parseCryptomatteIds(const std::string& list);  // hex ids, separated by commas or spaces
std::string cryptomatteIdList(const std::vector<float>& ids);

// Writing: layers of 1 to 4 channels (interleaved floats, the image's size), half or full float, one compression.
struct ExrOutLayer {
    std::string name;                   // "" for the main picture
    std::vector<std::string> channels;  // e.g. {"R","G","B","A"} or {"A"} for a matte
    std::vector<float> data;            // width * height * channels.size()
    bool fullFloat = false;             // 32-bit (ids, depth) rather than half
};
// Every channel of a file as stored, for copying into another: in layers by the name before their last dot (half or
// full float as kept), the display window's size, each with the layer readExrInfo lists it under ("" the main picture)
// or the Cryptomatte it belongs to (its name). The Cryptomattes' header entries go in `attributes`, manifests inline.
struct ExrCopiedLayer {
    std::string listedAs;  // readExrInfo's layer, or the Cryptomatte's name
    bool cryptomatte = false;
    ExrOutLayer layer;
};
bool readExrChannels(const std::string& file, int& width, int& height, std::vector<ExrCopiedLayer>& out,
                     std::map<std::string, std::string>* attributes = nullptr, std::string* error = nullptr);

std::vector<std::string> exrCompressions();  // the names writeExr takes
std::string exrCompressionLabel(const std::string& name);
bool writeExr(const std::string& file, int width, int height, const std::vector<ExrOutLayer>& layers, const std::string& compression,
              const std::map<std::string, std::string>& attributes = {}, std::string* error = nullptr);

}  // namespace montage
