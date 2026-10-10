#include "Midi.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <thread>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#elif defined(_WIN32)
#include <windows.h>
#include <mmsystem.h>
#elif defined(__linux__)
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <fstream>
#include <sstream>
#endif

namespace montage {

int midiDataBytes(uint8_t s) {
    if (s < 0x80) return 0;
    if (s < 0xF0) return (s & 0xF0) == 0xC0 || (s & 0xF0) == 0xD0 ? 1 : 2;
    if (s == 0xF1 || s == 0xF3) return 1;
    if (s == 0xF2) return 2;
    return 0;
}

void MidiParser::reset() {
    msg_.clear();
    running_ = 0;
    need_ = 0;
    sysex_ = false;
}

void MidiParser::feed(const uint8_t* data, size_t n, const std::function<void(const MidiMessage&)>& out) {
    for (size_t i = 0; i < n; ++i) {
        const uint8_t b = data[i];
        if (b >= 0xF8) {  // real time: on its own, wherever it falls
            if (out) out(MidiMessage{b});
            continue;
        }
        if (sysex_) {
            if (b < 0x80) {
                msg_.push_back(b);
                if (msg_.size() > (1 << 16)) reset();  // runaway: dropped
                continue;
            }
            sysex_ = false;
            if (b == 0xF7) {
                msg_.push_back(b);
                if (out) out(msg_);
                msg_.clear();
                continue;
            }
            msg_.clear();  // cut short by another status: dropped, and that status is read below
        }
        if (b == 0xF0) {
            sysex_ = true;
            running_ = 0;
            msg_.assign(1, b);
            continue;
        }
        if (b >= 0x80) {
            msg_.assign(1, b);
            need_ = size_t(midiDataBytes(b));
            running_ = b < 0xF0 ? b : 0;  // system common messages cancel running status
            if (need_ == 0) {
                if (b == 0xF6 && out) out(msg_);  // tune request; a stray end of exclusive is dropped
                msg_.clear();
            }
            continue;
        }
        if (msg_.empty()) {
            if (!running_) continue;  // data with no status: dropped
            msg_.assign(1, running_);
            need_ = size_t(midiDataBytes(running_));
        }
        msg_.push_back(b);
        if (msg_.size() == need_ + 1) {
            if (out) out(msg_);
            msg_.clear();
        }
    }
}

#if defined(__APPLE__)

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

namespace {

std::string cfString(CFStringRef s) {
    if (!s) return {};
    char buf[512];
    return CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8) ? std::string(buf) : std::string();
}

std::string endpointName(MIDIEndpointRef e) {
    CFStringRef name = nullptr;
    if (MIDIObjectGetStringProperty(e, kMIDIPropertyDisplayName, &name) != noErr || !name) return "MIDI";
    std::string s = cfString(name);
    CFRelease(name);
    return s.empty() ? std::string("MIDI") : s;
}

std::string endpointId(MIDIEndpointRef e) {
    SInt32 uid = 0;
    MIDIObjectGetIntegerProperty(e, kMIDIPropertyUniqueID, &uid);
    return std::to_string(uid);
}

MIDIEndpointRef findEndpoint(bool source, const std::string& id) {
    const ItemCount n = source ? MIDIGetNumberOfSources() : MIDIGetNumberOfDestinations();
    for (ItemCount i = 0; i < n; ++i) {
        const MIDIEndpointRef e = source ? MIDIGetSource(i) : MIDIGetDestination(i);
        if (e && endpointId(e) == id) return e;
    }
    return 0;
}

class CoreMidiConnection : public MidiConnection {
public:
    ~CoreMidiConnection() override {
        if (inPort_) {
            if (src_) MIDIPortDisconnectSource(inPort_, src_);
            MIDIPortDispose(inPort_);
        }
        if (outPort_) MIDIPortDispose(outPort_);
        if (client_) MIDIClientDispose(client_);
    }
    bool send(const MidiMessage& bytes) override {
        if (!outPort_ || !dst_ || bytes.empty()) return false;
        std::vector<Byte> buffer(bytes.size() + 256);
        auto* list = reinterpret_cast<MIDIPacketList*>(buffer.data());
        MIDIPacket* packet = MIDIPacketListInit(list);
        packet = MIDIPacketListAdd(list, buffer.size(), packet, 0, bytes.size(), bytes.data());
        return packet && MIDISend(outPort_, dst_, list) == noErr;
    }
    std::string inputName() const override { return inName_; }
    std::string outputName() const override { return outName_; }

    static void readProc(const MIDIPacketList* list, void* refCon, void*) {
        auto* self = static_cast<CoreMidiConnection*>(refCon);
        const MIDIPacket* p = &list->packet[0];
        for (UInt32 i = 0; i < list->numPackets; ++i) {
            self->parser_.feed(p->data, p->length, self->onMessage_);
            p = MIDIPacketNext(p);
        }
    }

