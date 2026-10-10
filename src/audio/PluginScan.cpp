// Plugin discovery, metadata and the scan cache / blocklist.
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <map>
#include <set>
#include <thread>
#include <utility>

#include "Plugins.h"

#ifdef __APPLE__
#include <AudioToolbox/AudioToolbox.h>
#endif

namespace montage::plugins {

// ---------------------------------------------------------------------------
// Formats

const char* formatName(Format f) {
    switch (f) {
        case Format::Clap: return "CLAP";
        case Format::Vst3: return "VST3";
        case Format::Lv2: return "LV2";
        case Format::AudioUnit: return "AU";
    }
    return "?";
}

std::optional<Format> formatFromName(std::string_view name) {
    for (Format f : kAllFormats)
        if (name == formatName(f)) return f;
    return std::nullopt;
}

bool canHost(Format f) {
#ifdef MONTAGE_WITH_VST3
    if (f == Format::Vst3) return true;
#endif
#ifdef __APPLE__
    if (f == Format::AudioUnit) return true;
#endif
#ifdef MONTAGE_WITH_LV2
    if (f == Format::Lv2) return true;
#endif
    return f == Format::Clap;
}

namespace {

QString q(const std::string& s) { return QString::fromStdString(s); }

const char* idPrefix(Format f) {
    switch (f) {
        case Format::Clap: return "clap:";
        case Format::Vst3: return "vst3:";
        case Format::Lv2: return "lv2:";
        case Format::AudioUnit: return "au:";
    }
    return "";
}

QStringList envPaths(const char* var) {
    const QByteArray v = qgetenv(var);
    if (v.isEmpty()) return {};
#ifdef _WIN32
    const char sep = ';';
#else
    const char sep = ':';
#endif
    return QString::fromLocal8Bit(v).split(QLatin1Char(sep), Qt::SkipEmptyParts);
}

[[maybe_unused]] QString envDir(const char* var, const QString& rest) {
    const QByteArray v = qgetenv(var);
    return v.isEmpty() ? QString() : QDir::cleanPath(QString::fromLocal8Bit(v) + "/" + rest);
}

// Size + newest modification time of a file, or of the files in a bundle.
std::string signature(const std::string& path) {
    QFileInfo fi(q(path));
    if (!fi.exists()) return {};
    qint64 size = 0, newest = fi.lastModified().toMSecsSinceEpoch();
    if (fi.isDir()) {
        QDirIterator it(fi.absoluteFilePath(), QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            size += it.fileInfo().size();
            newest = std::max(newest, it.fileInfo().lastModified().toMSecsSinceEpoch());
        }
    } else {
        size = fi.size();
    }
    return std::to_string(size) + ":" + std::to_string(newest);
}

std::string categoryFromWords(const QString& text) {
    static const std::pair<const char*, const char*> kMap[] = {
        {"eq", "EQ"},           {"equalizer", "EQ"},  {"filter", "Filter"},       {"compressor", "Dynamics"},
        {"dynamics", "Dynamics"}, {"limiter", "Dynamics"}, {"gate", "Dynamics"},   {"reverb", "Reverb"},
        {"delay", "Delay"},     {"distortion", "Distortion"}, {"modulation", "Modulation"}, {"chorus", "Modulation"},
        {"pitch", "Pitch"},     {"restoration", "Restoration"}, {"analyzer", "Analyzer"}, {"spatial", "Spatial"},
        {"utility", "Utility"}, {"tools", "Utility"}, {"mastering", "Mastering"},
    };
    const QString lower = text.toLower();
    for (const auto& [word, cat] : kMap)
        if (lower.contains(QRegularExpression(QStringLiteral("\\b%1").arg(word)))) return cat;
    return {};
}

// ---- VST3: Contents/Resources/moduleinfo.json (VST 3.7.5+) ----------------

std::optional<std::vector<Descriptor>> readVst3ModuleInfo(const std::string& path) {
    QFile f(q(path) + "/Contents/Resources/moduleinfo.json");
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
    QString text = QString::fromUtf8(f.readAll());
    // moduleinfo.json is JSON5-flavoured: drop comments and trailing commas.
    text.remove(QRegularExpression(QStringLiteral("//[^\\n]*")));
    text.replace(QRegularExpression(QStringLiteral(",\\s*([\\]}])")), QStringLiteral("\\1"));
    QJsonParseError err{};
    QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return std::nullopt;
    const QJsonObject root = doc.object();
    const QString factoryVendor = root.value("Factory Info").toObject().value("Vendor").toString();
    std::vector<Descriptor> out;
    for (const QJsonValue& v : root.value("Classes").toArray()) {
        const QJsonObject c = v.toObject();
        if (c.value("Category").toString() != "Audio Module Class") continue;
        Descriptor d;
        d.format = Format::Vst3;
        d.pluginId = c.value("CID").toString().toStdString();
        d.id = idPrefix(Format::Vst3) + d.pluginId;
        d.name = c.value("Name").toString().toStdString();
        d.vendor = c.value("Vendor").toString(factoryVendor).toStdString();
        d.version = c.value("Version").toString().toStdString();
        QStringList subs;
        for (const QJsonValue& s : c.value("Sub Categories").toArray()) subs << s.toString();
        d.instrument = subs.contains("Instrument");
        d.category = categoryFromWords(subs.join(' '));
        d.path = path;
        out.push_back(d);
    }
    return out;
}

// ---- LV2: manifest.ttl names the plugins, their .ttl files carry the details ---

std::optional<std::vector<Descriptor>> readLv2Bundle(const std::string& path) {
    const QString bundle = q(path);
    QFile mf(bundle + "/manifest.ttl");
    if (!mf.open(QIODevice::ReadOnly)) return std::nullopt;
    const QString manifest = QString::fromUtf8(mf.readAll());
    // Statements "<uri> a ... ." (a Turtle statement ends at a dot followed by
    // whitespace, so file names like comp.so stay inside it).
    static const QRegularExpression subjectRe(QStringLiteral("<([^>]+)>\\s+a\\s+((?:[^.]|\\.(?=\\S))*)\\.(?=\\s|$)"));
    static const QRegularExpression seeAlsoRe(QStringLiteral("rdfs:seeAlso\\s+<([^>]+)>"));
    static const QRegularExpression nameRe(QStringLiteral("doap:name\\s+\"([^\"]+)\""));
    static const QRegularExpression typeRe(QStringLiteral("\\ba\\s+([^;]*lv2:\\w*Plugin[^;]*);"));
    std::vector<Descriptor> out;
    auto it = subjectRe.globalMatch(manifest);
    while (it.hasNext()) {
        auto m = it.next();
        if (!m.captured(2).contains(QLatin1String("lv2:Plugin")) && !m.captured(2).contains(QLatin1String("lv2core#Plugin")))
            continue;
        Descriptor d;
        d.format = Format::Lv2;
        d.pluginId = m.captured(1).toStdString();
        d.id = idPrefix(Format::Lv2) + d.pluginId;
        d.path = path;
        QString details = m.captured(2);
        auto see = seeAlsoRe.match(details);
        if (see.hasMatch()) {
            QFile pf(bundle + "/" + see.captured(1));
            if (pf.open(QIODevice::ReadOnly)) details += QString::fromUtf8(pf.readAll());
        }
        auto nm = nameRe.match(details);
        d.name = (nm.hasMatch() ? nm.captured(1) : QFileInfo(bundle).completeBaseName()).toStdString();
        auto ty = typeRe.match(details);
        if (ty.hasMatch()) {
            d.instrument = ty.captured(1).contains("InstrumentPlugin");
            d.category = categoryFromWords(QString(ty.captured(1)).replace("Plugin", " "));
        }
        out.push_back(d);
    }
    return out;
}

// ---- Audio Units: the system registry lists them without loading code ------

#ifdef __APPLE__
std::string fourcc(OSType t) {
    char c[5] = {char((t >> 24) & 0xff), char((t >> 16) & 0xff), char((t >> 8) & 0xff), char(t & 0xff), 0};
    return c;
}

std::vector<Descriptor> enumerateAudioUnits() {
    std::vector<Descriptor> out;
    for (OSType type : {kAudioUnitType_Effect, kAudioUnitType_MusicEffect}) {
        AudioComponentDescription want{};
        want.componentType = type;
        AudioComponent comp = nullptr;
        while ((comp = AudioComponentFindNext(comp, &want)) != nullptr) {
            AudioComponentDescription desc{};
            if (AudioComponentGetDescription(comp, &desc) != noErr) continue;
            Descriptor d;
            d.format = Format::AudioUnit;
            d.pluginId = fourcc(desc.componentType) + ":" + fourcc(desc.componentSubType) + ":" +
                         fourcc(desc.componentManufacturer);
            d.id = idPrefix(Format::AudioUnit) + d.pluginId;
            CFStringRef name = nullptr;
            if (AudioComponentCopyName(comp, &name) == noErr && name) {
                char buf[512] = {};
                CFStringGetCString(name, buf, sizeof buf, kCFStringEncodingUTF8);
                CFRelease(name);
                QString full = QString::fromUtf8(buf);  // "Vendor: Name"
                int colon = full.indexOf(':');
                d.vendor = (colon > 0 ? full.left(colon).trimmed() : QString()).toStdString();
                d.name = (colon > 0 ? full.mid(colon + 1).trimmed() : full).toStdString();
            }
            UInt32 version = 0;
            if (AudioComponentGetVersion(comp, &version) == noErr)
                d.version = std::to_string(version >> 16) + "." + std::to_string((version >> 8) & 0xff) + "." +
                            std::to_string(version & 0xff);
            d.category = categoryFromWords(q(d.name));
            d.path = "AudioUnit";
            out.push_back(d);
        }
    }
    return out;
}
#endif

}  // namespace

// ---------------------------------------------------------------------------
// Discovery

std::vector<std::string> defaultSearchPaths(Format f) {
    QStringList dirs;
    const QString home = QDir::homePath();
    switch (f) {
        case Format::Clap:
            dirs << envPaths("CLAP_PATH");
#if defined(__APPLE__)
            dirs << home + "/Library/Audio/Plug-Ins/CLAP" << "/Library/Audio/Plug-Ins/CLAP";
#elif defined(_WIN32)
            dirs << envDir("LOCALAPPDATA", "Programs/Common/CLAP") << envDir("COMMONPROGRAMFILES", "CLAP");
#else
            dirs << home + "/.clap" << "/usr/lib/clap" << "/usr/local/lib/clap";
#endif
            break;
        case Format::Vst3:
            dirs << envPaths("VST3_PATH");
#if defined(__APPLE__)
            dirs << home + "/Library/Audio/Plug-Ins/VST3" << "/Library/Audio/Plug-Ins/VST3";
#elif defined(_WIN32)
            dirs << envDir("LOCALAPPDATA", "Programs/Common/VST3") << envDir("COMMONPROGRAMFILES", "VST3");
#else
            dirs << home + "/.vst3" << "/usr/lib/vst3" << "/usr/local/lib/vst3";
#endif
            break;
        case Format::Lv2: {
            // LV2_PATH replaces the defaults (LV2 specification).
            QStringList env = envPaths("LV2_PATH");
            if (!env.isEmpty()) {
                dirs = env;
                break;
            }
#if defined(__APPLE__)
            dirs << home + "/Library/Audio/Plug-Ins/LV2" << "/Library/Audio/Plug-Ins/LV2";
#elif defined(_WIN32)
            dirs << envDir("APPDATA", "LV2") << envDir("COMMONPROGRAMFILES", "LV2");
#else
            dirs << home + "/.lv2" << "/usr/local/lib/lv2" << "/usr/lib/lv2" << "/usr/lib/x86_64-linux-gnu/lv2"
                 << "/usr/lib/aarch64-linux-gnu/lv2";
#endif
            break;
        }
        case Format::AudioUnit:
#if defined(__APPLE__)
            dirs << "AudioUnit";  // enumerated through the system component registry
#endif
            break;
    }
    std::vector<std::string> out;
    for (const QString& d : dirs)
        if (!d.isEmpty() && std::find(out.begin(), out.end(), d.toStdString()) == out.end()) out.push_back(d.toStdString());
    return out;
}

std::vector<std::string> findPluginFiles(Format f, const std::vector<std::string>& dirs) {
    std::vector<std::string> out;
    if (f == Format::AudioUnit) {
        // Audio Units come from the system registry, enabled by the "AudioUnit"
        // entry of the default search list.
#ifdef __APPLE__
        if (std::find(dirs.begin(), dirs.end(), "AudioUnit") != dirs.end()) out.push_back("AudioUnit");
#endif
        return out;
    }
    const QString ext = f == Format::Clap ? ".clap" : f == Format::Vst3 ? ".vst3" : ".lv2";
    std::function<void(const QString&, int)> walk = [&](const QString& dir, int depth) {
        if (depth > 6) return;
        const QFileInfoList list = QDir(dir).entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
        for (const QFileInfo& fi : list) {
            if (fi.fileName().endsWith(ext, Qt::CaseInsensitive)) {
                if (f == Format::Lv2 && !QFileInfo::exists(fi.absoluteFilePath() + "/manifest.ttl")) continue;
                out.push_back(fi.absoluteFilePath().toStdString());
            } else if (fi.isDir() && !fi.isSymLink()) {
                walk(fi.absoluteFilePath(), depth + 1);
            }
        }
    };
    for (const std::string& d : dirs) walk(q(d), 0);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::optional<std::vector<Descriptor>> readStaticMetadata(Format f, const std::string& path) {
    switch (f) {
        case Format::Vst3: return readVst3ModuleInfo(path);
        case Format::Lv2: return readLv2Bundle(path);
        case Format::AudioUnit:
#ifdef __APPLE__
            return enumerateAudioUnits();
#else
            return std::vector<Descriptor>{};
#endif
        case Format::Clap: return std::nullopt;
    }
    return std::nullopt;
}

// Implemented per format (ClapHost.cpp, Vst3Host.cpp).
std::vector<Descriptor> probeClap(const std::string& path, std::string* error);
std::unique_ptr<Instance> instantiateClap(const Descriptor& d, std::string* error);
#ifdef MONTAGE_WITH_VST3
std::vector<Descriptor> probeVst3(const std::string& path, std::string* error);
std::unique_ptr<Instance> instantiateVst3(const Descriptor& d, std::string* error);
#endif
#ifdef __APPLE__
std::unique_ptr<Instance> instantiateAudioUnit(const Descriptor& d, std::string* error);
#endif
#ifdef MONTAGE_WITH_LV2
std::unique_ptr<Instance> instantiateLv2(const Descriptor& d, std::string* error);
#endif

std::vector<Descriptor> probeInProcess(Format f, const std::string& path, std::string* error) {
    if (f == Format::Clap) return probeClap(path, error);
#ifdef MONTAGE_WITH_VST3
    if (f == Format::Vst3) return probeVst3(path, error);
#endif
    if (auto s = readStaticMetadata(f, path)) return *s;
    if (error) *error = std::string(formatName(f)) + " plugins without metadata cannot be probed by this build";
    return {};
}

namespace {
std::atomic<int> gInstancesCreated{0};
}

int instancesCreated() { return gInstancesCreated.load(); }

namespace {
std::mutex gSidechainsM;
std::map<std::string, bool> gSidechains;  // descriptor id -> has a key input
}  // namespace

std::unique_ptr<Instance> instantiate(const Descriptor& d, std::string* error) {
    std::unique_ptr<Instance> inst;
    switch (d.format) {
        case Format::Clap: inst = instantiateClap(d, error); break;
#ifdef MONTAGE_WITH_VST3
        case Format::Vst3: inst = instantiateVst3(d, error); break;
#endif
#ifdef __APPLE__
        case Format::AudioUnit: inst = instantiateAudioUnit(d, error); break;
#endif
#ifdef MONTAGE_WITH_LV2
        case Format::Lv2: inst = instantiateLv2(d, error); break;
#endif
        default:
            if (error) *error = std::string(formatName(d.format)) + " plugins cannot be run by this version yet";
            break;
    }
    if (inst) {
        ++gInstancesCreated;
        std::lock_guard lock(gSidechainsM);
        gSidechains[d.id] = inst->hasSidechain();
    }
    return inst;
}

bool knownSidechain(const std::string& id) {
    std::lock_guard lock(gSidechainsM);
    const auto it = gSidechains.find(id);
    return it != gSidechains.end() && it->second;
}

// ---------------------------------------------------------------------------
// JSON

namespace {
QJsonObject toJson(const Descriptor& d) {
    return QJsonObject{{"format", formatName(d.format)},
                       {"id", q(d.id)},
                       {"pluginId", q(d.pluginId)},
                       {"name", q(d.name)},
                       {"vendor", q(d.vendor)},
                       {"version", q(d.version)},
                       {"category", q(d.category)},
                       {"path", q(d.path)},
                       {"instrument", d.instrument}};
}

std::optional<Descriptor> fromJson(const QJsonObject& o) {
    auto f = formatFromName(o.value("format").toString().toStdString());
    if (!f) return std::nullopt;
    Descriptor d;
    d.format = *f;
    d.id = o.value("id").toString().toStdString();
    d.pluginId = o.value("pluginId").toString().toStdString();
    d.name = o.value("name").toString().toStdString();
    d.vendor = o.value("vendor").toString().toStdString();
    d.version = o.value("version").toString().toStdString();
    d.category = o.value("category").toString().toStdString();
    d.path = o.value("path").toString().toStdString();
    d.instrument = o.value("instrument").toBool();
    if (d.id.empty()) return std::nullopt;
    return d;
}

QJsonArray toJsonArray(const std::vector<Descriptor>& ds) {
    QJsonArray a;
    for (const Descriptor& d : ds) a.append(toJson(d));
    return a;
}

std::vector<Descriptor> fromJsonArray(const QJsonArray& a) {
    std::vector<Descriptor> out;
    for (const QJsonValue& v : a)
        if (auto d = fromJson(v.toObject())) out.push_back(*d);
    return out;
}
}  // namespace

std::string descriptorsToJson(const std::vector<Descriptor>& ds) {
    return QJsonDocument(QJsonObject{{"plugins", toJsonArray(ds)}}).toJson(QJsonDocument::Compact).toStdString();
}

std::vector<Descriptor> descriptorsFromJson(const std::string& json, std::string* error) {
    QJsonParseError err{};
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(json), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error) *error = "invalid probe output";
        return {};
    }
    return fromJsonArray(doc.object().value("plugins").toArray());
}

