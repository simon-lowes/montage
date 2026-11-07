// Montage — what Montage and a montage-pluginhost process say to each other (audio/PluginHost.h), laid out in the
// shared memory of one channel. Internal to the two.
//
// A channel is one block of shared memory: a header (the message and its reply, parameter changes queued with it),
// a payload area for longer data (states, parameter lists, effect descriptions, editor events) and, for an audio
// instance, its audio: kMaxChannels main and kMaxChannels key channels of kMaxFrames samples. Montage writes a message,
// bumps `toHost` and rings; the host does it, writes the reply, bumps `toClient` and rings back. Only one message is
// out at a time on a channel; a reply always names the message it answers (`replySeq`), so a reply that comes after
// Montage stopped waiting for it is recognised and set aside.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace montage::plugins::wire {

constexpr uint32_t kMagic = 0x3148504d;  // "MPH1"
constexpr uint32_t kVersion = 2;
constexpr int kMaxChannels = 32;
constexpr int kMaxFrames = 8192;
constexpr int kMaxParamChanges = 1024;
constexpr size_t kPayload = size_t(1) << 20;

enum Command : uint32_t {
    Nop = 0,
    // On a host's control channel.
    Attach,  // payload "<audio|ofx>\n<channel name>\n<descriptor JSON>"; the reply says what the instance has
    Quit,
    // On an instance's channel.
    Activate,  // argD sample rate, argI max frames
    Process,   // frames, channels, keyChannels (0: silence), keyEnable, transport; audio in place
    Parameters,
    GetParameter,  // argU id
    Flush,         // only the queued parameter changes
    SaveState,     // reply: resultI the size, the first part in the payload (FetchBlob for the rest)
    FetchBlob,     // argI offset
    PutBlob,       // argI offset, argU the whole size, a part in the payload
    LoadState,     // the state put with PutBlob (argU its size), or in the payload
    Reset,
    SetOffline,       // argI
    SetMainChannels,  // argI
    OpenEditor,       // argU parent window (0: a window of the host's own), reply resultI width, resultI2 height
    EditorIdle,
    CloseEditor,
    SetEditorSize,  // argI width, argI2 height
    Destroy,
    // On an OpenFX channel.
    OfxRender,      // payload "<effect JSON>\0<plug-in JSON>\0<colour space id>"; images in the arena
    CallbackReply,  // Montage's answer to the host's question (a frame at another time, an input)
};

// What the host's reply is (replyKind).
enum ReplyKind : uint32_t { Final = 0, Callback = 1 };
enum CallbackKind : int32_t { FetchFrame = 1, ResolveInput = 2 };

// Bits of `flags`.
enum : uint32_t {
    kTransport = 1,    // position, playing, bpm and beatsPerBar are set
    kPlaying = 2,
    kPayloadInBlob = 4,  // the message's payload was put with PutBlob
    kHasFetch = 8,       // (OpenFX) Montage can give frames at other times
    kHasInputs = 16,     // (OpenFX) and connected inputs
    kHasMask = 32,       // (OpenFX) the effect's own mask follows the pictures in the arena
};

// Bits of `resultFlags` (Attach).
enum : uint32_t { kHasSidechain = 1, kHasEditor = 2, kEditorResizable = 4, kOwnWindow = 8 };

// Editor events in a reply's payload: one byte of kind, then id (4), value (8), width (4), height (4).
enum EventKind : uint8_t { EvParameter = 1, EvGestureBegin = 2, EvGestureEnd = 3, EvResize = 4, EvClosed = 5 };
constexpr size_t kEventBytes = 1 + 4 + 8 + 4 + 4;

struct ParamChange {
    uint32_t id;
    uint32_t pad;
    double value;
};

struct Header {
    uint32_t magic;
    uint32_t version;
    // Set by the host as it crashes (the signal, or on Windows the exception code), before the process has ended:
    // a core dump or a crash reporter can keep a crashed process from ending for seconds.
    std::atomic<uint32_t> crashed;
    alignas(64) std::atomic<uint32_t> toHost;
    alignas(64) std::atomic<uint32_t> toClient;
    alignas(64) uint32_t command;
    uint32_t seq;
    int32_t frames, channels, keyChannels, keyEnable;  // keyEnable: -1 unchanged, else 0 or 1
    uint32_t flags;
    int32_t beatsPerBar;
    int64_t position;
    double bpm;
    double argD, argD2, argD3;
    int64_t argI, argI2, argI3;
    uint64_t argU;
    uint32_t paramCount;
    uint32_t pad0;
    ParamChange params[kMaxParamChanges];
    // (OpenFX) the shared block pictures travel in, made larger by Montage when needed (a new name each time).
    char arenaName[64];
    uint64_t arenaSize;
    uint32_t arenaGen;
    uint32_t pad1;
    // The reply.
    uint32_t replySeq, replyKind;
    int32_t status;   // 1 done, 0 failed
    int32_t latency;  // the plug-in's latency after the message
    double resultD;
    int64_t resultI, resultI2;
    uint32_t resultFlags;
    uint32_t pad2;
    uint64_t payloadBytes;  // in the payload, either way
};

constexpr size_t alignUp(size_t v, size_t a) { return (v + a - 1) / a * a; }
constexpr size_t kPayloadOffset = alignUp(sizeof(Header), 4096);
constexpr size_t kAudioOffset = alignUp(kPayloadOffset + kPayload, 4096);
constexpr size_t kChannelSamples = size_t(kMaxFrames);
constexpr size_t kChannelBytes = kAudioOffset + size_t(2 * kMaxChannels) * kChannelSamples * sizeof(float);

inline Header* header(void* base) { return static_cast<Header*>(base); }
inline char* payload(void* base) { return static_cast<char*>(base) + kPayloadOffset; }
// Main channel c (key: c + kMaxChannels).
inline float* audio(void* base, int c) {
    return reinterpret_cast<float*>(static_cast<char*>(base) + kAudioOffset) + size_t(c) * kChannelSamples;
}

// Exit codes a host ends with on purpose.
constexpr int kExitTooMuchMemory = 86;
constexpr int kExitLostMontage = 87;

}  // namespace montage::plugins::wire
