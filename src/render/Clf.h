// Montage — Common LUT Format (CLF 3, SMPTE ST 2136-1; Academy S-2014-006): the colour transforms ACES workflows pass
// along with their looks (an AMF's look and input transforms name them). A process list of operators: Matrix, Range,
// LUT1D, LUT3D, ASC_CDL, Log and Exponent, each in the bit depths it says, run in order on RGB. Half-float-domain
// 1D LUTs and the inverse LUTs of Autodesk's CTF are not read.
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace montage {

class ClfTransform {
public:
    struct Op;
    // Runs the process list on one colour (values as the first operator's input bit depth says, normalised to 0..1
    // for integer depths).
    void apply(float rgb[3]) const;
    std::string name, description;
    std::vector<std::shared_ptr<const Op>> ops;
};

// Reads a .clf (or a .ctf using only CLF's operators) from text; null with the reason when it is not one or uses
// something not read.
std::shared_ptr<const ClfTransform> parseClf(const std::string& xml, std::string* error = nullptr);
// The file, kept while it is unchanged on disk.
std::shared_ptr<const ClfTransform> loadClf(const std::string& path, std::string* error = nullptr);

}  // namespace montage
