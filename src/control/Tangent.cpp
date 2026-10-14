#include "Tangent.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace montage::tangent {

namespace {

void putU32(std::string& out, uint32_t v) {
    const char b[4] = {char(v >> 24), char(v >> 16), char(v >> 8), char(v)};
    out.append(b, 4);
}
void putF32(std::string& out, float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    putU32(out, v);
}
void putString(std::string& out, const std::string& s) {
    putU32(out, uint32_t(s.size()));
    out += s;
}
// A whole message: its length, then its body.
std::string message(const std::string& body) {
    std::string out;
    putU32(out, uint32_t(body.size()));
    return out + body;
}

// Reads a message body's fields in turn; `ok` false once one runs past its end.
struct Reader {
    const std::string& b;
    size_t at = 0;
    bool ok = true;
    bool more() const { return at < b.size(); }
    uint32_t u32() {
        if (at + 4 > b.size()) {
            ok = false;
            return 0;
        }
        const auto* p = reinterpret_cast<const unsigned char*>(b.data() + at);
        at += 4;
        return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
    }
    float f32() {
        const uint32_t v = u32();
        float f;
        std::memcpy(&f, &v, 4);
        return f;
    }
    std::string str() {
        const uint32_t n = u32();
        if (!ok || n > b.size() - at) {
            ok = false;
            return {};
        }
        std::string s = b.substr(at, n);
        at += n;
        return s;
    }
};

std::string xmlEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '"') out += "&quot;";
        else out += c;
    }
    return out;
}

std::string hexId(uint32_t id) {
    char b[16];
    std::snprintf(b, sizeof b, "%#010x", id);
    return b;
}

std::string number(double v) {
    char b[48];
    std::snprintf(b, sizeof b, "%.10f", v);
    return b;
}

void writeControl(std::string& x, const Control& c, const std::string& indent) {
    static const char* tags[] = {"Parameter", "Menu", "Action"};
    const char* tag = tags[c.kind];
    x += indent + "<" + tag + " id=\"" + hexId(c.id) + "\">\n";
    x += indent + "  <Name>" + xmlEscape(ascii(c.name)) + "</Name>\n";
    if (!c.shortName.empty()) x += indent + "  <Name9>" + xmlEscape(ascii(c.shortName, 9)) + "</Name9>\n";
    if (c.kind == Control::Parameter) {
        x += indent + "  <MinValue>" + number(c.min) + "</MinValue>\n";
        x += indent + "  <MaxValue>" + number(c.max) + "</MaxValue>\n";
        x += indent + "  <StepSize>" + number(c.step) + "</StepSize>\n";
    }
    x += indent + "</" + tag + ">\n";
}

void writeGroup(std::string& x, const Group& g, const std::string& indent) {
    x += indent + "<Group name=\"" + xmlEscape(ascii(g.name)) + "\">\n";
    for (const Group& sub : g.groups) writeGroup(x, sub, indent + "  ");
    for (const Control& c : g.controls) writeControl(x, c, indent + "  ");
    for (const Binding& b : g.bindings) {
        x += indent + "  <Binding name=\"" + xmlEscape(ascii(b.name)) + "\">\n";
        for (uint32_t m : b.members) x += indent + "    <Member id=\"" + hexId(m) + "\"/>\n";
        x += indent + "  </Binding>\n";
    }
    x += indent + "</Group>\n";
}

}  // namespace

