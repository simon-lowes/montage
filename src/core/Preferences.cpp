#include "Preferences.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QSaveFile>
#include <QSet>
#include <QSettings>
#include <algorithm>
#include <cmath>

namespace montage::prefs {

namespace {

const char* const kContext = "montage::Preferences";

// The pages, in the window's order.
const char* const kPages[] = {
    QT_TRANSLATE_NOOP("montage::Preferences", "General"),   QT_TRANSLATE_NOOP("montage::Preferences", "Media"),
    QT_TRANSLATE_NOOP("montage::Preferences", "Playback"),  QT_TRANSLATE_NOOP("montage::Preferences", "Sound"),
    QT_TRANSLATE_NOOP("montage::Preferences", "Editing"),   QT_TRANSLATE_NOOP("montage::Preferences", "Programs"),
    QT_TRANSLATE_NOOP("montage::Preferences", "Libraries"), QT_TRANSLATE_NOOP("montage::Preferences", "Models"),
    QT_TRANSLATE_NOOP("montage::Preferences", "Network"),
};

Setting toggle(const char* key, const char* page, const char* label, const char* description, bool fallback,
               const char* env = nullptr) {
    Setting s;
    s.key = QString::fromLatin1(key);
    s.page = page;
    s.label = label;
    s.description = description;
    s.kind = Kind::Toggle;
    s.fallback = fallback;
    if (env) s.env = QString::fromLatin1(env);
    return s;
}

Setting whole(const char* key, const char* page, const char* label, const char* description, int fallback, int min,
              int max, const char* unit = nullptr, const char* env = nullptr) {
    Setting s = toggle(key, page, label, description, false, env);
    s.kind = Kind::Whole;
    s.fallback = fallback;
    s.min = min;
    s.max = max;
    s.unit = unit;
    return s;
}

Setting path(Kind kind, const char* key, const char* page, const char* label, const char* description,
             const char* env = nullptr) {
    Setting s = toggle(key, page, label, description, false, env);
    s.kind = kind;
    s.fallback = QString();
    return s;
}

Setting restarting(Setting s) {
    s.restart = true;
    return s;
}

std::vector<Setting> build() {
    std::vector<Setting> v;
    const char* const general = kPages[0];
    const char* const media = kPages[1];
    const char* const playback = kPages[2];
    const char* const sound = kPages[3];
    const char* const editing = kPages[4];
    const char* const programs = kPages[5];
    const char* const libraries = kPages[6];
    const char* const models = kPages[7];
    const char* const network = kPages[8];

    // General
    {
        Setting s =
            path(Kind::Choice, "ui/language", general, QT_TRANSLATE_NOOP("montage::Preferences", "Language"),
                 QT_TRANSLATE_NOOP(
                     "montage::Preferences",
                     "The language Montage's menus and windows are in; the system's own unless one is chosen."));
        v.push_back(restarting(s));
        s = toggle("ui/highContrast", general, QT_TRANSLATE_NOOP("montage::Preferences", "High contrast"),
                   QT_TRANSLATE_NOOP("montage::Preferences",
                                     "Colours with more contrast between text, controls and their backgrounds."),
                   false);
        v.push_back(restarting(s));
        s = toggle(
            "ui/textScale", general, QT_TRANSLATE_NOOP("montage::Preferences", "Text size"),
            QT_TRANSLATE_NOOP("montage::Preferences", "How much larger than the system's own the text is drawn."),
            false);
        s.kind = Kind::Number;
        s.fallback = 1.0;
        s.min = 1.0;
        s.max = 2.0;
        s.unit = QT_TRANSLATE_NOOP("montage::Preferences", "times");
        v.push_back(restarting(s));
    }
    v.push_back(
        whole("jobs/at_once", general, QT_TRANSLATE_NOOP("montage::Preferences", "Background jobs at once"),
              QT_TRANSLATE_NOOP("montage::Preferences",
                                "How many background jobs (renders, analyses, transcriptions) run side by side."),
              2, 1, 16));

    // Media
    v.push_back(whole(
        "media/growingRefreshMs", media, QT_TRANSLATE_NOOP("montage::Preferences", "Growing files looked at every"),
        QT_TRANSLATE_NOOP("montage::Preferences",
                          "How often media that is still being recorded is looked at again for what was added; 0 to "
                          "look only when asked."),
        15000, 0, 3600000, QT_TRANSLATE_NOOP("montage::Preferences", "ms")));
    v.push_back(toggle("import/imageSequences", media,
                       QT_TRANSLATE_NOOP("montage::Preferences", "Numbered stills as one clip"),
                       QT_TRANSLATE_NOOP("montage::Preferences",
                                         "Imported stills numbered one after another come in as a single clip."),
                       true));
    {
        Setting s =
            path(Kind::Choice, "import/psd", media, QT_TRANSLATE_NOOP("montage::Preferences", "Layered still images"),
                 QT_TRANSLATE_NOOP(
                     "montage::Preferences",
                     "How a layered image file comes in: merged into one picture, as a layer on a track each, or "
                     "as a sequence of its layers."));
        s.fallback = QStringLiteral("merged");
        s.choices = {
            {QStringLiteral("merged"), QT_TRANSLATE_NOOP("montage::Preferences", "Merged")},
            {QStringLiteral("layers"), QT_TRANSLATE_NOOP("montage::Preferences", "A track for each layer")},
            {QStringLiteral("sequence"), QT_TRANSLATE_NOOP("montage::Preferences", "A sequence of the layers")}};
        v.push_back(s);
    }
    v.push_back(
        toggle("soundLibrary/copyToProject", media,
               QT_TRANSLATE_NOOP("montage::Preferences", "Copy library sounds into the project"),
               QT_TRANSLATE_NOOP(
                   "montage::Preferences",
                   "A sound added from the sound library is copied into a Sounds folder beside the saved project."),
               false));
    v.push_back(path(
        Kind::Folder, "media/depthCache", media, QT_TRANSLATE_NOOP("montage::Preferences", "Depth analysis folder"),
        QT_TRANSLATE_NOOP("montage::Preferences",
                          "Where the depth of analysed video is kept; Montage's cache folder unless one is chosen."),
        "MONTAGE_DEPTH_CACHE"));
    v.push_back(whole("media/depthCacheGb", media, QT_TRANSLATE_NOOP("montage::Preferences", "Depth analysis kept"),
                      QT_TRANSLATE_NOOP("montage::Preferences",
                                        "How much depth analysis is kept: the analyses unchanged longest go first."),
                      20, 1, 100000, QT_TRANSLATE_NOOP("montage::Preferences", "GB"), "MONTAGE_DEPTH_CACHE_GB"));
    v.push_back(path(
        Kind::Folder, "media/graphicsCache", media,
        QT_TRANSLATE_NOOP("montage::Preferences", "Browser graphics folder"),
        QT_TRANSLATE_NOOP("montage::Preferences",
                          "Where rendered browser graphics are kept; Montage's cache folder unless one is chosen."),
        "MONTAGE_OGRAF_CACHE"));
    v.push_back(
        whole("media/graphicsCacheMb", media, QT_TRANSLATE_NOOP("montage::Preferences", "Browser graphics kept"),
              QT_TRANSLATE_NOOP("montage::Preferences",
                                "How much of the rendered browser graphics is kept: the oldest renders go first."),
              2048, 64, 10000000, QT_TRANSLATE_NOOP("montage::Preferences", "MB"), "MONTAGE_OGRAF_CACHE_MB"));
    v.push_back(restarting(whole(
        "media/deepCacheMb", media, QT_TRANSLATE_NOOP("montage::Preferences", "Deep images held"),
        QT_TRANSLATE_NOOP("montage::Preferences", "How much memory deep images are held in while they are composited."),
        1024, 64, 10000000, QT_TRANSLATE_NOOP("montage::Preferences", "MB"), "MONTAGE_DEEP_CACHE_MB")));

    // Playback
    {
        Setting s = toggle(
            "playback/gpuRendering", playback, QT_TRANSLATE_NOOP("montage::Preferences", "Render on the GPU"),
            QT_TRANSLATE_NOOP("montage::Preferences",
                              "Frames are composited on the graphics card when it can; otherwise on the processor."),
            true, "MONTAGE_GPU");
        v.push_back(restarting(s));
    }
    v.push_back(restarting(
        whole("playback/gpuMemoryMb", playback, QT_TRANSLATE_NOOP("montage::Preferences", "Texture budget"),
              QT_TRANSLATE_NOOP("montage::Preferences",
                                "The graphics card memory the GPU compositor keeps its textures in; 0 for half the "
                                "card's memory."),
              0, 0, 1048576, QT_TRANSLATE_NOOP("montage::Preferences", "MB"), "MONTAGE_GPU_MEMORY")));
    v.push_back(restarting(
        toggle("playback/gpuShare", playback, QT_TRANSLATE_NOOP("montage::Preferences", "Share the GPU with effects"),
               QT_TRANSLATE_NOOP("montage::Preferences",
                                 "Plug-in effects that draw on the graphics card share its frames with the "
                                 "compositor instead of copying them."),
               true, "MONTAGE_GPU_SHARE")));
    v.push_back(
        toggle("playback/gpuHandles", playback,
               QT_TRANSLATE_NOOP("montage::Preferences", "Pass frames to isolated effects on the GPU"),
               QT_TRANSLATE_NOOP(
                   "montage::Preferences",
                   "A plug-in effect running in its own process is handed frames on the graphics card rather than "
                   "copied through memory."),
               true, "MONTAGE_GPU_HANDLES"));
    v.push_back(restarting(
        toggle("playback/hardwareDecoding", playback, QT_TRANSLATE_NOOP("montage::Preferences", "Hardware decoding"),
               QT_TRANSLATE_NOOP("montage::Preferences", "Video is decoded by the graphics card where it can."), true,
               "MONTAGE_HWACCEL")));
    v.push_back(
        restarting(toggle("playback/hdrViewer", playback, QT_TRANSLATE_NOOP("montage::Preferences", "HDR viewer"),
                          QT_TRANSLATE_NOOP("montage::Preferences",
                                            "The viewer shows HDR pictures in HDR on a display that can show them."),
                          true)));

    // Sound
    v.push_back(toggle(
        "mixer/latchPrime", sound, QT_TRANSLATE_NOOP("montage::Preferences", "Latch writes from the start"),
        QT_TRANSLATE_NOOP("montage::Preferences",
                          "In Latch, automation is written from where playback starts, not from the first touch."),
        false));
    v.push_back(toggle("mixer/coalesceTrim", sound,
                       QT_TRANSLATE_NOOP("montage::Preferences", "Fold trim into the automation"),
                       QT_TRANSLATE_NOOP("montage::Preferences",
                                         "When a pass in Trim ends, its trim is folded into the automation under it."),
                       false));
    {
        Setting s = path(
            Kind::Choice, "plugins/hosting/isolation", sound, QT_TRANSLATE_NOOP("montage::Preferences", "Plug-ins run"),
            QT_TRANSLATE_NOOP("montage::Preferences",
                              "Which plug-ins share a process: a fault in one stops only the plug-ins beside it."));
        s.fallback = QStringLiteral("plugin");
        s.choices = {
            {QStringLiteral("plugin"), QT_TRANSLATE_NOOP("montage::Preferences", "Each plug-in in its own process")},
            {QStringLiteral("instance"), QT_TRANSLATE_NOOP("montage::Preferences", "Each instance in its own process")},
            {QStringLiteral("vendor"), QT_TRANSLATE_NOOP("montage::Preferences", "Each maker's in their own process")},
            {QStringLiteral("shared"), QT_TRANSLATE_NOOP("montage::Preferences", "All in one process")},
            {QStringLiteral("off"), QT_TRANSLATE_NOOP("montage::Preferences", "Inside Montage")}};
        v.push_back(restarting(s));
    }
    v.push_back(restarting(whole(
        "plugins/hosting/missedBlocks", sound, QT_TRANSLATE_NOOP("montage::Preferences", "Blocks a plug-in may miss"),
        QT_TRANSLATE_NOOP(
            "montage::Preferences",
            "How many blocks of sound in a row an isolated plug-in may be late with before it is restarted."),
        16, 1, 1000)));
    v.push_back(restarting(
        toggle("plugins/hosting/bypassWithSilence", sound,
               QT_TRANSLATE_NOOP("montage::Preferences", "Silence a stopped plug-in"),
               QT_TRANSLATE_NOOP(
                   "montage::Preferences",
                   "While an isolated plug-in restarts, its track is silent instead of passing its sound through."),
               false)));
    v.push_back(restarting(whole(
        "plugins/hosting/exportTimeoutMs", sound, QT_TRANSLATE_NOOP("montage::Preferences", "Plug-in wait in exports"),
        QT_TRANSLATE_NOOP("montage::Preferences",
                          "How long an export waits for an isolated plug-in's block before the export fails."),
        30000, 1000, 3600000, QT_TRANSLATE_NOOP("montage::Preferences", "ms"))));
    v.push_back(restarting(whole(
        "plugins/hosting/memoryLimitMb", sound, QT_TRANSLATE_NOOP("montage::Preferences", "Plug-in memory limit"),
        QT_TRANSLATE_NOOP("montage::Preferences",
                          "The memory an isolated plug-in's process may use before it is stopped; 0 for no limit."),
        4096, 0, 1048576, QT_TRANSLATE_NOOP("montage::Preferences", "MB"))));
    v.push_back(restarting(
        whole("plugins/hosting/autoRestarts", sound, QT_TRANSLATE_NOOP("montage::Preferences", "Plug-in restarts"),
              QT_TRANSLATE_NOOP("montage::Preferences",
                                "How often an isolated plug-in that stopped is started again before it is left off."),
              2, 0, 100)));

    // Editing
    v.push_back(restarting(toggle(
        "editing/sequencesAsClips", editing, QT_TRANSLATE_NOOP("montage::Preferences", "Sequences go in as one clip"),
        QT_TRANSLATE_NOOP("montage::Preferences",
                          "A sequence dropped into another goes in as one clip rather than as its own clips."),
        false)));

    // Programs
    v.push_back(
        path(Kind::Program, "editing/stillProgram", programs, QT_TRANSLATE_NOOP("montage::Preferences", "Image editor"),
             QT_TRANSLATE_NOOP(
                 "montage::Preferences",
                 "The program Edit in Another Application opens stills in; asked for the first time when none is "
                 "chosen.")));
    v.push_back(
        restarting(path(Kind::Program, "programs/python", programs, QT_TRANSLATE_NOOP("montage::Preferences", "Python"),
                        QT_TRANSLATE_NOOP("montage::Preferences",
                                          "The Python 3 scripts run with; python3 on the path unless one is chosen."),
                        "MONTAGE_PYTHON")));
    v.push_back(path(
        Kind::Program, "programs/browser", programs, QT_TRANSLATE_NOOP("montage::Preferences", "Browser for graphics"),
        QT_TRANSLATE_NOOP("montage::Preferences",
                          "The program browser graphics are drawn in; Chrome, Chromium or Edge where installed unless "
                          "one is chosen."),
        "MONTAGE_CHROMIUM"));

    // Libraries
    v.push_back(
        path(Kind::Folder, "libraries/lenses", libraries, QT_TRANSLATE_NOOP("montage::Preferences", "Lens database"),
             QT_TRANSLATE_NOOP(
                 "montage::Preferences",
                 "The folder of lens profiles lenses are corrected from; the downloaded or installed one unless one "
                 "is chosen."),
             "MONTAGE_LENS_DB"));
    v.push_back(
        path(Kind::Folder, "libraries/userLenses", libraries, QT_TRANSLATE_NOOP("montage::Preferences", "Your lenses"),
             QT_TRANSLATE_NOOP("montage::Preferences",
                               "Where the lenses you calibrate are kept; Montage's data folder unless one is chosen."),
             "MONTAGE_USER_LENSES"));
    v.push_back(path(
        Kind::File, "libraries/sounds", libraries, QT_TRANSLATE_NOOP("montage::Preferences", "Sound library"),
        QT_TRANSLATE_NOOP("montage::Preferences",
                          "The sound library's catalogue file; one in Montage's data folder unless one is chosen."),
        "MONTAGE_SOUND_LIBRARY"));
    v.push_back(path(Kind::Folder, "libraries/templates", libraries,
                     QT_TRANSLATE_NOOP("montage::Preferences", "Graphics templates"),
                     QT_TRANSLATE_NOOP("montage::Preferences", "The folder graphics templates are kept in."),
                     "MONTAGE_TEMPLATES_DIR"));
    v.push_back(path(Kind::Folder, "libraries/scripts", libraries, QT_TRANSLATE_NOOP("montage::Preferences", "Scripts"),
                     QT_TRANSLATE_NOOP("montage::Preferences", "The folder scripts are kept in."),
                     "MONTAGE_SCRIPTS_DIR"));
    v.push_back(path(Kind::Folder, "libraries/midiMaps", libraries,
                     QT_TRANSLATE_NOOP("montage::Preferences", "MIDI maps"),
                     QT_TRANSLATE_NOOP("montage::Preferences", "The folder MIDI controller maps are kept in."),
                     "MONTAGE_MIDI_MAPS"));
    v.push_back(path(Kind::File, "libraries/grain", libraries,
                     QT_TRANSLATE_NOOP("montage::Preferences", "Grain profiles"),
                     QT_TRANSLATE_NOOP("montage::Preferences", "The file film grain profiles are kept in."),
                     "MONTAGE_GRAIN_LIBRARY"));

    // Models
    v.push_back(
        path(Kind::Folder, "models/folder", models, QT_TRANSLATE_NOOP("montage::Preferences", "Models folder"),
             QT_TRANSLATE_NOOP("montage::Preferences",
                               "Where downloaded models are kept; Montage's data folder unless one is chosen.")));
    v.push_back(path(Kind::Folder, "models/speech", models, QT_TRANSLATE_NOOP("montage::Preferences", "Speech models"),
                     QT_TRANSLATE_NOOP("montage::Preferences",
                                       "Where the speech recognition models transcripts are made with are kept."),
                     "MONTAGE_WHISPER_MODELS"));
    v.push_back(path(Kind::Text, "models/speechDownloads", models,
                     QT_TRANSLATE_NOOP("montage::Preferences", "Speech models downloaded from"),
                     QT_TRANSLATE_NOOP(
                         "montage::Preferences",
                         "The address speech recognition models are downloaded from, for a copy kept nearer; empty for "
                         "their own."),
                     "MONTAGE_WHISPER_MODEL_URL"));
    v.push_back(path(
        Kind::Folder, "models/vmaf", models, QT_TRANSLATE_NOOP("montage::Preferences", "Quality models"),
        QT_TRANSLATE_NOOP("montage::Preferences",
                          "The folder the HDR picture quality model is looked for in; one beside Montage unless one is "
                          "chosen."),
        "MONTAGE_VMAF_MODELS"));
    {
        Setting s = path(
            Kind::Choice, "ai/inference", models, QT_TRANSLATE_NOOP("montage::Preferences", "Models run on"),
            QT_TRANSLATE_NOOP("montage::Preferences",
                              "Where models run: the fastest device, the one that uses least power, or the processor."),
            "MONTAGE_INFERENCE");
        s.fallback = QStringLiteral("auto");
        s.choices = {
            {QStringLiteral("auto"), QT_TRANSLATE_NOOP("montage::Preferences", "The fastest device")},
            {QStringLiteral("efficient"), QT_TRANSLATE_NOOP("montage::Preferences", "The most efficient device")},
            {QStringLiteral("cpu"), QT_TRANSLATE_NOOP("montage::Preferences", "The processor")}};
        v.push_back(s);
    }

    // Network
    v.push_back(
        restarting(toggle("agentLink/enabled", network, QT_TRANSLATE_NOOP("montage::Preferences", "Agent link"),
                          QT_TRANSLATE_NOOP("montage::Preferences",
                                            "Assistants on this computer may drive Montage through its agent link."),
                          false)));
    v.push_back(
        restarting(toggle("osc/enabled", network, QT_TRANSLATE_NOOP("montage::Preferences", "OSC control"),
                          QT_TRANSLATE_NOOP("montage::Preferences", "Montage listens for OSC messages."), false)));
    v.push_back(restarting(whole(
        "osc/port", network, QT_TRANSLATE_NOOP("montage::Preferences", "OSC port"),
        QT_TRANSLATE_NOOP("montage::Preferences", "The port OSC messages are listened for on."), 9100, 1, 65535)));
    v.push_back(restarting(toggle(
        "osc/network", network, QT_TRANSLATE_NOOP("montage::Preferences", "OSC from other computers"),
        QT_TRANSLATE_NOOP("montage::Preferences", "OSC messages are taken from other computers, not only this one."),
        false)));
    v.push_back(restarting(
        whole("osc/feedbackPort", network, QT_TRANSLATE_NOOP("montage::Preferences", "OSC feedback port"),
              QT_TRANSLATE_NOOP("montage::Preferences", "The port Montage's state is sent back to; 0 to send none."), 0,
              0, 65535)));
    v.push_back(
        restarting(toggle("osc/tools", network, QT_TRANSLATE_NOOP("montage::Preferences", "OSC may run tools"),
                          QT_TRANSLATE_NOOP("montage::Preferences",
                                            "OSC messages may run the agent tools, which can change the project."),
                          false)));
    v.push_back(path(Kind::Text, "network/omtDiscovery", network,
                     QT_TRANSLATE_NOOP("montage::Preferences", "OMT discovery server"),
                     QT_TRANSLATE_NOOP(
                         "montage::Preferences",
                         "The server network video sources are found through; found on the local network unless one is "
                         "named."),
                     "MONTAGE_OMT_DISCOVERY_SERVER"));
    return v;
}

// The facility's file, read once (or again by reloadFacility).
struct Facility {
    bool read = false;
    QString file, problem;
    QVariantMap values;
    QSet<QString> locked;
};

QMutex& facilityMutex() {
    static QMutex m;
    return m;
}

Facility& facilityLocked() {
    static Facility f;
    if (f.read) return f;
    f = Facility();
    f.read = true;
    f.file = qEnvironmentVariable("MONTAGE_FACILITY_SETTINGS");
    if (f.file.isEmpty()) return f;
    QFile file(f.file);
    if (!file.open(QIODevice::ReadOnly)) {
        f.problem =
            QCoreApplication::translate(kContext, "The facility's settings file %1 could not be read").arg(f.file);
        return f;
    }
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &err);
    if (!doc.isObject() || doc.object().value(QStringLiteral("montage-settings")).toInt() != 1) {
        f.problem =
            QCoreApplication::translate(kContext, "The facility's settings file %1 is not a Montage settings file")
                .arg(f.file);
        return f;
    }
    f.values = doc.object().value(QStringLiteral("settings")).toObject().toVariantMap();
    for (const QJsonValue& k : doc.object().value(QStringLiteral("locked")).toArray())
        if (k.isString()) f.locked.insert(k.toString());
    QStringList unknown;
    for (auto it = f.values.cbegin(); it != f.values.cend(); ++it)
        if (!find(it.key())) unknown << it.key();
    for (const QString& k : std::as_const(f.locked))
        if (!find(k) && !unknown.contains(k)) unknown << k;
    if (!unknown.isEmpty())
        f.problem = QCoreApplication::translate(
                        kContext, "The facility's settings file %1 names settings Montage does not have: %2")
                        .arg(f.file, unknown.join(QStringLiteral(", ")));
    return f;
}

QSettings store() {
    return QSettings(QSettings::defaultFormat(), QSettings::UserScope, QStringLiteral("Montage"),
                     QStringLiteral("Montage"));
}

bool offWord(const QString& v) {
    const QString w = v.trimmed().toLower();
    return w == QLatin1String("off") || w == QLatin1String("0") || w == QLatin1String("false") ||
           w == QLatin1String("no") || w == QLatin1String("none");
}

// A value made the setting's kind (a number in its range, a choice one of its own); invalid when it cannot be.
QVariant asKind(const Setting& s, const QVariant& v) {
    if (!v.isValid() || v.isNull()) return {};
    bool ok = false;
    switch (s.kind) {
        case Kind::Toggle:
            if (v.typeId() == QMetaType::Bool) return v.toBool();
            if (v.typeId() == QMetaType::QString) return !offWord(v.toString());
            return v.toDouble() != 0;
        case Kind::Whole: {
            const double d = v.toDouble(&ok);
            if (!ok || !std::isfinite(d)) return {};
            return int(std::clamp(std::round(d), s.min, s.max));
        }
        case Kind::Number: {
            const double d = v.toDouble(&ok);
            if (!ok || !std::isfinite(d)) return {};
            return std::clamp(d, s.min, s.max);
        }
        case Kind::Choice: {
            const QString c = v.toString();
            if (s.choices.empty()) return c;
            for (const Choice& o : s.choices)
                if (o.value == c) return c;
            return {};
        }
        default:
            // (a list some earlier version kept is one text)
            if (v.typeId() == QMetaType::QStringList) return v.toStringList().join(QLatin1Char(' '));
            return v.toString();
    }
}

QVariant envValue(const Setting& s) {
    if (s.env.isEmpty()) return {};
    const QString e = qEnvironmentVariable(s.env.toLatin1().constData());
    if (e.isEmpty()) return {};
    return asKind(s, e);
}

}  // namespace

