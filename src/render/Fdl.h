// Montage — ASC Framing Decision Lists (FDL: the ASC's open JSON format, read, checked and transformed with the ASC's
// own libfdl_core): the framing a production chose for each camera's picture (the frame, and the room protected
// around it), carried from the set through editorial to visual effects and finishing.
//  - Read and checked against the FDL schema and its rules, and attached to the footage it describes (by its clip_id,
//    else by the picture's size): MediaItem::fdl.
//  - Frame lines over the Source and Program monitors, and burnt into exports (BurnIn::frameLines).
//  - A canvas template (the FDL's own, or another FDL's) applied to clips: each scaled, placed and cropped as the
//    template's pipeline says, so its framing lands where the delivery frame asks.
//  - Written out: beside each VFX pull's plate, and for a sequence (a context for each clip), saying what the cut
//    shows of each recording as a framing decision on its canvas.
#pragma once

#include <array>
#include <string>
#include <vector>

#include "core/Model.h"

class QPainter;
class QRectF;

namespace montage {

struct FdlRect {
    double x = 0, y = 0, w = 0, h = 0;
};

struct FdlFraming {
    std::string id, label, intent;
    FdlRect rect;
    bool protection = false;
    FdlRect protectionRect;
};

struct FdlCanvas {
    std::string id, label, sourceId;
    int width = 0, height = 0;
    double squeeze = 1;
    bool effective = false;
    FdlRect effectiveRect;
    std::vector<FdlFraming> framings;
};

struct FdlContext {
    std::string label, creator;
    std::string clipName, clipFile;  // its clip_id, when it names one ("" = none)
    std::vector<FdlCanvas> canvases;
};

struct FdlIntent {
    std::string id, label;
    int aspectW = 0, aspectH = 0;
    double protection = 0;
};

struct FdlTemplate {
    std::string id, label;
    int width = 0, height = 0;
    double squeeze = 1;
};

struct FdlDocument {
    std::string uuid, creator, defaultIntent;
    int versionMajor = 0, versionMinor = 0;
    std::vector<FdlIntent> intents;
    std::vector<FdlContext> contexts;
    std::vector<FdlTemplate> templates;
};

// Reads an FDL and checks it (the schema and the format's rules: ids unique and linked up, framing inside its canvas,
// and so on). False, with every problem found in `error` (one a line), when it is not one or breaks them.
bool parseFdl(const std::string& json, FdlDocument& out, std::string* error = nullptr);
bool readFdlFile(const std::string& path, std::string& json, FdlDocument& out, std::string* error = nullptr);

// The context of `doc` describing the media: the one whose clip_id names it (its clip name, or file name, as the
// media's name or file name with or without the extension); else, with `bySize`, one with a canvas the media's
// size or shape (a desqueezed or scaled-down copy); -1 for none.
int fdlContextFor(const FdlDocument& doc, const MediaItem& m, bool bySize);
// The canvas of a context the media shows: the one its size or shape matches, else the original (its own source).
int fdlCanvasFor(const FdlContext& ctx, const MediaItem& m);

// Attaches the FDL to the media it describes: each media item of `media` (all the project's footage when empty)
// with a context naming it, or (for those named in `media`) a canvas its size or shape, gets that context (the FDL cut
// down to it, with the document's framing intents and canvas templates). Returns how many were attached; 0, with
// `error` saying why, when the FDL could not be read or described none of them.
int attachFdl(Project& p, const std::string& json, const std::vector<Id>& media, std::string* error = nullptr);

// The media's FDL as attached (MediaItem::fdl), read; false when it has none (or it no longer reads).
bool mediaFdl(const MediaItem& m, FdlDocument& doc);
// Its canvas and the framing decision shown (MediaItem::fdlFraming, else the default intent's, else the first).
bool mediaFdlFraming(const MediaItem& m, FdlCanvas& canvas, FdlFraming& framing);

// A frame line: a framing decision (kind 0), its protection (1) or the canvas's effective area (2), as four corners
// (x, y pairs: top left, top right, bottom right, bottom left) in fractions of the picture it is drawn over.
struct FrameLine {
    int kind = 0;
    std::string label;
    std::array<double, 8> corners{};
};
// The media's frame lines, over its own picture.
std::vector<FrameLine> mediaFrameLines(const MediaItem& m);
// The frame lines of the top clip with an FDL showing at sequence frame t, through the clip's transform (and crop),
// over the sequence's picture.
std::vector<FrameLine> sequenceFrameLines(const Project& p, const Sequence& seq, FrameTime t);
// Draws them over `picture` (where the picture is drawn), `scale` times the usual line width (1 on a monitor).
void paintFrameLines(QPainter& painter, const QRectF& picture, const std::vector<FrameLine>& lines, double scale = 1);

// A canvas template applied to a source framing decision (the template pipeline of libfdl_core): the new canvas,
// where the source's canvas pixels land in it (out = (in - anchor) * scale + offset, per axis), and the FDL the
// pipeline writes (the source's context with the new canvas added).
struct FdlFit {
    int width = 0, height = 0;  // the new canvas
    double squeeze = 1;
    FdlRect framing;            // the source's framing decision in the new canvas
    bool protection = false;
    FdlRect protectionRect;
    double scaleX = 1, scaleY = 1;
    double anchorX = 0, anchorY = 0;
    double offsetX = 0, offsetY = 0;
    std::string json;
    std::string canvasId, framingId;
};
// The template `templateId` ("" = the first) of `templates` (an FDL with canvas templates) applied to the framing
// decision `framingId` ("" = as mediaFdlFraming picks) of the canvas `canvasId` ("" = the first) of the first context
// of `source` (or the context labelled `context`).
bool fitFdlTemplate(const std::string& source, const std::string& context, const std::string& canvasId, const std::string& framingId,
                    const std::string& templates, const std::string& templateId, FdlFit& out, std::string* error = nullptr);

// Frames the clips (video clips of footage with an FDL; ids, all of the sequence's when empty) by the template:
// each clip's transform set (fit None, its scale, position and crop) so its picture lands as the template's new
// canvas says, that canvas fitted into the sequence's frame and centred; with `resizeSequence`, the sequence first
// takes the new canvas's size (the first clip's). Returns the clips framed; `skipped` gets why the others were not.
std::vector<Id> frameClipsWithFdl(Project& p, Sequence& seq, const std::vector<Id>& clips, const std::string& templates,
                                  const std::string& templateId, bool resizeSequence, std::vector<std::string>* skipped = nullptr,
                                  std::string* error = nullptr);

// What an FDL's context names as its clip (clip_id): its name and a file, or numbered frames (`sequence` as
// "shot.####.exr", the #'s the frame number, frames first to last).
struct FdlClipId {
    std::string name, file, sequence;
    int first = 0, last = 0;
};
// An FDL for a clip's recording: its canvas (the media's FDL's context when it has one, else the media's size),
// `width` x `height` when given (a plate at another size: the framing scaled to it), with a framing decision for what
// the cut shows of it (the sequence's frame through the clip's transform and crop at its first frame), labelled with
// the sequence's name; its clip_id `clipId` (else the media's, or the media's name and file name). Empty, with
// `error`, when the clip has no picture.
std::string clipFdl(const Project& p, const Sequence& seq, const Clip& c, int width = 0, int height = 0, const FdlClipId* clipId = nullptr,
                    std::string* error = nullptr);
// One for the sequence: a context for each of the clips (ids; all its footage clips when empty), named by the clip
// and naming its media in its clip_id.
std::string sequenceFdl(const Project& p, const Sequence& seq, const std::vector<Id>& clips, std::string* error = nullptr);

}  // namespace montage
