// Montage — reading Media Composer bins (.avb): the bin's object table read into a plain model of its clips and
// sequences. A bin is a file of numbered objects, each a four-letter class and its bytes in the file's byte order (the
// first two bytes say which), with references between them by number. The bin object lists its items (each a
// composition, or mob: a master clip, a source or file mob, a sequence), its view's columns and its display settings;
// each mob has its tracks, and each track a tree of components (sequences, source clips, fillers, transitions, timecode,
// edge code and effects holding tracks of their own). Objects are read as the model reaches them; one that cannot be read
// (a class this reader does not know, or damaged bytes) is kept as its class and the reason, and the rest of the bin is
// still read. Follows pyavb's reader (see AvidBin.cpp). The importer turns this model into Montage sequences and media.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace montage {

// A mob ID (a SMPTE UMID) as AAF keeps it: the 12-byte label, the length and instance number, then the material number
// with its UUID fields little-endian. This is the layout aafMobIdFromUmid() makes from an MXF file's package UMID, so a
// file mob's ID can be looked for among MXF files with it.
using AvidMobId = std::array<uint8_t, 32>;

// The ID written as pyavb and pyaaf2 write it ("urn:smpte:umid:060a2b34.01010101...").
std::string avidMobIdUrn(const AvidMobId& id);

// One entry of an attribute list (class ATTR): a name and an integer, text (decoded from Mac Roman to UTF-8), bytes or a
// reference to another object. A reference to another attribute list (as the "_USER" entry, the bin's custom columns,
// is) is read into `children`; to a bin (class MCBR, "_ORG_BIN") into `binName`; others keep only their class.
struct AvidAttribute {
    enum class Kind { Int, String, Bytes, Object };
    std::string name;
    Kind kind = Kind::Int;
    int32_t intValue = 0;
    std::string text;             // String: UTF-8; Bytes: the bytes
    std::string objectClass;      // Object: the referenced object's class ("" if the reference is empty)
    std::vector<AvidAttribute> children;  // Object of class ATTR
    std::string binName;          // Object of class MCBR
};
using AvidAttributes = std::vector<AvidAttribute>;

// The last entry named `name`, or nullptr.
const AvidAttribute* findAvidAttribute(const AvidAttributes& list, const std::string& name);

// A locator (pyavb's Marker, class TMBC, also called a time crumb). Locators hang on components, in the "_TMP_CRM" entry
// of a component's attributes; a locator's frame is the component's start plus `offset`, in the component's edit rate.
struct AvidLocator {
    int32_t offset = 0;              // comp_offset: frames from the start of the component it hangs on
    int32_t position = 0;            // the MobRef's position
    std::optional<AvidMobId> mobId;  // the mob it was placed in
    std::array<uint16_t, 3> rgb{};   // its colour, 16 bits a channel
    std::string comment;             // "_ATN_CRM_COM"
    std::string colorName;           // "_ATN_CRM_COLOR" (Red, Green, ...)
    std::string user;                // "_ATN_CRM_USER"
    AvidAttributes attributes;       // all of them
};

// A control point of a parameter's curve (PRCL) or of a control clip (CTRL), at offsetNum / offsetDen. A PRCL point's
// value is its integer or double; a CTRL point's is the rational valueNum / valueDen.
struct AvidControlPoint {
    int32_t offsetNum = 0, offsetDen = 1;
    int32_t timeScale = 0;
    double value = 0;
    int32_t valueNum = 0, valueDen = 1;
};

// An effect parameter (class PRIT, from a component's parameter list PRLS): its UUID, its name where this reader knows
// it (SpeedRatio, Level, Pan and a few others; else the UUID), and a constant value or a curve.
struct AvidParameter {
    std::string uuid;
    std::string name;
    int16_t valueType = 0;  // 1 integer, 2 double, 4 a reference
    int32_t intValue = 0;
    double doubleValue = 0;
    bool enabled = true;
    int32_t interpKind = 0;  // of the curve: 2 constant, 3 linear, 5 Bezier, 6 cubic
    std::vector<AvidControlPoint> points;
};

struct AvidComponent;