const std::vector<Setting>& all() {
    static const std::vector<Setting> settings = build();
    return settings;
}

const QStringList& pages() {
    static const QStringList p = [] {
        QStringList l;
        for (const char* page : kPages) l << QString::fromLatin1(page);
        return l;
    }();
    return p;
}

const Setting* find(const QString& key) {
    static const QHash<QString, const Setting*> index = [] {
        QHash<QString, const Setting*> h;
        for (const Setting& s : all()) h.insert(s.key, &s);
        return h;
    }();
    return index.value(key, nullptr);
}

QVariant defaultFor(const QString& key) {
    const Setting* s = find(key);
    if (!s) return {};
    {
        QMutexLocker lock(&facilityMutex());
        const Facility& f = facilityLocked();
        if (const QVariant v = asKind(*s, f.values.value(key)); v.isValid()) return v;
    }
    return s->fallback;
}

QVariant get(const QString& key) {
    const Setting* s = find(key);
    if (!s) return {};
    if (const QVariant v = envValue(*s); v.isValid()) return v;
    {
        QMutexLocker lock(&facilityMutex());
        const Facility& f = facilityLocked();
        if (f.locked.contains(key))
            if (const QVariant v = asKind(*s, f.values.value(key)); v.isValid()) return v;
    }
    if (const QVariant v = asKind(*s, store().value(key)); v.isValid()) return v;
    return defaultFor(key);
}

