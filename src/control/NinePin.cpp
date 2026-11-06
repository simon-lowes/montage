#include "NinePin.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "audio/Ltc.h"

namespace montage::ninepin {

std::vector<uint8_t> encode(const Message& m) {
    const size_t n = std::min<size_t>(m.data.size(), 15);
    std::vector<uint8_t> out;
    out.reserve(n + 3);
    out.push_back(uint8_t((m.group << 4) | n));
    out.push_back(m.cmd);
    out.insert(out.end(), m.data.begin(), m.data.begin() + ptrdiff_t(n));
    uint8_t sum = 0;
    for (uint8_t b : out) sum = uint8_t(sum + b);
    out.push_back(sum);
    return out;
}

void Parser::feed(const uint8_t* bytes, size_t n, Clock::time_point now) {
    buf_.insert(buf_.end(), bytes, bytes + n);
    came_.insert(came_.end(), n, now);
    if (buf_.size() > 4096) drop(buf_.size() - 64);  // (a line full of noise)
}

void Parser::drop(size_t n) {
    buf_.erase(buf_.begin(), buf_.begin() + ptrdiff_t(n));
    came_.erase(came_.begin(), came_.begin() + ptrdiff_t(n));
}

bool Parser::goodAt(size_t k) const {
    if (k >= buf_.size() || (buf_[k] >> 4) > 7) return false;
    const size_t total = (buf_[k] & 0x0F) + 3;
    if (buf_.size() - k < total) return false;
    uint8_t sum = 0;
    for (size_t i = k; i + 1 < k + total; ++i) sum = uint8_t(sum + buf_[i]);
    return sum == buf_[k + total - 1];
}

Parser::Result Parser::next(Message& out, Clock::time_point now) {
    for (;;) {
        // (groups 0-7 only: a byte above starts nothing, as on a line picked up mid-message)
        size_t skip = 0;
        while (skip < buf_.size() && (buf_[skip] >> 4) > 7) ++skip;
        drop(skip);
        if (buf_.empty()) {
            resyncing_ = false;
            return None;
        }
        const size_t total = (buf_[0] & 0x0F) + 3, have = std::min(total, buf_.size());
        // A message whose bytes stopped coming for longer than kByteGap (before the next came, or until now while it
        // is short) was not one: its first byte passed over.
        bool broken = have < total && now - came_[have - 1] > kByteGap;
        for (size_t i = 1; i < have && !broken; ++i) broken = came_[i] - came_[i - 1] > kByteGap;
        if (broken) {
            drop(1);
            continue;
        }
        if (have < total) return None;
        if (goodAt(0)) {
            out.group = uint8_t(buf_[0] >> 4);
            out.cmd = buf_[1];
            out.data.assign(buf_.begin() + 2, buf_.begin() + ptrdiff_t(total - 1));
            drop(total);
            resyncing_ = false;
            return Ok;
        }
        // A wrong sum: a good message starting inside this one is taken, the bytes before it noise.
        size_t k = 1;
        while (k < total && !goodAt(k)) ++k;
        if (k < total) {
            drop(k);
            continue;
        }
        // Garbled: reported once, then passed over a byte at a time until the line is back in step.
        out.group = uint8_t(buf_[0] >> 4);
        out.cmd = buf_[1];
        out.data.clear();
        drop(1);
        if (resyncing_) continue;
        resyncing_ = true;
        return BadChecksum;
    }
}

namespace {
uint8_t bcd(int v) { return uint8_t(((v / 10) << 4) | (v % 10)); }
int fromBcd(uint8_t b, int maxTens) {
    const int hi = b >> 4, lo = b & 0x0F;
    if (hi > maxTens || lo > 9) return -1;
    return hi * 10 + lo;
}
}  // namespace

std::array<uint8_t, 4> timecodeBytes(int64_t frame, int base, bool dropFrame) {
    const LtcFrame f = ltcFrameAt(frame, base, dropFrame && base == 30);
    return {uint8_t(bcd(f.frames) | (f.dropFrame ? 0x40 : 0)), bcd(f.seconds), bcd(f.minutes), bcd(f.hours)};
}

namespace {
// A label as a frame number, counted the way it says; one drop frame skips stands for the next there is.
int64_t labelNumber(LtcFrame f, int base) {
    if (f.dropFrame && base == 30 && f.seconds == 0 && f.frames < 2 && f.minutes % 10 != 0) f.frames = 2;
    return ltcFrameNumber(f, base);
}
}  // namespace

int64_t sameLabel(int64_t frame, int base, bool fromDrop, bool toDrop) {
    fromDrop = fromDrop && base == 30, toDrop = toDrop && base == 30;
    if (fromDrop == toDrop || frame < 0) return frame;
    // (whole days kept: a clip logged over midnight runs on into the next)
    const int64_t days = frame / ltcFramesPerDay(base, fromDrop);
    LtcFrame f = ltcFrameAt(frame, base, fromDrop);
    f.dropFrame = toDrop;
    return labelNumber(f, base) + days * ltcFramesPerDay(base, toDrop);
}

int64_t timecodeFromBytesAs(const uint8_t* b, int base, bool dropFrame) {
    bool flagged = false;
    const int64_t f = timecodeFromBytes(b, base, &flagged);
    return sameLabel(f, base, flagged, dropFrame);
}

int64_t timecodeFromBytes(const uint8_t* b, int base, bool* dropFrame) {
    LtcFrame f;
    f.dropFrame = (b[0] & 0x40) != 0 && base == 30;
    f.frames = fromBcd(b[0] & 0x3F, 3);
    f.seconds = fromBcd(b[1] & 0x7F, 5);  // (bit 7: the field, in VITC's)
    f.minutes = fromBcd(b[2] & 0x7F, 5);
    f.hours = fromBcd(b[3] & 0x3F, 2);
    if (dropFrame) *dropFrame = f.dropFrame;
    if (f.frames < 0 || f.seconds < 0 || f.minutes < 0 || f.hours < 0 || f.frames >= base || f.seconds > 59 || f.minutes > 59 || f.hours > 23)
        return -1;
    return labelNumber(f, base);
}

double speedFromData(const std::vector<uint8_t>& data) {
    if (data.empty()) return 1;
    double n = data[0];
    if (data.size() >= 2) n += data[1] / 256.0;
    if (n <= 0) return 0;  // (0: stopped, as decks take it)
    return std::pow(10.0, n / 32.0 - 2.0);
}

std::vector<uint8_t> speedData(double timesPlay, bool fine) {
    timesPlay = std::fabs(timesPlay);
    if (timesPlay <= 0) return {0};
    const double n = std::clamp(32.0 * (std::log10(timesPlay) + 2.0), 0.0, 255.0 + 255.0 / 256.0);
    if (!fine) return {uint8_t(std::lround(std::min(n, 255.0)))};
    const int whole = int(std::floor(n));
    return {uint8_t(whole), uint8_t(std::min(255L, std::lround((n - whole) * 256)))};
}

std::array<uint8_t, 10> Status::bytes() const {
    std::array<uint8_t, 10> b{};
    b[0] = uint8_t((local ? 0x01 : 0) | (servoRefMissing ? 0x10 : 0) | (tapeOut ? 0x20 : 0));
    b[1] = uint8_t((play ? 0x01 : 0) | (record ? 0x02 : 0) | (fastForward ? 0x04 : 0) | (rewind ? 0x08 : 0) | (eject ? 0x10 : 0) |
                   (stop ? 0x20 : 0) | (standby ? 0x80 : 0));
    b[2] = uint8_t((cueComplete ? 0x01 : 0) | (still ? 0x02 : 0) | (reverse ? 0x04 : 0) | (var ? 0x08 : 0) | (jog ? 0x10 : 0) |
                   (shuttle ? 0x20 : 0) | (servoLock ? 0x80 : 0));
    b[3] = uint8_t((inSet ? 0x01 : 0) | (outSet ? 0x02 : 0) | (audioInSet ? 0x04 : 0) | (audioOutSet ? 0x08 : 0) | (autoMode ? 0x80 : 0));
    b[4] = uint8_t((preroll ? 0x01 : 0) | (preview ? 0x02 : 0) | (autoEdit ? 0x04 : 0) | (review ? 0x08 : 0) | (edit ? 0x10 : 0) |
                   (fullEe ? 0x40 : 0) | (selectEe ? 0x80 : 0));
    return b;
}

Status Status::from(const uint8_t* b, size_t n) {
    uint8_t v[10] = {};
    std::copy(b, b + std::min<size_t>(n, 10), v);
    Status s;
    s.local = v[0] & 0x01, s.servoRefMissing = v[0] & 0x10, s.tapeOut = v[0] & 0x20;
    s.play = v[1] & 0x01, s.record = v[1] & 0x02, s.fastForward = v[1] & 0x04, s.rewind = v[1] & 0x08, s.eject = v[1] & 0x10,
    s.stop = v[1] & 0x20, s.standby = v[1] & 0x80;
    s.cueComplete = v[2] & 0x01, s.still = v[2] & 0x02, s.reverse = v[2] & 0x04, s.var = v[2] & 0x08, s.jog = v[2] & 0x10,
    s.shuttle = v[2] & 0x20, s.servoLock = v[2] & 0x80;
    s.inSet = v[3] & 0x01, s.outSet = v[3] & 0x02, s.audioInSet = v[3] & 0x04, s.audioOutSet = v[3] & 0x08, s.autoMode = v[3] & 0x80;
    s.preroll = v[4] & 0x01, s.preview = v[4] & 0x02, s.autoEdit = v[4] & 0x04, s.review = v[4] & 0x08, s.edit = v[4] & 0x10,
    s.fullEe = v[4] & 0x40, s.selectEe = v[4] & 0x80;
    return s;
}

// EDIT PRESET's bits: byte 1 A1 (bit 0), A2 (1), timecode (2), video (4), assemble (5); byte 2 A3 and A4 (bits 0, 1);
// byte 3 A5 to A8.
std::vector<uint8_t> EditChannels::data() const {
    std::vector<uint8_t> d(1, 0);
    d[0] = uint8_t((audio[0] ? 0x01 : 0) | (audio[1] ? 0x02 : 0) | (timecode ? 0x04 : 0) | (video ? 0x10 : 0) | (assemble ? 0x20 : 0));
    const bool more = audio[2] || audio[3], most = std::any_of(audio.begin() + 4, audio.end(), [](bool b) { return b; });
    if (more || most) d.push_back(uint8_t((audio[2] ? 0x01 : 0) | (audio[3] ? 0x02 : 0)));
    if (most) d.push_back(uint8_t((audio[4] ? 0x01 : 0) | (audio[5] ? 0x02 : 0) | (audio[6] ? 0x04 : 0) | (audio[7] ? 0x08 : 0)));
    return d;
}

EditChannels EditChannels::from(const std::vector<uint8_t>& d) {
    EditChannels c;
    c.audio.fill(false);
    const uint8_t b0 = d.empty() ? 0 : d[0], b1 = d.size() > 1 ? d[1] : 0, b2 = d.size() > 2 ? d[2] : 0;
    c.audio[0] = b0 & 0x01, c.audio[1] = b0 & 0x02, c.timecode = b0 & 0x04, c.video = b0 & 0x10, c.assemble = b0 & 0x20;
    c.audio[2] = b1 & 0x01, c.audio[3] = b1 & 0x02;
    for (int i = 0; i < 4; ++i) c.audio[size_t(4 + i)] = b2 & (1 << i);
    return c;
}

// ---- Montage controlling a deck

Deck::~Deck() { close(); }

bool Deck::open(const std::string& port, std::string* error) {
    std::lock_guard lock(m_);
    parser_.clear();
    const bool ok = port_.open(port, 38400, 'O', &error_);
    if (!ok && error) *error = error_;
    return ok;
}

void Deck::close() {
    std::lock_guard lock(m_);
    port_.close();
}

bool Deck::isOpen() const {
    std::lock_guard lock(m_);
    return port_.isOpen();
}

std::string Deck::lastError() const {
    std::lock_guard lock(m_);
    return error_;
}

namespace {
// Whether `a` can be the answer to `q`: a NAK, or the kind of answer `q` asks for (so one left from an earlier command
// that came too late is not taken for it).
bool answers(const Message& q, const Message& a) {
    if (a.is(1, cmd::Nak)) return true;
    if (q.group == 0 && q.cmd == cmd::DeviceTypeRequest) return a.is(1, cmd::DeviceType);
    if (q.group != 6) return a.is(1, cmd::Ack);
    if (a.group != 7) return false;
    if (q.cmd != cmd::CurrentTimeSense) return a.cmd == q.cmd;  // (STATUS SENSE to STATUS DATA, IN DATA SENSE to IN DATA...)
    const uint8_t ask = q.data.empty() ? cmd::SenseLtc : q.data[0];
    switch (a.cmd) {
    case cmd::LtcTimeData:
    case cmd::CorrectedLtcTimeData: return (ask & cmd::SenseLtc) != 0;
    case cmd::VitcTimeData:
    case cmd::HoldVitcTimeData: return (ask & cmd::SenseVitc) != 0;
    case cmd::Timer1Data: return (ask & cmd::SenseTimer1) != 0;
    case cmd::Timer2Data: return (ask & cmd::SenseTimer2) != 0;
    case cmd::LtcUserBitsData: return (ask & cmd::SenseLtcUserBits) != 0;
    case cmd::VitcUserBitsData: return (ask & cmd::SenseVitcUserBits) != 0;
    default: return false;
    }
}
}  // namespace

std::optional<Message> Deck::command(const Message& m, int timeoutMs, int tries) {
    std::lock_guard lock(m_);
    if (!port_.isOpen()) {
        error_ = "No deck connected";
        return std::nullopt;
    }
    const std::vector<uint8_t> bytes = encode(m);
    for (int attempt = 0; attempt < std::max(1, tries); ++attempt) {
        // (anything left from an answer that came too late is not this one's)
        port_.flushInput();
        parser_.clear();
        if (!port_.write(bytes)) {
            error_ = "The deck's port failed";
            return std::nullopt;
        }
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        uint8_t buf[64];
        bool again = false;
        for (;;) {
            Message reply;
            const Parser::Result r = parser_.next(reply);
            if (r == Parser::BadChecksum) {
                error_ = "The deck's answer was garbled";
                again = true;
                break;
            }
            if (r == Parser::Ok) {
                if (!answers(m, reply)) continue;
                if (reply.is(1, cmd::Nak)) {
                    const uint8_t why = reply.data.empty() ? 0 : reply.data[0];
                    error_ = why & cmd::NakUnknownCommand ? "The deck does not know that command"
                             : why & cmd::NakChecksum     ? "The deck heard a garbled command"
                             : why & cmd::NakTimeout      ? "The deck timed out on the command"
                                                          : "The deck refused the command (NAK " + std::to_string(int(why)) + ")";
                    // (the line's errors are worth another try; a command the deck will not take is not)
                    again = !(why & cmd::NakUnknownCommand) &&
                            (why & (cmd::NakChecksum | cmd::NakParity | cmd::NakOverrun | cmd::NakFraming | cmd::NakTimeout));
                    if (again) break;
                    return std::nullopt;
                }
                error_.clear();
                return reply;
            }
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count();
            if (left <= 0) {
                error_ = "The deck did not answer";
                again = true;
                break;
            }
            const int n = port_.read(buf, sizeof buf, int(std::min<int64_t>(left, Parser::kByteGap.count())));
            if (n < 0) {
                error_ = "The deck's port failed";
                return std::nullopt;
            }
            parser_.feed(buf, size_t(n));
        }
        if (!again) break;
    }
    return std::nullopt;
}

bool Deck::ack(uint8_t group, uint8_t c, std::vector<uint8_t> data) {
    const auto r = command(Message{group, c, std::move(data)});
    if (r && !r->is(1, cmd::Ack)) {
        std::lock_guard lock(m_);
        error_ = "The deck answered something else";
        return false;
    }
    return r.has_value();
}

bool Deck::step(bool forward) {
    const auto r = command(Message{2, forward ? cmd::StepForward : cmd::StepReverse, {}}, 100, 1);
    return r && r->is(1, cmd::Ack);
}

std::optional<std::array<uint8_t, 2>> Deck::deviceType() {
    const auto r = command(Message{0, cmd::DeviceTypeRequest, {}});
    if (!r || !r->is(1, cmd::DeviceType) || r->data.size() < 2) return std::nullopt;
    return std::array<uint8_t, 2>{r->data[0], r->data[1]};
}

bool Deck::shuttle(double speed) {
    if (speed == 0) return stop();
    return ack(2, speed > 0 ? cmd::ShuttleForward : cmd::ShuttleReverse, speedData(speed));
}

bool Deck::jog(double speed) {
    if (speed == 0) return stop();
    return ack(2, speed > 0 ? cmd::JogForward : cmd::JogReverse, speedData(speed));
}

bool Deck::variable(double speed) {
    if (speed == 0) return stop();
    return ack(2, speed > 0 ? cmd::VarForward : cmd::VarReverse, speedData(speed, true));
}

bool Deck::cue(int64_t frame) {
    const auto b = timecodeBytes(frame, base_, deckDrop_);
    return ack(2, cmd::CueUpWithData, {b.begin(), b.end()});
}

bool Deck::setIn(int64_t frame) {
    const auto b = timecodeBytes(frame, base_, deckDrop_);
    return ack(4, cmd::InDataPreset, {b.begin(), b.end()});
}

bool Deck::setOut(int64_t frame) {
    const auto b = timecodeBytes(frame, base_, deckDrop_);
    return ack(4, cmd::OutDataPreset, {b.begin(), b.end()});
}

bool Deck::setPrerollTime(int64_t frames) {
    const auto b = timecodeBytes(frames, base_, false);
    return ack(4, cmd::PrerollTimePreset, {b.begin(), b.end()});
}

bool Deck::setEditChannels(const EditChannels& c) { return ack(4, cmd::EditPreset, c.data()); }

std::optional<Status> Deck::status() {
    const auto r = command(Message{6, cmd::StatusSense, {0x0A}});  // (bytes 0 to 9)
    if (!r || r->group != 7 || r->cmd != cmd::StatusData) return std::nullopt;
    return Status::from(r->data.data(), r->data.size());
}

int64_t Deck::timecode(TimeSource source) {
    const uint8_t ask = source == TimeSource::Ltc     ? cmd::SenseLtc
                        : source == TimeSource::Vitc  ? cmd::SenseVitc
                        : source == TimeSource::Timer1 ? cmd::SenseTimer1
                                                       : uint8_t(cmd::SenseLtc | cmd::SenseVitc);
    const auto r = command(Message{6, cmd::CurrentTimeSense, {ask}});  // (only a time of the kind asked is taken)
    if (!r || r->group != 7 || r->data.size() < 4) return -1;
    bool drop = false;
    const int base = base_;
    const int64_t f = timecodeFromBytes(r->data.data(), base, &drop);
    if (f >= 0 && base == 30) deckDrop_ = drop;
    return f;
}

bool Deck::waitForCue(int64_t target, double seconds, const std::atomic<bool>* cancel) {
    const auto start = std::chrono::steady_clock::now();
    const auto until = start + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < until) {
        if (cancel && *cancel) return false;
        if (const auto s = status(); s && s->cueComplete) {
            // A deck that cannot say where it is: trust CUE UP once it has had time to drop the last one.
            const int64_t at = timecode();
            if (at < 0 ? std::chrono::steady_clock::now() - start > std::chrono::milliseconds(300)
                       : std::llabs(at - target) <= 2)
                return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

// ---- Montage answering as a deck

double TransportState::frameNow(std::chrono::steady_clock::time_point now) const {
    return double(frame) + speed * fps * std::chrono::duration<double>(now - at).count();
}

DeckEmulator::DeckEmulator() = default;
DeckEmulator::~DeckEmulator() { stop(); }

bool DeckEmulator::start(const std::string& port, std::string* error) {
    stop();
    if (!port_.open(port, 38400, 'O', error)) return false;
    stop_ = false, failed_ = false;
    thread_ = std::thread([this] { run(); });
    return true;
}

void DeckEmulator::stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    port_.close();
}

bool DeckEmulator::running() const { return thread_.joinable() && !stop_ && !failed_; }

void DeckEmulator::publish(const TransportState& s) {
    std::lock_guard lock(m_);
    state_ = s;
}

TransportState DeckEmulator::state() const {
    std::lock_guard lock(m_);
    return state_;
}

Message DeckEmulator::answer(const Message& m) {
    const Message ack{1, cmd::Ack, {}};
    const Message unknown{1, cmd::Nak, {cmd::NakUnknownCommand}};
    auto request = [&](TransportRequest r) {
        if (onRequest) onRequest(r);
    };
    auto tc = [&](uint8_t c, int64_t frame) {
        const auto b = timecodeBytes(frame, base_, dropFrame_);
        return Message{7, c, {b.begin(), b.end()}};
    };
    std::unique_lock lock(m_);
    const auto now = std::chrono::steady_clock::now();
    const TransportState s = state_;
    // (not before 00:00:00:00, where the Program starts: no 23:59 from a reverse shuttle or a pre-roll there)
    const int64_t here = std::max<int64_t>(0, int64_t(std::floor(s.frameNow(now) + 1e-6)));
    auto setState = [&](int64_t frame, double speed, bool cueing) {
        state_.frame = frame, state_.speed = speed, state_.at = now, state_.cueing = cueing;
    };
    switch (m.group) {
    case 0:
        if (m.cmd == cmd::DeviceTypeRequest)
            // (a 625- or 525-line Betacam editing recorder, the type controllers know best)
            return Message{1, cmd::DeviceType, {uint8_t(base_ == 25 ? 0x21 : 0x20), 0x00}};
        if (m.cmd == cmd::LocalDisable || m.cmd == cmd::LocalEnable) return ack;
        return unknown;
    case 2: {
        TransportRequest r;
        switch (m.cmd) {
        case cmd::Stop:
            setState(here, 0, false);
            r.kind = TransportRequest::Stop;
            break;
        case cmd::Play:
        case cmd::SyncPlay:
            setState(here, 1, false);
            r.kind = TransportRequest::Speed, r.speed = 1;
            break;
        case cmd::FastForward:
        case cmd::Rewind:
            r.kind = TransportRequest::Speed, r.speed = m.cmd == cmd::FastForward ? 10 : -10;
            setState(here, r.speed, false);
            break;
        case cmd::JogForward:
        case cmd::VarForward:
        case cmd::ShuttleForward:
        case cmd::JogReverse:
        case cmd::VarReverse:
        case cmd::ShuttleReverse: {
            const bool back = m.cmd >= cmd::Rewind;
            r.kind = TransportRequest::Speed, r.speed = speedFromData(m.data) * (back ? -1 : 1);
            setState(here, r.speed, false);
            break;
        }
        case cmd::StepForward:
        case cmd::StepReverse:
            r.kind = TransportRequest::Step, r.speed = m.cmd == cmd::StepForward ? 1 : -1;
            setState(here + (m.cmd == cmd::StepForward ? 1 : -1), 0, false);
            break;
        case cmd::CueUpWithData:
            if (m.data.size() < 4) return Message{1, cmd::Nak, {cmd::NakUnknownCommand}};
            // (by its label, counted as the Program counts, whatever the controller's drop frame flag)
            r.kind = TransportRequest::Locate, r.frame = timecodeFromBytesAs(m.data.data(), base_, dropFrame_);
            if (r.frame < 0) return Message{1, cmd::Nak, {cmd::NakUnknownCommand}};
            setState(r.frame, 0, true);
            break;
        case cmd::Preroll:
            if (in_ < 0) return ack;
            r.kind = TransportRequest::Locate, r.frame = std::max<int64_t>(0, in_ - preroll_);
            setState(r.frame, 0, true);
            break;
        case cmd::StandbyOn:
        case cmd::StandbyOff:
        case cmd::EditOff:
            return ack;
        default:
            // (Montage plays out; it does not record, eject or edit as a deck)
            return unknown;
        }
        lock.unlock();
        request(r);
        return ack;
    }
    case 4:
        switch (m.cmd) {
        case cmd::InDataPreset:
        case cmd::OutDataPreset:
        case cmd::PrerollTimePreset: {
            if (m.data.size() < 4) return unknown;
            // (In and Out by their labels, counted as the Program counts; the pre-roll a length, counted straight)
            const int64_t f = timecodeFromBytesAs(m.data.data(), base_, m.cmd != cmd::PrerollTimePreset && dropFrame_);
            if (f < 0) return unknown;
            (m.cmd == cmd::InDataPreset ? in_ : m.cmd == cmd::OutDataPreset ? out_ : preroll_) = f;
            return ack;
        }
        case cmd::InEntry: in_ = here; return ack;
        case cmd::OutEntry: out_ = here; return ack;
        case cmd::InReset: in_ = -1; return ack;
        case cmd::OutReset: out_ = -1; return ack;
        case cmd::EditPreset: channels_ = EditChannels::from(m.data); return ack;
        case cmd::TimecodePreset: return ack;
        default: return unknown;
        }
    case 6:
        switch (m.cmd) {
        case cmd::CurrentTimeSense: {
            const uint8_t ask = m.data.empty() ? cmd::SenseLtc : m.data[0];
            if (ask & cmd::SenseTimer1) return tc(cmd::Timer1Data, here);
            if (ask & cmd::SenseTimer2) return tc(cmd::Timer2Data, here);
            // (the Program's timecode stands for both LTC and VITC; with both asked, LTC's is given, as on tape at speed)
            if (ask & cmd::SenseLtc) return tc(cmd::LtcTimeData, here);
            if (ask & cmd::SenseVitc) return tc(cmd::VitcTimeData, here);
            if (ask & (cmd::SenseLtcUserBits | cmd::SenseVitcUserBits)) return Message{7, ask & cmd::SenseLtcUserBits ? cmd::LtcUserBitsData : cmd::VitcUserBitsData, {0, 0, 0, 0}};
            return tc(cmd::LtcTimeData, here);
        }
        case cmd::StatusSense: {
            const int first = m.data.empty() ? 0 : m.data[0] >> 4, count = m.data.empty() ? 10 : std::max(1, m.data[0] & 0x0F);
            Status st;
            st.play = s.speed == 1;
            st.stop = s.speed == 0;
            st.still = s.speed == 0;
            st.cueComplete = s.speed == 0 && !s.cueing;
            st.reverse = s.speed < 0;
            st.shuttle = s.speed != 0 && s.speed != 1;
            st.fastForward = s.speed >= 10;
            st.rewind = s.speed <= -10;
            st.servoLock = true;
            st.inSet = in_ >= 0;
            st.outSet = out_ >= 0;
            const auto all = st.bytes();
            std::vector<uint8_t> d;
            for (int i = first; i < std::min(10, first + count); ++i) d.push_back(all[size_t(i)]);
            return Message{7, cmd::StatusData, d};
        }
        case cmd::InDataSense: return tc(cmd::InData, std::max<int64_t>(in_, 0));
        case cmd::OutDataSense: return tc(cmd::OutData, std::max<int64_t>(out_, 0));
        case cmd::PrerollTimeSense: return tc(cmd::PrerollTimeData, preroll_);
        case cmd::EditPresetSense: return Message{7, cmd::EditPresetData, channels_.data()};
        case cmd::SpeedSense: {
            auto d = speedData(s.speed);
            return Message{7, cmd::SpeedData, d};
        }
        default: return unknown;
        }
    default: return unknown;
    }
}

void DeckEmulator::run() {
    Parser parser;
    uint8_t buf[256];
    while (!stop_) {
        // (sooner while a message is short, so one left so by a stray byte is passed over in time)
        const int n = port_.read(buf, sizeof buf, parser.pending() ? 10 : 50);
        if (n < 0) {
            // The port gone (an adapter pulled out): no longer a deck.
            failed_ = true;
            break;
        }
        parser.feed(buf, size_t(n));
        Message m;
        for (Parser::Result r; (r = parser.next(m)) != Parser::None;) {
            const Message reply = r == Parser::Ok ? answer(m) : Message{1, cmd::Nak, {cmd::NakChecksum}};
            port_.write(encode(reply));
            ++answered_;
        }
    }
}

}  // namespace montage::ninepin