// A track of a mob or of an effect (pyavb's Track): its number, its component and the filler proxy (a TrackRef).
struct AvidTrack {
    std::optional<int16_t> index;
    std::shared_ptr<const AvidComponent> component;
    std::shared_ptr<const AvidComponent> fillerProxy;
    std::optional<int16_t> lockNumber;
    std::optional<int32_t> startPos;
    AvidAttributes attributes;
};

// A component. `classId` says which: SEQU (a sequence: `components`), SCLP (a source clip: `mobId`, `trackId`,
// `startTime`), FILL (filler), TCCP (timecode), ECCP (edge code), TRKR (a track reference), CTRL and PRCL (control
// clips), and the track groups, which hold `tracks`: TNFX (a transition), TKFX (a track effect), PVOL (pan and volume),
// ASPI (an AudioSuite plug-in), EQMB (EQ), SPED (motion effect), MASK (capture mask), STRB (strobe), REPT (repeat),
// RSET (an essence group), SLCT (a selector, as group clips use) and TRKG. A component that could not be read has
// `readable` false, its class and `error`.
struct AvidComponent {
    std::string classId;
    bool readable = true;
    std::string error;

    int16_t mediaKindId = 0;
    std::string mediaKind;  // picture, sound, timecode, edgecode, attribute, effectdata, DescriptiveMetadata,
                            // DataEssenceTrack, unknownN or "" (pyavb's names)
    double editRate = 0;
    std::string name;
    std::string effectId;  // EFF2_BLEND_DISSOLVE, EFF_TIMEWARP.MOTION_CTL, Audio Pan Volume, ...
    // Clips and track groups: as stored. Sequences: the sum of their components' lengths less their transitions'.
    int64_t length = 0;
    AvidAttributes attributes;
    std::vector<AvidLocator> locators;
    std::vector<AvidParameter> parameters;

    // SEQU
    std::vector<std::shared_ptr<const AvidComponent>> components;
    // SCLP
    AvidMobId mobId{};
    bool hasMobId = false;
    int16_t trackId = 0;
    int32_t startTime = 0;
    // TCCP: bit 0 of the flags is drop frame
    uint32_t timecodeFlags = 0;
    uint16_t fps = 0;
    uint32_t timecodeStart = 0;
    // ECCP
    std::string edgeHeader;  // 8 bytes
    uint8_t filmKind = 0, codeFormat = 0;
    uint16_t basePerf = 0;
    int32_t edgeStart = 0;
    // TRKR
    int16_t relativeScope = 0, relativeTrack = 0;
    // CTRL, PRCL
    int32_t interpKind = 0;
    std::vector<AvidControlPoint> points;

    // Track groups
    bool isTrackGroup = false;
    uint8_t mcMode = 0;
    int32_t numScalars = 0;
    std::vector<AvidTrack> tracks;
    // TNFX
    int32_t cutPoint = 0;
    // TNFX, TKFX and its kinds
    int32_t leftLength = 0, rightLength = 0;
    // SPED, MASK, STRB, REPT
    int32_t phaseOffset = 0;
    // SPED
    int32_t speedNum = 0, speedDen = 1;
    std::optional<double> offsetAdjust;
    // MASK
    bool isDouble = false;
    uint32_t maskBits = 0;
    // STRB
    int32_t strobe = 0;
    // SLCT
    bool isGanged = false;
    uint16_t selected = 0;
    // PVOL: raw, as stored
    int32_t level = 0, pan = 0;
    bool levelSet = false, panSet = false;
    // RSET
    std::optional<int32_t> repSetType;
};

// Where a descriptor's media is: a file path (FILE on the Mac, WINF on Windows, URLL), or a media file locator (MSML: the
// volume it was last on and the file mob's ID).
struct AvidMediaLocator {
    std::string classId;
    std::string path, pathPosix, pathUtf8, path2Utf8;  // FILE, WINF
    std::string lastKnownVolume, lastKnownVolumeUtf8;  // MSML
    int32_t domainType = 0;
    std::optional<AvidMobId> mobId;
};