bool toggle(const QString& key) { return get(key).toBool(); }
int whole(const QString& key) { return get(key).toInt(); }
double number(const QString& key) { return get(key).toDouble(); }
QString text(const QString& key) { return get(key).toString(); }

bool isSet(const QString& key) { return find(key) && store().contains(key); }

QString overriddenBy(const QString& key) {
    const Setting* s = find(key);
    if (!s || !envValue(*s).isValid()) return {};
    return s->env + QLatin1Char('=') + qEnvironmentVariable(s->env.toLatin1().constData());
}

QString lockedBy(const QString& key) {
    QMutexLocker lock(&facilityMutex());
    const Facility& f = facilityLocked();
    return find(key) && f.locked.contains(key) ? f.file : QString();
}

bool set(const QString& key, const QVariant& value, QString* why) {
    const Setting* s = find(key);
    if (!s) {
        if (why) *why = QCoreApplication::translate(kContext, "%1 is not one of Montage's settings").arg(key);
        return false;
    }
    if (const QString file = lockedBy(key); !file.isEmpty()) {
        if (why) *why = QCoreApplication::translate(kContext, "Locked by the facility's settings file %1").arg(file);
        return false;
    }
    const QVariant v = asKind(*s, value);
    if (!v.isValid()) {
        if (why)
            *why = QCoreApplication::translate(kContext, "%1 is not a value %2 can take").arg(value.toString(), key);
        return false;
    }
    store().setValue(key, v);
    return true;
}

