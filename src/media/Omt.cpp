// Montage — Open Media Transport: the wire format, VMX, the sender and the receiver (media/Omt.h). Discovery is in
// OmtDiscovery.cpp. Written from libomtnet's source (MIT, Copyright (c) 2025 Open Media Transport Contributors):
// commands are sent without a NUL and matched whole (trailing NULs taken off), metadata frames carry no per-frame
// metadata length, and a sender greets each connection with its OMTInfo and the tally before any subscription.
#include "Omt.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>

#include <QXmlStreamReader>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <cpuid.h>
#endif

#include "vmxcodec.h"

#include "OmtNet.h"

namespace montage::omt {

// ---------------------------------------------------------------------------
// The wire

namespace {

void put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v), p[1] = uint8_t(v >> 8); }
void put32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i));
}
void put64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = uint8_t(v >> (8 * i));
}
uint16_t get16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint64_t get64(const uint8_t* p) { return uint64_t(get32(p)) | uint64_t(get32(p + 4)) << 32; }

}  // namespace

void writeHeader(const Header& h, uint8_t* out) {
    out[0] = h.version;
    out[1] = h.type;
    put64(out + 2, uint64_t(h.timestamp));
    put16(out + 10, h.metadataLength);
    put32(out + 12, uint32_t(h.dataLength));
}

bool readHeader(const uint8_t* in, Header& h) {
    h.version = in[0];
    h.type = in[1];
    h.timestamp = int64_t(get64(in + 2));
    h.metadataLength = get16(in + 10);
    h.dataLength = int32_t(get32(in + 12));
    return h.version == 1 && (h.type == Metadata || h.type == Video || h.type == Audio) && h.dataLength >= 0;
}

void writeVideoHeader(const VideoHeader& v, uint8_t* out) {
    put32(out, v.codec);
    put32(out + 4, uint32_t(v.width));
    put32(out + 8, uint32_t(v.height));
    put32(out + 12, uint32_t(v.rateN));
    put32(out + 16, uint32_t(v.rateD));
    uint32_t aspect;
    std::memcpy(&aspect, &v.aspect, 4);
    put32(out + 20, aspect);
    put32(out + 24, uint32_t(v.flags));
    put32(out + 28, uint32_t(v.colorSpace));
}

VideoHeader readVideoHeader(const uint8_t* in) {
    VideoHeader v;
    v.codec = get32(in);
    v.width = int32_t(get32(in + 4));
    v.height = int32_t(get32(in + 8));
    v.rateN = int32_t(get32(in + 12));
    v.rateD = int32_t(get32(in + 16));
    const uint32_t aspect = get32(in + 20);
    std::memcpy(&v.aspect, &aspect, 4);
    v.flags = int32_t(get32(in + 24));
    v.colorSpace = int32_t(get32(in + 28));
    return v;
}

void writeAudioHeader(const AudioHeader& a, uint8_t* out) {
    put32(out, a.codec);
    put32(out + 4, uint32_t(a.sampleRate));
    put32(out + 8, uint32_t(a.samplesPerChannel));
    put32(out + 12, uint32_t(a.channels));
    put32(out + 16, a.activeChannels);
    put32(out + 20, uint32_t(a.reserved));
}

AudioHeader readAudioHeader(const uint8_t* in) {
    AudioHeader a;
    a.codec = get32(in);
    a.sampleRate = int32_t(get32(in + 4));
    a.samplesPerChannel = int32_t(get32(in + 8));
    a.channels = int32_t(get32(in + 12));
    a.activeChannels = get32(in + 16);
    a.reserved = int32_t(get32(in + 20));
    return a;
}

std::string subscribeCommand(FrameType type) {
    return type == Video ? "<OMTSubscribe Video=\"true\" />" : type == Audio ? "<OMTSubscribe Audio=\"true\" />" : "<OMTSubscribe Metadata=\"true\" />";
}
std::string tallyCommand(bool preview, bool program) {
    return std::string("<OMTTally Preview=\"") + (preview ? "true" : "false") + "\" Program==\"" + (program ? "true" : "false") + "\" />";
}
std::string previewCommand(bool on) { return std::string("<OMTSettings Preview=\"") + (on ? "true" : "false") + "\" />"; }
std::string qualityCommand(Quality q) {
    const char* names[] = {"Default", "Low", "Medium", "High"};
    return std::string("<OMTSettings Quality=\"") + names[int(q)] + "\" />";
}

