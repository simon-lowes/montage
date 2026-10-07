// Audio Unit hosting on macOS, through the AudioComponent (AUv2) API that
// every Audio Unit answers, including AUv3 extensions bridged by the system.
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "Plugins.h"

namespace montage::plugins {

namespace {

OSType fourcc(const std::string& s) {
    if (s.size() != 4) return 0;
    return OSType(uint8_t(s[0])) << 24 | OSType(uint8_t(s[1])) << 16 | OSType(uint8_t(s[2])) << 8 | OSType(uint8_t(s[3]));
}

// "aufx:lpas:appl" -> type, subtype, manufacturer.
bool parseId(const std::string& id, AudioComponentDescription& d) {
    if (id.size() != 14 || id[4] != ':' || id[9] != ':') return false;
    d = AudioComponentDescription{};
    d.componentType = fourcc(id.substr(0, 4));
    d.componentSubType = fourcc(id.substr(5, 4));
    d.componentManufacturer = fourcc(id.substr(10, 4));
    return true;
}

std::string cfString(CFStringRef s) {
    if (!s) return {};
    char buf[512] = {};
    CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8);
    return buf;
}

class AuInstance final : public Instance {
public:
    ~AuInstance() override {
        if (!unit_) return;
        if (initialized_) AudioUnitUninitialize(unit_);
        AudioComponentInstanceDispose(unit_);
    }

    bool create(const Descriptor& d, std::string* error) {
        AudioComponentDescription desc{};
        if (!parseId(d.pluginId, desc)) {
            if (error) *error = "invalid Audio Unit id";
            return false;
        }
        AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
        if (!comp) {
            if (error) *error = "the Audio Unit is not installed";
            return false;
        }
        if (AudioComponentInstanceNew(comp, &unit_) != noErr || !unit_) {
            unit_ = nullptr;
            if (error) *error = "the Audio Unit could not be created";
            return false;
        }
        return true;
    }

    bool activate(double sampleRate, int maxFrames) override {
        if (initialized_) {
            AudioUnitUninitialize(unit_);
            initialized_ = false;
        }
        sampleRate_ = sampleRate;
        maxFrames_ = std::max(1, maxFrames);
        // Stereo in and out if the unit takes it, else mono.
        channels_ = 0;
        for (UInt32 ch : {2u, 1u}) {
            AudioStreamBasicDescription f{};
            f.mSampleRate = sampleRate;
            f.mFormatID = kAudioFormatLinearPCM;
            f.mFormatFlags = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved;
            f.mBytesPerPacket = 4;
            f.mFramesPerPacket = 1;
            f.mBytesPerFrame = 4;
            f.mChannelsPerFrame = ch;
            f.mBitsPerChannel = 32;
            if (AudioUnitSetProperty(unit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f) == noErr &&
                AudioUnitSetProperty(unit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &f, sizeof f) == noErr) {
                channels_ = int(ch);
                break;
            }
        }
        if (!channels_) return false;
        UInt32 maxSlice = UInt32(maxFrames_);
        AudioUnitSetProperty(unit_, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxSlice,
                             sizeof maxSlice);
        AURenderCallbackStruct cb{&AuInstance::pullInput, this};
        if (AudioUnitSetProperty(unit_, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb) != noErr)
            return false;
        if (AudioUnitInitialize(unit_) != noErr) return false;
        initialized_ = true;
        in_.assign(size_t(channels_) * size_t(maxFrames_), 0.0f);
        out_.assign(size_t(channels_) * size_t(maxFrames_), 0.0f);
        abl_.resize(sizeof(AudioBufferList) + sizeof(AudioBuffer) * size_t(channels_));
        return true;
    }

    void process(float* const* channels, int numChannels, int frames) override {
        if (!initialized_ || numChannels <= 0) return;
        for (int offset = 0; offset < frames; offset += maxFrames_) {
            const int n = std::min(maxFrames_, frames - offset);
            for (int c = 0; c < channels_; ++c) {
                float* dst = &in_[size_t(c) * size_t(maxFrames_)];
                if (channels_ == 1 && numChannels >= 2) {
                    for (int i = 0; i < n; ++i) dst[i] = 0.5f * (channels[0][offset + i] + channels[1][offset + i]);
                } else {
                    std::memcpy(dst, channels[std::min(c, numChannels - 1)] + offset, size_t(n) * sizeof(float));
                }
            }
            pullFrames_ = n;
            auto* abl = reinterpret_cast<AudioBufferList*>(abl_.data());
            abl->mNumberBuffers = UInt32(channels_);
            for (int c = 0; c < channels_; ++c) {
                abl->mBuffers[c].mNumberChannels = 1;
                abl->mBuffers[c].mDataByteSize = UInt32(n) * 4;
                abl->mBuffers[c].mData = &out_[size_t(c) * size_t(maxFrames_)];
            }
            AudioTimeStamp ts{};
            ts.mSampleTime = sampleTime_;
            ts.mFlags = kAudioTimeStampSampleTimeValid;
            AudioUnitRenderActionFlags flags = 0;
            const OSStatus rc = AudioUnitRender(unit_, &flags, &ts, 0, UInt32(n), abl);
            sampleTime_ += n;
            if (rc != noErr) continue;  // leave the audio untouched
            for (int c = 0; c < numChannels; ++c) {
                const auto* src = static_cast<const float*>(abl->mBuffers[std::min(c, channels_ - 1)].mData);
                std::memcpy(channels[c] + offset, src, size_t(n) * sizeof(float));
            }
        }
    }

