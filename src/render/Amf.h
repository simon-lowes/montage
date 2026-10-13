// Montage — ACES Metadata Files (AMF 2.0, Academy S-2019-001): the XML that carries a clip's ACES pipeline from set to
// editorial to VFX and the grade: the input transform its pictures need (a camera's into ACES), the looks on it (an ASC
// CDL in ACEScct, a CLF file, the Reference Gamut Compression) and the output transform it is viewed through. Read
// into a clip's Interpret Colour and effects, and written from them, one file per clip (and per VFX pull).
#pragma once

#include <string>
#include <vector>

#include "core/Cdl.h"
#include "core/Model.h"

namespace montage {

struct AmfLook {
    enum class Kind { Cdl, File, TransformId };
    Kind kind = Kind::Cdl;
    bool applied = false;     // already in the pictures
    std::string description;
    Cdl cdl;
    std::string cdlSpace;     // Kind::Cdl: the space it was made in (ColorSpace.h id; "" = not one Montage knows)
    std::string cdlSpaceId;   // ... as the file names it (a transform id or file)
    std::string file;         // Kind::File: the LUT (CLF), as an absolute path when the AMF's folder was known
    std::string transformId;  // Kind::TransformId
};

struct AmfDocument {
    std::string description;  // the file's (amfInfo)
    std::string uuid;         // the file's, "urn:uuid:..."
    // The clip it is for (any may be empty).
    std::string clipName, clipFile, clipUuid;
    std::string clipSequence;  // a numbered file sequence, the number as '#' (e.g. "A001_C012_####.exr")
    int sequenceMin = 0, sequenceMax = 0;
    // The input transform.
    bool hasInput = false, inputApplied = false;
    std::string inputTransformId;  // an IDT/CSC, an inverse output transform, or (with inputFile) none
    std::string inputFile;
    std::string inputSpace;  // the colour space it reads the pictures as (ColorSpace.h id; "" = not one Montage knows)
    std::vector<AmfLook> looks;
    // The output transform.
    bool hasOutput = false, outputApplied = false;
    std::string outputTransformId;
    std::string outputSpace;  // the display space it renders for ("" = not one Montage has), and its peak
    double outputPeakNits = 0;
};

// Reads an AMF (relative file references made absolute against `folder`); false with the reason when it is not one.
bool parseAmf(const std::string& xml, const std::string& folder, AmfDocument& out, std::string* error = nullptr);
bool readAmf(const std::string& path, AmfDocument& out, std::string* error = nullptr);
// The XML, with a new uuid and the time now where they are empty.
std::string writeAmf(const AmfDocument& amf);

// ACES transform ids (ACES 1 and 2, as AMFs name them) and Montage's colour spaces, both ways. The space an input
// transform reads pictures as, or "" (`exact` false when it is only near, e.g. a LogC3 IDT for another exposure index).
std::string acesInputSpace(const std::string& transformId, bool* exact = nullptr);
// The display space and peak an output transform renders for, or "" (cinema P3, XYZ: none of Montage's).
std::string acesOutputSpace(const std::string& transformId, double* peakNits = nullptr);
// The ACES 2 ids for a space; "" when ACES has none (input: a space without a CSC; output: Rec.2020 SDR).
std::string acesInputTransformId(const std::string& spaceId);
std::string acesOutputTransformId(const std::string& spaceId, double peakNits);
// A Common LUT Format file taking a space's code values to ACES2065-1 (its curve as a 1D LUT, then its primaries), for
// what ACES has no id for; Montage reads its own back as that space.
std::string clfToAces(const std::string& spaceId);

// Gives clips what an AMF describes: their media read as its input transform's space, its looks not yet applied as
// effects first in their chains (an earlier AMF's replaced). `clips` empty: the sequence's video clips whose names (the
// clip's, its media's, file or tape) match the AMF's clip; else those clips. The output transform can set the sequence's
// colour space. `notes` says what could not be carried (an unknown transform, a missing look file).
struct AmfImportOptions {
    bool setSequenceSpace = false;
};
struct AmfImportResult {
    int clips = 0;
    std::vector<std::string> notes;
};
AmfImportResult applyAmf(Project& p, Sequence& seq, const std::vector<Id>& clips, const AmfDocument& amf,
                         const AmfImportOptions& options = {});

// A clip's AMF: its media's space as the input transform, its CDL, CLF looks and gamut compression as looks, the
// sequence's space as the output transform. `folder` is where the AMF goes (a CLF Montage writes for a space ACES has no
// id for, and look files elsewhere, are put there); `pictureSpace` names the space the pictures it goes with are in when
// they are not the media's own (a VFX pull's plates). `notes` says what in the grade it could not carry.
bool clipAmf(const Project& p, const Sequence& seq, const Clip& clip, const std::string& folder, AmfDocument& out,
             std::vector<std::string>* notes = nullptr, const std::string& pictureSpace = {}, std::string* error = nullptr);
// clipAmf written to `path`.
bool writeClipAmf(const Project& p, const Sequence& seq, const Clip& clip, const std::string& path,
                  std::vector<std::string>* notes = nullptr, const std::string& pictureSpace = {}, std::string* error = nullptr);

}  // namespace montage
