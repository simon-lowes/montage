#include "St2110Receive.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace montage::st2110 {

namespace {

uint32_t get16(const uint8_t* p) { return uint32_t(p[0]) << 8 | p[1]; }

// a * b / c, rounded down, without overflowing where a * b would (a >= 0; (c - 1) * b fits).
int64_t mulDiv(int64_t a, int64_t b, int64_t c) { return a / c * b + a % c * b / c; }

std::string trim(const std::string& t) {
    const auto a = t.find_first_not_of(" \t\r"), b = t.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : t.substr(a, b - a + 1);
}

bool rational(const std::string& text, Rational& out) {
    const auto slash = text.find('/');
    char* end = nullptr;
    const long n = std::strtol(text.c_str(), &end, 10);
    long d = 1;
    if (slash != std::string::npos) d = std::strtol(text.c_str() + slash + 1, nullptr, 10);
    if (n <= 0 || d <= 0 || n > 1000000 || d > 1000000) return false;
    out = {int(n), int(d)};
    return true;
}

// A media description as an SDP gives it.
struct Media {
    SdpStream stream;
    std::string mid, address, source;
    int port = 0;
    std::map<std::string, std::string> fmtp;
    std::string rtpmap;
    double ptimeMs = 1;
};

// An RTP timestamp at `rate` as a whole count of its ticks since the PTP epoch, taken to be the one nearest now.
int64_t unwrap(uint32_t ts, int64_t rate) {
    const int64_t now = mulDiv(ptpNowNs(), rate, 1000000000);
    return now + int32_t(ts - uint32_t(now));
}

}  // namespace

std::string sdpText(const SdpStream& s) {
    SdpOptions o;
    o.source = s.source[0];
    o.source2 = s.source[1];
    const Endpoint* to2 = s.redundant() ? &s.path[1] : nullptr;
    if (s.media == "audio") return audioSdp(s.audio, s.path[0], s.payloadType, o, to2);
    if (s.encoding == "ST2110-41") return metadataSdp(s.path[0], s.payloadType, o, s.metadataTypes, to2);
    if (s.encoding == "smpte291") return ancSdp(s.video, s.path[0], s.payloadType, o, {{0x60, 0x60}, {0x61, 0x01}}, to2);
    std::string text = videoSdp(s.video, s.path[0], s.payloadType, o, to2);
    if (s.fullRange) {
        const size_t at = text.find("RANGE=NARROW");
        if (at != std::string::npos) text.replace(at, 12, "RANGE=FULL");
    }
    return text;
}

