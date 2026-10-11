// Montage — third-party audio plugins (CLAP, VST3, LV2, Audio Units).
//
// Scanning follows what DAWs and Resolve/Premiere do: plugins are found in
// the standard per-OS folders, and each new or changed plugin file is loaded
// in a separate process (montage-plugin-probe) so a plugin that crashes or
// hangs cannot take the editor down. Results are cached by path, size and
// modification time; plugins that fail to load go on a blocklist until the
// file changes or the user retries them.
//
// Hosting runs a plugin on stereo audio as a clip effect (type "plugin"), with another track as its key input when
// it has one (a sidechain).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace montage::plugins {

enum class Format { Clap, Vst3, Lv2, AudioUnit };
constexpr Format kAllFormats[] = {Format::Clap, Format::Vst3, Format::Lv2, Format::AudioUnit};

const char* formatName(Format f);  // "CLAP", "VST3", "LV2", "AU"
std::optional<Format> formatFromName(std::string_view name);
// Whether this build can run (not only list) plugins of the format.
bool canHost(Format f);

struct Descriptor {
    Format format = Format::Clap;
    std::string id;        // unique across formats, e.g. "clap:com.vendor.gain"
    std::string pluginId;  // the format's own identifier (CLAP id, VST3 CID, LV2 URI, AU type:subtype:manufacturer)
    std::string name;
    std::string vendor;
    std::string version;
    std::string category;  // e.g. "EQ", "Dynamics", "Reverb"
    std::string path;      // plugin file or bundle
    bool instrument = false;
    bool operator==(const Descriptor&) const = default;
};

struct ParamInfo {
    uint32_t id = 0;
    std::string name;
    double min = 0, max = 1, def = 0;
    bool automatable = true;
    bool stepped = false;
};

// What the plugin's own editor reports back to the host (main thread).
class EditorListener {
public:
    virtual ~EditorListener() = default;
    // The user changed a parameter in the editor (plain value, as parameters() ranges).
    virtual void editorParameter(uint32_t id, double value) = 0;
    // A drag on a control began or ended (changes in between belong together).
    virtual void editorGesture(uint32_t id, bool begin) {}
    // The editor wants a new size (pixels).
    virtual void editorResize(int width, int height) = 0;
};

// A loaded, running plugin. Not thread safe: use from one thread at a time.
class Instance {
public:
    virtual ~Instance() = default;
    virtual bool activate(double sampleRate, int maxFrames) = 0;
    // Processes `frames` samples in place on non-interleaved channels.
    virtual void process(float* const* channels, int numChannels, int frames) = 0;
    virtual std::vector<ParamInfo> parameters() = 0;
    virtual double parameter(uint32_t id) = 0;
    // Takes effect from the next process() call.
    virtual void setParameter(uint32_t id, double value) = 0;
    virtual std::string saveState() = 0;
    virtual bool loadState(const std::string& state) = 0;
    virtual int latencySamples() { return 0; }
    // A key input (sidechain), as compressors, gates and duckers have: CLAP's second audio input port, a VST3
    // auxiliary input bus, LV2 audio inputs marked lv2:isSideChain. setSidechain() gives the key for the next
    // process() call (`channels` buffers of at least its frames, valid until it returns); null is silence, which a
    // plugin with a key input hears when none is chosen.
    virtual bool hasSidechain() { return false; }
    virtual void setSidechain(const float* const* channels, int numChannels) {}
    // Whether a key is chosen at all. A VST3 plugin's auxiliary bus is switched on only then (DAWs leave it off until
    // a key is chosen, and plugins that follow it would otherwise listen to silence); the others ignore it.
    virtual void enableSidechain(bool on) {}
    // Clears internal audio state (delay lines, envelopes) after a seek.
    virtual void reset() {}

    // The plugin's own editor window, embedded in a native window of this
    // platform: an NSView* on macOS, an HWND on Windows, an X11 window id on
    // Linux. Main thread only; keep calling idle() while it is open.
    virtual bool hasEditor() { return false; }
    virtual bool openEditor(void* parent, EditorListener* listener, int& width, int& height) { return false; }
    virtual void closeEditor() {}
    virtual bool editorResizable() { return false; }
    virtual void setEditorSize(int width, int height) {}
    virtual void idle() {}
};

