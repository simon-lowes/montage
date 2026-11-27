#include "EncodeFolders.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSettings>
#include <set>

#include "core/EditOps.h"
#include "media/Decoder.h"
#include "render/Exporter.h"

namespace montage {

namespace {
QSettings settings() { return QSettings(QSettings::defaultFormat(), QSettings::UserScope, QStringLiteral("Montage"), QStringLiteral("Montage")); }
}  // namespace

bool encodeFolderSkips(const QString& name) {
    if (name.isEmpty() || name.startsWith(QLatin1Char('.')) || name.startsWith(QLatin1Char('~'))) return true;
    for (const char* partial : {".part", ".crdownload", ".download", ".tmp", ".partial"})
        if (name.endsWith(QLatin1String(partial), Qt::CaseInsensitive)) return true;
    return false;
}

EncodeFolderScan::EncodeFolderScan(EncodeFolder folder, qint64 settleMs) : folder_(std::move(folder)), settleMs_(std::max<qint64>(0, settleMs)) {
    reload();
}

QString EncodeFolderScan::ledgerPath() const { return QDir(folder_.output).filePath(QStringLiteral(".montage-encoded.json")); }

namespace {
QString entryKey(const QString& input, const QString& preset) { return input + QLatin1Char('\n') + preset; }
}  // namespace

QString EncodeFolderScan::key(const QString& file) const {
    return entryKey(QDir::cleanPath(QDir(folder_.input).absoluteFilePath(file)), folder_.preset);
}

void EncodeFolderScan::reload() {
    ledger_.clear();
    QFile f(ledgerPath());
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonObject all = QJsonDocument::fromJson(f.readAll()).object();
    // (a ledger of before: by bare name, whichever folder or preset it was)
    const QJsonObject files = all.value("files").toObject();
    for (auto it = files.begin(); it != files.end(); ++it) {
        const QJsonObject o = it.value().toObject();
        ledger_[it.key()] = {qint64(o.value("size").toDouble()), qint64(o.value("modified").toDouble()), o.value("output").toString(), {}, {}};
    }
    for (const QJsonValue& v : all.value("encodes").toArray()) {
        const QJsonObject o = v.toObject();
        const QString input = o.value("input").toString(), preset = o.value("preset").toString();
        if (input.isEmpty()) continue;
        ledger_[entryKey(input, preset)] = {qint64(o.value("size").toDouble()), qint64(o.value("modified").toDouble()), o.value("output").toString(), input, preset};
    }
}

const EncodeFolderScan::Done* EncodeFolderScan::entryFor(const QString& file) const {
    if (const auto it = ledger_.find(key(file)); it != ledger_.end()) return &it->second;
    const auto old = ledger_.find(file);
    if (old == ledger_.end()) return nullptr;
    const QFileInfo fi(QDir(folder_.input).filePath(file));
    return fi.size() == old->second.size && fi.lastModified().toMSecsSinceEpoch() == old->second.modified ? &old->second : nullptr;
}

bool EncodeFolderScan::encoded(const QString& file) const {
    const Done* d = entryFor(file);
    if (!d) return false;
    const QFileInfo fi(QDir(folder_.input).filePath(file));
    return fi.size() == d->size && fi.lastModified().toMSecsSinceEpoch() == d->modified;
}

int EncodeFolderScan::encodedCount() const {
    const QString input = QDir::cleanPath(QDir(folder_.input).absolutePath());
    int n = 0;
    for (const auto& [k, d] : ledger_)
        n += d.input.isEmpty() || (d.preset == folder_.preset && QFileInfo(d.input).absolutePath() == input);
    return n;
}

std::vector<QString> EncodeFolderScan::ready(qint64 now) {
    std::vector<QString> out;
    const QDir dir(folder_.input);
    if (!dir.exists()) return out;
    std::map<QString, Seen> next;
    waiting_ = 0;
    for (const QFileInfo& fi : dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name)) {
        const QString name = fi.fileName();
        if (encodeFolderSkips(name)) continue;
        Seen s{fi.size(), fi.lastModified().toMSecsSinceEpoch(), now};
        if (const auto was = seen_.find(name); was != seen_.end() && was->second.size == s.size && was->second.modified == s.modified)
            s.since = was->second.since;
        next[name] = s;
        if (now - s.since < settleMs_ || s.size <= 0) {  // still growing (or nothing written yet)
            waiting_ += !encoded(name);
            continue;
        }
        if (taken_.count(name) || encoded(name) || isOutput(fi.absoluteFilePath())) continue;
        if (const auto f = failed_.find(name); f != failed_.end() && f->second.size == s.size && f->second.modified == s.modified) continue;
        failed_.erase(name);
        taken_[name] = s;
        out.push_back(name);
    }
    seen_ = std::move(next);
    return out;
}

bool EncodeFolderScan::isOutput(const QString& path) const {
    for (const auto& [name, done] : ledger_)
        if (QFileInfo(done.output).absoluteFilePath() == path) return true;
    return false;
}

