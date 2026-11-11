// Montage — ST 2110-22's JPEG XS as RFC 9134 carries it: the boxes before each codestream, its packets in either
// packetization mode, and codestreams rebuilt from them (a lost packet's slices taken from the frame before).
#include "St2110.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "media/JpegXs.h"

namespace montage::st2110 {

namespace {

void put16(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 8);
    p[1] = uint8_t(v);
}
void put32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}
uint32_t get16(const uint8_t* p) { return uint32_t(p[0]) << 8 | p[1]; }
uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

void box(std::vector<uint8_t>& out, const char* type, size_t contentBytes) {
    const size_t at = out.size();
    out.resize(at + 8);
    put32(out.data() + at, uint32_t(8 + contentBytes));
    std::memcpy(out.data() + at + 4, type, 4);
}
void add16(std::vector<uint8_t>& out, uint32_t v) {
    out.resize(out.size() + 2);
    put16(out.data() + out.size() - 2, v);
}
void add32(std::vector<uint8_t>& out, uint32_t v) {
    out.resize(out.size() + 4);
    put32(out.data() + out.size() - 4, v);
}

// The boxes' bytes at the start of a packetization unit (each box its length and type, up to the codestream's SOC
// marker); 0 when they do not end within `n` bytes.
size_t boxBytes(const uint8_t* p, size_t n) {
    size_t at = 0;
    while (at + 2 <= n && !(p[at] == 0xFF && p[at + 1] == 0x10)) {
        if (at + 8 > n) return 0;
        const uint32_t len = get32(p + at);
        if (len < 8 || len > n - at) return 0;
        at += len;
    }
    return at + 2 <= n ? at : 0;
}

// Where each slice of a whole codestream starts (its SLH marker: FF20h, its length 4 and its index), and its end; empty
// when they are not found in order.
std::vector<size_t> sliceStarts(const std::vector<uint8_t>& cs) {
    JxsHeader h;
    if (!readJxsHeader(cs.data(), cs.size(), h) || h.sliceLines() <= 0) return {};
    const int slices = (h.height + h.sliceLines() - 1) / h.sliceLines();
    auto slh = [&](size_t at, int index) {
        return at + 6 <= cs.size() && cs[at] == 0xFF && cs[at + 1] == 0x20 && get16(&cs[at + 2]) == 4 && get16(&cs[at + 4]) == uint32_t(index);
    };
    std::vector<size_t> out;
    size_t at = h.headerBytes;
    if (!slh(at, 0)) return {};
    out.push_back(at);
    for (int s = 1; s < slices; ++s) {
        size_t next = at + 6;
        while (next + 6 <= cs.size() && !slh(next, s)) ++next;
        if (next + 6 > cs.size()) return {};
        out.push_back(at = next);
    }
    out.push_back(cs.size());
    return out;
}

}  // namespace

JxsPicture jxsPictureOf(const Raster& r) {
    const JxsSampling s = r.sampling == Sampling::Ycc422 ? JxsSampling::Ycc422 : r.sampling == Sampling::Ycc444 ? JxsSampling::Ycc444 : JxsSampling::Rgb;
    JxsPicture p(r.width, r.height, r.depth, s);
    for (int y = 0; y < r.height; ++y) {
        const uint16_t* in = r.row(y);
        uint16_t* py = p.planes[0].data() + size_t(y) * size_t(p.planeWidth(0));
        uint16_t* pb = p.planes[1].data() + size_t(y) * size_t(p.planeWidth(1));
        uint16_t* pr = p.planes[2].data() + size_t(y) * size_t(p.planeWidth(2));
        if (r.sampling == Sampling::Ycc422) {
            // (Cb Y Cr Y; the width even)
            for (int x = 0; x + 1 < r.width; x += 2) {
                pb[x / 2] = in[2 * x];
                py[x] = in[2 * x + 1];
                pr[x / 2] = in[2 * x + 2];
                py[x + 1] = in[2 * x + 3];
            }
        } else if (r.sampling == Sampling::Ycc444) {
            // (Cb Y Cr)
            for (int x = 0; x < r.width; ++x) pb[x] = in[3 * x], py[x] = in[3 * x + 1], pr[x] = in[3 * x + 2];
        } else {
            for (int x = 0; x < r.width; ++x) py[x] = in[3 * x], pb[x] = in[3 * x + 1], pr[x] = in[3 * x + 2];
        }
    }
    return p;
}

