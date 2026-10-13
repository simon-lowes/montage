// Montage — the Blackmagic Speed Editor and DaVinci Resolve Editor Keyboard: their HID protocol, from Sylvain Munaut's
// blackmagic-misc (Apache-2.0) and CommandPost's hs.blackmagic (MIT). The software answers the device's challenge on
// feature report 6 (until it does, the device sends nothing) and again before the time it gives runs out; keys held
// come as input report 4 (up to six key codes), the jog wheel as report 3 (its mode and a value), the battery as
// report 7; key lights are output report 2 (a bit each), the jog mode lights output report 4, the jog mode output
// report 3.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace montage::speed {

constexpr uint16_t kVendor = 0x1edb;
constexpr uint16_t kSpeedEditor = 0xda0e;
constexpr uint16_t kEditorKeyboard = 0xda0b;

// The key codes both devices send (the Editor Keyboard's extra keys too).
enum class Key : uint16_t {
    None = 0x00,
    SmartInsert = 0x01, Append = 0x02, RippleOverwrite = 0x03, CloseUp = 0x04, PlaceOnTop = 0x05, SourceOverwrite = 0x06,
    In = 0x07, Out = 0x08, TrimIn = 0x09, TrimOut = 0x0a, Roll = 0x0b, SlipSource = 0x0c, SlipDest = 0x0d,
    TransitionDuration = 0x0e, Cut = 0x0f, Dissolve = 0x10, SmoothCut = 0x11,
    DoubleZero = 0x12, Timecode = 0x13, Camera = 0x14, DateTime = 0x15, ClipName = 0x16, TrimEditor = 0x17,
    FrameTimecode = 0x18, DurationEnter = 0x19,
    Source = 0x1a, Timeline = 0x1b, Shuttle = 0x1c, Jog = 0x1d, Scroll = 0x1e,
    SyncBin = 0x1f, InsertBlack = 0x20, Freeze = 0x21, Transition = 0x22, PictureInPicture = 0x23, Swap = 0x24,
    VideoOnly = 0x25, AudioOnly = 0x26, Insert = 0x27, Overwrite = 0x28, Replace = 0x29, FitToFill = 0x2a,
    RippleDelete = 0x2b, AudioLevel = 0x2c, FullView = 0x2d, Snap = 0x2e, Split = 0x2f, LiveOverwrite = 0x30,
    Escape = 0x31,
    Cam1 = 0x33, Cam2 = 0x34, Cam3 = 0x35, Cam4 = 0x36, Cam5 = 0x37, Cam6 = 0x38, Cam7 = 0x39, Cam8 = 0x3a, Cam9 = 0x3b,
    StopPlay = 0x3c,
};
// Every key, in the order the devices' panels read (Speed Editor first, then the Editor Keyboard's own).
const std::vector<Key>& allKeys();
// As printed on the key ("SMART INSRT", "CAM 1"); the secondary function (pressed twice) in brackets, if any.
std::string keyLabel(Key k);
std::string keySecondLabel(Key k);
// A stable name for settings ("smart_insert", "cam1"); keyFromName is its inverse (false for none).
std::string keyName(Key k);
bool keyFromName(const std::string& name, Key& out);
// Whether the Speed Editor has the key (the Editor Keyboard lacks the CAM keys, LIVE O/WR, ESC, AUDIO LEVEL, FULL
// VIEW, SPLIT, SNAP and STOP/PLAY, which its ordinary keys stand in for, and has its own row of extras).
bool onSpeedEditor(Key k);

// The jog wheel's modes: Relative (0 and 2 alike: how far it turned since the last report, 360 a step), Absolute
// (from where it was when the mode was set: -4096 to 4096, half a turn), AbsoluteDeadZero (the same with a small band
// at 0 that reads 0, a mechanical stop on the Editor Keyboard).
enum class JogMode : uint8_t { Relative = 0, Absolute = 1, Relative2 = 2, AbsoluteDeadZero = 3 };
constexpr int kJogStep = 360;
constexpr int kJogHalfTurn = 4096;

// The key lights (Speed Editor only): the bit of each key with one, 0 for none.
uint32_t ledBit(Key k);
// The jog wheel's mode lights (both devices): JOG, SHTL and SCRL.
uint8_t jogLedBit(Key k);

// Output and feature reports to send (the report id first).
std::vector<uint8_t> ledReport(uint32_t leds);
std::vector<uint8_t> jogLedReport(uint8_t leds);
std::vector<uint8_t> jogModeReport(JogMode mode);

// What the device said.
struct Event {
    enum Kind { None, KeyDown, KeyUp, Jog, Battery } kind = None;
    Key key = Key::None;
    JogMode mode = JogMode::Relative;
    int32_t value = 0;      // Jog
    bool charging = false;  // Battery
    int level = -1;         // Battery: 0-100
};
// Turns input reports into events: keys pressed and released (from the set held, which report 4 repeats whole on
// every change), jog moves and the battery. Malformed or unknown reports give nothing.
class Decoder {
public:
    std::vector<Event> feed(const uint8_t* report, size_t n);
    const std::vector<Key>& held() const { return held_; }
    void reset() { held_.clear(); }

private:
    std::vector<Key> held_;
};

// The response to the device's challenge (both are 64-bit, sent little-endian).
uint64_t authResponse(uint64_t challenge);

// Authenticates through feature report 6: `send` sets a feature report, `get` reads one (the id given; filled with
// the report, its id first). Returns the seconds until it must be done again (the device says, about 600), 0 if it
// failed (with the reason).
int authenticate(const std::function<bool(const std::vector<uint8_t>&)>& send,
                 const std::function<bool(uint8_t id, std::vector<uint8_t>&)>& get, std::string* error = nullptr);

// Linux: a hidraw device's uevent ("HID_ID=0003:00001EDB:0000DA0E", HID_NAME, HID_UNIQ) read for its bus, vendor,
// product, name and serial. False if it has no HID_ID.
bool parseHidUevent(const std::string& text, int& bus, uint16_t& vendor, uint16_t& product, std::string& name, std::string& serial);

}  // namespace montage::speed