std::vector<uint8_t> encodeFpa1(const float* planar, int channels, int samples, uint32_t* active) {
    std::vector<uint8_t> out;
    *active = 0;
    if (channels <= 0 || samples <= 0) return out;
    for (int c = 0; c < std::min(channels, 32); ++c) {
        const float* p = planar + size_t(c) * size_t(samples);
        if (!std::any_of(p, p + samples, [](float v) { return v != 0.0f; })) continue;
        *active |= 1u << c;
        const size_t at = out.size();
        out.resize(at + size_t(samples) * 4);
        std::memcpy(out.data() + at, p, size_t(samples) * 4);  // (little-endian floats, as every machine Montage runs on)
    }
    return out;
}

bool plausible(const VideoHeader& v) {
    return v.width >= 16 && v.height >= 16 && v.width <= kMaxWidth && v.height <= kMaxHeight && v.width % 2 == 0;
}

bool plausible(const AudioHeader& a, size_t dataBytes) {
    if (a.channels <= 0 || a.channels > kMaxChannels || a.samplesPerChannel <= 0 || a.samplesPerChannel > kMaxSamples || a.sampleRate < 8000 ||
        a.sampleRate > 192000)
        return false;
    size_t active = 0;
    for (int c = 0; c < a.channels; ++c) active += (a.activeChannels >> c) & 1u;
    return dataBytes >= active * size_t(a.samplesPerChannel) * 4;
}

bool decodeFpa1(const uint8_t* data, size_t size, int channels, int samples, uint32_t active, float* planar) {
    std::fill(planar, planar + size_t(std::max(0, channels)) * size_t(std::max(0, samples)), 0.0f);
    size_t at = 0;
    for (int c = 0; c < std::min(channels, 32); ++c) {
        if (!(active & (1u << c))) continue;
        if (at + size_t(samples) * 4 > size) return false;
        std::memcpy(planar + size_t(c) * size_t(samples), data + at, size_t(samples) * 4);
        at += size_t(samples) * 4;
    }
    return true;
}

