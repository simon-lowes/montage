#include "SpeedEditor.h"

#include <algorithm>
#include <cstdio>
#include <sstream>

namespace montage::speed {

namespace {
struct KeyInfo {
    Key key;
    const char* name;
    const char* label;
    const char* second;
    uint32_t led;
    uint8_t jogLed;
    bool speedEditor;
};
// Key lights (report 2), from blackmagic-misc.
constexpr uint32_t L(int bit) { return 1u << bit; }
const KeyInfo kKeys[] = {
    {Key::SmartInsert, "smart_insert", "SMART INSRT", "CLIP", 0, 0, true},
    {Key::Append, "append", "APPND", "CLIP", 0, 0, true},
    {Key::RippleOverwrite, "ripple_overwrite", "RIPL O/WR", "", 0, 0, true},
    {Key::CloseUp, "close_up", "CLOSE UP", "YPOS", L(0), 0, true},
    {Key::PlaceOnTop, "place_on_top", "PLACE ON TOP", "", 0, 0, true},
    {Key::SourceOverwrite, "source_overwrite", "SRC O/WR", "", 0, 0, true},
    {Key::In, "in", "IN", "CLR", 0, 0, true},
    {Key::Out, "out", "OUT", "CLR", 0, 0, true},
    {Key::TrimIn, "trim_in", "TRIM IN", "", 0, 0, true},
    {Key::TrimOut, "trim_out", "TRIM OUT", "", 0, 0, true},
    {Key::Roll, "roll", "ROLL", "SLIDE", 0, 0, true},
    {Key::SlipSource, "slip_source", "SLIP SRC", "", 0, 0, true},
    {Key::SlipDest, "slip_dest", "SLIP DEST", "", 0, 0, true},
    {Key::TransitionDuration, "transition_duration", "TRANS DUR", "SET", 0, 0, true},
    {Key::Cut, "cut", "CUT", "", L(1), 0, true},
    {Key::Dissolve, "dissolve", "DIS", "", L(2), 0, true},
    {Key::SmoothCut, "smooth_cut", "SMTH CUT", "", L(3), 0, true},
    {Key::Escape, "escape", "ESC", "UNDO", 0, 0, true},
    {Key::SyncBin, "sync_bin", "SYNC BIN", "", 0, 0, true},
    {Key::AudioLevel, "audio_level", "AUDIO LEVEL", "MARK", 0, 0, true},
    {Key::FullView, "full_view", "FULL VIEW", "RVW", 0, 0, true},
    {Key::Transition, "transition", "TRANS", "TITLE", L(4), 0, true},
    {Key::Split, "split", "SPLIT", "MOVE", 0, 0, true},
    {Key::Snap, "snap", "SNAP", "=", L(5), 0, true},
    {Key::RippleDelete, "ripple_delete", "RIPL DEL", "", 0, 0, true},
    {Key::Cam1, "cam1", "CAM 1", "", L(14), 0, true},
    {Key::Cam2, "cam2", "CAM 2", "", L(15), 0, true},
    {Key::Cam3, "cam3", "CAM 3", "", L(16), 0, true},
    {Key::Cam4, "cam4", "CAM 4", "", L(10), 0, true},
    {Key::Cam5, "cam5", "CAM 5", "", L(11), 0, true},
    {Key::Cam6, "cam6", "CAM 6", "", L(12), 0, true},
    {Key::Cam7, "cam7", "CAM 7", "", L(6), 0, true},
    {Key::Cam8, "cam8", "CAM 8", "", L(7), 0, true},
    {Key::Cam9, "cam9", "CAM 9", "", L(8), 0, true},
    {Key::LiveOverwrite, "live_overwrite", "LIVE O/WR", "RND", L(9), 0, true},
    {Key::VideoOnly, "video_only", "VIDEO ONLY", "", L(13), 0, true},
    {Key::AudioOnly, "audio_only", "AUDIO ONLY", "", L(17), 0, true},
    {Key::StopPlay, "stop_play", "STOP/PLAY", "", 0, 0, true},
    {Key::Source, "source", "SOURCE", "", 0, 0, true},
    {Key::Timeline, "timeline", "TIMELINE", "", 0, 0, true},
    {Key::Shuttle, "shuttle", "SHTL", "", 0, 1u << 1, true},
    {Key::Jog, "jog", "JOG", "", 0, 1u << 0, true},
    {Key::Scroll, "scroll", "SCRL", "", 0, 1u << 2, true},
    // The Editor Keyboard's own.
    {Key::InsertBlack, "insert_black", "INSRT BLACK", "", 0, 0, false},
    {Key::Freeze, "freeze", "FREEZ", "", 0, 0, false},
    {Key::PictureInPicture, "picture_in_picture", "PIC IN PIC", "", 0, 0, false},
    {Key::Swap, "swap", "SWAP", "", 0, 0, false},
    {Key::Insert, "insert", "INSERT", "", 0, 0, false},
    {Key::Overwrite, "overwrite", "O/WR", "", 0, 0, false},
    {Key::Replace, "replace", "REPL", "", 0, 0, false},
    {Key::FitToFill, "fit_to_fill", "FIT TO FILL", "", 0, 0, false},
    {Key::Timecode, "timecode", "TIME CODE", "", 0, 0, false},
    {Key::Camera, "camera", "CAM", "", 0, 0, false},
    {Key::DateTime, "date_time", "DATE TIME", "", 0, 0, false},
    {Key::ClipName, "clip_name", "CLIP NAME", "", 0, 0, false},
    {Key::TrimEditor, "trim_editor", "TRIM EDTR", "", 0, 0, false},
    {Key::FrameTimecode, "frame_timecode", "F/TC", "", 0, 0, false},
    {Key::DurationEnter, "duration_enter", "DUR ENTER", "", 0, 0, false},
    {Key::DoubleZero, "double_zero", "00", "", 0, 0, false},
};

const KeyInfo* info(Key k) {
    for (const KeyInfo& i : kKeys)
        if (i.key == k) return &i;
    return nullptr;
}

uint64_t rol8(uint64_t v) { return (v << 56) | (v >> 8); }

uint64_t readLe64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}
}  // namespace