void restore(const QString& key) {
    if (!find(key) || !lockedBy(key).isEmpty()) return;
    store().remove(key);
}

QString facilityFile() {
    QMutexLocker lock(&facilityMutex());
    return facilityLocked().file;
}

void reloadFacility() {
    QMutexLocker lock(&facilityMutex());
    facilityLocked().read = false;
}

QString facilityProblem() {
    QMutexLocker lock(&facilityMutex());
    return facilityLocked().problem;
}

bool saveTo(const QString& path, QString* error) {
    QJsonObject values;
    QSettings settings = store();
    for (const Setting& s : all()) {
        if (!settings.contains(s.key)) continue;
        if (const QVariant v = asKind(s, settings.value(s.key)); v.isValid())
            values.insert(s.key, QJsonValue::fromVariant(v));
    }
    QJsonObject doc{{QStringLiteral("montage-settings"), 1}, {QStringLiteral("settings"), values}};
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(doc).toJson()) < 0 || !file.commit()) {
        if (error) *error = QCoreApplication::translate(kContext, "%1 could not be written").arg(path);
        return false;
    }
    return true;
}

bool loadFrom(const QString& path, QStringList* passed, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QCoreApplication::translate(kContext, "%1 could not be read").arg(path);
        return false;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();
    if (!doc.isObject() || doc.object().value(QStringLiteral("montage-settings")).toInt() != 1) {
        if (error) *error = QCoreApplication::translate(kContext, "%1 is not a Montage settings file").arg(path);
        return false;
    }
    const QJsonObject values = doc.object().value(QStringLiteral("settings")).toObject();
    for (auto it = values.begin(); it != values.end(); ++it) {
        const Setting* s = find(it.key());
        if (!s || !set(it.key(), it.value().toVariant())) {
            if (passed) *passed << it.key();
        }
    }
    return true;
}