// ---------------------------------------------------------------------------
// Registry

struct Registry::Entry {
    Format format = Format::Clap;
    std::string path;
    std::string signature;
    bool blocked = false;
    std::string reason;
    std::vector<Descriptor> plugins;
};

Registry& Registry::instance() {
    static Registry r;
    return r;
}

Registry::Registry() = default;
Registry::~Registry() = default;

void Registry::setCachePath(const std::string& path) {
    std::lock_guard lock(m_);
    cachePath_ = path;
    loaded_ = false;
    entries_.clear();
}

std::string Registry::cachePath() const {
    std::lock_guard lock(m_);
    return cachePath_.empty() ? defaultCachePath() : cachePath_;
}

std::string Registry::defaultCachePath() const {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dir.isEmpty()) dir = QDir::homePath() + "/.montage";
    return (dir + "/plugin-cache.json").toStdString();
}

void Registry::setProbeExecutable(const std::string& path) {
    std::lock_guard lock(m_);
    probe_ = path;
}

void Registry::setProbeTimeoutMs(int ms) {
    std::lock_guard lock(m_);
    timeoutMs_ = ms;
}

void Registry::setSearchPaths(Format f, std::vector<std::string> dirs) {
    std::lock_guard lock(m_);
    customPaths_[int(f)] = !dirs.empty();
    searchPaths_[int(f)] = std::move(dirs);
}

