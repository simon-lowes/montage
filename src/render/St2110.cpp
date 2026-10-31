#include "St2110.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <ctime>
#endif

#include "core/Cea708.h"
#include "render/VideoPacking.h"

namespace montage::st2110 {

namespace {

#ifdef _WIN32
using sock_t = SOCKET;
const sock_t kBad = INVALID_SOCKET;
void closeSock(sock_t s) { closesocket(s); }
int pollIn(sock_t s, int ms) {
    WSAPOLLFD p{s, POLLRDNORM, 0};
    return WSAPoll(&p, 1, ms) > 0 ? 1 : 0;
}
#else
using sock_t = int;
const sock_t kBad = -1;
void closeSock(sock_t s) { ::close(s); }
int pollIn(sock_t s, int ms) {
    pollfd p{s, POLLIN, 0};
    return ::poll(&p, 1, ms) > 0 && (p.revents & POLLIN) ? 1 : 0;
}
#endif

void netInit() {
#ifdef _WIN32
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
    });
#endif
}

template <typename T>
void option(sock_t s, int level, int name, T value) {
    setsockopt(s, level, name, reinterpret_cast<const char*>(&value), sizeof value);
}

sock_t udpSocket() {
    netInit();
    sock_t s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#ifdef _WIN32
    if (s != kBad) SetHandleInformation(reinterpret_cast<HANDLE>(s), HANDLE_FLAG_INHERIT, 0);
#else
    if (s != kBad) fcntl(s, F_SETFD, FD_CLOEXEC);
#endif
    return s;
}

bool ipv4(const std::string& address, in_addr& out) { return !address.empty() && inet_pton(AF_INET, address.c_str(), &out) == 1; }

sockaddr_in socketAddress(const Endpoint& e) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(uint16_t(e.port));
    if (!ipv4(e.address, a.sin_addr)) a.sin_addr.s_addr = htonl(INADDR_ANY);
    return a;
}

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

// The 12-byte RTP header (version 2, no padding, extension or CSRCs), the sequence number's low 16 bits.
void rtpHeader(uint8_t* p, bool marker, const RtpState& rtp, uint32_t timestamp) {
    p[0] = 0x80;
    p[1] = uint8_t((marker ? 0x80 : 0) | (rtp.payloadType & 0x7F));
    put16(p + 2, rtp.sequence & 0xFFFF);
    put32(p + 4, timestamp);
    put32(p + 8, rtp.ssrc);
}

// a * b / c, rounded down, without overflowing where a * b would (a >= 0; (c - 1) * b fits).
int64_t mulDiv(int64_t a, int64_t b, int64_t c) { return a / c * b + a % c * b / c; }

std::string rateText(Rational r) { return r.den == 1 ? std::to_string(r.num) : std::to_string(r.num) + "/" + std::to_string(r.den); }

bool tenBit(output::PixelFormat f) { return f == output::PixelFormat::V210; }

// A row's Cb Y Cr Y components (2 a pixel) from V210 or UYVY.
void rowComponents(const uint8_t* row, output::PixelFormat format, int width, uint16_t* out) {
    const int n = width * 2;
    if (format == output::PixelFormat::Uyvy8) {
        for (int i = 0; i < n; ++i) out[i] = row[i];
        return;
    }
    for (int g = 0, i = 0; i < n; ++g) {
        for (int w = 0; w < 4 && i < n; ++w) {
            const uint8_t* p = row + size_t(g) * 16 + size_t(w) * 4;
            const uint32_t v = uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
            for (int k = 0; k < 3 && i < n; ++k) out[i++] = uint16_t((v >> (10 * k)) & 0x3FF);
        }
    }
}

// And back, the row's padding zeroed.
void componentsRow(const uint16_t* in, output::PixelFormat format, int width, uint8_t* row, size_t rowSize) {
    std::memset(row, 0, rowSize);
    const int n = width * 2;
    if (format == output::PixelFormat::Uyvy8) {
        for (int i = 0; i < n; ++i) row[i] = uint8_t(in[i]);
        return;
    }
    for (int g = 0, i = 0; i < n; ++g) {
        for (int w = 0; w < 4 && i < n; ++w) {
            uint32_t v = 0;
            for (int k = 0; k < 3 && i < n; ++k) v |= uint32_t(in[i++] & 0x3FF) << (10 * k);
            uint8_t* p = row + size_t(g) * 16 + size_t(w) * 4;
            p[0] = uint8_t(v), p[1] = uint8_t(v >> 8), p[2] = uint8_t(v >> 16), p[3] = uint8_t(v >> 24);
        }
    }
}

// Pixel groups (2 pixels: Cb Y0 Cr Y1, most significant bit first): 5 bytes at 10 bits, 4 at 8.
void toPgroups(const uint16_t* c, int pixels, bool ten, uint8_t* out) {
    for (int i = 0; i < pixels * 2; i += 4, out += ten ? 5 : 4) {
        if (!ten) {
            for (int k = 0; k < 4; ++k) out[k] = uint8_t(c[i + k]);
            continue;
        }
        const uint32_t a = c[i], b = c[i + 1], d = c[i + 2], e = c[i + 3];
        out[0] = uint8_t(a >> 2);
        out[1] = uint8_t((a & 3) << 6 | b >> 4);
        out[2] = uint8_t((b & 15) << 4 | d >> 6);
        out[3] = uint8_t((d & 63) << 2 | e >> 8);
        out[4] = uint8_t(e);
    }
}
void fromPgroups(const uint8_t* in, int pixels, bool ten, uint16_t* c) {
    for (int i = 0; i < pixels * 2; i += 4, in += ten ? 5 : 4) {
        if (!ten) {
            for (int k = 0; k < 4; ++k) c[i + k] = in[k];
            continue;
        }
        c[i] = uint16_t(in[0] << 2 | in[1] >> 6);
        c[i + 1] = uint16_t((in[1] & 63) << 4 | in[2] >> 4);
        c[i + 2] = uint16_t((in[2] & 15) << 6 | in[3] >> 2);
        c[i + 3] = uint16_t((in[3] & 3) << 8 | in[4]);
    }
}

// ---- ST 291 words: 8 bits, b8 their even parity, b9 its inverse.
uint16_t word10(uint8_t v) {
    int ones = 0;
    for (int i = 0; i < 8; ++i) ones += (v >> i) & 1;
    const uint16_t p = uint16_t(ones & 1);
    return uint16_t(v | p << 8 | (p ^ 1) << 9);
}
bool wordOk(uint16_t w) { return word10(uint8_t(w)) == w; }

