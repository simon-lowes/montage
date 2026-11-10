// Montage — OpenFX video plugins (as DaVinci Resolve, Vegas, Nuke, Fusion, Natron and, since 2026, Kdenlive and
// Shotcut host them): Neat Video, Boris FX Sapphire and Continuum, RE:Vision, Dehancer, FilmConvert and the rest of the
// cross-vendor catalogue run as clip effects.
//
// Plugins are found in the standard folders (OFX_PLUGIN_PATH first) as *.ofx.bundle folders, and each new or changed
// binary is described in montage-plugin-probe, a separate process, so one that crashes or hangs while loading cannot
// take the editor down; descriptions are cached by path, size and time, and failures are blocklisted until the file
// changes. A plugin runs in one of four contexts, the first it takes of: Filter (one Source clip, one Output) or
// General (a Source and further inputs: mattes and second pictures, each connected to a video track, a bin item or the
// effect's own mask) as a clip effect; Transition (SourceFrom and SourceTo, its Transition parameter driven by the
// transition's progress) among the transitions; Generator (an Output alone) as a generator clip. Pictures are
// premultiplied RGBA float (or 8-bit, when that is all it takes), with the host suites: properties, image effect,
// parameters, memory, multithreading, messages, progress, timeline, interact and draw (on-screen controls) and OpenGL
// render. Its parameters become the effect's (type "ofx"), keyframed like any other, and the plugin reads them at any
// time it asks for.
//
// On-screen controls: a plugin's overlay interact draws over the Program monitor, through the OpenFX 1.5 Draw Suite
// (its V2 overlay; painted with QPainter over whatever draws the picture) or OpenGL (V1; drawn in an offscreen OpenGL
// context and laid over the picture). Pen and key events reach it, and the parameters it sets while it holds the pen
// are written keyframe-aware into a copy of the effect, which the editor makes one undo step. A plugin that crashes
// while its controls run is blocked, as one that crashes on loading is, until its binary changes.
//
// Isolation: with plug-ins in host processes (audio/PluginHost.h), each render goes to a montage-pluginhost for the
// plug-in (render/OfxRemote.h): its pictures through shared memory, and frames at other times and inputs asked back
// from Montage while it renders. One that crashes or does not answer is bypassed (the picture as it came) and named in
// the failure list; it is started again for the next render, a set number of times. An OpenGL render runs in the
// host's own OpenGL context and its pictures come back through shared memory too. On-screen controls run in the host
// as well: it draws them (Draw Suite and OpenGL alike, OpenGL in an offscreen context of its own) at the monitor's
// size and pixel ratio into one premultiplied picture in the instance's shared block, which Montage lays over the
// picture; pen, key and focus events go there with the same view, and the parameters a control sets come back. A
// control that misses its deadline keeps its last picture up and is marked as not responding; one that crashes its
// host, or does not answer for longer, is closed and its plug-in bypassed and named as for a render. No plug-in code
// runs in Montage's process.
//
// GPU: a plugin that renders with OpenGL (kOfxImageEffectPropOpenGLRenderSupported) does so in Montage's own OpenGL
// context, on a thread of its own, texture in and texture out; the pictures go up and come back. Without OpenGL, or
// when it fails, the CPU render is used (a plugin that needs OpenGL then fails with why). CUDA, OpenCL and Metal
// rendering are not offered.
#pragma once

#include <QImage>
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/Model.h"
#include "media/Image.h"

class QPainter;

namespace montage {
struct ColorSpace;
}

