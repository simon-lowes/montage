#include "AafImport.h"

#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <set>

#include "Aaf.h"
#include "Cfb.h"
#include "EditOps.h"
#include "ImportBuilder.h"

namespace montage {

namespace {

// ---- The object model, read back ----------------------------------------------

uint16_t rd16(const std::string& d, size_t at) { return at + 2 <= d.size() ? uint16_t(uint8_t(d[at]) | uint8_t(d[at + 1]) << 8) : 0; }
uint32_t rd32(const std::string& d, size_t at) { return at + 4 <= d.size() ? uint32_t(rd16(d, at)) | uint32_t(rd16(d, at + 2)) << 16 : 0; }
int64_t rd64(const std::string& d, size_t at) { return at + 8 <= d.size() ? int64_t(uint64_t(rd32(d, at)) | uint64_t(rd32(d, at + 4)) << 32) : 0; }

std::string utf16z(const std::string& d) {
    std::u16string s;
    for (size_t i = 0; i + 1 < d.size(); i += 2) {
        const char16_t c = char16_t(rd16(d, i));
        if (!c) break;
        s.push_back(c);
    }
    return QString::fromStdU16String(s).toStdString();
}

struct Prop {
    uint16_t format = 0;
    std::string data;
};

// One object: its storage, class and properties.
struct Obj {
    const CfbEntry* entry = nullptr;
    std::map<uint16_t, Prop> props;