struct BitWriter {
    std::vector<uint8_t> bytes;
    int bit = 0;  // bits used in the last byte
    void put(uint32_t v, int n) {
        for (int i = n - 1; i >= 0; --i) {
            if (bit == 0) bytes.push_back(0);
            if ((v >> i) & 1) bytes.back() |= uint8_t(0x80 >> bit);
            bit = (bit + 1) & 7;
        }
    }
    size_t bits() const { return bytes.size() * 8 - (bit ? 8 - bit : 0); }
};
struct BitReader {
    const uint8_t* p;
    size_t size, pos = 0;  // in bits
    bool get(int n, uint32_t& v) {
        if (pos + size_t(n) > size * 8) return false;
        v = 0;
        for (int i = 0; i < n; ++i, ++pos) v = v << 1 | ((p[pos >> 3] >> (7 - (pos & 7))) & 1);
        return true;
    }
};

std::mutex optionsMutex;
SenderOptions& storedOptions() {
    static SenderOptions o;
    return o;
}

}  // namespace

// ---------------------------------------------------------------------------

bool parseEndpoint(const std::string& text, Endpoint& out, int port) {
    std::string address = text;
    const auto colon = text.rfind(':');
    if (colon != std::string::npos) {
        address = text.substr(0, colon);
        const std::string p = text.substr(colon + 1);
        if (p.empty() || p.size() > 5 || p.find_first_not_of("0123456789") != std::string::npos) return false;
        port = std::stoi(p);
    }
    in_addr a;
    if (!ipv4(address, a) || port < 0 || port > 65535) return false;
    out.address = address;
    out.port = port;
    return true;
}

std::string endpointText(const Endpoint& e) { return e.address + ":" + std::to_string(e.port); }

bool isMulticast(const std::string& address) {
    in_addr a;
    return ipv4(address, a) && (ntohl(a.s_addr) >> 28) == 0xE;
}

int64_t ptpNowNs(std::string* source) {
    using namespace std::chrono;
#ifdef __linux__
    // CLOCK_TAI runs ahead of UTC by the kernel's TAI offset, which phc2sys (or ptp4l's servo) sets; 0 when nothing has.
    timespec tai{}, utc{};
    clock_gettime(CLOCK_TAI, &tai);
    clock_gettime(CLOCK_REALTIME, &utc);
    const int64_t taiNs = int64_t(tai.tv_sec) * 1000000000 + tai.tv_nsec, utcNs = int64_t(utc.tv_sec) * 1000000000 + utc.tv_nsec;
    if (taiNs - utcNs > 10 * int64_t(1000000000)) {
        if (source) *source = "TAI clock";
        return taiNs;
    }
#endif
    if (source) *source = "system clock + 37 s";
    return duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count() + 37 * int64_t(1000000000);
}

// ---- SDP.

namespace {

std::string sdpHead(const Endpoint& to, const SdpOptions& o, const std::string& media, int pt) {
    const std::string source = o.source.empty() ? "0.0.0.0" : o.source;
    std::ostringstream s;
    s << "v=0\r\n"
      << "o=- " << o.session << " " << o.session << " IN IP4 " << source << "\r\n"
      << "s=" << o.name << "\r\n"
      << "t=0 0\r\n"
      << "m=" << media << " " << to.port << " RTP/AVP " << pt << "\r\n"
      << "c=IN IP4 " << to.address << (isMulticast(to.address) ? "/" + std::to_string(o.ttl) : std::string()) << "\r\n";
    if (isMulticast(to.address) && !o.source.empty()) s << "a=source-filter: incl IN IP4 " << to.address << " " << o.source << "\r\n";
    return s.str();
}

std::string sdpClock(const SdpOptions& o) { return "a=ts-refclk:" + o.refclk + "\r\na=mediaclk:direct=0\r\n"; }

}  // namespace

std::string videoSdp(const VideoFormat& f, const Endpoint& to, int pt, const SdpOptions& o) {
    std::ostringstream s;
    s << sdpHead(to, o, "video", pt) << "a=rtpmap:" << pt << " raw/90000\r\n"
      << "a=fmtp:" << pt << " sampling=YCbCr-4:2:2; width=" << f.width << "; height=" << f.height
      << "; exactframerate=" << rateText(f.fps) << "; depth=" << f.depth << "; TCS=" << f.tcs << "; colorimetry=" << f.colorimetry
      << "; RANGE=NARROW; PM=2110GPM; SSN=ST2110-20:2017; TP=2110TPW" << (f.interlaced ? "; interlace" : "") << "\r\n"
      << sdpClock(o);
    return s.str();
}

std::string audioSdp(const AudioFormat& f, const Endpoint& to, int pt, const SdpOptions& o) {
    // Channel order: a stereo pair, or as many pairs as there are, an odd one last a mono channel (ST 2110-30's symbols).
    std::string order = "SMPTE2110.(";
    for (int i = 0; i < f.channels / 2; ++i) order += i ? ",ST" : "ST";
    if (f.channels % 2) order += f.channels > 1 ? ",M" : "M";
    order += ")";
    std::ostringstream s;
    s << sdpHead(to, o, "audio", pt) << "a=rtpmap:" << pt << " L24/" << f.rate << "/" << f.channels << "\r\n"
      << "a=fmtp:" << pt << " channel-order=" << order << "\r\n"
      << "a=ptime:1\r\n"
      << sdpClock(o);
    return s.str();
}

std::string ancSdp(const VideoFormat& f, const Endpoint& to, int pt, const SdpOptions& o, const std::vector<std::pair<int, int>>& ids) {
    std::ostringstream s;
    s << sdpHead(to, o, "video", pt) << "a=rtpmap:" << pt << " smpte291/90000\r\n"
      << "a=fmtp:" << pt << " ";
    char id[32];
    for (const auto& [did, sdid] : ids) {
        std::snprintf(id, sizeof id, "DID_SDID={0x%02X,0x%02X}; ", did & 0xFF, sdid & 0xFF);
        s << id;
    }
    s << "exactframerate=" << rateText(f.fps) << "; SSN=ST2110-40:2018\r\n"
      << sdpClock(o);
    return s.str();
}

