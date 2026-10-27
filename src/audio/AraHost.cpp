#include "AraHost.h"

#include <QByteArray>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#include "ARA_Library/Dispatch/ARAHostDispatch.h"
#include "DialogueAlign.h"
#include "PluginEffect.h"
#include "SpeechCleanup.h"
#include "media/MediaPool.h"

namespace montage::ara {

using namespace ARA;
using namespace ARA::Host;

namespace {

using DocumentProps = SizedStruct<ARA_STRUCT_MEMBER(ARADocumentProperties, name)>;
using MusicalContextProps = SizedStruct<ARA_STRUCT_MEMBER(ARAMusicalContextProperties, color)>;
using RegionSequenceProps = SizedStruct<ARA_STRUCT_MEMBER(ARARegionSequenceProperties, color)>;
using AudioSourceProps = SizedStruct<ARA_STRUCT_MEMBER(ARAAudioSourceProperties, channelArrangement)>;
using AudioModificationProps = SizedStruct<ARA_STRUCT_MEMBER(ARAAudioModificationProperties, persistentID)>;
using PlaybackRegionProps = SizedStruct<ARA_STRUCT_MEMBER(ARAPlaybackRegionProperties, color)>;
using InterfaceConfig = SizedStruct<ARA_STRUCT_MEMBER(ARAInterfaceConfiguration, assertFunctionAddress)>;

// ARA is initialised once per factory (per loaded plug-in file) while any document of it is open.
std::mutex gInitMutex;
std::map<const ARAFactory*, int> gInitCount;
ARAAssertFunction gAssert = nullptr;  // no assertions reported back

std::atomic<int> gRegions{0};
std::atomic<int> gRenders{0};

std::mutex gLogMutex;
bool gLogging = false;
std::map<std::string, ReadLog> gLog;

// Archives as stored: the document archive id, a zero byte, then the plug-in's bytes.
std::string archiveIdOf(const std::string& stored) {
    const size_t z = stored.find('\0');
    return z == std::string::npos ? std::string() : stored.substr(0, z);
}
std::string archiveBytesOf(const std::string& stored) {
    const size_t z = stored.find('\0');
    return z == std::string::npos ? std::string() : stored.substr(z + 1);
}

// The plug-in's content readers, read into Montage's structs.
template <typename Event, typename Fn>
void readContent(DocumentController& dc, ARAAudioSourceRef source, ARAContentType type, Fn&& fn) {
    if (!dc.isAudioSourceContentAvailable(source, type)) return;
    ARAContentReaderRef reader = dc.createAudioSourceContentReader(source, type, nullptr);
    if (!reader) return;
    const ARAInt32 n = dc.getContentReaderEventCount(reader);
    for (ARAInt32 i = 0; i < n; ++i)
        if (const auto* ev = static_cast<const Event*>(dc.getContentReaderDataForEvent(reader, i))) fn(*ev);
    dc.destroyContentReader(reader);
}

}  // namespace

Timeline timelineOf(const Effect& e) {
    Timeline t;
    t.bpm = std::clamp(e.p("tempo_bpm", 0, 120), 10.0, 1000.0);
    t.beatsPerBar = std::clamp(int(std::lround(e.p("beats_per_bar", 0, 4))), 1, 32);
    return t;
}

struct Session::Impl final : AudioAccessControllerInterface,
                             ArchivingControllerInterface,
                             ContentAccessControllerInterface,
                             ModelUpdateControllerInterface,
                             PlaybackControllerInterface {
    plugins::Descriptor desc;
    std::shared_ptr<const void> library;  // the plug-in's file, loaded until the document is gone
    const ARAFactory* factory = nullptr;
    bool initialised = false;
    DocumentControllerHostInstance hostInstance;
    std::unique_ptr<DocumentController> dc;
    std::unique_ptr<plugins::Instance> inst;
    const ARAPlugInExtensionInstance* extension = nullptr;
    uint32_t roles = 0;

    AudioBufferPtr source;
    std::string sourceId, modificationId;
    Timeline timeline;
    ARAMusicalContextRef context = nullptr;
    ARARegionSequenceRef sequence = nullptr;
    ARAAudioSourceRef audioSource = nullptr;
    ARAAudioModificationRef modification = nullptr;
    ARAPlaybackRegionRef region = nullptr;
    bool samplesAccess = false;
    bool rendered = false;

    // The musical context's content, as the plug-in reads it.
    std::vector<ARAContentTempoEntry> tempo;
    std::vector<ARAContentBarSignature> bars;
    struct HostReader {
        ARAContentType type;
        std::vector<ARAContentTempoEntry> tempo;
        std::vector<ARAContentBarSignature> bars;
    };

    // Archives being read or written (only one at a time, on the document's thread).
    struct ArchiveIn {
        std::string id, bytes;
    };
    struct ArchiveOut {
        std::string bytes;
    };

    std::atomic<bool> dirty{false};
    std::atomic<bool> analysisDone{false};

    Impl() : hostInstance(this, this, this, this, this) {}

    ~Impl() override {
        // Instances bound to the document go first, then its objects, then the document; ARA itself last.
        inst.reset();
        if (dc) {
            if (samplesAccess) dc->enableAudioSourceSamplesAccess(audioSource, false);
            dc->beginEditing();
            if (region) {
                dc->destroyPlaybackRegion(region);
                --gRegions;
            }
            if (modification) dc->destroyAudioModification(modification);
            if (audioSource) dc->destroyAudioSource(audioSource);
            if (sequence) dc->destroyRegionSequence(sequence);
            if (context) dc->destroyMusicalContext(context);
            dc->endEditing();
            dc->destroyDocumentController();
            dc.reset();
        }
        if (initialised) {
            std::lock_guard lock(gInitMutex);
            if (--gInitCount[factory] == 0) {
                gInitCount.erase(factory);
                factory->uninitializeARA();
            }
        }
    }

    double seconds() const { return double(source->frames()) / double(std::max(1, source->sampleRate)); }

    void describeTimeline() {
        // A steady tempo: two entries give its rate; one bar signature from the start.
        const double secondsPerQuarter = 60.0 / timeline.bpm;
        tempo = {{0.0, 0.0}, {secondsPerQuarter, 1.0}};
        bars = {{timeline.beatsPerBar, 4, 0.0}};
    }

    // --- Audio access: the source, sample-exact, from any thread.
    struct Reader {
        Impl* session;
        bool use64;
    };
    ARAAudioReaderHostRef createAudioReaderForSource(ARAAudioSourceHostRef, bool use64BitSamples) noexcept override {
        return reinterpret_cast<ARAAudioReaderHostRef>(new Reader{this, use64BitSamples});
    }
    bool readAudioSamples(ARAAudioReaderHostRef readerRef, ARASamplePosition position, ARASampleCount count,
                          void* const buffers[]) noexcept override {
        const auto* r = reinterpret_cast<const Reader*>(readerRef);
        if (!r || count < 0) return false;
        const AudioBuffer& b = *source;
        const int64_t n = b.frames();
        const int nch = b.channels;
        for (int ch = 0; ch < nch; ++ch) {
            for (ARASampleCount i = 0; i < count; ++i) {
                const int64_t at = position + i;
                const float v = at >= 0 && at < n ? b.samples[size_t(at) * size_t(nch) + size_t(ch)] : 0.0f;
                if (r->use64) static_cast<double*>(buffers[ch])[i] = v;
                else static_cast<float*>(buffers[ch])[i] = v;
            }
        }
        std::lock_guard lock(gLogMutex);
        if (gLogging) {
            ReadLog& log = gLog[sourceId];
            if (log.seen.size() != b.samples.size()) {
                log.seen.assign(b.samples.size(), std::nanf(""));
                log.channels = nch;
            }
            ++log.reads;
            for (ARASampleCount i = 0; i < count; ++i) {
                const int64_t at = position + i;
                if (at < 0 || at >= n) continue;
                for (int ch = 0; ch < nch; ++ch) {
                    const size_t k = size_t(at) * size_t(nch) + size_t(ch);
                    log.seen[k] = r->use64 ? float(static_cast<double*>(buffers[ch])[i]) : static_cast<float*>(buffers[ch])[i];
                }
            }
        }
        return true;
    }
    void destroyAudioReader(ARAAudioReaderHostRef readerRef) noexcept override { delete reinterpret_cast<Reader*>(readerRef); }

    // --- Archiving.
    ARASize getArchiveSize(ARAArchiveReaderHostRef ref) noexcept override {
        return ARASize(reinterpret_cast<const ArchiveIn*>(ref)->bytes.size());
    }
    bool readBytesFromArchive(ARAArchiveReaderHostRef ref, ARASize position, ARASize length, ARAByte buffer[]) noexcept override {
        const std::string& bytes = reinterpret_cast<const ArchiveIn*>(ref)->bytes;
        if (position > bytes.size() || length > bytes.size() - position) return false;
        std::memcpy(buffer, bytes.data() + position, length);
        return true;
    }
    bool writeBytesToArchive(ARAArchiveWriterHostRef ref, ARASize position, ARASize length, const ARAByte buffer[]) noexcept override {
        std::string& bytes = reinterpret_cast<ArchiveOut*>(ref)->bytes;
        if (length > bytes.max_size() || position > bytes.max_size() - length) return false;  // (the end past what can be held)
        try {
            if (bytes.size() < position + length) bytes.resize(position + length);
        } catch (...) {
            return false;
        }
        std::memcpy(bytes.data() + position, buffer, length);
        return true;
    }
    void notifyDocumentArchivingProgress(float) noexcept override {}
    void notifyDocumentUnarchivingProgress(float) noexcept override {}
    ARAPersistentID getDocumentArchiveID(ARAArchiveReaderHostRef ref) noexcept override {
        return reinterpret_cast<const ArchiveIn*>(ref)->id.c_str();
    }

    // --- Content Montage gives: the musical context's tempo and bars (the audio source has none of its own).
    bool isMusicalContextContentAvailable(ARAMusicalContextHostRef, ARAContentType type) noexcept override {
        return type == kARAContentTypeTempoEntries || type == kARAContentTypeBarSignatures;
    }
    ARAContentGrade getMusicalContextContentGrade(ARAMusicalContextHostRef, ARAContentType) noexcept override {
        return kARAContentGradeApproved;  // set by the editor
    }
    ARAContentReaderHostRef createMusicalContextContentReader(ARAMusicalContextHostRef, ARAContentType type,
                                                              const ARAContentTimeRange*) noexcept override {
        if (!isMusicalContextContentAvailable(nullptr, type)) return nullptr;
        return reinterpret_cast<ARAContentReaderHostRef>(new HostReader{type, tempo, bars});
    }
    bool isAudioSourceContentAvailable(ARAAudioSourceHostRef, ARAContentType) noexcept override { return false; }
    ARAContentGrade getAudioSourceContentGrade(ARAAudioSourceHostRef, ARAContentType) noexcept override {
        return kARAContentGradeInitial;
    }
    ARAContentReaderHostRef createAudioSourceContentReader(ARAAudioSourceHostRef, ARAContentType,
                                                           const ARAContentTimeRange*) noexcept override {
        return nullptr;
    }
    ARAInt32 getContentReaderEventCount(ARAContentReaderHostRef ref) noexcept override {
        const auto* r = reinterpret_cast<const HostReader*>(ref);
        return ARAInt32(r->type == kARAContentTypeTempoEntries ? r->tempo.size() : r->bars.size());
    }
    const void* getContentReaderDataForEvent(ARAContentReaderHostRef ref, ARAInt32 index) noexcept override {
        const auto* r = reinterpret_cast<const HostReader*>(ref);
        if (index < 0) return nullptr;
        if (r->type == kARAContentTypeTempoEntries) return size_t(index) < r->tempo.size() ? &r->tempo[size_t(index)] : nullptr;
        return size_t(index) < r->bars.size() ? &r->bars[size_t(index)] : nullptr;
    }
    void destroyContentReader(ARAContentReaderHostRef ref) noexcept override { delete reinterpret_cast<HostReader*>(ref); }

    // --- Model updates from the plug-in.
    void notifyAudioSourceAnalysisProgress(ARAAudioSourceHostRef, ARAAnalysisProgressState state, float) noexcept override {
        if (state == kARAAnalysisProgressCompleted) analysisDone = true;
    }
    void notifyAudioSourceContentChanged(ARAAudioSourceHostRef, const ARAContentTimeRange*, ContentUpdateScopes) noexcept override {
        dirty = true;
    }
    void notifyAudioModificationContentChanged(ARAAudioModificationHostRef, const ARAContentTimeRange*,
                                               ContentUpdateScopes) noexcept override {
        dirty = true;
    }
    void notifyPlaybackRegionContentChanged(ARAPlaybackRegionHostRef, const ARAContentTimeRange*, ContentUpdateScopes) noexcept override {
        dirty = true;
    }
    void notifyDocumentDataChanged() noexcept override { dirty = true; }

    // --- Transport requests from the plug-in's editor: Montage's own transport stays in charge.
    void requestStartPlayback() noexcept override {}
    void requestStopPlayback() noexcept override {}
    void requestSetPlaybackPosition(ARATimePosition) noexcept override {}
    void requestSetCycleRange(ARATimePosition, ARATimeDuration) noexcept override {}
    void requestEnableCycle(bool) noexcept override {}

    bool analysisIncomplete() {
        for (ARASize i = 0; i < factory->analyzeableContentTypesCount; ++i)
            if (dc->isAudioSourceContentAnalysisIncomplete(audioSource, factory->analyzeableContentTypes[i])) return true;
        return false;
    }

    // Asks for every analysis the plug-in offers and waits for it.
    bool analyseAll(int timeoutMs, const std::atomic<bool>* cancel) {
        if (factory->analyzeableContentTypesCount == 0) return true;
        if (!analysisIncomplete()) return true;
        dc->requestAudioSourceContentAnalysis(audioSource, factory->analyzeableContentTypesCount, factory->analyzeableContentTypes);
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (true) {
            dc->notifyModelUpdates();
            if (!analysisIncomplete()) return true;
            if ((cancel && cancel->load()) || std::chrono::steady_clock::now() > until) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
};

Session::Session(std::unique_ptr<Impl> impl) : d_(std::move(impl)) {}
Session::~Session() = default;

std::unique_ptr<Session> Session::open(const plugins::Descriptor& desc, const std::string& archive, AudioBufferPtr source,
                                       const std::string& sourceId, const std::string& modificationId,
                                       const Timeline& timeline, uint32_t roles, std::string* error) {
    auto fail = [&](const std::string& msg) -> std::unique_ptr<Session> {
        if (error) *error = msg;
        return nullptr;
    };
    if (!source || source->frames() <= 0) return fail("the clip has no sound");
    if (source->channels > 2) return fail("ARA plug-ins take mono or stereo sound here");
    auto impl = std::make_unique<Impl>();
    impl->desc = desc;
    impl->source = std::move(source);
    impl->sourceId = sourceId;
    impl->modificationId = modificationId;
    impl->timeline = timeline;
    impl->roles = roles;
    impl->describeTimeline();
    std::string err;
    impl->inst = plugins::instantiate(desc, &err);
    if (!impl->inst) return fail("the plug-in could not be loaded: " + err);
    impl->library = impl->inst->library();
    impl->factory = static_cast<const ARAFactory*>(impl->inst->araFactory());
    const ARAFactory* f = impl->factory;
    if (!f) return fail(desc.name + " offers no ARA here");
    if (f->highestSupportedApiGeneration < kARAAPIGeneration_2_0_Final || f->lowestSupportedApiGeneration > kARAAPIGeneration_2_0_Final)
        return fail(desc.name + " does not support ARA 2");
    {
        std::lock_guard lock(gInitMutex);
        if (gInitCount[f]++ == 0) {
            const InterfaceConfig config{kARAAPIGeneration_2_0_Final, &gAssert};
            f->initializeARAWithConfiguration(&config);
        }
        impl->initialised = true;
    }
    const DocumentProps docProps{"Montage"};
    const ARADocumentControllerInstance* dci = f->createDocumentControllerWithDocument(&impl->hostInstance, &docProps);
    if (!dci) return fail(desc.name + " could not open an ARA document");
    impl->dc = std::make_unique<DocumentController>(dci);
    DocumentController& dc = *impl->dc;

    const AudioBuffer& b = *impl->source;
    const double seconds = impl->seconds();
    const std::string name = "Montage";
    dc.beginEditing();
    const MusicalContextProps contextProps{name.c_str(), 0, nullptr};
    impl->context = dc.createMusicalContext(reinterpret_cast<ARAMusicalContextHostRef>(impl.get()), &contextProps);
    const RegionSequenceProps sequenceProps{name.c_str(), 0, impl->context, nullptr};
    impl->sequence = dc.createRegionSequence(reinterpret_cast<ARARegionSequenceHostRef>(impl.get()), &sequenceProps);
    const AudioSourceProps sourceProps{name.c_str(), impl->sourceId.c_str(), b.frames(), double(b.sampleRate), b.channels, false,
                                       kARAChannelArrangementUndefined, nullptr};
    impl->audioSource = dc.createAudioSource(reinterpret_cast<ARAAudioSourceHostRef>(impl.get()), &sourceProps);
    const AudioModificationProps modProps{name.c_str(), impl->modificationId.c_str()};
    impl->modification = dc.createAudioModification(impl->audioSource, reinterpret_cast<ARAAudioModificationHostRef>(impl.get()), &modProps);
    // The region plays the whole source at its own time: Montage's clip then cuts and places what the plug-in made.
    const PlaybackRegionProps regionProps{kARAPlaybackTransformationNoChanges, 0.0, seconds, 0.0, seconds, impl->context,
                                          impl->sequence, name.c_str(), nullptr};
    impl->region = dc.createPlaybackRegion(impl->modification, reinterpret_cast<ARAPlaybackRegionHostRef>(impl.get()), &regionProps);
    if (impl->region) ++gRegions;
    if (!archive.empty()) {
        Impl::ArchiveIn in{archiveIdOf(archive), archiveBytesOf(archive)};
        bool known = in.id == (f->documentArchiveID ? f->documentArchiveID : "");
        for (ARASize i = 0; i < f->compatibleDocumentArchiveIDsCount && !known; ++i)
            known = in.id == f->compatibleDocumentArchiveIDs[i];
        // (An archive of another plug-in or version it cannot read is left aside: the document starts afresh.)
        if (known && !in.bytes.empty()) dc.restoreObjectsFromArchive(reinterpret_cast<ARAArchiveReaderHostRef>(&in), nullptr);
    }
    dc.endEditing();
    if (!impl->context || !impl->sequence || !impl->audioSource || !impl->modification || !impl->region)
        return fail(desc.name + " refused the ARA document");
    dc.enableAudioSourceSamplesAccess(impl->audioSource, true);
    impl->samplesAccess = true;

    // The instance plays the clip's channels, bound to the document in its roles.
    impl->inst->setMainChannels(b.channels);
    impl->extension = static_cast<const ARAPlugInExtensionInstance*>(impl->inst->bindToAra(dc.getRef(), kAllRoles, roles));
    if (!impl->extension) return fail(desc.name + " could not be bound to the ARA document");
    if (roles & kPlaybackRenderer) {
        if (!impl->extension->playbackRendererInterface) return fail(desc.name + " cannot render ARA playback");
        PlaybackRenderer(impl->extension).addPlaybackRegion(impl->region);
    }
    // An editor shows the clip's region: heard through its editor renderer, and selected in its view.
    if ((roles & kEditorRenderer) && impl->extension->editorRendererInterface)
        EditorRenderer(impl->extension).addPlaybackRegion(impl->region);
    if ((roles & kEditorView) && impl->extension->editorViewInterface) {
        const ARAPlaybackRegionRef regions[] = {impl->region};
        const ARAContentTimeRange range{0.0, seconds};
        const SizedStruct<ARA_STRUCT_MEMBER(ARAViewSelection, timeRange)> selection{1, regions, 0, nullptr, &range};
        EditorView(impl->extension).notifySelection(&selection);
    }
    impl->dirty = false;
    return std::unique_ptr<Session>(new Session(std::move(impl)));
}

plugins::Instance* Session::instance() { return d_->inst.get(); }
const plugins::Descriptor& Session::descriptor() const { return d_->desc; }

bool Session::render(AudioBuffer& out, const std::atomic<bool>* cancel) {
    Impl& s = *d_;
    if (!(s.roles & kPlaybackRenderer) || !s.inst) return false;
    // Plug-ins that render from their analysis (pitch and timing editors) need it done first.
    if (!s.analyseAll(10 * 60 * 1000, cancel)) return false;
    const AudioBuffer& b = *s.source;
    const int nch = b.channels;
    const int64_t n = b.frames();
    constexpr int kBlock = 2048;  // ARA renderers need no live input: large blocks
    if (!s.rendered) {
        s.inst->setOffline(true);
        if (!s.inst->activate(double(b.sampleRate), kBlock)) return false;
        s.rendered = true;
    } else {
        s.inst->reset();
    }
    // The plug-in's latency: render that much further and drop as much from the front.
    const int64_t latency = std::max(0, s.inst->latencySamples());
    out.sampleRate = b.sampleRate;
    out.channels = nch;
    out.samples.assign(size_t(n) * size_t(nch), 0.0f);
    std::vector<std::vector<float>> planes(static_cast<size_t>(nch), std::vector<float>(kBlock));
    std::vector<float*> ptrs(static_cast<size_t>(nch));
    s.inst->setTransport(0, true, s.timeline.bpm, s.timeline.beatsPerBar);
    for (int64_t pos = 0; pos < n + latency; pos += kBlock) {
        if (cancel && cancel->load()) return false;
        const int frames = int(std::min<int64_t>(kBlock, n + latency - pos));
        for (int ch = 0; ch < nch; ++ch) {
            std::fill(planes[size_t(ch)].begin(), planes[size_t(ch)].begin() + frames, 0.0f);
            ptrs[size_t(ch)] = planes[size_t(ch)].data();
        }
        s.inst->process(ptrs.data(), nch, frames);
        for (int i = 0; i < frames; ++i) {
            const int64_t at = pos + i - latency;
            if (at < 0 || at >= n) continue;
            for (int ch = 0; ch < nch; ++ch) out.samples[size_t(at) * size_t(nch) + size_t(ch)] = planes[size_t(ch)][size_t(i)];
        }
    }
    s.inst->setTransport(0, false, s.timeline.bpm, s.timeline.beatsPerBar);
    ++gRenders;
    return true;
}

Content Session::content() {
    Impl& s = *d_;
    Content c;
    DocumentController& dc = *s.dc;
    dc.notifyModelUpdates();
    readContent<ARAContentNote>(dc, s.audioSource, kARAContentTypeNotes, [&](const ARAContentNote& e) {
        Note n;
        n.start = e.startPosition;
        n.duration = e.noteDuration;
        n.frequency = e.frequency;
        n.pitch = e.pitchNumber == kARAInvalidPitchNumber ? -1 : e.pitchNumber;
        n.volume = e.volume;
        c.notes.push_back(n);
    });
    readContent<ARAContentTempoEntry>(dc, s.audioSource, kARAContentTypeTempoEntries,
                                      [&](const ARAContentTempoEntry& e) { c.tempo.push_back({e.timePosition, e.quarterPosition}); });
    readContent<ARAContentBarSignature>(dc, s.audioSource, kARAContentTypeBarSignatures, [&](const ARAContentBarSignature& e) {
        c.bars.push_back({e.numerator, e.denominator, e.position});
    });
    std::vector<Chord> chords;
    readContent<ARAContentChord>(dc, s.audioSource, kARAContentTypeSheetChords, [&](const ARAContentChord& e) {
        Chord ch;
        ch.position = e.position;
        ch.root = e.root;
        ch.bass = e.bass;
        if (e.name) ch.name = e.name;
        chords.push_back(ch);
    });
    for (size_t i = 0; i < chords.size(); ++i)  // each lasts until the next
        chords[i].duration = i + 1 < chords.size() ? chords[i + 1].position - chords[i].position : 0;
    c.chords = std::move(chords);
    c.analysed = !s.analysisIncomplete();
    return c;
}

bool Session::analyse(Content& content, int timeoutMs, const std::atomic<bool>* cancel) {
    const bool ok = d_->analyseAll(timeoutMs, cancel);
    content = this->content();
    return ok;
}

std::string Session::archive() {
    Impl& s = *d_;
    Impl::ArchiveOut out;
    s.dc->notifyModelUpdates();
    if (!s.dc->storeObjectsToArchive(reinterpret_cast<ARAArchiveWriterHostRef>(&out), nullptr)) return {};
    const char* id = s.factory->documentArchiveID ? s.factory->documentArchiveID : "";
    return std::string(id) + '\0' + out.bytes;
}

bool Session::changed() { return d_->dirty.exchange(false); }

void Session::stillChanged() { d_->dirty = true; }

void Session::idle() {
    d_->dc->notifyModelUpdates();
    if (d_->inst) d_->inst->idle();
}


std::string modificationIdFor(const Effect& e) {
    const std::string stored = e.s("modification_id");
    return stored.empty() ? "montage-effect:" + std::to_string(e.id) : stored;
}

std::string sourceIdFor(const Effect& e, const AudioBuffer& source) {
    return "montage-source:" + modificationIdFor(e) + ":" + std::to_string(source.frames()) + "@" +
           std::to_string(source.sampleRate) + "x" + std::to_string(source.channels);
}

namespace {
// The effect's state and parameters, as a "plugin" effect gives them (they do not change over the clip here).
void applySettings(const Effect& e, plugins::Instance& inst) {
    if (const std::string state = plugins::decodeState(e.s("state")); !state.empty()) inst.loadState(state);
    for (const auto& [key, param] : e.params)
        if (key.rfind("param.", 0) == 0) inst.setParameter(uint32_t(std::strtoul(key.c_str() + 6, nullptr, 10)), param.at(0));
}
}  // namespace

bool renderEffect(const Effect& e, const AudioBufferPtr& source, AudioBuffer& out, const std::atomic<bool>* cancel,
                  std::string* error) {
    const auto d = plugins::Registry::instance().find(e.s("plugin_id"));
    if (!d) {
        if (error) *error = "the plug-in is not installed or has not been scanned";
        return false;
    }
    if (!source) return false;
    auto session = Session::open(*d, plugins::decodeState(e.s("archive")), source, sourceIdFor(e, *source), modificationIdFor(e),
                                 timelineOf(e), kPlaybackRenderer, error);
    if (!session) return false;
    applySettings(e, *session->instance());
    if (session->render(out, cancel)) return true;
    if (error && !(cancel && cancel->load())) *error = d->name + " did not finish rendering (its analysis timed out or it failed)";
    return false;
}

bool clipSource(const Project& p, const Sequence& seq, const Clip& c, std::string& key, AudioBufferPtr& buffer, std::string* error,
                const Effect* upTo) {
    const MediaItem* m = c.mediaId ? p.findMedia(c.mediaId) : nullptr;
    if (!m || !m->hasAudio) {
        if (error) *error = "the clip has no sound of its own";
        return false;
    }
    key = audioKey(m->path, c.channels);
    buffer = MediaPool::instance().audio(key, seq.sampleRate);
    if (!buffer || buffer->frames() <= 0) {
        if (error) *error = "the clip's sound could not be read";
        return false;
    }
    if (!upTo) return true;
    // (Chosen as the mixer chooses them: render/Compositor.cpp.)
    std::vector<const Effect*> before;
    for (const Effect& e : c.effects) {
        if (e.id == upTo->id) break;
        if (e.enabled && isSourceAudioEffect(e.type) && dialogueAlignmentApplies(e, c) && bandwidthExtensionApplies(e, c)) before.push_back(&e);
    }
    if (before.empty()) return true;
    if (AudioBufferPtr through = cleanedAudio(key, buffer, before, true); through && through->frames() > 0) {
        buffer = through;
        key += "|through:" + std::to_string(reinterpret_cast<uintptr_t>(through.get()));
    }
    return true;
}

namespace {
struct Live {
    std::string sourceKey;
    std::string archive;  // the effect's archive the document holds
    std::shared_ptr<Session> session;
};
std::mutex gLiveMutex;
std::map<Id, Live> gLive;
}  // namespace

std::shared_ptr<Session> liveSession(const Effect& e, const std::string& sourceKey, const AudioBufferPtr& source, std::string* error) {
    {
        std::lock_guard lock(gLiveMutex);
        auto it = gLive.find(e.id);
        if (it != gLive.end() && it->second.sourceKey == sourceKey && it->second.archive == e.s("archive")) return it->second.session;
    }
    const auto d = plugins::Registry::instance().find(e.s("plugin_id"));
    if (!d) {
        if (error) *error = "the plug-in is not installed or has not been scanned";
        return nullptr;
    }
    if (!source) return nullptr;
    std::shared_ptr<Session> session = Session::open(*d, plugins::decodeState(e.s("archive")), source, sourceIdFor(e, *source),
                                                     modificationIdFor(e), timelineOf(e), kEditorRenderer | kEditorView, error);
    if (!session) return nullptr;
    applySettings(e, *session->instance());
    std::shared_ptr<Session> old;
    {
        std::lock_guard lock(gLiveMutex);
        Live& slot = gLive[e.id];
        old = std::move(slot.session);  // (another source or archive: closed outside the lock)
        slot = {sourceKey, e.s("archive"), session};
    }
    return session;
}

std::shared_ptr<Session> findLiveSession(Id effect) {
    std::lock_guard lock(gLiveMutex);
    auto it = gLive.find(effect);
    return it == gLive.end() ? nullptr : it->second.session;
}

void noteArchiveSaved(Id effect, const std::string& archive) {
    std::lock_guard lock(gLiveMutex);
    if (auto it = gLive.find(effect); it != gLive.end()) it->second.archive = archive;
}

void dropSessionsNotIn(const Project& p) {
    std::map<Id, std::string> present;  // effect -> its archive
    for (const Sequence& s : p.sequences)
        for (const auto* tracks : {&s.videoTracks, &s.audioTracks})
            for (const Track& t : *tracks)
                for (const Clip& c : t.clips)
                    for (const Effect& e : c.effects)
                        if (e.type == "ara") present[e.id] = e.s("archive");
    std::vector<std::shared_ptr<Session>> closing;  // closed outside the lock
    {
        std::lock_guard lock(gLiveMutex);
        for (auto it = gLive.begin(); it != gLive.end();) {
            if (auto e = present.find(it->first); e != present.end() && e->second == it->second.archive) {
                ++it;
                continue;
            }
            closing.push_back(std::move(it->second.session));
            it = gLive.erase(it);
        }
    }
}

void dropAllSessions() {
    std::map<Id, Live> closing;
    std::lock_guard lock(gLiveMutex);
    closing.swap(gLive);
}

int liveRegions() { return gRegions.load(); }
int rendersDone() { return gRenders.load(); }

void logReads(bool on) {
    std::lock_guard lock(gLogMutex);
    gLogging = on;
    if (on) gLog.clear();
}

std::vector<std::pair<std::string, ReadLog>> readsLogged() {
    std::lock_guard lock(gLogMutex);
    return {gLog.begin(), gLog.end()};
}

}  // namespace montage::ara