bool Decoder::feed(const std::string& bytes, std::vector<HubMessage>& out, std::string* error) {
    pending_ += bytes;
    size_t at = 0;
    while (pending_.size() - at >= 4) {
        const auto* p = reinterpret_cast<const unsigned char*>(pending_.data() + at);
        const uint32_t n = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
        if (n > (1u << 20)) {
            if (error) *error = "Tangent Hub message of " + std::to_string(n) + " bytes";
            pending_.clear();
            return false;
        }
        if (pending_.size() - at - 4 < n) break;
        const std::string body = pending_.substr(at + 4, n);
        at += 4 + n;
        Reader r{body};
        while (r.ok && r.more()) {
            HubMessage m;
            m.command = r.u32();
            switch (m.command) {
                case InitiateComms: {
                    m.protocolRev = r.u32();
                    const uint32_t count = r.u32();
                    for (uint32_t i = 0; r.ok && i < count; ++i) {
                        const uint32_t type = r.u32(), id = r.u32();
                        if (m.panels.size() < 64) m.panels.push_back({type, id});
                    }
                    break;
                }
                case ParameterChange:
                    m.id = r.u32();
                    m.increment = r.f32();
                    // (a NaN or infinity from the socket would be written into the grade)
                    if (!std::isfinite(m.increment)) m.increment = 0;
                    break;
                case MenuChange:
                    m.id = r.u32();
                    m.steps = std::clamp(int32_t(r.u32()), -1000, 1000);
                    break;
                case Transport:
                    // (clamped, so adding them up cannot overflow)
                    m.jog = std::clamp(int32_t(r.u32()), -1000, 1000);
                    m.shuttle = std::clamp(int32_t(r.u32()), -1000, 1000);
                    break;
                case ParameterReset:
                case ParameterValueRequest:
                case MenuReset:
                case MenuStringRequest:
                case ActionOn:
                case ActionOff:
                case ModeChange:
                    m.id = r.u32();
                    break;
                case UnmanagedPanelCapabilities:
                    for (int i = 0; i < 6; ++i) r.u32();
                    m.known = false;
                    break;
                case UnmanagedButtonDown:
                case UnmanagedButtonUp:
                case PanelConnectionState:
                    r.u32(), r.u32();
                    m.known = false;
                    break;
                case UnmanagedEncoderChange:
                    r.u32(), r.u32(), r.u32();
                    m.known = false;
                    break;
                case UnmanagedDisplayRefresh:
                    r.u32();
                    m.known = false;
                    break;
                default:
                    // (its fields are unknown, so the rest of the message cannot be read)
                    m.known = false;
                    r.at = body.size();
                    break;
            }
            if (!r.ok) {
                if (error) *error = "Tangent Hub message cut short";
                pending_.clear();
                return false;
            }
            out.push_back(std::move(m));
        }
    }
    pending_.erase(0, at);
    return true;
}

std::string applicationDefinition(const std::string& app, const std::string& systemDir, const std::string& userDir) {
    std::string b;
    putU32(b, ApplicationDefinition);
    putString(b, app);
    putString(b, systemDir);
    putString(b, userDir);
    return message(b);
}

std::string parameterValue(uint32_t id, float value, bool atDefault) {
    std::string b;
    putU32(b, ParameterValue);
    putU32(b, id);
    putF32(b, value);
    putU32(b, atDefault ? 1 : 0);
    return message(b);
}

std::string menuString(uint32_t id, const std::string& text, bool atDefault) {
    std::string b;
    putU32(b, MenuString);
    putU32(b, id);
    putString(b, ascii(text));
    putU32(b, atDefault ? 1 : 0);
    return message(b);
}

std::string allChange() {
    std::string b;
    putU32(b, AllChange);
    return message(b);
}

std::string modeValue(uint32_t mode) {
    std::string b;
    putU32(b, ModeValue);
    putU32(b, mode);
    return message(b);
}

std::string displayText(const std::vector<std::string>& lines) {
    std::string b;
    putU32(b, DisplayText);
    const size_t n = std::min<size_t>(lines.size(), 3);
    putU32(b, uint32_t(n));
    for (size_t i = 0; i < n; ++i) {
        const std::string s = ascii(lines[i]);
        putU32(b, uint32_t(s.size()));
        if (!s.empty()) {
            b += s;
            putU32(b, 0);  // (not double height)
        }
    }
    return message(b);
}

std::string initiateComms(uint32_t protocolRev, const std::vector<std::pair<uint32_t, uint32_t>>& panels) {
    std::string b;
    putU32(b, InitiateComms);
    putU32(b, protocolRev);
    putU32(b, uint32_t(panels.size()));
    for (const auto& [type, id] : panels) putU32(b, type), putU32(b, id);
    return message(b);
}

std::string parameterChange(uint32_t id, float increment) {
    std::string b;
    putU32(b, ParameterChange);
    putU32(b, id);
    putF32(b, increment);
    return message(b);
}

std::string idCommand(uint32_t command, uint32_t id) {
    std::string b;
    putU32(b, command);
    putU32(b, id);
    return message(b);
}

std::string menuChange(uint32_t id, int32_t steps) {
    std::string b;
    putU32(b, MenuChange);
    putU32(b, id);
    putU32(b, uint32_t(steps));
    return message(b);
}

std::string transport(int32_t jog, int32_t shuttle) {
    std::string b;
    putU32(b, Transport);
    putU32(b, uint32_t(jog));
    putU32(b, uint32_t(shuttle));
    return message(b);
}