void Registry::setExtraSearchPaths(Format f, std::vector<std::string> dirs) {
    std::lock_guard lock(m_);
    extraPaths_[int(f)] = std::move(dirs);
}

std::vector<std::string> Registry::extraSearchPaths(Format f) const {
    std::lock_guard lock(m_);
    return extraPaths_[int(f)];
}

std::vector<std::string> Registry::searchPaths(Format f) const {
    std::lock_guard lock(m_);
    if (customPaths_[int(f)]) return searchPaths_[int(f)];
    std::vector<std::string> dirs = defaultSearchPaths(f);
    for (const std::string& d : extraPaths_[int(f)])
        if (std::find(dirs.begin(), dirs.end(), d) == dirs.end()) dirs.push_back(d);
    return dirs;
}

void Registry::setPluginDisabled(const std::string& id, bool disabled) {
    std::lock_guard lock(m_);
    loadCacheLocked();
    if (disabled) disabled_.insert(id);
    else disabled_.erase(id);
    saveCacheLocked();
}

bool Registry::isPluginDisabled(const std::string& id) const {
    std::lock_guard lock(m_);
    loadCacheLocked();
    return disabled_.count(id) > 0;
}

void Registry::loadCacheLocked() const {
    if (loaded_) return;
    loaded_ = true;
    entries_.clear();
    disabled_.clear();
    const std::string path = cachePath_.empty() ? defaultCachePath() : cachePath_;
    QFile f(q(path));
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    if (root.value("version").toInt() != 1) return;
    for (const QJsonValue& v : root.value("disabled").toArray()) disabled_.insert(v.toString().toStdString());
    for (const QJsonValue& v : root.value("entries").toArray()) {
        const QJsonObject o = v.toObject();
        auto fmt = formatFromName(o.value("format").toString().toStdString());
        if (!fmt) continue;
        Entry e;
        e.format = *fmt;
        e.path = o.value("path").toString().toStdString();
        e.signature = o.value("signature").toString().toStdString();
        e.blocked = o.value("blocked").toBool();
        e.reason = o.value("reason").toString().toStdString();
        e.plugins = fromJsonArray(o.value("plugins").toArray());
        entries_.push_back(std::move(e));
    }
}

