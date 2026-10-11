# Mackie Control (MCU) host-side protocol: implementation notes

Checked in October 2026. Mackie never published an MCU MIDI specification; the original is Emagic's Logic Control MIDI implementation (chapter 13 of `LogicControl_en.pdf`), which could not be fetched. The facts below are cross-checked against open host code and reverse-engineered captures. Where no source is named, all of them agree.

- [A] Ardour `libs/surfaces/mackie` — https://github.com/Ardour/ardour/tree/master/libs/surfaces/mackie
- [R] Reaper `csurf_mcu.cpp` — https://github.com/justinfrankel/reaper-sdk/blob/main/reaper-plugins/reaper_csurf/csurf_mcu.cpp
- [T] Tracktion `tracktion_MackieMCU.cpp` — https://github.com/Tracktion/tracktion_engine/blob/develop/modules/tracktion_engine/control_surfaces/types/tracktion_MackieMCU.cpp
- [M] TouchMCU, reverse-engineered on an MCU Pro (v3 firmware) — https://github.com/NicoG60/TouchMCU/blob/main/doc/mackie_control_protocol.md
- [L] libMackieControl — https://github.com/Do-sth-sharp/libMackieControl/blob/main/doc/MackieControl.md
- [D] a capture from a real MCU — https://forum.pdpatchrepo.info/uploads/files/1645596025977-mackie-control-midi-table.pdf

## Faders
- **Position.** Pitch bend `En LL MM`: n 0–7 are the strips, 8 the master. 14-bit, 0 at the bottom, 0x3FFF at the top. The same message goes both ways: host to surface drives the motor, surface to host reports a move.
- **Resolution.** Real resolution is 10 bits ([D] saw a maximum of 0x3FF0). [T] treats 0x3F70 and above as the top.
- **Echo.** The servo is closed-loop: the host re-sends every position it receives, or the fader springs back ([A], quoting Mackie, and [T]).
- **Touch.** Notes 0x68–0x6F for the strips and 0x70 for the master. 0x7F is touched, 0x00 released; [A] treats anything over 64 as a press.
- **Never move a fader under a hand.** [R] treats a fader without touch sensing as held for 3 s after it last moved.

## V-Pots and the jog wheel
- **Rotation.** CC 0x10–0x17. Bit 6 is the direction (set = anticlockwise) and bits 0–5 count the steps; [A] reads a count of 0 as 1.
- **Push.** Notes 0x20–0x27.
- **LED ring.** CC 0x30–0x37 with the value `0 C M M V V V V`:
  - C is the centre LED.
  - MM is the mode: 0 a single dot, 1 boost/cut, 2 wrap, 3 spread.
  - VVVV is the position: 0 off, 1–11 a position (6 is the middle).
- **Jog wheel.** CC 0x3C, encoded the same way as a V-Pot.

## Buttons (notes on channel 1; press 0x7F, release 0x00; LEDs 0x7F on, 0x01 flash, 0x00 off)
| Notes | Buttons |
|---|---|
| 00–07 | Rec/Arm |
| 08–0F | Solo |
| 10–17 | Mute |
| 18–1F | Select |
| 20–27 | V-Pot push |
| 28–2D | Track, Send, Pan, Plug-in, EQ, Instrument |
| 2E/2F | Bank left/right |
| 30/31 | Channel left/right |
| 32/33 | Flip / Global view |
| 34/35 | Name/Value / SMPTE-Beats |
| 36–3D | F1–F8 |
| 3E–45 | View buttons |
| 46–49 | Shift, Option, Control, Alt |
| 4A–4F | Read, Write, Trim, Touch, Latch, Group |
| 50–53 | Save, Undo, Cancel, Enter |
| 54–5A | Marker, Nudge, Cycle, Drop, Replace, Click, Solo |
| 5B–5F | Rewind, Fast forward, Stop, Play, Record |
| 60–65 | Up, Down, Left, Right, Zoom, Scrub |
| 66/67 | User switches |
| 68–70 | Fader touch |

LEDs 71 and 72 are SMPTE and BEATS, 73 is Rude Solo. The Relay LED is given as 74 by [A] and [L], and as 76 by [M].

## Displays and meters
- **Scribble strips.** `F0 00 00 66 dd 12 oo <ASCII> F7`, where dd is 0x14 for an MCU, 0x15 an extender, 0x10/0x11 a Logic Control. The offset is 0x00–0x37 for the top row and 0x38–0x6F for the bottom. Each strip has 7 characters (6 and a space). Send ASCII 0x20–0x7E only.
- **Meters.**
  - Level: `D0 sv` (strip s, level v). 0–0xC are the steps, 0xD is over 0 dB, 0xE sets overload and 0xF clears it.
  - Meter mode: `F0 00 00 66 14 20 ss mm F7`, where bit 0 is the signal LED and bit 1 peak hold. Hosts send 0x03 or 0x07.
  - The surface lets meters fall by itself, so hosts resend while a level holds ([A] every 100 ms).
  - Each host chooses its own dB per step; Montage uses [M]'s table: 1 from −60 dB, then −50, −40, −30, −20, −14, −10, −8, −6, −4, −2, and 0xC at 0 dB.
- **Timecode.** CC 0x40–0x49, where 0x40 is the rightmost digit, grouped 3-2-2-3. Characters:
  - ASCII 0x40–0x5F minus 0x40;
  - 0x20–0x3F as they are;
  - plus 0x40 to light the dot.
- **Assignment display.** CC 0x4B is the left character and 0x4A the right.

## Connecting
- **Device query.** `F0 00 00 66 14 00 F7`. A surface answers, and also announces itself at power-on, with `… 14 01 <serial 7> <challenge 4> F7`. In MCU mode no challenge response is needed: [A], [R] and [T] treat the 0x01 message as "ready" and send everything again.
- **Waiting for the host.** Some devices (MCU Pro, X-Touch) wait for the query; others accept any input. So send the query, and also treat any input as the surface being there.
- **Device id.** Copy the device id the surface uses (Qcon extenders reply as 0x14).
- **Connect and disconnect.** On both, zero the faders, rings, meters, LEDs and displays.

## Fader law
There is no standard. [A] uses `pos = ((6·log2(g·2/max) + 192)/198)^8` with +6 dB at the top. Montage keeps its mixer's range (−60 to +12 dB) with 0 dB three quarters of the way up.
