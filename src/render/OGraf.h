// Montage — OGraf graphics (the EBU's open HTML graphics format, as Resolve 21 plays them): a graphic is a folder with
// a manifest (*.ograf.json: its name, its main JavaScript module, the data it takes as a JSON schema, whether it can
// be moved to any time) and the module, whose default export is a custom element that loads its data and plays,
// updates and stops. Montage renders them in a headless Chrome, Chromium or Edge that is already installed (or the one
// MONTAGE_CHROMIUM names), offline (only the graphic's own folder is served) and frame by frame: a graphic that can go
// to any time is told each frame's time; any other plays on a clock the renderer steps a frame at a time (its timers,
// animation frames, CSS transitions and animations, and Web Animations). The frames (with their alpha) are cached, so
// a graphic is rendered once for its data, size, rate and length.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "core/Model.h"
#include "media/Image.h"

namespace montage {

// One field of a graphic's data, from its schema's top-level properties.
struct OGrafField {
    std::string key, title, description;
    std::string type;  // "string", "number", "integer", "boolean", or "json" (anything else: an object, an array)
    std::vector<std::string> choices;  // a string's enum
    bool multiline = false;            // a string the schema marks as several lines (gddType "multi-line")
};

struct OGrafGraphic {
    std::string manifest;  // its path
    std::string id, name, description, version, main;
    bool realTime = true, nonRealTime = false;
    int stepCount = 1;
    std::string schema;  // the data's JSON schema, as JSON ("{}" none)
    std::vector<OGrafField> fields;
    std::vector<std::pair<std::string, std::string>> customActions;  // (id, name)
};

bool isOGrafManifest(const std::string& path);  // a *.ograf.json file
// Reads a graphic's manifest. False with `error` when it is not one (no main module, not JSON).
bool readOGraf(const std::string& manifest, OGrafGraphic& out, std::string* error = nullptr);
// The data a graphic starts with: its schema's defaults, as a JSON object.
std::string ografDefaultData(const OGrafGraphic& g);

// The browser graphics are rendered in: MONTAGE_CHROMIUM when set, else Chrome, Chromium or Edge where they install
// themselves (or on the PATH); "" when there is none.
std::string ografBrowser();

// An action at a time (seconds from the graphic's start): playAction, stopAction, updateAction or customAction, its
// parameters a JSON object.
struct OGrafAction {
    double at = 0;
    std::string type;
    std::string params = "{}";
};
struct OGrafJob {
    std::string manifest;
    std::string data = "{}";  // JSON
    int width = 1920, height = 1080;
    double fps = 25;
    int frames = 0;
    std::vector<OGrafAction> actions;
};
using OGrafProgress = std::function<void(double fraction)>;
// Renders frames 0 to job.frames - 1 into `folder` as frame_00000.png... (RGBA, straight alpha). False with `error`
// when there is no browser, the graphic fails to load (its message), or `cancel` is set.
bool renderOGraf(const OGrafJob& job, const std::string& folder, const OGrafProgress& progress = {},
                 const std::atomic<bool>* cancel = nullptr, std::string* error = nullptr);

// ---- On the timeline: the "ograf" generator ---------------------------------------------------------------------
// Its strings: "manifest" (the graphic), "data" (JSON), "actions" (JSON, [{"at": seconds, "type": ..., "params":
// {...}}], replacing the usual ones). Usually: playAction at the start, the next step every "advance" seconds when it
// has steps, and stopAction "out" seconds before the end.
OGrafJob ografClipJob(const Effect& g, int width, int height, double fps, FrameTime duration);
// Where the clip's frames are cached ("" when it has no graphic), and whether they all are.
std::string ografCacheFolder(const OGrafJob& job);
bool ografCached(const OGrafJob& job);
// Renders and caches them unless they are (one render at a time for the same job; others wait for it).
bool prepareOGraf(const OGrafJob& job, const OGrafProgress& progress = {}, const std::atomic<bool>* cancel = nullptr,
                  std::string* error = nullptr);
// Why the job's last render failed ("" when it has not): renders that fail are tried again after two minutes, or now
// by prepareOGraf. The cache is kept to MONTAGE_OGRAF_CACHE_MB (2048 by default), dropping the oldest renders.
std::string ografFailure(const OGrafJob& job);
// The generator's picture at clip frame t, w x h: rendered at the sequence's size (first if need be) and scaled.
// Transparent when it cannot be.
Image ografFrame(const Effect& g, FrameTime t, int w, int h, int sequenceWidth, int sequenceHeight, FrameTime duration, double fps);

}  // namespace montage