void Registry::saveCacheLocked() const {
    const std::string path = cachePath_.empty() ? defaultCachePath() : cachePath_;
    QJsonArray entries;
    for (const Entry& e : entries_)
        entries.append(QJsonObject{{"format", formatName(e.format)},
                                   {"path", q(e.path)},
                                   {"signature", q(e.signature)},
                                   {"blocked", e.blocked},
                                   {"reason", q(e.reason)},
                                   {"plugins", toJsonArray(e.plugins)}});
    QDir().mkpath(QFileInfo(q(path)).absolutePath());
    QSaveFile f(q(path));
    if (!f.open(QIODevice::WriteOnly)) return;
    QJsonArray disabled;
    for (const std::string& id : disabled_) disabled.append(q(id));
    f.write(QJsonDocument(QJsonObject{{"version", 1}, {"entries", entries}, {"disabled", disabled}}).toJson());
    f.commit();
}

namespace {
std::string defaultProbe() {
#ifdef _WIN32
    const QString exe = "montage-plugin-probe.exe";
#else
    const QString exe = "montage-plugin-probe";
#endif
    QString dir = QCoreApplication::instance() ? QCoreApplication::applicationDirPath() : QDir::currentPath();
    // Next to the program, or a few levels up (development builds, app bundles).
    for (int up = 0; up < 5; ++up) {
        QString candidate = QDir(dir).filePath(exe);
        if (QFileInfo::exists(candidate)) return candidate.toStdString();
        dir = QDir(dir).filePath("..");
    }
    return exe.toStdString();
}
}  // namespace