namespace montage::ofx {

struct ParamDesc {
    std::string name, label, type;  // type: the OFX parameter type ("OfxParamTypeDouble"...)
    std::vector<double> def;        // per dimension (colours: r, g, b[, a])
    double min = -1e9, max = 1e9, displayMin = -1e9, displayMax = 1e9;
    std::vector<std::string> choices;
    std::string stringDefault;
    std::string doubleType;  // "OfxParamDoubleTypeAngle"...
    bool normalised = false;  // a position or size kept as a fraction of the frame (the plugin's default coordinates)
    bool animates = true, secret = false;
    int dimensions() const;  // the numbers it holds
    bool numeric() const;
};

// An input clip of a plugin in the General context beyond its Source (a matte, a second picture).
struct InputDesc {
    std::string name, label;
    bool mask = false;      // kOfxImageClipPropIsMask
    bool optional = false;  // kOfxImageClipPropOptional
    bool operator==(const InputDesc&) const = default;
};

struct PluginDesc {
    std::string id;          // "ofx:" + identifier + "/" + major version
    std::string identifier;  // the plug-in's own
    int versionMajor = 1, versionMinor = 0;
    std::string label, group, description;
    std::string binary;   // the .ofx file
    std::string bundle;   // the .ofx.bundle folder
    int index = 0;        // in the binary
    bool temporal = false;  // asks for frames other than the one rendered
    bool floatImages = true;  // takes 32-bit float images (else 8-bit)
    std::string colour;       // the colour management it takes (OpenFX 1.5: Basic, Core; empty for none)
    std::string context = "OfxImageEffectContextFilter";  // the context Montage runs it in (kOfxImageEffectContext*)
    std::vector<InputDesc> inputs;  // General: its inputs other than Source
    std::string openGL = "false";   // kOfxImageEffectPropOpenGLRenderSupported: "false", "true" or "needed"
    int overlay = 0;                // its on-screen controls: 0 none, 1 OpenGL (V1), 2 the Draw Suite (V2)
    std::vector<ParamDesc> params;
    bool transition() const;  // a transition (the Transition context)
    bool generator() const;   // a generator (the Generator context)
    bool operator==(const PluginDesc&) const = default;
};

// OFX_PLUGIN_PATH, then the platform's standard folder (/usr/OFX/Plugins, /Library/OFX/Plugins,
// C:\Program Files\Common Files\OFX\Plugins).
std::vector<std::string> defaultSearchPaths();
// The binaries for this platform inside the *.ofx.bundle folders under `dirs`.
std::vector<std::string> findBinaries(const std::vector<std::string>& dirs);
// Loads `binary` in this process and describes its image effects that work as filters (montage-plugin-probe does this
// out of process). A broken plugin may crash the caller.
std::vector<PluginDesc> describeBinary(const std::string& binary, std::string* error = nullptr);
std::string descriptionsToJson(const std::vector<PluginDesc>& ds);
std::vector<PluginDesc> descriptionsFromJson(const std::string& json);

class Registry {
public:
    static Registry& instance();
    void setCachePath(const std::string& path);  // (the crash guard for on-screen controls sits beside it)
    void setProbeExecutable(const std::string& path);  // default: montage-plugin-probe beside the program
    void setSearchPaths(const std::vector<std::string>& dirs);  // empty: the defaults
    // Finds and describes the plugins (new or changed binaries in the probe process). How many there are.
    int scan(std::vector<std::string>* log = nullptr);
    void ensureScanned();  // scans if nothing has yet (renders outside the app, before its startup scan ends)
    std::vector<PluginDesc> plugins() const;
    std::vector<std::pair<std::string, std::string>> blocked() const;  // binary, why
    bool find(const std::string& id, PluginDesc& out) const;
    // The file naming the plug-in whose on-screen controls are running (the next scan blocks it if Montage stopped then).
    std::string guardPath() const;
    // Takes a plug-in Montage's own scan found (in montage-pluginhost, which scans nothing itself).
    void adopt(const PluginDesc& d);
    // Blocks a binary that crashed its plug-in host while loading, as the scan would, until the file changes.
    void blockAfterCrash(const std::string& binary, const std::string& reason);
    // The plug-in an effect runs, if this computer's scan found it and did not block it (else false, with why).
    bool lookUp(const Effect& e, PluginDesc& d, std::string* error = nullptr);

private:
    Registry();
    mutable std::mutex mutex_;
    std::mutex scanMutex_;
    bool scanned_ = false;
    std::string cachePath_, probe_;
    std::vector<std::string> paths_;
    std::vector<PluginDesc> plugins_;
    std::vector<std::pair<std::string, std::string>> blocked_;
};

// Effect type strings: "ofx:<plugin id>" in browsers and drag and drop; the clip effect itself is type "ofx".
constexpr const char* kTypePrefix = "ofx:";
bool isOfxType(const std::string& type);
// A clip effect running `d`: its identity and one parameter per plugin parameter, at the plugin's defaults.
Effect makeEffect(Project& p, const PluginDesc& d);
std::string effectName(const Effect& e);

// Runs the effect's plugin on `img` (premultiplied RGBA float, rendered at `scale` of full size) at clip frame `t`.
// `fetch` gives the source at another clip frame for plugins that ask (false: none; the current frame is given).
// Runs only plug-ins the registry found and did not block (never a binary named in the project file). False (with
// `error`) when the plugin is missing, blocked or fails; `img` is then left as it was.
// A plugin that takes OpenFX 1.5 colour management is told the colourspace of its pictures (the `working` space, or
// one of the colourspaces it prefers, converted into and out of), and Montage takes its output's colourspace into account.
using FrameFetch = std::function<bool(double t, Image& out)>;
bool applyEffect(const Effect& e, double t, Image& img, double scale, const FrameFetch& fetch = {}, std::string* error = nullptr,
                 const ColorSpace* working = nullptr);
// A transition plugin's picture at `progress` (0 at the transition's first frame, 1 past its last) from `from` to `to`
// (premultiplied, the same size; either may be empty: transparent), at transition frame `t`. False (with `error`) as
// applyEffect; `out` is then untouched.
bool applyTransition(const Effect& e, double t, double progress, const Image& from, const Image& to, Image& out, double scale,
                     std::string* error = nullptr, const ColorSpace* working = nullptr);
// A generator plugin's picture at clip frame `t`, `w` x `h` at `scale` of full size.
bool renderGenerator(const Effect& e, double t, int w, int h, double scale, Image& out, std::string* error = nullptr,
                     const ColorSpace* working = nullptr);

// A General plugin's inputs: the effect string "input.<clip name>" says what each is connected to: "" (nothing),
// "track:<id>" (what that video track shows), "media:<id>" (a bin item, placed as a new clip would be) or "mask" (the
// effect's own mask, as its alpha; the effect's result is then not cut by the mask again).
std::string inputKey(const std::string& clip);
bool usesOwnMask(const Effect& e);
// The pictures connected inputs show at clip frame `t`, sized as the compositor makes them (they are fitted to the
// source): set by the compositor around a clip's effects.
using InputResolver = std::function<bool(const std::string& connection, double t, Image& out)>;
class InputScope {
public:
    explicit InputScope(InputResolver resolve);
    ~InputScope();
    InputScope(const InputScope&) = delete;
    InputScope& operator=(const InputScope&) = delete;

private:
    InputResolver resolve_;
    const InputResolver* previous_;
};
const InputResolver& currentInputs();
// The mask an OwnMaskScope set on this thread (null: none).
const std::vector<float>* currentOwnMask();
// The effect's own mask (one value a pixel, the size of the picture it runs on) for an input connected to "mask".
class OwnMaskScope {
public:
    explicit OwnMaskScope(const std::vector<float>* matte);
    ~OwnMaskScope();
    OwnMaskScope(const OwnMaskScope&) = delete;
    OwnMaskScope& operator=(const OwnMaskScope&) = delete;

private:
    const std::vector<float>* previous_;
};

// ---- GPU (the OpenGL render suite) ----
// Makes the offscreen surface OpenGL renders and controls use (the GUI thread, once; the app does at startup). False,
// with why, where there is no OpenGL; plugins then render on the CPU.
bool initializeGpu(std::string* why = nullptr);
bool gpuAvailable(std::string* why = nullptr);
void setGpuRendering(bool on);  // use OpenGL for plugins that take it (on by default once initialised)
bool gpuRendering();
int gpuRenders();  // renders made with OpenGL in this process (for tests)

// ---- On-screen controls (overlay interacts) ----
// What a control drew: Draw Suite primitives, in canonical coordinates (the effect's full-size frame, y up), and what
// it drew with OpenGL, as a premultiplied picture the size of the view.
struct DrawOp {
    enum Kind { Lines, LineStrip, LineLoop, Rectangle, Polygon, Ellipse, Text } kind = Lines;
    std::array<float, 4> colour{1, 1, 1, 1};
    float width = 0;  // 0: one pixel
    int stipple = 0;  // OfxDrawLineStipplePattern
    std::vector<std::array<double, 2>> points;
    std::string text;
    int align = 0;  // OfxDrawTextAlignment
};
struct Overlay {
    std::vector<DrawOp> ops;
    QImage gl;  // (null when it drew nothing with OpenGL)
};
// Where the effect's frame is on screen: canonical (x, y) shows at (m[0] x + m[1] y + m[2], m[3] x + m[4] y + m[5])
// in a view `width` x `height` pixels (y down).
struct View {
    int width = 0, height = 0;
    double m[6] = {1, 0, 0, 0, -1, 0};
    double renderScale = 1;       // of the pictures the monitor shows
    double frameW = 0, frameH = 0;  // the effect's full-size frame (canonical extent); 0: the project's
    double pixelRatio = 1;          // the monitor's device pixels to one of the view's (a host draws that finely)
    double pixelScale() const;      // canonical pixels a screen pixel
    std::array<double, 2> toView(double x, double y) const;
    std::array<double, 2> toCanonical(double vx, double vy) const;
};
// Draws `o` with `p` (a QPainter on the view).
void paintOverlay(QPainter& p, const Overlay& o, const View& v);

namespace remote {
class Controls;
}

// A plugin effect's on-screen control: an instance of its own (interactive) and its overlay interact, in this process
// or, with plug-ins isolated, in the plug-in's host. Calls come from the GUI thread.
class Interact {
public:
    // Null (with `error` when something failed) when the effect's plugin has no overlay.
    static std::unique_ptr<Interact> open(const Effect& e, std::string* error = nullptr);
    ~Interact();
    Interact(const Interact&) = delete;
    Interact& operator=(const Interact&) = delete;
    // What it draws at clip frame `t`. False when it failed (it is then closed).
    bool draw(const Effect& e, double t, const View& v, Overlay& out);
    enum class Pen { Down, Move, Up };
    // A pen event at view point (vx, vy). True when the plugin took it; `edited` (a copy of `e`) then holds the
    // parameters it set, keyed at `t` where they are animated, and `changed` says whether it set any.
    bool pen(Pen kind, const Effect& e, double t, const View& v, double vx, double vy, double pressure, Effect& edited, bool& changed);
    // A key (an OfxKeySyms code, and its text) pressed or let go.
    bool key(bool down, int keysym, const std::string& text, const Effect& e, double t, const View& v, Effect& edited, bool& changed);
    // The view gained or lost the keyboard's focus.
    void focus(bool gained, const Effect& e, double t, const View& v);
    bool failed() const;
    // (in a host) Its last reply missed the deadline: its last picture stays up, events are not sent until it answers.
    bool unresponsive() const;
    // (in a host) A message is still out: its answer is taken at a later call (a poll repaints for it).
    bool waiting() const;
    // (in a host) What the control set in a reply that came after its deadline and that no pen or key event has handed
    // on: only those parameters and strings. False when there is none.
    bool lateEdits(Effect& changes);
    // In a plug-in host (not this process).
    bool remote() const { return remote_ != nullptr; }
    // Why it failed, when it failed in a host ("crashed (signal 11)"...); "" otherwise.
    std::string failure() const;
    struct Impl;

private:
    explicit Interact(std::unique_ptr<Impl> impl);
    explicit Interact(std::unique_ptr<remote::Controls> remote);
    std::unique_ptr<Impl> impl_;
    std::unique_ptr<remote::Controls> remote_;
    bool failed_ = false;
};

// Called around each plug-in binary's loading, with the plug-in's id (montage-pluginhost notes it for blame).
void setLoadWatcher(std::function<void(const std::string& id, bool loading)> watch);

// The frames of the clip being rendered on this thread, for plugins that ask for other times: set by the compositor
// around a clip's effects.
class FetchScope {
public:
    explicit FetchScope(FrameFetch fetch);
    ~FetchScope();
    FetchScope(const FetchScope&) = delete;
    FetchScope& operator=(const FetchScope&) = delete;

private:
    FrameFetch previous_;
};
const FrameFetch& currentFetch();

// Instances made in this process, and those not yet destroyed (for crash reports and tests). Each clip effect has its
// own; free ones past a small limit are destroyed, least recently used first.
int instancesCreated();
int instancesAlive();

}  // namespace montage::ofx
