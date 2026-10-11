// Montage — the HUI protocol (Mackie's Human User Interface, as Pro Tools speaks it to hardware), which many surfaces
// offer as their other mode besides Mackie Control: the Mackie HUI itself, Behringer X-Touch, iCON Platform M+,
// PreSonus FaderPort, Avid and SSL surfaces in HUI mode. Eight strips (a touch-sensitive motor fader, a V-Pot with an
// LED ring, Select, Mute, Solo and Rec buttons, a four-character display and a stereo meter), a 2x40 display, an
// eight-digit timecode display, transport, jog wheel and keys. Switches and their lights are ports of zones, sent as
// a zone then a port; faders as their high then low seven bits; and the host pings once a second, or the faders stop.
// Like control/MackieControl.h this only turns messages into events (in Mackie Control's terms, so app/ControlSurface
// drives both protocols the same way) and states into messages.
#pragma once

#include <array>
#include <string>
#include <vector>

#include "control/MackieControl.h"

namespace montage::hui {

constexpr int kStrips = 8;
constexpr int kStripChars = 4;     // each strip's display
constexpr int kDisplayWidth = 40;  // each of the main display's two rows: four zones of ten characters
constexpr int kZoneChars = 10;

// Buttons a HUI has that Mackie Control's notes do not name (Event::Button indices above any MIDI note).
enum Extra : int {
    ReturnToZero = 0x100,  // |< RTZ
    GoToEnd = 0x101,       // END >|
    AutomationOff = 0x102,  // the automation mode keys' OFF
};

// A switch and its light.
struct Port {
    int zone = -1, port = 0;  // zone -1: none
};
// Where a Mackie Control note's (or an Extra's) button and light are on a HUI (zone -1 if it has none), and which note
// a zone and port's button is (-1 none; a strip's fader touch is an Event::Touch, not a note).
Port portOf(int note);
int noteOf(int zone, int port);

// The surface's messages as events: a switch comes as its zone then its port, a fader as its high then its low seven
// bits, so what came before is kept.
class Decoder {
public:
    mcu::Event decode(const MidiMessage& m);
    void reset();

private:
    int zone_ = -1;
    std::array<int, kStrips> high_{};
};
bool isPingReply(const MidiMessage& m);  // the surface answering a ping (it is there)
bool isSystemReset(const MidiMessage& m);  // a surface switched on or off sends these

// Messages to the surface.
MidiMessage ping();
std::vector<MidiMessage> fader(int strip, int value);         // moves a motor fader (0-16383)
std::vector<MidiMessage> led(int zone, int port, bool on);    // a light (zone select, then the port)
std::vector<MidiMessage> zoneOff(int zone);                   // every light of a zone out
MidiMessage ring(int strip, int value);  // a V-Pot's ring: 1-11 one lit dot (6 the middle; mcu::panRing fits), 0 dark
MidiMessage meter(int strip, int side, int level);  // side 0 left, 1 right; level 0-12 (mcu::meterLevel, 13 as 12)
MidiMessage stripText(int strip, const std::string& text);    // the four characters (strip 8: the Select-Assign display)
MidiMessage display(int zone, const std::string& text);       // ten characters of the main display (zones 0-3 the top row)
// The timecode display, from text such as "01:00:00:00" (the last eight digits, a point after hours, minutes and
// seconds); nothing for text with no digits.
std::vector<MidiMessage> timecode(const std::string& text);
// Text for the displays, plain ASCII the surface can show, cut or padded to `width`.
std::string fit(const std::string& text, int width, bool mainDisplay = false);

}  // namespace montage::hui
