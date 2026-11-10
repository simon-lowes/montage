#include "St2110.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cerrno>
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
#include <mmsystem.h>
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
#include "media/JpegXs.h"
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

// A socket buffer as large as the system allows, up to `bytes`: Linux caps a request silently, but macOS and the BSDs
// refuse one above their limit (kern.ipc.maxsockbuf) and leave the small default, so smaller sizes are tried in turn.
void bufferSize(sock_t s, int name, int bytes) {
    for (; bytes >= (256 << 10); bytes /= 2)
        if (setsockopt(s, SOL_SOCKET, name, reinterpret_cast<const char*>(&bytes), sizeof bytes) == 0) return;
}

// Whether a send failed only because the socket's or interface's queue was full for a moment (macOS says so of UDP
// rather than waiting), so it is worth trying again.
bool queueFull() {
#ifdef _WIN32
    const int e = WSAGetLastError();
    return e == WSAENOBUFS || e == WSAEWOULDBLOCK;
#else
    return errno == ENOBUFS || errno == EAGAIN || errno == EWOULDBLOCK;
#endif
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

// Any sampling and depth: the samples' codes one after another, most significant bit first (pixel groups are whole bytes,
// so a row of them is one stream of bits).
void toBits(const uint16_t* c, size_t n, int depth, uint8_t* out) {
    if (depth == 8) {
        for (size_t i = 0; i < n; ++i) out[i] = uint8_t(c[i]);
        return;
    }
    uint32_t acc = 0;
    int bits = 0;
    const uint32_t mask = (1u << depth) - 1;
    for (size_t i = 0; i < n; ++i) {
        acc = (acc << depth) | (c[i] & mask);
        bits += depth;
        while (bits >= 8) {
            bits -= 8;
            *out++ = uint8_t(acc >> bits);
        }
        acc &= (1u << bits) - 1;
    }
    if (bits) *out = uint8_t(acc << (8 - bits));
}
void fromBits(const uint8_t* in, size_t n, int depth, uint16_t* c) {
    if (depth == 8) {
        for (size_t i = 0; i < n; ++i) c[i] = in[i];
        return;
    }
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; ++i) {
        while (bits < depth) acc = (acc << 8) | *in++, bits += 8;
        bits -= depth;
        c[i] = uint16_t(acc >> bits);
        acc &= (1u << bits) - 1;
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

std::string samplingName(Sampling s) {
    return s == Sampling::Ycc444 ? "YCbCr-4:4:4" : s == Sampling::Rgb ? "RGB" : "YCbCr-4:2:2";
}

bool parseSampling(const std::string& name, Sampling& out) {
    for (Sampling s : {Sampling::Ycc422, Sampling::Ycc444, Sampling::Rgb})
        if (name == samplingName(s)) {
            out = s;
            return true;
        }
    return false;
}

Pgroup pgroup(Sampling s, int depth) {
    // (the fewest pixels whose samples come to whole bytes)
    const int perPixelUnit = s == Sampling::Ycc422 ? 4 : 3, unitPixels = s == Sampling::Ycc422 ? 2 : 1;
    for (int units = 1;; ++units)
        if (units * perPixelUnit * depth % 8 == 0) return {units * unitPixels, units * perPixelUnit * depth / 8};
}

bool videoFormatOk(const VideoFormat& f, std::string* why) {
    auto fail = [&](const std::string& w) {
        if (why) *why = w;
        return false;
    };
    if (f.codec != "raw" && f.codec != "jxsv") return fail("Montage sends and receives ST 2110-20 and ST 2110-22's JPEG XS, not " + f.codec);
    if (f.depth != 8 && f.depth != 10 && f.depth != 12)
        return fail(f.jpegXs() ? "ST 2110-22's JPEG XS is sent here at 8, 10 or 12 bits" : "ST 2110-20 is sent here at 8, 10 or 12 bits");
    if (f.width <= 0 || f.height <= 0 || !f.fps.valid()) return fail("ST 2110 carries pictures of a size at a frame rate");
    // (what an SDP, which anyone on the network can hand an NMOS receiver, may ask to be held)
    if (f.width > 8192 || f.height > 4320) return fail("ST 2110 pictures here are at most 8192 x 4320");
    if (f.fps.num <= 0 || f.fps.den <= 0 || f.fps.num > 1000000 || f.fps.den > 100000 || f.fps.toDouble() < 1 || f.fps.toDouble() > 300)
        return fail("ST 2110 pictures here come at 1 to 300 frames a second");
    // (and not both: what it may ask to be received a second is bounded too, at 8192 x 4320 at 60 frames)
    if (double(f.width) * f.height * f.fps.toDouble() > 8192.0 * 4320 * 60)
        return fail("ST 2110 pictures here are at most as many pixels a second as 8192 x 4320 at 60 frames");
    if (f.jpegXs()) {
        if (f.interlaced) return fail("ST 2110-22's JPEG XS is sent and received here progressive");
        if (f.sampling == Sampling::Ycc422 && f.width % 2) return fail("JPEG XS at 4:2:2 needs an even width");
        if (f.width < 16 || f.height < 16) return fail("JPEG XS pictures here are at least 16 x 16");
        if (!(f.bpp >= 0.5 && f.bpp <= 24)) return fail("JPEG XS is coded here at 0.5 to 24 bits a pixel");
        if (f.packetMode != 0 && f.packetMode != 1) return fail("RFC 9134's packetization mode is 0 (codestream) or 1 (slice)");
        return true;
    }
    const Pgroup pg = pgroup(f.sampling, f.depth);
    if (f.width % pg.pixels)
        return fail(samplingName(f.sampling) + " at " + std::to_string(f.depth) + " bits needs a width in whole groups of " +
                    std::to_string(pg.pixels) + " pixels");
    return true;
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

std::string sdpSession(const SdpOptions& o, bool duplicated) {
    const std::string source = o.source.empty() ? "0.0.0.0" : o.source;
    std::ostringstream s;
    s << "v=0\r\n"
      << "o=- " << o.session << " " << o.session << " IN IP4 " << source << "\r\n"
      << "s=" << o.name << "\r\n"
      << "t=0 0\r\n";
    if (duplicated) s << "a=group:DUP primary secondary\r\n";
    return s.str();
}

std::string sdpMedia(const Endpoint& to, const SdpOptions& o, const std::string& source, const std::string& media, int pt, int64_t kbps) {
    std::ostringstream s;
    s << "m=" << media << " " << to.port << " RTP/AVP " << pt << "\r\n"
      << "c=IN IP4 " << to.address << (isMulticast(to.address) ? "/" + std::to_string(o.ttl) : std::string()) << "\r\n";
    if (kbps > 0) s << "b=AS:" << kbps << "\r\n";
    if (isMulticast(to.address) && !source.empty()) s << "a=source-filter: incl IN IP4 " << to.address << " " << source << "\r\n";
    return s.str();
}

std::string sdpClock(const SdpOptions& o) { return "a=ts-refclk:" + o.refclk + "\r\na=mediaclk:direct=0\r\n"; }

// The session, then each path's media description (the same attributes, its own address and "a=mid").
// (`kbps`: a b=AS line's bandwidth, 0 none)
std::string sdpStreams(const Endpoint& to, const SdpOptions& o, const Endpoint* to2, const std::string& media, int pt, const std::string& attributes,
                       int64_t kbps = 0) {
    std::string out = sdpSession(o, to2 != nullptr) + sdpMedia(to, o, o.source, media, pt, kbps) + attributes;
    if (!to2) return out;
    return out + "a=mid:primary\r\n" + sdpMedia(*to2, o, o.source2, media, pt, kbps) + attributes + "a=mid:secondary\r\n";
}

}  // namespace

std::string videoSdp(const VideoFormat& f, const Endpoint& to, int pt, const SdpOptions& o, const Endpoint* to2) {
    std::ostringstream s;
    if (f.jpegXs()) {
        // ST 2110-22 (its frame size, TP and SSN, and the bandwidth) with RFC 9134's parameters (sequential packets).
        s << "a=rtpmap:" << pt << " jxsv/90000\r\n"
          << "a=fmtp:" << pt << " packetmode=" << f.packetMode << "; transmode=1";
        if (!f.profile.empty()) s << "; profile=" << f.profile;
        if (!f.level.empty()) s << "; level=" << f.level;
        if (!f.sublevel.empty()) s << "; sublevel=" << f.sublevel;
        s << "; sampling=" << samplingName(f.sampling) << "; width=" << f.width << "; height=" << f.height << "; depth=" << f.depth
          << "; exactframerate=" << rateText(f.fps) << "; colorimetry=" << f.colorimetry << "; TCS=" << f.tcs
          << "; RANGE=NARROW; SSN=ST2110-22:2022; TP=2110TPW\r\n"
          << sdpClock(o);
        return sdpStreams(to, o, to2, "video", pt, s.str(), f.bandwidthKbps);
    }
    // (RGB is full range in ST 2110-20's sense only when said; Montage sends it narrow, as YCbCr)
    s << "a=rtpmap:" << pt << " raw/90000\r\n"
      << "a=fmtp:" << pt << " sampling=" << samplingName(f.sampling) << "; width=" << f.width << "; height=" << f.height
      << "; exactframerate=" << rateText(f.fps) << "; depth=" << f.depth << "; TCS=" << f.tcs << "; colorimetry=" << f.colorimetry
      << "; RANGE=NARROW; PM=2110GPM; SSN=ST2110-20:2017; TP=2110TPW" << (f.interlaced ? "; interlace" : "") << "\r\n"
      << sdpClock(o);
    return sdpStreams(to, o, to2, "video", pt, s.str());
}

std::string audioSdp(const AudioFormat& f, const Endpoint& to, int pt, const SdpOptions& o, const Endpoint* to2) {
    // Channel order: a stereo pair, or as many pairs as there are, an odd one last a mono channel (ST 2110-30's symbols).
    std::string order = "SMPTE2110.(";
    for (int i = 0; i < f.channels / 2; ++i) order += i ? ",ST" : "ST";
    if (f.channels % 2) order += f.channels > 1 ? ",M" : "M";
    order += ")";
    std::ostringstream s;
    if (f.aes3) {
        // (ST 2110-31: AM824 words, AES3's subframes in pairs; no channel order, as they need not be audio)
        s << "a=rtpmap:" << pt << " AM824/" << f.rate << "/" << f.channels << "\r\n";
    } else {
        s << "a=rtpmap:" << pt << " L" << (f.bits == 16 ? 16 : 24) << "/" << f.rate << "/" << f.channels << "\r\n"
          << "a=fmtp:" << pt << " channel-order=" << order << "\r\n";
    }
    s << "a=ptime:" << (f.packetUs == 125 ? "0.125" : "1") << "\r\n" << sdpClock(o);
    return sdpStreams(to, o, to2, "audio", pt, s.str());
}

std::string metadataSdp(const Endpoint& to, int pt, const SdpOptions& o, const std::vector<uint32_t>& types, const Endpoint* to2) {
    // (ST 2110-41:2026: an "application" media type; SSN as its 2024 edition, which it keeps; DIT in hexadecimal,
    // capitals without "0x" or spaces)
    std::ostringstream s;
    s << "a=rtpmap:" << pt << " ST2110-41/90000\r\n"
      << "a=fmtp:" << pt << " SSN=ST2110-41:2024";
    if (!types.empty()) {
        s << "; DIT=";
        char t[16];
        for (size_t i = 0; i < types.size(); ++i) {
            std::snprintf(t, sizeof t, "%s%X", i ? "," : "", unsigned(types[i] & 0x3FFFFF));
            s << t;
        }
    }
    s << "\r\n" << sdpClock(o);
    return sdpStreams(to, o, to2, "application", pt, s.str());
}

std::string ancSdp(const VideoFormat& f, const Endpoint& to, int pt, const SdpOptions& o, const std::vector<std::pair<int, int>>& ids,
                   const Endpoint* to2) {
    std::ostringstream s;
    s << "a=rtpmap:" << pt << " smpte291/90000\r\n"
      << "a=fmtp:" << pt << " ";
    char id[32];
    for (const auto& [did, sdid] : ids) {
        std::snprintf(id, sizeof id, "DID_SDID={0x%02X,0x%02X}; ", did & 0xFF, sdid & 0xFF);
        s << id;
    }
    s << "exactframerate=" << rateText(f.fps) << "; SSN=ST2110-40:2018\r\n"
      << sdpClock(o);
    return sdpStreams(to, o, to2, "video", pt, s.str());
}

std::map<std::string, std::string> sdpParameters(const std::string& sdp, std::string* encoding) {
    std::map<std::string, std::string> out;
    std::istringstream in(sdp);
    std::string line;
    auto trim = [](std::string t) {
        const auto a = t.find_first_not_of(" \t\r"), b = t.find_last_not_of(" \t\r");
        return a == std::string::npos ? std::string() : t.substr(a, b - a + 1);
    };
    int media = 0;
    while (std::getline(in, line)) {
        line = trim(line);
        // (the first media description's: a second is the same stream's other path)
        if (line.rfind("m=", 0) == 0 && ++media > 1) break;
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

// Bytes of pixels a packet carries: whole pixel groups, at most `maxPayload`.
int packetData(const Pgroup& pg, int maxPayload) { return std::max(1, maxPayload / pg.bytes) * pg.bytes; }

// The packets of a picture's rows (or a field's), each row's pixel groups from `line` (row number, out).
bool packRows(int width, int height, int field, const Pgroup& pg, uint32_t timestamp, RtpState& rtp, const PacketSink& send, int maxPayload,
              const std::function<void(int row, uint8_t* out)>& line) {
    const int rows = field ? (height + (field == 1 ? 1 : 0)) / 2 : height;
    const size_t lineBytes = size_t(width / pg.pixels) * size_t(pg.bytes);
    const size_t chunk = size_t(packetData(pg, maxPayload));
    std::vector<uint8_t> bytes(lineBytes + 8), packet(12 + 2 + 6 + chunk);
    for (int r = 0; r < rows; ++r) {
        line(field ? r * 2 + (field - 1) : r, bytes.data());
        for (size_t off = 0; off < lineBytes; off += chunk) {
            const size_t n = std::min(chunk, lineBytes - off);
            const bool last = r == rows - 1 && off + n == lineBytes;
            uint8_t* p = packet.data();
            rtpHeader(p, last, rtp, timestamp);
            put16(p + 12, rtp.sequence >> 16);
            put16(p + 14, uint32_t(n));
            put16(p + 16, uint32_t(field == 2 ? 0x8000 : 0) | uint32_t(r & 0x7FFF));
            put16(p + 18, uint32_t(off / size_t(pg.bytes) * size_t(pg.pixels)) & 0x7FFF);
            std::memcpy(p + 20, bytes.data() + off, n);
            ++rtp.sequence;
            if (!send(p, 20 + n)) return false;
        }
    }
    return true;
}

}  // namespace

bool packVideo(const uint8_t* data, size_t stride, output::PixelFormat format, int width, int height, int field, uint32_t timestamp,
               RtpState& rtp, const PacketSink& send, int maxPayload) {
    if (format == output::PixelFormat::P216 || width <= 0 || width % 2 || height <= 0 || !data) return false;
    const bool ten = tenBit(format);
    std::vector<uint16_t> comps(size_t(width) * 2);
    return packRows(width, height, field, pgroup(Sampling::Ycc422, ten ? 10 : 8), timestamp, rtp, send, maxPayload, [&](int row, uint8_t* out) {
        rowComponents(data + size_t(row) * stride, format, width, comps.data());
        toPgroups(comps.data(), width, ten, out);
    });
}

bool packVideo(const Raster& picture, int field, uint32_t timestamp, RtpState& rtp, const PacketSink& send, int maxPayload) {
    VideoFormat f;
    f.width = picture.width, f.height = picture.height, f.sampling = picture.sampling, f.depth = picture.depth;
    if (!videoFormatOk(f) || picture.samples.size() < size_t(picture.rowSamples()) * size_t(picture.height)) return false;
    const size_t n = size_t(picture.rowSamples());
    return packRows(picture.width, picture.height, field, pgroup(picture.sampling, picture.depth), timestamp, rtp, send, maxPayload,
                    [&](int row, uint8_t* out) { toBits(picture.row(row), n, picture.depth, out); });
}

int videoPacketCount(int width, int height, int depth, bool interlaced, int maxPayload, Sampling sampling) {
    const Pgroup pg = pgroup(sampling, depth);
    const int64_t lineBytes = int64_t(width / pg.pixels) * pg.bytes, chunk = packetData(pg, maxPayload);
    (void)interlaced;  // (the same rows, in two fields)
    return int(height * ((lineBytes + chunk - 1) / chunk));
}

VideoAssembler::VideoAssembler(int width, int height, output::PixelFormat format, bool interlaced)
    : width_(width), height_(height), format_(format), bytes_(true), interlaced_(interlaced), stride_(output::rowBytes(format, width)),
      pg_(pgroup(Sampling::Ycc422, tenBit(format) ? 10 : 8)), building_(width, height, Sampling::Ycc422, tenBit(format) ? 10 : 8) {}

VideoAssembler::VideoAssembler(int width, int height, Sampling sampling, int depth, bool interlaced)
    : width_(width), height_(height), bytes_(false), interlaced_(interlaced), pg_(pgroup(sampling, depth)), building_(width, height, sampling, depth) {}

bool VideoAssembler::add(const RtpPacket& p) {
    if (p.size < 2 + 6) return false;
    const uint32_t seq = get16(p.payload) << 16 | p.sequence;
    if (lastSeq_ >= 0) {
        const uint32_t gap = seq - uint32_t(lastSeq_) - 1;
        if (gap > 0 && gap < (1u << 20)) lost_ += gap, frameLost_ += gap;
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
    const size_t perPixel = building_.sampling == Sampling::Ycc422 ? 2 : 3;
    const bool fast422 = building_.sampling == Sampling::Ycc422 && building_.depth <= 10;
    for (const Srd& s : srds) {
        if (at + s.length > p.size) return false;
        const int row = interlaced_ ? int(s.row) * 2 + (s.second ? 1 : 0) : int(s.row);
        const int pixels = int(s.length) / pg_.bytes * pg_.pixels;
        if (row < height_ && int(s.offset) + pixels <= width_) {
            uint16_t* dst = building_.row(row) + size_t(s.offset) * perPixel;
            if (fast422) fromPgroups(p.payload + at, pixels, building_.depth == 10, dst);
            else fromBits(p.payload + at, size_t(pixels) * perPixel, building_.depth, dst);
        }
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
    if (bytes_) {
        done_.assign(stride_ * size_t(height_), 0);
        for (int r = 0; r < height_; ++r) componentsRow(building_.row(r), format_, width_, done_.data() + size_t(r) * stride_, stride_);
    } else {
        doneRaster_ = building_;
    }
    doneTimestamp_ = timestamp_;
    doneLost_ = frameLost_;
    frameLost_ = 0;
    fieldsDone_ = 0;
}

// ---- ST 2110-30.

bool packAudio(const float* samples, int frames, int channels, uint32_t timestamp, RtpState& rtp, const PacketSink& send, int bits) {
    if (frames <= 0 || channels <= 0) return false;
    const bool l16 = bits == 16;
    const size_t bytes = l16 ? 2 : 3;
    const double full = l16 ? 32768.0 : 8388608.0;
    std::vector<uint8_t> packet(12 + size_t(frames) * size_t(channels) * bytes);
    rtpHeader(packet.data(), false, rtp, timestamp);
    uint8_t* p = packet.data() + 12;
    for (size_t i = 0, n = size_t(frames) * size_t(channels); i < n; ++i, p += bytes) {
        const float x = samples ? samples[i] : 0.0f;
        const double v = std::nearbyint(double(std::isfinite(x) ? x : 0.0f) * full);
        const int32_t s = int32_t(std::clamp(v, -full, full - 1));
        if (l16) p[0] = uint8_t(s >> 8), p[1] = uint8_t(s);
        else p[0] = uint8_t(s >> 16), p[1] = uint8_t(s >> 8), p[2] = uint8_t(s);
    }
    ++rtp.sequence;
    return send(packet.data(), packet.size());
}

std::vector<float> unpackAudio(const uint8_t* payload, size_t size, int channels, int bits) {
    std::vector<float> out;
    if (channels <= 0) return out;
    const bool l16 = bits == 16;
    const size_t bytes = l16 ? 2 : 3;
    const size_t n = size / bytes / size_t(channels) * size_t(channels);
    out.resize(n);
    for (size_t i = 0; i < n; ++i, payload += bytes) {
        if (l16) {
            out[i] = float(double(int16_t(uint16_t(payload[0] << 8 | payload[1]))) / 32768.0);
            continue;
        }
        int32_t s = int32_t(uint32_t(payload[0]) << 16 | uint32_t(payload[1]) << 8 | payload[2]);
        if (s & 0x800000) s -= 0x1000000;
        out[i] = float(double(s) / 8388608.0);
    }
    return out;
}

// ---- ST 2110-31.

uint32_t am824Word(int32_t data24, bool blockStart, bool frameStart, bool c, bool u, bool v) {
    const uint32_t data = uint32_t(data24) & 0xFFFFFF;
    // (even parity over the 24 bits, V, U, C and P)
    const bool odd = (std::popcount(data) + int(v) + int(u) + int(c)) & 1;
    const uint32_t label = uint32_t(blockStart) << 5 | uint32_t(frameStart) << 4 | uint32_t(odd) << 3 | uint32_t(c) << 2 | uint32_t(u) << 1 | uint32_t(v);
    return label << 24 | data;
}

bool am824ParityOk(uint32_t word) { return (std::popcount(word & 0x0FFFFFFFu) & 1) == 0; }

uint8_t aes3Crc(const uint8_t* bytes, size_t n) {
    // CRC-8/AES: x^8 + x^4 + x^3 + x^2 + 1, from all ones, each byte's bits least significant first (as they are sent).
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < n; ++i) {
        crc ^= bytes[i];
        for (int b = 0; b < 8; ++b) crc = (crc & 1) ? uint8_t((crc >> 1) ^ 0xB8) : uint8_t(crc >> 1);
    }
    return crc;
}

std::array<uint8_t, 24> aes3ChannelStatus(bool nonAudio) {
    std::array<uint8_t, 24> s{};
    s[0] = uint8_t(0x01 | (nonAudio ? 0x02 : 0));
    s[23] = aes3Crc(s.data(), 23);
    return s;
}

bool aes3ReadChannelStatus(const uint32_t* words, size_t frames, int channels, int channel, std::array<uint8_t, 24>& out) {
    if (channels <= 0 || channel < 0 || channel >= channels) return false;
    for (size_t f = 0; f + 192 <= frames; ++f) {
        if (!am824BlockStart(words[f * size_t(channels) + size_t(channel)])) continue;
        out.fill(0);
        for (int i = 0; i < 192; ++i)
            if (am824C(words[(f + size_t(i)) * size_t(channels) + size_t(channel)])) out[size_t(i / 8)] |= uint8_t(1 << (i % 8));
        return true;
    }
    return false;
}

std::vector<uint32_t> aes3FromPcm(const float* samples, int frames, int channels, int& blockFrame, const std::array<uint8_t, 24>& status) {
    std::vector<uint32_t> out(size_t(std::max(0, frames)) * size_t(std::max(0, channels)));
    for (int f = 0; f < frames; ++f) {
        const int b = (blockFrame + f) % 192;
        const bool c = (status[size_t(b / 8)] >> (b % 8)) & 1;
        for (int ch = 0; ch < channels; ++ch) {
            const float x = samples ? samples[size_t(f) * size_t(channels) + size_t(ch)] : 0.0f;
            const double v = std::nearbyint(double(std::isfinite(x) ? x : 0.0f) * 8388608.0);
            const int32_t d = int32_t(std::clamp(v, -8388608.0, 8388607.0));
            // (the first subframe of each pair begins a frame: AES3's channels go in pairs)
            out[size_t(f) * size_t(channels) + size_t(ch)] = am824Word(d, b == 0, ch % 2 == 0, c);
        }
    }
    blockFrame = (blockFrame + std::max(0, frames)) % 192;
    return out;
}

bool packAm824(const uint32_t* words, int frames, int channels, uint32_t timestamp, RtpState& rtp, const PacketSink& send) {
    if (frames <= 0 || channels <= 0) return false;
    std::vector<uint8_t> packet(12 + size_t(frames) * size_t(channels) * 4);
    rtpHeader(packet.data(), false, rtp, timestamp);
    for (size_t i = 0, n = size_t(frames) * size_t(channels); i < n; ++i) put32(packet.data() + 12 + i * 4, words ? words[i] : 0);
    ++rtp.sequence;
    return send(packet.data(), packet.size());
}

std::vector<uint32_t> unpackAm824(const uint8_t* payload, size_t size, int channels) {
    std::vector<uint32_t> out;
    if (channels <= 0) return out;
    const size_t n = size / 4 / size_t(channels) * size_t(channels);
    out.resize(n);
    for (size_t i = 0; i < n; ++i) out[i] = get32(payload + i * 4);
    return out;
}

// ---- ST 2110-41.

bool packFastMetadata(const std::vector<FastMetadata>& items, uint32_t timestamp, RtpState& rtp, const PacketSink& send, int maxPayload) {
    const size_t room = std::clamp<size_t>(size_t(std::max(maxPayload, 8)), 8, kMaxMetadataPayload);
    for (const FastMetadata& m : items)
        if (m.words.empty() || m.words.size() > kMaxMetadataWords || 4 + m.words.size() * 4 > kMaxMetadataPayload || m.type > 0x3FFFFF) return false;
    std::vector<uint8_t> packet;
    bool ok = true;
    size_t i = 0;
    // (at least one packet, empty when there is nothing: a stream with a packet each frame)
    do {
        packet.assign(12, 0);
        while (i < items.size()) {
            const size_t bytes = 4 + items[i].words.size() * 4;
            // (items a packet holds; one alone longer than `maxPayload` still goes, a packet of its own within the
            // UDP size limit)
            if (packet.size() > 12 && packet.size() - 12 + bytes > room) break;
            const size_t at = packet.size();
            packet.resize(at + bytes);
            put32(packet.data() + at, items[i].type << 10 | uint32_t(items[i].k) << 9 | uint32_t(items[i].words.size()));
            for (size_t w = 0; w < items[i].words.size(); ++w) put32(packet.data() + at + 4 + w * 4, items[i].words[w]);
            ++i;
        }
        // (ST 2110-41:2026: the marker bit 0 on every packet)
        rtpHeader(packet.data(), false, rtp, timestamp);
        ++rtp.sequence;
        ok = send(packet.data(), packet.size()) && ok;
    } while (ok && i < items.size());
    return ok;
}

std::vector<FastMetadata> segmentObject(uint32_t type, const uint8_t* data, size_t size, size_t maxWords) {
    std::vector<FastMetadata> out;
    const size_t per = std::clamp<size_t>(maxWords, 2, kMaxMetadataWords) - 1;
    const size_t words = (size + 3) / 4;
    for (size_t at = 0; at < words || (words == 0 && out.empty()); at += per) {
        FastMetadata m;
        m.type = type & 0x3FFFFF;
        const size_t n = std::min(per, words - at);
        m.k = at + n >= words;
        m.words.reserve(n + 1);
        m.words.push_back(uint32_t(at));
        for (size_t w = 0; w < n; ++w) {
            uint32_t v = 0;
            for (size_t b = 0; b < 4; ++b) {
                const size_t k = (at + w) * 4 + b;
                v = v << 8 | (k < size ? data[k] : 0);
            }
            m.words.push_back(v);
        }
        out.push_back(std::move(m));
        if (words == 0) break;
    }
    return out;
}

FastMetadata frameItem(int64_t frame, const output::Timecode& tc) {
    FastMetadata m;
    m.type = kFrameItemType;
    const uint32_t when = uint32_t(tc.dropFrame) << 31 | uint32_t(tc.hours & 0x7F) << 24 | uint32_t(tc.minutes & 0xFF) << 16 |
                          uint32_t(tc.seconds & 0xFF) << 8 | uint32_t(tc.frames & 0xFF);
    m.words = {uint32_t(uint64_t(frame) >> 32), uint32_t(uint64_t(frame)), when};
    return m;
}

bool parseFrameItem(const FastMetadata& m, int64_t& frame, output::Timecode* tc) {
    if (m.type != kFrameItemType || m.words.size() < 3) return false;
    frame = int64_t(uint64_t(m.words[0]) << 32 | m.words[1]);
    if (tc) {
        const uint32_t w = m.words[2];
        tc->dropFrame = w >> 31;
        tc->hours = int((w >> 24) & 0x7F), tc->minutes = int((w >> 16) & 0xFF), tc->seconds = int((w >> 8) & 0xFF), tc->frames = int(w & 0xFF);
    }
    return true;
}

bool joinObject(const std::vector<FastMetadata>& items, uint32_t type, std::vector<uint32_t>& out) {
    out.clear();
    for (const FastMetadata& m : items) {
        if (m.type != type || m.words.empty()) continue;
        // (a segment from offset 0 starts the object again)
        if (m.words[0] == 0) out.clear();
        else if (m.words[0] != out.size()) continue;
        out.insert(out.end(), m.words.begin() + 1, m.words.end());
        if (m.k) return true;
    }
    return false;
}

bool unpackFastMetadata(const uint8_t* payload, size_t size, std::vector<FastMetadata>& out) {
    size_t at = 0;
    while (at + 4 <= size) {
        const uint32_t h = get32(payload + at);
        FastMetadata m;
        m.type = h >> 10;
        m.k = (h >> 9) & 1;
        const size_t n = h & 0x1FF;
        at += 4;
        if (at + n * 4 > size) return false;
        m.words.resize(n);
        for (size_t w = 0; w < n; ++w) m.words[w] = get32(payload + at + w * 4);
        at += n * 4;
        out.push_back(std::move(m));
    }
    return at == size;
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
    sock_t s = kBad, s2 = kBad;  // (the second path's: ST 2022-7)
    sockaddr_in video{}, audio{}, anc{}, meta{}, video2{}, audio2{}, anc2{}, meta2{};
    std::function<bool(int, int, uint64_t)> drop;
    bool pathOn[4][2] = {{true, true}, {true, true}, {true, true}, {true, true}};
    uint64_t sent[4] = {0, 0, 0, 0};
    int64_t queueDrops = 0;  // packets the system's queue had no room for
    int64_t errors[2] = {0, 0};  // packets each path could not send (its interface down, no route)
#ifdef _WIN32
    // (Windows sleeps in steps of its timer, 15.6 ms unless asked for 1 ms: too coarse to pace packets within a frame,
    // so a sender would fall a frame behind and skip one)
    Sockets() { timeBeginPeriod(1); }
#else
    Sockets() = default;
#endif
    ~Sockets() {
        if (s != kBad) closeSock(s);
        if (s2 != kBad) closeSock(s2);
#ifdef _WIN32
        timeEndPeriod(1);
#endif
    }
    // A packet of stream 0 (video), 1 (audio), 2 (ancillary data) or 3 (fast metadata), on each path; false only when
    // every path failed (ST 2022-7: one path's error leaves the other sending, the error counted against its path).
    bool to(int stream, const uint8_t* data, size_t size) {
        const uint64_t index = sent[stream]++;
        const sockaddr_in* first[4] = {&video, &audio, &anc, &meta};
        const sockaddr_in* second[4] = {&video2, &audio2, &anc2, &meta2};
        const int paths = s2 != kBad ? 2 : 1;
        int failed = 0, tried = 0;
        for (int path = 0; path < paths; ++path) {
            if (!pathOn[stream][path]) continue;
            ++tried;  // (a leg turned off is not one that can fail)
            if (drop && drop(stream, path, index)) continue;
            const sockaddr_in& a = *(path ? second : first)[stream];
            // (a queue still full after a few milliseconds loses the packet, as a network would: the stream goes on)
            bool sent = false, full = false;
            for (int tries = 0; !sent && tries < 100; ++tries) {
                sent = ::sendto(path ? s2 : s, reinterpret_cast<const char*>(data), int(size), 0, reinterpret_cast<const sockaddr*>(&a), sizeof a) == int(size);
                full = !sent && queueFull();
                if (!full) break;
                std::this_thread::sleep_for(std::chrono::microseconds(50));  // (the queue drains in microseconds)
            }
            if (full) ++queueDrops;
            if (!sent && !full) ++errors[path], ++failed;
        }
        return tried == 0 || failed < tried;
    }
};

namespace {

// A sending socket: TTL, DSCP and (when given) its interface and port.
sock_t senderSocket(const SenderOptions& o, const std::string& interfaceAddress, std::string* why) {
    sock_t s = udpSocket();
    if (s == kBad) {
        *why = "Could not make a UDP socket";
        return s;
    }
    bufferSize(s, SO_SNDBUF, 8 << 20);
    option(s, IPPROTO_IP, IP_MULTICAST_TTL, int(std::clamp(o.ttl, 1, 255)));
    option(s, IPPROTO_IP, IP_TTL, int(std::clamp(o.ttl, 1, 255)));
    option(s, IPPROTO_IP, IP_TOS, int(34 << 2));  // DSCP AF41, media's class in AES67 and ST 2110 networks
    in_addr iface{};
    iface.s_addr = htonl(INADDR_ANY);
    if (!interfaceAddress.empty() && !ipv4(interfaceAddress, iface)) {
        *why = "Not an IPv4 address: " + interfaceAddress;
        closeSock(s);
        return kBad;
    }
    if (!interfaceAddress.empty() || o.sourcePort > 0) {
        // From the interface (and port) asked for; a port in use gives way to any free one.
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr = iface;
        local.sin_port = htons(uint16_t(std::clamp(o.sourcePort, 0, 65535)));
        option(s, SOL_SOCKET, SO_REUSEADDR, int(1));
        if (::bind(s, reinterpret_cast<const sockaddr*>(&local), sizeof local) != 0) {
            local.sin_port = 0;
            if (::bind(s, reinterpret_cast<const sockaddr*>(&local), sizeof local) != 0) {
                *why = "This computer has no interface at " + interfaceAddress;
                closeSock(s);
                return kBad;
            }
        }
        if (!interfaceAddress.empty()) setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&iface), sizeof iface);
    }
    return s;
}

// The address packets to `to` leave from: the interface's, else the one routing picks.
std::string sourceAddress(const std::string& interfaceAddress, const sockaddr_in& to) {
    if (!interfaceAddress.empty()) return interfaceAddress;
    std::string out;
    sock_t t = udpSocket();
    sockaddr_in local{};
    socklen_t len = sizeof local;
    char text[INET_ADDRSTRLEN] = {};
    if (t != kBad && ::connect(t, reinterpret_cast<const sockaddr*>(&to), sizeof to) == 0 &&
        ::getsockname(t, reinterpret_cast<sockaddr*>(&local), &len) == 0 && inet_ntop(AF_INET, &local.sin_addr, text, sizeof text))
        out = text;
    if (t != kBad) closeSock(t);
    return out == "0.0.0.0" ? std::string() : out;
}

}  // namespace

struct Sender::Jxs {
    JxsEncoder encoder;
};

namespace {
std::string withoutSpaces(std::string t) {
    t.erase(std::remove(t.begin(), t.end(), ' '), t.end());
    return t;
}
}  // namespace

Sender::Sender() = default;
Sender::~Sender() { close(); }

bool Sender::open(const SenderOptions& o, const VideoFormat& video, const AudioFormat& audio, std::string* error) {
    close();
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        s_.reset();
        return false;
    };
    std::string why;
    if (!videoFormatOk(video, &why)) return fail(why);
    in_addr probe;
    for (const Endpoint* e : {&o.video, &o.audio, &o.anc})
        if (!ipv4(e->address, probe) || e->port <= 0 || e->port > 65535) return fail("Not an IPv4 address and port: " + endpointText(*e));
    if (o.redundant)
        for (const Endpoint* e : {&o.video2, &o.audio2, &o.anc2})
            if (!ipv4(e->address, probe) || e->port <= 0 || e->port > 65535) return fail("Not an IPv4 address and port: " + endpointText(*e));
    if (o.sendMetadata)
        for (const Endpoint* e : {&o.metadata, &o.metadata2}) {
            if (e == &o.metadata2 && !o.redundant) continue;
            if (!ipv4(e->address, probe) || e->port <= 0 || e->port > 65535) return fail("Not an IPv4 address and port: " + endpointText(*e));
        }
    // (a 1 ms packet of 4-byte words keeps within a network's 1,500 bytes up to 6 subframes)
    if (o.sendAudio && audio.aes3 && (audio.rate != 48000 || audio.channels < 1 || (audio.packetUs != 1000 && audio.packetUs != 125) ||
                                      audio.channels > (audio.packetUs == 125 ? 8 : 6)))
        return fail("ST 2110-31 here carries AES3 subframes at 48 kHz: 1 to 6 in 1 ms packets, or up to 8 in 125 µs ones");
    if (o.sendAudio && !audio.aes3 && (audio.rate != 48000 || audio.channels < 1 || audio.channels > 8 || (audio.bits != 24 && audio.bits != 16) ||
                        (audio.packetUs != 1000 && audio.packetUs != 125)))
        return fail("ST 2110-30 here carries 1 to 8 channels at 48 kHz, L24 or L16, in 1 ms or 125 µs packets");
    // ST 2110-22: the encoder, and from a first codestream the stream's profile, level, packets and bandwidth.
    VideoFormat vf = video;
    jxs_.reset();
    codestream_.clear();
    boxes_.clear();
    units_.clear();
    if (vf.jpegXs() && o.sendVideo) {
        if (!jpegXsAvailable()) return fail("This build of Montage has no JPEG XS (SVT-JPEG-XS)");
        jxs_ = std::make_unique<Jxs>();
        JxsEncodeSettings es;
        es.width = vf.width;
        es.height = vf.height;
        es.depth = vf.depth;
        es.sampling = vf.sampling == Sampling::Ycc422 ? JxsSampling::Ycc422 : vf.sampling == Sampling::Ycc444 ? JxsSampling::Ycc444 : JxsSampling::Rgb;
        es.bppNum = int(std::lround(vf.bpp * 1000));
        es.bppDen = 1000;
        es.slices = vf.packetMode == 1;
        if (!jxs_->encoder.open(es, &why)) return fail(why);
        Raster grey(vf.width, vf.height, vf.sampling, vf.depth);
        std::fill(grey.samples.begin(), grey.samples.end(), uint16_t(1 << (vf.depth - 1)));
        std::vector<uint8_t> cs;
        std::vector<uint32_t> units;
        JxsHeader h;
        if (!jxs_->encoder.encode(jxsPictureOf(grey), cs, &units, &why)) return fail(why);
        if (!readJxsHeader(cs.data(), cs.size(), h)) return fail("SVT-JPEG-XS made no JPEG XS codestream");
        vf.profile = withoutSpaces(jxsProfileName(h.ppih));
        vf.level = withoutSpaces(jxsLevelName(h.plev));
        vf.sublevel = withoutSpaces(jxsSublevelName(h.plev));
        boxes_ = jxsvBoxes(vf, h.ppih, h.plev, uint32_t(cs.size()));
        // (ST 2110-22's b=AS: each frame's IP packets, their IPv4, UDP and RTP headers and RFC 9134's, in kilobits a
        // second, rounded up)
        const int packets = jxsvPacketCount(boxes_.size(), cs.size(), units, vf.packetMode, o.maxPayload);
        const double bits = (double(boxes_.size() + cs.size()) + double(packets) * (20 + 8 + 12 + 4)) * 8;
        vf.bandwidthKbps = int64_t(std::ceil(bits * vf.fps.num / vf.fps.den / 1000));
    }
    s_ = std::make_unique<Sockets>();
    s_->s = senderSocket(o, o.interfaceAddress, &why);
    if (s_->s == kBad) return fail(why);
    if (o.redundant) {
        s_->s2 = senderSocket(o, o.interfaceAddress2, &why);
        if (s_->s2 == kBad) return fail(why);
        s_->video2 = socketAddress(o.video2);
        s_->audio2 = socketAddress(o.audio2);
        s_->anc2 = socketAddress(o.anc2);
        if (o.sendMetadata) s_->meta2 = socketAddress(o.metadata2);
    }
    if (o.sendMetadata) s_->meta = socketAddress(o.metadata);
    s_->drop = o.drop;
    std::memcpy(s_->pathOn, o.pathOn, sizeof s_->pathOn);
    s_->video = socketAddress(o.video);
    s_->audio = socketAddress(o.audio);
    s_->anc = socketAddress(o.anc);
    o_ = o;
    video_ = vf;
    audio_ = audio;
    std::random_device rd;
    std::mt19937 rng(rd());
    rtpVideo_ = {uint8_t(o.videoPayload), uint32_t(rng()), uint32_t(rng())};
    rtpAudio_ = {uint8_t(o.audioPayload), uint32_t(rng()), uint32_t(rng())};
    rtpAnc_ = {uint8_t(o.ancPayload), uint32_t(rng()), uint32_t(rng())};
    rtpMeta_ = {uint8_t(o.metadataPayload), uint32_t(rng()), uint32_t(rng())};
    nextFrame_ = 0;
    audioSample_ = 0;
    audioFifo_.clear();
    aesFifo_.clear();
    aesBlockFrame_ = 0;
    aesQueued_ = false;
    frameAnc_.clear();
    nextMetadata_.clear();
    frames_ = 0;
    ptpNowNs(&clock_);
    source_ = sourceAddress(o.interfaceAddress, s_->video);
    source2_ = o.redundant ? sourceAddress(o.interfaceAddress2, s_->video2) : std::string();
    return true;
}

void Sender::close() {
    s_.reset();
    jxs_.reset();
}

bool Sender::isOpen() const { return s_ != nullptr; }
int64_t Sender::packetsDropped() const { return s_ ? s_->queueDrops : 0; }
int64_t Sender::sendErrors(int path) const { return s_ ? s_->errors[path & 1] : 0; }

namespace {
SdpOptions sdpOptions(const SenderOptions& o, const std::string& source, const std::string& source2) {
    SdpOptions s;
    s.source = source;
    s.source2 = source2;
    s.refclk = o.refclk;
    s.ttl = o.ttl;
    s.session = uint64_t(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    return s;
}
}  // namespace

std::string Sender::sdpVideo() const {
    return isOpen() && o_.sendVideo
               ? videoSdp(video_, o_.video, o_.videoPayload, sdpOptions(o_, source_, source2_), o_.redundant ? &o_.video2 : nullptr)
               : std::string();
}
std::string Sender::sdpAudio() const {
    return isOpen() && o_.sendAudio
               ? audioSdp(audio_, o_.audio, o_.audioPayload, sdpOptions(o_, source_, source2_), o_.redundant ? &o_.audio2 : nullptr)
               : std::string();
}
std::string Sender::sdpAnc() const {
    if (!isOpen() || !o_.sendAnc) return {};
    std::vector<std::pair<int, int>> ids{{0x60, 0x60}, {0x61, 0x01}};
    for (const AncPacket& a : frameAnc_)
        if (std::find(ids.begin(), ids.end(), std::pair<int, int>{a.did, a.sdid}) == ids.end()) ids.emplace_back(a.did, a.sdid);
    return ancSdp(video_, o_.anc, o_.ancPayload, sdpOptions(o_, source_, source2_), ids, o_.redundant ? &o_.anc2 : nullptr);
}

std::string Sender::sdpMetadata() const {
    if (!isOpen() || !o_.sendMetadata) return {};
    return metadataSdp(o_.metadata, o_.metadataPayload, sdpOptions(o_, source_, source2_), o_.metadataTypes, o_.redundant ? &o_.metadata2 : nullptr);
}

void Sender::queueAes3(const uint32_t* words, int frames) {
    if (!s_ || !audio_.aes3 || !words || frames <= 0) return;
    aesQueued_ = true;  // (in place of send's PCM from now)
    aesFifo_.insert(aesFifo_.end(), words, words + size_t(frames) * size_t(audio_.channels));
}

bool Sender::send(const uint8_t* data, size_t stride, output::PixelFormat format, const float* audio, int audioFrames,
                  const output::Timecode* timecode, const std::vector<AncPacket>& anc, const std::atomic<bool>* stop) {
    if (!s_) return false;
    if (video_.jpegXs()) {
        // (the rows as a raster of 4:2:2 at their depth, then coded)
        if (format == output::PixelFormat::P216 || !data || video_.sampling != Sampling::Ycc422 || video_.depth != (tenBit(format) ? 10 : 8)) return false;
        Raster r(video_.width, video_.height, Sampling::Ycc422, video_.depth);
        for (int y = 0; y < video_.height; ++y) rowComponents(data + size_t(y) * stride, format, video_.width, r.row(y));
        return sendJxs(r, audio, audioFrames, timecode, anc, stop);
    }
    // (rows of the reference output: 4:2:2 at the video format's size)
    return sendFrame([&](int field, uint32_t ts, const PacketSink& keep) {
        return packVideo(data, stride, format, video_.width, video_.height, field, ts, rtpVideo_, keep, o_.maxPayload);
    }, audio, audioFrames, timecode, anc, stop);
}

bool Sender::send(const Raster& picture, const float* audio, int audioFrames, const output::Timecode* timecode,
                  const std::vector<AncPacket>& anc, const std::atomic<bool>* stop) {
    if (!s_ || picture.width != video_.width || picture.height != video_.height || picture.sampling != video_.sampling ||
        picture.depth != video_.depth)
        return false;
    if (video_.jpegXs()) return sendJxs(picture, audio, audioFrames, timecode, anc, stop);
    return sendFrame([&](int field, uint32_t ts, const PacketSink& keep) { return packVideo(picture, field, ts, rtpVideo_, keep, o_.maxPayload); },
                     audio, audioFrames, timecode, anc, stop);
}

bool Sender::sendJxs(const Raster& picture, const float* audio, int audioFrames, const output::Timecode* timecode, const std::vector<AncPacket>& anc,
                     const std::atomic<bool>* stop) {
    // Coded before its slot comes (a constant number of bytes, so of packets), then sent as the slot's packets are.
    if (o_.sendVideo) {
        if (!jxs_) return false;
        std::string why;
        if (!jxs_->encoder.encode(jxsPictureOf(picture), codestream_, &units_, &why)) return false;
    }
    const int64_t frame = frames_;
    return sendFrame([&](int, uint32_t ts, const PacketSink& keep) {
        return packJxsv(boxes_, codestream_.data(), codestream_.size(), units_, video_.packetMode, frame, ts, rtpVideo_, keep, o_.maxPayload);
    }, audio, audioFrames, timecode, anc, stop);
}

bool Sender::sendFrame(const std::function<bool(int field, uint32_t ts, const PacketSink& keep)>& pack, const float* audio, int audioFrames,
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
    // The frame's slot: the next one, or (starting, or behind by more than a fifth of a second) the next to start from
    // now. A moment's hold-up (a busy machine) loses nothing: late frames go out straight away in their own slots, paced
    // as on time (below), the receiver placing them by their timestamps, the sender catching up in the gaps it leaves.
    const int64_t now = ptpNowNs();
    const int64_t catchUp = std::max<int64_t>(1, num / (5 * den));
    if (nextFrame_ == 0 || frameStart(nextFrame_ + catchUp) < now) {
        const bool behind = nextFrame_ != 0;
        const int64_t from = mulDiv(now, num, second * den) + 1;
        if (behind) skipped_ += std::max<int64_t>(0, from - nextFrame_);
        nextFrame_ = from;
        audioSample_ = mulDiv(frameStart(nextFrame_), audio_.rate, second);
        audioFifo_.clear();
        // (AES3 words fallen behind dropped as the PCM is, all but a frame's, in whole blocks of 192 frames so the
        // channel status runs on)
        const size_t ch = size_t(std::max(1, audio_.channels)), frames = aesFifo_.size() / ch;
        const size_t due = size_t(mulDiv(int64_t(audio_.rate), den, num)) + 1;
        if (behind && frames > due) {
            const size_t drop = (frames - due) - (frames - due) % 192;
            aesFifo_.erase(aesFifo_.begin(), aesFifo_.begin() + ptrdiff_t(drop * ch));
        }
    }
    const int64_t start = frameStart(nextFrame_), end = frameStart(nextFrame_ + 1);
    if (o_.sendAudio && audio && audioFrames > 0) {
        if (audio_.aes3 && aesQueued_) {
            // (its subframes come from queueAes3)
        } else if (audio_.aes3) {
            const std::vector<uint32_t> w = aes3FromPcm(audio, audioFrames, audio_.channels, aesBlockFrame_, aes3ChannelStatus(false));
            aesFifo_.insert(aesFifo_.end(), w.begin(), w.end());
        } else {
            audioFifo_.insert(audioFifo_.end(), audio, audio + size_t(audioFrames) * size_t(audio_.channels));
        }
    }
    if (!waitUntil(start)) return false;

    bool ok = true;
    auto sendAudioDue = [&](int64_t until) {
        // Packets (1 ms or 125 µs) whose last sample's time has come.
        const int per = std::max(1, audio_.packetSamples()), ch = audio_.channels;
        while (ok && o_.sendAudio && audio_.aes3 && aesFifo_.size() >= size_t(per) * size_t(ch) &&
               mulDiv(audioSample_ + per, second, audio_.rate) <= until) {
            ok = packAm824(aesFifo_.data(), per, ch, uint32_t(audioSample_), rtpAudio_, [&](const uint8_t* p, size_t n) { return s_->to(1, p, n); });
            aesFifo_.erase(aesFifo_.begin(), aesFifo_.begin() + ptrdiff_t(per) * ch);
            audioSample_ += per;
        }
        while (ok && o_.sendAudio && !audio_.aes3 && audioFifo_.size() >= size_t(per) * size_t(ch) &&
               mulDiv(audioSample_ + per, second, audio_.rate) <= until) {
            ok = packAudio(audioFifo_.data(), per, ch, uint32_t(audioSample_), rtpAudio_,
                           [&](const uint8_t* p, size_t n) { return s_->to(1, p, n); }, audio_.bits);
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
            ok = packAnc(out, field, ts, rtpAnc_, [&](const uint8_t* p, size_t n) { return s_->to(2, p, n); });
        }
        if (ok && o_.sendMetadata && f == 0) {
            ok = packFastMetadata(nextMetadata_, ts, rtpMeta_, [&](const uint8_t* p, size_t n) { return s_->to(3, p, n); }, o_.maxPayload);
            nextMetadata_.clear();
        }
        packets.clear();
        if (ok && o_.sendVideo)
            ok = pack(field, ts, [&](const uint8_t* p, size_t n) {
                packets.emplace_back(p, p + n);
                return true;
            });
        // From ST 2110-21's default offset (43 lines of 1125 into a progressive frame, 22 into a field), spread over
        // most of what is left (a wide sender's pacing), in bursts of what is due. Late (catching up after a hold-up),
        // from now at the same pace, never faster: the sender catches up by the offset and the tenth it leaves each
        // frame (some 13 % of its time: a fifth of a second's hold-up made up in about a second and a half).
        const int64_t troffset = (end - start) * (video_.interlaced ? 22 : 43) / 1125;
        int64_t vStart = fStart + troffset;
        const int64_t span = (fEnd - vStart) * 9 / 10;
        if (!waitUntil(vStart)) return false;
        if (const int64_t late = ptpNowNs() - vStart; late > 0) vStart += late;
        size_t i = 0;
        while (ok && i < packets.size()) {
            const int64_t t = ptpNowNs();
            const size_t due = std::min(packets.size(), size_t(std::max<int64_t>(1, (t - vStart) * int64_t(packets.size()) / std::max<int64_t>(1, span) + 1)));
            for (; ok && i < due; ++i) {
                if (stopped()) return false;
                ok = s_->to(0, packets[i].data(), packets[i].size());
            }
            sendAudioDue(t);
            if (i < packets.size()) {
                if (stopped()) return false;
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        }
        if (ok && o_.sendMetadata && f == 0 && end - start > second / 2) {
            // (ST 2110-41: a packet at least every 500 ms, so below two frames a second an empty one between)
            if (!waitUntil(start + (end - start) / 2)) return false;
            ok = packFastMetadata({}, ts, rtpMeta_, [&](const uint8_t* p, size_t n) { return s_->to(3, p, n); }, o_.maxPayload);
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
    std::vector<uint8_t> datagram;  // (as large as one can be, made once: what came is copied out at its own size)
    ~Socket() {
        if (s != kBad) closeSock(s);
    }
};

Receiver::Receiver() = default;
Receiver::~Receiver() = default;

bool Receiver::open(const Endpoint& e, const std::string& interfaceAddress, std::string* error, const std::string& source) {
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
    bufferSize(s_->s, SO_RCVBUF, 32 << 20);
    const bool multicast = isMulticast(e.address);
    sockaddr_in local = socketAddress(e);
    if (multicast) {
#ifdef _WIN32
        local.sin_addr.s_addr = htonl(INADDR_ANY);  // (Windows binds no multicast address)
#endif
    }
    if (::bind(s_->s, reinterpret_cast<const sockaddr*>(&local), sizeof local) != 0) return fail("Could not listen at " + endpointText(e));
    if (multicast) {
        // From the one source named (source-specific: what SSM networks need, and nobody else's packets taken), else
        // from any.
        bool joined = false;
        if (!source.empty() && source != "0.0.0.0") {
            ip_mreq_source m{};  // (fields by name: their order differs between systems)
            ipv4(e.address, m.imr_multiaddr);
            ipv4(source, m.imr_sourceaddr);
            if (!interfaceAddress.empty()) ipv4(interfaceAddress, m.imr_interface);
            joined = setsockopt(s_->s, IPPROTO_IP, IP_ADD_SOURCE_MEMBERSHIP, reinterpret_cast<const char*>(&m), sizeof m) == 0;
        }
        if (!joined) {
            ip_mreq m{};
            ipv4(e.address, m.imr_multiaddr);
            if (!interfaceAddress.empty()) ipv4(interfaceAddress, m.imr_interface);
            if (setsockopt(s_->s, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&m), sizeof m) != 0)
                return fail("Could not join " + e.address);
        }
    }
    sockaddr_in bound{};
    socklen_t len = sizeof bound;
    if (::getsockname(s_->s, reinterpret_cast<sockaddr*>(&bound), &len) == 0) s_->port = ntohs(bound.sin_port);
    return true;
}

bool Receiver::receive(std::vector<uint8_t>& out, int ms) {
    if (!s_ || !pollIn(s_->s, ms)) return false;
    // (not into `out` made large: that would clear 64 KB for each packet, and a packet held would keep it all)
    if (s_->datagram.empty()) s_->datagram.resize(65536);
    const auto n = ::recv(s_->s, reinterpret_cast<char*>(s_->datagram.data()), int(s_->datagram.size()), 0);
    if (n <= 0) return false;
    out.assign(s_->datagram.begin(), s_->datagram.begin() + ptrdiff_t(n));
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
            // (4:2:2 at 8 and 10 bits from UYVY and v210; 12 bits, 4:4:4 and RGB from the 16-bit formats)
            d.formats = {PixelFormat::V210, PixelFormat::Uyvy8, PixelFormat::P216, PixelFormat::Ycc444, PixelFormat::Rgb48};
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
        v.depth = signal.format == PixelFormat::V210 ? 10 : signal.format == PixelFormat::Uyvy8 ? 8 : std::clamp(signal.bits, 8, 12);
        v.sampling = signal.format == PixelFormat::Ycc444 ? st2110::Sampling::Ycc444
                     : signal.format == PixelFormat::Rgb48 ? st2110::Sampling::Rgb
                                                           : st2110::Sampling::Ycc422;
        v.tcs = signal.eotf == Eotf::Pq ? "PQ" : signal.eotf == Eotf::Hlg ? "HLG" : "SDR";
        v.colorimetry = signal.eotf != Eotf::Sdr ? "BT2100" : signal.bt2020 ? "BT2020" : "BT709";
        if (o.jpegXs) {
            // (ST 2110-22: the same pictures coded as JPEG XS)
            v.codec = "jxsv";
            v.bpp = o.jpegXsBpp;
            v.packetMode = o.jpegXsPacketMode;
        }
        st2110::AudioFormat a;
        a.channels = std::max(1, audioChannels);
        a.rate = audioRate;
        o.sendAudio = o.sendAudio && audioChannels > 0;
        // (ST 2110-41: each frame's number and time code, the item named in the SDP)
        if (o.sendMetadata && std::find(o.metadataTypes.begin(), o.metadataTypes.end(), st2110::kFrameItemType) == o.metadataTypes.end())
            o.metadataTypes.push_back(st2110::kFrameItemType);
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
        const float* audio = a.channels > 0 ? a.samples : nullptr;
        const int audioFrames = a.channels > 0 ? a.frames : 0;
        if (sender_.options().sendMetadata) sender_.setNextMetadata({st2110::frameItem(f.frame, f.timecode)});
        if (f.format == PixelFormat::V210 || f.format == PixelFormat::Uyvy8)
            return sender_.send(f.data, f.stride, f.format, audio, audioFrames, &f.timecode, anc, &stop_);
        // The 16-bit formats' samples cut to the stream's depth.
        const st2110::VideoFormat& v = sender_.videoFormat();
        st2110::Raster r(v.width, v.height, v.sampling, v.depth);
        const int shift = 16 - v.depth, top = (1 << v.depth) - 1;
        auto cut = [&](const uint8_t* p) { return uint16_t(std::min(top, (int(p[0] | p[1] << 8) + (1 << (shift - 1))) >> shift)); };
        for (int y = 0; y < v.height; ++y) {
            uint16_t* out = r.row(y);
            if (f.format == PixelFormat::P216) {
                // (a plane of Y, then one of Cb Cr pairs: to Cb Y Cr Y)
                const uint8_t* py = f.data + size_t(y) * f.stride;
                const uint8_t* puv = f.data + f.stride * size_t(v.height) + size_t(y) * f.stride;
                for (int x = 0; x < v.width; x += 2) {
                    out[2 * x] = cut(puv + 2 * x);
                    out[2 * x + 1] = cut(py + 2 * x);
                    out[2 * x + 2] = cut(puv + 2 * x + 2);
                    out[2 * x + 3] = cut(py + 2 * x + 2);
                }
            } else {
                const uint8_t* p = f.data + size_t(y) * f.stride;
                for (int i = 0; i < v.width * 3; ++i) out[i] = cut(p + 2 * i);
            }
        }
        return sender_.send(r, audio, audioFrames, &f.timecode, anc, &stop_);
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
        if (!sender_.sdpMetadata().empty()) out["metadata"] = sender_.sdpMetadata();
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