    std::vector<ParamInfo> parameters() override {
        std::vector<ParamInfo> out;
        UInt32 size = 0;
        if (AudioUnitGetPropertyInfo(unit_, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, &size, nullptr) != noErr ||
            size == 0)
            return out;
        std::vector<AudioUnitParameterID> ids(size / sizeof(AudioUnitParameterID));
        if (AudioUnitGetProperty(unit_, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, ids.data(), &size) != noErr)
            return out;
        for (AudioUnitParameterID id : ids) {
            AudioUnitParameterInfo info{};
            UInt32 isz = sizeof info;
            if (AudioUnitGetProperty(unit_, kAudioUnitProperty_ParameterInfo, kAudioUnitScope_Global, id, &info, &isz) != noErr)
                continue;
            if (!(info.flags & kAudioUnitParameterFlag_IsWritable)) continue;
            ParamInfo p;
            p.id = id;
            p.name = (info.flags & kAudioUnitParameterFlag_HasCFNameString) ? cfString(info.cfNameString) : info.name;
            if ((info.flags & kAudioUnitParameterFlag_HasCFNameString) && (info.flags & kAudioUnitParameterFlag_CFNameRelease))
                CFRelease(info.cfNameString);
            p.min = info.minValue;
            p.max = info.maxValue;
            p.def = info.defaultValue;
            p.automatable = true;
            p.stepped = info.unit == kAudioUnitParameterUnit_Indexed || info.unit == kAudioUnitParameterUnit_Boolean;
            out.push_back(p);
        }
        return out;
    }

    double parameter(uint32_t id) override {
        AudioUnitParameterValue v = 0;
        AudioUnitGetParameter(unit_, id, kAudioUnitScope_Global, 0, &v);
        return v;
    }

    void setParameter(uint32_t id, double value) override {
        AudioUnitSetParameter(unit_, id, kAudioUnitScope_Global, 0, AudioUnitParameterValue(value), 0);
    }

    std::string saveState() override {
        CFPropertyListRef plist = nullptr;
        UInt32 size = sizeof plist;
        if (AudioUnitGetProperty(unit_, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &plist, &size) != noErr || !plist)
            return {};
        std::string out;
        if (CFDataRef data = CFPropertyListCreateData(nullptr, plist, kCFPropertyListBinaryFormat_v1_0, 0, nullptr)) {
            out.assign(reinterpret_cast<const char*>(CFDataGetBytePtr(data)), size_t(CFDataGetLength(data)));
            CFRelease(data);
        }
        CFRelease(plist);
        return out;
    }

    bool loadState(const std::string& state) override {
        if (state.empty()) return false;
        CFDataRef data = CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(state.data()), CFIndex(state.size()));
        if (!data) return false;
        CFPropertyListRef plist = CFPropertyListCreateWithData(nullptr, data, kCFPropertyListImmutable, nullptr, nullptr);
        CFRelease(data);
        if (!plist) return false;
        const bool ok =
            AudioUnitSetProperty(unit_, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &plist, sizeof plist) == noErr;
        CFRelease(plist);
        return ok;
    }

    int latencySamples() override {
        Float64 seconds = 0;
        UInt32 size = sizeof seconds;
        if (AudioUnitGetProperty(unit_, kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0, &seconds, &size) != noErr)
            return 0;
        return int(seconds * sampleRate_ + 0.5);
    }

    void reset() override {
        if (initialized_) AudioUnitReset(unit_, kAudioUnitScope_Global, 0);
    }

private:
    // The unit pulls its input through this callback during AudioUnitRender.
    static OSStatus pullInput(void* ref, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32 frames,
                              AudioBufferList* io) {
        auto* self = static_cast<AuInstance*>(ref);
        const UInt32 n = std::min<UInt32>(frames, UInt32(self->pullFrames_));
        for (UInt32 b = 0; b < io->mNumberBuffers; ++b) {
            float* src = &self->in_[size_t(std::min<int>(int(b), self->channels_ - 1)) * size_t(self->maxFrames_)];
            if (!io->mBuffers[b].mData) {
                io->mBuffers[b].mData = src;  // the unit asked for our buffer
            } else {
                std::memcpy(io->mBuffers[b].mData, src, size_t(n) * sizeof(float));
                if (n < frames) std::memset(static_cast<float*>(io->mBuffers[b].mData) + n, 0, size_t(frames - n) * sizeof(float));
            }
            io->mBuffers[b].mDataByteSize = frames * 4;
        }
        return noErr;
    }

    AudioUnit unit_ = nullptr;
    bool initialized_ = false;
    double sampleRate_ = 48000;
    int maxFrames_ = 1024;
    int channels_ = 2;
    int pullFrames_ = 0;
    Float64 sampleTime_ = 0;
    std::vector<float> in_, out_;
    std::vector<char> abl_;
};

}  // namespace

std::unique_ptr<Instance> instantiateAudioUnit(const Descriptor& d, std::string* error) {
    auto inst = std::make_unique<AuInstance>();
    if (!inst->create(d, error)) return nullptr;
    return inst;
}

}  // namespace montage::plugins
