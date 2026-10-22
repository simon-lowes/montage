#include "AvidBinImport.h"

#include <QDateTime>
#include <QFileInfo>
#include <QString>
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>

#include "AvidBin.h"
#include "AvidMediaIndex.h"
#include "EditOps.h"
#include "ImportBuilder.h"
#include "MarkerList.h"

namespace montage {

namespace {

std::string idKey(const AvidMobId& id) { return std::string(reinterpret_cast<const char*>(id.data()), id.size()); }
// A source clip pointing nowhere (the end of a chain): its material number is all zeros.
bool nullId(const AvidMobId& id) { return std::all_of(id.begin() + 16, id.end(), [](uint8_t b) { return b == 0; }); }

// An edit rate as Media Composer stores it (23.976) as the rational it stands for (24000/1001).
Rational rationalRate(double r) {
    if (!(r >= 1 && r <= 1e6)) return {25, 1};
    for (int base : {24, 30, 48, 60, 120})
        if (std::fabs(r - base * 1000.0 / 1001) < 0.005) return {base * 1000, 1001};
    if (std::fabs(r - std::round(r)) < 0.005) return {int(std::lround(r)), 1};
    return {int(std::lround(r * 1000)), 1000};
}

// The locator of the file a descriptor stands for: its own, its physical media's, or a child's (multiple
// descriptors).
const AvidMediaLocator* fileLocator(const AvidDescriptor& d, std::set<const AvidDescriptor*>& seen, int depth = 0) {
    auto isFile = [](const AvidMediaLocator& l) { return l.classId != "MSML" && !(l.pathPosix.empty() && l.pathUtf8.empty() && l.path.empty()); };
    if (d.locator && isFile(*d.locator)) return &*d.locator;
    if (d.physicalMedia && d.physicalMedia->locator && isFile(*d.physicalMedia->locator)) return &*d.physicalMedia->locator;
    if (depth < 8)
        for (const auto& c : d.children)
            if (c && seen.insert(c.get()).second)  // (each looked at once, however often it is named)
                if (const AvidMediaLocator* l = fileLocator(*c, seen, depth + 1)) return l;
    return nullptr;
}
const AvidMediaLocator* fileLocator(const AvidDescriptor& d) {
    std::set<const AvidDescriptor*> seen;
    return fileLocator(d, seen);
}

// The path a locator gives, as this computer would have it: as written (the UTF-8 forms first), or (written on a Mac
// with its volume's name first, "Macintosh HD/Users/...") under /Volumes or from the root. The first that exists, else
// the first given.
std::string locatorPath(const AvidMediaLocator& l, bool* exists = nullptr) {
    std::vector<std::string> tries;
    for (const std::string* s : {&l.pathUtf8, &l.path2Utf8, &l.pathPosix, &l.path}) {
        if (s->empty()) continue;
        std::string p = *s;
        std::replace(p.begin(), p.end(), '\\', '/');
        tries.push_back(p);
        if (p[0] != '/' && !(p.size() > 1 && p[1] == ':')) {
            tries.push_back("/Volumes/" + p);
            if (const size_t slash = p.find('/'); slash != std::string::npos) tries.push_back(p.substr(slash));
        }
    }
    for (const std::string& t : tries)
        if (QFileInfo(QString::fromStdString(t)).isFile()) {
            if (exists) *exists = true;
            return t;
        }
    if (exists) *exists = false;
    return tries.empty() ? std::string() : tries.front();
}

const AvidComponent* trackComponent(const AvidMob& m, int index, const std::string& kind) {
    for (const AvidTrack& t : m.tracks)
        if (t.index && *t.index == index && t.component && t.component->mediaKind == kind) return t.component.get();
    return nullptr;
}

// The source clip under `units` (the component's edit units) of a component, and how far into it.
const AvidComponent* clipAt(const AvidComponent* c, double units, double& into, int depth = 0) {
    if (!c || depth > 64) return nullptr;
    if (c->classId == "SCLP") {
        into = units;
        return c;
    }
    if (c->classId == "SEQU") {
        double cursor = 0;
        for (const auto& child : c->components) {
            if (!child) continue;
            if (child->classId == "TNFX") {
                cursor -= double(child->length);
                continue;
            }
            if (units < cursor + double(child->length)) return clipAt(child.get(), units - cursor, into, depth + 1);
            cursor += double(child->length);
        }
        return nullptr;
    }
    if (c->isTrackGroup && !c->tracks.empty()) {
        const size_t pick = c->classId == "SLCT" && c->selected < c->tracks.size() ? c->selected : 0;
        return clipAt(c->tracks[pick].component.get(), units, into, depth + 1);
    }
    return nullptr;
}

// Where a source clip leads: its master clip, the file mob (Media Composer's media for it) and the time into it, and
// what that was made from (a file linked or imported) and its tape.
struct Source {
    const AvidMob* master = nullptr;
    const AvidMob* file = nullptr;
    double seconds = 0;
    int channel = -1;  // the file's sound track played, where it has more than one
    const AvidMob* original = nullptr;
    double originalSeconds = 0;
    int originalChannel = -1;
    std::string tape;
};

// Which of a mob's sound tracks `track` is (-1 when it has one, or for picture).
int soundChannel(const AvidMob& m, int track, const std::string& kind) {
    if (kind != "sound") return -1;
    int index = 0, channel = -1;
    for (const AvidTrack& t : m.tracks) {
        if (!t.component || t.component->mediaKind != "sound") continue;
        if (t.index && *t.index == track && channel < 0) channel = index;
        ++index;
    }
    return index > 1 ? channel : -1;
}

// Follows a source clip to (mob, track) at `units` of the caller's edit rate.
void follow(const AvidBin& bin, const AvidMobId& id, int track, const std::string& kind, double units, double callerRate,
            Source& s, int depth = 0) {
    const AvidMob* m = depth < 24 ? bin.findMob(id) : nullptr;
    if (!m) return;
    const AvidComponent* c = trackComponent(*m, track, kind);
    const double rate = c && c->editRate > 0 ? c->editRate : callerRate > 0 ? callerRate : m->editRate;
    const double here = callerRate > 0 && rate > 0 ? units * rate / callerRate : units;  // in this track's units
    const double seconds = rate > 0 ? here / rate : 0;
    if (m->mobType == "MasterMob" && !s.master) s.master = m;
    if (const AvidDescriptor* d = m->descriptor.get()) {
        if (d->mobKind == 2 || d->classId == "MDTP") {  // a tape
            s.tape = m->name;
            return;
        }
        if (d->mobKind == 1 && !s.file) {
            s.file = m;
            s.seconds = seconds;
            s.channel = soundChannel(*m, track, kind);
        } else if (!s.original && fileLocator(*d)) {
            s.original = m;
            s.originalSeconds = seconds;
            s.originalChannel = soundChannel(*m, track, kind);
        }
    }
    double into = 0;
    const AvidComponent* next = clipAt(c, here, into);
    if (!next || !next->hasMobId || nullId(next->mobId)) return;
    follow(bin, next->mobId, next->trackId, kind, double(next->startTime) + into, next->editRate > 0 ? next->editRate : rate, s, depth + 1);
}

// A stretch of a track: a source clip or a transition, in the track's edit units.
struct Piece {
    enum Kind { Clip, Transition } kind = Clip;
    double pos = 0, len = 0;
    AvidMobId mob{};
    int track = 0;
    double start = 0, rate = 0;  // into the clip's mob track, in the source clip's units
    double speed = 1;
    bool reverse = false;
    double cutPoint = 0;
};
struct Locator {
    double at = 0;  // track units
    std::string comment, colour;
};

class Walker {
public:
    Walker(std::vector<Piece>& pieces, std::vector<Locator>& locators, std::vector<std::string>& warnings, int& budget)
        : pieces_(pieces), locators_(locators), warnings_(warnings), budget_(budget) {}
    void walk(const AvidComponent* c, double pos, double speed = 1, bool reverse = false, int depth = 0) {
        if (!c || depth > 64) return;
        // (A sequence may name the same part many times, and that part the same again: the parts walked are counted.)
        if (--budget_ < 0) {
            if (budget_ == -1) warnings_.push_back("A sequence too large to bring in whole was cut short");
            return;
        }
        for (const AvidLocator& l : c->locators) locators_.push_back({pos + l.offset, l.comment, l.colorName});
        if (!c->readable) {
            warnings_.push_back("Part of a sequence could not be read (" + c->classId + ") and came in as a gap");
            return;
        }
        if (c->classId == "SEQU") {
            double cursor = pos;
            for (const auto& child : c->components) {
                if (!child) continue;
                if (child->classId == "TNFX") {
                    cursor -= double(child->length);
                    Piece t;
                    t.kind = Piece::Transition;
                    t.pos = cursor;
                    t.len = double(child->length);
                    t.cutPoint = child->cutPoint;
                    pieces_.push_back(t);
                    continue;
                }
                walk(child.get(), cursor, speed, reverse, depth + 1);
                cursor += double(child->length);
            }
            return;
        }
        if (c->classId == "SCLP") {
            if (!c->hasMobId || nullId(c->mobId) || c->length <= 0) return;
            Piece pc;
            pc.pos = pos;
            pc.len = double(c->length);
            pc.mob = c->mobId;
            pc.track = c->trackId;
            pc.start = c->startTime;
            pc.rate = c->editRate;
            pc.speed = speed;
            pc.reverse = reverse;
            pieces_.push_back(pc);
            return;
        }
        if (!c->isTrackGroup) return;  // fillers, timecode, edge code
        // A group: what it shows (a group clip's selected angle, else its first input) for its length.
        size_t pick = 0;
        if (c->classId == "SLCT" && c->selected < c->tracks.size()) pick = c->selected;
        while (pick < c->tracks.size() && !c->tracks[pick].component) ++pick;
        if (pick >= c->tracks.size()) return;
        const std::string name = !c->effectId.empty() ? c->effectId : !c->name.empty() ? c->name : c->classId;
        double inner = speed;
        bool backwards = reverse;
        if (c->classId == "SPED") {
            if (c->speedNum != 0 && c->speedDen > 0) inner = speed * std::fabs(double(c->speedNum)) / double(c->speedDen);
            if (c->speedNum < 0) backwards = !backwards;
        } else if (c->classId == "PVOL") {
            if ((c->levelSet && c->level != (1 << 29)) || !c->parameters.empty())
                warnings_.push_back("Clip volume and pan from Media Composer were left at their defaults");
        } else if (c->classId != "SLCT" && c->classId != "RSET" && c->classId != "TRKG") {
            warnings_.push_back("Effect " + name + " was left off (what it was applied to is kept)");
        }
        std::vector<Piece> sub;
        std::vector<Locator> subLocators;
        Walker w(sub, subLocators, warnings_, budget_);
        w.walk(c->tracks[pick].component.get(), pos, inner, backwards, depth + 1);
        locators_.insert(locators_.end(), subLocators.begin(), subLocators.end());
        const double end = pos + double(c->length);
        if (c->classId == "SPED") {
            // A motion effect plays its input's clip from that clip's start, at its speed, for the effect's length.
            int clips = 0;
            for (Piece& pc : sub)
                if (pc.kind == Piece::Clip && clips++ == 0) {
                    pc.pos = pos;
                    pc.len = double(c->length);
                    pieces_.push_back(pc);
                }
            if (clips > 1) warnings_.push_back("A motion effect over more than one clip came in as its first clip");
            return;
        }
        for (Piece pc : sub) {  // the input, kept to the group's length
            if (pc.pos >= end) continue;
            pc.len = std::min(pc.len, end - pc.pos);
            if (pc.len > 0) pieces_.push_back(pc);
        }
    }

private:
    std::vector<Piece>& pieces_;
    std::vector<Locator>& locators_;
    std::vector<std::string>& warnings_;
    int& budget_;
};

std::string metadataKey(const std::string& column) {
    std::string k = QString::fromStdString(column).trimmed().toLower().toStdString();
    for (const char* known : {"scene", "shot", "take", "tape", "camera", "description", "comment"})
        if (k == known || k == std::string(known) + "s") return known;
    return QString::fromStdString(column).trimmed().toStdString();
}

}  // namespace

AvidBinImport importAvidBin(Project& p, const std::string& path, const MediaProber& probe) {
    AvidBinImport res;
    AvidBin bin;
    std::string err;
    if (!readAvidBin(path, bin, &err)) {
        res.error = "Not a Media Composer bin Montage can read: " + err;
        return res;
    }
    const QFileInfo info(QString::fromStdString(path));
    res.bin = info.completeBaseName().toStdString();
    if (std::find(p.bins.begin(), p.bins.end(), res.bin) == p.bins.end()) p.bins.push_back(res.bin);
    const std::string binDir = info.absolutePath().toStdString();
    AvidMediaIndex avidMedia(binDir);
    if (!bin.issues.empty()) res.warnings.push_back("Parts of the bin could not be read and were left out");

    // The file a source leads to: Media Composer's media for it, else what that was made from; "" if none is known.
    // Where it is not found, the path it was at (to relink), and `found` false.
    std::map<std::string, std::pair<std::string, bool>> located;  // file mob -> path, found
    auto locate = [&](const Source& s, double& seconds, bool& found, int* channel = nullptr) -> std::string {
        found = false;
        if (channel) *channel = -1;
        if (s.file) {
            auto [it, fresh] = located.try_emplace(idKey(s.file->mobId));
            if (fresh) {
                const AvidMediaLocator* l = s.file->descriptor ? fileLocator(*s.file->descriptor) : nullptr;
                bool exists = false;
                std::string at = l ? locatorPath(*l, &exists) : std::string();
                if (!exists && !at.empty()) {
                    const std::string beside = findInterchangeFile(at, binDir);
                    exists = QFileInfo(QString::fromStdString(beside)).isFile();
                    if (exists) at = beside;
                }
                if (!exists)
                    if (const std::string byId = avidMedia.find(idKey(s.file->mobId), at); !byId.empty()) at = byId, exists = true;
                it->second = {at, exists};
            }
            if (it->second.second || !s.original) {
                seconds = s.seconds;
                found = it->second.second;
                if (channel) *channel = s.channel;
                return it->second.first;
            }
        }
        if (s.original && s.original->descriptor)
            if (const AvidMediaLocator* l = fileLocator(*s.original->descriptor)) {
                seconds = s.originalSeconds;
                if (channel) *channel = s.originalChannel;
                return locatorPath(*l, &found);
            }
        return {};
    };
    // What a media item stands for: its file where it was found; else the mob that names the file (two master clips
    // of one name are two clips, and a master clip's channels in one file one); else the master clip and kind.
    auto mediaKey = [&](const Source& s, const std::string& file, bool found, const std::string& kind) {
        if (found) return file;
        if (s.file) {
            const auto it = located.find(idKey(s.file->mobId));
            if (!s.original || (it != located.end() && it->second.second)) return "mob:" + idKey(s.file->mobId);
        }
        if (s.original) return "mob:" + idKey(s.original->mobId);
        if (s.master) return "master:" + idKey(s.master->mobId) + kind;
        return "path:" + file;
    };
    auto kindOf = [](const std::string& k) { return k == "picture" ? 0 : k == "sound" ? 1 : -1; };

    // Master clips: a media item for each file they play (one for picture and sound in one file), in a bin named
    // after the .avb.
    std::map<std::string, Id> byKey;       // mediaKey -> item
    std::map<std::string, Id> masterMedia;  // master mob -> its first item
    auto addMedia = [&](const std::string& key, const std::string& file, bool found, const std::string& name, bool video, bool audio,
                        double seconds, double rate) -> Id {
        if (auto it = byKey.find(key); it != byKey.end()) {
            if (MediaItem* m = p.findMedia(it->second); m && !found) {
                m->hasVideo |= video;
                m->hasAudio |= audio;
            }
            return it->second;
        }
        for (const MediaItem& m : p.media)
            if (found && m.path == file) return byKey[key] = m.id;
        MediaItem m;
        m.id = p.newId();
        m.name = name;
        bool probed = false;
        if (found && probe) {
            MediaItem pm = m;
            if (probe(file, pm)) {
                pm.id = m.id;
                pm.name = name;
                m = pm;
                probed = true;
            }
        }
        if (!probed) {
            m.path = file;
            m.kind = video ? MediaKind::Video : MediaKind::Audio;
            m.hasVideo = video;
            m.hasAudio = audio;
            m.duration = seconds;
            m.fps = rationalRate(rate);
            res.offline.push_back(file.empty() ? name : file);
        }
        m.bin = res.bin;
        p.media.push_back(m);
        res.media.push_back(m.id);
        return byKey[key] = m.id;
    };
    auto applyColumns = [&](MediaItem& m, const AvidMob& mob, const std::string& tape) {
        if (const AvidAttribute* user = findAvidAttribute(mob.attributes, "_USER"))
            for (const AvidAttribute& a : user->children) {
                const std::string v = a.kind == AvidAttribute::Kind::Int ? std::to_string(a.intValue) : a.text;
                if (!a.name.empty() && !v.empty()) m.metadata[metadataKey(a.name)] = v;
            }
        if (!tape.empty() && !m.metadata.count("tape")) m.metadata["tape"] = tape;
        if (m.created.empty() && mob.creationTime && *mob.creationTime > 0)
            m.created = QDateTime::fromSecsSinceEpoch(*mob.creationTime, Qt::UTC).toString(Qt::ISODate).toStdString();
    };
    for (const AvidMob& mob : bin.mobs) {
        if (mob.mobType != "MasterMob" || mob.usageCode != 0) continue;
        const double length = mob.editRate > 0 ? double(mob.length) / mob.editRate : 0;
        std::vector<Id> items;
        std::string tape;
        int sounds = 0;
        for (const AvidTrack& t : mob.tracks) {
            if (!t.component || !t.index) continue;
            const int kind = kindOf(t.component->mediaKind);
            if (kind < 0) continue;
            Source s;
            follow(bin, mob.mobId, *t.index, t.component->mediaKind, 0, t.component->editRate, s);
            if (!s.tape.empty()) tape = s.tape;
            double seconds = 0;
            bool found = false;
            const std::string file = locate(s, seconds, found);
            const std::string name = items.empty() || kind == 0 ? mob.name : mob.name + " A" + std::to_string(sounds + 1);
            if (kind == 1) ++sounds;
            const Id id = addMedia(mediaKey(s, file, found, t.component->mediaKind), file, found, name, kind == 0, kind == 1, length,
                                   t.component->editRate);
            if (std::find(items.begin(), items.end(), id) == items.end()) items.push_back(id);
        }
        for (const Id id : items)
            if (MediaItem* m = p.findMedia(id)) applyColumns(*m, mob, tape);
        if (!items.empty()) masterMedia[idKey(mob.mobId)] = items.front();
    }
    // Subclips: a stretch of their master clip.
    for (const AvidMob& mob : bin.mobs) {
        if (mob.usageCode == 4 || mob.usageCode == 5) {
            if (mob.usageCode == 4) res.warnings.push_back("Group clip \"" + mob.name + "\" was left out");
            continue;
        }
        if (mob.usageCode != 2) continue;
        for (const AvidTrack& t : mob.tracks) {
            if (!t.component || !t.index || kindOf(t.component->mediaKind) < 0) continue;
            // Followed to the master clip's file like a sequence's clip, so its stretch is measured in that file.
            Source s;
            follow(bin, mob.mobId, *t.index, t.component->mediaKind, 0, t.component->editRate, s);
            double seconds = 0;
            bool found = false;
            const std::string file = locate(s, seconds, found);
            Id parent = 0;
            if (const auto it = byKey.find(mediaKey(s, file, found, t.component->mediaKind)); it != byKey.end()) parent = it->second;
            else if (s.master)
                if (const auto m = masterMedia.find(idKey(s.master->mobId)); m != masterMedia.end()) parent = m->second;
            const MediaItem* pm = parent ? p.findMedia(parent) : nullptr;
            if (!pm) continue;
            MediaItem m = *pm;
            m.id = p.newId();
            m.name = mob.name;
            m.subclipOf = pm->id;
            m.subclipIn = seconds;
            m.subclipOut = m.subclipIn + (mob.editRate > 0 ? double(mob.length) / mob.editRate : 0);
            m.metadata.clear();
            applyColumns(m, mob, {});
            m.bin = res.bin;
            p.media.push_back(m);
            res.media.push_back(m.id);
            break;
        }
    }

    // Sequences, as AAF import builds them; one another plays (a nested sequence) once, when first met.
    std::map<std::string, Id> built, nestedMedia;
    std::set<std::string> building;
    std::function<Id(const AvidMob&)> build = [&](const AvidMob& comp) -> Id {
        const std::string key = idKey(comp.mobId);
        if (const auto it = built.find(key); it != built.end()) return it->second;
        if (building.count(key) || building.size() > 32) return 0;
        building.insert(key);
        double editRate = comp.editRate;
        for (const AvidTrack& t : comp.tracks)
            if (t.component && t.component->mediaKind == "picture" && t.component->editRate > 0) {
                editRate = t.component->editRate;
                break;
            }
        const Rational fps = rationalRate(editRate);
        TimelineBuilder b(p, comp.name, fps, probe);
        const double f = fps.toDouble();
        std::vector<const AvidTrack*> tracks;
        for (const AvidTrack& t : comp.tracks)
            if (t.component && kindOf(t.component->mediaKind) >= 0) tracks.push_back(&t);
        std::stable_sort(tracks.begin(), tracks.end(), [&](const AvidTrack* a, const AvidTrack* c) {
            const int ka = kindOf(a->component->mediaKind), kc = kindOf(c->component->mediaKind);
            return ka != kc ? ka < kc : a->index.value_or(0) < c->index.value_or(0);
        });
        int videoTrack = 0, audioTrack = 0;
        std::set<std::pair<FrameTime, std::string>> marked;
        int budget = 200000;
        for (const AvidTrack* t : tracks) {
            const AvidComponent& top = *t->component;
            const bool pic = top.mediaKind == "picture";
            const TrackKind kind = pic ? TrackKind::Video : TrackKind::Audio;
            const int index = pic ? videoTrack++ : audioTrack++;
            b.track(kind, index);
            std::vector<Piece> pieces;
            std::vector<Locator> locators;
            Walker(pieces, locators, res.warnings, budget).walk(&top, 0);
            const double toFrames = top.editRate > 0 && !(rationalRate(top.editRate) == fps) ? f / top.editRate : 1;
            for (const Locator& l : locators) {
                Marker m;
                m.t = FrameTime(std::llround(l.at * toFrames));
                m.name = l.comment;
                m.color = labelForColourName(l.colour);
                if (marked.insert({m.t, m.name}).second) b.sequence().markers.push_back(m);
            }
            struct Placed {
                Id id;
                double pos, len;
            };
            std::vector<Placed> placed;
            for (const Piece& pc : pieces) {
                if (pc.kind != Piece::Clip || pc.len <= 0) continue;
                const FrameTime start = FrameTime(std::llround(pc.pos * toFrames));
                const FrameTime end = FrameTime(std::llround((pc.pos + pc.len) * toFrames));
                if (end <= start) continue;
                Id media = 0;
                double sourceIn = 0;
                std::string name;
                int channel = -1;
                const AvidMob* target = bin.findMob(pc.mob);
                if (target && target->mobType == "CompositionMob" && target->usageCode == 0) {
                    // A nested sequence, played whole.
                    const Id seq = build(*target);
                    if (!seq) {
                        res.warnings.push_back("A nested sequence that contains itself was left out");
                        continue;
                    }
                    auto [it, added] = nestedMedia.try_emplace(idKey(target->mobId), 0);
                    if (added) {
                        const Sequence* ns = p.findSequence(seq);
                        MediaItem m;
                        m.id = p.newId();
                        m.kind = MediaKind::Sequence;
                        m.name = target->name;
                        m.sequenceId = seq;
                        m.hasVideo = m.hasAudio = true;
                        m.bin = res.bin;
                        if (ns) {
                            m.width = ns->width;
                            m.height = ns->height;
                            m.fps = ns->fps;
                            m.duration = double(ns->duration()) / std::max(1e-9, ns->fpsValue());
                        }
                        p.media.push_back(m);
                        it->second = m.id;
                    }
                    media = it->second;
                    const double rate = pc.rate > 0 ? pc.rate : top.editRate;
                    sourceIn = rate > 0 ? pc.start / rate * f : 0;
                    name = target->name;
                } else {
                    Source s;
                    follow(bin, pc.mob, pc.track, top.mediaKind, pc.start, pc.rate > 0 ? pc.rate : top.editRate, s);
                    double seconds = 0;
                    bool found = false;
                    const std::string file = locate(s, seconds, found, &channel);
                    name = s.master ? s.master->name : target ? target->name : std::string();
                    const double length = top.editRate > 0 ? seconds + pc.len * pc.speed / top.editRate : seconds;
                    media = addMedia(mediaKey(s, file, found, top.mediaKind), file, found, name, pic, !pic, length, top.editRate);
                    sourceIn = seconds * f;
                }
                Clip* c = b.addClip(kind, index, media, start, end - start, sourceIn, name);
                if (!c) continue;
                if (std::fabs(pc.speed - 1) > 1e-9) c->speed = pc.speed;
                c->reverse = pc.reverse;
                if (!pic && channel >= 0)
                    if (const MediaItem* mi = p.findMedia(media); mi && mi->channels > 1 && channel < mi->channels) c->channels = {channel};
                placed.push_back({c->id, pc.pos, pc.len});
            }
            // Transitions between the clips either side of each (or fades, beside a gap): the clips meet at its cut.
            for (const Piece& pc : pieces) {
                if (pc.kind != Piece::Transition || pc.len <= 0) continue;
                const Placed* a = nullptr;
                const Placed* bb = nullptr;
                for (const Placed& x : placed) {
                    if (std::fabs(x.pos + x.len - (pc.pos + pc.len)) < 0.5) a = &x;
                    if (std::fabs(x.pos - pc.pos) < 0.5) bb = &x;
                }
                const FrameTime length = FrameTime(std::llround(pc.len * toFrames));
                if (!a && bb) {
                    b.addTransition(kind, index, 0, bb->id, length, {});
                    continue;
                }
                if (a && !bb) {
                    b.addTransition(kind, index, a->id, 0, length, {});
                    continue;
                }
                if (!a || !bb) continue;
                Clip* ca = edit::clipById(b.sequence(), a->id);
                Clip* cb = edit::clipById(b.sequence(), bb->id);
                if (!ca || !cb) continue;
                const FrameTime cut = FrameTime(std::llround((pc.pos + std::clamp(pc.cutPoint, 0.0, pc.len)) * toFrames));
                const FrameTime trimB = cut - cb->start;
                if (cut <= ca->start || trimB >= cb->duration) continue;
                ca->duration = cut - ca->start;
                cb->sourceIn += double(trimB) * cb->speed;
                cb->start = cut;
                cb->duration -= trimB;
                b.addTransition(kind, index, ca->id, cb->id, length, {});
            }
        }
        ImportResult one = b.finish();
        building.erase(key);
        built[key] = one.sequence;
        res.clips += one.clips;
        res.offline.insert(res.offline.end(), one.offline.begin(), one.offline.end());
        res.warnings.insert(res.warnings.end(), one.warnings.begin(), one.warnings.end());
        return one.sequence;
    };
    for (const AvidBinItem& item : bin.items) {
        if (item.mob < 0 || size_t(item.mob) >= bin.mobs.size()) continue;
        const AvidMob& mob = bin.mobs[size_t(item.mob)];
        if (mob.mobType != "CompositionMob" || mob.usageCode != 0) continue;
        if (const Id seq = build(mob); seq && std::find(res.sequences.begin(), res.sequences.end(), seq) == res.sequences.end())
            res.sequences.push_back(seq);
    }
    if (!res.sequences.empty()) p.activeSequence = res.sequences.front();
    res.ok = !res.media.empty() || !res.sequences.empty();
    if (!res.ok) res.error = "The bin holds no clips or sequences Montage can bring in";
    std::sort(res.warnings.begin(), res.warnings.end());
    res.warnings.erase(std::unique(res.warnings.begin(), res.warnings.end()), res.warnings.end());
    std::sort(res.offline.begin(), res.offline.end());
    res.offline.erase(std::unique(res.offline.begin(), res.offline.end()), res.offline.end());
    return res;
}

}  // namespace montage