    MIDIClientRef client_ = 0;
    MIDIPortRef inPort_ = 0, outPort_ = 0;
    MIDIEndpointRef src_ = 0, dst_ = 0;
    MidiParser parser_;
    std::function<void(const MidiMessage&)> onMessage_;
    std::string inName_, outName_;
};

}  // namespace

std::vector<MidiPortInfo> midiInputs() {
    std::vector<MidiPortInfo> v;
    for (ItemCount i = 0, n = MIDIGetNumberOfSources(); i < n; ++i)
        if (const MIDIEndpointRef e = MIDIGetSource(i)) v.push_back({endpointId(e), endpointName(e)});
    return v;
}

std::vector<MidiPortInfo> midiOutputs() {
    std::vector<MidiPortInfo> v;
    for (ItemCount i = 0, n = MIDIGetNumberOfDestinations(); i < n; ++i)
        if (const MIDIEndpointRef e = MIDIGetDestination(i)) v.push_back({endpointId(e), endpointName(e)});
    return v;
}

std::unique_ptr<MidiConnection> openMidi(const std::string& inputId, const std::string& outputId,
                                         std::function<void(const MidiMessage&)> onMessage, std::string* error) {
    auto fail = [&](const std::string& e) {
        if (error) *error = e;
        return std::unique_ptr<MidiConnection>();
    };
    auto c = std::make_unique<CoreMidiConnection>();
    c->onMessage_ = std::move(onMessage);
    if (MIDIClientCreate(CFSTR("Montage"), nullptr, nullptr, &c->client_) != noErr) return fail("CoreMIDI is not available");
    if (!inputId.empty()) {
        c->src_ = findEndpoint(true, inputId);
        if (!c->src_) return fail("That MIDI input is not connected");
        c->inName_ = endpointName(c->src_);
        if (MIDIInputPortCreate(c->client_, CFSTR("Montage In"), &CoreMidiConnection::readProc, c.get(), &c->inPort_) != noErr ||
            MIDIPortConnectSource(c->inPort_, c->src_, nullptr) != noErr)
            return fail("Cannot open the MIDI input " + c->inName_);
    }
    if (!outputId.empty()) {
        c->dst_ = findEndpoint(false, outputId);
        if (!c->dst_) return fail("That MIDI output is not connected");
        c->outName_ = endpointName(c->dst_);
        if (MIDIOutputPortCreate(c->client_, CFSTR("Montage Out"), &c->outPort_) != noErr)
            return fail("Cannot open the MIDI output " + c->outName_);
    }
    return c;
}

#pragma clang diagnostic pop

#elif defined(_WIN32)