std::map<std::string, std::string> sdpParameters(const std::string& sdp, std::string* encoding) {
    std::map<std::string, std::string> out;
    std::istringstream in(sdp);
    std::string line;
    auto trim = [](std::string t) {
        const auto a = t.find_first_not_of(" \t\r"), b = t.find_last_not_of(" \t\r");
        return a == std::string::npos ? std::string() : t.substr(a, b - a + 1);
    };
    while (std::getline(in, line)) {
        line = trim(line);
        if (encoding && line.rfind("a=rtpmap:", 0) == 0) {
            const auto sp = line.find(' ');
            if (sp != std::string::npos) *encoding = trim(line.substr(sp + 1));
        }
        if (line.rfind("a=fmtp:", 0) != 0) continue;
        const auto sp = line.find(' ');
        if (sp == std::string::npos) continue;
        std::istringstream params(line.substr(sp + 1));
        std::string p;
        while (std::getline(params, p, ';')) {
            p = trim(p);
            if (p.empty()) continue;
            const auto eq = p.find('=');
            const std::string name = eq == std::string::npos ? p : p.substr(0, eq), value = eq == std::string::npos ? std::string() : p.substr(eq + 1);
            auto [it, fresh] = out.emplace(name, value);
            if (!fresh) it->second += "," + value;  // (DID_SDID, given once for each)
        }
    }
    return out;
}

// ---- RTP.

bool parseRtp(const uint8_t* data, size_t size, RtpPacket& out) {
    if (size < 12 || (data[0] >> 6) != 2) return false;
    size_t head = 12 + size_t(data[0] & 15) * 4;
    if (data[0] & 0x10) {  // (a header extension)
        if (size < head + 4) return false;
        head += 4 + size_t(get16(data + head + 2)) * 4;
    }
    size_t end = size;
    if (data[0] & 0x20) {  // (padding)
        if (data[size - 1] > size - head) return false;
        end -= data[size - 1];
    }
    if (head > end) return false;
    out.marker = data[1] & 0x80;
    out.payloadType = data[1] & 0x7F;
    out.sequence = uint16_t(get16(data + 2));
    out.timestamp = get32(data + 4);
    out.ssrc = get32(data + 8);
    out.payload = data + head;
    out.size = end - head;
    return true;
}

// ---- ST 2110-20.

namespace {

int pgroupBytes(bool ten) { return ten ? 5 : 4; }

// Bytes of pixels a packet carries: whole pixel groups, at most `maxPayload`.
int packetData(bool ten, int maxPayload) { return std::max(1, maxPayload / pgroupBytes(ten)) * pgroupBytes(ten); }

}  // namespace

bool packVideo(const uint8_t* data, size_t stride, output::PixelFormat format, int width, int height, int field, uint32_t timestamp,
               RtpState& rtp, const PacketSink& send, int maxPayload) {
    if (format == output::PixelFormat::P216 || width <= 0 || width % 2 || height <= 0 || !data) return false;
    const bool ten = tenBit(format);
    const int rows = field ? (height + (field == 1 ? 1 : 0)) / 2 : height;
    const size_t lineBytes = size_t(width / 2) * size_t(pgroupBytes(ten));
    const size_t chunk = size_t(packetData(ten, maxPayload));
    std::vector<uint16_t> comps(size_t(width) * 2);
    std::vector<uint8_t> line(lineBytes), packet(12 + 2 + 6 + chunk);
    for (int r = 0; r < rows; ++r) {
        const int src = field ? r * 2 + (field - 1) : r;
        rowComponents(data + size_t(src) * stride, format, width, comps.data());
        toPgroups(comps.data(), width, ten, line.data());
        for (size_t off = 0; off < lineBytes; off += chunk) {
            const size_t n = std::min(chunk, lineBytes - off);
            const bool last = r == rows - 1 && off + n == lineBytes;
            uint8_t* p = packet.data();
            rtpHeader(p, last, rtp, timestamp);
            put16(p + 12, rtp.sequence >> 16);
            put16(p + 14, uint32_t(n));
            put16(p + 16, uint32_t(field == 2 ? 0x8000 : 0) | uint32_t(r & 0x7FFF));
            put16(p + 18, uint32_t(off / size_t(pgroupBytes(ten)) * 2) & 0x7FFF);
            std::memcpy(p + 20, line.data() + off, n);
            ++rtp.sequence;
            if (!send(p, 20 + n)) return false;
        }
    }
    return true;
}

int videoPacketCount(int width, int height, int depth, bool interlaced, int maxPayload) {
    const bool ten = depth > 8;
    const int64_t lineBytes = int64_t(width / 2) * pgroupBytes(ten), chunk = packetData(ten, maxPayload);
    (void)interlaced;  // (the same rows, in two fields)
    return int(height * ((lineBytes + chunk - 1) / chunk));
}

VideoAssembler::VideoAssembler(int width, int height, output::PixelFormat format, bool interlaced)
    : width_(width), height_(height), format_(format), interlaced_(interlaced), stride_(output::rowBytes(format, width)) {
    components_.assign(size_t(width) * 2 * size_t(height), 0);
}

bool VideoAssembler::add(const RtpPacket& p) {
    if (p.size < 2 + 6) return false;
    const uint32_t seq = get16(p.payload) << 16 | p.sequence;
    if (lastSeq_ >= 0) {
        const uint32_t gap = seq - uint32_t(lastSeq_) - 1;
        if (gap > 0 && gap < (1u << 20)) lost_ += gap;
    }
    lastSeq_ = seq;
    // The sample row data headers, then their pixels in turn.
    struct Srd {
        uint32_t length, row, offset;
        bool second;
    };
    std::vector<Srd> srds;
    size_t at = 2;
    for (;;) {
        if (at + 6 > p.size) return false;
        const uint32_t len = get16(p.payload + at), fr = get16(p.payload + at + 2), co = get16(p.payload + at + 4);
        srds.push_back({len, fr & 0x7FFF, co & 0x7FFF, (fr & 0x8000) != 0});
        at += 6;
        if (!(co & 0x8000)) break;
    }
    const bool second = !srds.empty() && srds[0].second;
    // A new picture: another timestamp on a frame's (or a first field's) packets.
    if (!second && (!started_ || p.timestamp != timestamp_)) {
        timestamp_ = p.timestamp;
        fieldsDone_ = 0;
        started_ = true;
    }
    const bool ten = tenBit(format_);
    const int pg = pgroupBytes(ten);
    for (const Srd& s : srds) {
        if (at + s.length > p.size) return false;
        const int row = interlaced_ ? int(s.row) * 2 + (s.second ? 1 : 0) : int(s.row);
        const int pixels = int(s.length) / pg * 2;
        if (row < height_ && int(s.offset) + pixels <= width_)
            fromPgroups(p.payload + at, pixels, ten, components_.data() + (size_t(row) * size_t(width_) + s.offset) * 2);
        at += s.length;
    }
    if (!p.marker) return false;
    if (interlaced_ && !second) {
        fieldsDone_ = 1;
        return false;
    }
    if (interlaced_ && fieldsDone_ != 1) return false;  // (a second field without its first)
    finish();
    return true;
}

