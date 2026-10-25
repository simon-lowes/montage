#include "Exr.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <list>
#include <mutex>
#include <set>
#include <thread>

#ifdef MONTAGE_WITH_OPENEXR
#include <ImathBox.h>
#include <ImfChannelList.h>
#include <ImfCompositeDeepScanLine.h>
#include <ImfCompression.h>
#include <ImfDeepFrameBuffer.h>
#include <ImfDeepScanLineInputFile.h>
#include <ImfDeepScanLineInputPart.h>
#include <ImfDeepScanLineOutputFile.h>
#include <ImfDeepTiledInputPart.h>
#include <ImfDeepTiledOutputFile.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfInputPart.h>
#include <ImfMultiPartInputFile.h>
#include <ImfOutputFile.h>
#include <ImfStringAttribute.h>
#include <ImfThreading.h>
#include <half.h>
#endif

#include "DeepImage.h"
#include "Image.h"
#include "ImageSequence.h"
#include "core/Interpretation.h"

namespace montage {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The slot (0 R .. 3 A) a channel's last name fills, or -1.
int slotOf(const std::string& suffix) {
    const std::string s = lower(suffix);
    if (s == "r" || s == "red" || s == "x" || s == "u") return 0;
    if (s == "g" || s == "green" || s == "y" || s == "v") return 1;
    if (s == "b" || s == "blue" || s == "z") return 2;
    if (s == "a" || s == "alpha") return 3;
    return -1;
}

// Whether a channel without a layer name is the main picture's (colour, alpha or luminance) rather than a layer of
// its own (Z, the commonest).
bool mainChannel(const std::string& suffix) {
    const std::string s = lower(suffix);
    return s == "r" || s == "g" || s == "b" || s == "a" || s == "y" || s == "red" || s == "green" || s == "blue" || s == "alpha";
}

void splitChannel(const std::string& channel, std::string& prefix, std::string& suffix) {
    const size_t dot = channel.rfind('.');
    if (dot == std::string::npos) prefix.clear(), suffix = channel;
    else prefix = channel.substr(0, dot), suffix = channel.substr(dot + 1);
}

// "<name>NN": the rank group's number, or -1.
int cryptoGroup(const std::string& prefix, const std::string& name) {
    if (prefix.size() != name.size() + 2 || prefix.compare(0, name.size(), name) != 0) return -1;
    const char a = prefix[name.size()], b = prefix[name.size() + 1];
    if (!std::isdigit(static_cast<unsigned char>(a)) || !std::isdigit(static_cast<unsigned char>(b))) return -1;
    return (a - '0') * 10 + (b - '0');
}

uint32_t murmur3(const std::string& key) {
    const auto* data = reinterpret_cast<const uint8_t*>(key.data());
    const size_t len = key.size(), blocks = len / 4;
    uint32_t h = 0;
    const uint32_t c1 = 0xcc9e2d51, c2 = 0x1b873593;
    auto rotl = [](uint32_t x, int r) { return (x << r) | (x >> (32 - r)); };
    for (size_t i = 0; i < blocks; ++i) {
        uint32_t k;
        std::memcpy(&k, data + i * 4, 4);  // (little-endian hosts, as the spec's reference is)
        k *= c1, k = rotl(k, 15), k *= c2;
        h ^= k, h = rotl(h, 13), h = h * 5 + 0xe6546b64;
    }
    const uint8_t* tail = data + blocks * 4;
    uint32_t k = 0;
    switch (len & 3) {
    case 3: k ^= uint32_t(tail[2]) << 16; [[fallthrough]];
    case 2: k ^= uint32_t(tail[1]) << 8; [[fallthrough]];
    case 1:
        k ^= tail[0];
        k *= c1, k = rotl(k, 15), k *= c2;
        h ^= k;
    }
    h ^= uint32_t(len);
    h ^= h >> 16, h *= 0x85ebca6b, h ^= h >> 13, h *= 0xc2b2ae35, h ^= h >> 16;
    return h;
}

#ifdef MONTAGE_WITH_OPENEXR
void useThreads() {
    static std::once_flag once;
    std::call_once(once, [] { Imf::setGlobalThreadCount(int(std::max(1u, std::thread::hardware_concurrency()))); });
}

std::string compressionName(Imf::Compression c) {
    std::string name;
    Imf::getCompressionNameFromId(c, name);
    return name;
}

// Where a layer is: its part and its channels by slot (R, G, B, A).
struct Located {
    int part = -1;
    std::string channel[4];
    int count = 0;
};

std::string partPrefix(const Imf::Header& h, int parts) {
    return parts > 1 && h.hasName() && !h.name().empty() ? h.name() : std::string();
}

std::string joinName(const std::string& part, const std::string& layer) {
    if (part.empty()) return layer;
    return layer.empty() ? part : part + "." + layer;
}

// The Cryptomatte type names a header declares.
std::vector<std::pair<std::string, std::string>> cryptoKeys(const Imf::Header& h) {
    std::vector<std::pair<std::string, std::string>> out;  // key, name
    for (auto it = h.begin(); it != h.end(); ++it) {
        const std::string attr = it.name();
        if (attr.rfind("cryptomatte/", 0) != 0 || attr.size() < 17 || attr.compare(attr.size() - 5, 5, "/name") != 0) continue;
        if (const auto* s = dynamic_cast<const Imf::StringAttribute*>(&it.attribute()))
            out.emplace_back(attr.substr(12, attr.size() - 17), s->value());
    }
    return out;
}

std::string stringAttr(const Imf::Header& h, const std::string& name) {
    const auto* s = h.findTypedAttribute<Imf::StringAttribute>(name);
    return s ? s->value() : std::string();
}

void readManifest(const std::string& file, const Imf::Header& h, const std::string& key, Cryptomatte& c) {
    std::string json = stringAttr(h, "cryptomatte/" + key + "/manifest");
    if (json.empty()) {
        // A sidecar beside the file (never above it).
        const std::string side = stringAttr(h, "cryptomatte/" + key + "/manif_file");
        const auto rel = std::filesystem::u8path(side);
        if (!side.empty() && side.find("..") == std::string::npos && !rel.is_absolute() && !rel.has_root_name() && !rel.has_root_directory()) {
            // (a stream opened by path, not by narrow string, so folders with names that aren't ASCII open on Windows)
            std::ifstream f(std::filesystem::u8path(file).parent_path() / rel, std::ios::binary);
            json.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        }
    }
    if (json.empty()) return;
    const QJsonObject o = QJsonDocument::fromJson(QByteArray::fromStdString(json)).object();
    for (auto it = o.begin(); it != o.end(); ++it) {
        float id;
        if (parseCryptomatteHex(it.value().toString().toStdString(), id)) c.objects.emplace_back(it.key().toStdString(), id);
    }
    std::sort(c.objects.begin(), c.objects.end());
}

bool locate(const Imf::MultiPartInputFile& in, const std::string& layer, Located& out) {
    for (int p = 0; p < in.parts(); ++p) {
        const Imf::Header& h = in.header(p);
        const std::string part = partPrefix(h, in.parts());
        std::vector<std::string> extra;
        Located l;
        l.part = p;
        for (auto it = h.channels().begin(); it != h.channels().end(); ++it) {
            if (it.channel().xSampling != 1 || it.channel().ySampling != 1) continue;  // (luminance-chroma's RY, BY: Y alone, grey)
            std::string prefix, suffix;
            splitChannel(it.name(), prefix, suffix);
            const int slot = slotOf(suffix);
            std::string name = joinName(part, prefix);
            // (a bare channel that isn't a colour is a layer of its own, Z the commonest)
            if (prefix.empty() && !mainChannel(suffix)) name = joinName(part, suffix);
            if (name != layer) continue;
            const bool main = prefix.empty() && lower(suffix) == "y";  // luminance: grey
            if (main) l.channel[0] = it.name();
            else if (slot >= 0 && l.channel[slot].empty()) l.channel[slot] = it.name();
            else extra.push_back(it.name());
            ++l.count;
        }
        if (!l.count) continue;
        if (l.count == 1 && extra.empty() && l.channel[0].empty())  // (one channel, Z or a matte's A: grey)
            for (int s = 1; s < 4; ++s)
                if (!l.channel[s].empty()) std::swap(l.channel[0], l.channel[s]);
        for (const std::string& e : extra)
            for (int s = 0; s < 3; ++s)
                if (l.channel[s].empty()) {
                    l.channel[s] = e;
                    break;
                }
        out = l;
        return true;
    }
    return false;
}

// The first deep part of a file, or -1.
int isDeep(const Imf::MultiPartInputFile& in) {
    for (int p = 0; p < in.parts(); ++p)
        if (in.header(p).hasType() && in.header(p).type().find("deep") != std::string::npos) return p;
    return -1;
}
#endif

}  // namespace