const QStringList& remembered() {
    // Remembered choices: a dialog's last folder or options, the window's layout, what a panel showed last. Whole keys,
    // and groups ending in "/*" (every key under them).
    static const QStringList keys = {
        QStringLiteral("ad/*"),
        QStringLiteral("adr/*"),
        QStringLiteral("assistant/*"),
        QStringLiteral("attributes/*"),
        QStringLiteral("avidBake"),
        QStringLiteral("avidCodec"),
        QStringLiteral("avidDrive"),
        QStringLiteral("avidHandles"),
        QStringLiteral("bin/*"),
        QStringLiteral("breathControl/*"),
        QStringLiteral("calibration/*"),
        QStringLiteral("captions/*"),
        QStringLiteral("capture/*"),
        QStringLiteral("commands/*"),
        QStringLiteral("controlSurface/*"),
        QStringLiteral("credentials/*"),
        QStringLiteral("dcp/*"),
        QStringLiteral("deck/*"),
        QStringLiteral("encode/*"),
        QStringLiteral("eq/*"),
        QStringLiteral("export/*"),
        QStringLiteral("grade/*"),
        QStringLiteral("imf/*"),
        QStringLiteral("intelligibility/*"),
        QStringLiteral("keymap"),
        QStringLiteral("keymap/*"),
        QStringLiteral("keymapPreset"),
        QStringLiteral("lastExportDir"),
        QStringLiteral("lastImportDir"),
        QStringLiteral("lastImportTimelineDir"),
        QStringLiteral("lastProductionDir"),
        QStringLiteral("lastProjectDir"),
        QStringLiteral("loudness/*"),
        QStringLiteral("loudnessReport/*"),
        QStringLiteral("mediaBin/*"),
        QStringLiteral("meters/*"),
        QStringLiteral("midiLearn/*"),
        QStringLiteral("monitor/*"),
        QStringLiteral("monitoring/*"),
        QStringLiteral("multicam/*"),
        QStringLiteral("newSequence/*"),
        QStringLiteral("offload/*"),
        QStringLiteral("output/*"),
        QStringLiteral("playback/ambisonicBinaural"),
        QStringLiteral("playback/binauralHrtf"),
        QStringLiteral("playback/frameLines"),
        QStringLiteral("playback/gpuStarting"),
        QStringLiteral("playback/reviewDrawings"),
        QStringLiteral("playback/stereoView"),
        QStringLiteral("playback/twoUpTrim"),
        QStringLiteral("plugins/extraFolders"),
        QStringLiteral("plugins/extraFolders/*"),
        QStringLiteral("program/*"),
        QStringLiteral("proxy/*"),
        QStringLiteral("recentProjects"),
        QStringLiteral("render/*"),
        QStringLiteral("renderFarm/*"),
        QStringLiteral("review/*"),
        QStringLiteral("reviewViewer/*"),
        QStringLiteral("scripts/*"),
        QStringLiteral("shorts/*"),
        QStringLiteral("speedEditor/*"),
        QStringLiteral("spelling/*"),
        QStringLiteral("stripSilence/*"),
        QStringLiteral("tangent/*"),
        QStringLiteral("timeline/*"),
        QStringLiteral("transcribe/*"),
        QStringLiteral("transcript/*"),
        QStringLiteral("vfx/*"),
        QStringLiteral("view/*"),
        QStringLiteral("window/*"),
        QStringLiteral("workspaceDirections/*"),
        QStringLiteral("workspaces/*"),
    };
    return keys;
}

bool isRemembered(const QString& key) {
    for (const QString& r : remembered()) {
        if (r.endsWith(QLatin1String("/*")) ? key.startsWith(r.chopped(1)) || key == r.chopped(2) : key == r)
            return !find(key);
    }
    return false;
}

}  // namespace montage::prefs