bool decodeApp(const std::string& bytes, std::vector<AppMessage>& out, size_t* used, std::string* error) {
    size_t at = 0;
    while (bytes.size() - at >= 4) {
        Reader len{bytes, at};
        const uint32_t n = len.u32();
        if (bytes.size() - at - 4 < n) break;
        const std::string body = bytes.substr(at + 4, n);
        at += 4 + n;
        Reader r{body};
        while (r.ok && r.more()) {
            AppMessage m;
            m.command = r.u32();
            switch (m.command) {
                case ApplicationDefinition:
                    for (int i = 0; i < 3; ++i) m.strings.push_back(r.str());
                    break;
                case ParameterValue:
                    m.id = r.u32();
                    m.value = r.f32();
                    m.atDefault = r.u32() != 0;
                    break;
                case MenuString:
                    m.id = r.u32();
                    m.strings.push_back(r.str());
                    m.atDefault = r.u32() != 0;
                    break;
                case AllChange:
                    break;
                case ModeValue:
                    m.id = r.u32();
                    break;
                case DisplayText: {
                    const uint32_t lines = r.u32();
                    for (uint32_t i = 0; r.ok && i < lines && i < 16; ++i) {
                        const uint32_t k = r.u32();
                        if (!r.ok || k > body.size() - r.at) {
                            r.ok = false;
                            break;
                        }
                        m.strings.push_back(body.substr(r.at, k));
                        r.at += k;
                        if (k > 0) r.u32();
                    }
                    break;
                }
                default:
                    if (error) *error = "unknown application command " + std::to_string(m.command);
                    return false;
            }
            if (!r.ok) {
                if (error) *error = "application message cut short";
                return false;
            }
            out.push_back(std::move(m));
        }
    }
    if (used) *used = at;
    return true;
}

std::string controlsXml(const ControlSystem& system) {
    std::string x = "<?xml version=\"1.0\" encoding=\"utf-8\" standalone=\"yes\"?>\n";
    x += "<TangentWave fileType=\"ControlSystem\" fileVersion=\"3.0\">\n";
    x += "  <Capabilities>\n";
    x += std::string("    <Jog enabled=\"") + (system.jog ? "true" : "false") + "\"/>\n";
    x += std::string("    <Shuttle enabled=\"") + (system.shuttle ? "true" : "false") + "\"/>\n";
    x += "    <StatusDisplay lineCount=\"" + std::to_string(system.displayLines) + "\"/>\n";
    x += "  </Capabilities>\n";
    x += "  <DefaultGlobalSettings>\n";
    x += "    <KnobSensitivity std=\"3\" alt=\"5\"/>\n";
    x += "    <JogDialSensitivity std=\"1\" alt=\"5\"/>\n";
    x += "    <TrackerballSensitivity std=\"1\" alt=\"5\"/>\n";
    x += "    <TrackerballDialSensitivity std=\"1\" alt=\"5\"/>\n";
    x += "    <IndependentPanelBanks enabled=\"false\"/>\n";
    x += "  </DefaultGlobalSettings>\n";
    x += "  <Modes>\n";
    for (const auto& [id, name] : system.modes)
        x += "    <Mode id=\"" + hexId(id) + "\">\n      <Name>" + xmlEscape(ascii(name)) + "</Name>\n    </Mode>\n";
    x += "  </Modes>\n";
    x += "  <Controls>\n";
    for (const Group& g : system.groups) writeGroup(x, g, "    ");
    for (const Control& c : system.controls) writeControl(x, c, "    ");
    x += "  </Controls>\n";
    x += "</TangentWave>\n";
    return x;
}

std::pair<std::string, std::string> emptyMap() {
    return {"element-tk-map.xml",
            "<?xml version=\"1.0\" encoding=\"utf-8\" standalone=\"yes\"?>\n"
            "<TangentWave fileType=\"PanelMap\" fileVersion=\"3.0\">\n"
            "    <Panels>\n"
            "        <Panel type=\"Element-Tk\">\n"
            "        </Panel>\n"
            "    </Panels>\n"
            "</TangentWave>\n"};
}

std::string ascii(const std::string& utf8, size_t max) {
    std::string out;
    for (size_t i = 0; i < utf8.size() && out.size() < max; ++i) {
        const unsigned char c = static_cast<unsigned char>(utf8[i]);
        if (c == '&') {
            if (out.size() + 3 <= max) out += "and";
        } else if (c >= 0x20 && c <= 0x7E) {
            out += char(c);
        } else if (c >= 0xC0) {
            out += '?';  // (a character beyond ASCII: its lead byte stands for it, its other bytes are dropped)
        }
    }
    return out;
}

}  // namespace montage::tangent