    explicit operator bool() const { return entry != nullptr; }
    bool is(const char* classAuid) const {
        const std::string want = aafAuid(classAuid);
        return entry && std::memcmp(entry->clsid.data(), want.data(), 16) == 0;
    }
    bool has(uint16_t pid) const { return props.count(pid) > 0; }
    const std::string& raw(uint16_t pid) const {
        static const std::string none;
        const auto it = props.find(pid);
        return it == props.end() ? none : it->second.data;
    }
    int64_t i64(uint16_t pid, int64_t def = 0) const { return has(pid) ? rd64(raw(pid), 0) : def; }
    uint32_t u32(uint16_t pid, uint32_t def = 0) const { return has(pid) ? rd32(raw(pid), 0) : def; }
    uint16_t u16(uint16_t pid, uint16_t def = 0) const { return has(pid) ? rd16(raw(pid), 0) : def; }
    std::string text(uint16_t pid) const { return utf16z(raw(pid)); }
    Rational rational(uint16_t pid, Rational def = {0, 1}) const {
        if (raw(pid).size() < 8) return def;
        return {int64_t(int32_t(rd32(raw(pid), 0))), int64_t(int32_t(rd32(raw(pid), 4)))};
    }
    // A weak reference's key (the referenced object's identification, e.g. a definition's AUID).
    std::string weakKey(uint16_t pid) const {
        const std::string& d = raw(pid);
        if (d.size() < 5) return {};
        const size_t n = uint8_t(d[4]);
        return d.size() >= 5 + n ? d.substr(5, n) : std::string();
    }
};

Obj object(const CfbEntry* e) {
    Obj o;
    if (!e || !e->storage) return o;
    const CfbEntry* ps = e->find("properties");
    if (!ps || ps->data.size() < 4 || uint8_t(ps->data[0]) != 0x4c) return o;  // little-endian property streams only
    const std::string& d = ps->data;
    const uint16_t count = rd16(d, 2);
    size_t head = 4, data = 4 + size_t(count) * 6;
    for (uint16_t i = 0; i < count; ++i, head += 6) {
        const uint16_t pid = rd16(d, head), format = rd16(d, head + 2), size = rd16(d, head + 4);
        if (data + size > d.size()) return Obj{};
        o.props[pid] = {format, d.substr(data, size)};
        data += size;
    }
    o.entry = e;
    return o;
}

// Strong references: a child storage, or a vector / set of them through the index stream.
Obj strong(const Obj& o, uint16_t pid) {
    if (!o.has(pid)) return {};
    return object(o.entry->find(utf16z(o.raw(pid))));
}
std::vector<Obj> strongList(const Obj& o, uint16_t pid) {
    std::vector<Obj> out;
    if (!o.has(pid)) return out;
    const Prop& p = o.props.at(pid);
    const std::string name = utf16z(p.data);
    const CfbEntry* index = o.entry->find(name + " index");
    if (!index) return out;
    const std::string& ix = index->data;
    const uint32_t count = rd32(ix, 0);
    const bool set = p.format == 0x3A;
    size_t at = 12, keySize = 0;
    if (set) {
        keySize = ix.size() > 14 ? uint8_t(ix[14]) : 0;
        at = 15;
    }
    for (uint32_t i = 0; i < count && at + 4 <= ix.size(); ++i) {
        const uint32_t key = rd32(ix, at);
        at += set ? 8 + keySize : 4;
        char buf[24];
        std::snprintf(buf, sizeof buf, "{%x}", key);
        const CfbEntry* child = o.entry->find(name + buf);
        if (!child) {
            std::snprintf(buf, sizeof buf, "{%X}", key);
            child = o.entry->find(name + buf);
        }
        if (Obj c = object(child)) out.push_back(std::move(c));
    }
    return out;
}

// ---- Classes, properties and definitions (the AAF object model's IDs) ---------

const char* const kCompositionMob = "0d010101-0101-3500-060e-2b3402060101";
const char* const kMasterMob = "0d010101-0101-3600-060e-2b3402060101";
const char* const kSourceMob = "0d010101-0101-3700-060e-2b3402060101";
const char* const kEventMobSlot = "0d010101-0101-3900-060e-2b3402060101";
const char* const kSequence = "0d010101-0101-0f00-060e-2b3402060101";
const char* const kSourceClip = "0d010101-0101-1100-060e-2b3402060101";
const char* const kFiller = "0d010101-0101-0900-060e-2b3402060101";
const char* const kTransition = "0d010101-0101-1700-060e-2b3402060101";
const char* const kOperationGroup = "0d010101-0101-0a00-060e-2b3402060101";
const char* const kSelector = "0d010101-0101-0e00-060e-2b3402060101";
const char* const kEssenceGroup = "0d010101-0101-0500-060e-2b3402060101";
const char* const kNestedScope = "0d010101-0101-0b00-060e-2b3402060101";
const char* const kTimecode = "0d010101-0101-1400-060e-2b3402060101";
const char* const kConstantValue = "0d010101-0101-3d00-060e-2b3402060101";
const char* const kVaryingValue = "0d010101-0101-3e00-060e-2b3402060101";
const char* const kMultipleDescriptor = "0d010101-0101-4400-060e-2b3402060101";

bool isAuid(const std::string& key, std::initializer_list<const char*> ids) {
    return std::any_of(ids.begin(), ids.end(), [&](const char* id) { return key == aafAuid(id); });
}
bool picture(const std::string& dataDef) {
    return isAuid(dataDef, {"01030202-0100-0000-060e-2b3404010101", "6f3c8ce1-6cef-11d2-807d-006008143e6f", "05cba731-1daa-11d3-80ad-006008143e6f"});
}
bool sound(const std::string& dataDef) {
    return isAuid(dataDef, {"01030202-0200-0000-060e-2b3404010101", "78e1ebe1-6cef-11d2-807d-006008143e6f"});
}
const char* const kUsageTopLevel = "0d010102-0101-0700-060e-2b3404010101";
const char* const kAmplitude = "e4962321-2267-11d3-8a4c-0050040ef7d2";
const char* const kSpeedRatio = "72559a80-24d7-11d3-8a50-0050040ef7d2";

// A parameter's value as a number: a constant (or a varying value's first point) holding a Rational or an integer.
std::optional<double> parameterValue(const Obj& group, const char* parameterDef) {
    for (const Obj& prm : strongList(group, 0x0b03)) {
        if (prm.raw(0x4c01) != aafAuid(parameterDef)) continue;
        std::string v;
        if (prm.is(kConstantValue)) v = prm.raw(0x4d01);
        else if (prm.is(kVaryingValue))
            if (const std::vector<Obj> pts = strongList(prm, 0x4e02); !pts.empty()) v = pts.front().raw(0x1a02);
        // An Indirect value: byte order, the type's AUID, then the value.
        if (v.size() < 17) continue;
        const std::string value = v.substr(17);
        if (value.size() >= 8) {
            const int32_t num = int32_t(rd32(value, 0)), den = int32_t(rd32(value, 4));
            if (den) return double(num) / double(den);
        }
        if (value.size() == 4) return double(int32_t(rd32(value, 0)));
    }
    return std::nullopt;
}

// ---- Mobs ------------------------------------------------------------------------

struct Slot {
    uint32_t id = 0;
    std::string name;
    Rational rate{25, 1};
    Obj segment;
    bool event = false;
};
struct Mob {
    Obj obj;
    std::string id, name;
    std::vector<Slot> mobSlots;
    const Slot* slot(uint32_t id) const {
        for (const Slot& s : mobSlots)
            if (s.id == id) return &s;
        return nullptr;
    }
};

double rateOf(Rational r) { return r.den ? double(r.num) / double(r.den) : 0; }

// Where a source clip leads: the file (its locator), its name, the time into it and which of its sound slots.
struct Source {
    std::string url, name;
    double seconds = 0;   // offset into the file
    double length = 0;    // the file's length, when known
    int channel = -1;     // the file's sound slot (channel) played
    bool found = false;
};

class Reader {
public:
    Reader(const CfbEntry& root, std::vector<std::string>& warnings) : warnings_(warnings) {
        Obj top = object(&root);
        Obj header = strong(top, 0x0002);
        if (!header) header = object(root.find("Header-2"));
        Obj content = strong(header, 0x3b03);
        for (Obj& m : strongList(content, 0x1901)) {
            Mob mob;
            mob.id = m.raw(0x4401);
            mob.name = m.text(0x4402);
            for (Obj& s : strongList(m, 0x4403)) {
                Slot slot;
                slot.id = s.u32(0x4801);
                slot.name = s.text(0x4802);
                slot.event = s.is(kEventMobSlot);
                slot.rate = s.rational(slot.event ? 0x4901 : 0x4b01, {25, 1});
                if (!slot.rate.num || !slot.rate.den) slot.rate = {25, 1};
                slot.segment = strong(s, 0x4803);
                mob.mobSlots.push_back(std::move(slot));
            }
            mob.obj = std::move(m);
            mobs_[mob.id] = std::move(mob);
        }
    }