void VideoAssembler::finish() {
    done_.assign(stride_ * size_t(height_), 0);
    for (int r = 0; r < height_; ++r)
        componentsRow(components_.data() + size_t(r) * size_t(width_) * 2, format_, width_, done_.data() + size_t(r) * stride_, stride_);
    doneTimestamp_ = timestamp_;
    fieldsDone_ = 0;
}

// ---- ST 2110-30.

bool packAudio(const float* samples, int frames, int channels, uint32_t timestamp, RtpState& rtp, const PacketSink& send) {
    if (frames <= 0 || channels <= 0) return false;
    std::vector<uint8_t> packet(12 + size_t(frames) * size_t(channels) * 3);
    rtpHeader(packet.data(), false, rtp, timestamp);
    uint8_t* p = packet.data() + 12;
    for (size_t i = 0, n = size_t(frames) * size_t(channels); i < n; ++i, p += 3) {
        const float x = samples ? samples[i] : 0.0f;
        const double v = std::nearbyint(double(std::isfinite(x) ? x : 0.0f) * 8388608.0);
        const int32_t s = int32_t(std::clamp(v, -8388608.0, 8388607.0));
        p[0] = uint8_t(s >> 16), p[1] = uint8_t(s >> 8), p[2] = uint8_t(s);
    }
    ++rtp.sequence;
    return send(packet.data(), packet.size());
}

std::vector<float> unpackAudio(const uint8_t* payload, size_t size, int channels) {
    std::vector<float> out;
    if (channels <= 0) return out;
    const size_t n = size / 3 / size_t(channels) * size_t(channels);
    out.resize(n);
    for (size_t i = 0; i < n; ++i, payload += 3) {
        int32_t s = int32_t(uint32_t(payload[0]) << 16 | uint32_t(payload[1]) << 8 | payload[2]);
        if (s & 0x800000) s -= 0x1000000;
        out[i] = float(double(s) / 8388608.0);
    }
    return out;
}

// ---- ST 2110-40 (RFC 8331).

bool packAnc(const std::vector<AncPacket>& packets, int field, uint32_t timestamp, RtpState& rtp, const PacketSink& send) {
    BitWriter b;
    int count = 0;
    for (const AncPacket& a : packets) {
        if (a.data.size() > 255 || count == 255) return false;
        b.put(a.chroma ? 1 : 0, 1);
        b.put(uint32_t(a.line) & 0x7FF, 11);
        b.put(uint32_t(a.horizontalOffset) & 0xFFF, 12);
        b.put(0, 1);  // S: no data stream number
        b.put(0, 7);
        uint32_t sum = 0;
        auto word = [&](uint8_t v) {
            const uint16_t w = word10(v);
            sum += w & 0x1FF;
            b.put(w, 10);
        };
        word(a.did);
        word(a.sdid);
        word(uint8_t(a.data.size()));
        for (uint8_t v : a.data) word(v);
        sum &= 0x1FF;
        b.put(sum | ((~sum >> 8) & 1) << 9, 10);
        while (b.bits() % 32) b.put(0, 1);
        ++count;
    }
    std::vector<uint8_t> packet(12 + 8 + b.bytes.size());
    rtpHeader(packet.data(), true, rtp, timestamp);
    uint8_t* p = packet.data() + 12;
    put16(p, rtp.sequence >> 16);
    put16(p + 2, uint32_t(b.bytes.size()));
    p[4] = uint8_t(count);
    p[5] = uint8_t(field == 1 ? 0x80 : field == 2 ? 0xC0 : 0);
    p[6] = p[7] = 0;
    std::memcpy(p + 8, b.bytes.data(), b.bytes.size());
    ++rtp.sequence;
    return send(packet.data(), packet.size());
}

bool unpackAnc(const uint8_t* payload, size_t size, std::vector<AncPacket>& out, int* field) {
    out.clear();
    if (size < 8) return false;
    const size_t length = get16(payload + 2);
    const int count = payload[4];
    const int f = payload[5] >> 6;
    if (field) *field = f == 2 ? 1 : f == 3 ? 2 : 0;
    if (8 + length > size) return false;
    BitReader r{payload + 8, length};
    for (int i = 0; i < count; ++i) {
        AncPacket a;
        uint32_t c, line, offset, s, stream, did, sdid, dc, cs;
        if (!r.get(1, c) || !r.get(11, line) || !r.get(12, offset) || !r.get(1, s) || !r.get(7, stream)) return false;
        if (!r.get(10, did) || !r.get(10, sdid) || !r.get(10, dc)) return false;
        if (!wordOk(uint16_t(did)) || !wordOk(uint16_t(sdid)) || !wordOk(uint16_t(dc))) return false;
        uint32_t sum = (did & 0x1FF) + (sdid & 0x1FF) + (dc & 0x1FF);
        for (uint32_t k = 0; k < (dc & 0xFF); ++k) {
            uint32_t w;
            if (!r.get(10, w) || !wordOk(uint16_t(w))) return false;
            sum += w & 0x1FF;
            a.data.push_back(uint8_t(w));
        }
        if (!r.get(10, cs)) return false;
        sum &= 0x1FF;
        if (cs != (sum | ((~sum >> 8) & 1) << 9)) return false;
        while (r.pos % 32) ++r.pos;
        a.chroma = c != 0;
        a.line = int(line);
        a.horizontalOffset = int(offset);
        a.did = uint8_t(did);
        a.sdid = uint8_t(sdid);
        out.push_back(std::move(a));
    }
    return true;
}