bool exrAvailable() {
#ifdef MONTAGE_WITH_OPENEXR
    return true;
#else
    return false;
#endif
}

bool isExrPath(const std::string& file) {
    return file.size() > 4 && lower(file.substr(file.size() - 4)) == ".exr";
}

std::string exrFrameFile(const std::string& mediaPath, double seconds) {
    const std::string file = uninterpretedPath(mediaPath);
    if (ImageSequence seq; parseImageSequencePath(file, seq)) {
        if (!isExrPath(seq.pattern)) return {};
        return imageSequenceFrame(seq, seq.first + std::clamp(int(std::floor(std::max(0.0, seconds) * seq.fps.toDouble() + 1e-6)), 0,
                                                              seq.frames() - 1));
    }
    return isExrPath(file) ? file : std::string();
}

std::string Cryptomatte::objectName(float id) const {
    for (const auto& [name, v] : objects)
        if (std::memcmp(&v, &id, 4) == 0) return name;
    return {};
}

bool ExrInfo::hasLayer(const std::string& name) const {
    return std::any_of(layers.begin(), layers.end(), [&](const ExrLayer& l) { return l.name == name; });
}

const Cryptomatte* ExrInfo::cryptomatte(const std::string& name) const {
    for (const Cryptomatte& c : cryptomattes)
        if (name.empty() || c.name == name) return &c;
    return nullptr;
}

bool exrNeedsLibrary(const std::string& compression) {
    return compression.rfind("htj2k", 0) == 0 || compression == "lj2k" || compression == "zstd";
}

bool readExrInfo(const std::string& file, ExrInfo& out, std::string* error) {
    out = {};
#ifdef MONTAGE_WITH_OPENEXR
    try {
        useThreads();
        Imf::MultiPartInputFile in(file.c_str());
        const Imf::Header& first = in.header(0);
        const Imath::Box2i& dw = first.displayWindow();
        out.width = dw.max.x - dw.min.x + 1;
        out.height = dw.max.y - dw.min.y + 1;
        out.pixelAspect = first.pixelAspectRatio() > 0 ? first.pixelAspectRatio() : 1;
        out.compression = compressionName(first.compression());
        for (int p = 0; p < in.parts(); ++p) {
            const Imf::Header& h = in.header(p);
            if (h.hasType() && h.type().find("deep") != std::string::npos) {
                // Deep data: its samples (readDeepExr), shown flattened as the main picture.
                if (!out.deep) {
                    out.deep = true;
                    ExrLayer l;
                    for (const char* c : {"R", "G", "B", "A"})
                        if (h.channels().findChannel(c)) l.channels.push_back(c);
                    out.layers.insert(out.layers.begin(), std::move(l));
                }
                continue;
            }
            const std::string part = partPrefix(h, in.parts());
            // Cryptomattes first, so their channels aren't listed as layers.
            std::set<std::string> cryptoPrefixes;
            for (const auto& [key, name] : cryptoKeys(h)) {
                Cryptomatte c;
                c.name = name;
                c.part = p;
                std::map<int, std::array<std::string, 4>> groups;
                for (auto it = h.channels().begin(); it != h.channels().end(); ++it) {
                    std::string prefix, suffix;
                    splitChannel(it.name(), prefix, suffix);
                    const int g = cryptoGroup(prefix, name), slot = slotOf(suffix);
                    if (g >= 0 && slot >= 0) groups[g][size_t(slot)] = it.name(), cryptoPrefixes.insert(prefix);
                    if (prefix == name) cryptoPrefixes.insert(prefix);  // (the deprecated preview)
                }
                for (const auto& [g, ch] : groups)
                    for (const std::string& s : ch)
                        if (!s.empty()) c.channels.push_back(s);
                if (c.channels.size() < 2) continue;
                c.channels.resize(c.channels.size() & ~size_t(1));
                readManifest(file, h, key, c);
                c.name = joinName(part, name);
                out.cryptomattes.push_back(std::move(c));
            }
            std::map<std::string, ExrLayer> layers;
            std::vector<std::string> order;
            for (auto it = h.channels().begin(); it != h.channels().end(); ++it) {
                if (it.channel().xSampling != 1 || it.channel().ySampling != 1) continue;  // (subsampled chroma: not read)
                std::string prefix, suffix;
                splitChannel(it.name(), prefix, suffix);
                if (cryptoPrefixes.count(prefix)) continue;
                std::string name = joinName(part, prefix);
                if (prefix.empty() && !mainChannel(suffix)) name = joinName(part, suffix);
                if (!layers.count(name)) order.push_back(name), layers[name].name = name;
                layers[name].channels.push_back(it.name());
            }
            // The main picture first, then the rest in the file's order.
            std::stable_sort(order.begin(), order.end(), [&](const std::string& a, const std::string& b) {
                return (a == part) > (b == part);
            });
            for (const std::string& n : order) {
                ExrLayer& l = layers[n];
                std::stable_sort(l.channels.begin(), l.channels.end(), [](const std::string& a, const std::string& b) {
                    std::string pa, sa, pb, sb;
                    splitChannel(a, pa, sa), splitChannel(b, pb, sb);
                    const int x = slotOf(sa), y = slotOf(sb);
                    return (x < 0 ? 9 : x) < (y < 0 ? 9 : y);
                });
                out.layers.push_back(std::move(l));
            }
        }
        return true;
    } catch (const std::exception& e) {
        if (error) *error = "Cannot read " + file + ": " + e.what();
        return false;
    }
#else
    if (error) *error = "This build of Montage reads OpenEXR layers only with the OpenEXR library";
    (void)file;
    return false;
#endif
}