Raster rasterOf(const JxsPicture& p) {
    const Sampling s = p.sampling == JxsSampling::Ycc422 ? Sampling::Ycc422 : p.sampling == JxsSampling::Ycc444 ? Sampling::Ycc444 : Sampling::Rgb;
    Raster r(p.width, p.height, s, p.depth);
    for (int y = 0; y < p.height; ++y) {
        uint16_t* out = r.row(y);
        const uint16_t* py = p.planes[0].data() + size_t(y) * size_t(p.planeWidth(0));
        const uint16_t* pb = p.planes[1].data() + size_t(y) * size_t(p.planeWidth(1));
        const uint16_t* pr = p.planes[2].data() + size_t(y) * size_t(p.planeWidth(2));
        if (s == Sampling::Ycc422) {
            for (int x = 0; x + 1 < p.width; x += 2) {
                out[2 * x] = pb[x / 2];
                out[2 * x + 1] = py[x];
                out[2 * x + 2] = pr[x / 2];
                out[2 * x + 3] = py[x + 1];
            }
        } else if (s == Sampling::Ycc444) {
            for (int x = 0; x < p.width; ++x) out[3 * x] = pb[x], out[3 * x + 1] = py[x], out[3 * x + 2] = pr[x];
        } else {
            for (int x = 0; x < p.width; ++x) out[3 * x] = py[x], out[3 * x + 1] = pb[x], out[3 * x + 2] = pr[x];
        }
    }
    return r;
}

std::vector<uint8_t> jxsvBoxes(const VideoFormat& f, uint16_t ppih, uint16_t plev, uint32_t codestreamBytes) {
    std::vector<uint8_t> out;
    // The video support box: its information box (the maximum bit rate in Mb/s, rounded up; the frame rate as
    // progressive, a whole numerator over 1 or 1.001; the sampling, said only for 4:2:2 at its depth; no time code)
    // and its profile and level box.
    box(out, "jpvs", 22 + 12);
    box(out, "jpvi", 14);
    const double fps = f.fps.toDouble();
    add32(out, uint32_t(std::ceil(double(codestreamBytes) * 8 * fps / 1e6)));
    const bool ntsc = f.fps.den == 1001;
    const uint32_t numerator = ntsc ? uint32_t(f.fps.num / 1000) : uint32_t(std::lround(fps));
    add32(out, (ntsc ? 2u : 1u) << 24 | (numerator & 0xFFFF));
    add16(out, f.sampling == Sampling::Ycc422 ? 0x8000u | uint32_t(f.depth - 1) << 4 : 0u);
    add32(out, 0);
    box(out, "jxpl", 4);
    add16(out, ppih);
    add16(out, plev);
    // The colour specification box: method 5 (CICP), its primaries, transfer and matrix, narrow range.
    box(out, "colr", 10);
    out.push_back(5);
    out.push_back(0);
    out.push_back(0);
    const bool hdr = f.colorimetry == "BT2100" || f.tcs == "PQ" || f.tcs == "HLG";
    const bool wide = hdr || f.colorimetry == "BT2020";
    const uint32_t transfer = f.tcs == "PQ" ? 16 : f.tcs == "HLG" ? 18 : wide ? (f.depth > 10 ? 15 : 14) : 1;
    add16(out, wide ? 9 : 1);
    add16(out, transfer);
    add16(out, f.sampling == Sampling::Rgb ? 0 : wide ? 9 : 1);
    out.push_back(0);
    return out;
}

