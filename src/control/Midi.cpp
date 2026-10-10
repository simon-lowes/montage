#include "Midi.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
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

class CoreMidiConnection;
std::mutex gLiveMutex;
std::vector<CoreMidiConnection*> gLive;  // open connections, told when the MIDI setup changes
void setupChanged();

// One client for the whole process, never disposed (disposing an app's last client can stop the MIDI server, after
// which no new client can be made).
MIDIClientRef sharedClient() {
    static MIDIClientRef client = 0;
    static std::once_flag once;
    std::call_once(once, [] {
        MIDIClientCreate(CFSTR("Montage"),
                         [](const MIDINotification* n, void*) {
                             if (n && (n->messageID == kMIDIMsgSetupChanged || n->messageID == kMIDIMsgObjectRemoved)) setupChanged();
                         },
                         nullptr, &client);
    });
    return client;
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
        {
            std::lock_guard<std::mutex> lock(gLiveMutex);
            std::erase(gLive, this);
        }
        if (inPort_) {
            if (src_) MIDIPortDisconnectSource(inPort_, src_);
            MIDIPortDispose(inPort_);
        }
        if (outPort_) MIDIPortDispose(outPort_);
    }
    // Still plugged in? Told once when not.
    void check() {
        if (lost_) return;
        if ((!inId_.empty() && !findEndpoint(true, inId_)) || (!outId_.empty() && !findEndpoint(false, outId_))) {
            lost_ = true;
            if (onLost_) onLost_();
        }
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

    MIDIPortRef inPort_ = 0, outPort_ = 0;
    MIDIEndpointRef src_ = 0, dst_ = 0;
    MidiParser parser_;
    std::function<void(const MidiMessage&)> onMessage_;
    std::function<void()> onLost_;
    std::string inName_, outName_, inId_, outId_;
    bool lost_ = false;
};