std::unique_ptr<Instance> instantiate(const Descriptor& d, std::string* error = nullptr);
// Whether a plugin (by descriptor id) has turned out to have a key input, once any instance of it was made here.
bool knownSidechain(const std::string& id);
// Plugins loaded by this process so far (crash reports note whether plugins were in use).
int instancesCreated();

// Standard folders for a format on this OS, after the format's environment
// variable (CLAP_PATH, VST3_PATH, LV2_PATH) if set.
std::vector<std::string> defaultSearchPaths(Format f);
// Plugin files or bundles of the format found under `dirs` (recursively).
std::vector<std::string> findPluginFiles(Format f, const std::vector<std::string>& dirs);

// Loads the plugin file in this process and lists the plugins inside it.
// Used by montage-plugin-probe; a broken plugin may crash the caller.
std::vector<Descriptor> probeInProcess(Format f, const std::string& path, std::string* error = nullptr);
// Descriptors readable without loading any code (VST3 moduleinfo.json, LV2
// manifests); nullopt when the format or file needs a probe.
std::optional<std::vector<Descriptor>> readStaticMetadata(Format f, const std::string& path);

std::string descriptorsToJson(const std::vector<Descriptor>& ds);
std::vector<Descriptor> descriptorsFromJson(const std::string& json, std::string* error = nullptr);

struct Blocked {
    Format format = Format::Clap;
    std::string path;
    std::string reason;
};

struct ScanReport {
    int files = 0;      // plugin files found
    int fromCache = 0;  // unchanged files answered from the cache
    int probed = 0;     // files loaded in the probe process
    std::vector<Blocked> newlyBlocked;
    std::vector<std::string> log;  // one line per file: cached / read / probed / blocked / listed
};

class Registry {
public:
    static Registry& instance();
    Registry();
    ~Registry();

    // Where the scan cache lives (default: the user's app data folder).
    void setCachePath(const std::string& path);
    std::string cachePath() const;
    // The probe helper (default: montage-plugin-probe next to the running program).
    void setProbeExecutable(const std::string& path);
    void setProbeTimeoutMs(int ms);
    // Folders scanned for a format; empty restores the defaults.
    void setSearchPaths(Format f, std::vector<std::string> dirs);
    // Folders the user added, scanned after the defaults.
    void setExtraSearchPaths(Format f, std::vector<std::string> dirs);
    std::vector<std::string> extraSearchPaths(Format f) const;
    std::vector<std::string> searchPaths(Format f) const;

    // Scans every format's folders. Unchanged files come from the cache,
    // blocklisted files are skipped (unless rescanBlocked), the rest are probed.
    ScanReport scan(bool rescanBlocked = false,
                    const std::function<void(int done, int total, const std::string& path)>& progress = {});
    // Like scan(), but loads these files again even if they are unchanged or blocked.
    ScanReport rescan(const std::vector<std::string>& paths,
                      const std::function<void(int done, int total, const std::string& path)>& progress = {});

    // Safe mode: while disabled, no plugin is listed or found (effects pass audio through).
    void setEnabled(bool on);
    bool enabled() const;

    std::vector<Descriptor> plugins() const;
    std::optional<Descriptor> find(const std::string& id) const;
    std::vector<Blocked> blocklist() const;
    void unblock(const std::string& path);
    // Disabled plugins stay installed and keep working in existing projects,
    // but are not offered in effect lists. Saved with the cache.
    void setPluginDisabled(const std::string& id, bool disabled);
    bool isPluginDisabled(const std::string& id) const;
    void clear();  // forgets the cache and the blocklist

private:
    struct Entry;
    ScanReport scanImpl(bool rescanBlocked, const std::set<std::string>& force,
                        const std::function<void(int, int, const std::string&)>& progress);
    std::string defaultCachePath() const;
    void loadCacheLocked() const;
    void saveCacheLocked() const;

    mutable std::mutex m_;
    std::string cachePath_;
    std::string probe_;
    int timeoutMs_ = 15000;
    std::vector<std::string> searchPaths_[4];
    std::vector<std::string> extraPaths_[4];
    mutable std::set<std::string> disabled_;
    bool customPaths_[4] = {false, false, false, false};
    mutable bool loaded_ = false;
    bool enabled_ = true;
    mutable std::vector<Entry> entries_;
};

}  // namespace montage::plugins