bool readExrLayer(const std::string& file, const std::string& layer, int& width, int& height, std::vector<float>& rgba,
                  std::string* error) {
#ifdef MONTAGE_WITH_OPENEXR
    try {
        useThreads();
        Imf::MultiPartInputFile in(file.c_str());
        if (layer.empty() && isDeep(in) >= 0) {
            // A deep file's picture: its samples flattened.
            const std::shared_ptr<const DeepFrame> deep = cachedDeepExr(file, error);
            if (!deep) return false;
            width = deep->width, height = deep->height;
            rgba = flattenDeep(*deep);
            return true;
        }
        Located l;
        if (!locate(in, layer, l)) {
            if (error) *error = file + " has no layer \"" + layer + "\"";
            return false;
        }
        const Imf::Header& h = in.header(l.part);
        const Imath::Box2i disp = in.header(0).displayWindow(), dw = h.dataWindow();
        width = disp.max.x - disp.min.x + 1;
        height = disp.max.y - disp.min.y + 1;
        const int dwW = dw.max.x - dw.min.x + 1, dwH = dw.max.y - dw.min.y + 1;
        const bool alpha = !l.channel[3].empty();
        std::vector<float> data(size_t(dwW) * dwH * 4, 0.0f);
        Imf::FrameBuffer fb;
        for (int s = 0; s < 4; ++s) {
            if (l.channel[s].empty()) continue;
            // (OpenEXR's slices are addressed from the data window's origin)
            char* base = reinterpret_cast<char*>(data.data() + s) -
                         (ptrdiff_t(dw.min.y) * dwW + dw.min.x) * ptrdiff_t(4 * sizeof(float));
            fb.insert(l.channel[s], Imf::Slice(Imf::FLOAT, base, 4 * sizeof(float), size_t(dwW) * 4 * sizeof(float)));
        }
        Imf::InputPart part(in, l.part);
        part.setFrameBuffer(fb);
        part.readPixels(dw.min.y, dw.max.y);
        const bool grey = l.channel[1].empty() && l.channel[2].empty();
        rgba.assign(size_t(width) * height * 4, 0.0f);
        if (!alpha)
            for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 1;
        for (int y = std::max(dw.min.y, disp.min.y); y <= std::min(dw.max.y, disp.max.y); ++y)
            for (int x = std::max(dw.min.x, disp.min.x); x <= std::min(dw.max.x, disp.max.x); ++x) {
                const float* q = &data[(size_t(y - dw.min.y) * dwW + size_t(x - dw.min.x)) * 4];
                float* o = &rgba[(size_t(y - disp.min.y) * width + size_t(x - disp.min.x)) * 4];
                o[0] = q[0];
                o[1] = grey ? q[0] : q[1];
                o[2] = grey ? q[0] : q[2];
                o[3] = alpha ? q[3] : 1.0f;
            }
        return true;
    } catch (const std::exception& e) {
        if (error) *error = "Cannot read " + file + ": " + e.what();
        return false;
    }
#else
    (void)file, (void)layer, (void)width, (void)height, (void)rgba;
    if (error) *error = "This build of Montage reads OpenEXR layers only with the OpenEXR library";
    return false;
#endif
}