std::vector<SdpStream> parseSdp(const std::string& sdp, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return std::vector<SdpStream>{};
    };
    std::vector<Media> media;
    std::vector<std::vector<std::string>> dup;
    // (a session-level connection and source filter: every description's that gives none of its own)
    std::string sessionAddress, sessionSource;
    std::istringstream in(sdp);
    for (std::string line; std::getline(in, line);) {
        line = trim(line);
        if (line.size() < 2 || line[1] != '=') continue;
        const std::string v = line.substr(2);
        Media* m = media.empty() ? nullptr : &media.back();
        // (the sender's address is taken only from a source filter: the origin's may be another computer's)
        if (line[0] == 'm') {
            Media n;
            std::istringstream ms(v);
            std::string port, proto, type;
            ms >> n.stream.media >> port >> proto >> type;
            n.port = std::atoi(port.c_str());
            // (its first format, a payload type 0 to 127; -2 when there is none, refused below)
            char* end = nullptr;
            const long pt = std::strtol(type.c_str(), &end, 10);
            n.stream.payloadType = !type.empty() && end && *end == 0 && pt >= 0 && pt <= 127 ? int(pt) : -2;
            n.address = sessionAddress;
            n.source = sessionSource;
            media.push_back(n);
        } else if (line[0] == 'c') {
            std::istringstream cs(v);
            std::string net, type, addr;
            cs >> net >> type >> addr;
            (m ? m->address : sessionAddress) = addr.substr(0, addr.find('/'));
        } else if (line[0] == 'a') {
            const auto colon = v.find(':');
            const std::string name = v.substr(0, colon), value = colon == std::string::npos ? std::string() : trim(v.substr(colon + 1));
            if (name == "group" && value.rfind("DUP", 0) == 0) {
                std::istringstream g(value.substr(3));
                std::vector<std::string> ids;
                for (std::string id; g >> id;) ids.push_back(id);
                dup.push_back(ids);
            } else if (!m) {
                if (name == "source-filter") {
                    std::istringstream f(value);
                    std::vector<std::string> words;
                    for (std::string w; f >> w;) words.push_back(w);
                    if (words.size() >= 5 && words[0] == "incl") sessionSource = words[4];
                }
                continue;
            } else if (name == "mid") {
                m->mid = value;
            } else if (name == "rtpmap") {
                m->rtpmap = trim(value.substr(value.find(' ') + 1));
            } else if (name == "ptime") {
                m->ptimeMs = std::atof(value.c_str());
            } else if (name == "source-filter") {
                // ("incl IN IP4 <destination> <source>")
                std::istringstream f(value);
                std::string w;
                std::vector<std::string> words;
                while (f >> w) words.push_back(w);
                if (words.size() >= 5 && words[0] == "incl") m->source = words[4];
            } else if (name == "fmtp") {
                std::istringstream params(value.substr(value.find(' ') + 1));
                for (std::string p; std::getline(params, p, ';');) {
                    p = trim(p);
                    if (p.empty()) continue;
                    const auto eq = p.find('=');
                    m->fmtp[eq == std::string::npos ? p : p.substr(0, eq)] = eq == std::string::npos ? std::string() : p.substr(eq + 1);
                }
            }
        }
    }
    if (media.empty()) return fail("The SDP describes no streams");
    // What each carries.
    for (Media& m : media) {
        SdpStream& s = m.stream;
        s.path[0] = {m.address, m.port};
        s.source[0] = m.source;
        const auto slash = m.rtpmap.find('/');
        s.encoding = m.rtpmap.substr(0, slash);
        if (m.address.empty() || m.port <= 0 || m.port > 65535) return fail("A stream in the SDP has no address and port");
        if (s.payloadType < 0) return fail("A stream in the SDP has no RTP payload type");
        if (s.encoding == "raw") {
            VideoFormat& f = s.video;
            if (!parseSampling(m.fmtp["sampling"], f.sampling)) return fail("Montage receives YCbCr-4:2:2, YCbCr-4:4:4 and RGB, not " + m.fmtp["sampling"]);
            f.width = std::atoi(m.fmtp["width"].c_str());
            f.height = std::atoi(m.fmtp["height"].c_str());
            f.depth = std::atoi(m.fmtp["depth"].c_str());
            f.interlaced = m.fmtp.count("interlace") > 0;
            if (!rational(m.fmtp["exactframerate"], f.fps)) return fail("The video's SDP gives no exactframerate");
            if (m.fmtp.count("TCS")) f.tcs = m.fmtp["TCS"];
            if (m.fmtp.count("colorimetry")) f.colorimetry = m.fmtp["colorimetry"];
            s.fullRange = m.fmtp["RANGE"].rfind("FULL", 0) == 0;
            std::string why;
            if (!videoFormatOk(f, &why)) return fail(why);
        } else if (s.encoding == "smpte291") {
            if (m.fmtp.count("exactframerate")) rational(m.fmtp["exactframerate"], s.video.fps);
        } else if (s.encoding == "ST2110-41") {
            // (DIT: the data item types, apart by commas)
            std::istringstream types(m.fmtp["DIT"]);
            for (std::string t; std::getline(types, t, ',');) {
                t = trim(t);
                if (t.empty()) continue;
                char* end = nullptr;
                const unsigned long v = std::strtoul(t.c_str(), &end, 0);
                if (end && *end == 0 && v <= 0x3FFFFF) s.metadataTypes.push_back(uint32_t(v));
            }
        } else if (s.encoding == "L24" || s.encoding == "L16" || s.encoding == "AM824") {
            AudioFormat& a = s.audio;
            a.bits = s.encoding == "L16" ? 16 : 24;
            a.aes3 = s.encoding == "AM824";
            std::istringstream r(m.rtpmap.substr(slash + 1));
            std::string rate, ch;
            std::getline(r, rate, '/');
            std::getline(r, ch, '/');
            a.rate = std::atoi(rate.c_str());
            a.channels = ch.empty() ? 1 : std::atoi(ch.c_str());
            a.packetUs = int(std::lround(m.ptimeMs * 1000));
            if (a.rate <= 0 || a.channels < 1 || a.channels > 64 || a.packetUs <= 0) return fail("The audio's SDP gives no rate, channels or packet time");
            if (a.rate != 44100 && a.rate != 48000 && a.rate != 96000) return fail("ST 2110-30 here is at 44.1, 48 or 96 kHz");
            if (a.packetUs > 20000) return fail("The audio's packets are at most 20 ms here");
        } else {
            return fail("Montage receives ST 2110-20, -30, -31, -40 and -41 streams, not " + m.rtpmap);
        }
    }
    // ST 2022-7: a group's second description is its first's other path.
    std::vector<SdpStream> out;
    std::vector<bool> used(media.size());
    for (const auto& ids : dup) {
        int first = -1, second = -1;
        for (size_t i = 0; i < media.size(); ++i) {
            if (ids.size() > 0 && media[i].mid == ids[0]) first = int(i);
            if (ids.size() > 1 && media[i].mid == ids[1]) second = int(i);
        }
        if (first < 0 || second < 0) continue;
        SdpStream s = media[size_t(first)].stream;
        s.path[1] = media[size_t(second)].stream.path[0];
        s.source[1] = media[size_t(second)].stream.source[0];
        used[size_t(first)] = used[size_t(second)] = true;
        out.push_back(s);
    }
    for (size_t i = 0; i < media.size(); ++i)
        if (!used[i]) out.push_back(media[i].stream);
    return out;
}