namespace {

// The payload header: T (sequential), K, L, I (progressive), F, SEP and P.
uint32_t payloadHeader(int mode, bool last, int64_t frame, uint32_t sep, uint32_t p) {
    return 1u << 31 | uint32_t(mode & 1) << 30 | uint32_t(last) << 29 | uint32_t(frame & 31) << 22 | (sep & 0x7FF) << 11 | (p & 0x7FF);
}

// A packetization unit of `size` bytes from `parts` (pieces read in turn) in packets of `chunk` bytes.
template <typename Read>
bool packUnit(size_t size, size_t chunk, int mode, bool endsFrame, int64_t frame, uint32_t sep, uint32_t timestamp, RtpState& rtp, const PacketSink& send,
              const Read& read) {
    std::vector<uint8_t> packet;
    uint32_t index = 0;
    for (size_t at = 0; at < size || (size == 0 && index == 0); at += chunk, ++index) {
        const size_t n = std::min(chunk, size - at);
        const bool last = at + n >= size;
        packet.assign(16 + n, 0);
        packet[0] = 0x80;
        packet[1] = uint8_t((last && endsFrame ? 0x80 : 0) | (rtp.payloadType & 0x7F));
        put16(packet.data() + 2, rtp.sequence & 0xFFFF);
        put32(packet.data() + 4, timestamp);
        put32(packet.data() + 8, rtp.ssrc);
        // (the codestream mode's packets counted on by SEP past P's 2048)
        const uint32_t s = mode == 0 ? index >> 11 : sep, p = index & 0x7FF;
        put32(packet.data() + 12, payloadHeader(mode, last, frame, s, p));
        read(at, n, packet.data() + 16);
        ++rtp.sequence;
        if (!send(packet.data(), packet.size())) return false;
        if (size == 0) break;
    }
    return true;
}

}  // namespace

bool packJxsv(const std::vector<uint8_t>& boxes, const uint8_t* codestream, size_t size, const std::vector<uint32_t>& units, int packetMode,
              int64_t frameCount, uint32_t timestamp, RtpState& rtp, const PacketSink& send, int maxPayload) {
    if (!codestream || size == 0) return false;
    const size_t chunk = size_t(std::max(64, maxPayload));
    if (packetMode == 0) {
        // One unit: the boxes, then the codestream.
        const size_t total = boxes.size() + size;
        return packUnit(total, chunk, 0, true, frameCount, 0, timestamp, rtp, send, [&](size_t at, size_t n, uint8_t* out) {
            for (size_t i = 0; i < n; ++i) {
                const size_t k = at + i;
                out[i] = k < boxes.size() ? boxes[k] : codestream[k - boxes.size()];
            }
        });
    }
    // The header segment (the boxes and the codestream's header), then a unit for each slice (the last with EOC).
    size_t sum = 0;
    for (uint32_t u : units) sum += u;
    if (units.size() < 2 || sum != size) return false;
    const size_t head = boxes.size() + units[0];
    if (!packUnit(head, chunk, 1, false, frameCount, 0x7FF, timestamp, rtp, send, [&](size_t at, size_t n, uint8_t* out) {
            for (size_t i = 0; i < n; ++i) {
                const size_t k = at + i;
                out[i] = k < boxes.size() ? boxes[k] : codestream[k - boxes.size()];
            }
        }))
        return false;
    size_t at = units[0];
    for (size_t s = 1; s < units.size(); ++s) {
        const uint8_t* slice = codestream + at;
        if (!packUnit(units[s], chunk, 1, s + 1 == units.size(), frameCount, uint32_t((s - 1) % 2047), timestamp, rtp, send,
                      [&](size_t from, size_t n, uint8_t* out) { std::memcpy(out, slice + from, n); }))
            return false;
        at += units[s];
    }
    return true;
}