// A source mob's media descriptor. `classId`: MDES (a plain one; on a file mob's physical media, with the file path),
// MDTP (tape), MDFM (film), MDNG (Nagra), and the media file descriptors MDFL, MULD (several: `children`), WAVE, AIFC,
// PCMA and MPGA (sound), DIDD, CDCI, MPGI, JPED and RGBA (picture), DATD and ANCD (data).
struct AvidDescriptor {
    std::string classId;
    bool readable = true;
    std::string error;
    uint8_t mobKind = 0;
    std::optional<AvidMediaLocator> locator;
    bool intermediate = false;
    std::shared_ptr<const AvidDescriptor> physicalMedia;
    std::string uuid;
    AvidAttributes attributes;
    // Media file descriptors
    bool isFile = false;
    double editRate = 0;
    int32_t length = 0;
    // MULD
    std::vector<std::shared_ptr<const AvidDescriptor>> children;
    // Picture: stored and displayed size, aspect ratio, compression (a four-letter code) and resolution ID
    int32_t storedWidth = 0, storedHeight = 0, displayWidth = 0, displayHeight = 0;
    int32_t aspectNum = 0, aspectDen = 0;
    std::string compression;
    int32_t resolutionId = 0;
    // Sound
    uint16_t channels = 0, bitsPerSample = 0;
    double sampleRate = 0;
    // MDTP
    int16_t cframe = 0;
};

// A mob (pyavb's Composition, class CMPO).
struct AvidMob {
    std::string name;
    AvidMobId mobId{};
    std::string urn;
    std::string mobType;  // CompositionMob, MasterMob, SourceMob ("" if the type is unknown)
    uint8_t mobTypeId = 0;
    // The usage code: 0 none, 1 precompute, 2 subclip, 3 effect, 4 group, 5 group's backing mob, 6 motion effect,
    // 7 master mob, 9 precompute file; `usage` names it as pyavb does ("" for 0, "unknown" for others).
    int32_t usageCode = 0;
    std::string usage;
    int64_t length = 0;
    double editRate = 0;
    int64_t lastModified = 0;  // seconds since 1970
    std::optional<int64_t> creationTime;
    AvidAttributes attributes;  // "_USER" holds the custom columns' values
    std::vector<AvidLocator> locators;
    std::vector<AvidTrack> tracks;
    std::shared_ptr<const AvidDescriptor> descriptor;  // source mobs
};

struct AvidBinColumn {
    std::string title;
    int16_t format = 0;
    int16_t type = 0;
    bool hidden = false;
};

struct AvidBinItem {
    int mob = -1;  // into AvidBin::mobs
    int16_t x = 0, y = 0;  // its place in frame view
    int32_t keyframe = 0;
    bool userPlaced = false;
};

struct AvidBin {
    std::string creatorVersion;
    std::string lastSave;     // as the header writes it ("2018/11/11 21:38:02")
    bool bigEndian = false;
    uint64_t uid = 0;
    bool largeBin = false;
    std::vector<AvidBinItem> items;
    std::vector<AvidMob> mobs;  // each once, in the order the items name them
    // The view (BVst)
    std::string viewName, viewKind;
    std::vector<AvidBinColumn> columns;
    std::vector<std::pair<int16_t, std::string>> formatDescriptors;
    // Display: the view mode and what it shows, as stored (pyavb does not name their values)
    int16_t displayMode = 0;
    int32_t displayMask = 0;
    bool sifted = false;
    std::vector<std::pair<uint8_t, std::string>> sortColumns;  // direction, column title
    int16_t macFont = 0, macFontSize = 0, macImageScale = 0, qlImageScale = 0;
    std::array<int16_t, 4> homeRect{};
    std::array<uint16_t, 3> backgroundColor{}, foregroundColor{};
    bool wasIconic = false;
    AvidAttributes attributes;
    // Objects that could not be read ("index CLASS: reason"), each once.
    std::vector<std::string> issues;

    const AvidMob* findMob(const AvidMobId& id) const;
    // Where each mob read is in `mobs` (the first of an ID), for findMob; mobs added later are searched one by one.
    std::map<AvidMobId, size_t> mobIndex;
    size_t indexedMobs = 0;
};

bool readAvidBin(const std::string& path, AvidBin& out, std::string* error = nullptr);
// The same from the file's bytes.
bool readAvidBinData(const std::string& data, AvidBin& out, std::string* error = nullptr);

}  // namespace montage
