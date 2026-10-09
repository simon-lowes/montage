// Montage — OpenFX video plugins (as DaVinci Resolve, Vegas, Nuke, Fusion, Natron and, since 2026, Kdenlive and
// Shotcut host them): Neat Video, Boris FX Sapphire and Continuum, RE:Vision, Dehancer, FilmConvert and the rest of the
// cross-vendor catalogue run as clip effects.
//
// Plugins are found in the standard folders (OFX_PLUGIN_PATH first) as *.ofx.bundle folders, and each new or changed
// binary is described in montage-plugin-probe, a separate process, so one that crashes or hangs while loading cannot
// take the editor down; descriptions are cached by path, size and time, and failures are blocklisted until the file
// changes. A plugin runs in the Filter context (one Source clip, one Output) on premultiplied RGBA float frames (or
// 8-bit, when that is all it takes), with the host suites a filter needs: properties, image effect, parameters,
// memory, multithreading, messages, progress and timeline. Its parameters become the clip effect's (type "ofx"),
// keyframed like any other, and the plugin reads them at any time it asks for.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/Model.h"
#include "media/Image.h"

namespace montage::ofx {

struct ParamDesc {
    std::string name, label, type;  // type: the OFX parameter type ("OfxParamTypeDouble"...)
    std::vector<double> def;        // per dimension (colours: r, g, b[, a])
    double min = -1e9, max = 1e9, displayMin = -1e9, displayMax = 1e9;
    std::vector<std::string> choices;
    std::string stringDefault;
    std::string doubleType;  // "OfxParamDoubleTypeAngle"...
    bool animates = true, secret = false;
    int dimensions() const;  // the numbers it holds
    bool numeric() const;
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
    std::vector<ParamDesc> params;
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
    void setCachePath(const std::string& path);
    void setProbeExecutable(const std::string& path);  // default: montage-plugin-probe beside the program
    void setSearchPaths(const std::vector<std::string>& dirs);  // empty: the defaults
    // Finds and describes the plugins (new or changed binaries in the probe process). How many there are.
    int scan(std::vector<std::string>* log = nullptr);
    std::vector<PluginDesc> plugins() const;
    std::vector<std::pair<std::string, std::string>> blocked() const;  // binary, why
    bool find(const std::string& id, PluginDesc& out) const;

private:
    Registry();
    mutable std::mutex mutex_;
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
// False (with `error`) when the plugin is missing or fails; `img` is then left as it was.
using FrameFetch = std::function<bool(double t, Image& out)>;
bool applyEffect(const Effect& e, double t, Image& img, double scale, const FrameFetch& fetch = {}, std::string* error = nullptr);

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

// Binaries loaded and instances made in this process (for crash reports and tests).
int instancesCreated();

}  // namespace montage::ofx