std::string machineName() {
    net::init();
    char b[256] = {};
    if (gethostname(b, sizeof b - 1) != 0 || !b[0]) return "MONTAGE";
    std::string s = b;
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string fullName(const std::string& name) {
    const std::string host = machineName();
    std::string n = name;
    std::string h = host;
    const int over = int(h.size() + n.size() + 3) - 63;
    if (over > 0 && over < int(n.size())) {
        n = n.substr(0, n.size() - size_t(over));
        while (!n.empty() && n.back() == ' ') n.pop_back();
    } else if (over > 0) {
        // (a computer name too long even for a one-letter source, as cloud machines have: it is shortened instead,
        // since DNS-SD cuts an instance name at 63 bytes and the source could no longer be found by its name)
        n = n.substr(0, std::min<size_t>(n.size(), 16));
        h = h.substr(0, size_t(std::max(1, 63 - int(n.size()) - 3)));
        while (h.size() > 1 && (h.back() == '.' || h.back() == '-')) h.pop_back();
    }
    return h + " (" + n + ")";
}

std::string xmlAttribute(const std::string& xml, const std::string& name) {
    // (attributes Montage reads are plain: name="value", no entities but the usual five)
    const std::string key = " " + name + "=\"";
    const size_t at = xml.find(key);
    if (at == std::string::npos) return {};
    const size_t start = at + key.size(), end = xml.find('"', start);
    if (end == std::string::npos) return {};
    std::string v = xml.substr(start, end - start);
    const std::pair<const char*, char> entities[] = {{"&quot;", '"'}, {"&apos;", '\''}, {"&lt;", '<'}, {"&gt;", '>'}, {"&amp;", '&'}};
    for (const auto& [e, c] : entities)
        for (size_t p; (p = v.find(e)) != std::string::npos;) v.replace(p, std::strlen(e), 1, c);
    return v;
}

// ---------------------------------------------------------------------------
// VMX

size_t pictureBytes(PictureFormat f, int width, int height) {
    return size_t(width) * size_t(height) * (f == PictureFormat::P216 ? 4 : 2);
}

int vmxProfile(Quality q) {
    return q == Quality::Low ? VMX_PROFILE_OMT_LQ : q == Quality::High ? VMX_PROFILE_OMT_HQ : VMX_PROFILE_OMT_SQ;
}

bool vmxAvailable(std::string* why) {
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
    // (libvmx's x86 code is built for LZCNT and BMI1 throughout, as its own builds are)
    unsigned a = 0, b = 0, c = 0, d = 0;
    const bool lzcnt = __get_cpuid(0x80000001, &a, &b, &c, &d) && (c & (1u << 5));
    const bool bmi = __get_cpuid_count(7, 0, &a, &b, &c, &d) && (b & (1u << 3));
    if (!lzcnt || !bmi) {
        if (why) *why = "Open Media Transport's VMX codec needs a processor from 2013 or later (LZCNT and BMI1)";
        return false;
    }
#endif
    (void)why;
    return true;
}

VmxEncoder::~VmxEncoder() { close(); }

void VmxEncoder::close() {
    if (inst_) VMX_Destroy(static_cast<VMX_INSTANCE*>(inst_));
    inst_ = nullptr;
}

bool VmxEncoder::open(int width, int height, int profile, int colorSpace, double fps) {
    close();
    if (!vmxAvailable()) return false;
    VMX_SIZE size{width, height};
    inst_ = VMX_Create(size, VMX_PROFILE(profile), VMX_COLORSPACE(colorSpace == 601 || colorSpace == 709 ? colorSpace : 0));
    if (!inst_) return false;
    width_ = width, height_ = height, profile_ = profile;
    // (as libomtnet: twice the threads above 60 frames a second)
    if (fps > 60) VMX_SetThreads(static_cast<VMX_INSTANCE*>(inst_), VMX_GetThreads(static_cast<VMX_INSTANCE*>(inst_)) * 2);
    return true;
}

bool VmxEncoder::encode(PictureFormat f, const uint8_t* data, int stride, bool interlaced, std::vector<uint8_t>& out) {
    auto* inst = static_cast<VMX_INSTANCE*>(inst_);
    if (!inst) return false;
    auto* src = const_cast<BYTE*>(data);
    const VMX_ERR err = f == PictureFormat::P216 ? VMX_EncodeP216(inst, src, stride, interlaced ? 1 : 0)
                                                 : VMX_EncodeUYVY(inst, src, stride, interlaced ? 1 : 0);
    if (err != VMX_ERR_OK) return false;
    const size_t at = out.size();
    const size_t room = size_t(width_) * size_t(height_) * 4 + 4096;
    out.resize(at + room);
    const int n = VMX_SaveTo(inst, out.data() + at, int(room));
    out.resize(at + size_t(std::max(0, n)));
    return n > 0;
}

int VmxEncoder::previewLength() const {
    return inst_ ? VMX_GetEncodedPreviewLength(static_cast<VMX_INSTANCE*>(inst_)) : 0;
}

VmxDecoder::~VmxDecoder() {
    if (inst_) VMX_Destroy(static_cast<VMX_INSTANCE*>(inst_));
}

bool VmxDecoder::decode(const uint8_t* data, size_t size, int width, int height, int colorSpace, PictureFormat f, uint8_t* out, int stride) {
    if (!vmxAvailable() || width <= 0 || height <= 0 || size == 0 || size > size_t(INT32_MAX)) return false;
    if (!inst_ || width != width_ || height != height_ || colorSpace != colorSpace_) {
        if (inst_) VMX_Destroy(static_cast<VMX_INSTANCE*>(inst_));
        VMX_SIZE sz{width, height};
        // (the profile is in each picture; the instance's only sizes the tables)
        inst_ = VMX_Create(sz, VMX_PROFILE_OMT_SQ, VMX_COLORSPACE(colorSpace == 601 || colorSpace == 709 ? colorSpace : 0));
        if (!inst_) return false;
        width_ = width, height_ = height, colorSpace_ = colorSpace;
    }
    auto* inst = static_cast<VMX_INSTANCE*>(inst_);
    if (VMX_LoadFrom(inst, const_cast<BYTE*>(data), int(size)) != VMX_ERR_OK) return false;
    return (f == PictureFormat::P216 ? VMX_DecodeP216(inst, out, stride) : VMX_DecodeUYVY(inst, out, stride)) == VMX_ERR_OK;
}

// ---------------------------------------------------------------------------
// Sending

namespace {

using Bytes = std::shared_ptr<const std::vector<uint8_t>>;

// A frame's bytes: header, extended header, data and metadata (with its NUL; commands go without).
Bytes buildFrame(FrameType type, int64_t timestamp, const uint8_t* ext, int extBytes, const uint8_t* data, size_t dataBytes,
                 const std::string& metadata, bool metadataNul) {
    auto out = std::make_shared<std::vector<uint8_t>>();
    // (metadata too long for the header's 16-bit length is left out rather than misplacing the frame's end)
    size_t meta = metadata.empty() ? 0 : metadata.size() + (metadataNul ? 1 : 0);
    if (type != Metadata && meta > 65535) meta = 0;
    out->resize(size_t(kHeaderBytes) + size_t(extBytes) + dataBytes + meta);
    Header h;
    h.type = type;
    h.timestamp = timestamp;
    h.metadataLength = type == Metadata ? 0 : uint16_t(meta);
    h.dataLength = int32_t(size_t(extBytes) + dataBytes + meta);
    writeHeader(h, out->data());
    uint8_t* p = out->data() + kHeaderBytes;
    if (extBytes) std::memcpy(p, ext, size_t(extBytes)), p += extBytes;
    if (dataBytes) std::memcpy(p, data, dataBytes), p += dataBytes;
    if (meta) std::memcpy(p, metadata.data(), metadata.size());  // (the NUL is the vector's zero)
    return out;
}

Bytes metadataFrame(const std::string& xml, int64_t timestamp = 0) {
    return buildFrame(Metadata, timestamp, nullptr, 0, reinterpret_cast<const uint8_t*>(xml.data()), xml.size(), {}, false);
}

// What a metadata frame's data says, its trailing NULs taken off.
std::string commandText(const std::vector<uint8_t>& data) {
    size_t n = data.size();
    while (n > 0 && data[n - 1] == 0) --n;
    return std::string(reinterpret_cast<const char*>(data.data()), n);
}

std::string escapeXml(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += c;
        }
    }
    return out;
}

