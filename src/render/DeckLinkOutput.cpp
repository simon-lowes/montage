// Montage — reference output through a Blackmagic DeckLink or UltraStudio (SDI and HDMI): scheduled playback paced by
// the card's clock, 10-bit v210 or 8-bit 4:2:2, RP 188 timecode, embedded 48 kHz audio, HDR10 / HLG metadata on HDMI
// and SDI where the card takes it. The Desktop Video driver's DeckLink API is loaded when it is installed (SDK 12
// interfaces, which later drivers still serve); the Windows build has no DeckLink output yet (its COM interfaces).
#include "ReferenceOutput.h"

#if defined(__linux__) || defined(__APPLE__)

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>

#include "DeckLinkAPI.h"
#include "audio/DynLib.h"

namespace montage::output {

namespace {

#ifdef __APPLE__
using DlString = CFStringRef;
std::string take(DlString s) {
    if (!s) return {};
    char buf[512] = {};
    CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8);
    CFRelease(s);
    return buf;
}
const char* const kLibrary = "/Library/Frameworks/DeckLinkAPI.framework/DeckLinkAPI";
#else
using DlString = const char*;
std::string take(DlString s) {
    if (!s) return {};
    std::string out = s;
    free(const_cast<char*>(s));
    return out;
}
const char* const kLibrary = "libDeckLinkAPI.so";
#endif

bool sameIid(const REFIID& a, const REFIID& b) { return std::memcmp(&a, &b, sizeof(REFIID)) == 0; }

struct DlApi {
    plugins::DynLib lib;
    IDeckLinkIterator* (*createIterator)() = nullptr;
    bool ok = false;
    std::string error;
};

DlApi& api() {
    static DlApi a;
    static std::once_flag once;
    std::call_once(once, [] {
        std::string why;
        if (!a.lib.open(kLibrary, &why)) {
            a.error = "Blackmagic Desktop Video is not installed (blackmagicdesign.com/support)";
            return;
        }
        a.createIterator = reinterpret_cast<IDeckLinkIterator* (*)()>(a.lib.symbol("CreateDeckLinkIteratorInstance_0004"));
        a.ok = a.createIterator != nullptr;
        if (!a.ok) a.error = "The Desktop Video installed is too old (version 12 or later needed)";
    });
    return a;
}

template <typename T>
struct Ref {
    T* p = nullptr;
    Ref() = default;
    explicit Ref(T* x) : p(x) {}
    Ref(const Ref&) = delete;
    Ref& operator=(const Ref&) = delete;
    Ref(Ref&& o) noexcept : p(o.p) { o.p = nullptr; }
    Ref& operator=(Ref&& o) noexcept {
        reset();
        p = o.p;
        o.p = nullptr;
        return *this;
    }
    ~Ref() { reset(); }
    void reset() {
        if (p) p->Release();
        p = nullptr;
    }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

template <typename T>
Ref<T> query(IUnknown* from, REFIID iid) {
    void* out = nullptr;
    if (!from || from->QueryInterface(iid, &out) != S_OK) return {};
    return Ref<T>(static_cast<T*>(out));
}

// The cards with outputs, in the order the driver lists them; `id` "<n>:<name>".
struct Card {
    std::string id, name;
    Ref<IDeckLink> deckLink;
};
std::vector<Card> cards() {
    std::vector<Card> out;
    if (!api().ok) return out;
    Ref<IDeckLinkIterator> it(api().createIterator());
    if (!it) return out;
    IDeckLink* d = nullptr;
    for (int n = 0; it->Next(&d) == S_OK; ++n) {
        Ref<IDeckLink> dl(d);
        if (!query<IDeckLinkOutput>(dl.p, IID_IDeckLinkOutput)) continue;
        DlString name = nullptr;
        dl->GetDisplayName(&name);
        Card c;
        c.name = take(name);
        c.id = std::to_string(n) + ":" + c.name;
        c.deckLink = std::move(dl);
        out.push_back(std::move(c));
    }
    return out;
}

// A frame the card made, with HDR metadata the card reads from it (SDK 12 has no mutable metadata on its own frames).
class HdrFrame : public IDeckLinkVideoFrame, public IDeckLinkVideoFrameMetadataExtensions {
public:
    IDeckLinkMutableVideoFrame* inner = nullptr;
    Signal signal;
    // IUnknown (owned by the backend: its count is not what frees it)
    HRESULT QueryInterface(REFIID iid, LPVOID* out) override {
        if (sameIid(iid, IID_IDeckLinkVideoFrameMetadataExtensions)) {
            *out = static_cast<IDeckLinkVideoFrameMetadataExtensions*>(this);
        } else if (sameIid(iid, IID_IDeckLinkVideoFrame) || sameIid(iid, IID_IUnknown)) {
            *out = static_cast<IDeckLinkVideoFrame*>(this);
        } else {
            *out = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    ULONG AddRef() override { return ++refs_; }
    ULONG Release() override { return --refs_; }
    // IDeckLinkVideoFrame
    long GetWidth() override { return inner->GetWidth(); }
    long GetHeight() override { return inner->GetHeight(); }
    long GetRowBytes() override { return inner->GetRowBytes(); }
    BMDPixelFormat GetPixelFormat() override { return inner->GetPixelFormat(); }
    BMDFrameFlags GetFlags() override { return inner->GetFlags() | bmdFrameContainsHDRMetadata; }
    HRESULT GetBytes(void** buffer) override { return inner->GetBytes(buffer); }
    HRESULT GetTimecode(BMDTimecodeFormat format, IDeckLinkTimecode** tc) override { return inner->GetTimecode(format, tc); }
    HRESULT GetAncillaryData(IDeckLinkVideoFrameAncillary** anc) override { return inner->GetAncillaryData(anc); }
    // IDeckLinkVideoFrameMetadataExtensions: CEA-861.3's EOTF (0 SDR, 2 PQ, 3 HLG), BT.2020, the mastering display
    // and light levels.
    HRESULT GetInt(BMDDeckLinkFrameMetadataID id, int64_t* v) override {
        if (id == bmdDeckLinkFrameMetadataHDRElectroOpticalTransferFunc) *v = signal.eotf == Eotf::Pq ? 2 : signal.eotf == Eotf::Hlg ? 3 : 0;
        else if (id == bmdDeckLinkFrameMetadataColorspace) *v = signal.bt2020 ? bmdColorspaceRec2020 : bmdColorspaceRec709;
        else return E_INVALIDARG;
        return S_OK;
    }
    HRESULT GetFloat(BMDDeckLinkFrameMetadataID id, double* v) override {
        // P3-D65 or BT.2020 primaries, D65 white.
        const double p3[6] = {0.680, 0.320, 0.265, 0.690, 0.150, 0.060}, bt2020[6] = {0.708, 0.292, 0.170, 0.797, 0.131, 0.046};
        const double* pr = signal.masteringP3 ? p3 : bt2020;
        if (!signal.hdrMetadata) return E_INVALIDARG;  // (HDR10's static metadata turned off, or HLG: the EOTF alone)
        switch (id) {
            case bmdDeckLinkFrameMetadataHDRDisplayPrimariesRedX: *v = pr[0]; break;
            case bmdDeckLinkFrameMetadataHDRDisplayPrimariesRedY: *v = pr[1]; break;
            case bmdDeckLinkFrameMetadataHDRDisplayPrimariesGreenX: *v = pr[2]; break;
            case bmdDeckLinkFrameMetadataHDRDisplayPrimariesGreenY: *v = pr[3]; break;
            case bmdDeckLinkFrameMetadataHDRDisplayPrimariesBlueX: *v = pr[4]; break;
            case bmdDeckLinkFrameMetadataHDRDisplayPrimariesBlueY: *v = pr[5]; break;
            case bmdDeckLinkFrameMetadataHDRWhitePointX: *v = 0.3127; break;
            case bmdDeckLinkFrameMetadataHDRWhitePointY: *v = 0.3290; break;
            case bmdDeckLinkFrameMetadataHDRMaxDisplayMasteringLuminance: *v = signal.masteringPeak; break;
            case bmdDeckLinkFrameMetadataHDRMinDisplayMasteringLuminance: *v = signal.masteringMin; break;
            case bmdDeckLinkFrameMetadataHDRMaximumContentLightLevel: *v = signal.maxCll; break;
            case bmdDeckLinkFrameMetadataHDRMaximumFrameAverageLightLevel: *v = signal.maxFall; break;
            default: return E_INVALIDARG;
        }
        return S_OK;
    }
    HRESULT GetFlag(BMDDeckLinkFrameMetadataID, bool*) override { return E_INVALIDARG; }
    HRESULT GetString(BMDDeckLinkFrameMetadataID, DlString*) override { return E_INVALIDARG; }
    HRESULT GetBytes(BMDDeckLinkFrameMetadataID, void*, uint32_t*) override { return E_INVALIDARG; }

private:
    std::atomic<ULONG> refs_{1};
};

class DeckLinkBackend : public Backend, public IDeckLinkVideoOutputCallback {
public:
    ~DeckLinkBackend() override { close(); }
    std::string name() const override { return "DeckLink"; }
    bool available(std::string* why) override {
        if (!api().ok && why) *why = api().error;
        return api().ok;
    }
    std::vector<Device> devices() override {
        std::vector<Device> out;
        for (Card& c : cards()) {
            Device d;
            d.id = c.id;
            d.name = c.name;
            auto o = query<IDeckLinkOutput>(c.deckLink.p, IID_IDeckLinkOutput);
            IDeckLinkDisplayModeIterator* modes = nullptr;
            bool tenBit = false;
            if (o && o->GetDisplayModeIterator(&modes) == S_OK) {
                Ref<IDeckLinkDisplayModeIterator> mit(modes);
                IDeckLinkDisplayMode* m = nullptr;
                while (mit->Next(&m) == S_OK) {
                    Ref<IDeckLinkDisplayMode> mode(m);
                    bool ok8 = false, ok10 = false;
                    o->DoesSupportVideoMode(bmdVideoConnectionUnspecified, mode->GetDisplayMode(), bmdFormat8BitYUV, bmdNoVideoOutputConversion,
                                            bmdSupportedVideoModeDefault, nullptr, &ok8);
                    o->DoesSupportVideoMode(bmdVideoConnectionUnspecified, mode->GetDisplayMode(), bmdFormat10BitYUV, bmdNoVideoOutputConversion,
                                            bmdSupportedVideoModeDefault, nullptr, &ok10);
                    if (!ok8 && !ok10) continue;
                    tenBit |= ok10;
                    BMDTimeValue duration = 0;
                    BMDTimeScale scale = 0;
                    mode->GetFrameRate(&duration, &scale);
                    if (duration <= 0 || scale <= 0) continue;
                    Mode x;
                    x.width = int(mode->GetWidth());
                    x.height = int(mode->GetHeight());
                    x.fps = {int(scale), int(duration)};
                    const BMDFieldDominance f = mode->GetFieldDominance();
                    x.interlaced = f == bmdLowerFieldFirst || f == bmdUpperFieldFirst;
                    x.name = modeName(x.width, x.height, x.fps, x.interlaced);
                    x.id = int64_t(mode->GetDisplayMode());
                    if (!findMode(d.modes, x.name)) d.modes.push_back(x);
                }
            }
            d.formats = tenBit ? std::vector<PixelFormat>{PixelFormat::V210, PixelFormat::Uyvy8} : std::vector<PixelFormat>{PixelFormat::Uyvy8};
            auto attrs = query<IDeckLinkProfileAttributes>(c.deckLink.p, IID_IDeckLinkProfileAttributes);
            bool hdr = false;
            int64_t channels = 2;
            if (attrs) {
                attrs->GetFlag(BMDDeckLinkSupportsHDRMetadata, &hdr);
                attrs->GetInt(BMDDeckLinkMaximumAudioChannels, &channels);
            }
            d.hdr = hdr;
            d.audioChannels.clear();
            for (int n : {2, 8, 16})
                if (n <= channels) d.audioChannels.push_back(n);
            d.audioRates = {48000};
            out.push_back(std::move(d));
        }
        return out;
    }

    bool open(const Device& device, const Mode& mode, const Signal& signal, int audioChannels, int, std::string* error) override {
        close();
        auto fail = [&](const std::string& why) {
            if (error) *error = why;
            close();
            return false;
        };
        if (!api().ok) return fail(api().error);
        for (Card& c : cards())
            if (c.id == device.id) deckLink_ = std::move(c.deckLink);
        if (!deckLink_) return fail("The DeckLink device is not there any more");
        hdrCard_ = device.hdr;
        output_ = query<IDeckLinkOutput>(deckLink_.p, IID_IDeckLinkOutput);
        if (!output_) return fail("The DeckLink device has no output");
        // Its frame duration in the card's time scale.
        IDeckLinkDisplayModeIterator* modes = nullptr;
        duration_ = 0;
        if (output_->GetDisplayModeIterator(&modes) == S_OK) {
            Ref<IDeckLinkDisplayModeIterator> mit(modes);
            IDeckLinkDisplayMode* m = nullptr;
            while (mit->Next(&m) == S_OK) {
                Ref<IDeckLinkDisplayMode> dm(m);
                if (int64_t(dm->GetDisplayMode()) == mode.id) dm->GetFrameRate(&duration_, &scale_);
            }
        }
        if (duration_ <= 0) return fail("The DeckLink device has no " + mode.name + " mode");
        signal_ = signal;
        format_ = signal.format == PixelFormat::V210 ? bmdFormat10BitYUV : bmdFormat8BitYUV;
        if (output_->EnableVideoOutput(BMDDisplayMode(mode.id), bmdVideoOutputRP188) != S_OK &&
            output_->EnableVideoOutput(BMDDisplayMode(mode.id), bmdVideoOutputFlagDefault) != S_OK)
            return fail("The DeckLink device would not start " + mode.name + " (is it in use?)");
        videoOn_ = true;
        channels_ = audioChannels;
        if (channels_ > 0 && output_->EnableAudioOutput(bmdAudioSampleRate48kHz, bmdAudioSampleType32bitInteger, uint32_t(channels_),
                                                        bmdAudioOutputStreamTimestamped) != S_OK)
            channels_ = 0;
        audioOn_ = channels_ > 0;
        // A few frames of its own in turn, each back when the card has shown it.
        width_ = mode.width;
        height_ = mode.height;
        const long rowBytes = long(rowBytes_ = output::rowBytes(signal.format, mode.width));
        for (int i = 0; i < kFrames; ++i) {
            IDeckLinkMutableVideoFrame* f = nullptr;
            if (output_->CreateVideoFrame(width_, height_, int32_t(rowBytes), format_, bmdFrameFlagDefault, &f) != S_OK || !f)
                return fail("The DeckLink device could not make frames");
            slots_[i].frame = f;
            slots_[i].hdr.inner = f;
            slots_[i].hdr.signal = signal;
            slots_[i].busy = false;
        }
        output_->SetScheduledFrameCompletionCallback(this);
        if (audioOn_) output_->BeginAudioPreroll();
        scheduled_ = 0;
        audioTime_ = 0;
        started_ = false;
        stopped_ = false;
        return true;
    }

    bool send(const VideoFrame& vf, const Audio& a) override {
        if (!output_) return false;
        // A frame the card is done with (it paces the output: kFrames - 1 wait to be shown).
        int slot = -1;
        {
            std::unique_lock lock(m_);
            const bool got = cv_.wait_for(lock, std::chrono::seconds(2), [&] {
                if (stopped_) return true;
                for (int i = 0; i < kFrames; ++i)
                    if (!slots_[i].busy) return true;
                return false;
            });
            if (!got || stopped_) return false;
            for (int i = 0; i < kFrames && slot < 0; ++i)
                if (!slots_[i].busy) slot = i;
            slots_[slot].busy = true;
        }
        Slot& s = slots_[slot];
        void* bytes = nullptr;
        if (s.frame->GetBytes(&bytes) != S_OK || !bytes) return false;
        const size_t dst = size_t(s.frame->GetRowBytes()), src = vf.stride, n = std::min(dst, src);
        for (int y = 0; y < height_; ++y) std::memcpy(static_cast<uint8_t*>(bytes) + size_t(y) * dst, vf.data + size_t(y) * src, n);
        const Timecode& tc = vf.timecode;
        s.frame->SetTimecodeFromComponents(bmdTimecodeRP188Any, uint8_t(tc.hours), uint8_t(tc.minutes), uint8_t(tc.seconds), uint8_t(tc.frames),
                                           tc.dropFrame ? bmdTimecodeIsDropFrame : bmdTimecodeFlagDefault);
        // (a card that takes no HDR metadata gets the frame as it is)
        IDeckLinkVideoFrame* shown = hdrCard_ && (signal_.eotf != Eotf::Sdr || signal_.bt2020) ? static_cast<IDeckLinkVideoFrame*>(&s.hdr)
                                                                                    : static_cast<IDeckLinkVideoFrame*>(s.frame);
        if (output_->ScheduleVideoFrame(shown, scheduled_ * duration_, duration_, scale_) != S_OK) {
            std::lock_guard lock(m_);
            s.busy = false;
        }
        // Its sound, as 32-bit integers on the 48 kHz time line.
        if (audioOn_ && a.frames > 0) {
            pcm_.resize(size_t(a.frames) * size_t(channels_));
            for (size_t i = 0; i < pcm_.size(); ++i) {
                const double v = a.samples && a.channels == channels_ ? std::clamp(double(a.samples[i]), -1.0, 1.0) : 0.0;
                pcm_[i] = int32_t(std::lround(v * 2147483647.0));
            }
            uint32_t written = 0;
            output_->ScheduleAudioSamples(pcm_.data(), uint32_t(a.frames), audioTime_, 48000, &written);
            audioTime_ += a.frames;
        }
        ++scheduled_;
        if (!started_ && scheduled_ >= kPreroll) {
            if (audioOn_) output_->EndAudioPreroll();
            started_ = output_->StartScheduledPlayback(0, scale_, 1.0) == S_OK;
            if (!started_) return false;
        }
        return true;
    }

    void close() override {
        if (output_) {
            if (started_) output_->StopScheduledPlayback(0, nullptr, 0);
            output_->SetScheduledFrameCompletionCallback(nullptr);
            if (audioOn_) output_->DisableAudioOutput();
            if (videoOn_) output_->DisableVideoOutput();
        }
        for (Slot& s : slots_) {
            if (s.frame) s.frame->Release();
            s.frame = nullptr;
            s.hdr.inner = nullptr;
            s.busy = false;
        }
        output_.reset();
        deckLink_.reset();
        started_ = videoOn_ = audioOn_ = false;
    }
    void wake() override {
        std::lock_guard lock(m_);
        stopped_ = true;
        cv_.notify_all();
    }

    // IDeckLinkVideoOutputCallback
    HRESULT ScheduledFrameCompleted(IDeckLinkVideoFrame* done, BMDOutputFrameCompletionResult) override {
        std::lock_guard lock(m_);
        for (Slot& s : slots_)
            if (done == s.frame || done == static_cast<IDeckLinkVideoFrame*>(&s.hdr)) s.busy = false;
        cv_.notify_all();
        return S_OK;
    }
    HRESULT ScheduledPlaybackHasStopped() override { return S_OK; }
    HRESULT QueryInterface(REFIID iid, LPVOID* out) override {
        if (sameIid(iid, IID_IDeckLinkVideoOutputCallback) || sameIid(iid, IID_IUnknown)) {
            *out = static_cast<IDeckLinkVideoOutputCallback*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    // (Owned by Montage, not by its count.)
    ULONG AddRef() override { return 1; }
    ULONG Release() override { return 1; }

private:
    static constexpr int kFrames = 5, kPreroll = 3;
    struct Slot {
        IDeckLinkMutableVideoFrame* frame = nullptr;
        HdrFrame hdr;
        bool busy = false;
    };
    Ref<IDeckLink> deckLink_;
    Ref<IDeckLinkOutput> output_;
    Signal signal_;
    BMDPixelFormat format_ = bmdFormat10BitYUV;
    BMDTimeValue duration_ = 0;
    BMDTimeScale scale_ = 0;
    int width_ = 0, height_ = 0, channels_ = 0;
    size_t rowBytes_ = 0;
    bool videoOn_ = false, audioOn_ = false, started_ = false, hdrCard_ = false;
    int64_t scheduled_ = 0, audioTime_ = 0;
    Slot slots_[kFrames];
    std::vector<int32_t> pcm_;
    std::mutex m_;
    std::condition_variable cv_;
    bool stopped_ = false;
};

}  // namespace

std::shared_ptr<Backend> makeDeckLinkBackend() { return std::make_shared<DeckLinkBackend>(); }

}  // namespace montage::output

#else

namespace montage::output {

namespace {

class DeckLinkBackend : public Backend {
public:
    std::string name() const override { return "DeckLink"; }
    bool available(std::string* why) override {
        if (why) *why = "DeckLink output is not in the Windows build yet (NDI is)";
        return false;
    }
    std::vector<Device> devices() override { return {}; }
    bool open(const Device&, const Mode&, const Signal&, int, int, std::string* error) override {
        available(error);
        return false;
    }
    bool send(const VideoFrame&, const Audio&) override { return false; }
    void close() override {}
};

}  // namespace

std::shared_ptr<Backend> makeDeckLinkBackend() { return std::make_shared<DeckLinkBackend>(); }

}  // namespace montage::output

#endif