bool readExrChannels(const std::string& file, int& width, int& height, std::vector<ExrCopiedLayer>& out,
                     std::map<std::string, std::string>* attributes, std::string* error) {
    out.clear();
#ifdef MONTAGE_WITH_OPENEXR
    ExrInfo info;
    if (!readExrInfo(file, info, error)) return false;
    try {
        Imf::MultiPartInputFile in(file.c_str());
        const Imath::Box2i disp = in.header(0).displayWindow();
        width = disp.max.x - disp.min.x + 1;
        height = disp.max.y - disp.min.y + 1;
        std::set<std::string> takenCryptoNames;
        for (int p = 0; p < in.parts(); ++p) {
            const Imf::Header& h = in.header(p);
            if (h.hasType() && h.type().find("deep") != std::string::npos) continue;
            const std::string part = partPrefix(h, in.parts());
            // Which Cryptomatte each channel prefix belongs to (its listed name, and the prefix it is copied under), and
            // the header entries naming them. A Cryptomatte keeps its own name, so readers find it, unless another part's
            // already took it; then it and its key are made the part's.
            std::map<std::string, std::pair<std::string, std::string>> cryptoOf;
            for (const auto& [key, name] : cryptoKeys(h)) {
                const std::string outName = takenCryptoNames.insert(name).second ? name : joinName(part, name);
                takenCryptoNames.insert(outName);
                std::string outKey = key;
                if (attributes && attributes->count("cryptomatte/" + key + "/name")) outKey = cryptomatteHex(cryptomatteId(outName)).substr(0, 7);
                for (auto it = h.channels().begin(); it != h.channels().end(); ++it) {
                    std::string prefix, suffix;
                    splitChannel(it.name(), prefix, suffix);
                    if (cryptoGroup(prefix, name) >= 0 || prefix == name)
                        cryptoOf[prefix] = {joinName(part, name), outName + prefix.substr(name.size())};
                }
                if (!attributes) continue;
                (*attributes)["cryptomatte/" + outKey + "/name"] = outName;
                for (const char* k : {"hash", "conversion"})
                    if (const std::string v = stringAttr(h, "cryptomatte/" + key + "/" + k); !v.empty()) (*attributes)["cryptomatte/" + outKey + "/" + k] = v;
                if (const Cryptomatte* c = info.cryptomatte(joinName(part, name)); c && !c->objects.empty()) {
                    QJsonObject manifest;
                    for (const auto& [object, id] : c->objects) manifest[QString::fromStdString(object)] = QString::fromStdString(cryptomatteHex(id));
                    (*attributes)["cryptomatte/" + outKey + "/manifest"] = QJsonDocument(manifest).toJson(QJsonDocument::Compact).toStdString();
                }
            }
            // The channels in groups, read as floats over the data window.
            const Imath::Box2i dw = h.dataWindow();
            const int dwW = dw.max.x - dw.min.x + 1, dwH = dw.max.y - dw.min.y + 1;
            std::vector<ExrCopiedLayer> groups;
            std::vector<std::vector<float>> buffers;
            std::vector<std::pair<size_t, size_t>> where;  // (group, channel) of each channel read
            std::vector<std::string> names;
            for (auto it = h.channels().begin(); it != h.channels().end(); ++it) {
                if (it.channel().xSampling != 1 || it.channel().ySampling != 1) continue;  // (subsampled chroma: not copied)
                std::string prefix, suffix;
                splitChannel(it.name(), prefix, suffix);
                ExrCopiedLayer key;
                key.layer.name = joinName(part, prefix);
                if (const auto c = cryptoOf.find(prefix); c != cryptoOf.end()) {
                    key.listedAs = c->second.first;
                    key.layer.name = c->second.second;
                    key.cryptomatte = true;
                } else {
                    key.listedAs = prefix.empty() && !mainChannel(suffix) ? joinName(part, suffix) : key.layer.name;
                }
                auto g = std::find_if(groups.begin(), groups.end(), [&](const ExrCopiedLayer& x) {
                    return x.layer.name == key.layer.name && x.listedAs == key.listedAs;
                });
                if (g == groups.end()) g = groups.insert(groups.end(), key);
                g->layer.channels.push_back(suffix);
                if (it.channel().type != Imf::HALF) g->layer.fullFloat = true;
                where.emplace_back(size_t(g - groups.begin()), g->layer.channels.size() - 1);
                names.push_back(it.name());
            }
            if (groups.empty()) continue;
            buffers.resize(groups.size());
            for (size_t g = 0; g < groups.size(); ++g) buffers[g].assign(size_t(dwW) * dwH * groups[g].layer.channels.size(), 0.0f);
            Imf::FrameBuffer fb;
            for (size_t i = 0; i < names.size(); ++i) {
                const auto [g, k] = where[i];
                const size_t n = groups[g].layer.channels.size();
                char* base = reinterpret_cast<char*>(buffers[g].data() + k) - (ptrdiff_t(dw.min.y) * dwW + dw.min.x) * ptrdiff_t(n * sizeof(float));
                fb.insert(names[i], Imf::Slice(Imf::FLOAT, base, n * sizeof(float), size_t(dwW) * n * sizeof(float)));
            }
            Imf::InputPart reader(in, p);
            reader.setFrameBuffer(fb);
            reader.readPixels(dw.min.y, dw.max.y);
            // Placed in the display window.
            for (size_t g = 0; g < groups.size(); ++g) {
                const size_t n = groups[g].layer.channels.size();
                std::vector<float>& d = groups[g].layer.data;
                d.assign(size_t(width) * height * n, 0.0f);
                for (int y = std::max(dw.min.y, disp.min.y); y <= std::min(dw.max.y, disp.max.y); ++y) {
                    const int x0 = std::max(dw.min.x, disp.min.x), x1 = std::min(dw.max.x, disp.max.x);
                    if (x1 < x0) break;
                    std::copy_n(&buffers[g][(size_t(y - dw.min.y) * dwW + size_t(x0 - dw.min.x)) * n], size_t(x1 - x0 + 1) * n,
                                &d[(size_t(y - disp.min.y) * width + size_t(x0 - disp.min.x)) * n]);
                }
                buffers[g] = {};
                out.push_back(std::move(groups[g]));
            }
        }
        return true;
    } catch (const std::exception& e) {
        if (error) *error = "Cannot read " + file + ": " + e.what();
        return false;
    }
#else
    (void)file, (void)width, (void)height, (void)attributes;
    if (error) *error = "This build of Montage reads OpenEXR layers only with the OpenEXR library";
    return false;
#endif
}

float CryptomatteFrame::idAt(int x, int y) const {
    if (x < 0 || y < 0 || x >= width || y >= height || ranks <= 0) return 0;
    const float* p = &pairs[(size_t(y) * width + x) * size_t(ranks) * 2];
    float best = 0, cover = 0;
    for (int r = 0; r < ranks; ++r)
        if (p[r * 2 + 1] > cover) cover = p[r * 2 + 1], best = p[r * 2];
    return best;
}

std::vector<float> CryptomatteFrame::matte(const std::vector<float>& ids) const {
    std::vector<float> out(size_t(width) * height, 0.0f);
    if (ids.empty() || ranks <= 0) return out;
    std::vector<uint32_t> bits(ids.size());
    for (size_t i = 0; i < ids.size(); ++i) std::memcpy(&bits[i], &ids[i], 4);
    std::sort(bits.begin(), bits.end());
    parallelRows(height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < width; ++x) {
                const size_t i = size_t(y) * width + x;
                const float* p = &pairs[i * size_t(ranks) * 2];
                float sum = 0;
                for (int r = 0; r < ranks; ++r) {
                    uint32_t b;
                    std::memcpy(&b, &p[r * 2], 4);
                    if (p[r * 2 + 1] != 0 && std::binary_search(bits.begin(), bits.end(), b)) sum += p[r * 2 + 1];
                }
                out[i] = std::clamp(sum, 0.0f, 1.0f);
            }
    });
    return out;
}