// Reads one frame (its header and everything after it); false when the connection ended or sent nonsense.
bool readFrame(net::Socket s, Header& h, std::vector<uint8_t>& body, const std::atomic<bool>* stop) {
    uint8_t head[kHeaderBytes];
    if (!net::recvAll(s, head, kHeaderBytes, stop)) return false;
    if (!readHeader(head, h)) return false;
    // (OMT's own receivers take 10 MB pictures at most; 16-bit 8K ones from Montage can be larger)
    const int32_t limit = h.type == Video ? (64 << 20) : (4 << 20);
    if (h.dataLength > limit) return false;
    body.resize(size_t(h.dataLength));
    return h.dataLength == 0 || net::recvAll(s, body.data(), body.size(), stop);
}

}  // namespace

struct Sender::Channel {
    net::Socket sock = net::kInvalid;
    std::atomic<bool> video{false}, audio{false}, metadata{false}, preview{false};
    std::atomic<int> quality{0};
    std::atomic<bool> tallyPreview{false}, tallyProgram{false};
    std::atomic<bool> dead{false}, readerDone{false}, writerDone{false};
    std::mutex m;
    std::condition_variable cv;
    struct Item {
        Bytes bytes;
        bool av = false;
    };
    std::deque<Item> queue;
    int av = 0, meta = 0;
    bool closing = false;
    std::thread reader, writer;
};

struct Sender::State {
    Options o;
    net::Socket listener = net::kInvalid;
    int port = 0;
    std::string full;
    std::atomic<bool> running{false};
    std::thread acceptor;
    mutable std::mutex m;
    std::vector<std::shared_ptr<Channel>> channels;
    bool tallyPreview = false, tallyProgram = false;
    std::string info;
    std::mutex encodeM;
    VmxEncoder encoder;
    std::vector<uint8_t> vmx;
    std::unique_ptr<Announcement> announcement;
    std::string announced;
    std::atomic<int64_t> dropped{0};

    // Queues a frame for a channel: sound and pictures four deep (then dropped), metadata 64 deep.
    void enqueue(Channel& c, const Bytes& b, bool av) {
        std::lock_guard lock(c.m);
        if (c.closing) return;
        if (av ? c.av >= 4 : c.meta >= 64) {
            ++dropped;
            return;
        }
        (av ? c.av : c.meta)++;
        c.queue.push_back({b, av});
        c.cv.notify_one();
    }

    void broadcastTally() {
        bool preview = false, program = false;
        {
            std::lock_guard lock(m);
            for (const auto& c : channels) {
                if (c->dead) continue;
                preview = preview || c->tallyPreview;
                program = program || c->tallyProgram;
            }
            if (preview == tallyPreview && program == tallyProgram) return;
            tallyPreview = preview, tallyProgram = program;
            // (queued under the lock, so two readers' changes reach every receiver in the order they were made)
            const Bytes b = metadataFrame(tallyCommand(preview, program));
            for (const auto& c : channels)
                if (c->metadata && !c->dead) enqueue(*c, b, false);
        }
    }