bool frameTimecode(const std::vector<AncPacket>& anc, Rational fps, output::Timecode& out) {
    const double rate = fps.toDouble();
    const bool pairs = rate > 30.5;
    const int r = int(std::lround(pairs ? rate / 2 : rate));
    const int count = r == 25 ? 25 : r == 24 ? 24 : 30;
    bool have = false;
    for (const AncPacket& p : anc) {
        output::Timecode tc;
        AtcType type;
        bool mark = false;
        if (!parseAtc(p, tc, &type, &mark, count)) continue;
        if (pairs) {
            // (a frame pair's time address: the second frame's VITC2 carries the field mark)
            if (type == AtcType::Ltc) continue;
            tc.frames = tc.frames * 2 + (type == AtcType::Vitc2 && mark ? 1 : 0);
            out = tc;
            return true;
        }
        if (type == AtcType::Ltc) {
            out = tc;
            return true;
        }
        if (!have) out = tc, have = true;
    }
    return have;
}

// ---- ST 2022-7.

namespace {
// (a held packet's own bytes beside its buffer: the map's node and the vector)
constexpr size_t kHeldOverhead = 64;
}  // namespace

void PathMerger::push(std::vector<uint8_t> packet, int path, std::vector<std::vector<uint8_t>>& out) {
    // Checked before it takes a place: a stray or forged datagram must not stand in for the packet of its number.
    RtpPacket p;
    if (!parseRtp(packet.data(), packet.size(), p) || (payloadType_ >= 0 && p.payloadType != payloadType_)) {
        ++rejected;
        return;
    }
    // (RFC 4175's extended sequence number: the payload's first two bytes, after any header extension, above the
    // header's; placed by the count so far)
    const uint32_t low = p.sequence;
    const uint32_t seq = extended_ && p.size >= 2 ? get16(p.payload) << 16 | low : low;
    auto place = [&] { return extended_ ? next_ + int32_t(seq - uint32_t(next_)) : next_ + int16_t(uint16_t(seq - uint16_t(next_))); };
    const int side = path & 1, otherSide = side ^ 1;
    Sender& who = sender_[side];
    Sender& other = sender_[otherSide];
    auto turnAway = [&] {
        ++rejected;
        if (who.others.size() > 8) {
            // (a few strangers counted at a time: the one heard longest ago let go, never this one, so a sender that
            // keeps sending builds up its count however many strays trickle in beside it)
            auto least = who.others.end();
            for (auto it = who.others.begin(); it != who.others.end(); ++it)
                if (it->first != p.ssrc && (least == who.others.end() || it->second.last < least->second.last)) least = it;
            if (least != who.others.end()) who.others.erase(least);
        }
    };
    if (!who.have && started_ && other.have && p.ssrc != other.ssrc && std::llabs(place() - next_) > int64_t(window_)) {
        // A path's first sender is the other path's, or one of its own (ST 2022-7's legs may have SSRCs of their own)
        // numbered as the stream is, not a stray far off; unless the stray is the other path's (heard first on a leg
        // that is otherwise dead): this one is taken once it has sent a window with fewer than a quarter as many of
        // that one's between, the count started again from its numbers and the other path let go to find its own.
        // (each counted against the other path's packets since it was first heard: a stray heard long before on this
        // leg does not hold back a sender that starts on it once the other leg has died)
        auto heard = who.others.find(p.ssrc);
        if (heard == who.others.end()) heard = who.others.emplace(p.ssrc, Heard{0, received[otherSide], 0}).first;
        heard->second.last = ++heardClock_;
        const int64_t theirs = ++heard->second.count;
        if (theirs <= int64_t(window_) || (received[otherSide] - heard->second.otherAt) * 4 >= theirs) return turnAway();
        other = Sender{};
        flush(out);
        started_ = false;
        lateRun_ = 0;
        restartedBy_ = side;
        sinceRestart_ = 0;
    }
    if (who.have && p.ssrc != who.ssrc) {
        // (the one taken's packets counted from the first stranger's; all counted again once it has sent a window)
        if (who.others.empty()) who.own = 0;
        Heard& h = who.others[p.ssrc];
        h.last = ++heardClock_;
        const int64_t theirs = ++h.count;
        if (theirs <= int64_t(window_) || who.own * 4 >= theirs) return turnAway();
        // A window of another sender's, with few of the one taken between: that one now. The count starts again from
        // its numbers, unless the other path's new sender has just started it (both legs' senders started again).
        who.have = false;
        if (restartedBy_ != otherSide || sinceRestart_ > 4 * int64_t(window_)) {
            flush(out);
            started_ = false;
            lateRun_ = 0;
            restartedBy_ = side;
            sinceRestart_ = 0;
        }
    }
    if (!who.have) who = Sender{true, p.ssrc, 0, {}};
    if (++who.own >= int64_t(window_)) who.own = 0, who.others.clear();
    ++sinceRestart_;
    ++received[path & 1];
    if (!started_) {
        started_ = true;
        next_ = seq;
    }
    const int64_t n = place();
    if (n < next_) {
        // (the other path's copy of what was given out, or one too late to place)
        if (n >= next_ - int64_t(window_)) {
            ++duplicates;
        } else {
            ++late;
            // A window's worth in a row from far before: the sender started again with other numbers (or a stray
            // packet far ahead was jumped to), so the count starts again from these.
            if (++lateRun_ > int64_t(window_)) {
                std::vector<std::vector<uint8_t>> rest;
                flush(rest);
                for (auto& r : rest) out.push_back(std::move(r));
                started_ = false;
                lateRun_ = 0;
                push(std::move(packet), path, out);
                --received[path & 1];
            }
        }
        return;
    }
    lateRun_ = 0;
    const size_t bytes = packet.capacity() + kHeldOverhead;
    if (!held_.emplace(n, std::move(packet)).second) {
        ++duplicates;
        return;
    }
    heldBytes_ += bytes;
    release(out);
    // A gap the other path has not filled in a window's packets (or in as much memory as is held for one): given up on.
    while (!held_.empty() && (held_.size() > window_ || heldBytes_ > maxBytes_)) {
        lost += held_.begin()->first - next_;
        next_ = held_.begin()->first;
        release(out);
    }
}