    std::vector<const Mob*> compositions() const {
        std::vector<const Mob*> all, top;
        std::set<std::string> referenced;
        for (const auto& [id, m] : mobs_) {
            if (!m.obj.is(kCompositionMob)) continue;
            all.push_back(&m);
            if (m.obj.raw(0x4408) == aafAuid(kUsageTopLevel)) top.push_back(&m);
            for (const Slot& s : m.mobSlots) collectRefs(s.segment, referenced);
        }
        if (!top.empty()) return top;
        std::vector<const Mob*> roots;  // compositions no other composition uses
        for (const Mob* m : all)
            if (!referenced.count(m->id)) roots.push_back(m);
        return roots.empty() ? all : roots;
    }

    // Follows a reference to (mob, slot) at `start` units of the caller's rate to the file it ends in.
    Source resolve(const std::string& mobId, uint32_t slotId, double start, Rational callerRate, int depth = 0) const {
        Source out;
        const auto it = mobs_.find(mobId);
        if (it == mobs_.end() || depth > 16) return out;
        const Mob& m = it->second;
        out.name = m.name;
        const Slot* slot = m.slot(slotId);
        const Rational rate = slot ? slot->rate : callerRate;
        const double here = start * rateOf(rate) / std::max(1e-9, rateOf(callerRate));  // in this slot's units
        // A file mob: its descriptor's locator names the file.
        if (m.obj.is(kSourceMob)) {
            const Obj desc = strong(m.obj, 0x4701);
            std::string url = locator(desc);
            if (url.empty() && desc.is(kMultipleDescriptor))
                for (const Obj& d : strongList(desc, 0x3f01))
                    if (url.empty()) url = locator(d);
            if (desc.has(0x3002) && desc.has(0x3001)) {
                const double r = rateOf(desc.rational(0x3001));
                if (r > 0) out.length = double(desc.i64(0x3002)) / r;
            }
            // Which of the file's sound slots this is: its channel.
            int index = 0;
            for (const Slot& s : m.mobSlots) {
                if (!s.segment || !sound(s.segment.weakKey(0x0201))) continue;
                if (s.id == slotId) out.channel = index;
                ++index;
            }
            if (index <= 1) out.channel = -1;
            if (!url.empty()) {
                out.url = url;
                out.seconds = here / std::max(1e-9, rateOf(rate));
                out.found = true;
                return out;
            }
        }
        // Otherwise down the slot's source clip (a master mob to its file, a file mob to its tape or import source).
        if (slot && slot->segment) {
            Obj seg = slot->segment;
            if (seg.is(kSequence))
                if (const std::vector<Obj> parts = strongList(seg, 0x1001); !parts.empty()) seg = parts.front();
            if (seg.is(kEssenceGroup))
                if (const std::vector<Obj> choices = strongList(seg, 0x0501); !choices.empty()) seg = choices.front();
            if (seg.is(kSourceClip) && seg.raw(0x1101) != std::string(32, '\0') && seg.has(0x1101)) {
                Source deeper = resolve(seg.raw(0x1101), seg.u32(0x1102), here + double(seg.i64(0x1201)), rate, depth + 1);
                if (deeper.found || !deeper.url.empty()) {
                    if (!m.obj.is(kSourceMob) && !m.name.empty()) deeper.name = m.name;  // the master mob's name is the clip's
                    if (out.channel >= 0 && deeper.channel < 0) deeper.channel = out.channel;
                    return deeper;
                }
                if (out.length <= 0) out.length = deeper.length;
            }
        }
        return out;
    }

