#include "Aaf.h"

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <cstdio>
#include <random>

namespace montage {

namespace {

#include "AafMetaDictionary.inc"

void append16(std::string& s, uint16_t v) {
    s += char(v & 0xff);
    s += char(v >> 8);
}
void append32(std::string& s, uint32_t v) {
    for (int i = 0; i < 4; ++i) s += char((v >> (8 * i)) & 0xff);
}
void append64(std::string& s, uint64_t v) {
    for (int i = 0; i < 8; ++i) s += char((v >> (8 * i)) & 0xff);
}

std::string utf16z(const std::string& utf8) {
    std::string out;
    for (char16_t c : QString::fromStdString(utf8).toStdU16String()) append16(out, uint16_t(c));
    append16(out, 0);
    return out;
}

// pyaaf2's names for the storages and streams of a property: the property's name (squeezed in the middle to
// fit) and its id in hex.
std::string mangle(const std::string& name, uint16_t pid, int size) {
    char hex[8];
    std::snprintf(hex, sizeof hex, "%x", pid);
    const int max = size - int(std::string(hex).size()) - 2;
    std::string squeezed = name;
    if (int(name.size()) > max) {
        squeezed.clear();
        const int half = max / 2;
        for (int i = 0; i < max; ++i) squeezed += i < half ? name[size_t(i)] : i == half ? '-' : name[name.size() - size_t(max - i)];
    }
    return squeezed + "-" + hex;
}

std::string hexKey(size_t k) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "{%zx}", k);
    return buf;
}

CfbEntry stream(const std::string& name, std::string data) {
    CfbEntry e;
    e.name = name;
    e.data = std::move(data);
    return e;
}

std::array<uint8_t, 16> clsid(const std::string& auidBytes) {
    std::array<uint8_t, 16> c{};
    for (size_t i = 0; i < 16 && i < auidBytes.size(); ++i) c[i] = uint8_t(auidBytes[i]);
    return c;
}

// The MetaDictionary pyaaf2 writes (scripts/gen-aaf-metadict.py).
bool metaDictionary(CfbEntry& out) {
    const QByteArray raw = qUncompress(reinterpret_cast<const uchar*>(kAafMetaDictionary), int(sizeof kAafMetaDictionary));
    if (raw.isEmpty()) return false;
    const std::string b(raw.constData(), size_t(raw.size()));
    size_t at = 0;
    bool first = true;
    while (at + 3 <= b.size()) {
        const uint8_t type = uint8_t(b[at]);
        const size_t len = uint8_t(b[at + 1]) | size_t(uint8_t(b[at + 2])) << 8;
        at += 3;
        if (at + len > b.size()) return false;
        const std::string path = b.substr(at, len);
        at += len;
        // Down to the parent of this entry, by path.
        CfbEntry* parent = &out;
        std::string leaf = path;
        for (size_t slash; (slash = leaf.find('/')) != std::string::npos;) {
            parent = parent->find(leaf.substr(0, slash));
            if (!parent) return false;
            leaf = leaf.substr(slash + 1);
        }
        if (type == 1) {
            if (at + 16 > b.size()) return false;
            CfbEntry* e = &out;
            if (!first) {
                parent->children.push_back(CfbEntry{});
                e = &parent->children.back();
                e->name = leaf;
            }
            e->storage = true;
            for (size_t i = 0; i < 16; ++i) e->clsid[i] = uint8_t(b[at + i]);
            at += 16;
        } else {
            if (at + 4 > b.size()) return false;
            const size_t n = uint8_t(b[at]) | size_t(uint8_t(b[at + 1])) << 8 | size_t(uint8_t(b[at + 2])) << 16 |
                             size_t(uint8_t(b[at + 3])) << 24;
            at += 4;
            if (at + n > b.size()) return false;
            parent->children.push_back(stream(leaf, b.substr(at, n)));
            at += n;
        }
        first = false;
    }
    out.name = "MetaDictionary-1";
    return true;
}

}  // namespace

std::string aafAuid(const std::string& text) {
    std::string hex;
    for (char c : text)
        if (std::isxdigit(static_cast<unsigned char>(c))) hex += c;
    std::string b(16, '\0');
    if (hex.size() != 32) return b;
    for (size_t i = 0; i < 16; ++i) b[i] = char(std::stoi(hex.substr(i * 2, 2), nullptr, 16));
    // Data1 (4 bytes), Data2 and Data3 (2 each) are stored little-endian.
    std::swap(b[0], b[3]);
    std::swap(b[1], b[2]);
    std::swap(b[4], b[5]);
    std::swap(b[6], b[7]);
    return b;
}