void PathMerger::release(std::vector<std::vector<uint8_t>>& out) {
    while (!held_.empty() && held_.begin()->first == next_) {
        heldBytes_ -= held_.begin()->second.capacity() + kHeldOverhead;
        out.push_back(std::move(held_.begin()->second));
        held_.erase(held_.begin());
        ++next_;
    }
}

void PathMerger::flush(std::vector<std::vector<uint8_t>>& out) {
    while (!held_.empty()) {
        lost += held_.begin()->first - next_;
        next_ = held_.begin()->first;
        release(out);
    }
}

// ---- The input.

struct Input::Stream {
    int kind = 0;  // 0 video, 1 audio, 2 ancillary data, 3 fast metadata
    int sound = -1;  // (audio: its place in sounds_)
    SdpStream sdp;
    Receiver socket[2];
    std::mutex m;
    PathMerger merger{true};
    std::unique_ptr<VideoAssembler> assembler;
    std::chrono::steady_clock::time_point last[2];  // when each path last brought a packet
    // Ancillary data and metadata: the latest RTP timestamp this stream brought (the input's lock held).
    bool dataSeen = false;
    uint32_t dataLatest = 0;
    Stream(int k, const SdpStream& s) : kind(k), sdp(s), merger(k == 0) {}
};

Input::Input() = default;
Input::~Input() { close(); }