bool readCryptomatte(const std::string& file, const std::string& name, CryptomatteFrame& out, std::string* error) {
    out = {};
#ifdef MONTAGE_WITH_OPENEXR
    ExrInfo info;
    if (!readExrInfo(file, info, error)) return false;
    const Cryptomatte* c = info.cryptomatte(name);
    if (!c) {
        if (error) *error = file + " has no Cryptomatte" + (name.empty() ? std::string() : " \"" + name + "\"");
        return false;
    }
    try {
        Imf::MultiPartInputFile in(file.c_str());
        // The part holding its channels (parts' channels are named alike: CryptoObject00 in each).
        const int part = c->part;
        if (part < 0 || part >= in.parts() || !in.header(part).channels().findChannel(c->channels[0]))
            throw std::runtime_error("its Cryptomatte is not where its header said");
        const Imf::Header& h = in.header(part);
        const Imath::Box2i disp = in.header(0).displayWindow(), dw = h.dataWindow();
        out.width = disp.max.x - disp.min.x + 1;
        out.height = disp.max.y - disp.min.y + 1;
        out.ranks = int(c->channels.size() / 2);
        out.crypto = *c;
        const int n = int(c->channels.size()), dwW = dw.max.x - dw.min.x + 1, dwH = dw.max.y - dw.min.y + 1;
        // Read straight into the frame when the data window is the display window (as renders mostly write them).
        const bool whole = dw == disp;
        std::vector<float> data;
        out.pairs.assign(size_t(out.width) * out.height * n, 0.0f);
        float* const into = whole ? out.pairs.data() : (data.assign(size_t(dwW) * dwH * n, 0.0f), data.data());
        Imf::FrameBuffer fb;
        for (int k = 0; k < n; ++k) {
            char* base = reinterpret_cast<char*>(into + k) - (ptrdiff_t(dw.min.y) * dwW + dw.min.x) * ptrdiff_t(n * sizeof(float));
            // (ids must be read as stored: 32-bit floats, never through half)
            fb.insert(c->channels[size_t(k)], Imf::Slice(Imf::FLOAT, base, n * sizeof(float), size_t(dwW) * n * sizeof(float)));
        }
        Imf::InputPart ip(in, part);
        ip.setFrameBuffer(fb);
        ip.readPixels(dw.min.y, dw.max.y);
        const int x0 = std::max(dw.min.x, disp.min.x), x1 = std::min(dw.max.x, disp.max.x);
        if (!whole && x1 >= x0)
            for (int y = std::max(dw.min.y, disp.min.y); y <= std::min(dw.max.y, disp.max.y); ++y)
                std::memcpy(&out.pairs[(size_t(y - disp.min.y) * out.width + size_t(x0 - disp.min.x)) * n],
                            &data[(size_t(y - dw.min.y) * dwW + size_t(x0 - dw.min.x)) * n], size_t(x1 - x0 + 1) * n * sizeof(float));
        return true;
    } catch (const std::exception& e) {
        if (error) *error = "Cannot read " + file + ": " + e.what();
        out = {};
        return false;
    }
#else
    (void)file, (void)name;
    if (error) *error = "This build of Montage reads Cryptomattes only with the OpenEXR library";
    return false;
#endif
}

namespace {
// A cache key that changes when the file is written again (a render replaced in place).
std::string fileKey(const std::string& file) {
    std::error_code ec;
    const auto when = std::filesystem::last_write_time(std::filesystem::u8path(file), ec);
    return file + '\n' + (ec ? std::string() : std::to_string(static_cast<long long>(when.time_since_epoch().count())));
}
}  // namespace

std::shared_ptr<const ExrInfo> cachedExrInfo(const std::string& file) {
    static std::mutex mutex;
    static std::list<std::pair<std::string, std::shared_ptr<const ExrInfo>>> recent;  // newest first
    const std::string key = fileKey(file);
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto it = recent.begin(); it != recent.end(); ++it)
            if (it->first == key) {
                recent.splice(recent.begin(), recent, it);
                return it->second;
            }
    }
    auto info = std::make_shared<ExrInfo>();
    if (!readExrInfo(file, *info)) info.reset();
    std::lock_guard<std::mutex> lock(mutex);
    recent.emplace_front(key, info);
    while (recent.size() > 8) recent.pop_back();
    return info;
}

std::shared_ptr<const CryptomatteFrame> cachedCryptomatte(const std::string& file, const std::string& name, bool read) {
    static std::mutex mutex;
    static std::list<std::pair<std::string, std::shared_ptr<const CryptomatteFrame>>> recent;  // newest first
    const std::string key = fileKey(file) + '\n' + name;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto it = recent.begin(); it != recent.end(); ++it)
            if (it->first == key) {
                recent.splice(recent.begin(), recent, it);
                return it->second;
            }
    }
    if (!read) return nullptr;
    auto frame = std::make_shared<CryptomatteFrame>();
    if (!readCryptomatte(file, name, *frame)) frame.reset();
    std::lock_guard<std::mutex> lock(mutex);
    recent.emplace_front(key, frame);
    // A few frames, as long as they stay small: a 4K frame of six ranks is over 400 MB.
    size_t bytes = 0, kept = 0;
    for (auto it = recent.begin(); it != recent.end(); ++it, ++kept) {
        bytes += it->second ? it->second->pairs.size() * sizeof(float) : 0;
        if (kept >= 6 || (kept >= 1 && bytes > (size_t(512) << 20))) {
            recent.erase(it, recent.end());
            break;
        }
    }
    return frame;
}

float cryptomatteId(const std::string& name) {
    uint32_t h = murmur3(name);
    const uint32_t exponent = h >> 23 & 255;
    if (exponent == 0 || exponent == 255) h ^= 1u << 23;
    float f;
    std::memcpy(&f, &h, 4);
    return f;
}

std::string cryptomatteHex(float id) {
    uint32_t bits;
    std::memcpy(&bits, &id, 4);
    char buf[9];
    std::snprintf(buf, sizeof buf, "%08x", bits);
    return buf;
}

bool parseCryptomatteHex(const std::string& hex, float& id) {
    if (hex.size() != 8 || !std::all_of(hex.begin(), hex.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); }))
        return false;
    const uint32_t bits = uint32_t(std::stoul(hex, nullptr, 16));
    std::memcpy(&id, &bits, 4);
    return true;
}