const std::vector<Key>& allKeys() {
    static const std::vector<Key> keys = [] {
        std::vector<Key> out;
        for (const KeyInfo& i : kKeys) out.push_back(i.key);
        return out;
    }();
    return keys;
}

std::string keyLabel(Key k) {
    const KeyInfo* i = info(k);
    return i ? i->label : std::string();
}

std::string keySecondLabel(Key k) {
    const KeyInfo* i = info(k);
    return i ? i->second : std::string();
}

std::string keyName(Key k) {
    const KeyInfo* i = info(k);
    return i ? i->name : std::string();
}

bool keyFromName(const std::string& name, Key& out) {
    for (const KeyInfo& i : kKeys)
        if (name == i.name) {
            out = i.key;
            return true;
        }
    return false;
}

bool onSpeedEditor(Key k) {
    const KeyInfo* i = info(k);
    return i && i->speedEditor;
}

uint32_t ledBit(Key k) {
    const KeyInfo* i = info(k);
    return i ? i->led : 0;
}

uint8_t jogLedBit(Key k) {
    const KeyInfo* i = info(k);
    return i ? i->jogLed : 0;
}

std::vector<uint8_t> ledReport(uint32_t leds) {
    return {2, uint8_t(leds), uint8_t(leds >> 8), uint8_t(leds >> 16), uint8_t(leds >> 24)};
}

std::vector<uint8_t> jogLedReport(uint8_t leds) { return {4, leds}; }

std::vector<uint8_t> jogModeReport(JogMode mode) { return {3, uint8_t(mode), 0, 0, 0, 0, 255}; }