bool Input::open(const std::vector<SdpStream>& streams, const InputOptions& o, std::string* error) {
    close();
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        close();
        return false;
    };
    options_ = o;
    stop_ = false;
    int channels = 0;
    for (const SdpStream& s : streams) {
        const int kind = s.encoding == "raw" ? 0 : s.encoding == "smpte291" ? 2 : s.encoding == "ST2110-41" ? 3 : 1;
        if (kind == 0)
            for (const auto& have : streams_)
                if (have->kind == 0) return fail("Montage takes one video stream at a time (with any number of sound and data streams)");
        auto st = std::make_unique<Stream>(kind, s);
        st->merger.setPayloadType(s.payloadType);
        if (kind == 0) {
            std::string why;
            if (!videoFormatOk(s.video, &why)) return fail(why);
            video_ = s.video;
            st->assembler = std::make_unique<VideoAssembler>(s.video.width, s.video.height, s.video.sampling, s.video.depth, s.video.interlaced);
            // ST 2022-7's paths may be some milliseconds apart: a gap is waited on for 30 ms of the stream's packets
            // (from its bytes a second; packets carry at most about 1,400 bytes of pixels), and fewer large pictures
            // are held for a reader that falls behind.
            const Pgroup pg = pgroup(s.video.sampling, s.video.depth);
            const double frameBytes = double(s.video.width) * s.video.height * pg.bytes / pg.pixels;
            st->merger.setWindow(std::max<size_t>(512, size_t(frameBytes * s.video.fps.toDouble() * 0.03 / 1000)));
            // (each held as a raster of 16-bit samples)
            const double rasterBytes = double(s.video.width) * s.video.height * (s.video.sampling == Sampling::Ycc422 ? 2 : 3) * 2;
            pendingLimit_ = size_t(std::clamp(512e6 / rasterBytes, 2.0, 16.0));
        } else if (kind == 1) {
            if (hasAudio_ && s.audio.rate != audio_.rate) return fail("The sound streams are at different rates");
            if (channels + s.audio.channels > 64) return fail("Montage takes up to 64 channels of sound at a time");
            if (!hasAudio_) audio_ = s.audio;
            hasAudio_ = true;
            aes3_ = aes3_ || s.audio.aes3;
            st->sound = int(sounds_.size());
            Sound snd;
            snd.format = s.audio;
            snd.offset = channels;
            sounds_.push_back(snd);
            channels += s.audio.channels;
            audio_.channels = channels;
        } else if (kind == 2) {
            hasAnc_ = true;
        } else {
            hasMeta_ = true;
        }
        // (a gap waited on, and a new sender passed over, for about half a second of packets: a data stream sends a
        // packet or two a frame, so 512 of them were twenty seconds; sound by its packet time)
        if (kind == 1) st->merger.setWindow(std::clamp<size_t>(size_t(500000 / std::max(1, s.audio.packetUs)), 64, 4096));
        else if (kind >= 2) st->merger.setWindow(32);
        for (int p = 0; p < (s.redundant() ? 2 : 1); ++p) {
            std::string why;
            const std::string& at = s.interfaceAddress[p].empty() ? o.interfaceAddress[p] : s.interfaceAddress[p];
            if (!st->socket[p].open(s.path[p], at, &why, s.source[p])) return fail(why);
        }
        streams_.push_back(std::move(st));
    }
    bool video = false;
    for (const auto& s : streams_) video = video || s->kind == 0;
    if (!video) return fail("The SDP has no video stream (ST 2110-20)");
    for (const auto& s : streams_)
        for (int p = 0; p < (s->sdp.redundant() ? 2 : 1); ++p) threads_.emplace_back([this, st = s.get(), p] { receive(st, p); });
    return true;
}

void Input::close() {
    stop_ = true;
    cv_.notify_all();
    for (std::thread& t : threads_) t.join();
    threads_.clear();
    streams_.clear();
    std::lock_guard lock(m_);
    pending_.clear();
    anc_.clear();
    meta_.clear();
    sounds_.clear();
    hasAudio_ = hasAnc_ = ancSeen_ = joined_ = false;
    hasMeta_ = metaSeen_ = aes3_ = false;
    stats_ = {};
}

