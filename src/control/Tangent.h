// Montage — Tangent's colour panels (Elements, Ripple, Wave 2, Arc) through Tangent Hub: the TUBE protocol (TIPC
// revision 4 and later) the Hub speaks to an application over TCP on this computer, as Hammerspoon's hs.tangent and
// CommandPost (both MIT) speak it. Each message is a 32-bit big-endian length then its commands, each a 32-bit
// command id and its fields: 32-bit big-endian integers, IEEE floats and booleans, and strings as a length and their
// bytes. The Hub starts (InitiateComms); the application names itself and the folder holding its controls.xml
// (ApplicationDefinition), and the Hub sends the panels' turns and presses as parameter, menu and action ids that file
// defines. This file only reads and writes the messages and the file; app/TangentControl talks to the Hub.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace montage::tangent {

constexpr uint16_t kHubPort = 64246;

// Hub to application.
enum HubCommand : uint32_t {
    InitiateComms = 0x01,
    ParameterChange = 0x02,
    ParameterReset = 0x03,
    ParameterValueRequest = 0x04,
    MenuChange = 0x05,
    MenuReset = 0x06,
    MenuStringRequest = 0x07,
    ActionOn = 0x08,
    ModeChange = 0x09,
    Transport = 0x0A,
    ActionOff = 0x0B,
    UnmanagedPanelCapabilities = 0x30,
    UnmanagedButtonDown = 0x31,
    UnmanagedButtonUp = 0x32,
    UnmanagedEncoderChange = 0x33,
    UnmanagedDisplayRefresh = 0x34,
    PanelConnectionState = 0x35,
};

// Application to Hub.
enum AppCommand : uint32_t {
    ApplicationDefinition = 0x81,
    ParameterValue = 0x82,
    MenuString = 0x83,
    AllChange = 0x84,
    ModeValue = 0x85,
    DisplayText = 0x86,
};

// A message from the Hub. `id` is the parameter, menu, action or mode; `panels` are InitiateComms' (type, id) pairs.
struct HubMessage {
    uint32_t command = 0;
    uint32_t id = 0;
    float increment = 0;     // ParameterChange
    int32_t steps = 0;       // MenuChange (+1 or -1)
    int32_t jog = 0, shuttle = 0;  // Transport
    uint32_t protocolRev = 0;      // InitiateComms
    std::vector<std::pair<uint32_t, uint32_t>> panels;
    bool known = true;  // false: a command Montage does not take (the rest of its message is skipped)
};

// Reads the Hub's stream as it arrives (messages split or run together).
class Decoder {
public:
    // The whole messages in `bytes` and what came before; false with the reason for a malformed one (the stream
    // cannot be trusted after it).
    bool feed(const std::string& bytes, std::vector<HubMessage>& out, std::string* error = nullptr);

private:
    std::string pending_;
};

// Application messages, each a whole message with its length.
std::string applicationDefinition(const std::string& app, const std::string& systemDir, const std::string& userDir);
std::string parameterValue(uint32_t id, float value, bool atDefault);
std::string menuString(uint32_t id, const std::string& text, bool atDefault);
std::string allChange();
std::string modeValue(uint32_t mode);
// Up to three lines on the panels' status displays (each at most 32 characters; ASCII).
std::string displayText(const std::vector<std::string>& lines);

// The Hub's messages, for tests and a stand-in Hub.
std::string initiateComms(uint32_t protocolRev, const std::vector<std::pair<uint32_t, uint32_t>>& panels);
std::string parameterChange(uint32_t id, float increment);
std::string idCommand(uint32_t command, uint32_t id);  // ParameterReset, ...ValueRequest, MenuReset, ...StringRequest,
                                                       // ActionOn, ActionOff, ModeChange
std::string menuChange(uint32_t id, int32_t steps);
std::string transport(int32_t jog, int32_t shuttle);

// An application message as a stand-in Hub reads it: its command, id, value, default flag and strings.
struct AppMessage {
    uint32_t command = 0;
    uint32_t id = 0;
    float value = 0;
    bool atDefault = false;
    std::vector<std::string> strings;  // ApplicationDefinition's three, MenuString's text, DisplayText's lines
};
bool decodeApp(const std::string& bytes, std::vector<AppMessage>& out, size_t* used = nullptr, std::string* error = nullptr);

// controls.xml: the controls an application offers, grouped; Bindings tie a trackball's parameters together.
struct Control {
    enum Kind { Parameter, Menu, Action } kind = Parameter;
    uint32_t id = 0;
    std::string name, shortName;  // short: at most 9 characters (the panels' small displays)
    double min = 0, max = 1, step = 0.001;  // Parameter
};
struct Binding {
    std::string name;
    std::vector<uint32_t> members;
};
struct Group {
    std::string name;
    std::vector<Control> controls;
    std::vector<Group> groups;
    std::vector<Binding> bindings;
};
struct ControlSystem {
    std::vector<std::pair<uint32_t, std::string>> modes;
    std::vector<Group> groups;
    std::vector<Control> controls;  // outside any group
    bool jog = true, shuttle = true;
    int displayLines = 3;
};
std::string controlsXml(const ControlSystem& system);
// A panel map with nothing assigned, so the Hub's own defaults apply (CommandPost ships the Element Tk's):
// "element-tk-map.xml" and its contents.
std::pair<std::string, std::string> emptyMap();

// Text as the panels take it: printable ASCII, "&" as "and", at most `max` characters.
std::string ascii(const std::string& utf8, size_t max = 32);

}  // namespace montage::tangent
