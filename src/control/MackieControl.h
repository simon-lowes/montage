// Montage — the Mackie Control protocol (MCU), as Logic, Pro Tools, Cubase, Ardour and Resolve speak it to hardware
// surfaces: Mackie MCU Pro, Behringer X-Touch, iCON Platform M+, PreSonus FaderPort 8/16 and SSL UF8 in MCU mode, and
// the like. Eight channel strips (a touch-sensitive motor fader, a V-Pot encoder with an LED ring, Rec, Solo, Mute
// and Select buttons, a scribble strip and a meter) and a master fader, transport and jog wheel. This file only turns
// MIDI messages into events and states into messages; app/ControlSurface maps them onto the mixer and transport.
#pragma once

#include <string>
#include <vector>

#include "control/Midi.h"

namespace montage::mcu {

constexpr int kStrips = 8;
constexpr int kMasterChannel = 8;  // the master fader's pitch-bend channel
constexpr int kLcdWidth = 56;      // characters per row of the scribble strips (7 per strip)

// Button notes (pressed: velocity 127, released: 0) and the LEDs of the same notes.
enum Note : int {
    RecArm = 0x00,    // + strip
    Solo = 0x08,      // + strip
    Mute = 0x10,      // + strip
    Select = 0x18,    // + strip
    VPotPush = 0x20,  // + strip
    AssignTrack = 0x28,
    AssignSend = 0x29,
    AssignPan = 0x2A,
    AssignPlugin = 0x2B,
    AssignEq = 0x2C,
    AssignInstrument = 0x2D,
    BankLeft = 0x2E,
    BankRight = 0x2F,
    ChannelLeft = 0x30,
    ChannelRight = 0x31,
    Flip = 0x32,
    GlobalView = 0x33,
    NameValue = 0x34,
    SmpteBeats = 0x35,
    F1 = 0x36,  // to F8 = 0x3D
    Shift = 0x46,
    Option = 0x47,
    Control = 0x48,
    Alt = 0x49,
    Read = 0x4A,
    Write = 0x4B,
    Trim = 0x4C,
    Touch = 0x4D,
    Latch = 0x4E,
    Group = 0x4F,
    Save = 0x50,
    Undo = 0x51,
    Cancel = 0x52,
    Enter = 0x53,
    Marker = 0x54,
    Nudge = 0x55,
    Cycle = 0x56,
    Drop = 0x57,
    Replace = 0x58,
    Click = 0x59,
    SoloGlobal = 0x5A,
    Rewind = 0x5B,
    FastForward = 0x5C,
    Stop = 0x5D,
    Play = 0x5E,
    Record = 0x5F,
    Up = 0x60,
    Down = 0x61,
    Left = 0x62,
    Right = 0x63,
    Zoom = 0x64,
    Scrub = 0x65,
    FaderTouch = 0x68,   // + strip; the master's is 0x70
    MasterTouch = 0x70,
    SmpteLed = 0x71,
    BeatsLed = 0x72,
};

struct Event {
    enum Kind { None, Fader, Touch, Button, VPot, Jog } kind = None;
    int index = 0;      // Fader, Touch and VPot: the strip (0-7, kMasterChannel for the master); Button: its note
    int value = 0;      // Fader: 0-16383; VPot and Jog: steps turned, clockwise positive
    bool down = false;  // Touch and Button: pressed
};
// What a message from the surface means (kind None for anything else).
Event decode(const MidiMessage& m);

// Messages to the surface.
MidiMessage deviceQuery(int device = 0x14);      // asks a surface to say it is there (it answers with message 0x01)
MidiMessage meterMode(int strip, int mode, int device = 0x14);  // bit 0 the signal LED, bit 1 peak hold, bit 2 the LCD meter
MidiMessage fader(int channel, int value);       // moves a motor fader (0-16383)
MidiMessage led(int note, int state);            // 0 off, 1 flashing, 2 on
MidiMessage ring(int strip, int value);          // a V-Pot's LED ring, raw (see panRing)
MidiMessage lcd(int offset, const std::string& text, int device = 0x14);  // scribble strip text from offset (0-111)
MidiMessage meter(int strip, int level);         // a strip's meter, 0-13 (the surface lets it fall by itself)
// The ten-digit timecode display, from text right-aligned in it ("01:00:00:00" style: colons light the dots).
std::vector<MidiMessage> timecode(const std::string& text);
// The two-character assignment display.
std::vector<MidiMessage> assignment(const std::string& text);
// A system exclusive message from a surface: its device id (0x14 a Mackie Control, 0x15 an extender, 0x10 and 0x11
// Logic Control) and command byte; false if it is not a Mackie Control message.
bool surfaceSysex(const MidiMessage& m, int& device, int& command);

// The fader law: 0 dB three quarters of the way up, +12 dB at the top, -60 dB (silence) at the bottom. Faders resolve
// ten bits, so the top of their travel reads 0x3F70 or a little more: anything from there up is the top.
int faderValue(double db);
double faderDb(int value);
// The ring for a pan (-1 left to 1 right): one lit dot (single-dot mode), the middle of eleven for centre.
int panRing(double pan);
// A meter level from a linear peak (1 = 0 dBFS): 12 at 0 dB, 13 over it.
int meterLevel(double peak);
// A name fitted to a strip's 7 characters (6 and a space, as hosts do), plain ASCII.
std::string stripText(const std::string& name);

}  // namespace montage::mcu