std::vector<AudioFormat> Input::audioStreams() const {
    std::vector<AudioFormat> out;
    for (const Sound& s : sounds_) out.push_back(s.format);
    return out;
}

bool Input::isOpen() const { return !streams_.empty(); }

InputStats Input::stats() const {
    InputStats s;
    {
        std::lock_guard lock(m_);
        s = stats_;
    }
    // (a stream's lock is taken before the input's while packets come in, so never the other way round)
    for (const auto& st : streams_) {
        std::lock_guard sl(st->m);
        // (each kind's streams together: several sound or data streams beside the picture)
        s.packets[st->kind][0] += st->merger.received[0];
        s.packets[st->kind][1] += st->merger.received[1];
        s.duplicates += st->merger.duplicates;
        s.lost += st->merger.lost;
    }
    return s;
}

void Input::receive(Stream* s, int path) {
    std::vector<uint8_t> buf;
    std::vector<std::vector<uint8_t>> out, spare;  // (spare: buffers of packets given out, used again)
    while (!stop_) {
        if (buf.capacity() == 0 && !spare.empty()) {
            buf = std::move(spare.back());
            spare.pop_back();
        }
        const bool got = s->socket[path].receive(buf, 20);
        std::lock_guard lock(s->m);
        out.clear();
        const auto now = std::chrono::steady_clock::now();
        if (got) {
            s->last[path] = now;
            s->merger.push(std::move(buf), path, out);
        } else if (!s->sdp.redundant() || now - s->last[path ^ 1] > std::chrono::milliseconds(20)) {
            s->merger.flush(out);  // (a pause on every path: nothing more is coming for a gap)
        }
        for (auto& p : out) {
            take(*s, p);
            if (spare.size() < 256) spare.push_back(std::move(p));
        }
    }
}

