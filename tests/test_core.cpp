// Engine tests: keyframes, timecode, edit operations, undo, project I/O.
#include <QtTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "core/AutoTag.h"
#include "core/Captions.h"
#include "core/EditOps.h"
#include "core/Effects.h"
#include "core/History.h"
#include "core/Interchange.h"
#include "core/KeyframeEdit.h"
#include "core/MediaLog.h"
#include "core/Multicam.h"
#include "core/ProjectIO.h"
#include "core/Transcript.h"
#include "core/TranscriptEdit.h"

using namespace montage;
using namespace montage::edit;

namespace {

const TrackRef V1{TrackKind::Video, 0};
const TrackRef V2{TrackKind::Video, 1};
const TrackRef A1{TrackKind::Audio, 0};

// Project with one 30 fps sequence and a 10 s A/V media item (no file needed).
std::string readData(const char* name) {
    QFile f(QStringLiteral(MONTAGE_TEST_DATA_DIR "/") + name);
    return f.open(QIODevice::ReadOnly) ? f.readAll().toStdString() : std::string();
}

struct Fixture {
    Project p = makeDefaultProject();
    Id media = 0;
    Fixture() {
        MediaItem m;
        m.id = p.newId();
        m.kind = MediaKind::Video;
        m.name = "clip.mov";
        m.path = "/nonexistent/clip.mov";
        m.duration = 10.0;
        m.width = 1920;
        m.height = 1080;
        m.fps = {30, 1};
        m.hasVideo = m.hasAudio = true;
        media = m.id;
        p.media.push_back(m);
    }
    Sequence& s() { return *p.active(); }
    Track& v1() { return *trackAt(s(), V1); }
    Track& a1() { return *trackAt(s(), A1); }
    // Places a video-only clip [start, start+len) with source in-point `in`.
    Id put(TrackRef t, FrameTime start, FrameTime len, double in = 0) {
        Clip c = makeClip(p, *p.findMedia(media), t.kind, s());
        c.start = start;
        c.duration = len;
        c.sourceIn = in;
        auto r = overwrite(p, s(), t, c);
        return r.created.at(0);
    }
};

}  // namespace

class TestCore : public QObject {
    Q_OBJECT
private slots:
    void captionTracks() {
        // Wrapping: short text stays on one line; long text splits into two balanced lines.
        QCOMPARE(QString::fromStdString(wrapCaptionText("Hello there")), QString("Hello there"));
        const QString two = QString::fromStdString(
            wrapCaptionText("And so, my fellow Americans, ask not what your country can do for you"));
        const QStringList lines = two.split('\n');
        QCOMPARE(lines.size(), 2);
        QVERIFY(lines[0].size() <= 42 && lines[1].size() <= 42);
        QVERIFY2(lines[0].endsWith(','), qPrintable(two));  // breaks after punctuation

        // Lookup and normalisation.
        CaptionTrack tr;
        tr.captions = {{30, 60, "b"}, {0, 40, "a"}, {70, 70, "empty length"}, {80, 90, "  "}};
        normalizeCaptions(tr.captions);
        QCOMPARE(tr.captions.size(), size_t(2));
        QCOMPARE(tr.captions[0].end, FrameTime(30));  // trimmed to the next caption
        QCOMPARE(captionAt(tr, 10)->text, std::string("a"));
        QCOMPARE(captionAt(tr, 30)->text, std::string("b"));
        QVERIFY(!captionAt(tr, 60));
        QCOMPARE(captionIndexAt(tr, 35), size_t(1));
        QCOMPARE(captionIndexAt(tr, 65), size_t(2));

        // SubRip and WebVTT, out and back in.
        const Rational fps{25, 1};
        std::vector<Caption> caps = {{25, 75, "First line\nsecond line"}, {100, 150, "Second caption"}};
        const std::string srt = captionsToSrt(caps, fps);
        QVERIFY(srt.find("00:00:01,000 --> 00:00:03,000\nFirst line\nsecond line") != std::string::npos);
        std::vector<Caption> back;
        QVERIFY(parseSubtitles(srt, fps, back));
        QCOMPARE(back, caps);
        QVERIFY(parseSubtitles(captionsToVtt(caps, fps), fps, back));
        QCOMPARE(back, caps);
        const std::string vtt =
            "\xEF\xBB\xBFWEBVTT - a title\r\n\r\nNOTE a comment\r\n\r\nSTYLE\r\n::cue { color: yellow }\r\n\r\n"
            "intro\r\n00:01.000 --> 00:02.500 align:start position:10%\r\n<v Roger>Hi &amp; <i>welcome</i>\r\n\r\n"
            "00:00:03.000 --> 00:00:04.000\r\n{\\an8}Top\r\n";
        QVERIFY(parseSubtitles(vtt, fps, back));
        QCOMPARE(back.size(), size_t(2));
        QCOMPARE(back[0].start, FrameTime(25));
        QCOMPARE(back[0].end, FrameTime(63));  // 2.5 s at 25 fps, rounded
        QCOMPARE(back[0].text, std::string("Hi & welcome"));
        QCOMPARE(back[1].text, std::string("Top"));
        std::string err;
        QVERIFY(!parseSubtitles("not subtitles at all", fps, back, &err));
        QVERIFY(!err.empty());

        // Scenarist SCC: pop-on loading, bottom row, parity, drop-frame timecodes.
        const std::string scc = captionsToScc({{30, 90, "HI"}, {300, 360, "Two\nlines"}}, Rational{30000, 1001});
        QVERIFY(scc.rfind("Scenarist_SCC V1.0\n", 0) == 0);
        // "HI" is the only row: preamble 9470 (row 15), tab offset to column 15, then 'H' (c8) 'I' (49).
        QVERIFY2(scc.find("9420 9420 94ae 94ae 9476 9476 9723 9723 c849 942f 942f") != std::string::npos, scc.c_str());
        // Caption 1 shows at frame 30: 11 pairs loaded from frame 20; cleared at 90.
        QVERIFY2(scc.find("\n00:00:00;20\t9420") != std::string::npos, scc.c_str());
        QVERIFY2(scc.find("\n00:00:03;00\t942c 942c") != std::string::npos, scc.c_str());
        // Explicit lines go on rows 14 and 15: "Two" at column 14, "lines" at column 13.
        QVERIFY2(scc.find("94d6 94d6 97a2 97a2") != std::string::npos, scc.c_str());
        QVERIFY2(scc.find("9476 9476 97a1 97a1") != std::string::npos, scc.c_str());
        // Characters outside the basic set: é is basic (5c); Ü is extended (fallback U then 92a4).
        const std::string accents = captionsToScc({{0, 30, "\xC3\xA9\xC3\x9C"}}, Rational{30000, 1001});
        QVERIFY2(accents.find("dcd5 92a4 92a4") != std::string::npos, accents.c_str());

        // Project files keep caption tracks.
        Project p = makeDefaultProject();
        CaptionTrack ct;
        ct.id = p.newId();
        ct.name = "English";
        ct.style.size = 0.07;
        ct.style.boxOpacity = 0;
        ct.captions = caps;
        p.active()->captionTracks.push_back(ct);
        Project q;
        QVERIFY(projectFromJson(projectToJson(p), q));
        QCOMPARE(q.active()->captionTracks, p.active()->captionTracks);
    }

    void captionsFromClipTranscripts() {
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.fps = {25, 1};
        MediaItem m;
        m.id = p.newId();
        m.kind = MediaKind::Audio;
        m.hasAudio = true;
        m.duration = 20;
        m.name = "talk.wav";
        auto t = std::make_shared<Transcript>();
        TranscriptSegment seg;
        const char* words[] = {"One", "two", "three.", "Four", "five", "six."};
        for (int i = 0; i < 6; ++i) seg.words.push_back({2.0 + i * 0.5, 2.4 + i * 0.5, words[i], 1});
        t->segments.push_back(seg);
        m.transcript = t;
        p.media.push_back(m);
        // The clip starts 2.5 s into the media (skipping "One"), at timeline frame 100, at double speed.
        Clip c;
        c.id = p.newId();
        c.mediaId = m.id;
        c.start = 100;
        c.duration = 50;
        c.sourceIn = 2.5 * 25;
        c.speed = 2.0;
        s.audioTracks.at(0).clips.push_back(c);
        // A linked copy on A2 counts once; a muted track does not count.
        while (s.audioTracks.size() < 3) s.audioTracks.push_back(Track{p.newId(), TrackKind::Audio, "A"});
        Clip copy = c;
        copy.id = p.newId();
        s.audioTracks[1].clips.push_back(copy);
        Clip muted = c;
        muted.id = p.newId();
        muted.start = 400;
        s.audioTracks[2].clips.push_back(muted);
        s.audioTracks[2].muted = true;

        auto caps = captionsFromTranscripts(p, s);
        QCOMPARE(caps.size(), size_t(1));
        QCOMPARE(QString::fromStdString(caps[0].text), QString("two three. Four five six."));
        // "two" is at 2.5 s in the media = 0 s into the clip = frame 100; "six." ends at 4.9 s = 2.4 s
        // of source = 1.2 s of timeline = frame 130.
        QCOMPARE(caps[0].start, FrameTime(100));
        QCOMPARE(caps[0].end, FrameTime(130));
        // Without transcripts there is nothing to caption.
        p.media[0].transcript.reset();
        QVERIFY(captionsFromTranscripts(p, s).empty());
    }

    void renderAndReplaceBookkeeping() {
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        Clip c;
        c.id = p.newId();
        c.mediaId = 41;
        c.start = 100;
        c.duration = 50;
        c.sourceIn = 20;
        c.speed = 2;
        c.effects.push_back(makeEffect(p, "limiter"));
        s.audioTracks[0].clips.push_back(c);
        QVERIFY(edit::replaceWithRender(s, c.id, 77).ok);
        Clip* r = edit::clipById(s, c.id);
        QCOMPARE(r->mediaId, Id(77));
        QCOMPARE(r->sourceIn, 0.0);
        QCOMPARE(r->speed, 1.0);
        QVERIFY(r->effects.empty());
        QVERIFY(!r->unrendered.empty());
        // Saved with the project.
        Project q;
        QVERIFY(projectFromJson(projectToJson(p), q));
        QCOMPARE(q.active()->audioTracks[0].clips[0].unrendered, r->unrendered);
        // Trim 10 frames off the head of the rendered clip, then restore: the
        // original source moves on by 10 frames at its speed of 2.
        r->start += 10;
        r->duration -= 10;
        r->sourceIn = 10;
        QVERIFY(edit::restoreUnrendered(s, c.id).ok);
        r = edit::clipById(s, c.id);
        QCOMPARE(r->mediaId, Id(41));
        QCOMPARE(r->sourceIn, 40.0);
        QCOMPARE(r->speed, 2.0);
        QCOMPARE(r->effects.size(), size_t(1));
        QVERIFY(r->unrendered.empty());
        QVERIFY(!edit::restoreUnrendered(s, c.id).ok);
        // Reversed clips count trims from the other end of the source.
        r->reverse = true;
        QVERIFY(edit::replaceWithRender(s, c.id, 78).ok);
        r = edit::clipById(s, c.id);
        r->duration -= 5;  // trim 5 frames off the tail
        QVERIFY(edit::restoreUnrendered(s, c.id).ok);
        r = edit::clipById(s, c.id);
        QCOMPARE(r->sourceIn, 40.0 + 5 * 2);
        QVERIFY(r->reverse);
    }