void setupChanged() {
    std::lock_guard<std::mutex> lock(gLiveMutex);
    for (CoreMidiConnection* c : gLive) c->check();
}

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
                                         std::function<void(const MidiMessage&)> onMessage, std::string* error,
                                         std::function<void()> onLost) {
    auto fail = [&](const std::string& e) {
        if (error) *error = e;
        return std::unique_ptr<MidiConnection>();
    };
    const MIDIClientRef client = sharedClient();
    if (!client) return fail("CoreMIDI is not available");
    auto c = std::make_unique<CoreMidiConnection>();
    c->onMessage_ = std::move(onMessage);
    c->onLost_ = std::move(onLost);
    c->inId_ = inputId;
    c->outId_ = outputId;
    if (!inputId.empty()) {
        c->src_ = findEndpoint(true, inputId);
        if (!c->src_) return fail("That MIDI input is not connected");
        c->inName_ = endpointName(c->src_);
        if (MIDIInputPortCreate(client, CFSTR("Montage In"), &CoreMidiConnection::readProc, c.get(), &c->inPort_) != noErr ||
            MIDIPortConnectSource(c->inPort_, c->src_, nullptr) != noErr)
            return fail("Cannot open the MIDI input " + c->inName_);
    }
    if (!outputId.empty()) {
        c->dst_ = findEndpoint(false, outputId);
        if (!c->dst_) return fail("That MIDI output is not connected");
        c->outName_ = endpointName(c->dst_);
        if (MIDIOutputPortCreate(client, CFSTR("Montage Out"), &c->outPort_) != noErr)
            return fail("Cannot open the MIDI output " + c->outName_);
    }
    {
        std::lock_guard<std::mutex> lock(gLiveMutex);
        gLive.push_back(c.get());
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
        // The sender first (it may be mid-message), then the input with its exclusive buffers handed back.
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            stop_ = true;
        }
        queueReady_.notify_all();
        if (sender_.joinable()) sender_.join();
        if (in_) {
            closing_ = true;
            midiInStop(in_);
            midiInReset(in_);
            for (MIDIHDR& h : headers_)
                if (h.dwFlags & MHDR_PREPARED) midiInUnprepareHeader(in_, &h, sizeof h);
            midiInClose(in_);
        }
        if (out_) {
            midiOutReset(out_);
            midiOutClose(out_);
        }
    }
    // Queued for the sending thread, so a slow driver never holds up the caller.
    bool send(const MidiMessage& bytes) override {
        if (!out_ || bytes.empty() || lost_) return false;
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            if (queue_.size() > 4096) return false;  // the port cannot keep up: dropped
            queue_.push_back(bytes);
        }
        queueReady_.notify_one();
        return true;
    }
    std::string inputName() const override { return inName_; }
    std::string outputName() const override { return outName_; }

    void startSender() {
        sender_ = std::thread([this] {
            for (;;) {
                MidiMessage m;
                {
                    std::unique_lock<std::mutex> lock(queueMutex_);
                    queueReady_.wait(lock, [&] { return stop_ || !queue_.empty(); });
                    if (stop_) return;
                    m = std::move(queue_.front());
                    queue_.pop_front();
                }
                if (!write(m)) gone();
            }
        });
    }
    bool write(const MidiMessage& bytes) {
        if (bytes[0] == 0xF0) {
            // System exclusive: a prepared buffer, waited on until the driver is done with it.
            std::vector<char> data(bytes.begin(), bytes.end());
            MIDIHDR h{};
            h.lpData = data.data();
            h.dwBufferLength = DWORD(data.size());
            if (midiOutPrepareHeader(out_, &h, sizeof h) != MMSYSERR_NOERROR) return false;
            const MMRESULT r = midiOutLongMsg(out_, &h, sizeof h);
            for (int i = 0; r == MMSYSERR_NOERROR && !(h.dwFlags & MHDR_DONE) && i < 1000; ++i) Sleep(1);
            if (!(h.dwFlags & MHDR_DONE)) midiOutReset(out_);  // (so the driver hands the buffer back before it goes)
            midiOutUnprepareHeader(out_, &h, sizeof h);
            return r == MMSYSERR_NOERROR;
        }
        DWORD msg = bytes[0];
        if (bytes.size() > 1) msg |= DWORD(bytes[1]) << 8;
        if (bytes.size() > 2) msg |= DWORD(bytes[2]) << 16;
        return midiOutShortMsg(out_, msg) == MMSYSERR_NOERROR;
    }
    void gone() {
        if (lost_.exchange(true)) return;
        if (onLost_) onLost_();
    }

    // Short messages whole, system exclusive through buffers handed back to the driver after each is read.
    static void CALLBACK inProc(HMIDIIN in, UINT msg, DWORD_PTR instance, DWORD_PTR p1, DWORD_PTR) {
        auto* self = reinterpret_cast<WinMidiConnection*>(instance);
        if (msg == MIM_DATA) {
            const uint8_t status = uint8_t(p1 & 0xff);
            MidiMessage m{status};
            const int n = midiDataBytes(status);
            if (n >= 1) m.push_back(uint8_t((p1 >> 8) & 0x7f));
            if (n >= 2) m.push_back(uint8_t((p1 >> 16) & 0x7f));
            if (self->onMessage_) self->onMessage_(m);
        } else if (msg == MIM_LONGDATA) {
            auto* h = reinterpret_cast<MIDIHDR*>(p1);
            if (h->dwBytesRecorded > 0)
                self->sysex_.feed(reinterpret_cast<const uint8_t*>(h->lpData), h->dwBytesRecorded, self->onMessage_);
            if (!self->closing_) midiInAddBuffer(in, h, sizeof *h);
        } else if (msg == MIM_CLOSE && !self->closing_) {
            self->gone();
        }
    }
    bool addBuffers() {
        for (size_t i = 0; i < headers_.size(); ++i) {
            buffers_[i].assign(1024, 0);
            headers_[i] = MIDIHDR{};
            headers_[i].lpData = buffers_[i].data();
            headers_[i].dwBufferLength = DWORD(buffers_[i].size());
            if (midiInPrepareHeader(in_, &headers_[i], sizeof headers_[i]) != MMSYSERR_NOERROR ||
                midiInAddBuffer(in_, &headers_[i], sizeof headers_[i]) != MMSYSERR_NOERROR)
                return false;
        }
        return true;
    }

    HMIDIIN in_ = nullptr;
    HMIDIOUT out_ = nullptr;
    std::array<MIDIHDR, 4> headers_{};
    std::array<std::vector<char>, 4> buffers_;
    MidiParser sysex_;
    std::atomic<bool> closing_{false}, lost_{false};
    std::function<void(const MidiMessage&)> onMessage_;
    std::function<void()> onLost_;
    std::string inName_, outName_;
    std::thread sender_;
    std::mutex queueMutex_;
    std::condition_variable queueReady_;
    std::deque<MidiMessage> queue_;
    bool stop_ = false;
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
                                         std::function<void(const MidiMessage&)> onMessage, std::string* error,
                                         std::function<void()> onLost) {
    auto fail = [&](const std::string& e) {
        if (error) *error = e;
        return std::unique_ptr<MidiConnection>();
    };
    auto c = std::make_unique<WinMidiConnection>();
    c->onMessage_ = std::move(onMessage);
    c->onLost_ = std::move(onLost);
    if (!inputId.empty()) {
        UINT index = 0;
        if (!findDevice(true, inputId, index, c->inName_)) return fail("That MIDI input is not connected");
        if (midiInOpen(&c->in_, index, DWORD_PTR(&WinMidiConnection::inProc), DWORD_PTR(c.get()), CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
            c->in_ = nullptr;
            return fail("Cannot open the MIDI input " + c->inName_ + " (another program may be using it)");
        }
        c->addBuffers();  // (without them only short messages come in)
        midiInStart(c->in_);
    }
    if (!outputId.empty()) {
        UINT index = 0;
        if (!findDevice(false, outputId, index, c->outName_)) return fail("That MIDI output is not connected");
        if (midiOutOpen(&c->out_, index, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
            c->out_ = nullptr;
            return fail("Cannot open the MIDI output " + c->outName_ + " (another program may be using it)");
        }
        c->startSender();
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
    // Non-blocking: a port that is full is waited on briefly, then what does not fit is dropped.
    bool send(const MidiMessage& bytes) override {
        if (out_ < 0 || bytes.empty() || lost_) return false;
        size_t done = 0;
        int waits = 0;
        while (done < bytes.size()) {
            const ssize_t n = write(out_, bytes.data() + done, bytes.size() - done);
            if (n > 0) {
                done += size_t(n);
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EINTR) && waits++ < 4) {
                pollfd p{out_, POLLOUT, 0};
                poll(&p, 1, 5);
                continue;
            }
            if (n < 0 && errno != EAGAIN && errno != EINTR) gone();  // unplugged
            return false;
        }
        return true;
    }
    void gone() {
        if (lost_.exchange(true)) return;
        if (onLost_) onLost_();
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
                if (n > 0) {
                    parser_.feed(buf, size_t(n), onMessage_);
                } else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
                    gone();  // unplugged
                    break;
                }
            }
        });
    }

    int in_ = -1, out_ = -1;
    std::atomic<bool> stop_{false}, lost_{false};
    std::thread reader_;
    MidiParser parser_;
    std::function<void(const MidiMessage&)> onMessage_;
    std::function<void()> onLost_;
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
                                         std::function<void(const MidiMessage&)> onMessage, std::string* error,
                                         std::function<void()> onLost) {
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
    c->onLost_ = std::move(onLost);
    if (!inputId.empty()) {
        if (!ours(inputId)) return fail("Not a MIDI device: " + inputId);
        c->inName_ = nameOf(inputId);
        c->in_ = open(inputId.c_str(), O_RDONLY | O_NONBLOCK);
        if (c->in_ < 0) return fail("Cannot open the MIDI input " + c->inName_ + " (another program may be using it)");
    }
    if (!outputId.empty()) {
        if (!ours(outputId)) return fail("Not a MIDI device: " + outputId);
        c->outName_ = nameOf(outputId);
        c->out_ = open(outputId.c_str(), O_WRONLY | O_NONBLOCK);  // (a busy port would otherwise block the open)
        if (c->out_ < 0) return fail("Cannot open the MIDI output " + c->outName_ + " (another program may be using it)");
    }
    c->start();
    return c;
}

#else

std::vector<MidiPortInfo> midiInputs() { return {}; }
std::vector<MidiPortInfo> midiOutputs() { return {}; }
std::unique_ptr<MidiConnection> openMidi(const std::string&, const std::string&, std::function<void(const MidiMessage&)>,
                                         std::string* error, std::function<void()>) {
    if (error) *error = "MIDI is not available on this system";
    return nullptr;
}

#endif

}  // namespace montage