AncPacket atcPacket(const output::Timecode& tc, AtcType type, bool fieldMark, int frameCount) {
    // The 64 information bits of ST 12-1 as 16 nibbles, each in bits 4-7 of a user data word; bit 3 carries the
    // distribution bits (DBB1 in words 1-8: the payload type; DBB2 in words 9-16: 0, a valid time code processed).
    const int f = tc.frames, s = tc.seconds, m = tc.minutes, h = tc.hours;
    uint8_t n[16] = {uint8_t(f % 10), 0, uint8_t((f / 10 & 3) | (tc.dropFrame && frameCount == 30 ? 4 : 0)), 0,
                     uint8_t(s % 10), 0, uint8_t(s / 10 & 7), 0,
                     uint8_t(m % 10), 0, uint8_t(m / 10 & 7), 0,
                     uint8_t(h % 10), 0, uint8_t(h / 10 & 3), 0};
    if (fieldMark && type != AtcType::Ltc) {
        if (frameCount == 25) n[14] |= 8;  // (bit 75)
        else n[6] |= 8;                    // (bit 35)
    }
    AncPacket a;
    a.did = 0x60;
    a.sdid = 0x60;
    a.line = 0x7FF;              // (no particular line)
    a.horizontalOffset = 0xFFE;  // (in HANC, as SDI carries it)
    const uint8_t dbb1 = uint8_t(type);
    for (int i = 0; i < 16; ++i) a.data.push_back(uint8_t(n[i] << 4 | (i < 8 ? ((dbb1 >> i) & 1) << 3 : 0)));
    return a;
}

bool parseAtc(const AncPacket& p, output::Timecode& out, AtcType* type, bool* fieldMark, int frameCount) {
    if (p.did != 0x60 || p.sdid != 0x60 || p.data.size() < 16) return false;
    auto n = [&](int i) { return p.data[size_t(i)] >> 4; };
    int dbb1 = 0;
    for (int i = 0; i < 8; ++i) dbb1 |= ((p.data[size_t(i)] >> 3) & 1) << i;
    if (dbb1 > 2) return false;
    out.frames = n(0) + (n(2) & 3) * 10;
    out.dropFrame = frameCount == 30 && (n(2) & 4);
    out.seconds = n(4) + (n(6) & 7) * 10;
    out.minutes = n(8) + (n(10) & 7) * 10;
    out.hours = n(12) + (n(14) & 3) * 10;
    if (type) *type = AtcType(dbb1);
    if (fieldMark) *fieldMark = dbb1 != 0 && (frameCount == 25 ? (n(14) & 8) : (n(6) & 8));
    return true;
}

std::vector<AncPacket> timecodePackets(const output::Timecode& tc, Rational fps, int field) {
    const double rate = fps.toDouble();
    std::vector<AncPacket> out;
    if (rate > 30.5) {
        // Frame pairs: the time address counts at half the rate, its frames halved.
        output::Timecode pair = tc;
        pair.frames = tc.frames / 2;
        const bool second = tc.frames % 2;
        const int count = int(std::lround(rate / 2)) == 25 ? 25 : int(std::lround(rate / 2)) == 24 ? 24 : 30;
        if (!second) out.push_back(atcPacket(pair, AtcType::Ltc, false, count));
        out.push_back(atcPacket(pair, second ? AtcType::Vitc2 : AtcType::Vitc1, second, count));
        return out;
    }
    const int count = std::lround(rate) == 25 ? 25 : std::lround(rate) == 24 ? 24 : 30;
    if (field != 2) out.push_back(atcPacket(tc, AtcType::Ltc, false, count));
    if (field) out.push_back(atcPacket(tc, field == 2 ? AtcType::Vitc2 : AtcType::Vitc1, field == 2, count));
    return out;
}

namespace {
void be16(std::vector<uint8_t>& v, double x) {
    const auto u = uint32_t(std::clamp(std::lround(x), 0L, 65535L));
    v.push_back(uint8_t(u >> 8));
    v.push_back(uint8_t(u));
}
void be32(std::vector<uint8_t>& v, double x) {
    const auto u = uint32_t(std::clamp<double>(std::round(x), 0, 4294967295.0));
    for (int k = 3; k >= 0; --k) v.push_back(uint8_t(u >> (8 * k)));
}
}  // namespace

AncPacket hdrMetadataPacket(const output::Signal& s) {
    // Primaries G, B, R (HEVC's order), x and y in 0.00002 steps; luminance in 0.0001 cd/m².
    static const double p3[3][2] = {{0.265, 0.690}, {0.150, 0.060}, {0.680, 0.320}};
    static const double bt2020[3][2] = {{0.170, 0.797}, {0.131, 0.046}, {0.708, 0.292}};
    const auto& pr = s.masteringP3 ? p3 : bt2020;
    AncPacket a;
    a.did = 0x41;
    a.sdid = 0x0C;
    std::vector<uint8_t>& d = a.data;
    d = {0x00, 0x1A, 0x89, 0x18};  // static type 1: the mastering display colour volume SEI (137), 24 bytes
    for (const auto& c : pr) {
        be16(d, c[0] * 50000);
        be16(d, c[1] * 50000);
    }
    be16(d, 0.3127 * 50000);
    be16(d, 0.3290 * 50000);
    be32(d, s.masteringPeak * 10000);
    be32(d, s.masteringMin * 10000);
    d.insert(d.end(), {0x01, 0x06, 0x90, 0x04});  // static type 2: content light level (144), 4 bytes
    be16(d, s.maxCll);
    be16(d, s.maxFall);
    return a;
}

bool parseHdrMetadata(const AncPacket& p, HdrMetadata& out) {
    if (p.did != 0x41 || p.sdid != 0x0C) return false;
    out = {};
    const auto& d = p.data;
    auto u16 = [&](size_t i) { return double(uint32_t(d[i]) << 8 | d[i + 1]); };
    for (size_t i = 0; i + 2 <= d.size();) {
        const int type = d[i];
        const size_t len = d[i + 1];
        if (i + 2 + len > d.size()) return false;
        const size_t b = i + 2;
        if (type == 0 && len == 26 && d[b] == 0x89 && d[b + 1] == 0x18) {
            static const int rgb[3] = {1, 2, 0};  // (G B R in, R G B out)
            for (int c = 0; c < 3; ++c) {
                out.primaries[rgb[c]][0] = u16(b + 2 + size_t(c) * 4) / 50000;
                out.primaries[rgb[c]][1] = u16(b + 4 + size_t(c) * 4) / 50000;
            }
            out.white[0] = u16(b + 14) / 50000;
            out.white[1] = u16(b + 16) / 50000;
            out.maxLuminance = (u16(b + 18) * 65536 + u16(b + 20)) / 10000;
            out.minLuminance = (u16(b + 22) * 65536 + u16(b + 24)) / 10000;
            out.display = true;
        } else if (type == 1 && len == 6 && d[b] == 0x90 && d[b + 1] == 0x04) {
            out.maxCll = int(u16(b + 2));
            out.maxFall = int(u16(b + 4));
            out.lightLevel = true;
        }
        i = b + len;
    }
    return out.display || out.lightLevel;
}