int jxsvPacketCount(size_t boxBytes, size_t size, const std::vector<uint32_t>& units, int packetMode, int maxPayload) {
    const size_t chunk = size_t(std::max(64, maxPayload));
    auto packets = [&](size_t n) { return int((n + chunk - 1) / chunk); };
    if (packetMode == 0 || units.size() < 2) return packets(boxBytes + size);
    int n = packets(boxBytes + units[0]);
    for (size_t s = 1; s < units.size(); ++s) n += packets(units[s]);
    return n;
}

// ---- Receiving.

bool JxsvAssembler::add(const RtpPacket& p) {
    if (p.size < 4) return false;
    int64_t gap = 0;
    if (lastSeq_ >= 0) {
        const uint16_t d = uint16_t(p.sequence - uint16_t(lastSeq_ + 1));
        if (d >= 0x8000) return false;  // (one from before: the paths' merger gives them in order)
        gap = d;
    }
    lastSeq_ = p.sequence;
    lost_ += gap;
    bool completed = false;
    if (started_ && p.timestamp != timestamp_) {
        // (the frame being built ended without its last packet: what went missing between was its)
        frameLost_ += gap;
        gap = 0;
        completed = finish();
    }
    if (!started_) {
        started_ = true;
        timestamp_ = p.timestamp;
        parts_.clear();
        frameLost_ = 0;
    }
    frameLost_ += gap;
    const uint32_t h = get32(p.payload);
    mode_ = (h >> 30) & 1;
    Part part;
    part.sep = int((h >> 11) & 0x7FF);
    part.p = int(h & 0x7FF);
    part.last = (h >> 29) & 1;
    part.data.assign(p.payload + 4, p.payload + p.size);
    if (parts_.size() < 1000000) parts_.push_back(std::move(part));
    if (p.marker) completed = finish() || completed;
    return completed;
}

