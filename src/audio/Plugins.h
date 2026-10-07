// Montage — third-party audio plugins (CLAP, VST3, LV2, Audio Units).
//
// Scanning follows what DAWs and Resolve/Premiere do: plugins are found in
// the standard per-OS folders, and each new or changed plugin file is loaded
// in a separate process (montage-plugin-probe) so a plugin that crashes or
// hangs cannot take the editor down. Results are cached by path, size and
// modification time; plugins that fail to load go on a blocklist until the
// file changes or the user retries them.
//
// Hosting runs a plugin on stereo audio as a clip effect (type "plugin").
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
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
    // Clears internal audio state (delay lines, envelopes) after a seek.
    virtual void reset() {}
};

std::unique_ptr<Instance> instantiate(const Descriptor& d, std::string* error = nullptr);

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
    std::vector<std::string> searchPaths(Format f) const;

    // Scans every format's folders. Unchanged files come from the cache,
    // blocklisted files are skipped (unless rescanBlocked), the rest are probed.
    ScanReport scan(bool rescanBlocked = false,
                    const std::function<void(int done, int total, const std::string& path)>& progress = {});

    std::vector<Descriptor> plugins() const;
    std::optional<Descriptor> find(const std::string& id) const;
    std::vector<Blocked> blocklist() const;
    void unblock(const std::string& path);
    void clear();  // forgets the cache and the blocklist

private:
    struct Entry;
    std::string defaultCachePath() const;
    void loadCacheLocked() const;
    void saveCacheLocked() const;

    mutable std::mutex m_;
    std::string cachePath_;
    std::string probe_;
    int timeoutMs_ = 15000;
    std::vector<std::string> searchPaths_[4];
    bool customPaths_[4] = {false, false, false, false};
    mutable bool loaded_ = false;
    mutable std::vector<Entry> entries_;
};

}  // namespace montage::plugins