namespace {
// Loads one plugin file in the probe process and turns its outcome into a
// cache entry (plugins found, or blocked with the reason).
void probeFile(const std::string& probe, int timeout, Format format, const std::string& path, std::vector<Descriptor>& plugins,
               bool& blocked, std::string& reason) {
    QProcess proc;
    proc.start(q(probe), {formatName(format), q(path)});
    if (!proc.waitForStarted(5000)) {
        blocked = true;
        reason = "the plugin probe could not be started (" + probe + ")";
    } else if (!proc.waitForFinished(timeout)) {
        proc.kill();
        proc.waitForFinished(2000);
        blocked = true;
        reason = "timed out after " + std::to_string(timeout / 1000) + " s";
    } else if (proc.exitStatus() != QProcess::NormalExit) {
        blocked = true;
        reason = "crashed while loading";
    } else if (proc.exitCode() != 0) {
        blocked = true;
        QString msg = QString::fromUtf8(proc.readAllStandardError()).trimmed().section('\n', 0, 0);
        reason = msg.isEmpty() ? "failed to load (exit code " + std::to_string(proc.exitCode()) + ")" : msg.toStdString();
    } else {
        std::string err;
        plugins = descriptorsFromJson(proc.readAllStandardOutput().toStdString(), &err);
        if (!err.empty()) {
            blocked = true;
            reason = err;
        }
    }
}
}  // namespace