AncPacket captionPacket(const std::vector<uint8_t>& cdp) {
    AncPacket a;
    a.did = 0x61;
    a.sdid = 0x01;
    a.data = cdp;
    return a;
}

// ---- Options.

void setOutputOptions(const SenderOptions& o) {
    std::lock_guard lock(optionsMutex);
    storedOptions() = o;
}

SenderOptions outputOptions() {
    std::lock_guard lock(optionsMutex);
    return storedOptions();
}

// ---- Sender.

struct Sender::Sockets {
    sock_t s = kBad;
    sockaddr_in video{}, audio{}, anc{};
    ~Sockets() {
        if (s != kBad) closeSock(s);
    }
    bool to(const sockaddr_in& a, const uint8_t* data, size_t size) {
        return ::sendto(s, reinterpret_cast<const char*>(data), int(size), 0, reinterpret_cast<const sockaddr*>(&a), sizeof a) == int(size);
    }
};

Sender::Sender() = default;
Sender::~Sender() { close(); }

bool Sender::open(const SenderOptions& o, const VideoFormat& video, const AudioFormat& audio, std::string* error) {
    close();
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        s_.reset();
        return false;
    };
    if (video.width <= 0 || video.width % 2 || video.height <= 0 || !video.fps.valid())
        return fail("ST 2110 carries pictures of an even width at a frame rate");
    in_addr probe;
    for (const Endpoint* e : {&o.video, &o.audio, &o.anc})
        if (!ipv4(e->address, probe) || e->port <= 0 || e->port > 65535) return fail("Not an IPv4 address and port: " + endpointText(*e));
    if (o.sendAudio && (audio.rate != 48000 || audio.channels < 1 || audio.channels > 8))
        return fail("ST 2110-30 here carries 1 to 8 channels at 48 kHz");
    s_ = std::make_unique<Sockets>();
    s_->s = udpSocket();
    if (s_->s == kBad) return fail("Could not make a UDP socket");
    option(s_->s, SOL_SOCKET, SO_SNDBUF, int(8 << 20));
    option(s_->s, IPPROTO_IP, IP_MULTICAST_TTL, int(std::clamp(o.ttl, 1, 255)));
    option(s_->s, IPPROTO_IP, IP_TTL, int(std::clamp(o.ttl, 1, 255)));
    option(s_->s, IPPROTO_IP, IP_TOS, int(34 << 2));  // DSCP AF41, media's class in AES67 and ST 2110 networks
    in_addr iface{};
    iface.s_addr = htonl(INADDR_ANY);
    if (!o.interfaceAddress.empty() && !ipv4(o.interfaceAddress, iface)) return fail("Not an IPv4 address: " + o.interfaceAddress);
    if (!o.interfaceAddress.empty() || o.sourcePort > 0) {
        // From the interface (and port) asked for; a port in use gives way to any free one.
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr = iface;
        local.sin_port = htons(uint16_t(std::clamp(o.sourcePort, 0, 65535)));
        option(s_->s, SOL_SOCKET, SO_REUSEADDR, int(1));
        if (::bind(s_->s, reinterpret_cast<const sockaddr*>(&local), sizeof local) != 0) {
            local.sin_port = 0;
            if (::bind(s_->s, reinterpret_cast<const sockaddr*>(&local), sizeof local) != 0)
                return fail("This computer has no interface at " + o.interfaceAddress);
        }
        if (!o.interfaceAddress.empty()) setsockopt(s_->s, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&iface), sizeof iface);
    }
    s_->video = socketAddress(o.video);
    s_->audio = socketAddress(o.audio);
    s_->anc = socketAddress(o.anc);
    o_ = o;
    video_ = video;
    audio_ = audio;
    std::random_device rd;
    std::mt19937 rng(rd());
    rtpVideo_ = {uint8_t(o.videoPayload), uint32_t(rng()), uint32_t(rng())};
    rtpAudio_ = {uint8_t(o.audioPayload), uint32_t(rng()), uint32_t(rng())};
    rtpAnc_ = {uint8_t(o.ancPayload), uint32_t(rng()), uint32_t(rng())};
    nextFrame_ = 0;
    audioSample_ = 0;
    audioFifo_.clear();
    frameAnc_.clear();
    frames_ = 0;
    ptpNowNs(&clock_);
    // The address the streams come from (the interface's, else the one routing picks for the video's destination).
    source_ = o.interfaceAddress;
    if (source_.empty()) {
        sock_t t = udpSocket();
        sockaddr_in dst = s_->video;
        sockaddr_in local{};
        socklen_t len = sizeof local;
        char text[INET_ADDRSTRLEN] = {};
        if (t != kBad && ::connect(t, reinterpret_cast<const sockaddr*>(&dst), sizeof dst) == 0 &&
            ::getsockname(t, reinterpret_cast<sockaddr*>(&local), &len) == 0 && inet_ntop(AF_INET, &local.sin_addr, text, sizeof text))
            source_ = text;
        if (t != kBad) closeSock(t);
        if (source_ == "0.0.0.0") source_.clear();
    }
    return true;
}

void Sender::close() { s_.reset(); }

bool Sender::isOpen() const { return s_ != nullptr; }