bool JxsvAssembler::finish() {
    started_ = false;
    std::vector<Part> parts = std::move(parts_);
    parts_.clear();
    int64_t lost = frameLost_;
    frameLost_ = 0;
    if (parts.empty()) return false;
    for (const Part& q : parts)
        if (!q.last) unit_ = std::max(unit_, q.data.size());
    const bool slices = mode_ == 1;
    // (slice indices counted on past SEP's 2047, in the order they came)
    std::vector<int> sliceOf(parts.size(), -1);
    {
        int base = 0, prevSep = -1;
        for (size_t i = 0; i < parts.size(); ++i) {
            if (!slices || parts[i].sep == 0x7FF) continue;
            if (prevSep >= 0 && parts[i].sep + 1024 < prevSep) base += 2047;
            prevSep = parts[i].sep;
            sliceOf[i] = base + parts[i].sep;
        }
    }
    const Part& first = parts.front();
    const bool startsWhole = first.p == 0 && (slices ? first.sep == 0x7FF : first.sep == 0);
    if (lost == 0 && startsWhole && parts.back().last) {
        // Whole: its units in turn, the boxes taken off the front.
        std::vector<uint8_t> unit;
        for (const Part& q : parts) unit.insert(unit.end(), q.data.begin(), q.data.end());
        const size_t boxes = boxBytes(unit.data(), unit.size());
        JxsHeader h;
        if (boxes == 0 && !(unit.size() >= 2 && unit[0] == 0xFF && unit[1] == 0x10)) return ++passedOver_, false;
        std::vector<uint8_t> cs(unit.begin() + ptrdiff_t(boxes), unit.end());
        if (!readJxsHeader(cs.data(), cs.size(), h)) return ++passedOver_, false;
        // Where its slices start, for a frame after that misses some: as its units say, or found in the codestream.
        std::vector<size_t> starts;
        if (slices) {
            size_t at = 0;
            int current = -2;
            for (size_t i = 0; i < parts.size(); ++i) {
                const int s = parts[i].sep == 0x7FF ? -1 : sliceOf[i];
                if (s != current && s >= 0) starts.push_back(at >= boxes ? at - boxes : 0);
                current = s;
                at += parts[i].data.size();
            }
            starts.push_back(cs.size());
        } else {
            starts = sliceStarts(cs);
        }
        prev_ = cs;
        prevSlices_ = starts;
        prevBoxes_ = boxes;
        done_ = std::move(cs);
        doneTimestamp_ = timestamp_;
        doneLost_ = 0;
        doneConcealed_ = 0;
        return true;
    }
    // Missing packets: what came put in its place in the frame before's layout, the slices with anything missing taken
    // whole from that frame.
    if (prev_.empty() || prevSlices_.size() < 2 || unit_ == 0) return ++passedOver_, false;
    const size_t size = prev_.size();
    std::vector<uint8_t> cs(size, 0), known(size, 0);
    size_t boxes = prevBoxes_;
    for (const Part& q : parts)
        if (q.p == 0 && (slices ? q.sep == 0x7FF : q.sep == 0)) {
            const size_t b = boxBytes(q.data.data(), q.data.size());
            if (b) boxes = b;
        }
    const size_t sliceCount = prevSlices_.size() - 1;
    for (size_t i = 0; i < parts.size(); ++i) {
        const Part& q = parts[i];
        int64_t at;
        if (!slices) {
            at = (int64_t(q.sep) * 2048 + q.p) * int64_t(unit_) - int64_t(boxes);
        } else if (q.sep == 0x7FF) {
            at = int64_t(q.p) * int64_t(unit_) - int64_t(boxes);
        } else {
            if (sliceOf[i] < 0 || size_t(sliceOf[i]) >= sliceCount) return ++passedOver_, false;
            at = int64_t(prevSlices_[size_t(sliceOf[i])]) + int64_t(q.p) * int64_t(unit_);
            // (a slice's packets stay within it)
            if (at + int64_t(q.data.size()) > int64_t(prevSlices_[size_t(sliceOf[i]) + 1])) return ++passedOver_, false;
        }
        for (size_t k = 0; k < q.data.size(); ++k) {
            const int64_t to = at + int64_t(k);
            if (to < 0) continue;  // (the boxes)
            if (to >= int64_t(size)) return ++passedOver_, false;  // (not the frame before's layout)
            cs[size_t(to)] = q.data[k];
            known[size_t(to)] = 1;
        }
    }
    auto whole = [&](size_t a, size_t b) { return std::all_of(known.begin() + ptrdiff_t(a), known.begin() + ptrdiff_t(b), [](uint8_t k) { return k != 0; }); };
    // The header (the same each frame), and each slice.
    if (!whole(0, prevSlices_[0])) std::copy(prev_.begin(), prev_.begin() + ptrdiff_t(prevSlices_[0]), cs.begin());
    int concealed = 0;
    for (size_t s = 0; s < sliceCount; ++s) {
        const size_t a = prevSlices_[s], b = prevSlices_[s + 1];
        if (whole(a, b)) {
            // (a slice that came must start where the frame before's did: else the layout is another, and the frame
            // is passed over sooner than put together wrongly)
            if (b - a < 6 || cs[a] != 0xFF || cs[a + 1] != 0x20 || get16(&cs[a + 4]) != uint32_t(s)) return ++passedOver_, false;
            continue;
        }
        std::copy(prev_.begin() + ptrdiff_t(a), prev_.begin() + ptrdiff_t(b), cs.begin() + ptrdiff_t(a));
        ++concealed;
    }
    JxsHeader h;
    if (!readJxsHeader(cs.data(), cs.size(), h) || (h.lcod && h.lcod != cs.size())) return ++passedOver_, false;
    prev_ = cs;
    done_ = std::move(cs);
    doneTimestamp_ = timestamp_;
    doneLost_ = std::max<int64_t>(1, lost);
    doneConcealed_ = concealed;
    return true;
}

}  // namespace montage::st2110