    void editingByTranscript() {
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.fps = {25, 1};
        MediaItem m;
        m.id = p.newId();
        m.kind = MediaKind::Video;
        m.hasVideo = m.hasAudio = true;
        m.duration = 20;
        auto t = std::make_shared<Transcript>();
        TranscriptSegment seg;
        // "Hello um world" then a 2.4 s pause, then "again".
        seg.words = {{1.0, 1.4, "Hello", 1}, {1.5, 1.8, "um,", 1}, {1.9, 2.3, "world.", 1}, {4.7, 5.1, "Again", 1}};
        t->segments.push_back(seg);
        m.transcript = t;
        p.media.push_back(m);
        QVERIFY(edit::placeMedia(p, s, m.id, 0, 0, 10 * 25, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        QCOMPARE(s.duration(), FrameTime(250));

        // Linked video and audio copies give each word once.
        auto words = sequenceTranscriptWords(p, s);
        QCOMPARE(words.size(), size_t(4));
        QCOMPARE(QString::fromStdString(words[1].text), QString("um,"));

        QVERIFY(isFillerWord("Um,"));
        QVERIFY(isFillerWord("uh"));
        QVERIFY(!isFillerWord("umbrella"));
        auto fillers = fillerWordRanges(words, 25);
        QCOMPARE(fillers.size(), size_t(1));
        QCOMPARE(fillers[0], FrameRange(38, 48));  // 1.5 s to the next word at 1.9 s

        auto pauses = pauseRanges(words, 25, 1.0, 0.4);
        QCOMPARE(pauses.size(), size_t(1));
        QCOMPARE(pauses[0], FrameRange(63, 112));  // 2.3 + 0.2 s .. 4.7 - 0.2 s, whole frames inside

        QCOMPARE(mergeRanges({{10, 20}, {5, 12}, {30, 30}, {20, 25}}), (std::vector<FrameRange>{{5, 25}}));

        // Captions follow the ripple: inside goes, across shortens, later moves up.
        CaptionTrack ct;
        ct.id = p.newId();
        ct.captions = {{20, 30, "before"}, {40, 45, "inside"}, {35, 60, "x"}, {120, 140, "after"}};
        normalizeCaptions(ct.captions);  // "inside" is cut short by the overlap rules: {20,30} {35,40} {40,45} {120,140}
        s.captionTracks.push_back(ct);
        auto r = rippleDeleteRanges(p, s, {pauses[0], fillers[0]});
        QVERIFY(r.ok);
        QCOMPARE(r.applied, FrameTime(59));
        QCOMPARE(s.duration(), FrameTime(191));
        // Both tracks were cut the same way.
        QCOMPARE(s.videoTracks[0].clips.size(), size_t(3));
        QCOMPARE(s.audioTracks[0].clips.size(), size_t(3));
        const auto& caps = s.captionTracks[0].captions;
        QCOMPARE(caps.front(), (Caption{20, 30, "before"}));
        QCOMPARE(caps.back(), (Caption{61, 81, "after"}));  // 120 - 59
        // The word after the pause now starts 59 frames earlier.
        words = sequenceTranscriptWords(p, s);
        QCOMPARE(words.size(), size_t(3));  // the filler is gone
        QVERIFY(std::fabs(words.back().start - (4.7 - 59 / 25.0)) < 0.05);
        QVERIFY(!rippleDeleteRanges(p, s, {}).ok);
    }

    void transcriptsCaptionsAndSearch() {
        Transcript t;
        t.language = "en";
        t.model = "tiny.en";
        TranscriptSegment a;
        a.start = 0.5;
        a.end = 4.0;
        a.text = "And so, my fellow Americans, ask not";
        const char* words[] = {"And", "so,", "my", "fellow", "Americans,", "ask", "not"};
        for (int i = 0; i < 7; ++i) a.words.push_back({0.5 + i * 0.5, 0.9 + i * 0.5, words[i], 0.9f});
        TranscriptSegment b;
        b.start = 6.0;  // a pause before this one
        b.end = 8.0;
        b.text = "what your country can do for you.";
        const char* words2[] = {"what", "your", "country", "can", "do", "for", "you."};
        for (int i = 0; i < 7; ++i) b.words.push_back({6.0 + i * 0.25, 6.2 + i * 0.25, words2[i], 0.8f});
        t.segments = {a, b};
        QCOMPARE(t.wordCount(), size_t(14));
        QCOMPARE(QString::fromStdString(t.text()), QString("And so, my fellow Americans, ask not what your country can do for you."));

        // JSON round trip.
        Transcript back;
        QVERIFY(transcriptFromJson(transcriptToJson(t), back));
        QCOMPARE(back, t);

        // Cues: at most 20 characters, never across the pause.
        auto cues = transcriptCues(t, 20, 6.0);
        QVERIFY(cues.size() >= 3);
        for (const Cue& c : cues) {
            QVERIFY2(c.text.size() <= 20, c.text.c_str());
            QVERIFY(c.end > c.start);
        }
        QVERIFY(std::none_of(cues.begin(), cues.end(), [](const Cue& c) { return c.start < 4.0 && c.end > 6.0; }));
        const std::string srt = cuesToSrt(cues, 3600);
        QVERIFY2(srt.rfind("1\n01:00:00,500 --> ", 0) == 0, srt.c_str());
        QVERIFY(srt.find("\n2\n") != std::string::npos);
        QVERIFY(cuesToVtt(cues).rfind("WEBVTT\n\n00:00:00.500 --> ", 0) == 0);

        // Phrase search: whole words, ignoring case and punctuation, across segments.
        auto hits = findPhrase(t, "Ask NOT what");
        QCOMPARE(hits.size(), size_t(1));
        QCOMPARE(hits[0].first, 3.0);
        QCOMPARE(hits[0].second, 6.2);
        QCOMPARE(findPhrase(t, "for you").size(), size_t(1));
        QVERIFY(findPhrase(t, "country can't").empty());
        QVERIFY(findPhrase(t, "").empty());

        // Stored on media items and saved with the project.
        Project p = makeDefaultProject();
        MediaItem m;
        m.id = p.newId();
        m.name = "interview";
        m.transcript = std::make_shared<const Transcript>(t);
        p.media.push_back(m);
        Project loaded;
        std::string err;
        QVERIFY2(projectFromJson(projectToJson(p), loaded, &err), err.c_str());
        QVERIFY(loaded.media.at(0).transcript);
        QCOMPARE(*loaded.media.at(0).transcript, t);
    }

    void keyframeInterpolation() {
        Param p(5);
        QCOMPARE(p.at(100), 5.0);
        p.addKey(0, 0);
        p.addKey(10, 10);
        QCOMPARE(p.at(-5), 0.0);
        QCOMPARE(p.at(5), 5.0);
        QCOMPARE(p.at(20), 10.0);
        p.keys[0].interp = Interp::Hold;
        QCOMPARE(p.at(9), 0.0);
        p.keys[0].interp = Interp::Smooth;
        QVERIFY(p.at(2) < 2.0);  // eases in
        QCOMPARE(p.at(5), 5.0);  // symmetric midpoint
        p.set(10, 20);           // replaces existing key
        QCOMPARE(p.keys.size(), size_t(2));
        QCOMPARE(p.at(10), 20.0);
        QVERIFY(p.removeKey(0));
        QVERIFY(p.removeKey(10));
        QCOMPARE(p.at(3), 20.0);  // last value becomes static
    }

    void effectDefaults() {
        Project p;
        Effect cc = makeEffect(p, "color_correct");
        QCOMPARE(cc.p("saturation", 0), 1.0);
        Effect ck = makeEffect(p, "chroma_key");
        QCOMPARE(ck.p("key.g", 0), 1.0);
        Effect title = makeEffect(p, "title");
        QCOMPARE(QString::fromStdString(title.s("text")), QString("Title"));
        QVERIFY(findEffectInfo("gaussian_blur"));
        QVERIFY(!effectsInCategory(EffectCategory::VideoTransition).empty());
    }

    void timecode() {
        Rational f30{30, 1};
        QCOMPARE(QString::fromStdString(formatTimecode(0, f30)), QString("00:00:00:00"));
        QCOMPARE(QString::fromStdString(formatTimecode(30 * 3661 + 7, f30)), QString("01:01:01:07"));
        Rational df{30000, 1001};
        // Drop-frame skips ;00 and ;01 at the start of each minute except every tenth.
        QCOMPARE(QString::fromStdString(formatTimecode(1800, df)), QString("00:01:00;02"));
        QCOMPARE(QString::fromStdString(formatTimecode(17982, df)), QString("00:10:00;00"));
        FrameTime t = 0;
        QVERIFY(parseTimecode("00:01:00;02", df, t));
        QCOMPARE(t, FrameTime(1800));
        QVERIFY(parseTimecode("01:01:01:07", f30, t));
        QCOMPARE(t, FrameTime(30 * 3661 + 7));
        QVERIFY(parseTimecode("2.5s", f30, t));
        QCOMPARE(t, FrameTime(75));
        QVERIFY(parseTimecode("1:00", f30, t));  // SS:FF
        QCOMPARE(t, FrameTime(30));
        QVERIFY(parseTimecode("120", f30, t));
        QCOMPARE(t, FrameTime(120));
        QVERIFY(!parseTimecode("abc", f30, t));
        // Round trip across a range of frames.
        for (FrameTime f : {0, 1, 1799, 1800, 1801, 17981, 17982, 107892})
            QVERIFY(parseTimecode(formatTimecode(f, df), df, t) && t == f);
    }

    void overwriteSplitsAndTrims() {
        Fixture fx;
        Id a = fx.put(V1, 0, 100);
        fx.put(V1, 40, 20, 300);
        auto& clips = fx.v1().clips;
        QCOMPARE(clips.size(), size_t(3));
        QCOMPARE(clips[0].id, a);
        QCOMPARE(clips[0].duration, FrameTime(40));
        QCOMPARE(clips[1].sourceIn, 300.0);
        QCOMPARE(clips[2].start, FrameTime(60));
        QCOMPARE(clips[2].sourceIn, 60.0);  // right remainder keeps source continuity
        QVERIFY(clips[2].id != a);
    }

    void insertRipplesSyncLockedTracks() {
        Fixture fx;
        fx.put(V1, 0, 100);
        Id onV2 = fx.put(V2, 80, 40);
        Clip c = makeClip(fx.p, *fx.p.findMedia(fx.media), TrackKind::Video, fx.s());
        c.start = 50;
        c.duration = 10;
        QVERIFY(insert(fx.p, fx.s(), V1, c).ok);
        auto& v1 = fx.v1().clips;
        QCOMPARE(v1.size(), size_t(3));
        QCOMPARE(v1[0].end(), FrameTime(50));
        QCOMPARE(v1[1].start, FrameTime(50));
        QCOMPARE(v1[2].start, FrameTime(60));
        QCOMPARE(v1[2].sourceIn, 50.0);
        // The V2 clip after the insert point moves with the edit (sync lock).
        const Clip* moved = clipById(fx.s(), onV2);
        QVERIFY(moved);
        QCOMPARE(moved->start, FrameTime(90));
        QCOMPARE(fx.s().duration(), FrameTime(130));
        // Locked tracks don't move.
        Fixture fx2;
        fx2.put(V1, 0, 100);
        Id locked = fx2.put(V2, 80, 40);
        trackAt(fx2.s(), V2)->locked = true;
        QVERIFY(insert(fx2.p, fx2.s(), V1, c).ok);
        QCOMPARE(clipById(fx2.s(), locked)->start, FrameTime(80));
    }

    void placeMediaLinksAudio() {
        Fixture fx;
        auto r = placeMedia(fx.p, fx.s(), fx.media, 10, 30, 90, V1, A1, false);
        QVERIFY(r.ok);
        QCOMPARE(r.created.size(), size_t(2));
        const Clip* v = clipById(fx.s(), r.created[0]);
        const Clip* a = clipById(fx.s(), r.created[1]);
        QCOMPARE(v->duration, FrameTime(60));
        QCOMPARE(v->sourceIn, 30.0);
        QVERIFY(v->linkGroup != 0);
        QCOMPARE(v->linkGroup, a->linkGroup);
        QCOMPARE(linkedClips(fx.s(), v->id).size(), size_t(2));
        // Full-length placement uses the media duration (10 s at 30 fps).
        auto r2 = placeMedia(fx.p, fx.s(), fx.media, 200, 0, -1, V1, A1, false);
        QCOMPARE(clipById(fx.s(), r2.created[0])->duration, FrameTime(300));
    }

    void razorAndRippleDelete() {
        Fixture fx;
        auto r = placeMedia(fx.p, fx.s(), fx.media, 0, 0, 90, V1, A1, false);
        auto r2 = placeMedia(fx.p, fx.s(), fx.media, 90, 0, 60, V1, A1, false);
        QVERIFY(razorAll(fx.p, fx.s(), 30).ok);
        QCOMPARE(fx.v1().clips.size(), size_t(3));
        QCOMPARE(fx.a1().clips.size(), size_t(3));
        // The right halves share a new link group distinct from the left halves.
        const Clip& rv = fx.v1().clips[1];
        const Clip& ra = fx.a1().clips[1];
        QCOMPARE(rv.linkGroup, ra.linkGroup);
        QVERIFY(rv.linkGroup != fx.v1().clips[0].linkGroup);
        // Ripple delete the middle pair: the last clip slides left by 60.
        auto ids = expandLinks(fx.s(), {rv.id});
        QCOMPARE(ids.size(), size_t(2));
        QVERIFY(removeClips(fx.p, fx.s(), ids, true).ok);
        QCOMPARE(fx.v1().clips.size(), size_t(2));
        QCOMPARE(clipById(fx.s(), r2.created[0])->start, FrameTime(30));
        QCOMPARE(clipById(fx.s(), r2.created[1])->start, FrameTime(30));
        (void)r;
    }

    void trimClampsToSourceAndNeighbours() {
        Fixture fx;
        Id a = fx.put(V1, 0, 100, 0);
        Id b = fx.put(V1, 120, 50, 0);
        // Normal trim cannot extend into the next clip.
        auto r = trim(fx.p, fx.s(), a, Edge::Out, 50, TrimMode::Normal);
        QCOMPARE(r.applied, FrameTime(20));
        QCOMPARE(clipById(fx.s(), a)->end(), FrameTime(120));
        // In-point cannot go before source frame 0.
        r = trim(fx.p, fx.s(), b, Edge::In, -500, TrimMode::Normal);
        QCOMPARE(clipById(fx.s(), b)->start, FrameTime(120));
        QCOMPARE(r.applied, FrameTime(0));
        // Shortening the head moves the start and the source in-point.
        r = trim(fx.p, fx.s(), b, Edge::In, 10, TrimMode::Normal);
        QCOMPARE(clipById(fx.s(), b)->start, FrameTime(130));
        QCOMPARE(clipById(fx.s(), b)->sourceIn, 10.0);
        // Ripple trim of the out-point pushes the following clip.
        r = trim(fx.p, fx.s(), a, Edge::Out, -20, TrimMode::Ripple);
        QCOMPARE(clipById(fx.s(), a)->end(), FrameTime(100));
        QCOMPARE(clipById(fx.s(), b)->start, FrameTime(110));
        // Ripple trim of the in-point keeps the clip in place and pulls the rest.
        r = trim(fx.p, fx.s(), a, Edge::In, 10, TrimMode::Ripple);
        QCOMPARE(clipById(fx.s(), a)->start, FrameTime(0));
        QCOMPARE(clipById(fx.s(), a)->duration, FrameTime(90));
        QCOMPARE(clipById(fx.s(), a)->sourceIn, 10.0);
        QCOMPARE(clipById(fx.s(), b)->start, FrameTime(100));
        // Out-point limited by media length (300 frames).
        r = trim(fx.p, fx.s(), b, Edge::Out, 10000, TrimMode::Ripple);
        QCOMPARE(clipById(fx.s(), b)->sourceIn + clipById(fx.s(), b)->sourceExtent(), 300.0);
    }

    void rollSlipSlide() {
        Fixture fx;
        Id a = fx.put(V1, 0, 50, 100);
        Id b = fx.put(V1, 50, 50, 100);
        Id c = fx.put(V1, 100, 50, 100);
        QCOMPARE(roll(fx.p, fx.s(), a, b, 10).applied, FrameTime(10));
        QCOMPARE(clipById(fx.s(), a)->end(), FrameTime(60));
        QCOMPARE(clipById(fx.s(), b)->start, FrameTime(60));
        QCOMPARE(clipById(fx.s(), b)->sourceIn, 110.0);
        // Slip changes only the source.
        QCOMPARE(slip(fx.p, fx.s(), b, -20).applied, FrameTime(-20));
        QCOMPARE(clipById(fx.s(), b)->sourceIn, 90.0);
        QCOMPARE(clipById(fx.s(), b)->start, FrameTime(60));
        // Slip is clamped to the media.
        QCOMPARE(slip(fx.p, fx.s(), b, -1000).applied, FrameTime(-90));
        QCOMPARE(clipById(fx.s(), b)->sourceIn, 0.0);
        // Slide moves the clip and adjusts both neighbours.
        QCOMPARE(slide(fx.p, fx.s(), b, 5).applied, FrameTime(5));
        QCOMPARE(clipById(fx.s(), b)->start, FrameTime(65));
        QCOMPARE(clipById(fx.s(), a)->end(), FrameTime(65));
        QCOMPARE(clipById(fx.s(), c)->start, FrameTime(105));
        QCOMPARE(clipById(fx.s(), c)->sourceIn, 105.0);
        QCOMPARE(clipById(fx.s(), c)->end(), FrameTime(150));
    }

    void speedChanges() {
        Fixture fx;
        Id a = fx.put(V1, 0, 100);
        Id b = fx.put(V1, 100, 50);
        QVERIFY(setSpeed(fx.p, fx.s(), a, 2.0, true).ok);
        QCOMPARE(clipById(fx.s(), a)->duration, FrameTime(50));
        QCOMPARE(clipById(fx.s(), b)->start, FrameTime(50));  // rippled
        QCOMPARE(clipById(fx.s(), a)->sourceFrameAt(10), 20.0);
        QVERIFY(setSpeed(fx.p, fx.s(), a, 0.5, false).ok);  // non-ripple growth is clamped by the next clip
        QCOMPARE(clipById(fx.s(), a)->duration, FrameTime(50));
        QVERIFY(setSpeed(fx.p, fx.s(), a, 1.0, false, true).ok);
        const Clip* ca = clipById(fx.s(), a);
        QCOMPARE(ca->sourceFrameAt(0), ca->sourceIn + double(ca->duration - 1));  // reverse plays from the end
    }

    void moveAndTransitions() {
        Fixture fx;
        Id a = fx.put(V1, 0, 60);
        Id b = fx.put(V1, 60, 60);
        auto r = addTransition(fx.p, fx.s(), a, Edge::Out, "cross_dissolve", 20);
        QVERIFY(r.ok);
        Transition* tr = transitionById(fx.s(), r.created[0]);
        QVERIFY(tr);
        QCOMPARE(tr->clipA, a);
        QCOMPARE(tr->clipB, b);
        FrameTime from, to;
        QVERIFY(transitionRange(fx.v1(), *tr, from, to));
        QCOMPARE(from, FrameTime(50));
        QCOMPARE(to, FrameTime(70));
        // Moving both clips together keeps the transition (even to another track).
        QVERIFY(moveClips(fx.p, fx.s(), {a, b}, 30, 1, 0).ok);
        QCOMPARE(trackAt(fx.s(), V2)->transitions.size(), size_t(1));
        QCOMPARE(fx.v1().transitions.size(), size_t(0));
        // Separating them drops the transition.
        QVERIFY(moveClips(fx.p, fx.s(), {b}, 10, 0, 0).ok);
        QCOMPARE(trackAt(fx.s(), V2)->transitions.size(), size_t(0));
        // Fade-out on a lone clip; razor keeps it on the tail half.
        auto f = addTransition(fx.p, fx.s(), b, Edge::Out, "dip_to_black", 10);
        QVERIFY(f.ok);
        auto split = razor(fx.p, fx.s(), V2, clipById(fx.s(), b)->start + 20);
        QVERIFY(split.ok);
        QCOMPARE(transitionById(fx.s(), f.created[0])->clipA, split.created[0]);
        // Audio tracks get audio crossfades whatever type was asked for.
        Id au = fx.put(A1, 0, 60);
        auto af = addTransition(fx.p, fx.s(), au, Edge::In, "wipe", 10);
        QCOMPARE(QString::fromStdString(transitionById(fx.s(), af.created[0])->type), QString("crossfade"));
    }

    void moveOverwritesAndClamps() {
        Fixture fx;
        Id a = fx.put(V1, 0, 50);
        Id b = fx.put(V1, 100, 50);
        QVERIFY(moveClips(fx.p, fx.s(), {b}, -70, 0, 0).ok);  // lands on top of the tail of a
        QCOMPARE(clipById(fx.s(), b)->start, FrameTime(30));
        QCOMPARE(clipById(fx.s(), a)->duration, FrameTime(30));
        auto r = moveClips(fx.p, fx.s(), {a}, -100, 0, 0);
        QCOMPARE(r.applied, FrameTime(0));  // clamped at 0
        QVERIFY(!moveClips(fx.p, fx.s(), {a}, 0, 7, 0).ok);  // no such track
    }

    void insertModeMove() {
        Fixture fx;
        Id a = fx.put(V1, 0, 50);
        Id b = fx.put(V1, 50, 50);
        Id c = fx.put(V1, 100, 50);
        // Insert-move c to the start: everything else shifts right.
        QVERIFY(moveClips(fx.p, fx.s(), {c}, -100, 0, 0, true).ok);
        QCOMPARE(clipById(fx.s(), c)->start, FrameTime(0));
        QCOMPARE(clipById(fx.s(), a)->start, FrameTime(50));
        QCOMPARE(clipById(fx.s(), b)->start, FrameTime(100));
    }

    void rangeEditsAndGaps() {
        Fixture fx;
        fx.put(V1, 0, 100);
        fx.put(A1, 0, 100);
        QVERIFY(extractRange(fx.p, fx.s(), 20, 40, {V1, A1}).ok);
        QCOMPARE(fx.s().duration(), FrameTime(80));
        QCOMPARE(fx.v1().clips.size(), size_t(2));
        QCOMPARE(fx.v1().clips[1].start, FrameTime(20));
        QCOMPARE(fx.v1().clips[1].sourceIn, 40.0);
        QVERIFY(liftRange(fx.p, fx.s(), 10, 20, {V1}).ok);
        QCOMPARE(fx.v1().clips.size(), size_t(2));
        QCOMPARE(fx.v1().clips[0].duration, FrameTime(10));
        QVERIFY(closeGap(fx.p, fx.s(), V1, 15).ok);
        QCOMPARE(fx.v1().clips[1].start, FrameTime(10));
        QVERIFY(!closeGap(fx.p, fx.s(), V1, 500).ok);
    }

    void copyPasteAndDuplicate() {
        Fixture fx;
        auto r = placeMedia(fx.p, fx.s(), fx.media, 0, 0, 30, V1, A1, false);
        auto items = copyClips(fx.s(), r.created);
        QCOMPARE(items.size(), size_t(2));
        auto p = pasteClips(fx.p, fx.s(), items, 100, false);
        QVERIFY(p.ok);
        QCOMPARE(p.created.size(), size_t(2));
        const Clip* pv = clipById(fx.s(), p.created[0]);
        QCOMPARE(pv->start, FrameTime(100));
        QVERIFY(pv->linkGroup != clipById(fx.s(), r.created[0])->linkGroup);
        QCOMPARE(pv->linkGroup, clipById(fx.s(), p.created[1])->linkGroup);
        // Insert-paste at 0 pushes everything right by 30.
        QVERIFY(pasteClips(fx.p, fx.s(), items, 0, true).ok);
        QCOMPARE(clipById(fx.s(), p.created[0])->start, FrameTime(130));
    }

    void snappingAndNavigation() {
        Fixture fx;
        fx.put(V1, 10, 20);
        fx.put(V1, 50, 10);
        fx.s().playhead = 100;
        auto pts = snapPoints(fx.s(), {});
        bool snapped = false;
        QCOMPARE(snap(pts, 32, 3, &snapped), FrameTime(30));
        QVERIFY(snapped);
        QCOMPARE(snap(pts, 40, 3, &snapped), FrameTime(40));
        QVERIFY(!snapped);
        QCOMPARE(snap(pts, 98, 3), FrameTime(100));  // playhead
        QCOMPARE(nextEdit(fx.s(), 30), FrameTime(50));
        QCOMPARE(prevEdit(fx.s(), 50), FrameTime(30));
    }

    void compoundClip() {
        Fixture fx;
        auto r1 = placeMedia(fx.p, fx.s(), fx.media, 10, 0, 30, V1, A1, false);
        fx.put(V2, 20, 10);
        Id seqId = fx.s().id;
        auto ids = r1.created;
        ids.push_back(trackAt(fx.s(), V2)->clips[0].id);
        auto r = makeCompound(fx.p, fx.s(), ids, "Nest");
        QVERIFY(r.ok);
        QCOMPARE(fx.p.sequences.size(), size_t(2));
        Sequence& outer = *fx.p.findSequence(seqId);
        QCOMPARE(trackAt(outer, V1)->clips.size(), size_t(1));
        QCOMPARE(trackAt(outer, V2)->clips.size(), size_t(0));
        const Clip& nestClip = trackAt(outer, V1)->clips[0];
        QCOMPARE(nestClip.start, FrameTime(10));
        QCOMPARE(nestClip.duration, FrameTime(30));
        const MediaItem* m = fx.p.findMedia(nestClip.mediaId);
        QVERIFY(m && m->kind == MediaKind::Sequence);
        const Sequence* nested = fx.p.findSequence(m->sequenceId);
        QCOMPARE(trackAt(*nested, V2)->clips[0].start, FrameTime(10));
        QCOMPARE(trackAt(outer, A1)->clips.size(), size_t(1));  // nested audio gets its own clip
    }

    void undoRedo() {
        Fixture fx;
        History h;
        Project before = fx.p;
        h.push("Add", fx.p);
        fx.put(V1, 0, 100);
        Project after = fx.p;
        h.push("Razor", fx.p);
        razor(fx.p, fx.s(), V1, 50);
        QCOMPARE(fx.v1().clips.size(), size_t(2));
        QCOMPARE(QString::fromStdString(h.undoLabel()), QString("Razor"));
        QVERIFY(h.undo(fx.p));
        QVERIFY(fx.p == after);
        QVERIFY(h.undo(fx.p));
        QVERIFY(fx.p == before);
        QVERIFY(!h.undo(fx.p));
        QVERIFY(h.redo(fx.p));
        QVERIFY(fx.p == after);
        h.push("Other", fx.p);  // new edit clears redo
        QVERIFY(!h.canRedo());
    }

    void speedRamps() {
        Fixture fx;
        Clip c = makeClip(fx.p, *fx.p.findMedia(fx.media), TrackKind::Video, fx.s());
        c.duration = 60;
        c.sourceIn = 10;
        QVERIFY(!c.ramped());
        QCOMPARE(c.sourceExtent(), 60.0);
        // 100 % to 300 % over the clip: on average twice as fast.
        Param& sp = c.timing.params["speed"];
        sp.addKey(0, 100);
        sp.addKey(60, 300);
        QVERIFY(c.ramped());
        QVERIFY(std::fabs(c.sourceExtent() - 120) < 1e-9);
        QVERIFY(std::fabs(c.sourceOffset(30) - 45) < 1e-9);  // 30 * (1 + 2) / 2
        QVERIFY(std::fabs(c.speedAt(30) - 2) < 1e-4);
        QVERIFY(std::fabs(c.sourceFrameAt(30) - 55) < 1e-9);
        QVERIFY(std::fabs(c.localForSource(55) - 30) < 1e-6);
        // Hold, smooth (eased: the same total, less early on), and beyond the keys.
        sp.keys[0].interp = Interp::Hold;
        QVERIFY(std::fabs(c.sourceOffset(60) - 60) < 1e-9);
        sp.keys[0].interp = Interp::Smooth;
        QVERIFY(std::fabs(c.sourceOffset(60) - 120) < 1e-9);
        auto eased = [](double u) { return u * u * u - u * u * u * u / 2; };  // integral of smoothstep
        QVERIFY(std::fabs(c.sourceOffset(30) - 60 * (0.5 + 2 * eased(0.5))) < 1e-9);  // 41.25: slower early on
        QVERIFY(std::fabs(c.sourceOffset(15) - 60 * (0.25 + 2 * eased(0.25))) < 1e-9);
        QVERIFY(std::fabs(c.sourceOffset(70) - (120 + 30)) < 1e-9);  // 300 % after the last key
        QVERIFY(std::fabs(c.sourceOffset(-5) + 5) < 1e-9);           // 100 % before the first
        sp.keys[0].interp = Interp::Linear;
        // A constant speed multiplies the curve; reversed clips ignore it.
        c.speed = 0.5;
        QVERIFY(std::fabs(c.sourceExtent() - 60) < 1e-9);
        c.speed = 1;
        c.reverse = true;
        QVERIFY(!c.ramped());
        QCOMPARE(c.sourceExtent(), 60.0);
        c.reverse = false;

        // Splitting keeps the picture continuous; trimming the in point follows the curve.
        Id id = overwrite(fx.p, fx.s(), V1, c).created.at(0);
        const double at25 = clipById(fx.s(), id)->sourceFrameAt(25);
        QVERIFY(razor(fx.p, fx.s(), V1, 25).ok);
        const Clip& right = fx.v1().clips.at(1);
        QVERIFY(std::fabs(right.sourceFrameAt(25) - at25) < 1e-9);
        QVERIFY(std::fabs(right.sourceFrameAt(59) - (10 + 120 - c.speedAt(59.5))) < 0.05);
        const double extentBefore = fx.v1().clips[0].sourceExtent() + right.sourceExtent();
        QVERIFY(std::fabs(extentBefore - 120) < 1e-9);
        const double at35 = right.sourceFrameAt(35);
        QVERIFY(trim(fx.p, fx.s(), right.id, Edge::In, 10, TrimMode::Normal).ok);
        const Clip& trimmed = fx.v1().clips.at(1);
        QCOMPARE(trimmed.start, FrameTime(35));
        QVERIFY(std::fabs(trimmed.sourceFrameAt(35) - at35) < 1e-9);  // same picture at the same place
        // Saved with the project.
        Project back;
        QVERIFY(projectFromJson(projectToJson(fx.p), back));
        QVERIFY(back == fx.p);
    }

    void multicamClips() {
        Project p = makeDefaultProject();
        auto addMedia = [&](const char* name, bool video, bool audio, double tc) {
            MediaItem m;
            m.id = p.newId();
            m.kind = video ? MediaKind::Video : MediaKind::Audio;
            m.name = name;
            m.path = std::string("/nonexistent/") + name;
            m.duration = 10.0;
            m.width = 1920;
            m.height = 1080;
            m.fps = {30, 1};
            m.hasVideo = video;
            m.hasAudio = audio;
            m.timecode = tc;
            p.media.push_back(m);
            return m.id;
        };
        const Id camA = addMedia("CamA.mov", true, true, 3600.0);
        const Id camB = addMedia("CamB.mov", true, true, 3601.0);
        const Id lav = addMedia("Lav.wav", false, true, 3599.5);
        std::vector<double> offsets;
        QVERIFY(timecodeOffsets(p, {camA, camB, lav}, offsets));
        std::string err;
        const Id mcMedia = makeMulticam(p, {camA, camB, lav}, offsets, "Interview", &err);
        QVERIFY2(mcMedia, err.c_str());
        const MediaItem& mm = *p.findMedia(mcMedia);
        const Sequence* mc = p.findSequence(mm.sequenceId);
        QVERIFY(mc && mc->multicam);
        QCOMPARE(angleNames(*mc), (std::vector<std::string>{"CamA.mov", "CamB.mov"}));
        QCOMPARE(mc->audioTracks.size(), size_t(3));
        // Synced by timecode: the lav starts first (frame 0), A half a second later, B 1.5 s later.
        QCOMPARE(mc->videoTracks[0].clips[0].start, FrameTime(15));
        QCOMPARE(mc->videoTracks[1].clips[0].start, FrameTime(45));
        QCOMPARE(mc->audioTracks[2].clips[0].start, FrameTime(0));
        QCOMPARE(angleAudioTrack(*mc, 1), 1);
        QCOMPARE(audioTrackAngle(*mc, 2), -1);
        // Without timecode on every item there is no timecode sync.
        p.findMedia(lav)->timecode = -1;
        QVERIFY(!timecodeOffsets(p, {camA, lav}, offsets));

        // On the timeline: a video clip and its linked audio, showing angle 1 and the whole mix.
        Sequence& s = *p.active();
        QVERIFY(edit::placeMedia(p, s, mcMedia, 0, 0, -1, V1, A1, false).ok);
        const Id v = trackAt(s, V1)->clips.at(0).id;
        QCOMPARE(trackAt(s, V1)->clips[0].angle, 0);
        QCOMPARE(trackAt(s, A1)->clips[0].audioAngle, -1);
        QVERIFY(edit::switchAngle(p, s, v, 1, 0, false, false).ok);
        QCOMPARE(edit::clipById(s, v)->angle, 1);
        QVERIFY(!edit::switchAngle(p, s, v, 5, 0, false, false).ok);
        // Cutting to angle 0 at frame 100: picture and sound are cut together.
        Result r = edit::switchAngle(p, s, v, 0, 100, true, true);
        QVERIFY(r.ok);
        QCOMPARE(trackAt(s, V1)->clips.size(), size_t(2));
        QCOMPARE(trackAt(s, A1)->clips.size(), size_t(2));
        QCOMPARE(trackAt(s, V1)->clips[0].angle, 1);
        QCOMPARE(trackAt(s, V1)->clips[1].angle, 0);
        QCOMPARE(trackAt(s, V1)->clips[1].start, FrameTime(100));
        QCOMPARE(trackAt(s, A1)->clips[0].audioAngle, -1);  // audio follows only from the cut on
        QCOMPARE(trackAt(s, A1)->clips[1].audioAngle, 0);
        QCOMPARE(trackAt(s, V1)->clips[1].linkGroup, trackAt(s, A1)->clips[1].linkGroup);
        QVERIFY(trackAt(s, V1)->clips[1].linkGroup != trackAt(s, V1)->clips[0].linkGroup);
        QVERIFY(edit::setAudioAngle(p, s, trackAt(s, A1)->clips[1].id, 2).ok);
        QCOMPARE(trackAt(s, A1)->clips[1].audioAngle, 2);
        QVERIFY(!edit::setAudioAngle(p, s, trackAt(s, A1)->clips[1].id, 3).ok);

        // Automatic changes in multicam frames: the clip is cut wherever the angle changes.
        Sequence& s2 = s;
        removeClips(p, s2, expandLinks(s2, {trackAt(s2, V1)->clips[0].id, trackAt(s2, V1)->clips[1].id}), false);
        QVERIFY(trackAt(s2, V1)->clips.empty() && trackAt(s2, A1)->clips.empty());
        QVERIFY(edit::placeMedia(p, s2, mcMedia, 30, 60, 300, V1, A1, false).ok);  // shows multicam frames 60..299
        const Id mcClip = trackAt(s2, V1)->clips.at(0).id;
        r = edit::applyAngleChanges(p, s2, mcClip, {{0, 1}, {90, 0}, {150, 0}, {200, 1}, {400, 0}}, false);
        QVERIFY2(r.ok, r.error.c_str());
        const auto& vc = trackAt(s2, V1)->clips;
        QCOMPARE(vc.size(), size_t(3));
        QCOMPARE(vc[0].angle, 1);
        QCOMPARE(vc[1].start, FrameTime(60));  // multicam frame 90 = timeline 30 + (90 - 60)
        QCOMPARE(vc[1].angle, 0);
        QCOMPARE(vc[2].start, FrameTime(170));
        QCOMPARE(vc[2].angle, 1);
        QCOMPARE(trackAt(s2, A1)->clips.size(), size_t(3));

        // Flattening puts the angles' own clips in their place.
        r = edit::flattenMulticam(p, s2, {vc[0].id, vc[1].id, vc[2].id});
        QVERIFY2(r.ok, r.error.c_str());
        const auto& flat = trackAt(s2, V1)->clips;
        QCOMPARE(flat.size(), size_t(3));
        QCOMPARE(flat[0].mediaId, camB);
        QCOMPARE(flat[1].mediaId, camA);
        QCOMPARE(flat[2].mediaId, camB);
        QCOMPARE(flat[0].start, FrameTime(30));
        // Camera A starts at multicam frame 15: multicam frame 90 is its frame 75.
        QCOMPARE(flat[1].sourceIn, 75.0);
        QCOMPARE(flat[2].sourceIn, 200.0 - 45.0);
        // The whole-mix audio stays a multicam clip.
        QVERIFY(multicamSequence(p, trackAt(s2, A1)->clips.at(0)));

        // Saved with the project.
        trackAt(s2, A1)->clips[0].audioAngle = 1;
        Project back;
        QVERIFY(projectFromJson(projectToJson(p), back));
        QVERIFY(back == p);
        QVERIFY(back.findSequence(mm.sequenceId)->multicam);
    }

    void projectRoundTrip() {
        Fixture fx;
        auto r = placeMedia(fx.p, fx.s(), fx.media, 0, 15, 75, V1, A1, false);
        Clip* v = clipById(fx.s(), r.created[0]);
        v->effects.push_back(makeEffect(fx.p, "color_correct"));
        v->effects.back().params["saturation"].addKey(0, 0.5, Interp::Smooth);
        v->effects.back().params["saturation"].addKey(20, 1.5);
        v->motion.params["rotation"] = Param(12.5);
        v->blendMode = "screen";
        Clip title = makeGeneratorClip(fx.p, "title", 60);
        title.start = 100;
        title.generator.strings["text"] = "Hello\nWorld — ünïcode";
        overwrite(fx.p, fx.s(), V2, title);
        addTransition(fx.p, fx.s(), r.created[0], Edge::In, "wipe", 10);
        addMarker(fx.s(), Marker{42, 0, "Beat", "drop here", 3});
        fx.s().inPoint = 5;
        fx.s().outPoint = 50;
        trackAt(fx.s(), A1)->volumeDb = -3;
        fx.s().colorSpace = "rec2100hlg";
        fx.s().hdrPeakNits = 1600;
        fx.p.findMedia(fx.media)->colorSpace = "rec2100pq";
        fx.p.findMedia(fx.media)->colorOverride = "slog3-sgamut3cine";
        // An object mask: clicks on two frames and two segmented frames.
        {
            auto obj = std::make_shared<ObjectMask>();
            obj->fps = 29.97;
            obj->prompts[12] = {{0.25, 0.5, 1}, {0.75, 0.125, 0}};
            obj->prompts[40] = {{0.1, 0.2, 2}, {0.6, 0.7, 3}};
            std::vector<float> logits(size_t(kObjectGrid) * kObjectGrid);
            for (int y = 0; y < kObjectGrid; ++y)
                for (int x = 0; x < kObjectGrid; ++x)
                    logits[size_t(y) * kObjectGrid + size_t(x)] = float(40 - std::hypot(x - 100.0, y - 120.0)) * 0.37f;
            obj->frames[12] = packObjectLogits(logits.data());
            obj->frames[13] = packObjectLogits(logits.data());
            v->effects.back().params["mask.shape"] = Param(3.0);
            v->effects.back().object = obj;
            // Packed to 1/8 of a logit, clamped where the sigmoid is flat; a few kB a frame.
            std::vector<float> back;
            QVERIFY(obj->logits(12, back));
            for (size_t i = 0; i < logits.size(); ++i)
                QVERIFY(std::fabs(back[i] - std::clamp(logits[i], -15.875f, 15.875f)) <= 1.0f / 16 + 1e-6f);
            QVERIFY2(obj->frames[12].size() < 8000, qPrintable(QString::number(obj->frames[12].size())));
            QVERIFY(std::fabs(objectCoverage(back) - objectCoverage(logits)) < 0.001);
            QVERIFY(obj->logitsAt(12.5 / 29.97, back) && !obj->logitsAt(14.2 / 29.97, back));
            QCOMPARE(obj->frameAt(13 / 29.97), int64_t(13));  // the frame on screen, not the nearest
            QCOMPARE(obj->frameAt(13.9 / 29.97), int64_t(13));
        }
        std::string json = projectToJson(fx.p);
        Project back;
        std::string err;
        QVERIFY2(projectFromJson(json, back, &err), err.c_str());
        QVERIFY(back == fx.p);
        QVERIFY(clipById(*back.active(), r.created[0])->effects.back().object != v->effects.back().object);  // equal by value
        // Garbage is rejected with a message.
        QVERIFY(!projectFromJson("{nope", back, &err));
        QVERIFY(!err.empty());
        QVERIFY(!projectFromJson("{\"format\":\"other\"}", back, &err));
    }

    void keyframeLines() {
        // The volume scale: silence at the bottom, +6 dB at the top, 0 dB at 71 %.
        QCOMPARE(gainToLevel(kGainLineMaxDb), 1.0);
        QCOMPARE(gainToLevel(kGainLineMinDb), 0.0);
        QCOMPARE(gainToLevel(-200), 0.0);
        QVERIFY(std::fabs(gainToLevel(0) - std::sqrt(0.5)) < 0.01);
        QVERIFY(std::fabs(gainToLevel(-6.02) - 0.5) < 0.01);
        for (double db : {-40.0, -12.0, -3.0, 0.0, 4.5}) QVERIFY(std::fabs(levelToGain(gainToLevel(db)) - db) < 1e-9);
        QCOMPARE(levelToGain(0), kGainLineMinDb);
        QCOMPARE(levelToGain(2), kGainLineMaxDb);

        // Dragging the line: the static value, or the keys around the point.
        Param flat(-3);
        offsetLine(flat, 10, -2, -60, 6);
        QCOMPARE(flat.value, -5.0);
        offsetLine(flat, 10, 100, -60, 6);
        QCOMPARE(flat.value, 6.0);
        Param keyed;
        keyed.addKey(10, 0);
        keyed.addKey(20, -6);
        keyed.addKey(30, -12);
        offsetLine(keyed, 15, -1, -60, 6);  // between the first two
        QCOMPARE(keyed.keys[0].v, -1.0);
        QCOMPARE(keyed.keys[1].v, -7.0);
        QCOMPARE(keyed.keys[2].v, -12.0);
        offsetLine(keyed, 2, 3, -60, 6);  // before the first: the first
        QCOMPARE(keyed.keys[0].v, 2.0);
        offsetLine(keyed, 99, -100, -60, 6);  // after the last: the last, clamped
        QCOMPARE(keyed.keys[2].v, -60.0);
        offsetLine(keyed, 20, 1, -60, 6);  // on a key: it and the next
        QCOMPARE(keyed.keys[1].v, -6.0);
        QCOMPARE(keyed.keys[2].v, -59.0);

        // Moving a key stays between its neighbours and inside the clip.
        QCOMPARE(moveKey(keyed, 20, 25, -4, 99), FrameTime(25));
        QCOMPARE(keyed.keys[1].t, FrameTime(25));
        QCOMPARE(keyed.keys[1].v, -4.0);
        QCOMPARE(moveKey(keyed, 25, 50, -4, 99), FrameTime(29));
        QCOMPARE(moveKey(keyed, 10, -5, 0, 99), FrameTime(0));
        QCOMPARE(moveKey(keyed, 30, 500, 0, 99), FrameTime(99));
        QCOMPARE(moveKey(keyed, 31, 40, 0, 99), FrameTime(-1));
        QCOMPARE(keyed.keys.size(), size_t(3));
    }

    void autoTagging() {
        // Labels as orthogonal directions, and samples that mix one label from some categories.
        const auto& cats = tagCategories();
        QCOMPARE(int(cats.size()), 4);
        LabelEmbeddings labels;
        int dim = 0;
        for (const auto& c : cats) dim += int(c.labels.size());
        int axis = 0;
        for (const auto& c : cats) {
            labels.emplace_back();
            for (size_t l = 0; l < c.labels.size(); ++l) {
                std::vector<float> e(size_t(dim), 0.f);
                e[size_t(axis++)] = 1.f;
                labels.back().push_back(e);
            }
        }
        auto mix = [&](std::vector<std::pair<size_t, size_t>> parts) {
            std::vector<float> e(size_t(dim), 0.f);
            for (auto [c, l] : parts)
                for (size_t i = 0; i < e.size(); ++i) e[i] += labels[c][l][i];
            double len = 0;
            for (float x : e) len += double(x) * x;
            for (float& x : e) x = float(x / std::sqrt(len));
            return e;
        };
        // Ten samples 2 s apart: close-ups then wide shots, all interior, daylight in the first three.
        VisualIndex v;
        v.step = 2;
        for (int k = 0; k < 10; ++k) {
            std::vector<std::pair<size_t, size_t>> parts{{0, k < 6 ? 0u : 2u}, {1, 0}};
            if (k < 3) parts.push_back({2, 0});
            v.add(k * 2.0, mix(parts));
        }
        AutoTags t = autoTags(v, labels);
        QCOMPARE(t.keywords, (std::vector<std::string>{"Close-up", "Wide shot", "Interior"}));  // daylight is too little of it
        auto run = [&](const char* k) {
            for (const TagRun& r : t.runs)
                if (r.keyword == k) return std::pair{r.start, r.end};
            return std::pair{-1.0, -1.0};
        };
        QCOMPARE(run("Close-up"), (std::pair{0.0, 11.0}));
        QCOMPARE(run("Wide shot"), (std::pair{11.0, 19.0}));
        QCOMPARE(run("Interior"), (std::pair{0.0, 19.0}));
        QCOMPARE(run("Day"), (std::pair{0.0, 5.0}));  // a run, though not a keyword
        QVERIFY(std::is_sorted(t.runs.begin(), t.runs.end(), [](const TagRun& a, const TagRun& b) { return a.start < b.start; }));
        // Part of the footage, as for a subclip.
        QCOMPARE(autoTags(v, labels, 12, 18).keywords, (std::vector<std::string>{"Wide shot", "Interior"}));
        Fixture fx;
        MediaItem& m = *fx.p.findMedia(fx.media);
        m.visual = std::make_shared<const VisualIndex>(v);
        auto sub = makeSubclip(fx.p, fx.media, 0, 4.5);
        QCOMPARE(autoTagMedia(fx.p, *sub, labels).keywords, (std::vector<std::string>{"Close-up", "Interior", "Day"}));
        QCOMPARE(autoTagMedia(fx.p, m, labels).keywords, t.keywords);
        m.visual.reset();
        QVERIFY(autoTagMedia(fx.p, m, labels).keywords.empty());
        // An undecided sample takes no label; "no people" never becomes a keyword.
        VisualIndex unsure;
        unsure.add(0, mix({{0, 0}, {0, 1}}));
        unsure.add(1, mix({{3, 1}}));
        QVERIFY(autoTags(unsure, labels).keywords.empty());
    }

    void subclips() {
        Fixture fx;
        Project& p = fx.p;
        MediaItem& m = *p.findMedia(fx.media);
        m.bin = "Interviews";
        m.keywords = {"interview"};
        Transcript t;
        t.segments.push_back({1, 6, "hello world", {{1, 2, "hello", 1, {}}, {5, 6, "world", 1, {}}}, -1});
        m.transcript = std::make_shared<const Transcript>(t);
        m.timecode = 3600;

        // A range of the media, in its bin, with its keywords; nothing for an empty range or a still.
        auto sub = makeSubclip(p, fx.media, 4, 7);
        QVERIFY(sub);
        QCOMPARE(sub->subclipOf, fx.media);
        QCOMPARE(sub->subclipIn, 4.0);
        QCOMPARE(sub->subclipOut, 7.0);
        QCOMPARE(sub->duration, 3.0);
        QCOMPARE(sub->name, std::string("clip.mov Subclip 1"));
        QCOMPARE(sub->bin, std::string("Interviews"));
        QCOMPARE(sub->keywords, std::vector<std::string>{"interview"});
        QCOMPARE(sub->timecode, 3604.0);
        QVERIFY(!sub->transcript && sub->id == 0);
        sub->id = p.newId();
        p.media.push_back(*sub);
        const Id subId = sub->id;
        QVERIFY(!makeSubclip(p, fx.media, 5, 5));
        QCOMPARE(makeSubclip(p, fx.media, 8, 50)->subclipOut, 10.0);  // kept inside the media
        QCOMPARE(makeSubclip(p, fx.media, 8, 9)->name, std::string("clip.mov Subclip 2"));
        QCOMPARE(makeSubclip(p, fx.media, 8, 9, "Best line")->name, std::string("Best line"));
        // A subclip of a subclip is a range of the same media, inside the first.
        auto inner = makeSubclip(p, subId, 1, 9);
        QVERIFY(inner);
        QCOMPARE(inner->subclipOf, fx.media);
        QCOMPARE(inner->subclipIn, 5.0);
        QCOMPARE(inner->subclipOut, 7.0);
        MediaItem still;
        still.id = p.newId();
        still.kind = MediaKind::Image;
        still.name = "still.png";
        p.media.push_back(still);
        QVERIFY(!makeSubclip(p, still.id, 0, 1));

        // What is said in it: only the words in its range.
        const MediaItem& s = *p.findMedia(subId);
        QCOMPARE(spokenText(&p, s), std::string("world"));
        QCOMPARE(spokenText(nullptr, s), std::string());
        QCOMPARE(spokenText(&p, *p.findMedia(fx.media)), std::string("hello world"));
        QVERIFY(mediaMatchesSearch(s, "world", &p));
        QVERIFY(!mediaMatchesSearch(s, "hello", &p));
        using Ids = std::vector<Id>;
        auto matches = [&](std::vector<SmartRule> rules) {
            std::vector<Id> ids = smartBinMedia(p, SmartBin{1, "t", true, std::move(rules)});
            std::sort(ids.begin(), ids.end());
            return ids;
        };
        QCOMPARE(matches({{"kind", "is", "subclip"}}), Ids{subId});
        QCOMPARE(matches({{"kind", "is", "video"}}), (Ids{fx.media, subId}));
        QCOMPARE(matches({{"transcript", "contains", "world"}}), (Ids{fx.media, subId}));
        QCOMPARE(matches({{"any", "contains", "hello"}}), Ids{fx.media});

        // Usage: clips of the media that play part of the range.
        fx.put(V1, 0, 90, 0);  // media 0-3 s
        std::map<Id, int> usage = mediaUsage(p);
        QCOMPARE(usage[fx.media], 1);
        QCOMPARE(usage.count(subId), size_t(0));
        fx.put(V1, 200, 30, 150);  // media 5-6 s
        usage = mediaUsage(p);
        QCOMPARE(usage[fx.media], 2);
        QCOMPARE(usage[subId], 1);

        // Saved with the project.
        Project back;
        std::string err;
        QVERIFY2(projectFromJson(projectToJson(p), back, &err), err.c_str());
        QCOMPARE(back.findMedia(subId)->subclipOf, fx.media);
        QCOMPARE(back.findMedia(subId)->subclipIn, 4.0);
        QCOMPARE(back.findMedia(subId)->subclipOut, 7.0);
    }

    void mediaLogging() {
        Fixture fx;
        Project& p = fx.p;
        auto addMedia = [&](const char* name, MediaKind kind, double duration) {
            MediaItem m;
            m.id = p.newId();
            m.kind = kind;
            m.name = name;
            m.path = std::string("/nonexistent/") + name;
            m.duration = duration;
            m.hasVideo = kind != MediaKind::Audio;
            m.hasAudio = kind != MediaKind::Image;
            m.width = 3840;
            m.height = 2160;
            m.fps = {25, 1};
            p.media.push_back(m);
            return m.id;
        };
        const Id clip = fx.media, wide = addMedia("wide.mov", MediaKind::Video, 4), song = addMedia("song.wav", MediaKind::Audio, 200),
                 still = addMedia("still.png", MediaKind::Image, 0);

        // Bins: paths with '/', parents implied, names unique per parent.
        QVERIFY(addBin(p, "Interviews/Day 1"));
        QVERIFY(!addBin(p, "Interviews"));  // implied by its child
        QVERIFY(addBin(p, "B-roll"));
        QCOMPARE(projectBins(p), (std::vector<std::string>{"B-roll", "Interviews", "Interviews/Day 1"}));
        QCOMPARE(uniqueBinName(p, "", "b-roll"), std::string("b-roll 2"));
        QCOMPARE(uniqueBinName(p, "Interviews", "Day 1"), std::string("Day 1 2"));
        QCOMPARE(uniqueBinName(p, "Interviews", "Day 2"), std::string("Day 2"));
        QVERIFY(binWithin("Interviews/Day 1", "Interviews") && binWithin("Interviews", "Interviews") && binWithin("x", ""));
        QVERIFY(!binWithin("Interviews 2", "Interviews") && !binWithin("Interviews", "Interviews/Day 1"));
        QCOMPARE(binParent("a/b/c"), std::string("a/b"));
        QCOMPARE(binLeaf("a/b/c"), std::string("c"));
        QVERIFY(moveMediaToBin(p, {clip}, "Interviews/Day 1"));
        QVERIFY(moveMediaToBin(p, {wide}, "Interviews"));
        QVERIFY(!moveMediaToBin(p, {wide}, "Interviews"));
        // Renaming moves what is inside; not onto another bin or into itself.
        QVERIFY(renameBin(p, "Interviews", "Talks"));
        QCOMPARE(p.findMedia(clip)->bin, std::string("Talks/Day 1"));
        QCOMPARE(p.findMedia(wide)->bin, std::string("Talks"));
        QVERIFY(!renameBin(p, "Talks", "B-roll"));
        QVERIFY(!renameBin(p, "Talks", "Talks/Inner"));
        QVERIFY(!renameBin(p, "Nope", "Other"));
        // Moving a bin keeps its name; deleting one moves its contents up.
        QVERIFY(moveBin(p, "Talks/Day 1", "B-roll"));
        QCOMPARE(p.findMedia(clip)->bin, std::string("B-roll/Day 1"));
        QVERIFY(!moveBin(p, "B-roll", "B-roll/Day 1"));
        QVERIFY(removeBin(p, "B-roll"));
        QCOMPARE(p.findMedia(clip)->bin, std::string("Day 1"));
        QCOMPARE(projectBins(p), (std::vector<std::string>{"Day 1", "Talks"}));
        QVERIFY(removeBin(p, "Day 1"));
        QCOMPARE(p.findMedia(clip)->bin, std::string());

        // Keywords: split on commas and semicolons, trimmed, no case-insensitive duplicates.
        QCOMPARE(parseKeywords(" Beach, sunset;beach ,, golden  hour "), (std::vector<std::string>{"Beach", "sunset", "golden hour"}));
        std::vector<std::string> kw{"Beach"};
        QVERIFY(addKeywords(kw, {"BEACH", "Dog"}));
        QCOMPARE(kw, (std::vector<std::string>{"Beach", "Dog"}));
        QVERIFY(!addKeywords(kw, {"dog"}));
        QVERIFY(removeKeywords(kw, {"beach"}));
        QCOMPARE(kw, std::vector<std::string>{"Dog"});
        QCOMPARE(joinKeywords({"a", "b c"}), std::string("a, b c"));

        // Fields, as the list view edits them.
        MediaItem& m = *p.findMedia(clip);
        QVERIFY(setMediaField(m, "rating", "****") && m.rating == 4);
        QVERIFY(setMediaField(m, "rating", "x") && m.rating == -1);
        QVERIFY(setMediaField(m, "rating", "5") && m.rating == 5);
        QVERIFY(!setMediaField(m, "rating", "7") && m.rating == 5);
        QVERIFY(setMediaField(m, "label", "rose") && m.label == labelFromName("Rose") && m.label > 0);
        QVERIFY(!setMediaField(m, "label", "chartreuse"));
        QVERIFY(setMediaField(m, "keywords", "interview, Anna") && m.keywords.size() == 2);
        QVERIFY(setMediaField(m, "scene", " 12A ") && m.metadata.at("scene") == "12A");
        QVERIFY(setMediaField(m, "take", "3"));
        QVERIFY(setMediaField(m, "comment", "Laughs at the end"));
        QVERIFY(setMediaField(m, "scene", "") && !m.metadata.count("scene"));
        QVERIFY(setMediaField(m, "scene", "12A"));
        QVERIFY(!setMediaField(m, "name", "  "));
        QVERIFY(!setMediaField(m, "duration", "3"));  // not editable
        QCOMPARE(mediaFieldText(m, "rating"), std::string("★★★★★"));
        QCOMPARE(mediaFieldText(m, "label"), std::string("Rose"));
        QCOMPARE(mediaFieldText(m, "duration"), std::string("00:00:10.00"));
        QCOMPARE(mediaFieldText(*p.findMedia(song), "duration"), std::string("00:03:20.00"));
        QCOMPARE(mediaFieldText(m, "resolution"), std::string("1920×1080"));
        QCOMPARE(mediaFieldText(*p.findMedia(wide), "fps"), std::string("25"));
        QCOMPARE(mediaFieldText(*p.findMedia(still), "fps"), std::string());
        QCOMPARE(mediaFieldText(m, "keywords"), std::string("interview, Anna"));
        p.findMedia(song)->rating = -1;
        QCOMPARE(mediaFieldText(*p.findMedia(song), "rating"), std::string("Rejected"));
        Transcript t;
        t.segments.push_back({0, 2, "Hello world", {{0, 1, "Hello", 0.9f, {}}, {1, 2, "world", 0.9f, {}}}, -1});
        p.findMedia(wide)->transcript = std::make_shared<const Transcript>(t);
        QCOMPARE(mediaFieldText(*p.findMedia(wide), "transcript"), std::string("2 words"));

        // Usage counts clips in every sequence.
        placeMedia(p, fx.s(), clip, 0, 0, 30, V1, A1, false);
        fx.put(V1, 100, 10);
        std::map<Id, int> usage = mediaUsage(p);
        QCOMPARE(usage[clip], 3);  // linked video and audio, and a second video clip
        QCOMPARE(usage.count(wide), size_t(0));
        QCOMPARE(mediaFieldText(m, "usage", &usage), std::string("3"));

        // Search: every word, or a "quoted phrase", in names, keywords, metadata or speech.
        QVERIFY(mediaMatchesSearch(m, "anna laughs"));
        QVERIFY(mediaMatchesSearch(m, "\"at the end\""));
        QVERIFY(!mediaMatchesSearch(m, "\"the at end\""));
        QVERIFY(mediaMatchesSearch(m, "12a"));
        QVERIFY(!mediaMatchesSearch(m, "anna beach"));
        QVERIFY(mediaMatchesSearch(*p.findMedia(wide), "hello"));
        QVERIFY(mediaMatchesSearch(m, "  "));

        // Smart bin rules.
        auto matches = [&](std::vector<SmartRule> rules, bool all = true) {
            SmartBin b{1, "test", all, std::move(rules)};
            std::vector<Id> ids = smartBinMedia(p, b);
            std::sort(ids.begin(), ids.end());
            return ids;
        };
        using Ids = std::vector<Id>;
        QCOMPARE(matches({{"rating", ">=", "3"}}), Ids{clip});
        QCOMPARE(matches({{"rating", "is", "-1"}}), Ids{song});
        QCOMPARE(matches({{"rating", "<=", "0"}}), (Ids{wide, song, still}));
        QCOMPARE(matches({{"label", "is", "Rose"}}), Ids{clip});
        QCOMPARE(matches({{"label", "!is", "rose"}}), (Ids{wide, song, still}));
        QCOMPARE(matches({{"kind", "is", "audio"}}), Ids{song});
        QCOMPARE(matches({{"keywords", "includes", "ANNA"}}), Ids{clip});
        QCOMPARE(matches({{"keywords", "includes", "ann"}}), Ids{});  // whole keywords
        QCOMPARE(matches({{"keywords", "empty", ""}}), (Ids{wide, song, still}));
        QCOMPARE(matches({{"duration", "<", "5"}}), (Ids{wide, still}));
        QCOMPARE(matches({{"duration", ">=", "3:20"}}), Ids{song});
        QCOMPARE(matches({{"duration", ">", "nonsense"}}), Ids{});
        QCOMPARE(matches({{"usage", "is", "0"}}), (Ids{wide, song, still}));
        QCOMPARE(matches({{"height", ">=", "2160"}}), (Ids{wide, still}));
        QCOMPARE(matches({{"fps", "is", "25"}}), Ids{wide});
        QCOMPARE(matches({{"scene", "starts", "12"}}), Ids{clip});
        QCOMPARE(matches({{"comment", "!empty", ""}}), Ids{clip});
        QCOMPARE(matches({{"name", "contains", ".WAV"}}), Ids{song});
        QCOMPARE(matches({{"transcript", "contains", "hello world"}}), Ids{wide});
        QCOMPARE(matches({{"any", "contains", "world"}}), Ids{wide});
        QCOMPARE(matches({{"any", "contains", "laughs"}}), Ids{clip});
        QCOMPARE(matches({{"bogus", "is", "x"}}), Ids{});
        QCOMPARE(matches({{"kind", "is", "video"}, {"duration", ">", "5"}}), Ids{clip});
        QCOMPARE(matches({{"kind", "is", "audio"}, {"kind", "is", "image"}}, false), (Ids{song, still}));
        QCOMPARE(matches({}), (Ids{clip, wide, song, still}));  // no rules: everything

        // Saved with the project.
        p.bins = {"Talks", "Empty Bin"};
        p.findMedia(still)->created = "2024-05-06T07:08:09Z";
        p.findMedia(still)->metadata["device"] = "Canon EOS R5";
        SmartBin best{p.newId(), "Best takes", false, {{"rating", ">=", "4"}, {"keywords", "includes", "hero"}}};
        p.smartBins.push_back(best);
        const std::string json = projectToJson(p);
        Project back;
        std::string err;
        QVERIFY2(projectFromJson(json, back, &err), err.c_str());
        QCOMPARE(*back.findMedia(wide)->transcript, t);
        back.findMedia(wide)->transcript = p.findMedia(wide)->transcript;  // compared by pointer below
        QVERIFY(back == p);
        QCOMPARE(back.smartBins.at(0), best);
        QVERIFY(back.nextId > best.id);
        QVERIFY(findSmartBin(back, best.id) && !findSmartBin(back, best.id + 1000));
    }

    // Regression tests for review findings.
    void rippleTrimLinkedPartnersEndingElsewhere() {
        Fixture fx;
        const TrackRef A2{TrackKind::Audio, 1}, A3{TrackKind::Audio, 2};
        Id v = fx.put(V1, 0, 150);
        Id a = fx.put(A1, 0, 100);
        linkClips(fx.p, fx.s(), {v, a});
        Id a2 = fx.put(A2, 100, 50);
        Id a3 = fx.put(A3, 150, 50);
        QVERIFY(trim(fx.p, fx.s(), v, Edge::In, 10, TrimMode::Ripple).ok);
        // A3 follows V1 (shifted from its old end, 150); A2 is not pushed into it.
        QCOMPARE(clipById(fx.s(), a3)->start, FrameTime(140));
        QCOMPARE(clipById(fx.s(), a2)->start, FrameTime(100));
        for (TrackRef r : allTracks(fx.s())) {
            const auto& cl = trackAt(fx.s(), r)->clips;
            for (size_t i = 1; i < cl.size(); ++i) QVERIFY(cl[i - 1].end() <= cl[i].start);
        }
    }

    void rippleDeleteKeepsLinkedSync() {
        Fixture fx;
        // A linked pair with audio longer than video, then a J-cut pair (audio starts 5 later).
        Id v = fx.put(V1, 0, 10);
        Id a = fx.put(A1, 0, 15);
        linkClips(fx.p, fx.s(), {v, a});
        Id v2 = fx.put(V1, 10, 30);
        Id a2 = fx.put(A1, 15, 30);
        linkClips(fx.p, fx.s(), {v2, a2});
        QVERIFY(removeClips(fx.p, fx.s(), {v, a}, true).ok);
        // Both tracks move by the same amount, preserving the 5-frame offset.
        QCOMPARE(clipById(fx.s(), a2)->start - clipById(fx.s(), v2)->start, FrameTime(5));
        QCOMPARE(clipById(fx.s(), v2)->start, FrameTime(0));
    }

    void speedChangeOnLinkedPairRipplesOnce() {
        Fixture fx;
        auto r = placeMedia(fx.p, fx.s(), fx.media, 0, 0, 100, V1, A1, false);
        auto next = placeMedia(fx.p, fx.s(), fx.media, 100, 0, 50, V1, A1, false);
        QVERIFY(setSpeed(fx.p, fx.s(), r.created[0], 0.5, true).ok);
        QCOMPARE(clipById(fx.s(), r.created[0])->duration, FrameTime(200));
        QCOMPARE(clipById(fx.s(), r.created[1])->duration, FrameTime(200));  // linked audio too
        QCOMPARE(clipById(fx.s(), next.created[0])->start, FrameTime(200));  // pushed by 100, not 200
        QCOMPARE(clipById(fx.s(), next.created[1])->start, FrameTime(200));
    }

    void oneSidedFadeClampedToClip() {
        Fixture fx;
        Id red = fx.put(V1, 0, 30);
        fx.put(V1, 30, 10);
        // Fade-out of the short blue clip can't exceed its own 10 frames.
        Id blue = fx.v1().clips[1].id;
        auto r = addTransition(fx.p, fx.s(), blue, Edge::Out, "dip_to_black", 30);
        QCOMPARE(transitionById(fx.s(), r.created[0])->duration, FrameTime(10));
        // A centred transition between red and blue may use half of each: up to 20.
        auto c = addTransition(fx.p, fx.s(), red, Edge::Out, "cross_dissolve", 30);
        QCOMPARE(transitionById(fx.s(), c.created[0])->duration, FrameTime(20));
    }

    void interchangeExports() {
        Fixture fx;
        Id a = fx.put(V1, 0, 60, 30);
        Id b = fx.put(V1, 60, 60, 120);
        fx.put(V1, 150, 30, 0);  // after a gap
        addTransition(fx.p, fx.s(), a, Edge::Out, "cross_dissolve", 20);
        setSpeed(fx.p, fx.s(), clipById(fx.s(), b)->id, 1.0, false);
        addMarker(fx.s(), Marker{10, 0, "Start", "", 0});
        std::string edl = exportEdl(fx.p, fx.s());
        QVERIFY(edl.find("TITLE: Sequence 1") != std::string::npos);
        QVERIFY(edl.find("FCM: NON-DROP FRAME") != std::string::npos);
        // First event: clip a, source 30..80 (trimmed to the dissolve start at 50), record 0..50.
        QVERIFY2(edl.find("001  AX       V     C        00:00:01:00 00:00:02:20 00:00:00:00 00:00:01:20") != std::string::npos, edl.c_str());
        QVERIFY(edl.find("D    020") != std::string::npos);
        QVERIFY(edl.find("* FROM CLIP NAME: clip.mov") != std::string::npos);
        std::string otio = exportOtio(fx.p, fx.s());
        QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(otio));
        QVERIFY(doc.isObject());
        QJsonObject root = doc.object();
        QCOMPARE(root["OTIO_SCHEMA"].toString(), QString("Timeline.1"));
        QJsonArray tracks = root["tracks"].toObject()["children"].toArray();
        QCOMPARE(tracks.size(), int(fx.s().videoTracks.size() + fx.s().audioTracks.size()));
        QJsonArray v1 = tracks[0].toObject()["children"].toArray();
        // clip, transition, clip, gap, clip
        QStringList schemas;
        for (const auto& c : v1) schemas << c.toObject()["OTIO_SCHEMA"].toString();
        QCOMPARE(schemas, QStringList({"Clip.2", "Transition.1", "Clip.2", "Gap.1", "Clip.2"}));
        QCOMPARE(v1[1].toObject()["in_offset"].toObject()["value"].toDouble(), 10.0);
        QCOMPARE(v1[3].toObject()["source_range"].toObject()["duration"].toObject()["value"].toDouble(), 30.0);
        QCOMPARE(v1[0].toObject()["source_range"].toObject()["start_time"].toObject()["value"].toDouble(), 30.0);
        QVERIFY(v1[0].toObject()["media_references"].toObject()["DEFAULT_MEDIA"].toObject()["target_url"].toString().startsWith("file://"));
        QCOMPARE(root["tracks"].toObject()["markers"].toArray().size(), 1);
    }

    // A timeline with linked picture and sound, a dissolve, a gap, a title on
    // V2, a speed change and a marker: what interchange must carry.
    static Id interchangeFixture(Fixture& fx) {
        placeMedia(fx.p, fx.s(), fx.media, 0, 30, 90, V1, A1, false);          // record 0..60
        placeMedia(fx.p, fx.s(), fx.media, 60, 120, 180, V1, A1, false);       // record 60..120
        placeMedia(fx.p, fx.s(), fx.media, 150, 0, 30, V1, A1, false);         // after a gap
        const Id a = fx.v1().clips[0].id;
        addTransition(fx.p, fx.s(), a, Edge::Out, "cross_dissolve", 20);
        addTransition(fx.p, fx.s(), fx.a1().clips[0].id, Edge::Out, "crossfade", 20);
        setSpeed(fx.p, fx.s(), fx.v1().clips[2].id, 0.5, false);
        Clip title = makeGeneratorClip(fx.p, "title", 40);
        title.start = 20;
        title.generator.strings["text"] = "Hello";
        overwrite(fx.p, fx.s(), V2, title);
        addMarker(fx.s(), Marker{10, 0, "Start", "first beat", 0});
        return a;
    }

    static void compareTimelines(const Sequence& a, const Sequence& b, bool generators, size_t videoTracks) {
        for (size_t ti = 0; ti < videoTracks + a.audioTracks.size(); ++ti) {
            const bool video = ti < videoTracks;
            const Track& ta = video ? a.videoTracks[ti] : a.audioTracks[ti - videoTracks];
            const auto& list = video ? b.videoTracks : b.audioTracks;
            const size_t bi = video ? ti : ti - videoTracks;
            if (ta.clips.empty() && bi >= list.size()) continue;  // empty tracks need not come back
            QVERIFY(bi < list.size());
            const Track& tb = list[bi];
            std::vector<const Clip*> ca, cb;
            for (const Clip& c : ta.clips)
                if (generators || !c.isGenerator()) ca.push_back(&c);
            for (const Clip& c : tb.clips)
                if (generators || !c.isGenerator()) cb.push_back(&c);
            QCOMPARE(cb.size(), ca.size());
            for (size_t i = 0; i < ca.size(); ++i) {
                QCOMPARE(cb[i]->start, ca[i]->start);
                QCOMPARE(cb[i]->duration, ca[i]->duration);
                QCOMPARE(cb[i]->isGenerator(), ca[i]->isGenerator());
                if (!ca[i]->isGenerator()) {
                    QVERIFY2(std::fabs(cb[i]->sourceIn - ca[i]->sourceIn) < 0.51,
                             qPrintable(QString("%1 vs %2").arg(cb[i]->sourceIn).arg(ca[i]->sourceIn)));
                    QCOMPARE(cb[i]->speed, ca[i]->speed);
                    QCOMPARE(cb[i]->mediaId, ca[i]->mediaId);
                }
            }
            QCOMPARE(tb.transitions.size(), ta.transitions.size());
            for (size_t i = 0; i < ta.transitions.size(); ++i) QCOMPARE(tb.transitions[i].duration, ta.transitions[i].duration);
        }
    }

    void interchangeImports() {
        // OpenTimelineIO: everything comes back.
        {
            Fixture fx;
            interchangeFixture(fx);
            const Sequence original = fx.s();
            const std::string otio = exportOtio(fx.p, original);
            ImportResult r = importOtio(fx.p, otio);
            QVERIFY2(r.ok, r.error.c_str());
            const Sequence& back = *fx.p.findSequence(r.sequence);
            QCOMPARE(fx.p.activeSequence, r.sequence);
            QCOMPARE(back.fps, original.fps);
            QCOMPARE(fx.p.media.size(), size_t(1));  // the same file is not added twice
            QVERIFY(r.offline.empty());
            compareTimelines(original, back, true, 2);
            // Picture and sound are linked again, the title kept its text, the marker came back.
            QVERIFY(back.videoTracks[0].clips[0].linkGroup != 0);
            QCOMPARE(back.videoTracks[0].clips[0].linkGroup, back.audioTracks[0].clips[0].linkGroup);
            QCOMPARE(back.videoTracks[1].clips.at(0).generator.s("text"), std::string("Hello"));
            QCOMPARE(back.markers.size(), size_t(1));
            QCOMPARE(back.markers[0].comment, std::string("first beat"));
        }
        // EDL: one video track and the audio, cuts and dissolves.
        {
            Fixture fx;
            interchangeFixture(fx);
            const Sequence original = fx.s();
            const std::string edl = exportEdl(fx.p, original);
            ImportResult r = importEdl(fx.p, edl, original.fps);
            QVERIFY2(r.ok, r.error.c_str());
            const Sequence& back = *fx.p.findSequence(r.sequence);
            QCOMPARE(fx.p.media.size(), size_t(1));  // found by its SOURCE FILE comment
            compareTimelines(original, back, false, 1);
        }
        // An OTIO file in the older style (media_reference, 24 fps) from another tool.
        {
            Project p = makeDefaultProject();
            const char* otio = R"({"OTIO_SCHEMA":"Timeline.1","name":"From Resolve","tracks":{"OTIO_SCHEMA":"Stack.1","children":[
              {"OTIO_SCHEMA":"Track.1","kind":"Video","name":"Video 1","children":[
                {"OTIO_SCHEMA":"Gap.1","source_range":{"OTIO_SCHEMA":"TimeRange.1","start_time":{"OTIO_SCHEMA":"RationalTime.1","rate":24,"value":0},"duration":{"OTIO_SCHEMA":"RationalTime.1","rate":24,"value":24}}},
                {"OTIO_SCHEMA":"Clip.1","name":"A001_C002","source_range":{"OTIO_SCHEMA":"TimeRange.1","start_time":{"OTIO_SCHEMA":"RationalTime.1","rate":24,"value":48},"duration":{"OTIO_SCHEMA":"RationalTime.1","rate":24,"value":72}},
                 "media_reference":{"OTIO_SCHEMA":"ExternalReference.1","target_url":"file:///nowhere/A001_C002.mov","available_range":{"OTIO_SCHEMA":"TimeRange.1","start_time":{"OTIO_SCHEMA":"RationalTime.1","rate":24,"value":0},"duration":{"OTIO_SCHEMA":"RationalTime.1","rate":24,"value":240}}}}]}]}})";
            ImportResult r = importOtio(p, otio);
            QVERIFY2(r.ok, r.error.c_str());
            const Sequence& s = *p.findSequence(r.sequence);
            QCOMPARE(s.fps, (Rational{24, 1}));
            QCOMPARE(s.name, std::string("From Resolve"));
            const Clip& c = s.videoTracks.at(0).clips.at(0);
            QCOMPARE(c.start, FrameTime(24));
            QCOMPARE(c.duration, FrameTime(72));
            QCOMPARE(c.sourceIn, 48.0);
            QCOMPARE(r.offline.size(), size_t(1));  // the file is not here: offline, ten seconds long
            QCOMPARE(p.findMedia(c.mediaId)->duration, 10.0);
            QCOMPARE(p.findMedia(c.mediaId)->path, std::string("/nowhere/A001_C002.mov"));
        }
        // A Premiere-style EDL: A/V events, a dissolve, clip names without paths.
        {
            Project p = makeDefaultProject();
            const char* edl =
                "TITLE: Rough Cut\nFCM: NON-DROP FRAME\n\n"
                "001  AX       AA/V  C        00:00:00:00 00:00:04:00 01:00:00:00 01:00:04:00\n"
                "* FROM CLIP NAME: interview.mov\n\n"
                "002  AX       V     C        00:00:10:00 00:00:10:00 01:00:04:00 01:00:04:00\n"
                "002  AX       V     D    012 00:00:20:00 00:00:24:00 01:00:04:00 01:00:08:00\n"
                "* FROM CLIP NAME: interview.mov\n* TO CLIP NAME: broll.mov\n";
            ImportResult r = importEdl(p, edl, {25, 1});
            QVERIFY2(r.ok, r.error.c_str());
            const Sequence& s = *p.findSequence(r.sequence);
            QCOMPARE(s.name, std::string("Rough Cut"));
            QCOMPARE(s.videoTracks.at(0).clips.size(), size_t(2));
            const Clip& a = s.videoTracks[0].clips[0];
            const Clip& b = s.videoTracks[0].clips[1];
            // Record timecode 01:00:00:00 is where the EDL's timeline starts.
            QCOMPARE(a.start, FrameTime(25 * 3600));
            QCOMPARE(b.start - a.start, FrameTime(100 + 6));  // the dissolve is centred on the cut
            QCOMPARE(a.end(), b.start);
            QCOMPARE(s.videoTracks[0].transitions.size(), size_t(1));
            QCOMPARE(s.videoTracks[0].transitions[0].duration, FrameTime(12));
            QCOMPARE(s.audioTracks.at(0).clips.size(), size_t(1));
            QCOMPARE(s.audioTracks.at(1).clips.size(), size_t(1));  // AA: both channels
            QCOMPARE(r.offline.size(), size_t(2));
            QCOMPARE(p.findMedia(b.mediaId)->name, std::string("broll.mov"));
            QCOMPARE(b.sourceIn, 500.0 + 6);
        }
        // Final Cut Pro 7 XML and FCPXML: our exports read back to the same timeline.
        for (int flavour = 0; flavour < 2; ++flavour) {
            Fixture fx;
            interchangeFixture(fx);
            const Sequence original = fx.s();
            const std::string xml = flavour == 0 ? exportFcp7Xml(fx.p, original) : exportFcpXml(fx.p, original);
            QVERIFY(xml.find(flavour == 0 ? "<xmeml version=\"5\">" : "<fcpxml version=\"1.10\">") != std::string::npos);
            ImportResult r = importXmlTimeline(fx.p, xml);
            QVERIFY2(r.ok, r.error.c_str());
            const Sequence& back = *fx.p.findSequence(r.sequence);
            QCOMPARE(back.fps, original.fps);
            QCOMPARE(back.width, original.width);
            QCOMPARE(fx.p.media.size(), size_t(1));
            compareTimelines(original, back, true, 2);
            QVERIFY(back.videoTracks[0].clips[0].linkGroup != 0);
            QCOMPARE(back.videoTracks[0].clips[0].linkGroup, back.audioTracks[0].clips[0].linkGroup);
            QCOMPARE(back.videoTracks[1].clips.at(0).generator.type, std::string("title"));
            QCOMPARE(back.videoTracks[1].clips.at(0).generator.s("text"), std::string("Hello"));
            QCOMPARE(back.markers.size(), size_t(1));
            QCOMPARE(back.markers[0].t, FrameTime(10));
        }
        // FCPXML as Final Cut writes it: 29.97, a timecode start, a connected title and audio.
        {
            Project p = makeDefaultProject();
            const std::string fcpxml = readData("interchange/final-cut.fcpxml");
            ImportResult r = importXmlTimeline(p, fcpxml);
            QVERIFY2(r.ok, r.error.c_str());
            const Sequence& s = *p.findSequence(r.sequence);
            QCOMPARE(s.name, std::string("Edit 3"));
            QCOMPARE(s.fps, (Rational{30000, 1001}));
            const Clip& v = s.videoTracks.at(0).clips.at(0);
            QCOMPARE(v.start, FrameTime(0));
            QCOMPARE(v.duration, FrameTime(150));
            // The asset starts at timecode 01:00:00:00; 3603.6 s into it is 3.6 s into the file.
            QVERIFY(std::fabs(v.sourceIn - 108) < 0.01);
            QCOMPARE(s.audioTracks.at(0).clips.size(), size_t(1));  // the interview's own sound, linked
            QCOMPARE(s.audioTracks[0].clips[0].linkGroup, v.linkGroup);
            const Clip& title = s.videoTracks.at(1).clips.at(0);
            QCOMPARE(title.start, FrameTime(3));  // 111111 - 108108 = 3003/30000 s = 3 frames into the clip
            QCOMPARE(title.generator.s("text"), std::string("Jane Doe"));
            const Clip& music = s.audioTracks.at(1).clips.at(0);
            QCOMPARE(music.start, FrameTime(0));
            QCOMPARE(s.markers.size(), size_t(1));
            QCOMPARE(s.markers[0].t, FrameTime(12));
            QCOMPARE(s.markers[0].name, std::string("Good line"));
            QCOMPARE(r.offline.size(), size_t(2));
        }
        // FCP 7 XML as Premiere writes it: -1 edges around a dissolve, links, file references.
        {
            Project p = makeDefaultProject();
            const std::string xmeml = readData("interchange/premiere.xml");
            ImportResult r = importXmlTimeline(p, xmeml);
            QVERIFY2(r.ok, r.error.c_str());
            const Sequence& s = *p.findSequence(r.sequence);
            QCOMPARE(s.fps, (Rational{25, 1}));
            QCOMPARE(s.width, 1280);
            const auto& v = s.videoTracks.at(0).clips;
            QCOMPARE(v.size(), size_t(2));
            QCOMPARE(v[0].start, FrameTime(0));
            QCOMPARE(v[0].end(), FrameTime(110));  // cut in the middle of the dissolve
            QCOMPARE(v[1].start, FrameTime(110));
            QCOMPARE(v[1].sourceIn, 60.0);
            QCOMPARE(s.videoTracks[0].transitions.size(), size_t(1));
            QCOMPARE(s.videoTracks[0].transitions[0].duration, FrameTime(20));
            QCOMPARE(v[0].mediaId, v[1].mediaId);  // one file, referenced twice
            QVERIFY(v[0].linkGroup != 0);
            QCOMPARE(s.audioTracks.at(0).clips.at(0).linkGroup, v[0].linkGroup);
        }
        // Not a timeline.
        Project p = makeDefaultProject();
        QVERIFY(!importXmlTimeline(p, "<html/>").ok);
        QVERIFY(!importXmlTimeline(p, "not xml").ok);
        QVERIFY(!importOtio(p, "{}").ok);
        QVERIFY(!importEdl(p, "nothing here", {25, 1}).ok);
    }

    void projectFileRelinksRelativePaths() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QDir(dir.path()).mkpath("proj/media");
        QString mediaPath = dir.path() + "/proj/media/a.mov";
        QFile f(mediaPath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("x");
        f.close();
        Fixture fx;
        fx.p.media[0].path = mediaPath.toStdString();
        std::string projPath = (dir.path() + "/proj/p.montage").toStdString();
        QVERIFY(saveProject(fx.p, projPath));
        // Move the whole folder; the absolute path breaks, the relative one still resolves.
        QVERIFY(QDir().rename(dir.path() + "/proj", dir.path() + "/moved"));
        Project back;
        QVERIFY(loadProject((dir.path() + "/moved/p.montage").toStdString(), back));
        QCOMPARE(QString::fromStdString(back.media[0].path), dir.path() + "/moved/media/a.mov");
    }
};

QTEST_GUILESS_MAIN(TestCore)
#include "test_core.moc"
