// Engine tests: keyframes, timecode, edit operations, undo, project I/O.
#include <QtTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "core/EditOps.h"
#include "core/Effects.h"
#include "core/History.h"
#include "core/Interchange.h"
#include "core/ProjectIO.h"

using namespace montage;
using namespace montage::edit;

namespace {

const TrackRef V1{TrackKind::Video, 0};
const TrackRef V2{TrackKind::Video, 1};
const TrackRef A1{TrackKind::Audio, 0};

// Project with one 30 fps sequence and a 10 s A/V media item (no file needed).
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
        std::string json = projectToJson(fx.p);
        Project back;
        std::string err;
        QVERIFY2(projectFromJson(json, back, &err), err.c_str());
        QVERIFY(back == fx.p);
        // Garbage is rejected with a message.
        QVERIFY(!projectFromJson("{nope", back, &err));
        QVERIFY(!err.empty());
        QVERIFY(!projectFromJson("{\"format\":\"other\"}", back, &err));
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