std::vector<float> parseCryptomatteIds(const std::string& list) {
    std::vector<float> out;
    std::string item;
    auto flush = [&] {
        float id;
        if (parseCryptomatteHex(item, id) &&
            std::none_of(out.begin(), out.end(), [&](float v) { return std::memcmp(&v, &id, 4) == 0; }))
            out.push_back(id);
        item.clear();
    };
    for (char c : list) {
        if (c == ',' || std::isspace(static_cast<unsigned char>(c))) flush();
        else item += c;
    }
    flush();
    return out;
}

std::string cryptomatteIdList(const std::vector<float>& ids) {
    std::string out;
    for (float id : ids) out += (out.empty() ? "" : ",") + cryptomatteHex(id);
    return out;
}

std::vector<std::string> exrCompressions() {
    return {"none", "rle", "zips", "zip", "piz", "pxr24", "b44", "b44a", "dwaa", "dwab", "zstd", "htj2k256", "htj2k32"};
}

std::string exrCompressionLabel(const std::string& name) {
    static const std::map<std::string, std::string> labels{
        {"none", "None"},
        {"rle", "RLE"},
        {"zips", "ZIP (1 line)"},
        {"zip", "ZIP (16 lines)"},
        {"piz", "PIZ"},
        {"pxr24", "PXR24 (lossy for 32-bit)"},
        {"b44", "B44 (lossy)"},
        {"b44a", "B44A (lossy)"},
        {"dwaa", "DWAA (lossy)"},
        {"dwab", "DWAB (lossy)"},
        {"zstd", "Zstandard"},
        {"htj2k256", "HTJ2K (256 lines)"},
        {"htj2k32", "HTJ2K (32 lines)"},
    };
    const auto it = labels.find(name);
    return it == labels.end() ? name : it->second;
}

bool writeExr(const std::string& file, int width, int height, const std::vector<ExrOutLayer>& layers, const std::string& compression,
              const std::map<std::string, std::string>& attributes, std::string* error) {
#ifdef MONTAGE_WITH_OPENEXR
    try {
        useThreads();
        Imf::Compression c = Imf::NUM_COMPRESSION_METHODS;
        Imf::getCompressionIdFromName(compression.empty() ? "zip" : compression, c);
        if (c == Imf::NUM_COMPRESSION_METHODS) {
            if (error) *error = "Unknown OpenEXR compression \"" + compression + "\"";
            return false;
        }
        Imf::Header header(width, height);
        header.compression() = c;
        for (const auto& [k, v] : attributes) header.insert(k, Imf::StringAttribute(v));
        // Half channels converted once, kept alive until the file is written.
        std::vector<std::vector<half>> halves;
        halves.reserve(layers.size());
        Imf::FrameBuffer fb;
        for (const ExrOutLayer& l : layers) {
            const size_t n = l.channels.size();
            if (n == 0 || l.data.size() != size_t(width) * height * n) {
                if (error) *error = "Layer \"" + l.name + "\" is the wrong size";
                return false;
            }
            const Imf::PixelType type = l.fullFloat ? Imf::FLOAT : Imf::HALF;
            const char* base = nullptr;
            size_t elem = sizeof(float);
            if (!l.fullFloat) {
                halves.emplace_back(l.data.size());
                std::vector<half>& h = halves.back();
                for (size_t i = 0; i < l.data.size(); ++i) h[i] = half(l.data[i]);
                base = reinterpret_cast<const char*>(h.data());
                elem = sizeof(half);
            } else {
                base = reinterpret_cast<const char*>(l.data.data());
            }
            for (size_t k = 0; k < n; ++k) {
                const std::string name = l.name.empty() ? l.channels[k] : l.name + "." + l.channels[k];
                header.channels().insert(name, Imf::Channel(type));
                fb.insert(name, Imf::Slice(type, const_cast<char*>(base + k * elem), n * elem, size_t(width) * n * elem));
            }
        }
        Imf::OutputFile out(file.c_str(), header);
        out.setFrameBuffer(fb);
        out.writePixels(height);
        return true;
    } catch (const std::exception& e) {
        if (error) *error = "Cannot write " + file + ": " + e.what();
        return false;
    }
#else
    (void)file, (void)width, (void)height, (void)layers, (void)compression, (void)attributes;
    if (error) *error = "This build of Montage writes OpenEXR layers only with the OpenEXR library";
    return false;
#endif
}

// ---- Deep data -----------------------------------------------------------------------------------------------------