namespace {
SdpOptions sdpOptions(const SenderOptions& o, const std::string& source) {
    SdpOptions s;
    s.source = source;
    s.refclk = o.refclk;
    s.ttl = o.ttl;
    s.session = uint64_t(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    return s;
}
}  // namespace

std::string Sender::sdpVideo() const { return isOpen() && o_.sendVideo ? videoSdp(video_, o_.video, o_.videoPayload, sdpOptions(o_, source_)) : std::string(); }
std::string Sender::sdpAudio() const {
    return isOpen() && o_.sendAudio ? audioSdp(audio_, o_.audio, o_.audioPayload, sdpOptions(o_, source_)) : std::string();
}
std::string Sender::sdpAnc() const {
    if (!isOpen() || !o_.sendAnc) return {};
    std::vector<std::pair<int, int>> ids{{0x60, 0x60}, {0x61, 0x01}};
    for (const AncPacket& a : frameAnc_)
        if (std::find(ids.begin(), ids.end(), std::pair<int, int>{a.did, a.sdid}) == ids.end()) ids.emplace_back(a.did, a.sdid);
    return ancSdp(video_, o_.anc, o_.ancPayload, sdpOptions(o_, source_), ids);
}

bool Sender::send(const uint8_t* data, size_t stride, output::PixelFormat format, const float* audio, int audioFrames,
                  const output::Timecode* timecode, const std::vector<AncPacket>& anc, const std::atomic<bool>* stop) {
    if (!s_) return false;
    const int64_t num = video_.fps.num, den = video_.fps.den;
    const int64_t second = 1000000000;
    auto frameStart = [&](int64_t n) { return mulDiv(n, second * den, num); };
    auto stopped = [&] { return stop && stop->load(); };
    // Waits until PTP time `t` (in short sleeps, to see `stop`).
    auto waitUntil = [&](int64_t t) {
        for (;;) {
            const int64_t left = t - ptpNowNs();
            if (left <= 0) return true;
            if (stopped()) return false;
            std::this_thread::sleep_for(std::chrono::nanoseconds(std::min<int64_t>(left, 2000000)));
        }
    };
    // The frame's slot: the next one, or (behind by more than a frame, or starting) the next to start from now.
    const int64_t now = ptpNowNs();
    if (nextFrame_ == 0 || frameStart(nextFrame_ + 1) < now) {
        nextFrame_ = mulDiv(now, num, second * den) + 1;
        audioSample_ = mulDiv(frameStart(nextFrame_), audio_.rate, second);
        audioFifo_.clear();
    }
    const int64_t start = frameStart(nextFrame_), end = frameStart(nextFrame_ + 1);
    if (o_.sendAudio && audio && audioFrames > 0) audioFifo_.insert(audioFifo_.end(), audio, audio + size_t(audioFrames) * size_t(audio_.channels));
    if (!waitUntil(start)) return false;

    bool ok = true;
    auto sendAudioDue = [&](int64_t until) {
        // 1 ms packets whose last sample's time has come.
        const int per = audio_.rate / 1000, ch = audio_.channels;
        while (ok && o_.sendAudio && audioFifo_.size() >= size_t(per) * size_t(ch) &&
               mulDiv(audioSample_ + per, second, audio_.rate) <= until) {
            ok = packAudio(audioFifo_.data(), per, ch, uint32_t(audioSample_), rtpAudio_,
                           [&](const uint8_t* p, size_t n) { return s_->to(s_->audio, p, n); });
            audioFifo_.erase(audioFifo_.begin(), audioFifo_.begin() + ptrdiff_t(per) * ch);
            audioSample_ += per;
        }
    };
    const int fields = video_.interlaced ? 2 : 1;
    std::vector<std::vector<uint8_t>> packets;
    for (int f = 0; f < fields && ok; ++f) {
        const int field = video_.interlaced ? f + 1 : 0;
        // A field's time: its half of the frame; its timestamp from it (90 kHz).
        const int64_t fStart = video_.interlaced ? mulDiv(2 * nextFrame_ + f, second * den, 2 * num) : start;
        const int64_t fEnd = video_.interlaced ? mulDiv(2 * nextFrame_ + f + 1, second * den, 2 * num) : end;
        const uint32_t ts = uint32_t(video_.interlaced ? mulDiv(2 * nextFrame_ + f, 90000 * den, 2 * num) : mulDiv(nextFrame_, 90000 * den, num));
        if (!waitUntil(fStart)) return false;
        if (o_.sendAnc) {
            // (ST 2110-40 wants a packet each frame or field, empty when there is nothing)
            std::vector<AncPacket> out;
            if (timecode) out = timecodePackets(*timecode, video_.fps, field);
            if (f == 0) {
                out.insert(out.end(), frameAnc_.begin(), frameAnc_.end());
                out.insert(out.end(), anc.begin(), anc.end());
            }
            ok = packAnc(out, field, ts, rtpAnc_, [&](const uint8_t* p, size_t n) { return s_->to(s_->anc, p, n); });
        }
        packets.clear();
        if (ok && o_.sendVideo)
            ok = packVideo(data, stride, format, video_.width, video_.height, field, ts, rtpVideo_,
                           [&](const uint8_t* p, size_t n) {
                               packets.emplace_back(p, p + n);
                               return true;
                           },
                           o_.maxPayload);
        // From ST 2110-21's default offset (43 lines of 1125 into a progressive frame, 22 into a field), spread over
        // most of what is left (a wide sender's pacing), in bursts of what is due.
        const int64_t troffset = (end - start) * (video_.interlaced ? 22 : 43) / 1125;
        const int64_t vStart = fStart + troffset, span = (fEnd - vStart) * 9 / 10;
        if (!waitUntil(vStart)) return false;
        size_t i = 0;
        while (ok && i < packets.size()) {
            const int64_t t = ptpNowNs();
            const size_t due = std::min(packets.size(), size_t(std::max<int64_t>(1, (t - vStart) * int64_t(packets.size()) / std::max<int64_t>(1, span) + 1)));
            for (; ok && i < due; ++i) ok = s_->to(s_->video, packets[i].data(), packets[i].size());
            sendAudioDue(t);
            if (i < packets.size()) {
                if (stopped()) return false;
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        }
    }
    if (!ok) return false;
    sendAudioDue(end);
    ++nextFrame_;
    ++frames_;
    return true;
}

// ---- Receiver.

struct Receiver::Socket {
    sock_t s = kBad;
    int port = 0;
    ~Socket() {
        if (s != kBad) closeSock(s);
    }
};

Receiver::Receiver() = default;
Receiver::~Receiver() = default;

bool Receiver::open(const Endpoint& e, const std::string& interfaceAddress, std::string* error) {
    close();
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        s_.reset();
        return false;
    };
    s_ = std::make_unique<Socket>();
    s_->s = udpSocket();
    if (s_->s == kBad) return fail("Could not make a UDP socket");
    option(s_->s, SOL_SOCKET, SO_REUSEADDR, int(1));
    option(s_->s, SOL_SOCKET, SO_RCVBUF, int(32 << 20));
    const bool multicast = isMulticast(e.address);
    sockaddr_in local = socketAddress(e);
    if (multicast) {
#ifdef _WIN32
        local.sin_addr.s_addr = htonl(INADDR_ANY);  // (Windows binds no multicast address)
#endif
    }
    if (::bind(s_->s, reinterpret_cast<const sockaddr*>(&local), sizeof local) != 0) return fail("Could not listen at " + endpointText(e));
    if (multicast) {
        ip_mreq m{};
        ipv4(e.address, m.imr_multiaddr);
        if (!interfaceAddress.empty()) ipv4(interfaceAddress, m.imr_interface);
        if (setsockopt(s_->s, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&m), sizeof m) != 0)
            return fail("Could not join " + e.address);
    }
    sockaddr_in bound{};
    socklen_t len = sizeof bound;
    if (::getsockname(s_->s, reinterpret_cast<sockaddr*>(&bound), &len) == 0) s_->port = ntohs(bound.sin_port);
    return true;
}