std::vector<Event> Decoder::feed(const uint8_t* r, size_t n) {
    std::vector<Event> out;
    if (n == 0) return out;
    if (r[0] == 3 && n >= 6) {
        Event e;
        e.kind = Event::Jog;
        e.mode = JogMode(r[1] & 3);
        e.value = int32_t(uint32_t(r[2]) | uint32_t(r[3]) << 8 | uint32_t(r[4]) << 16 | uint32_t(r[5]) << 24);
        out.push_back(e);
    } else if (r[0] == 4 && n >= 13) {
        std::vector<Key> now;
        for (size_t i = 0; i < 6; ++i) {
            const Key k = Key(uint16_t(r[1 + i * 2] | r[2 + i * 2] << 8));
            if (k != Key::None && info(k) && std::find(now.begin(), now.end(), k) == now.end()) now.push_back(k);
        }
        // Released first, then pressed, each in the order the device lists them.
        for (Key k : held_)
            if (std::find(now.begin(), now.end(), k) == now.end()) {
                Event e;
                e.kind = Event::KeyUp;
                e.key = k;
                out.push_back(e);
            }
        for (Key k : now)
            if (std::find(held_.begin(), held_.end(), k) == held_.end()) {
                Event e;
                e.kind = Event::KeyDown;
                e.key = k;
                out.push_back(e);
            }
        held_ = std::move(now);
    } else if (r[0] == 7 && n >= 3) {
        Event e;
        e.kind = Event::Battery;
        e.charging = r[1] != 0;
        e.level = std::clamp(int(r[2]), 0, 100);
        out.push_back(e);
    }
    return out;
}

uint64_t authResponse(uint64_t challenge) {
    static const uint64_t even[] = {0x3ae1206f97c10bc8, 0x2a9ab32bebf244c6, 0x20a6f8b8df9adf0a, 0xaf80ece52cfc1719,
                                    0xec2ee2f7414fd151, 0xb055adfd73344a15, 0xa63d2e3059001187, 0x751bf623f42e0dde};
    static const uint64_t odd[] = {0x3e22b34f502e7fde, 0x24656b981875ab1c, 0xa17f3456df7bf8c3, 0x6df72e1941aef698,
                                   0x72226f011e66ab94, 0x3831a3c606296b42, 0xfd7ff81881332c89, 0x61a3f6474ff236c6};
    const uint64_t mask = 0xa79a63f585d37bf0;
    const int n = int(challenge & 7);
    uint64_t v = challenge;
    for (int i = 0; i < n; ++i) v = rol8(v);
    uint64_t k;
    if ((v & 1) == ((0x78u >> n) & 1)) {
        k = even[n];
    } else {
        v ^= rol8(v);
        k = odd[n];
    }
    return v ^ (rol8(v) & mask) ^ k;
}

int authenticate(const std::function<bool(const std::vector<uint8_t>&)>& send,
                 const std::function<bool(uint8_t, std::vector<uint8_t>&)>& get, std::string* error) {
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return 0;
    };
    std::vector<uint8_t> r;
    auto step = [&](uint8_t expect) { return get(6, r) && r.size() >= 10 && r[0] == 6 && r[1] == expect; };
    // Its state machine reset, then its challenge read.
    if (!send({6, 0, 0, 0, 0, 0, 0, 0, 0, 0})) return fail("The device did not take the authentication request");
    if (!step(0)) return fail("The device sent no challenge");
    const uint64_t challenge = readLe64(r.data() + 2);
    // Ours (the device's answer is not checked: nothing here depends on it being genuine).
    if (!send({6, 1, 0, 0, 0, 0, 0, 0, 0, 0}) || !step(2)) return fail("The device did not answer its challenge");
    const uint64_t answer = authResponse(challenge);
    std::vector<uint8_t> reply = {6, 3};
    for (int i = 0; i < 8; ++i) reply.push_back(uint8_t(answer >> (8 * i)));
    if (!send(reply)) return fail("The device did not take the response");
    if (!step(4)) return fail("The device refused the response");
    const int seconds = int(r[2] | r[3] << 8 | r[4] << 16);
    return seconds > 0 ? seconds : fail("The device gave no time until it must authenticate again");
}

bool parseHidUevent(const std::string& text, int& bus, uint16_t& vendor, uint16_t& product, std::string& name, std::string& serial) {
    std::istringstream in(text);
    std::string line;
    bool found = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "HID_ID") {
            unsigned b = 0, v = 0, p = 0;
            if (std::sscanf(value.c_str(), "%x:%x:%x", &b, &v, &p) == 3) {
                bus = int(b);
                vendor = uint16_t(v);
                product = uint16_t(p);
                found = true;
            }
        } else if (key == "HID_NAME") {
            name = value;
        } else if (key == "HID_UNIQ") {
            serial = value;
        }
    }
    return found;
}

}  // namespace montage::speed