    const std::map<std::string, Mob>& mobs() const { return mobs_; }
    void warn(const std::string& w) {
        if (std::find(warnings_.begin(), warnings_.end(), w) == warnings_.end()) warnings_.push_back(w);
    }

private:
    static std::string locator(const Obj& desc) {
        for (const Obj& l : strongList(desc, 0x2f01))
            if (l.has(0x4001)) return l.text(0x4001);
        return {};
    }
    void collectRefs(const Obj& seg, std::set<std::string>& out) const {
        if (!seg) return;
        if (seg.is(kSourceClip)) out.insert(seg.raw(0x1101));
        for (uint16_t pid : {uint16_t(0x1001), uint16_t(0x0b02), uint16_t(0x0c01), uint16_t(0x0501), uint16_t(0x0f02)})
            for (const Obj& c : strongList(seg, pid)) collectRefs(c, out);
        for (uint16_t pid : {uint16_t(0x0f01), uint16_t(0x1801), uint16_t(0x4803)})
            collectRefs(strong(seg, pid), out);
    }

    std::map<std::string, Mob> mobs_;
    std::vector<std::string>& warnings_;
};

// A placed piece of a slot, in the slot's edit units.
struct Piece {
    enum Kind { Clip, Transition } kind = Clip;
    double pos = 0, len = 0;
    std::string mob;
    uint32_t slot = 0;
    double start = 0;     // into the referenced mob slot, in this slot's units
    double gainDb = 0;
    double speed = 1;
    double cutPoint = 0;  // transitions: where the cut falls, from its start
};

// The pieces of a segment placed from `pos`; returns the length it takes.
double place(Reader& r, const Obj& seg, double pos, std::vector<Piece>& out, double gainDb = 0, double speed = 1) {
    if (!seg) return 0;
    const double len = double(seg.i64(0x0202));
    if (seg.is(kSequence)) {
        double cursor = pos;
        for (const Obj& c : strongList(seg, 0x1001)) {
            if (c.is(kTransition)) {
                const double l = double(c.i64(0x0202));
                cursor -= l;  // a transition overlaps the pieces on either side
                Piece t;
                t.kind = Piece::Transition;
                t.pos = cursor;
                t.len = l;
                t.cutPoint = double(c.i64(0x1802, int64_t(l / 2)));
                out.push_back(t);
                continue;
            }
            cursor += place(r, c, cursor, out, gainDb, speed);
        }
        return cursor - pos;
    }
    if (seg.is(kSourceClip)) {
        if (seg.has(0x1101) && seg.raw(0x1101) != std::string(32, '\0')) {
            Piece p;
            p.pos = pos;
            p.len = len;
            p.mob = seg.raw(0x1101);
            p.slot = seg.u32(0x1102);
            p.start = double(seg.i64(0x1201));
            p.gainDb = gainDb;
            p.speed = speed;
            out.push_back(p);
        }
        return len;
    }
    if (seg.is(kFiller) || seg.is(kTimecode)) return len;
    if (seg.is(kOperationGroup)) {
        const std::vector<Obj> inputs = strongList(seg, 0x0b02);
        double g = gainDb, sp = speed;
        if (const auto amp = parameterValue(seg, kAmplitude); amp && *amp > 0) g += 20 * std::log10(*amp);
        else if (const auto ratio = parameterValue(seg, kSpeedRatio); ratio && *ratio > 0) sp *= *ratio;
        else if (!inputs.empty()) r.warn("Some effects in the AAF could not be carried over; their clips came in without them");
        if (!inputs.empty()) {
            std::vector<Piece> inner;
            place(r, inputs.front(), pos, inner, g, sp);
            for (Piece& p : inner)
                if (p.kind == Piece::Clip) p.len = std::min(p.len, len);  // the effect's length is what plays
            out.insert(out.end(), inner.begin(), inner.end());
        }
        return len;
    }
    if (seg.is(kSelector)) {
        place(r, strong(seg, 0x0f01), pos, out, gainDb, speed);
        return len;
    }
    if (seg.is(kEssenceGroup)) {
        if (const std::vector<Obj> choices = strongList(seg, 0x0501); !choices.empty()) place(r, choices.front(), pos, out, gainDb, speed);
        return len;
    }
    if (seg.is(kNestedScope)) {
        if (const std::vector<Obj> scopes = strongList(seg, 0x0c01); !scopes.empty()) place(r, scopes.back(), pos, out, gainDb, speed);
        return len;
    }
    r.warn("Parts of the AAF Montage does not read came in as gaps");
    return len;
}

// The file a URL or path names, if it can be found: where it says, else beside the AAF or in a folder next to it.
std::string findFile(const std::string& url, const std::string& aafDir) {
    QString path = QString::fromStdString(url);
    if (path.startsWith("file:", Qt::CaseInsensitive)) {
        const QUrl u(path);
        path = u.isLocalFile() ? u.toLocalFile() : QUrl::fromPercentEncoding(path.mid(5).toUtf8());
        // Windows paths written as file:///C:/...
        if (path.size() > 2 && path[0] == '/' && path[2] == ':') path = path.mid(1);
    }
    if (!path.isEmpty() && QFileInfo(path).isFile()) return QFileInfo(path).absoluteFilePath().toStdString();
    const QString name = QFileInfo(path.replace('\\', '/')).fileName();
    if (name.isEmpty()) return path.toStdString();
    const QDir dir(QString::fromStdString(aafDir));
    if (QFileInfo(dir.filePath(name)).isFile()) return dir.filePath(name).toStdString();
    for (const QFileInfo& sub : dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
        if (QFileInfo(sub.filePath() + '/' + name).isFile()) return (sub.filePath() + '/' + name).toStdString();
    return path.toStdString();
}

}  // namespace

ImportResult importAaf(Project& p, const std::string& path, const MediaProber& probe) {
    ImportResult res;
    CfbEntry root;
    std::string err;
    if (!readCompoundFile(path, root, &err)) {
        res.error = "Not an AAF file Montage can read: " + err;
        return res;
    }
    std::vector<std::string> warnings;
    Reader reader(root, warnings);
    const std::vector<const Mob*> comps = reader.compositions();
    if (comps.empty()) {
        res.error = "The AAF holds no sequence (composition)";
        return res;
    }
    const std::string aafDir = QFileInfo(QString::fromStdString(path)).absolutePath().toStdString();
    for (const Mob* comp : comps) {
        // The sequence's rate: its first picture slot's (else its first slot's, when it is a video rate).
        Rational fps{0, 1};
        for (const Slot& s : comp->mobSlots)
            if (!fps.num && s.segment && picture(s.segment.weakKey(0x0201))) fps = s.rate;
        for (const Slot& s : comp->mobSlots)
            if (!fps.num && !s.event && rateOf(s.rate) > 0 && rateOf(s.rate) <= 120) fps = s.rate;
        if (!fps.num) fps = {25, 1};
        TimelineBuilder b(p, comp->name, fps, probe);
        const double f = rateOf(fps);
        int videoTrack = 0, audioTrack = 0;
        for (const Slot& s : comp->mobSlots) {
            if (!s.segment) continue;
            const std::string dataDef = s.segment.weakKey(0x0201);
            // Markers (Media Composer's locators): events, with a position and a comment, in an event or data slot.
            {
                std::vector<Obj> events = strongList(s.segment, 0x1001);
                if (events.empty()) events.push_back(s.segment);
                bool any = false;
                for (const Obj& e : events) {
                    if (!e.has(0x0601)) continue;
                    Marker m;
                    m.t = FrameTime(std::llround(double(e.i64(0x0601)) * f / rateOf(s.rate)));
                    m.name = e.text(0x0602);
                    b.sequence().markers.push_back(m);
                    any = true;
                }
                if (any || s.event) continue;
            }
            const bool pic = picture(dataDef), snd = sound(dataDef);
            if (!pic && !snd) continue;  // timecode, edgecode, data
            const TrackKind kind = pic ? TrackKind::Video : TrackKind::Audio;
            const int index = pic ? videoTrack++ : audioTrack++;
            Track& track = b.track(kind, index);
            if (!s.name.empty()) track.name = s.name;
            std::vector<Piece> pieces;
            place(reader, s.segment, 0, pieces);
            const double toFrames = f / rateOf(s.rate);
            // Clips first, then transitions between the clips either side of each.
            struct Placed {
                Id id;
                double pos, len;  // slot units
            };
            std::vector<Placed> placed;
            for (const Piece& pc : pieces) {
                if (pc.kind != Piece::Clip) continue;
                const Source src = reader.resolve(pc.mob, pc.slot, pc.start, s.rate);
                const std::string file = src.url.empty() ? std::string() : findFile(src.url, aafDir);
                const double seconds = std::max(src.length, src.seconds + pc.len * pc.speed / rateOf(s.rate));
                const Id media = b.media(file, src.name, pic, snd, seconds);
                const FrameTime start = FrameTime(std::llround(pc.pos * toFrames));
                const FrameTime end = FrameTime(std::llround((pc.pos + pc.len) * toFrames));
                Clip* c = b.addClip(kind, index, media, start, end - start, src.seconds * f, src.name);
                if (!c) continue;
                if (pc.speed != 1) c->speed = pc.speed;
                if (snd && std::fabs(pc.gainDb) > 1e-6) c->audio.params["gain_db"] = Param(pc.gainDb);
                if (snd && src.channel >= 0)
                    if (const MediaItem* mi = p.findMedia(media); mi && mi->channels > 1 && src.channel < mi->channels)
                        c->channels = {src.channel};
                placed.push_back({c->id, pc.pos, pc.len});
            }
            for (const Piece& pc : pieces) {
                if (pc.kind != Piece::Transition) continue;
                // The clips that end and begin across it.
                const Placed* a = nullptr;
                const Placed* bb = nullptr;
                for (const Placed& x : placed) {
                    if (std::fabs(x.pos + x.len - (pc.pos + pc.len)) < 0.5) a = &x;
                    if (std::fabs(x.pos - pc.pos) < 0.5) bb = &x;
                }
                if (!a || !bb) continue;
                // Montage's clips meet at the cut: the outgoing one ends there, the incoming one starts there.
                Clip* ca = edit::clipById(b.sequence(), a->id);
                Clip* cb = edit::clipById(b.sequence(), bb->id);
                if (!ca || !cb) continue;
                const FrameTime cut = FrameTime(std::llround((pc.pos + pc.cutPoint) * toFrames));
                const FrameTime trimB = cut - cb->start;
                if (cut <= ca->start || trimB >= cb->duration) continue;
                ca->duration = cut - ca->start;
                cb->sourceIn += double(trimB) * cb->speed;
                cb->start = cut;
                cb->duration -= trimB;
                b.addTransition(kind, index, ca->id, cb->id, FrameTime(std::llround(pc.len * toFrames)), {});
            }
        }
        ImportResult one = b.finish();
        if (!res.ok) {
            res = one;
        } else {
            res.clips += one.clips;
            res.offline.insert(res.offline.end(), one.offline.begin(), one.offline.end());
        }
    }
    p.activeSequence = res.sequence;  // the first composition
    res.warnings.insert(res.warnings.end(), warnings.begin(), warnings.end());
    std::sort(res.offline.begin(), res.offline.end());
    res.offline.erase(std::unique(res.offline.begin(), res.offline.end()), res.offline.end());
    return res;
}

}  // namespace montage