    void command(Channel& c, const std::string& text) {
        if (text == subscribeCommand(Video)) c.video = true;
        else if (text == subscribeCommand(Audio)) c.audio = true;
        else if (text == subscribeCommand(Metadata)) c.metadata = true;
        else if (text == previewCommand(true)) c.preview = true;
        else if (text == previewCommand(false)) c.preview = false;
        else if (text.rfind("<OMTSettings Quality=", 0) == 0) {
            const std::string q = xmlAttribute(text, "Quality");
            c.quality = q == "Low" ? int(Quality::Low) : q == "Medium" ? int(Quality::Medium) : q == "High" ? int(Quality::High) : int(Quality::Default);
        } else if (text.rfind("<OMTTally ", 0) == 0) {
            for (bool pv : {false, true})
                for (bool pg : {false, true})
                    if (text == tallyCommand(pv, pg)) c.tallyPreview = pv, c.tallyProgram = pg;
            broadcastTally();
        }
    }

    void startChannel(net::Socket sock) {
        auto c = std::make_shared<Channel>();
        c->sock = sock;
        // Its greeting: the sender's information and the tally (before it subscribes to anything).
        if (!info.empty()) enqueue(*c, metadataFrame(info), false);
        {
            std::lock_guard lock(m);
            enqueue(*c, metadataFrame(tallyCommand(tallyPreview, tallyProgram)), false);
            channels.push_back(c);
        }
        Channel* ch = c.get();
        c->reader = std::thread([this, ch] {
            Header h;
            std::vector<uint8_t> body;
            while (running && !ch->dead && readFrame(ch->sock, h, body, &ch->dead))
                if (h.type == Metadata) command(*ch, commandText(body));
            ch->dead = true;
            net::shutdownSocket(ch->sock);
            ch->cv.notify_all();
            ch->readerDone = true;
        });
        c->writer = std::thread([ch] {
            for (;;) {
                Channel::Item item;
                {
                    std::unique_lock lock(ch->m);
                    ch->cv.wait(lock, [&] { return ch->closing || ch->dead || !ch->queue.empty(); });
                    if (ch->closing || ch->dead) break;
                    item = std::move(ch->queue.front());
                    ch->queue.pop_front();
                }
                const bool ok = net::sendAll(ch->sock, item.bytes->data(), item.bytes->size());
                {
                    std::lock_guard lock(ch->m);
                    (item.av ? ch->av : ch->meta)--;
                }
                if (!ok) {
                    ch->dead = true;
                    net::shutdownSocket(ch->sock);
                    break;
                }
            }
            ch->writerDone = true;
        });
    }

    static void finish(Channel& c) {
        {
            std::lock_guard lock(c.m);
            c.closing = true;
        }
        c.dead = true;
        c.cv.notify_all();
        net::shutdownSocket(c.sock);
        if (c.reader.joinable()) c.reader.join();
        if (c.writer.joinable()) c.writer.join();
        net::closeSocket(c.sock);
        c.sock = net::kInvalid;
    }

    // Channels whose connection ended, let go (and the tally taken again without them).
    void reap() {
        std::vector<std::shared_ptr<Channel>> gone;
        {
            std::lock_guard lock(m);
            for (auto it = channels.begin(); it != channels.end();)
                if ((*it)->dead && (*it)->readerDone) {
                    gone.push_back(*it);
                    it = channels.erase(it);
                } else {
                    ++it;
                }
        }
        for (const auto& c : gone) finish(*c);
        if (!gone.empty()) broadcastTally();
    }

    std::vector<std::shared_ptr<Channel>> snapshot() const {
        std::lock_guard lock(m);
        std::vector<std::shared_ptr<Channel>> out;
        for (const auto& c : channels)
            if (!c->dead) out.push_back(c);
        return out;
    }
};

Sender::Sender() = default;
Sender::~Sender() { stop(); }