namespace {

std::string narrow(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(size_t(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

// Ids are "index:name": the index finds it, the name checks it is still the same device.
bool findDevice(bool input, const std::string& id, UINT& index, std::string& name) {
    const UINT n = input ? midiInGetNumDevs() : midiOutGetNumDevs();
    for (UINT i = 0; i < n; ++i) {
        std::string nm;
        if (input) {
            MIDIINCAPSW caps{};
            if (midiInGetDevCapsW(i, &caps, sizeof caps) != MMSYSERR_NOERROR) continue;
            nm = narrow(caps.szPname);
        } else {
            MIDIOUTCAPSW caps{};
            if (midiOutGetDevCapsW(i, &caps, sizeof caps) != MMSYSERR_NOERROR) continue;
            nm = narrow(caps.szPname);
        }
        if (std::to_string(i) + ":" + nm == id) {
            index = i;
            name = nm;
            return true;
        }
    }
    // Moved to another index (plugged into another port): found by name.
    const size_t colon = id.find(':');
    const std::string want = colon == std::string::npos ? id : id.substr(colon + 1);
    for (UINT i = 0; i < n; ++i) {
        std::string nm;
        if (input) {
            MIDIINCAPSW caps{};
            if (midiInGetDevCapsW(i, &caps, sizeof caps) == MMSYSERR_NOERROR) nm = narrow(caps.szPname);
        } else {
            MIDIOUTCAPSW caps{};
            if (midiOutGetDevCapsW(i, &caps, sizeof caps) == MMSYSERR_NOERROR) nm = narrow(caps.szPname);
        }
        if (!nm.empty() && nm == want) {
            index = i;
            name = nm;
            return true;
        }
    }
    return false;
}

class WinMidiConnection : public MidiConnection {
public:
    ~WinMidiConnection() override {
        if (in_) {
            midiInStop(in_);
            midiInReset(in_);
            midiInClose(in_);
        }
        if (out_) {
            midiOutReset(out_);
            midiOutClose(out_);
        }
    }
    bool send(const MidiMessage& bytes) override {
        if (!out_ || bytes.empty()) return false;
        if (bytes[0] == 0xF0) {
            // System exclusive: a prepared buffer, waited on until the driver is done with it.
            std::vector<char> data(bytes.begin(), bytes.end());
            MIDIHDR h{};
            h.lpData = data.data();
            h.dwBufferLength = DWORD(data.size());
            if (midiOutPrepareHeader(out_, &h, sizeof h) != MMSYSERR_NOERROR) return false;
            const bool ok = midiOutLongMsg(out_, &h, sizeof h) == MMSYSERR_NOERROR;
            for (int i = 0; ok && !(h.dwFlags & MHDR_DONE) && i < 1000; ++i) Sleep(1);
            midiOutUnprepareHeader(out_, &h, sizeof h);
            return ok;
        }
        DWORD msg = bytes[0];
        if (bytes.size() > 1) msg |= DWORD(bytes[1]) << 8;
        if (bytes.size() > 2) msg |= DWORD(bytes[2]) << 16;
        return midiOutShortMsg(out_, msg) == MMSYSERR_NOERROR;
    }
    std::string inputName() const override { return inName_; }
    std::string outputName() const override { return outName_; }

    // Channel and system common messages (all a control surface sends the host); system exclusive input is not read.
    static void CALLBACK inProc(HMIDIIN, UINT msg, DWORD_PTR instance, DWORD_PTR p1, DWORD_PTR) {
        if (msg != MIM_DATA) return;
        auto* self = reinterpret_cast<WinMidiConnection*>(instance);
        const uint8_t status = uint8_t(p1 & 0xff);
        MidiMessage m{status};
        const int n = midiDataBytes(status);
        if (n >= 1) m.push_back(uint8_t((p1 >> 8) & 0x7f));
        if (n >= 2) m.push_back(uint8_t((p1 >> 16) & 0x7f));
        if (self->onMessage_) self->onMessage_(m);
    }

    HMIDIIN in_ = nullptr;
    HMIDIOUT out_ = nullptr;
    std::function<void(const MidiMessage&)> onMessage_;
    std::string inName_, outName_;
};

}  // namespace

std::vector<MidiPortInfo> midiInputs() {
    std::vector<MidiPortInfo> v;
    for (UINT i = 0, n = midiInGetNumDevs(); i < n; ++i) {
        MIDIINCAPSW caps{};
        if (midiInGetDevCapsW(i, &caps, sizeof caps) != MMSYSERR_NOERROR) continue;
        const std::string name = narrow(caps.szPname);
        v.push_back({std::to_string(i) + ":" + name, name});
    }
    return v;
}

std::vector<MidiPortInfo> midiOutputs() {
    std::vector<MidiPortInfo> v;
    for (UINT i = 0, n = midiOutGetNumDevs(); i < n; ++i) {
        MIDIOUTCAPSW caps{};
        if (midiOutGetDevCapsW(i, &caps, sizeof caps) != MMSYSERR_NOERROR) continue;
        const std::string name = narrow(caps.szPname);
        v.push_back({std::to_string(i) + ":" + name, name});
    }
    return v;
}

std::unique_ptr<MidiConnection> openMidi(const std::string& inputId, const std::string& outputId,
                                         std::function<void(const MidiMessage&)> onMessage, std::string* error) {
    auto fail = [&](const std::string& e) {
        if (error) *error = e;
        return std::unique_ptr<MidiConnection>();
    };
    auto c = std::make_unique<WinMidiConnection>();
    c->onMessage_ = std::move(onMessage);
    if (!inputId.empty()) {
        UINT index = 0;
        if (!findDevice(true, inputId, index, c->inName_)) return fail("That MIDI input is not connected");
        if (midiInOpen(&c->in_, index, DWORD_PTR(&WinMidiConnection::inProc), DWORD_PTR(c.get()), CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
            c->in_ = nullptr;
            return fail("Cannot open the MIDI input " + c->inName_ + " (another program may be using it)");
        }
        midiInStart(c->in_);
    }
    if (!outputId.empty()) {
        UINT index = 0;
        if (!findDevice(false, outputId, index, c->outName_)) return fail("That MIDI output is not connected");
        if (midiOutOpen(&c->out_, index, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
            c->out_ = nullptr;
            return fail("Cannot open the MIDI output " + c->outName_ + " (another program may be using it)");
        }
    }
    return c;
}

#elif defined(__linux__)

namespace {

struct RawPort {
    std::string path, name;
    bool input = true, output = true;
};

// ALSA's raw MIDI devices: /dev/snd/midiC<card>D<device>, named in /proc/asound/card<card>/midi<device>.
std::vector<RawPort> rawPorts() {
    std::vector<RawPort> ports;
    DIR* dir = opendir("/dev/snd");
    if (!dir) return ports;
    while (dirent* e = readdir(dir)) {
        int card = -1, device = -1;
        char tail = 0;
        if (std::sscanf(e->d_name, "midiC%dD%d%c", &card, &device, &tail) != 2) continue;
        RawPort p;
        p.path = std::string("/dev/snd/") + e->d_name;
        p.name = p.path;
        std::ifstream info("/proc/asound/card" + std::to_string(card) + "/midi" + std::to_string(device));
        if (info) {
            std::stringstream ss;
            ss << info.rdbuf();
            const std::string text = ss.str();
            const std::string first = text.substr(0, text.find('\n'));
            if (!first.empty()) p.name = first;
            p.input = text.find("\nInput") != std::string::npos;
            p.output = text.find("\nOutput") != std::string::npos;
        }
        ports.push_back(p);
    }
    closedir(dir);
    std::sort(ports.begin(), ports.end(), [](const RawPort& a, const RawPort& b) { return a.path < b.path; });
    return ports;
}

class RawMidiConnection : public MidiConnection {
public:
    ~RawMidiConnection() override {
        stop_ = true;
        if (reader_.joinable()) reader_.join();
        if (in_ >= 0) close(in_);
        if (out_ >= 0) close(out_);
    }
    bool send(const MidiMessage& bytes) override {
        if (out_ < 0 || bytes.empty()) return false;
        size_t done = 0;
        while (done < bytes.size()) {
            const ssize_t n = write(out_, bytes.data() + done, bytes.size() - done);
            if (n <= 0) return false;
            done += size_t(n);
        }
        return true;
    }
    std::string inputName() const override { return inName_; }
    std::string outputName() const override { return outName_; }
    void start() {
        if (in_ < 0) return;
        reader_ = std::thread([this] {
            uint8_t buf[256];
            while (!stop_) {
                pollfd p{in_, POLLIN, 0};
                if (poll(&p, 1, 100) <= 0) continue;
                const ssize_t n = read(in_, buf, sizeof buf);
                if (n > 0) parser_.feed(buf, size_t(n), onMessage_);
                else if (n == 0 || (errno != EAGAIN && errno != EINTR)) break;  // unplugged
            }
        });
    }

    int in_ = -1, out_ = -1;
    std::atomic<bool> stop_{false};
    std::thread reader_;
    MidiParser parser_;
    std::function<void(const MidiMessage&)> onMessage_;
    std::string inName_, outName_;
};

}  // namespace

std::vector<MidiPortInfo> midiInputs() {
    std::vector<MidiPortInfo> v;
    for (const RawPort& p : rawPorts())
        if (p.input) v.push_back({p.path, p.name});
    return v;
}

std::vector<MidiPortInfo> midiOutputs() {
    std::vector<MidiPortInfo> v;
    for (const RawPort& p : rawPorts())
        if (p.output) v.push_back({p.path, p.name});
    return v;
}

std::unique_ptr<MidiConnection> openMidi(const std::string& inputId, const std::string& outputId,
                                         std::function<void(const MidiMessage&)> onMessage, std::string* error) {
    auto fail = [&](const std::string& e) {
        if (error) *error = e;
        return std::unique_ptr<MidiConnection>();
    };
    auto nameOf = [](const std::string& path) {
        for (const RawPort& p : rawPorts())
            if (p.path == path) return p.name;
        return path;
    };
    // Only ALSA's own devices are opened.
    auto ours = [](const std::string& path) { return path.rfind("/dev/snd/midiC", 0) == 0 && path.find("..") == std::string::npos; };
    auto c = std::make_unique<RawMidiConnection>();
    c->onMessage_ = std::move(onMessage);
    if (!inputId.empty()) {
        if (!ours(inputId)) return fail("Not a MIDI device: " + inputId);
        c->inName_ = nameOf(inputId);
        c->in_ = open(inputId.c_str(), O_RDONLY | O_NONBLOCK);
        if (c->in_ < 0) return fail("Cannot open the MIDI input " + c->inName_ + " (another program may be using it)");
    }
    if (!outputId.empty()) {
        if (!ours(outputId)) return fail("Not a MIDI device: " + outputId);
        c->outName_ = nameOf(outputId);
        c->out_ = open(outputId.c_str(), O_WRONLY);
        if (c->out_ < 0) return fail("Cannot open the MIDI output " + c->outName_ + " (another program may be using it)");
    }
    c->start();
    return c;
}

#else

std::vector<MidiPortInfo> midiInputs() { return {}; }
std::vector<MidiPortInfo> midiOutputs() { return {}; }
std::unique_ptr<MidiConnection> openMidi(const std::string&, const std::string&, std::function<void(const MidiMessage&)>,
                                         std::string* error) {
    if (error) *error = "MIDI is not available on this system";
    return nullptr;
}

#endif

}  // namespace montage