void Input::take(Stream& s, const std::vector<uint8_t>& packet) {
    RtpPacket p;
    if (!parseRtp(packet.data(), packet.size(), p)) return;  // (the merger took RTP of the stream's payload type alone)
    if (s.kind == 0) {
        if (!s.assembler->add(p)) return;
        std::lock_guard lock(m_);
        // (a reader that falls behind loses the oldest frames, not the newest)
        while (pending_.size() >= pendingLimit_) pending_.pop_front(), ++stats_.dropped;
        pending_.push_back({s.assembler->raster(), s.assembler->timestamp(), s.assembler->lostInFrame(), std::chrono::steady_clock::now()});
        if (s.assembler->lostInFrame()) ++stats_.incomplete;
        cv_.notify_all();
    } else if (s.kind == 1) {
        const AudioFormat& a = s.sdp.audio;
        // (AES3's words kept as they came; their 24 bits as the samples, exactly)
        std::vector<uint32_t> words;
        std::vector<float> samples;
        if (a.aes3) {
            words = unpackAm824(p.payload, p.size, a.channels);
            samples.resize(words.size());
            for (size_t i = 0; i < words.size(); ++i) samples[i] = float(double(am824Data(words[i])) / 8388608.0);
        } else {
            samples = unpackAudio(p.payload, p.size, a.channels, a.bits);
        }
        const int64_t at = unwrap(p.timestamp, a.rate);
        const int64_t frames = int64_t(samples.size()) / a.channels;
        std::lock_guard lock(m_);
        Sound& snd = sounds_[size_t(s.sound)];
        const int64_t held = snd.start + int64_t(snd.samples.size()) / a.channels;
        // (the first; after a long gap; or from well before what is held, so not a packet late by a little but time
        // gone back, a PTP step or the sender started again: from here)
        if (snd.start < 0 || at > held + a.rate || at + frames + a.rate / 50 < held) {
            snd.samples.clear();
            snd.words.clear();
            snd.start = at;
        }
        const int64_t end = snd.start + int64_t(snd.samples.size()) / a.channels;
        if (at + frames <= end) return;  // (already held)
        if (at > end) {
            // (a gap: silence)
            snd.samples.insert(snd.samples.end(), size_t(at - end) * size_t(a.channels), 0.0f);
            if (a.aes3) snd.words.insert(snd.words.end(), size_t(at - end) * size_t(a.channels), 0u);
        }
        const int64_t skip = std::max<int64_t>(0, end - at);
        snd.samples.insert(snd.samples.end(), samples.begin() + ptrdiff_t(skip * a.channels), samples.end());
        if (a.aes3) snd.words.insert(snd.words.end(), words.begin() + ptrdiff_t(skip * a.channels), words.end());
        // (two seconds kept)
        const int64_t keep = int64_t(a.rate) * 2;
        const int64_t extra = int64_t(snd.samples.size()) / a.channels - keep;
        if (extra > 0) {
            snd.samples.erase(snd.samples.begin(), snd.samples.begin() + ptrdiff_t(extra * a.channels));
            if (a.aes3) snd.words.erase(snd.words.begin(), snd.words.begin() + ptrdiff_t(extra * a.channels));
            snd.start += extra;
        }
        cv_.notify_all();
    } else if (s.kind == 3) {
        std::vector<FastMetadata> items;
        if (!unpackFastMetadata(p.payload, p.size, items)) return;
        std::lock_guard lock(m_);
        // (no entry made for what is refused: an empty one would never be let go)
        const auto have = meta_.find(p.timestamp);
        if ((have == meta_.end() ? 0 : have->second.size()) + items.size() > 256) return;
        auto& at = have == meta_.end() ? meta_[p.timestamp] : have->second;
        at.insert(at.end(), items.begin(), items.end());
        if (!metaSeen_ || int32_t(p.timestamp - metaLatest_) > 0) metaLatest_ = p.timestamp;
        metaSeen_ = true;
        if (!s.dataSeen || int32_t(p.timestamp - s.dataLatest) > 0) s.dataLatest = p.timestamp;
        s.dataSeen = true;
        while (meta_.size() > 64) {
            auto oldest = meta_.begin();
            for (auto it = meta_.begin(); it != meta_.end(); ++it)
                if (metaLatest_ - it->first > metaLatest_ - oldest->first) oldest = it;
            meta_.erase(oldest);
        }
        cv_.notify_all();
    } else {
        std::vector<AncPacket> anc;
        if (!unpackAnc(p.payload, p.size, anc)) return;
        std::lock_guard lock(m_);
        // (a frame carries a few dozen packets: no more held for one time, whatever a sender repeats; and no entry
        // made for what is refused)
        const auto have = anc_.find(p.timestamp);
        if ((have == anc_.end() ? 0 : have->second.size()) + anc.size() > 256) return;
        auto& at = have == anc_.end() ? anc_[p.timestamp] : have->second;
        at.insert(at.end(), anc.begin(), anc.end());
        if (!ancSeen_ || int32_t(p.timestamp - ancLatest_) > 0) ancLatest_ = p.timestamp;
        if (!s.dataSeen || int32_t(p.timestamp - s.dataLatest) > 0) s.dataLatest = p.timestamp;
        s.dataSeen = true;
        // (the oldest let go: furthest behind the latest, as the map's order breaks where the timestamps wrap)
        while (anc_.size() > 64) {
            auto oldest = anc_.begin();
            for (auto it = anc_.begin(); it != anc_.end(); ++it)
                if (ancLatest_ - it->first > ancLatest_ - oldest->first) oldest = it;
            anc_.erase(oldest);
        }
        if (!ancSeen_) ancFirst_ = p.timestamp;
        ancSeen_ = true;
        cv_.notify_all();
    }
}

namespace {
struct Span {
    int64_t number, startNs, from, to;  // the frame's count, start, and its samples [from, to)
};
Span spanOf(uint32_t ts, Rational fps, int rate) {
    const int64_t num = fps.num, den = fps.den, second = 1000000000;
    // (the frame count nearest its 90 kHz timestamp; its start and samples as the sender reckons them)
    const int64_t ticks = unwrap(ts, 90000);
    // (wider than 64 bits: ticks since the PTP epoch times a numerator such as 60000 is past 2^63)
    const int64_t n = int64_t((__int128(ticks) * num + 45000 * den) / (90000 * den));
    const int64_t start = mulDiv(n, second * den, num), end = mulDiv(n + 1, second * den, num);
    return {n, start, mulDiv(start, rate, second), mulDiv(end, rate, second)};
}
}  // namespace