bool readDeepExr(const std::string& file, DeepFrame& out, std::string* error, bool* floatColour) {
    out = {};
    if (floatColour) *floatColour = false;
#ifdef MONTAGE_WITH_OPENEXR
    try {
        useThreads();
        Imf::MultiPartInputFile in(file.c_str());
        const int p = isDeep(in);
        if (p < 0) {
            if (error) *error = file + " holds no deep data";
            return false;
        }
        const Imf::Header& h = in.header(p);
        const Imath::Box2i disp = in.header(0).displayWindow(), dw = h.dataWindow();
        const int dwW = dw.max.x - dw.min.x + 1, dwH = dw.max.y - dw.min.y + 1;
        const size_t n = size_t(dwW) * size_t(dwH);
        static const char* names[] = {"Z", "ZBack", "R", "G", "B", "A"};
        bool has[6] = {};
        for (int c = 0; c < 6; ++c) has[c] = h.channels().findChannel(names[c]) != nullptr;
        if (!has[0]) {
            if (error) *error = file + " has no Z channel";
            return false;
        }
        if (floatColour)
            if (const Imf::Channel* r = h.channels().findChannel("R")) *floatColour = r->type == Imf::FLOAT;
        // Each pixel's count, then its samples read a band of rows at a time straight into the frame: a pointer a
        // pixel of the band for each channel, into the samples' fields.
        std::vector<uint32_t> counts(n, 0);
        const ptrdiff_t origin = ptrdiff_t(dw.min.y) * dwW + dw.min.x;
        const Imf::Slice countSlice(Imf::UINT, reinterpret_cast<char*>(counts.data() - origin), sizeof(uint32_t), sizeof(uint32_t) * size_t(dwW));
        const bool tiled = h.type().find("tile") != std::string::npos;
        std::unique_ptr<Imf::DeepTiledInputPart> tiles;
        std::unique_ptr<Imf::DeepScanLineInputPart> lines;
        {
            Imf::DeepFrameBuffer fb;
            fb.insertSampleCountSlice(countSlice);
            if (tiled) {
                tiles = std::make_unique<Imf::DeepTiledInputPart>(in, p);
                tiles->setFrameBuffer(fb);
                tiles->readPixelSampleCounts(0, tiles->numXTiles(0) - 1, 0, tiles->numYTiles(0) - 1, 0, 0);
            } else {
                lines = std::make_unique<Imf::DeepScanLineInputPart>(in, p);
                lines->setFrameBuffer(fb);
                lines->readPixelSampleCounts(dw.min.y, dw.max.y);
            }
        }
        size_t total = 0;
        for (uint32_t c : counts) total += c;
        std::vector<DeepSample>& samples = out.samples;
        samples.assign(total, DeepSample{});
        const int band = tiled ? tiles->tileYSize() : 64;
        std::vector<std::vector<char*>> pointers(6, std::vector<char*>(size_t(band) * size_t(dwW), nullptr));
        size_t at = 0;
        for (int y0 = dw.min.y, b = 0; y0 <= dw.max.y; y0 += band, ++b) {
            const int y1 = std::min(dw.max.y, y0 + band - 1);
            const size_t first = size_t(y0 - dw.min.y) * size_t(dwW), last = size_t(y1 - dw.min.y + 1) * size_t(dwW);
            for (size_t i = first; i < last; ++i) {
                DeepSample* s = samples.data() + at;
                pointers[0][i - first] = reinterpret_cast<char*>(&s->z);
                pointers[1][i - first] = reinterpret_cast<char*>(&s->zBack);
                pointers[2][i - first] = reinterpret_cast<char*>(&s->r);
                pointers[3][i - first] = reinterpret_cast<char*>(&s->g);
                pointers[4][i - first] = reinterpret_cast<char*>(&s->b);
                pointers[5][i - first] = reinterpret_cast<char*>(&s->a);
                at += counts[i];
            }
            Imf::DeepFrameBuffer fb;
            fb.insertSampleCountSlice(countSlice);
            const ptrdiff_t bandOrigin = ptrdiff_t(y0) * dwW + dw.min.x;
            for (int c = 0; c < 6; ++c)
                if (has[c])
                    fb.insert(names[c], Imf::DeepSlice(Imf::FLOAT, reinterpret_cast<char*>(pointers[size_t(c)].data() - bandOrigin), sizeof(char*),
                                                       sizeof(char*) * size_t(dwW), sizeof(DeepSample)));
            if (tiled) {
                tiles->setFrameBuffer(fb);
                tiles->readTiles(0, tiles->numXTiles(0) - 1, b, b, 0, 0);
            } else {
                lines->setFrameBuffer(fb);
                lines->readPixels(y0, y1);
            }
        }
        // The samples laid out on the display window (in place: both run row by row), their depths made sound: a
        // sample with no depth goes, one with no (or an endless or inverted) ZBack becomes a point.
        out.width = disp.max.x - disp.min.x + 1;
        out.height = disp.max.y - disp.min.y + 1;
        out.offsets.assign(size_t(out.width) * size_t(out.height) + 1, 0);
        size_t read = 0, kept = 0;
        for (int sy = 0; sy < dwH; ++sy)
            for (int sx = 0; sx < dwW; ++sx) {
                const size_t count = counts[size_t(sy) * size_t(dwW) + size_t(sx)];
                const int x = sx + dw.min.x - disp.min.x, y = sy + dw.min.y - disp.min.y;
                if (x < 0 || y < 0 || x >= out.width || y >= out.height) {
                    read += count;
                    continue;
                }
                const size_t before = kept;
                for (size_t k = 0; k < count; ++k) {
                    DeepSample s = samples[read++];
                    if (!std::isfinite(s.z)) continue;
                    if (!has[1] || !std::isfinite(s.zBack) || !(s.zBack >= s.z)) s.zBack = s.z;
                    if (!has[5]) s.a = 1;  // (no alpha: opaque)
                    samples[kept++] = s;
                }
                out.offsets[size_t(y) * size_t(out.width) + size_t(x) + 1] = uint32_t(kept - before);
            }
        for (size_t i = 1; i < out.offsets.size(); ++i) out.offsets[i] += out.offsets[i - 1];
        samples.resize(kept);
        if (kept < total / 2) samples.shrink_to_fit();
        return true;
    } catch (const std::exception& e) {
        out = {};
        if (error) *error = "Cannot read " + file + ": " + e.what();
        return false;
    }
#else
    (void)file;
    if (error) *error = "This build of Montage reads deep OpenEXR only with the OpenEXR library";
    return false;
#endif
}