std::string aafNewMobId() {
    // SMPTE UMID: the universal label, length 0x13, instance 0, then a random material number.
    static const unsigned char label[16] = {0x06, 0x0a, 0x2b, 0x34, 0x01, 0x01, 0x01, 0x05,
                                            0x01, 0x01, 0x0f, 0x20, 0x13, 0x00, 0x00, 0x00};
    std::string id(reinterpret_cast<const char*>(label), 16);
    static std::mt19937_64 rng(std::random_device{}());
    for (int i = 0; i < 2; ++i) append64(id, rng());
    return id;
}

AafObject::AafObject(const std::string& classAuidText) : classId_(aafAuid(classAuidText)) {}

AafObject::Prop& AafObject::prop(uint16_t pid, uint8_t format) {
    for (Prop& p : props_)
        if (p.pid == pid) {
            p.format = format;
            return p;
        }
    props_.push_back(Prop{});
    props_.back().pid = pid;
    props_.back().format = format;
    return props_.back();
}

AafObject& AafObject::bytes(uint16_t pid, std::string value) {
    prop(pid, 0x82).data = std::move(value);
    return *this;
}

AafObject& AafObject::text(uint16_t pid, const std::string& utf8) { return bytes(pid, utf16z(utf8)); }

AafObject& AafObject::i64(uint16_t pid, int64_t v) {
    std::string s;
    append64(s, uint64_t(v));
    return bytes(pid, s);
}

AafObject& AafObject::u32(uint16_t pid, uint32_t v) {
    std::string s;
    append32(s, v);
    return bytes(pid, s);
}

AafObject& AafObject::u16(uint16_t pid, uint16_t v) {
    std::string s;
    append16(s, v);
    return bytes(pid, s);
}

AafObject& AafObject::u8(uint16_t pid, uint8_t v) { return bytes(pid, std::string(1, char(v))); }

AafObject& AafObject::rational(uint16_t pid, int32_t num, int32_t den) {
    std::string s;
    append32(s, uint32_t(num));
    append32(s, uint32_t(den));
    return bytes(pid, s);
}

AafObject& AafObject::now(uint16_t pid) {
    const QDateTime t = QDateTime::currentDateTimeUtc();
    std::string s;
    append16(s, uint16_t(t.date().year()));
    s += char(t.date().month());
    s += char(t.date().day());
    s += char(t.time().hour());
    s += char(t.time().minute());
    s += char(t.time().second());
    s += char(t.time().msec() / 4);  // 1/250ths
    return bytes(pid, s);
}

AafObject& AafObject::indirectRational(uint16_t pid, int32_t num, int32_t den) {
    std::string s(1, char(0x4c));  // little-endian
    s += aafAuid("03010100-0000-0000-060e-2b3401040101");  // TypeDef Rational
    append32(s, uint32_t(num));
    append32(s, uint32_t(den));
    return bytes(pid, s);
}

AafObject& AafObject::strong(uint16_t pid, const std::string& name, std::unique_ptr<AafObject> child) {
    Prop& p = prop(pid, 0x22);
    const std::string storage = mangle(name, pid, 32);
    p.data = utf16z(storage);
    p.children.clear();
    p.children.push_back({storage, std::move(child)});
    return *this;
}

AafObject& AafObject::add(uint16_t pid, const std::string& name, std::unique_ptr<AafObject> child) {
    Prop& p = prop(pid, 0x32);
    if (p.indexName.empty()) {
        p.indexName = mangle(name, pid, 22);
        p.data = utf16z(p.indexName);
    }
    p.children.push_back({p.indexName + hexKey(p.children.size()), std::move(child)});
    return *this;
}

AafObject& AafObject::addToSet(uint16_t pid, const std::string& name, uint16_t keyPid, const std::string& key,
                               std::unique_ptr<AafObject> child) {
    Prop& p = prop(pid, 0x3A);
    if (p.indexName.empty()) {
        p.indexName = mangle(name, pid, 22);
        p.data = utf16z(p.indexName);
    }
    p.keyPid = keyPid;
    p.keys.push_back(key);
    p.children.push_back({p.indexName + hexKey(p.children.size()), std::move(child)});
    return *this;
}