bool Sender::start(const Options& o, std::string* error) {
    stop();
    std::string why;
    if (!vmxAvailable(&why)) {
        if (error) *error = why;
        return false;
    }
    auto s = std::make_shared<State>();
    s->o = o;
    s->listener = net::listenOnFirstFree(o.portStart, o.portEnd, &s->port, &why);
    if (s->listener == net::kInvalid) {
        if (error) *error = why;
        return false;
    }
    s->full = omt::fullName(o.name.empty() ? "Montage" : o.name);
    s->info = "<OMTInfo ProductName=\"" + escapeXml(o.product) + "\" Manufacturer=\"" + escapeXml(o.manufacturer) + "\" Version=\"" +
              escapeXml(o.version) + "\" />";
    s->running = true;
    State* st = s.get();
    s->acceptor = std::thread([st] {
        while (st->running) {
            const net::Socket c = net::acceptWithin(st->listener, 200);
            if (c != net::kInvalid) {
                net::configure(c, false);
                st->startChannel(c);
            }
            st->reap();
        }
    });
    if (o.announce) {
        s->announcement = announce(s->full, s->port, &why);
        s->announced = s->announcement ? s->announcement->how() : why;
    }
    s_ = s;
    return true;
}

void Sender::stop() {
    if (!s_) return;
    auto s = s_;
    s_.reset();
    s->announcement.reset();
    s->running = false;
    if (s->acceptor.joinable()) s->acceptor.join();
    std::vector<std::shared_ptr<Channel>> all;
    {
        std::lock_guard lock(s->m);
        all.swap(s->channels);
    }
    for (const auto& c : all) State::finish(*c);
    net::closeSocket(s->listener);
}

bool Sender::running() const { return s_ && s_->running; }
int Sender::port() const { return s_ ? s_->port : 0; }
std::string Sender::fullName() const { return s_ ? s_->full : std::string(); }
std::string Sender::announcement() const { return s_ ? s_->announced : std::string(); }
int64_t Sender::framesDropped() const { return s_ ? s_->dropped.load() : 0; }

int Sender::connections() const {
    if (!s_) return 0;
    int n = 0;
    for (const auto& c : s_->snapshot()) n += (c->video || c->audio) ? 1 : 0;
    return n;
}

bool Sender::tallyProgram() const {
    if (!s_) return false;
    std::lock_guard lock(s_->m);
    return s_->tallyProgram;
}

bool Sender::tallyPreview() const {
    if (!s_) return false;
    std::lock_guard lock(s_->m);
    return s_->tallyPreview;
}

bool Sender::sendVideo(const VideoOut& v) {
    const auto s = s_;
    if (!s || !v.data) return false;
    VideoHeader size;
    size.width = v.width, size.height = v.height;
    if (!plausible(size)) return false;
    std::vector<std::shared_ptr<Channel>> to;
    int quality = 0;
    for (const auto& c : s->snapshot())
        if (c->video) {
            to.push_back(c);
            quality = std::max(quality, c->quality.load());
        }
    if (to.empty()) return true;  // (nobody watching: nothing encoded)
    std::lock_guard lock(s->encodeM);
    const double fps = v.rateD > 0 ? double(v.rateN) / v.rateD : 0;
    const int profile = vmxProfile(Quality(quality));
    if (!s->encoder.isOpen() || s->encoder.profile() != profile || v.width != s->encoder.width() || v.height != s->encoder.height())
        if (!s->encoder.open(v.width, v.height, profile, v.colorSpace, fps)) return false;
    s->vmx.clear();
    const int stride = v.stride > 0 ? v.stride : v.width * 2;
    if (!s->encoder.encode(v.format, v.data, stride, v.interlaced, s->vmx)) return false;
    VideoHeader vh;
    vh.width = v.width;
    vh.height = v.height;
    vh.rateN = v.rateN;
    vh.rateD = v.rateD;
    vh.aspect = float(v.width) / float(v.height);
    vh.flags = (v.interlaced ? Interlaced : 0) | (v.format == PictureFormat::P216 ? HighBitDepth : 0);
    vh.colorSpace = v.colorSpace;
    uint8_t ext[kVideoHeaderBytes];
    writeVideoHeader(vh, ext);
    const Bytes full = buildFrame(Video, v.timestamp, ext, kVideoHeaderBytes, s->vmx.data(), s->vmx.size(), v.metadata, true);
    Bytes preview;
    for (const auto& c : to) {
        if (c->preview && !preview) {
            // A preview: the start of the picture that makes an eighth-size one (with the metadata after it, whole).
            VideoHeader ph = vh;
            ph.flags |= Preview;
            uint8_t pext[kVideoHeaderBytes];
            writeVideoHeader(ph, pext);
            const size_t len = std::min(s->vmx.size(), size_t(std::max(0, s->encoder.previewLength())));
            preview = buildFrame(Video, v.timestamp, pext, kVideoHeaderBytes, s->vmx.data(), len, v.metadata, true);
        }
        s->enqueue(*c, c->preview ? preview : full, true);
    }
    return true;
}