bool writeDeepExr(const std::string& file, const DeepFrame& f, const std::string& compression, bool tiled, bool fullFloat, std::string* error) {
#ifdef MONTAGE_WITH_OPENEXR
    try {
        useThreads();
        if (f.empty() || f.offsets.size() != size_t(f.width) * size_t(f.height) + 1) {
            if (error) *error = "No deep frame to write";
            return false;
        }
        Imf::Compression c = Imf::NUM_COMPRESSION_METHODS;
        Imf::getCompressionIdFromName(compression.empty() ? "zips" : compression, c);
        // (What OpenEXR reads deep data in.)
        if (c != Imf::NO_COMPRESSION && c != Imf::RLE_COMPRESSION && c != Imf::ZIPS_COMPRESSION && c != Imf::ZSTD_COMPRESSION) {
            if (error) *error = "Deep OpenEXR is compressed with none, rle, zips or zstd, not \"" + compression + "\"";
            return false;
        }
        Imf::Header header(f.width, f.height);
        header.compression() = c;
        static const char* names[] = {"Z", "ZBack", "R", "G", "B", "A"};
        const Imf::PixelType colourType = fullFloat ? Imf::FLOAT : Imf::HALF;
        for (int k = 0; k < 6; ++k) header.channels().insert(names[k], Imf::Channel(k < 2 ? Imf::FLOAT : colourType));
        const size_t n = size_t(f.width) * size_t(f.height);
        std::vector<uint32_t> counts(n);
        for (size_t i = 0; i < n; ++i) counts[i] = f.offsets[i + 1] - f.offsets[i];
        // Halves for the colour, the depths as they are.
        std::vector<half> colour(fullFloat ? 0 : f.samples.size() * 4);
        for (size_t i = 0; i < f.samples.size() && !fullFloat; ++i) {
            const DeepSample& s = f.samples[i];
            colour[i * 4] = half(s.r), colour[i * 4 + 1] = half(s.g), colour[i * 4 + 2] = half(s.b), colour[i * 4 + 3] = half(s.a);
        }
        std::vector<std::vector<char*>> pointers(6, std::vector<char*>(n));
        for (size_t i = 0; i < n; ++i) {
            DeepSample* s = const_cast<DeepSample*>(f.samples.data()) + f.offsets[i];
            pointers[0][i] = reinterpret_cast<char*>(&s->z);
            pointers[1][i] = reinterpret_cast<char*>(&s->zBack);
            if (fullFloat) {
                pointers[2][i] = reinterpret_cast<char*>(&s->r), pointers[3][i] = reinterpret_cast<char*>(&s->g);
                pointers[4][i] = reinterpret_cast<char*>(&s->b), pointers[5][i] = reinterpret_cast<char*>(&s->a);
            } else {
                half* q = colour.data() + size_t(f.offsets[i]) * 4;
                for (int k = 0; k < 4; ++k) pointers[size_t(2 + k)][i] = reinterpret_cast<char*>(q + k);
            }
        }
        Imf::DeepFrameBuffer fb;
        fb.insertSampleCountSlice(Imf::Slice(Imf::UINT, reinterpret_cast<char*>(counts.data()), sizeof(uint32_t), sizeof(uint32_t) * size_t(f.width)));
        for (int k = 0; k < 6; ++k)
            fb.insert(names[k], Imf::DeepSlice(k < 2 ? Imf::FLOAT : colourType, reinterpret_cast<char*>(pointers[size_t(k)].data()), sizeof(char*),
                                               sizeof(char*) * size_t(f.width), k < 2 || fullFloat ? sizeof(DeepSample) : 4 * sizeof(half)));
        if (tiled) {
            header.setTileDescription(Imf::TileDescription(64, 64, Imf::ONE_LEVEL));
            Imf::DeepTiledOutputFile out(file.c_str(), header);
            out.setFrameBuffer(fb);
            out.writeTiles(0, out.numXTiles(0) - 1, 0, out.numYTiles(0) - 1, 0, 0);
        } else {
            Imf::DeepScanLineOutputFile out(file.c_str(), header);
            out.setFrameBuffer(fb);
            out.writePixels(f.height);
        }
        return true;
    } catch (const std::exception& e) {
        if (error) *error = "Cannot write " + file + ": " + e.what();
        return false;
    }
#else
    (void)file, (void)f, (void)compression, (void)tiled, (void)fullFloat;
    if (error) *error = "This build of Montage writes deep OpenEXR only with the OpenEXR library";
    return false;
#endif
}

bool compositeDeepWithOpenExr(const std::vector<std::string>& files, int& width, int& height, std::vector<float>& rgba, std::string* error) {
#ifdef MONTAGE_WITH_OPENEXR
    try {
        useThreads();
        std::vector<std::unique_ptr<Imf::DeepScanLineInputFile>> in;
        Imf::CompositeDeepScanLine comp;
        for (const std::string& f : files) {
            in.push_back(std::make_unique<Imf::DeepScanLineInputFile>(f.c_str()));
            comp.addSource(in.back().get());
        }
        const Imath::Box2i dw = comp.dataWindow();
        width = dw.max.x - dw.min.x + 1, height = dw.max.y - dw.min.y + 1;
        rgba.assign(size_t(width) * size_t(height) * 4, 0.0f);
        Imf::FrameBuffer fb;
        const ptrdiff_t origin = (ptrdiff_t(dw.min.y) * width + dw.min.x) * 4;
        const char* names[] = {"R", "G", "B", "A"};
        for (int k = 0; k < 4; ++k)
            fb.insert(names[k], Imf::Slice(Imf::FLOAT, reinterpret_cast<char*>(rgba.data() - origin + k), 4 * sizeof(float), size_t(width) * 4 * sizeof(float)));
        comp.setFrameBuffer(fb);
        comp.readPixels(dw.min.y, dw.max.y);
        return true;
    } catch (const std::exception& e) {
        if (error) *error = std::string("Cannot composite: ") + e.what();
        return false;
    }
#else
    (void)files, (void)width, (void)height, (void)rgba;
    if (error) *error = "This build of Montage has no OpenEXR library";
    return false;
#endif
}

namespace {
std::mutex deepMutex;
// 1 GB, or $MONTAGE_DEEP_CACHE_MB.
size_t deepBudget = [] {
    const char* mb = std::getenv("MONTAGE_DEEP_CACHE_MB");
    const long long v = mb ? std::atoll(mb) : 0;
    return v > 0 ? size_t(v) << 20 : size_t(1) << 30;
}();
std::list<std::pair<std::string, std::shared_ptr<const DeepFrame>>> deepRecent;  // newest first

void trimDeep() {
    size_t bytes = 0, kept = 0;
    for (auto it = deepRecent.begin(); it != deepRecent.end(); ++it, ++kept) {
        bytes += it->second->bytes();
        if (kept >= 1 && bytes > deepBudget) {
            deepRecent.erase(it, deepRecent.end());
            break;
        }
    }
}
}  // namespace

std::shared_ptr<const DeepFrame> cachedDeepExr(const std::string& file, std::string* error) {
    const std::string key = fileKey(file);
    {
        std::lock_guard<std::mutex> lock(deepMutex);
        for (auto it = deepRecent.begin(); it != deepRecent.end(); ++it)
            if (it->first == key) {
                deepRecent.splice(deepRecent.begin(), deepRecent, it);
                return it->second;
            }
    }
    auto f = std::make_shared<DeepFrame>();
    if (!readDeepExr(file, *f, error)) return nullptr;
    tidyDeep(*f);
    std::lock_guard<std::mutex> lock(deepMutex);
    for (auto it = deepRecent.begin(); it != deepRecent.end(); ++it)
        if (it->first == key) {  // (read meanwhile by another thread)
            deepRecent.splice(deepRecent.begin(), deepRecent, it);
            return it->second;
        }
    deepRecent.emplace_front(key, f);
    trimDeep();
    return f;
}

void setDeepCacheBudget(size_t bytes) {
    std::lock_guard<std::mutex> lock(deepMutex);
    deepBudget = bytes;
    trimDeep();
}

size_t deepCacheBudget() {
    std::lock_guard<std::mutex> lock(deepMutex);
    return deepBudget;
}

size_t deepCacheBytes() {
    std::lock_guard<std::mutex> lock(deepMutex);
    size_t bytes = 0;
    for (const auto& [k, f] : deepRecent) bytes += f->bytes();
    return bytes;
}

}  // namespace montage