bool EncodeFolderScan::markDone(const QString& file, const QString& output) {
    const auto t = taken_.find(file);
    Done d;
    if (t != taken_.end()) d.size = t->second.size, d.modified = t->second.modified;
    else {
        const QFileInfo fi(QDir(folder_.input).filePath(file));
        d.size = fi.size(), d.modified = fi.lastModified().toMSecsSinceEpoch();
    }
    d.output = output;
    d.input = QDir::cleanPath(QDir(folder_.input).absoluteFilePath(file));
    d.preset = folder_.preset;
    if (t != taken_.end()) taken_.erase(t);
    reload();  // (another process, or another folder sharing the output folder, may have added to it)
    // (its entry of before, by bare name, now this one's)
    if (const auto old = ledger_.find(file); old != ledger_.end() && entryFor(file) == &old->second) ledger_.erase(old);
    ledger_[key(file)] = d;
    QJsonObject files;
    QJsonArray encodes;
    for (const auto& [name, done] : ledger_) {
        QJsonObject o{{"size", double(done.size)}, {"modified", double(done.modified)}, {"output", done.output}};
        if (done.input.isEmpty()) {
            files[name] = o;
            continue;
        }
        o["input"] = done.input;
        o["preset"] = done.preset;
        encodes.append(o);
    }
    QDir().mkpath(folder_.output);
    QSaveFile f(ledgerPath());
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QJsonDocument(QJsonObject{{"files", files}, {"encodes", encodes}}).toJson());
    return f.commit();
}

void EncodeFolderScan::letGo(const QString& file) {
    if (const auto t = taken_.find(file); t != taken_.end()) {
        failed_[file] = t->second;
        taken_.erase(t);
    }
}

QString EncodeFolderScan::outputFor(const QString& file, const std::vector<QString>& busy) const {
    const ExportPreset* p = findExportPreset(folder_.preset.toStdString());
    const QString ext = p ? QString::fromStdString(p->extension) : QStringLiteral("mp4");
    const QString base = QFileInfo(file).completeBaseName();
    const Done* mine = entryFor(file);
    std::set<QString> taken;
    for (const auto& [name, done] : ledger_)
        if (&done != mine) taken.insert(QFileInfo(done.output).fileName());
    // (what other encodes are writing now: in the output folder, by name)
    const QString into = QDir::cleanPath(QDir(folder_.output).absolutePath());
    for (const QString& b : busy)
        if (QDir::cleanPath(QFileInfo(b).absolutePath()) == into) taken.insert(QFileInfo(b).fileName());
    for (int n = 1;; ++n) {
        const QString name = (n == 1 ? base : QStringLiteral("%1 %2").arg(base).arg(n)) + '.' + ext;
        const QString path = QDir(folder_.output).filePath(name);
        // (Its own earlier encode is written over; another file's, another folder's or preset's, or a file not from
        // here, is not.)
        const bool own = mine && QFileInfo(mine->output).fileName() == name && !taken.count(name);
        if (own || (!taken.count(name) && !QFileInfo::exists(path))) return path;
    }
}

bool encodeFileWithPreset(const QString& input, const QString& preset, const QString& output, std::string* error,
                          const std::function<void(double)>& progress, const std::atomic<bool>* cancel) {
    auto fail = [error](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    const ExportPreset* ps = findExportPreset(preset.toStdString());
    if (!ps) return fail("There is no export preset \"" + preset.toStdString() + "\"");
    Project p = makeDefaultProject();
    MediaItem m;
    m.id = p.newId();
    std::string why;
    if (!probeMedia(QFileInfo(input).absoluteFilePath().toStdString(), m, &why)) return fail(why.empty() ? "The file cannot be read" : why);
    p.media.push_back(m);
    Sequence& s = *p.active();
    if (m.hasVideo && m.width > 0) {
        s.width = m.width, s.height = m.height;
        if (m.fps.valid()) s.fps = m.fps;
    }
    const edit::Result r = edit::placeMedia(p, s, m.id, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false);
    if (!r.ok) return fail(r.error);
    if (s.duration() <= 0) return fail("The file has nothing to encode");
    ExportSettings st = ps->settings;
    st.path = output.toStdString();
    QDir().mkpath(QFileInfo(output).absolutePath());
    // Written under a hidden name beside it (the same extension, which says the format) and moved there once whole,
    // so nothing half-written is taken for an encode.
    const QFileInfo out(output);
    const QString partial = out.dir().filePath(QStringLiteral(".") + out.completeBaseName() + QStringLiteral(".encoding.") + out.suffix());
    st.path = partial.toStdString();
    const bool ok = exportSequence(
        p, s, st, [&progress](double f, FrameTime) { if (progress) progress(f); }, cancel, &why);
    if (!ok) {
        QFile::remove(partial);
        return fail(why.empty() ? "The encode failed" : why);
    }
    QFile::remove(output);
    if (!QFile::rename(partial, output)) {
        QFile::remove(partial);
        return fail("Could not write " + output.toStdString());
    }
    return true;
}

std::vector<EncodeFolder> savedEncodeFolders() {
    std::vector<EncodeFolder> out;
    const QJsonArray a = QJsonDocument::fromJson(settings().value(QStringLiteral("encode/folders")).toByteArray()).array();
    for (const QJsonValue& v : a) {
        const QJsonObject o = v.toObject();
        EncodeFolder f{o.value("input").toString(), o.value("preset").toString(), o.value("output").toString()};
        if (!f.input.isEmpty() && !f.output.isEmpty()) out.push_back(f);
    }
    return out;
}

void saveEncodeFolders(const std::vector<EncodeFolder>& folders) {
    QJsonArray a;
    for (const EncodeFolder& f : folders) a.append(QJsonObject{{"input", f.input}, {"preset", f.preset}, {"output", f.output}});
    settings().setValue(QStringLiteral("encode/folders"), QJsonDocument(a).toJson(QJsonDocument::Compact));
}

}  // namespace montage