AafObject& AafObject::weak(uint16_t pid, AafRefTable table, uint16_t keyPid, const std::string& key) {
    std::string s;
    append16(s, uint16_t(table));
    append16(s, keyPid);
    s += char(key.size());
    s += key;
    prop(pid, 0x02).data = s;
    return *this;
}

AafObject& AafObject::weakSet(uint16_t pid, const std::string& name, AafRefTable table, uint16_t keyPid,
                              const std::vector<std::string>& keys) {
    Prop& p = prop(pid, 0x1A);
    p.indexName = mangle(name, pid, 22);
    p.data = utf16z(p.indexName);
    p.table = uint16_t(table);
    p.keyPid = keyPid;
    p.keys = keys;
    return *this;
}

CfbEntry AafObject::storage(const std::string& name) const {
    CfbEntry e;
    e.name = name;
    e.storage = true;
    e.clsid = clsid(classId_);
    // The property stream: byte order, version, count, then (pid, format, size) for each and their data.
    std::string ps(1, char(0x4c));
    ps += char(32);
    append16(ps, uint16_t(props_.size()));
    for (const Prop& p : props_) {
        append16(ps, p.pid);
        append16(ps, p.format);
        append16(ps, uint16_t(p.data.size()));
    }
    for (const Prop& p : props_) ps += p.data;
    e.children.push_back(stream("properties", ps));
    for (const Prop& p : props_) {
        for (const Child& c : p.children) e.children.push_back(c.object->storage(c.name));
        if (p.format == 0x32 || p.format == 0x3A) {
            std::string ix;
            const uint32_t n = uint32_t(p.children.size());
            append32(ix, n);
            append32(ix, n);  // next free local key
            append32(ix, 0xFFFFFFFF);
            if (p.format == 0x32) {
                for (uint32_t k = 0; k < n; ++k) append32(ix, k);
            } else {
                append16(ix, p.keyPid);
                ix += char(p.keys.empty() ? 16 : p.keys.front().size());
                for (uint32_t k = 0; k < n; ++k) {
                    append32(ix, k);
                    append32(ix, 1);  // reference count
                    ix += p.keys[k];
                }
            }
            e.children.push_back(stream(p.indexName + " index", ix));
        } else if (p.format == 0x1A || p.format == 0x12) {
            std::string ix;
            append32(ix, uint32_t(p.keys.size()));
            append16(ix, p.table);
            append16(ix, p.keyPid);
            ix += char(p.keys.empty() ? 16 : p.keys.front().size());
            for (const std::string& k : p.keys) ix += k;
            e.children.push_back(stream(p.indexName + " index", ix));
        }
    }
    return e;
}

bool writeAafFile(const std::string& path, const AafObject& header, std::string* error) {
    CfbEntry root;
    root.storage = true;
    root.clsid = clsid(aafAuid("b3b398a5-1c90-11d4-8053-080036210804"));  // AAF root
    CfbEntry meta;
    if (!metaDictionary(meta)) {
        if (error) *error = "The AAF MetaDictionary could not be unpacked";
        return false;
    }
    root.children.push_back(std::move(meta));
    root.children.push_back(header.storage("Header-2"));
    // The root object: strong references to the two.
    std::string props(1, char(0x4c));
    props += char(32);
    append16(props, 2);
    const std::string md = utf16z("MetaDictionary-1"), hd = utf16z("Header-2");
    append16(props, 0x0001);
    append16(props, 0x22);
    append16(props, uint16_t(md.size()));
    append16(props, 0x0002);
    append16(props, 0x22);
    append16(props, uint16_t(hd.size()));
    props += md + hd;
    root.children.push_back(stream("properties", props));
    // Where weak references point (AafRefTable order): property-id paths from the root.
    static const std::vector<std::vector<uint16_t>> tables = {
        {0x0001, 0x0003},          {0x0001, 0x0004},          {0x0002, 0x3b04, 0x2605}, {0x0002, 0x3b04, 0x2608},
        {0x0002, 0x3b04, 0x2603},  {0x0002, 0x3b04, 0x2604},  {0x0002, 0x3b04, 0x2609},
    };
    std::string refs(1, char(0x4c));
    uint32_t pids = 0;
    for (const auto& t : tables) pids += uint32_t(t.size()) + 1;
    append16(refs, uint16_t(tables.size()));
    append32(refs, pids);
    for (const auto& t : tables) {
        for (uint16_t pid : t) append16(refs, pid);
        append16(refs, 0);
    }
    root.children.push_back(stream("referenced properties", refs));
    return writeCompoundFile(path, root, error);
}

}  // namespace montage