bool Sender::sendAudio(const float* planar, int channels, int samples, int stride, int rate, int64_t timestamp) {
    const auto s = s_;
    if (!s || channels <= 0 || channels > 32 || samples <= 0) return false;
    std::vector<std::shared_ptr<Channel>> to;
    for (const auto& c : s->snapshot())
        if (c->audio) to.push_back(c);
    if (to.empty()) return true;
    std::vector<float> packed(size_t(channels) * size_t(samples));
    for (int c = 0; c < channels; ++c)
        std::copy(planar + size_t(c) * size_t(stride), planar + size_t(c) * size_t(stride) + size_t(samples), packed.begin() + ptrdiff_t(c) * samples);
    AudioHeader ah;
    ah.sampleRate = rate;
    ah.samplesPerChannel = samples;
    ah.channels = channels;
    const std::vector<uint8_t> data = encodeFpa1(packed.data(), channels, samples, &ah.activeChannels);
    uint8_t ext[kAudioHeaderBytes];
    writeAudioHeader(ah, ext);
    const Bytes b = buildFrame(Audio, timestamp, ext, kAudioHeaderBytes, data.data(), data.size(), {}, false);
    for (const auto& c : to) s->enqueue(*c, b, true);
    return true;
}

bool Sender::sendMetadata(const std::string& xml, int64_t timestamp) {
    const auto s = s_;
    if (!s || xml.empty()) return false;
    const Bytes b = metadataFrame(xml, timestamp);
    for (const auto& c : s->snapshot())
        if (c->metadata) s->enqueue(*c, b, false);
    return true;
}

// ---------------------------------------------------------------------------
// Receiving

struct Receiver::State {
    Options o;
    std::string host;
    int port = 0;
    std::atomic<bool> closing{false};
    struct Conn {
        bool video = false;
        net::Socket sock = net::kInvalid;
        std::mutex sendM;
        std::atomic<bool> up{false};
        std::thread thread;
    };
    Conn conns[2];
    int connCount = 0;
    mutable std::mutex m;
    std::condition_variable cv;
    std::deque<Frame> queue;
    std::string info;
    bool tallyPreview = false, tallyProgram = false;
    std::atomic<int64_t> dropped{0};

    bool send(Conn& c, const std::string& command) {
        const Bytes b = metadataFrame(command);
        std::lock_guard lock(c.sendM);
        return c.sock != net::kInvalid && net::sendAll(c.sock, b->data(), b->size());
    }

    // The commands that start a connection: as libomtnet's receivers send them.
    bool greet(Conn& c) {
        std::vector<std::string> commands;
        if (c.video) {
            commands.push_back(subscribeCommand(Metadata));
            if (o.preview) commands.push_back(previewCommand(true));
            commands.push_back(subscribeCommand(Video));
            commands.push_back(qualityCommand(o.quality));
            std::lock_guard lock(m);
            commands.push_back(tallyCommand(tallyPreview, tallyProgram));
        } else {
            if (!o.video && o.metadata) commands.push_back(subscribeCommand(Metadata));
            if (o.audio) commands.push_back(subscribeCommand(Audio));
        }
        for (const std::string& cmd : commands)
            if (!send(c, cmd)) return false;
        return true;
    }

    void push(Frame&& f) {
        std::lock_guard lock(m);
        if (int(queue.size()) >= std::max(1, o.queue)) {
            ++dropped;
            return;
        }
        queue.push_back(std::move(f));
        cv.notify_one();
    }