ScanReport Registry::scan(bool rescanBlocked, const std::function<void(int, int, const std::string&)>& progress) {
    return scanImpl(rescanBlocked, {}, progress);
}

ScanReport Registry::rescan(const std::vector<std::string>& paths,
                            const std::function<void(int, int, const std::string&)>& progress) {
    return scanImpl(false, std::set<std::string>(paths.begin(), paths.end()), progress);
}

ScanReport Registry::scanImpl(bool rescanBlocked, const std::set<std::string>& force,
                              const std::function<void(int, int, const std::string&)>& progress) {
    // Snapshot the settings and the old cache, scan without the lock (it can
    // take a while), then publish the new entries.
    std::vector<Entry> old;
    std::string probe;
    int timeout;
    {
        std::lock_guard lock(m_);
        loadCacheLocked();
        old = entries_;
        probe = probe_.empty() ? defaultProbe() : probe_;
        timeout = timeoutMs_;
    }
    std::vector<std::pair<Format, std::string>> files;
    for (Format f : kAllFormats)
        for (const std::string& file : findPluginFiles(f, searchPaths(f))) files.emplace_back(f, file);

    ScanReport report;
    report.files = int(files.size());
    std::vector<Entry> fresh(files.size());
    std::vector<size_t> toProbe;
    std::mutex progressM;
    int done = 0;
    auto step = [&](const std::string& path) {
        std::lock_guard lock(progressM);
        if (progress) progress(done, int(files.size()), path);
        ++done;
    };
    // 1. Unchanged files come from the cache; metadata files and formats this
    //    build cannot load are read directly; the rest need the probe.
    for (size_t i = 0; i < files.size(); ++i) {
        const auto& [format, path] = files[i];
        Entry& e = fresh[i];
        e.format = format;
        e.path = path;
        // Audio Units come from the live system registry every time.
        e.signature = format == Format::AudioUnit ? std::string() : signature(path);
        auto prev = std::find_if(old.begin(), old.end(), [&](const Entry& o) { return o.format == format && o.path == path; });
        if (format != Format::AudioUnit && prev != old.end() && prev->signature == e.signature &&
            !(prev->blocked && rescanBlocked) && !force.count(path)) {
            e = *prev;
            ++report.fromCache;
            report.log.push_back("cached: " + path + (e.blocked ? " (blocked: " + e.reason + ")" : ""));
            step(path);
            continue;
        }
        if (auto meta = readStaticMetadata(format, path)) {
            e.plugins = *meta;
            report.log.push_back("read: " + path + " (" + std::to_string(e.plugins.size()) + " plugins)");
            step(path);
        } else if (!canHost(format)) {
            // Listed so the user can see it, but this build cannot load it.
            Descriptor d;
            d.format = format;
            d.path = path;
            d.name = QFileInfo(q(path)).completeBaseName().toStdString();
            d.pluginId = path;
            d.id = std::string(idPrefix(format)) + path;
            e.plugins.push_back(d);
            report.log.push_back("listed: " + path + " (this version cannot load " + formatName(format) + " plugins)");
            step(path);
        } else {
            toProbe.push_back(i);
        }
    }
    // 2. Probe in parallel: each file loads in its own helper process.
    report.probed = int(toProbe.size());
    std::atomic<size_t> next{0};
    auto worker = [&] {
        for (size_t k; (k = next++) < toProbe.size();) {
            Entry& e = fresh[toProbe[k]];
            step(e.path);
            probeFile(probe, timeout, e.format, e.path, e.plugins, e.blocked, e.reason);
        }
    };
    const size_t workers = std::min<size_t>(toProbe.size(), std::clamp<unsigned>(std::thread::hardware_concurrency() / 2, 2u, 4u));
    std::vector<std::thread> pool;
    for (size_t w = 1; w < workers; ++w) pool.emplace_back(worker);
    if (workers > 0) worker();
    for (auto& t : pool) t.join();
    for (size_t i : toProbe) {
        const Entry& e = fresh[i];
        if (e.blocked) {
            report.newlyBlocked.push_back({e.format, e.path, e.reason});
            report.log.push_back("blocked: " + e.path + " (" + e.reason + ")");
        } else {
            report.log.push_back("probed: " + e.path + " (" + std::to_string(e.plugins.size()) + " plugins)");
        }
    }
    if (progress) progress(int(files.size()), int(files.size()), {});
    {
        std::lock_guard lock(m_);
        entries_ = std::move(fresh);
        loaded_ = true;
        saveCacheLocked();
    }
    return report;
}

