// Montage — USD scenes (OpenUSD's file formats), read without the reference library: USDA text, USDC binary ("crate",
// versions 0.4 to 0.11, with its LZ4 and integer compression) and USDZ packages (a zip whose first file is the root
// layer, with the textures and layers it uses beside it). Each layer becomes a tree of prims with their attributes
// (defaults and time samples), relationships and connections. A stage composes the root layer with its sublayers,
// references and payloads (to prims in the same layer, or in files beside it or in the same package, with their time
// offsets), and the variants selected, into one flattened tree; inherits, specializes, relocates, value clips and
// variant fallbacks are not composed. Asset paths are resolved where they were authored, so a texture named in a
// referenced file is found beside that file.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace montage {
class ZipReader;
}

namespace montage::usd {

// An attribute's or metadatum's value: numbers (every component of every element in order; a bool is 0 or 1;
// quaternions as x, y, z, w; matrices row by row, as USD writes them) or texts (strings, tokens, asset paths and
// paths), as one value or an array.
struct Value {
    enum class Kind { None, Number, Text, Block } kind = Kind::None;  // Block: explicitly no value ("None")
    std::vector<double> numbers;
    std::vector<std::string> texts;
    int width = 1;  // components an element (3 for a vector of three, 16 for a 4 x 4 matrix)
    bool array = false;
    bool asset = false;  // texts are asset paths (resolved by the stage: see Stage::readAsset)
    size_t size() const;  // elements
    double number(size_t i = 0, double fallback = 0) const;
    std::string text(size_t i = 0) const;
    bool empty() const { return kind == Kind::None || kind == Kind::Block; }
};

struct Attribute {
    std::string typeName;  // as USD names it: "point3f[]", "token", "matrix4d"...
    bool uniform = false;
    Value value;  // its default
    std::vector<std::pair<double, Value>> samples;  // by time code, in order
    std::vector<std::string> connections;            // the attribute paths it reads from ("/Mat/Tex.outputs:rgb")
    std::map<std::string, Value> meta;               // interpolation, elementSize, colorSpace...
    // Its value at time code t: the default with no samples; held before the first sample and after the last; linear
    // between samples of the same size (slerped for quaternions), else the earlier one.
    Value at(double t) const;
    bool animated() const { return samples.size() > 1; }
    std::string metaText(const std::string& key, const std::string& fallback = {}) const;
};

// A reference or payload: a prim in another layer (`asset` empty: this one) or that layer's default prim (`prim`
// empty), its times shifted by t * scale + offset.
struct Arc {
    std::string asset, prim;
    double offset = 0, scale = 1;
};

struct Prim {
    std::string name, typeName;
    enum class Specifier { Def, Over, Class } specifier = Specifier::Def;
    std::vector<Prim> children;
    std::map<std::string, Attribute> attributes;
    std::map<std::string, std::vector<std::string>> relationships;  // their target paths
    std::vector<Arc> references, payloads;
    std::map<std::string, std::string> variantSelection;
    std::map<std::string, std::map<std::string, Prim>> variantSets;  // set, variant: what it adds to this prim
    std::vector<std::string> apiSchemas;
    std::string kind;
    bool active = true;

    Prim* child(const std::string& name);
    const Prim* child(const std::string& name) const;
    Prim& childOrNew(const std::string& name);
    const Attribute* attribute(const std::string& name) const;
    const std::vector<std::string>* relationship(const std::string& name) const;
};

struct Layer {
    Prim root;  // the pseudo-root: its children are the top prims
    std::string upAxis = "Y", defaultPrim;
    double metersPerUnit = 0.01;
    double timeCodesPerSecond = 0, framesPerSecond = 0;  // 0: not authored (24 is USD's fallback)
    double startTimeCode = 0, endTimeCode = 0;
    bool hasTimeRange = false;
    std::vector<std::string> subLayers;
    std::vector<std::pair<double, double>> subLayerOffsets;  // offset, scale
    double timeCodesPerSecondOr24() const;
};

// USDA text, USDC bytes, or either by its first bytes. False with `error` when it cannot be read.
bool parseUsda(const std::string& text, Layer& out, std::string* error = nullptr);
bool parseUsdc(const std::string& bytes, Layer& out, std::string* error = nullptr);
bool parseLayer(const std::string& bytes, Layer& out, std::string* error = nullptr);

// Reads a file whole (empty when it cannot), so the caller sees and limits what is read.
using FileReader = std::string (*)(const std::string& path);

// A composed stage: its layer has every arc flattened, and its asset paths resolved (to a file path, or to
// "package.usdz[inner/path]" for one in a package).
struct Stage {
    Layer layer;
    std::string path;                // the root layer's file
    std::vector<std::string> files;  // every file read (the root layer, its packages and the layers it composed)
    std::vector<std::string> notes;  // what could not be composed (a missing layer, a loop)
    const Prim* prim(const std::string& path) const;  // "/World/Cube"
    // The bytes of a resolved asset (a file, or a file in a package). False when it cannot be read.
    bool readAsset(const std::string& resolved, std::string& bytes) const;

    // (kept for readAsset)
    std::map<std::string, std::shared_ptr<const ZipReader>> packages_;
    FileReader reader_ = nullptr;
};

// The file's stage; false with `error` when the root layer cannot be read. `readFile` null reads files directly.
bool openStage(const std::string& path, Stage& out, std::string* error = nullptr, FileReader readFile = nullptr);

// Whether the path names a USD file (by extension: .usd, .usda, .usdc, .usdz).
bool isUsdPath(const std::string& path);

// ---- Writing USDA ----------------------------------------------------------------------------------------------------

// Numbers as USDA writes them: the shortest text that reads back as the same double (or float, when `single`).
std::string formatNumber(double v, bool single = false);
// A string quoted for USDA.
std::string quoted(const std::string& s);

}  // namespace montage::usd