    void run(Conn& c) {
        while (!closing) {
            std::string why;
            const net::Socket sock = net::connectTo(host, port, 2000, &why, &closing);
            if (sock == net::kInvalid) {
                for (int i = 0; i < 10 && !closing; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            net::configure(sock, true);
            {
                std::lock_guard lock(c.sendM);
                c.sock = sock;
            }
            if (greet(c)) {
                c.up = true;
                {
                    std::lock_guard lock(m);
                    cv.notify_all();
                }
                Header h;
                std::vector<uint8_t> body;
                while (!closing && readFrame(sock, h, body, &closing)) {
                    Frame f;
                    f.header = h;
                    if (h.type == Metadata) {
                        const std::string text = commandText(body);
                        if (text.rfind("<OMTInfo", 0) == 0) {
                            std::lock_guard lock(m);
                            info = text;
                        } else if (text.rfind("<OMTTally ", 0) != 0 && o.metadata) {
                            f.metadata = text;
                            push(std::move(f));
                        }
                        continue;
                    }
                    const int ext = h.type == Video ? kVideoHeaderBytes : kAudioHeaderBytes;
                    if (int(body.size()) < ext + int(h.metadataLength)) continue;
                    if (h.type == Video) f.video = readVideoHeader(body.data());
                    else f.audio = readAudioHeader(body.data());
                    const size_t end = body.size() - h.metadataLength;
                    // (a frame that makes no sense is passed over, never decoded)
                    if (h.type == Video ? !plausible(f.video) : !plausible(f.audio, end - size_t(ext))) continue;
                    f.data.assign(body.begin() + ext, body.begin() + ptrdiff_t(end));
                    if (h.metadataLength) f.metadata = commandText(std::vector<uint8_t>(body.begin() + ptrdiff_t(end), body.end()));
                    push(std::move(f));
                }
            }
            c.up = false;
            {
                std::lock_guard lock(c.sendM);
                c.sock = net::kInvalid;
            }
            net::shutdownSocket(sock);
            net::closeSocket(sock);
            {
                std::lock_guard lock(m);
                cv.notify_all();
            }
            // (made again each second, until closed)
            for (int i = 0; i < 10 && !closing; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
};

Receiver::Receiver() = default;
Receiver::~Receiver() { close(); }

bool Receiver::open(const std::string& source, const Options& o, std::string* error) {
    close();
    auto s = std::make_shared<State>();
    s->o = o;
    if (!resolve(source, s->host, s->port, error)) return false;
    if (o.video) s->conns[s->connCount++].video = true;
    if (o.audio || (!o.video && o.metadata)) s->conns[s->connCount++].video = false;
    if (!s->connCount) {
        if (error) *error = "Nothing to receive";
        return false;
    }
    State* st = s.get();
    for (int i = 0; i < s->connCount; ++i) s->conns[i].thread = std::thread([st, i] { st->run(st->conns[i]); });
    s_ = s;
    // Connected once every connection is up, or given up after the timeout.
    std::unique_lock lock(s->m);
    s->cv.wait_for(lock, std::chrono::milliseconds(int64_t(o.connectTimeout * 1000)), [&] {
        for (int i = 0; i < s->connCount; ++i)
            if (!s->conns[i].up) return false;
        return true;
    });
    bool any = false;
    for (int i = 0; i < s->connCount; ++i) any = any || s->conns[i].up;
    lock.unlock();
    if (!any) {
        if (error) *error = "Cannot connect to " + source + " (" + s->host + ":" + std::to_string(s->port) + ")";
        close();
        return false;
    }
    return true;
}

void Receiver::close() {
    if (!s_) return;
    auto s = s_;
    s_.reset();
    s->closing = true;
    for (int i = 0; i < s->connCount; ++i) {
        {
            std::lock_guard lock(s->conns[i].sendM);
            net::shutdownSocket(s->conns[i].sock);
        }
        if (s->conns[i].thread.joinable()) s->conns[i].thread.join();
    }
}

bool Receiver::next(Frame& out, int ms) {
    const auto s = s_;
    if (!s) return false;
    std::unique_lock lock(s->m);
    s->cv.wait_for(lock, std::chrono::milliseconds(ms), [&] { return !s->queue.empty() || s->closing; });
    if (s->queue.empty()) return false;
    out = std::move(s->queue.front());
    s->queue.pop_front();
    return true;
}

bool Receiver::connected() const {
    if (!s_) return false;
    for (int i = 0; i < s_->connCount; ++i)
        if (s_->conns[i].up) return true;
    return false;
}

std::string Receiver::address() const { return s_ ? s_->host + ":" + std::to_string(s_->port) : std::string(); }

std::string Receiver::senderInfo() const {
    if (!s_) return {};
    std::lock_guard lock(s_->m);
    return s_->info;
}

void Receiver::setTally(bool preview, bool program) {
    const auto s = s_;
    if (!s) return;
    {
        std::lock_guard lock(s->m);
        s->tallyPreview = preview, s->tallyProgram = program;
    }
    for (int i = 0; i < s->connCount; ++i)
        if (s->conns[i].video && s->conns[i].up) s->send(s->conns[i], tallyCommand(preview, program));
}

int64_t Receiver::framesDropped() const { return s_ ? s_->dropped.load() : 0; }

}  // namespace montage::omt