bool Input::ready(const Pending& p, std::chrono::steady_clock::time_point now) {
    if (now - p.at > std::chrono::milliseconds(options_.waitMs)) return true;
    // (its ancillary data and metadata, sent alongside on sockets of their own, may come a little after the picture:
    // each such stream must have reached the picture's time, not only the quickest of them)
    for (const auto& st : streams_)
        if ((st->kind == 2 || st->kind == 3) && !(st->dataSeen && int32_t(st->dataLatest - p.ts) >= 0)) return false;
    if (!hasAudio_) return true;
    const Span sp = spanOf(p.ts, video_.fps, audio_.rate);
    for (const Sound& s : sounds_)
        if (s.start < 0 || s.start + int64_t(s.samples.size()) / s.format.channels < sp.to) return false;
    return true;
}

void Input::build(Pending& p, InputFrame& out) {
    const Span sp = spanOf(p.ts, video_.fps, hasAudio_ ? audio_.rate : 48000);
    out.picture = std::move(p.picture);
    out.rtpTimestamp = p.ts;
    out.number = sp.number;
    out.ptpNs = sp.startNs;
    out.lostPackets = p.lost;
    out.channels = hasAudio_ ? audio_.channels : 0;
    out.audio.clear();
    out.aes3.clear();
    if (hasAudio_) {
        const int all = audio_.channels;
        out.audio.assign(size_t(sp.to - sp.from) * size_t(all), 0.0f);
        if (aes3_) out.aes3.assign(out.audio.size(), 0u);
        for (const Sound& snd : sounds_) {
            const int ch = snd.format.channels;
            for (int64_t i = std::max(sp.from, snd.start); snd.start >= 0 && i < sp.to; ++i) {
                const int64_t k = (i - snd.start) * ch;
                if (k + ch > int64_t(snd.samples.size())) break;
                for (int c = 0; c < ch; ++c) {
                    const size_t to = size_t((i - sp.from) * all + snd.offset + c);
                    out.audio[to] = snd.samples[size_t(k + c)];
                    if (!aes3_) continue;
                    out.aes3[to] = snd.format.aes3 ? snd.words[size_t(k + c)]
                                                   : uint32_t(int32_t(std::nearbyint(double(snd.samples[size_t(k + c)]) * 8388608.0))) & 0xFFFFFF;
                }
            }
        }
    }
    // Its fast metadata: the items timed within the frame.
    out.metadata.clear();
    const uint32_t frame90 = uint32_t(int64_t(90000) * video_.fps.den / std::max(1, video_.fps.num));
    for (auto it = meta_.begin(); it != meta_.end();) {
        const uint32_t d = it->first - p.ts;
        if (d < frame90) {
            out.metadata.insert(out.metadata.end(), it->second.begin(), it->second.end());
            it = meta_.erase(it);
        } else if (int32_t(d) < 0) {
            it = meta_.erase(it);
        } else {
            ++it;
        }
    }
    // Its ancillary data: the packets timed within the frame (both fields'), and its time code.
    out.anc.clear();
    const uint32_t span90 = uint32_t(int64_t(90000) * video_.fps.den / std::max(1, video_.fps.num));
    for (auto it = anc_.begin(); it != anc_.end();) {
        const uint32_t d = it->first - p.ts;
        if (d < span90) {
            out.anc.insert(out.anc.end(), it->second.begin(), it->second.end());
            it = anc_.erase(it);
        } else if (int32_t(d) < 0) {
            it = anc_.erase(it);  // (an earlier frame's, not taken)
        } else {
            ++it;
        }
    }
    out.hasTimecode = frameTimecode(out.anc, video_.fps, out.timecode);
}

bool Input::next(InputFrame& out, int ms) {
    std::unique_lock lock(m_);
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (!pending_.empty() && ready(pending_.front(), now)) {
            Pending p = std::move(pending_.front());
            pending_.pop_front();
            // (joining streams already going: frames from before the first ancillary data heard are passed over, as
            // their time code went by before the input listened)
            if (hasAnc_ && ancSeen_ && !joined_ && int32_t(p.ts - ancFirst_) < 0) continue;
            joined_ = true;
            build(p, out);
            ++stats_.frames;
            return true;
        }
        if (stop_ || now >= until) return false;
        cv_.wait_until(lock, std::min(until, now + std::chrono::milliseconds(10)));
    }
}

}  // namespace montage::st2110