bool Receiver::receive(std::vector<uint8_t>& out, int ms) {
    if (!s_ || !pollIn(s_->s, ms)) return false;
    out.resize(65536);
    const auto n = ::recv(s_->s, reinterpret_cast<char*>(out.data()), int(out.size()), 0);
    if (n <= 0) return false;
    out.resize(size_t(n));
    return true;
}

void Receiver::close() { s_.reset(); }

int Receiver::port() const { return s_ ? s_->port : 0; }

}  // namespace montage::st2110

// ---------------------------------------------------------------------------

namespace montage::output {

namespace {

// This computer's IPv4 interfaces (name, address), loopback last.
std::vector<std::pair<std::string, std::string>> interfaces() {
    std::vector<std::pair<std::string, std::string>> out, loop;
#ifdef _WIN32
    ULONG size = 16 << 10;
    std::vector<uint8_t> buf(size);
    auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, a, &size) ==
        ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, a, &size) != NO_ERROR)
            a = nullptr;
    }
    for (; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        char name[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, name, sizeof name, nullptr, nullptr);
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            char text[INET_ADDRSTRLEN] = {};
            const auto* sin = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
            inet_ntop(AF_INET, &sin->sin_addr, text, sizeof text);
            (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK ? loop : out).emplace_back(name, text);
        }
    }
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) == 0) {
        for (ifaddrs* i = list; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP)) continue;
            char text[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(i->ifa_addr)->sin_addr, text, sizeof text);
            ((i->ifa_flags & IFF_LOOPBACK) ? loop : out).emplace_back(i->ifa_name, text);
        }
        freeifaddrs(list);
    }
#endif
    out.insert(out.end(), loop.begin(), loop.end());
    return out;
}

class St2110Backend : public Backend {
public:
    ~St2110Backend() override { close(); }
    std::string name() const override { return "ST 2110"; }
    bool available(std::string*) override { return true; }
    std::vector<Device> devices() override {
        std::vector<Device> out;
        auto device = [](const std::string& id, const std::string& name) {
            Device d;
            d.id = id;
            d.name = name;
            d.modes = standardModes();
            d.formats = {PixelFormat::V210, PixelFormat::Uyvy8};
            d.hdr = true;  // (said by the SDP's TCS and colorimetry)
            d.audioChannels = {2, 8};
            d.audioRates = {48000};
            d.captions = true;
            return d;
        };
        out.push_back(device("", "ST 2110 (the interface routing chooses)"));
        for (const auto& [name, address] : interfaces()) out.push_back(device(address, "ST 2110 on " + name + " (" + address + ")"));
        return out;
    }
    bool open(const Device& device, const Mode& mode, const Signal& signal, int audioChannels, int audioRate, std::string* error) override {
        close();
        st2110::SenderOptions o = st2110::outputOptions();
        if (!device.id.empty()) o.interfaceAddress = device.id;
        st2110::VideoFormat v;
        v.width = mode.width;
        v.height = mode.height;
        v.fps = mode.fps;
        v.interlaced = mode.interlaced;
        v.depth = signal.format == PixelFormat::V210 ? 10 : 8;
        v.tcs = signal.eotf == Eotf::Pq ? "PQ" : signal.eotf == Eotf::Hlg ? "HLG" : "SDR";
        v.colorimetry = signal.eotf != Eotf::Sdr ? "BT2100" : signal.bt2020 ? "BT2020" : "BT709";
        st2110::AudioFormat a;
        a.channels = std::max(1, audioChannels);
        a.rate = audioRate;
        o.sendAudio = o.sendAudio && audioChannels > 0;
        std::lock_guard lock(m_);
        stop_ = false;
        cdpSequence_ = 0;
        fps_ = mode.fps;
        if (!sender_.open(o, v, a, error)) return false;
        sender_.setFrameAnc(signal.hdrMetadata && signal.eotf == Eotf::Pq ? std::vector<st2110::AncPacket>{st2110::hdrMetadataPacket(signal)}
                                                                          : std::vector<st2110::AncPacket>{});
        return true;
    }
    bool send(const VideoFrame& f, const Audio& a) override {
        std::vector<st2110::AncPacket> anc;
        if (f.ccCount > 0 && f.cc) {
            std::vector<CcTriplet> cc(size_t(f.ccCount));
            std::memcpy(cc.data(), f.cc, cc.size() * 3);
            anc.push_back(st2110::captionPacket(captionCdp(cc, fps_, cdpSequence_++)));
        }
        return sender_.send(f.data, f.stride, f.format, a.channels > 0 ? a.samples : nullptr, a.channels > 0 ? a.frames : 0, &f.timecode, anc, &stop_);
    }
    void close() override {
        std::lock_guard lock(m_);
        sender_.close();
    }
    void wake() override { stop_ = true; }
    std::string address() override {
        std::lock_guard lock(m_);
        return sender_.isOpen() ? "rtp://" + st2110::endpointText(sender_.options().video) : std::string();
    }
    std::map<std::string, std::string> sdp() override {
        std::lock_guard lock(m_);
        std::map<std::string, std::string> out;
        if (!sender_.isOpen()) return out;
        out["video"] = sender_.sdpVideo();
        if (!sender_.sdpAudio().empty()) out["audio"] = sender_.sdpAudio();
        if (!sender_.sdpAnc().empty()) out["data"] = sender_.sdpAnc();
        return out;
    }
    bool streams(st2110::VideoFormat& video, st2110::AudioFormat& audio, st2110::SenderOptions& options) {
        std::lock_guard lock(m_);
        if (!sender_.isOpen()) return false;
        video = sender_.videoFormat();
        audio = sender_.audioFormat();
        options = sender_.options();
        return true;
    }

private:
    std::mutex m_;
    st2110::Sender sender_;
    std::atomic<bool> stop_{false};
    uint16_t cdpSequence_ = 0;
    Rational fps_{25, 1};
};

}  // namespace

std::shared_ptr<Backend> makeSt2110Backend() { return std::make_shared<St2110Backend>(); }

}  // namespace montage::output

namespace montage::st2110 {

bool openStreams(output::Backend& b, VideoFormat& video, AudioFormat& audio, SenderOptions& options) {
    auto* x = dynamic_cast<output::St2110Backend*>(&b);
    return x && x->streams(video, audio, options);
}

}  // namespace montage::st2110