void Registry::setEnabled(bool on) {
    std::lock_guard lock(m_);
    enabled_ = on;
}

bool Registry::enabled() const {
    std::lock_guard lock(m_);
    return enabled_;
}

std::vector<Descriptor> Registry::plugins() const {
    std::lock_guard lock(m_);
    if (!enabled_) return {};
    loadCacheLocked();
    std::vector<Descriptor> out;
    for (const Entry& e : entries_)
        if (!e.blocked) out.insert(out.end(), e.plugins.begin(), e.plugins.end());
    std::sort(out.begin(), out.end(), [](const Descriptor& a, const Descriptor& b) {
        return QString::compare(q(a.name), q(b.name), Qt::CaseInsensitive) < 0;
    });
    return out;
}

std::optional<Descriptor> Registry::find(const std::string& id) const {
    std::lock_guard lock(m_);
    if (!enabled_) return std::nullopt;
    loadCacheLocked();
    for (const Entry& e : entries_)
        if (!e.blocked)
            for (const Descriptor& d : e.plugins)
                if (d.id == id) return d;
    return std::nullopt;
}

std::vector<Blocked> Registry::blocklist() const {
    std::lock_guard lock(m_);
    loadCacheLocked();
    std::vector<Blocked> out;
    for (const Entry& e : entries_)
        if (e.blocked) out.push_back({e.format, e.path, e.reason});
    return out;
}

void Registry::unblock(const std::string& path) {
    std::lock_guard lock(m_);
    loadCacheLocked();
    // Forgetting the entry makes the next scan probe the file again.
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [&](const Entry& e) { return e.blocked && e.path == path; }),
                   entries_.end());
    saveCacheLocked();
}

void Registry::clear() {
    std::lock_guard lock(m_);
    entries_.clear();
    disabled_.clear();
    loaded_ = true;
    saveCacheLocked();
}

}  // namespace montage::plugins
