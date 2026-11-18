// Montage — how a browser review judges each viewer's link from when the packets sent them arrive, not only from the
// ones that are lost. Each picture packet carries a sequence number counted over the whole connection (the
// transport-wide sequence number header extension), and the viewer's browser reports back which arrived and when
// (RTCP transport-wide feedback), both as draft-holmer-rmcat-transport-wide-cc-extensions-01 describes. From the
// arrival times Montage follows the queue on the way to the viewer: a queue that stands and does not drain means the
// link carries less than it is sent, well before anything is dropped. The same reports judge a probe: a short burst of
// padding at a higher rate, which either arrives as promptly as the pictures did or does not.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

namespace montage::output {

// The header extension's name in an offer, and the feedback's.
extern const char* const kTransportWideExtension;
extern const char* const kTransportWideFeedback;

// One transport-wide feedback report: for each packet from `base` on, whether it arrived, and how long after the one
// before it (the first: after the reference time).
struct ArrivalReport {
    uint16_t base = 0;          // the first packet's sequence number
    uint32_t referenceTime = 0; // the receiver's clock in 64 ms steps (24 bits, wrapping)
    struct Packet {
        bool received = false;
        int32_t delta = 0;      // 250 µs steps
    };
    std::vector<Packet> packets;
};

// Reads one RTCP packet (from its header on, `size` bytes with it) as a transport-wide feedback report; false when it
// is not one or is cut short.
bool parseTransportFeedback(const uint8_t* packet, size_t size, ArrivalReport& out);

// An RTP packet with the transport-wide sequence number added to its header extensions under `id` (1 to 14), in the
// one-byte or two-byte form the packet already uses (a header extension is begun when it has none). Empty when the
// packet is not RTP.
std::vector<uint8_t> withTransportSequence(const uint8_t* rtp, size_t size, int id, uint16_t sequence);

// Padding packets to follow a picture's last packet (`last`, with its marker bit): the same stream, payload type and
// timestamp, sequence numbers from `nextSequence` on, each with up to 255 bytes of padding and no payload, `bytes` in
// all (none when `last` does not end a picture).
std::vector<std::vector<uint8_t>> paddingAfter(const uint8_t* last, size_t size, uint16_t& nextSequence, size_t bytes);

// The queue on the way to one viewer, from their arrival reports.
class LinkDelay {
public:
    using Clock = std::chrono::steady_clock;

    // A packet of `bytes` sent at `at`: its transport-wide sequence number. Packets sent at the same moment (a
    // picture's, and the padding after it) are one group, judged by the first of them to arrive.
    uint16_t sent(size_t bytes, Clock::time_point at);
    // The number the next packet sent will be given.
    uint16_t nextSequence() const { return uint16_t(nextSequence_ & 0xffff); }
    // A report read at `now`.
    void feedback(const ArrivalReport& report, Clock::time_point now);

    // Reports have come in the last two seconds.
    bool active(Clock::time_point now) const;
    // How much longer the latest group took than the quickest of the last minute, in ms.
    double queueMs() const;
    // The queue stands: every group reported over the last 0.45 s or more (up to 2 s) took 50 ms or more longer than
    // the quickest, and over them the queue is not draining (a key frame's burst on a link with room drains).
    bool standing() const;
    // What arrived over the last second reported, in kb/s (0 while less than 0.3 s is known).
    double receivedKbps() const;
    // Only groups sent from `from` on count for standing(): after a move to another size, or a probe.
    void restart(Clock::time_point from);

    // A probe: the groups sent from `now` for `length` are watched.
    void beginProbe(Clock::time_point now, std::chrono::milliseconds length);
    enum class Probe {
        None,     // no probe begun
        Running,  // still being sent, or its reports still awaited
        Arrived,  // all but a tenth arrived, none of it queued
        Delayed,  // three groups in a row queued 40 ms or more, a tenth or more was lost, or no report came
    };
    Probe probe(Clock::time_point now) const;
    // Whether padding should still be sent for the probe (until its length is up or it is judged).
    bool probeSending(Clock::time_point now) const;

private:
    struct Sent {
        int64_t sentUs = 0;
        uint32_t bytes = 0;
        int64_t group = 0;
        bool arrived = false, lost = false;
    };
    struct Group {
        int64_t sentUs = 0;
        int64_t delayUs = 0;  // the least of its packets' (arrival minus sending, on two clocks)
        uint32_t bytesSent = 0, bytesArrived = 0;
        bool arrived = false;
    };
    int64_t baselineUs() const;
    double queueOf(const Group& g) const;

    Clock::time_point epoch_ = Clock::now();
    std::deque<Sent> sent_;    // by sequence number, from firstSequence_
    int64_t firstSequence_ = 0, nextSequence_ = 0;
    std::deque<Group> groups_; // by sending time, from firstGroup_
    int64_t firstGroup_ = 0;
    std::deque<std::pair<int64_t, int64_t>> minima_;   // each second's least delay (by the second sent), a minute of them
    std::deque<std::pair<int64_t, uint32_t>> arrivals_;  // arrival times (µs, the receiver's clock) and sizes, two seconds
    bool haveReference_ = false;
    uint32_t lastReference_ = 0;
    int64_t reference_ = 0;  // the reference time unwrapped, 64 ms steps
    uint64_t reports_ = 0;
    Clock::time_point lastReport_;
    int64_t sinceUs_ = 0;                       // standing() looks at groups sent from here on
    int64_t probeFromUs_ = -1, probeUntilUs_ = -1;
};

}  // namespace montage::output
