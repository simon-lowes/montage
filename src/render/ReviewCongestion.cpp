#include "render/ReviewCongestion.h"

#include <algorithm>

namespace montage::output {

const char* const kTransportWideExtension = "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01";
const char* const kTransportWideFeedback = "transport-cc";

namespace {

constexpr int64_t kGroupGapUs = 2000;         // packets sent within this of a group's first are of it
constexpr int64_t kHistoryUs = 10'000'000;    // how long a packet sent is remembered
constexpr int64_t kStandingWindowUs = 2'000'000, kStandingSpanUs = 450'000;
constexpr int kStandingGroups = 6;
constexpr double kStandingMs = 50, kDrainingMsPerS = -25;
constexpr double kProbeQueueMs = 40;
constexpr int64_t kProbeWaitUs = 1'000'000;   // how long after a probe its reports are waited for
constexpr size_t kRemembered = 32768;         // packets sent remembered at most
constexpr int64_t kRoomPerPacket = 4;         // packets reports may go over, for each one sent

int32_t signed24(uint32_t v) {
    v &= 0xffffff;
    return v & 0x800000 ? int32_t(v) - 0x1000000 : int32_t(v);
}

// Where an RTP packet's fixed header and contributing sources end, or 0 when it is not RTP.
size_t rtpHeaderEnd(const uint8_t* rtp, size_t size) {
    if (!rtp || size < 12 || (rtp[0] >> 6) != 2) return 0;
    const size_t end = 12 + 4 * size_t(rtp[0] & 15);
    return end <= size ? end : 0;
}

}  // namespace

bool parseTransportFeedback(const uint8_t* p, size_t size, ArrivalReport& out, size_t most) {
    out = {};
    if (!p || size < 20 || (p[0] >> 6) != 2 || (p[0] & 31) != 15 || p[1] != 205) return false;
    const size_t length = (size_t((p[2] << 8) | p[3]) + 1) * 4;
    if (length > size || length < 20) return false;
    size = length;
    if (p[0] & 0x20) {
        const size_t padding = p[size - 1];
        if (padding == 0 || padding > size - 20) return false;
        size -= padding;
    }
    out.base = uint16_t((p[12] << 8) | p[13]);
    const size_t count = size_t((p[14] << 8) | p[15]);
    if (count > most) return false;
    out.referenceTime = (uint32_t(p[16]) << 16) | (uint32_t(p[17]) << 8) | p[18];
    size_t at = 20;
    std::vector<uint8_t> symbols;
    symbols.reserve(count);
    while (symbols.size() < count) {
        if (at + 2 > size) return false;
        const unsigned chunk = unsigned((p[at] << 8) | p[at + 1]);
        at += 2;
        if (!(chunk & 0x8000)) {
            // (a run of one symbol)
            const uint8_t symbol = uint8_t((chunk >> 13) & 3);
            for (unsigned run = chunk & 0x1fff; run > 0 && symbols.size() < count; --run) symbols.push_back(symbol);
        } else if (!(chunk & 0x4000)) {
            // (fourteen symbols of one bit: arrived with a small delta, or not)
            for (int i = 13; i >= 0 && symbols.size() < count; --i) symbols.push_back(uint8_t((chunk >> i) & 1));
        } else {
            // (seven symbols of two bits)
            for (int i = 12; i >= 0 && symbols.size() < count; i -= 2) symbols.push_back(uint8_t((chunk >> i) & 3));
        }
    }
    out.packets.resize(count);
    for (size_t i = 0; i < count; ++i) {
        ArrivalReport::Packet& packet = out.packets[i];
        if (symbols[i] == 0) continue;
        if (symbols[i] == 1) {
            if (at + 1 > size) return false;
            packet.delta = p[at];
            at += 1;
        } else if (symbols[i] == 2) {
            if (at + 2 > size) return false;
            packet.delta = int16_t(uint16_t((p[at] << 8) | p[at + 1]));
            at += 2;
        } else {
            return false;  // (a symbol the draft keeps back)
        }
        packet.received = true;
    }
    return true;
}

std::vector<uint8_t> withTransportSequence(const uint8_t* rtp, size_t size, int id, uint16_t sequence) {
    const size_t header = rtpHeaderEnd(rtp, size);
    if (!header || id < 1 || id > 14) return {};
    std::vector<uint8_t> out;
    out.reserve(size + 8);
    if (!(rtp[0] & 0x10)) {
        out.assign(rtp, rtp + header);
        out[0] |= 0x10;
        const uint8_t extension[8] = {0xbe, 0xde, 0, 1, uint8_t((id << 4) | 1), uint8_t(sequence >> 8), uint8_t(sequence & 0xff), 0};
        out.insert(out.end(), extension, extension + 8);
        out.insert(out.end(), rtp + header, rtp + size);
        return out;
    }
    if (size < header + 4) return {};
    const unsigned profile = unsigned((rtp[header] << 8) | rtp[header + 1]);
    const size_t words = size_t((rtp[header + 2] << 8) | rtp[header + 3]);
    const size_t body = header + 4 + words * 4;
    const bool oneByte = profile == 0xbede, twoByte = (profile & 0xfff0) == 0x1000;
    if (body > size || (!oneByte && !twoByte) || words >= 0xffff) return {};
    out.assign(rtp, rtp + body);
    out[header + 2] = uint8_t((words + 1) >> 8);
    out[header + 3] = uint8_t((words + 1) & 0xff);
    // (four bytes more either way: the one-byte form's element and a byte of padding, or the two-byte form's)
    if (oneByte) out.insert(out.end(), {uint8_t((id << 4) | 1), uint8_t(sequence >> 8), uint8_t(sequence & 0xff), uint8_t(0)});
    else out.insert(out.end(), {uint8_t(id), uint8_t(2), uint8_t(sequence >> 8), uint8_t(sequence & 0xff)});
    out.insert(out.end(), rtp + body, rtp + size);
    return out;
}

std::vector<std::vector<uint8_t>> paddingAfter(const uint8_t* last, size_t size, uint16_t& nextSequence, size_t bytes) {
    std::vector<std::vector<uint8_t>> out;
    const size_t header = rtpHeaderEnd(last, size);
    if (!header || !(last[1] & 0x80)) return out;
    // (at most this many after one picture, whatever is asked)
    for (int n = 0; n < 512 && bytes > header; ++n) {
        const size_t padding = std::min<size_t>(255, bytes - header);
        std::vector<uint8_t> p(last, last + header);
        p[0] = uint8_t((p[0] & ~0x10) | 0x20);  // (no header extension, padding)
        p[1] &= 0x7f;                           // (no marker: the picture has ended already)
        p[2] = uint8_t(nextSequence >> 8);
        p[3] = uint8_t(nextSequence & 0xff);
        ++nextSequence;
        p.resize(header + padding, 0);
        p.back() = uint8_t(padding);
        bytes -= header + padding;
        out.push_back(std::move(p));
    }
    return out;
}

uint16_t LinkDelay::sent(size_t bytes, Clock::time_point at) {
    const int64_t us = std::chrono::duration_cast<std::chrono::microseconds>(at - epoch_).count();
    if (groups_.empty() || us - groups_.back().sentUs > kGroupGapUs) {
        Group g;
        g.sentUs = us;
        groups_.push_back(g);
    }
    groups_.back().bytesSent += uint32_t(bytes);
    Sent s;
    s.sentUs = us;
    s.bytes = uint32_t(bytes);
    s.group = firstGroup_ + int64_t(groups_.size()) - 1;
    sent_.push_back(s);
    room_ = std::min<int64_t>(room_ + kRoomPerPacket, kRoomPerPacket * int64_t(kRemembered));
    while (sent_.size() > kRemembered || (!sent_.empty() && us - sent_.front().sentUs > kHistoryUs)) {
        sent_.pop_front();
        ++firstSequence_;
    }
    // (a group goes once no packet remembered is of it)
    const int64_t oldest = sent_.empty() ? firstGroup_ + int64_t(groups_.size()) : sent_.front().group;
    while (firstGroup_ < oldest && groups_.size() > 1) {
        groups_.pop_front();
        ++firstGroup_;
    }
    return uint16_t(nextSequence_++ & 0xffff);
}

void LinkDelay::feedback(const ArrivalReport& r, Clock::time_point now) {
    ++reports_;
    lastReport_ = now;
    reference_ = haveReference_ ? reference_ + signed24(r.referenceTime - lastReference_) : signed24(r.referenceTime);
    haveReference_ = true;
    lastReference_ = r.referenceTime;
    room_ = std::max<int64_t>(0, room_ - int64_t(r.packets.size()));
    // (the latest packet sent with that number)
    int64_t sequence = nextSequence_ - int64_t(uint16_t(uint16_t(nextSequence_ & 0xffff) - r.base));
    int64_t arrivalUs = reference_ * 64000;
    int64_t latest = arrivals_.empty() ? INT64_MIN : arrivals_.back().first;
    const size_t before = arrivals_.size();
    for (const ArrivalReport::Packet& packet : r.packets) {
        const int64_t n = sequence++;
        if (packet.received) arrivalUs += int64_t(packet.delta) * 250;
        if (n < firstSequence_ || n >= nextSequence_) continue;
        Sent& s = sent_[size_t(n - firstSequence_)];
        if (!packet.received) {
            if (!s.arrived) s.lost = true;
            continue;
        }
        // (once only: a report may go over packets again, and one sent again arrives late for another reason)
        if (s.arrived || s.lost) continue;
        s.arrived = true;
        const int64_t delay = arrivalUs - s.sentUs;
        if (s.group >= firstGroup_) {
            Group& g = groups_[size_t(s.group - firstGroup_)];
            g.delayUs = g.arrived ? std::min(g.delayUs, delay) : delay;
            g.arrived = true;
            g.bytesArrived += s.bytes;
        }
        const int64_t second = s.sentUs / 1'000'000;
        auto slot = std::find_if(minima_.rbegin(), minima_.rend(), [&](const auto& m) { return m.first <= second; });
        if (slot != minima_.rend() && slot->first == second) slot->second = std::min(slot->second, delay);
        else minima_.insert(slot.base(), {second, delay});
        arrivals_.emplace_back(arrivalUs, s.bytes);
        latest = std::max(latest, arrivalUs);
    }
    while (!minima_.empty() && minima_.front().first < minima_.back().first - 60) minima_.pop_front();
    // (only what this report added is sorted in: a report that adds nothing costs nothing here)
    if (arrivals_.size() > before) {
        const auto added = arrivals_.begin() + ptrdiff_t(before);
        std::sort(added, arrivals_.end());
        if (before > 0 && *added < *(added - 1)) std::inplace_merge(arrivals_.begin(), added, arrivals_.end());
    }
    while (!arrivals_.empty() && arrivals_.front().first < latest - 2'000'000) arrivals_.pop_front();
}

bool LinkDelay::active(Clock::time_point now) const { return reports_ > 0 && now - lastReport_ < std::chrono::seconds(2); }

int64_t LinkDelay::baselineUs() const {
    int64_t least = INT64_MAX;
    for (const auto& m : minima_) least = std::min(least, m.second);
    return least;
}

double LinkDelay::queueOf(const Group& g) const { return double(g.delayUs - baselineUs()) / 1000.0; }

double LinkDelay::queueMs() const {
    for (auto g = groups_.rbegin(); g != groups_.rend(); ++g)
        if (g->arrived) return queueOf(*g);
    return 0;
}

bool LinkDelay::standing() const {
    auto last = std::find_if(groups_.rbegin(), groups_.rend(), [](const Group& g) { return g.arrived; });
    if (last == groups_.rend() || last->sentUs < sinceUs_) return false;
    const int64_t to = last->sentUs, from = std::max(to - kStandingWindowUs, sinceUs_);
    int n = 0;
    int64_t first = to;
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    // (the groups back from the latest that all queued: at least 0.45 s of them, at most 2 s)
    for (auto g = last; g != groups_.rend() && g->sentUs >= from; ++g) {
        if (!g->arrived) continue;
        const double queue = queueOf(*g);
        if (queue < kStandingMs) break;
        const double x = double(g->sentUs - to) / 1e6;
        sx += x, sy += queue, sxx += x * x, sxy += x * queue;
        first = g->sentUs;
        ++n;
    }
    if (n < kStandingGroups || to - first < kStandingSpanUs) return false;
    const double spread = n * sxx - sx * sx;
    const double slope = spread > 0 ? (n * sxy - sx * sy) / spread : 0;
    return slope > kDrainingMsPerS;
}

double LinkDelay::receivedKbps() const {
    if (arrivals_.empty()) return 0;
    const int64_t to = arrivals_.back().first, from = std::max(arrivals_.front().first, to - 1'000'000);
    if (to - from < 300'000) return 0;
    uint64_t bytes = 0;
    for (const auto& [at, size] : arrivals_)
        if (at > from) bytes += size;  // (the first only marks where the time begins)
    return double(bytes) * 8000.0 / double(to - from);
}

void LinkDelay::restart(Clock::time_point from) { sinceUs_ = std::chrono::duration_cast<std::chrono::microseconds>(from - epoch_).count(); }

void LinkDelay::beginProbe(Clock::time_point now, std::chrono::milliseconds length) {
    probeFromUs_ = std::chrono::duration_cast<std::chrono::microseconds>(now - epoch_).count();
    probeUntilUs_ = probeFromUs_ + std::chrono::duration_cast<std::chrono::microseconds>(length).count();
}

LinkDelay::Probe LinkDelay::probe(Clock::time_point now) const {
    if (probeFromUs_ < 0) return Probe::None;
    const int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(now - epoch_).count();
    int queued = 0;
    int64_t lastArrived = -1;
    for (const Group& g : groups_) {
        if (g.sentUs < probeFromUs_ || g.sentUs > probeUntilUs_ || !g.arrived) continue;
        queued = queueOf(g) >= kProbeQueueMs ? queued + 1 : 0;
        if (queued >= 3) return Probe::Delayed;
        lastArrived = g.sentUs;
    }
    if (nowUs < probeUntilUs_) return Probe::Running;
    // (judged once the reports reach its end, or a second after it)
    const bool reported = lastArrived >= probeUntilUs_ - 100'000;
    if (!reported && nowUs < probeUntilUs_ + kProbeWaitUs) return Probe::Running;
    if (lastArrived < 0) return Probe::Delayed;
    // (of the groups wholly reported: those before the last that arrived, or all of them once waiting is over)
    uint64_t sent = 0, arrived = 0;
    for (const Group& g : groups_) {
        if (g.sentUs < probeFromUs_ || g.sentUs > probeUntilUs_ || (reported && g.sentUs >= lastArrived)) continue;
        sent += g.bytesSent;
        arrived += g.bytesArrived;
    }
    return arrived * 10 >= sent * 9 ? Probe::Arrived : Probe::Delayed;
}

bool LinkDelay::probeSending(Clock::time_point now) const {
    if (probeFromUs_ < 0) return false;
    const int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(now - epoch_).count();
    return nowUs < probeUntilUs_ && probe(now) == Probe::Running;
}

}  // namespace montage::output
