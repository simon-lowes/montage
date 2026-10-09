// Engine tests: keyframes, timecode, edit operations, undo, project I/O.
#include <QtTest>
#include <random>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "core/AutoTag.h"
#include "core/Automation.h"
#include "core/Captions.h"
#include "core/Bleep.h"
#include "core/Cfb.h"
#include "core/Chapters.h"
#include "core/Checkerboard.h"
#include "core/EditOps.h"
#include "core/EffectPresets.h"
#include "core/Effects.h"
#include "core/History.h"
#include "core/Interchange.h"
#include "core/MarkerList.h"
#include "core/KeyframeEdit.h"
#include "core/MaskPath.h"
#include "core/MediaLog.h"
#include "core/Multicam.h"
#include "core/TimelineCompare.h"
#include "core/ProjectIO.h"
#include "core/ScriptCut.h"
#include "core/Surround.h"
#include "core/Transcript.h"
#include "core/Slate.h"
#include "core/TranscriptEdit.h"
#include "core/Zip.h"
#include "core/G2p.h"

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
    void numbersAndStressForSpeech() {
        QCOMPARE(numberWords(0), std::string("zero"));
        QCOMPARE(numberWords(42), std::string("forty-two"));
        QCOMPARE(numberWords(1200), std::string("one thousand, two hundred"));
        QCOMPARE(numberWords(1000001), std::string("one million and one"));
        QCOMPARE(numberWords(-7), std::string("minus seven"));
        QCOMPARE(ordinalWords(3), std::string("third"));
        QCOMPARE(ordinalWords(21), std::string("twenty-first"));
        QCOMPARE(ordinalWords(40), std::string("fortieth"));
        QCOMPARE(ordinalWords(112), std::string("one hundred and twelfth"));
        QCOMPARE(yearWords(2026), std::string("twenty twenty-six"));
        QCOMPARE(yearWords(1905), std::string("nineteen oh-five"));
        QCOMPARE(yearWords(2005), std::string("two thousand and five"));
        QCOMPARE(yearWords(1900), std::string("nineteen hundred"));
        QCOMPARE(yearWords(2000), std::string("two thousand"));
        // misaki's stress moves: marks go before their vowel.
        QCOMPARE(applyStress("hələ", 2), std::string("hˈələ"));
        QCOMPARE(applyStress("ˈhOm", -1), std::string("ˌhOm"));
        QCOMPARE(applyStress("ˈhOm", -2), std::string("hOm"));
        QCOMPARE(applyStress("bˌæk", 1), std::string("bˈæk"));
        QCOMPARE(applyStress("mm", 2), std::string("mm"));  // no vowel, nothing to stress
        // Unknown words sounded out, the first syllable stressed.
        QCOMPARE(spellingToPhonemes("zorblat"), std::string("zˈɔɹblæt"));
        QCOMPARE(spellingToPhonemes("chime"), std::string("ʧˈIm"));
    }

    void zipReaderAndInflate() {
        // Raw DEFLATE from Qt's zlib stream: without its length prefix, header and checksum.
        auto deflate = [](const QByteArray& data, int level) {
            const QByteArray z = qCompress(data, level);
            return z.mid(6, z.size() - 10);
        };
        QByteArray text;
        for (int i = 0; i < 2000; ++i) text += QByteArray::number(i * 7919 % 1000) + (i % 3 ? " the quick brown fox " : "\n");
        QByteArray noise(70000, Qt::Uninitialized);
        std::mt19937 rng(7);
        for (char& c : noise) c = char(rng());
        // Dynamic Huffman (level 9), fixed Huffman (a short string), stored blocks (level 0, over 64 KiB).
        for (const auto& [data, level] : {std::pair{text, 9}, std::pair{QByteArray("hello hello hello"), 9}, std::pair{noise, 0}, std::pair{noise, 6}}) {
            const QByteArray raw = deflate(data, level);
            std::string out;
            QVERIFY(inflateRaw(reinterpret_cast<const uint8_t*>(raw.constData()), size_t(raw.size()), out));
            QCOMPARE(QByteArray::fromStdString(out), data);
            // Cut short, it fails rather than returning part.
            std::string part;
            QVERIFY(!inflateRaw(reinterpret_cast<const uint8_t*>(raw.constData()), size_t(raw.size() / 2), part));
        }
        QCOMPARE(crc32("123456789", 9), 0xCBF43926u);

        // A zip with a stored and a deflated entry, laid out by hand.
        struct In {
            std::string name;
            QByteArray data;
            bool deflated;
        };
        const std::vector<In> files{{"a/stored.txt", "plain bytes", false}, {"a/text.txt", text, true}};
        QByteArray zip, central;
        auto u16 = [](QByteArray& b, int v) { b.append(char(v & 0xFF)).append(char((v >> 8) & 0xFF)); };
        auto u32 = [&](QByteArray& b, uint32_t v) { u16(b, int(v & 0xFFFF)), u16(b, int(v >> 16)); };
        for (const In& f : files) {
            const QByteArray body = f.deflated ? deflate(f.data, 9) : f.data;
            const uint32_t crc = crc32(f.data.constData(), size_t(f.data.size())), offset = uint32_t(zip.size());
            for (QByteArray* b : {&zip, &central}) {
                const bool c = b == &central;
                u32(*b, c ? 0x02014b50 : 0x04034b50);
                if (c) u16(*b, 20);
                u16(*b, 20), u16(*b, 0), u16(*b, f.deflated ? 8 : 0), u16(*b, 0), u16(*b, 0);
                u32(*b, crc), u32(*b, uint32_t(body.size())), u32(*b, uint32_t(f.data.size()));
                u16(*b, int(f.name.size())), u16(*b, 0);
                if (c) u16(*b, 0), u16(*b, 0), u16(*b, 0), u32(*b, 0), u32(*b, offset);
                b->append(f.name.c_str());
            }
            zip += body;
        }
        const uint32_t cdOffset = uint32_t(zip.size());
        zip += central;
        u32(zip, 0x06054b50), u16(zip, 0), u16(zip, 0), u16(zip, 2), u16(zip, 2);
        u32(zip, uint32_t(central.size())), u32(zip, cdOffset), u16(zip, 0);
        ZipReader r;
        QVERIFY(r.open(zip.toStdString()));
        QCOMPARE(int(r.entries().size()), 2);
        std::string got, err;
        QVERIFY2(r.read("a/text.txt", got, &err), err.c_str());
        QCOMPARE(QByteArray::fromStdString(got), text);
        QVERIFY(r.read("a/stored.txt", got));
        QCOMPARE(got, std::string("plain bytes"));
        QVERIFY(!r.read("missing", got, &err));
        // A damaged byte is caught by the checksum.
        QByteArray bad = zip;
        bad[30 + 12 + 2] = char(bad[30 + 12 + 2] ^ 0x20);  // inside "plain bytes", after the local header and name
        QVERIFY(r.open(bad.toStdString()));
        QVERIFY(!r.read("a/stored.txt", got, &err));
        QVERIFY(QString::fromStdString(err).contains("checksum"));
        QVERIFY(!r.open("not a zip at all, just some text that is long enough"));
    }

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
        // Caption 1 shows at frame 30 (its first end-of-caption code): 11 pairs loaded from frame 21; cleared at 90.
        QVERIFY2(scc.find("\n00:00:00;21\t9420") != std::string::npos, scc.c_str());
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

    void captionFormats() {
        const Rational fps{25, 1};
        const std::vector<Caption> caps = {{25, 75, "Caf\xC3\xA9 & <na\xC3\xAFve>\nsecond line"}, {100, 150, "\xE2\x99\xAA Pi\xC3\xB1" "a colada \xC2\xBD"}};
        std::vector<Caption> back;
        std::string err;
        auto same = [](const std::vector<Caption>& a, const std::vector<Caption>& b) {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i)
                if (a[i].start != b[i].start || a[i].end != b[i].end || a[i].text != b[i].text) return false;
            return true;
        };

        // TTML (IMSC 1.1): escaped text, lines as <br/>, the style's colours; and back.
        CaptionStyle style;
        style.textR = 1, style.textG = 1, style.textB = 0;
        const std::string ttml = captionsToTtml(caps, fps, "fr", style);
        QVERIFY(ttml.find("ttp:profile=\"http://www.w3.org/ns/ttml/profile/imsc1.1/text\"") != std::string::npos);
        QVERIFY(ttml.find("xml:lang=\"fr\"") != std::string::npos);
        QVERIFY(ttml.find("tts:color=\"#ffff00\"") != std::string::npos);
        QVERIFY2(ttml.find("begin=\"00:00:01.000\" end=\"00:00:03.000\"><span style=\"caption\">Caf\xC3\xA9 &amp; &lt;na\xC3\xAFve&gt;<br/>second line</span>") !=
                     std::string::npos,
                 ttml.c_str());
        QVERIFY(parseSubtitles(ttml, fps, back, &err));
        QVERIFY(same(back, caps));
        // Others' TTML: frames at 24 fps, offsets, times nested in a div, a duration, ticks, spans.
        const std::string theirs =
            "<?xml version=\"1.0\"?>\n<tt xmlns=\"http://www.w3.org/ns/ttml\" xmlns:ttp=\"http://www.w3.org/ns/ttml#parameter\" "
            "ttp:frameRate=\"24\" ttp:tickRate=\"10000000\">\n<body><div>\n"
            "  <p begin=\"00:00:01:12\" end=\"00:00:02:00\">Half\n    a second</p>\n"
            "  <p begin=\"2.5s\" end=\"3000ms\"><span>One</span><br/><span tts:color=\"red\">two</span></p>\n"
            "  <p begin=\"40000000t\" dur=\"1s\">Ticks</p>\n"
            "</div><div begin=\"10s\"><p begin=\"1s\" end=\"2s\">Nested</p><p begin=\"3s\">No end</p></div></body></tt>\n";
        QVERIFY(parseSubtitles(theirs, fps, back, &err));
        QCOMPARE(back.size(), size_t(4));
        QCOMPARE(back[0].start, FrameTime(38));  // 1.5 s
        QCOMPARE(back[0].end, FrameTime(50));
        QCOMPARE(back[0].text, std::string("Half a second"));
        QCOMPARE(back[1].start, FrameTime(63));  // 2.5 s, rounded
        QCOMPARE(back[1].end, FrameTime(75));
        QCOMPARE(back[1].text, std::string("One\ntwo"));
        QCOMPARE(back[2].start, FrameTime(100));
        QCOMPARE(back[2].end, FrameTime(125));
        QCOMPARE(back[3].start, FrameTime(275));
        QCOMPARE(back[3].end, FrameTime(300));
        QCOMPARE(back[3].text, std::string("Nested"));
        QVERIFY(!parseTtml("<tt xmlns=\"http://www.w3.org/ns/ttml\"><body><p>oops", fps, back, &err));
        QVERIFY(!err.empty());

        // EBU STL: the GSI block, one 128-byte block a subtitle, ISO 6937 text; and back.
        const std::string stl = captionsToStl(caps, fps, "fr", "Film");
        QCOMPARE(stl.size(), size_t(1024 + 2 * 128));
        QCOMPARE(stl.substr(0, 16), std::string("850STL25.01100" "0F"));
        QCOMPARE(stl.substr(16, 4), std::string("Film"));
        QCOMPARE(stl.substr(238, 10), std::string("0000200002"));
        const auto* tti = reinterpret_cast<const uint8_t*>(stl.data() + 1024);
        QCOMPARE(int(tti[3]), 0xFF);  // the last block of its subtitle
        QVERIFY(tti[5] == 0 && tti[6] == 0 && tti[7] == 1 && tti[8] == 0);    // in at 00:00:01:00
        QVERIFY(tti[9] == 0 && tti[10] == 0 && tti[11] == 3 && tti[12] == 0);  // out at 00:00:03:00
        QCOMPARE(int(tti[13]), 20);  // two lines: from row 20
        QCOMPARE(int(tti[14]), 2);   // centred
        // é is the acute diacritic (c2) before e; & stays; lines are double height in boxes.
        const std::string tf(stl.data() + 1024 + 16, 112);
        QVERIFY(tf.find("\x0D\x0B\x0B" "Caf\xC2" "e & <na\xC8" "ive>\x0A\x0A\x8A\x8A\x0D\x0B\x0B" "second line\x0A\x0A\x8F") == 0);
        QVERIFY(parseSubtitles(stl, fps, back, &err));
        QVERIFY2(same(back, caps), back.empty() ? "" : back[0].text.c_str());
        // A long subtitle runs into an extension block; a 30 fps sequence writes STL30.01.
        const std::string longText = "Thirty five characters on line one\nthirty five characters on line two\nand a third line of thirty five ch";
        const std::string two = captionsToStl({{30, 90, longText}}, Rational{30, 1});
        QCOMPARE(two.substr(3, 8), std::string("STL30.01"));
        QCOMPARE(two.size(), size_t(1024 + 2 * 128));
        QCOMPARE(int(uint8_t(two[1024 + 3])), 0);
        QCOMPARE(int(uint8_t(two[1024 + 128 + 3])), 0xFF);
        QVERIFY(parseSubtitles(two, Rational{30, 1}, back, &err));
        QCOMPARE(back.size(), size_t(1));
        QCOMPARE(back[0].text, longText);
        QCOMPARE(back[0].start, FrameTime(30));
        // Times count from the start of programme (here 10:00:00:00).
        std::string late = stl;
        late.replace(256, 8, "10000000");
        for (size_t b = 1024; b < late.size(); b += 128) late[b + 5] = char(late[b + 5] + 10), late[b + 9] = char(late[b + 9] + 10);
        QVERIFY(parseSubtitles(late, fps, back, &err));
        QCOMPARE(back.front().start, FrameTime(25));

        // ASS: the track's style at the frame size; and back, override tags and comments ignored.
        CaptionStyle box;
        box.font = "Source Sans 3";
        box.size = 0.05;
        box.boxOpacity = 0.5;
        const std::string ass = captionsToAss(caps, fps, box, 1920, 1080);
        QVERIFY(ass.find("PlayResX: 1920\nPlayResY: 1080") != std::string::npos);
        QVERIFY2(ass.find("Style: Default,Source Sans 3,54,&H00FFFFFF,&H00FFFFFF,&H7F000000,&H7F000000,0,0,0,0,100,100,0,0,3,8,0,2,96,96,86,1") !=
                     std::string::npos,
                 ass.c_str());
        QVERIFY(ass.find("Dialogue: 0,0:00:01.00,0:00:03.00,Default,,0,0,0,,Caf\xC3\xA9 & <na\xC3\xAFve>\\Nsecond line") != std::string::npos);
        QVERIFY(parseSubtitles(ass, fps, back, &err));
        QVERIFY(same(back, caps));
        const std::string theirAss =
            "[Script Info]\nTitle: x\n\n[Events]\nFormat: Layer, Start, End, Style, Actor, MarginL, MarginR, MarginV, Effect, Text\n"
            "Comment: 0,0:00:00.00,0:00:05.00,Default,,0,0,0,,not shown\n"
            "Dialogue: 0,0:00:01.50,0:00:02.00,Default,Bob,0,0,0,,{\\i1}Well,{\\i0} yes\\Nand\\hno\n";
        QVERIFY(parseSubtitles(theirAss, fps, back, &err));
        QCOMPARE(back.size(), size_t(1));
        QCOMPARE(back[0].start, FrameTime(38));
        QCOMPARE(back[0].text, std::string("Well, yes\nand no"));

        // SCC read back: pop-on captions as written, accents and music notes included, on the same frames.
        const Rational ntsc{30000, 1001};
        const std::vector<Caption> pop = {{30, 90, "HI THERE"}, {150, 240, "Two\nlines \xC3\xA9 \xC3\x9C \xE2\x99\xAA"}};
        QVERIFY(parseSubtitles(captionsToScc(pop, ntsc), ntsc, back, &err));
        QVERIFY2(same(back, pop), back.size() == 2 ? (back[0].text + "|" + back[1].text + "|" + std::to_string(back[1].start) + "-" + std::to_string(back[1].end)).c_str() : "count");
        // Roll-up: each line stays until the next one replaces it.
        const std::string rollUp =
            "Scenarist_SCC V1.0\n\n00:00:01:00\t9425 9425 94ad 94ad 9470 9470 c845 4c4c 4f80\n\n"
            "00:00:03:00\t94ad 94ad 9470 9470 574f 524c c480\n\n00:00:05:00\t942c 942c\n";
        QVERIFY(parseSubtitles(rollUp, ntsc, back, &err));
        QCOMPARE(back.size(), size_t(2));
        QCOMPARE(back[0].text, std::string("HELLO"));
        QCOMPARE(back[0].start, FrameTime(36));
        QCOMPARE(back[0].end, FrameTime(94));
        QCOMPARE(back[1].text, std::string("WORLD"));
        QCOMPARE(back[1].end, FrameTime(150));

        // By extension.
        QVERIFY(captionFormatKnown(".XML") && captionFormatKnown("dfxp") && captionFormatKnown("ssa") && !captionFormatKnown(".docx"));
        Sequence seq;
        seq.fps = fps;
        CaptionTrack track;
        track.captions = caps;
        QCOMPARE(exportCaptions(track, seq, ".srt"), captionsToSrt(caps, fps));
        QVERIFY(exportCaptions(track, seq, ".dfxp").find("<tt ") != std::string::npos);
        QVERIFY(exportCaptions(track, seq, ".stl").substr(3, 8) == "STL25.01");
        QVERIFY(exportCaptions(track, seq, ".docx").empty());
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
        // Each word's time comes along, as a fraction of the caption: "three." at 3.0 s of media is
        // 0.25 s into the clip's timeline (frame 106.25 of 100-130), "Four" 112.5, "five" 118.75, "six." 125.
        QCOMPARE(caps[0].wordTimes.size(), size_t(5));
        const double want[] = {0, 6.25 / 30, 12.5 / 30, 18.75 / 30, 25.0 / 30};
        for (int i = 0; i < 5; ++i) QVERIFY2(std::fabs(caps[0].wordTimes[size_t(i)] - want[i]) < 1e-3, qPrintable(QString::number(caps[0].wordTimes[size_t(i)])));
        QCOMPARE(captionWordAt(caps[0], 99), -1);
        QCOMPARE(captionWordAt(caps[0], 100), 0);
        QCOMPARE(captionWordAt(caps[0], 113), 2);
        QCOMPARE(captionWordAt(caps[0], 129), 4);
        // Without times (or with the text changed), words are spread by their length.
        Caption manual{0, 40, "ab\ncdef", {}};
        const auto spread = captionWordStarts(manual);
        QCOMPARE(spread.size(), size_t(2));
        QCOMPARE(spread[0], 0.0);
        QCOMPARE(spread[1], 3.0 / 8);
        manual.wordTimes = {0, 0.5, 0.9};  // three times for two words: not used
        QCOMPARE(captionWordStarts(manual), spread);
        // Word times and the animation style are saved with the project.
        CaptionTrack ct;
        ct.id = p.newId();
        ct.style.animation = 3;
        ct.style.hiR = 0.2;
        ct.captions = caps;
        s.captionTracks.push_back(ct);
        Project back;
        QVERIFY(projectFromJson(projectToJson(p), back));
        const CaptionTrack& bt = back.active()->captionTracks.at(0);
        QCOMPARE(bt.style.animation, 3);
        QCOMPARE(bt.style.hiR, 0.2);
        QCOMPARE(bt.captions[0].wordTimes.size(), size_t(5));
        QVERIFY(std::fabs(bt.captions[0].wordTimes[2] - want[2]) < 1e-3);
        s.captionTracks.clear();
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

    void retakes() {
        // Words at 0.4 s each, from `at`.
        auto say = [](const char* text, double at = 0) {
            std::vector<TranscriptWord> out;
            for (const QString& w : QString(text).split(' ', Qt::SkipEmptyParts)) {
                out.push_back({at, at + 0.35, w.toStdString(), 1});
                at += 0.4;
            }
            return out;
        };
        // Broken off with a filler, then said again: the first attempt goes, up to where the kept take starts.
        std::vector<std::pair<size_t, size_t>> takes;
        auto w = say("So today we're going to, um, so today we're going to talk about the budget.");
        auto r = retakeRanges(w, 25, 3, 30, &takes);
        QCOMPARE(takes.size(), size_t(1));
        QCOMPARE(takes[0], (std::pair<size_t, size_t>{0, 6}));
        QCOMPARE(r.size(), size_t(1));
        QCOMPARE(r[0], (FrameRange{0, FrameTime(std::llround(6 * 0.4 * 25))}));
        // Three attempts: only the last stays.
        takes.clear();
        w = say("The plan is the plan is, sorry, the plan is simple.");
        r = retakeRanges(w, 25, 3, 30, &takes);
        QCOMPARE(r.size(), size_t(1));
        QCOMPARE(r[0].first, FrameTime(0));
        QCOMPARE(r[0].second, FrameTime(std::llround(7 * 0.4 * 25)));  // "the" of the third "the plan is"
        // A finished sentence said again for effect is kept, as is a repeat of little words.
        QVERIFY(retakeRanges(say("We need to act. We need to act now."), 25).empty());
        QVERIFY(retakeRanges(say("it is in it is in the box"), 25).empty());
        // Too far apart to be a retake.
        std::vector<TranscriptWord> far = say("today we're going to");
        for (int i = 0; i < 40; ++i) far.push_back({far.back().end + 0.05, far.back().end + 0.4, "word" + std::to_string(i), 1});
        const auto again = say("today we're going to", far.back().end + 0.1);
        far.insert(far.end(), again.begin(), again.end());
        QVERIFY(retakeRanges(far, 25).empty());
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
        // With Smooth Cuts, each join in the picture gets one (and the sound none).
        Project smooth = p;
        QVERIFY(rippleDeleteRanges(smooth, *smooth.active(), {pauses[0], fillers[0]}, 5).ok);
        const Track& sv = smooth.active()->videoTracks[0];
        QCOMPARE(sv.transitions.size(), size_t(2));
        QVERIFY(smooth.active()->audioTracks[0].transitions.empty());
        for (const Transition& tr : sv.transitions) {
            QCOMPARE(tr.type, std::string("smooth_cut"));
            QCOMPARE(tr.duration, FrameTime(5));
            const Clip* a = edit::clipById(*smooth.active(), tr.clipA);
            const Clip* b = edit::clipById(*smooth.active(), tr.clipB);
            QVERIFY(a && b && a->end() == b->start);
            QVERIFY(a->end() == 38 || a->end() == 63 - 10);  // the filler's place, then the pause's less the filler
        }
        auto r = rippleDeleteRanges(p, s, {pauses[0], fillers[0]});
        QVERIFY(r.ok);
        QVERIFY(s.videoTracks[0].transitions.empty());
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

        // Moving several keys together: as far as the keys that stay and the clip allow.
        Param many;
        for (FrameTime t : {10, 20, 30, 40}) many.addKey(t, double(t));
        QCOMPARE(shiftRange(many, {20, 30}, 99), (std::pair<FrameTime, FrameTime>{-9, 9}));
        QCOMPARE(shiftRange(many, {40}, 50), (std::pair<FrameTime, FrameTime>{-9, 10}));
        QCOMPARE(shiftRange(many, {10, 20, 30, 40}, 99), (std::pair<FrameTime, FrameTime>{-10, 59}));
        shiftKeys(many, {20, 30}, 5);
        QCOMPARE(many.keys[1].t, FrameTime(25));
        QCOMPARE(many.keys[1].v, 20.0);  // values travel with their keys
        QCOMPARE(many.keys[2].t, FrameTime(35));
        // Addressing a clip's parameters.
        Fixture fx;
        Clip c = makeClip(fx.p, *fx.p.findMedia(fx.media), TrackKind::Video, fx.s());
        c.effects.push_back(makeEffect(fx.p, "gaussian_blur"));
        c.effects.back().params["radius"].addKey(5, 3);
        const ParamAddress blur{ParamSlot::Effect, c.effects.back().id, "radius"};
        QVERIFY(findParam(c, blur) && findParam(c, blur)->keyAt(5));
        QVERIFY(!findParam(c, ParamAddress{ParamSlot::Effect, 999999, "radius"}));
        QCOMPARE(paramOwner(c, ParamAddress{ParamSlot::Motion, 0, "opacity"}), &c.motion);
        QVERIFY(findParam(c, ParamAddress{ParamSlot::Motion, 0, "opacity"}));
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

    void bleepWordsAndCaptions() {
        QCOMPARE(maskWord("damn,"), std::string("d***,"));
        QCOMPARE(maskWord("\"Shit!\""), std::string("\"S***!\""));
        QVERIFY(isProfanity("Fucking.") && isProfanity("BULLSHIT") && !isProfanity("duck") && !isProfanity("hello"));
        Effect e = makeEffect("bleep", 1);
        setBleepRanges(e, {{2.0, 2.5}, {1.0, 1.2}, {2.4, 3.0}});
        const auto r = bleepRanges(e);
        QCOMPARE(r.size(), size_t(2));
        QCOMPARE(r[1], (SecondsRange{2.0, 3.0}));
        // A clip playing source 10-20 s from timeline 4 s (25 fps), and a caption over the words.
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.fps = {25, 1};
        MediaItem m;
        m.id = p.newId();
        m.kind = MediaKind::Audio;
        m.hasAudio = true;
        m.duration = 30;
        m.transcript = std::make_shared<Transcript>();
        p.media.push_back(m);
        Clip c;
        c.id = p.newId();
        c.mediaId = m.id;
        c.start = 100;
        c.duration = 250;
        c.sourceIn = 250;
        s.audioTracks[0].clips.push_back(c);
        CaptionTrack ct;
        Caption cap;
        cap.start = 125;
        cap.end = 175;
        cap.text = "well damn it\nall";
        cap.wordTimes = {0.0, 0.3, 0.6, 0.8};
        ct.captions.push_back(cap);
        s.captionTracks.push_back(ct);
        TranscriptWord w;
        w.text = "damn";
        w.start = 5.6;  // timeline seconds: frames 140-150, source 11.6-12.0 s
        w.end = 6.0;
        const edit::Result res = bleepWords(p, s, {w});
        QVERIFY2(res.ok, res.error.c_str());
        const Clip& after = s.audioTracks[0].clips[0];
        QCOMPARE(after.effects.size(), size_t(1));
        const auto br = bleepRanges(after.effects[0]);
        QCOMPARE(br.size(), size_t(1));
        QVERIFY(std::fabs(br[0].first - 11.6) < 1e-6 && std::fabs(br[0].second - 12.0) < 1e-6);
        QCOMPARE(s.captionTracks[0].captions[0].text, std::string("well d*** it\nall"));
        // Bleeping again adds to the same effect; nothing under a word is an error.
        w.start = 8.0;
        w.end = 8.2;
        QVERIFY(bleepWords(p, s, {w}).ok);
        QCOMPARE(bleepRanges(s.audioTracks[0].clips[0].effects[0]).size(), size_t(2));
        w.start = 50;
        w.end = 51;
        QVERIFY(!bleepWords(p, s, {w}).ok);
        // In the effects list it is there to remove, but not offered to add.
        QVERIFY(findEffectInfo("bleep") && findEffectInfo("bleep")->hidden);
    }

    void checkerboardDialogue() {
        // An interview at 25 fps: the host (0), a guest (1), the host again, a second guest (2).
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.fps = {25, 1};
        while (s.audioTracks.size() < 2) edit::addTrack(p, s, TrackKind::Audio);
        s.audioTracks.resize(2);
        MediaItem m;
        m.id = p.newId();
        m.name = "interview.wav";
        m.kind = MediaKind::Audio;
        m.hasAudio = true;
        m.duration = 20;
        auto t = std::make_shared<Transcript>();
        auto seg = [&](double a, double b, int speaker, const char* text) {
            TranscriptSegment g;
            g.start = a;
            g.end = b;
            g.speaker = speaker;
            g.text = text;
            t->segments.push_back(g);
        };
        seg(1.0, 3.0, 0, "So tell me");
        seg(3.0, 4.0, 0, "about it.");
        seg(4.5, 6.0, 1, "Well, it began");
        seg(6.6, 8.0, 0, "Really?");
        seg(8.2, 10.0, 2, "Yes, I was there.");
        seg(10.5, 12.0, 1, "Out of range");  // after the clip
        t->speakerNames = {"Host", "Ana", "Ben"};
        m.transcript = t;
        p.media.push_back(m);
        Clip c;
        c.id = p.newId();
        c.mediaId = m.id;
        c.start = 100;
        c.duration = 225;  // source 0.5 s to 9.5 s
        c.sourceIn = 12.5;
        c.audio = makeEffect(p, "volume");
        Param gain;
        gain.addKey(0, 0.0);
        gain.addKey(200, -6.0);
        c.audio.params["gain_db"] = gain;
        s.audioTracks[0].clips.push_back(c);
        // Something already on A2 where Ana speaks, so she gets a track of her own.
        Clip busy = c;
        busy.id = p.newId();
        busy.start = 200;
        busy.duration = 10;
        s.audioTracks[1].clips.push_back(busy);
        const size_t tracksBefore = s.audioTracks.size();

        int people = 0;
        const edit::Result r = checkerboardBySpeaker(p, s, c.id, &people);
        QVERIFY2(r.ok, r.error.c_str());
        QCOMPARE(people, 3);
        QCOMPARE(r.created.size(), size_t(4));
        // Cuts in the middle of each gap: 4.25 s, 6.3 s and 8.1 s of source, at 106.25, 157.5 and 202.5 frames.
        const Track& host = s.audioTracks[0];
        QCOMPARE(host.clips.size(), size_t(2));
        QCOMPARE(host.clips[0].start, FrameTime(100));
        QCOMPARE(host.clips[0].end(), FrameTime(194));   // 100 + 4.25 * 25 - 12.5
        QCOMPARE(host.clips[1].start, FrameTime(245));
        QCOMPARE(host.clips[1].end(), FrameTime(290));
        QCOMPARE(s.audioTracks.size(), tracksBefore + 1);  // Ana could not go on A2
        const Track& ana = s.audioTracks.back();
        QCOMPARE(ana.name, std::string("Ana"));
        QCOMPARE(ana.clips.size(), size_t(1));
        QCOMPARE(ana.clips[0].start, FrameTime(194));
        QCOMPARE(ana.clips[0].sourceIn, 12.5 + 94);  // carries on in the source where the host's part stopped
        QCOMPARE(ana.clips[0].linkGroup, Id(0));
        const Track& a2 = s.audioTracks[1];  // Ben fits on A2
        QCOMPARE(a2.clips.size(), size_t(2));
        QCOMPARE(a2.clips[1].start, FrameTime(290));
        QCOMPARE(a2.clips[1].end(), FrameTime(325));
        // Together they cover the clip, gain keyframes moved with each part.
        QCOMPARE(host.clips[1].audio.p("gain_db", 0), -6.0 * 145 / 200);
        // Errors: one speaker, no labels, video.
        QVERIFY(!checkerboardBySpeaker(p, s, host.clips[0].id).ok);
        Clip plain = c;
        plain.id = p.newId();
        plain.start = 1000;
        MediaItem bare = m;
        bare.id = p.newId();
        bare.transcript.reset();
        p.media.push_back(bare);
        plain.mediaId = bare.id;
        s.audioTracks[0].clips.push_back(plain);
        QVERIFY(!checkerboardBySpeaker(p, s, plain.id).ok);
    }

    void compoundFileRoundTrip() {
        // Storages in storages, many small streams (the mini stream), a big one, and names that sort by length first.
        CfbEntry root;
        root.storage = true;
        root.clsid[0] = 0xb3;
        std::mt19937 rng(3);
        for (int i = 0; i < 40; ++i) {
            CfbEntry st;
            st.storage = true;
            st.name = "Storage " + std::to_string(i);
            st.clsid[15] = uint8_t(i);
            for (int k = 0; k < 5; ++k) {
                CfbEntry s;
                s.name = std::string(size_t(k + 1), char('a' + k)) + std::to_string(i);
                s.data.resize(size_t(rng() % 300 + k * 7), char('0' + k));
                st.children.push_back(s);
            }
            root.children.push_back(st);
        }
        CfbEntry big;
        big.name = "big";
        for (int i = 0; i < 300000; ++i) big.data += char(rng() & 0xff);
        root.children.push_back(big);
        CfbEntry empty;
        empty.name = "empty";
        root.children.push_back(empty);
        QTemporaryDir dir;
        const std::string path = (dir.path() + "/test.cfb").toStdString();
        std::string err;
        QVERIFY2(writeCompoundFile(path, root, &err), err.c_str());
        CfbEntry back;
        QVERIFY2(readCompoundFile(path, back, &err), err.c_str());
        QCOMPARE(back.clsid[0], uint8_t(0xb3));
        QCOMPARE(back.children.size(), root.children.size());
        for (const CfbEntry& want : root.children) {
            const CfbEntry* got = back.find(want.name);
            QVERIFY2(got, want.name.c_str());
            QCOMPARE(got->storage, want.storage);
            QCOMPARE(got->data, want.data);
            QCOMPARE(got->clsid, want.clsid);
            for (const CfbEntry& w : want.children) {
                const CfbEntry* g = got->find(w.name);
                QVERIFY(g && g->data == w.data);
            }
        }
        QVERIFY(back.at("Storage 7/ccc7") && back.at("Storage 7/ccc7")->data.size() > 0);
        // Version 4: 4096-byte sectors.
        QFile f(QString::fromStdString(path));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray head = f.read(32);
        QCOMPARE(uint8_t(head[26]), uint8_t(4));
        QCOMPARE(uint8_t(head[30]), uint8_t(12));
    }

    void surroundPanning() {
        QCOMPARE(layoutChannels("stereo"), 2);
        QCOMPARE(layoutChannels("5.1"), 6);
        QCOMPARE(layoutChannels("7.1"), 8);
        QCOMPARE(layoutChannels("quad"), 2);  // unknown: stereo
        auto power = [](const std::vector<float>& g) {
            double p = 0;
            for (float v : g) p += double(v) * v;
            return p;
        };
        for (const char* layout : {"stereo", "5.1", "7.1"}) {
            const auto& sp = layoutSpeakers(layout);
            // A sound at a speaker's angle comes out of that speaker alone.
            for (size_t i = 0; i < sp.size(); ++i) {
                if (sp[i].lfe) continue;
                const auto g = panGains(layout, sp[i].angle, 1);
                for (size_t j = 0; j < g.size(); ++j) QVERIFY2(std::fabs(g[j] - (i == j ? 1.0f : 0.0f)) < 1e-6, layout);
            }
            // Anywhere, at any distance: constant power, and nothing to the LFE.
            for (double a = -180; a <= 180; a += 7.5)
                for (double d : {0.0, 0.3, 1.0}) {
                    const auto g = panGains(layout, a, d);
                    QVERIFY(std::fabs(power(g) - 1) < 1e-5);
                    for (size_t j = 0; j < g.size(); ++j)
                        if (sp[j].lfe) QCOMPARE(g[j], 0.0f);
                }
        }
        // 5.1: L R C LFE Ls Rs. The default panner puts a stereo track on L and R, as in stereo.
        SurroundPan p;
        auto g = surroundGains("5.1", p);
        QVERIFY(std::fabs(g.left[0] - 1) < 1e-6 && std::fabs(g.right[1] - 1) < 1e-6 && g.lfe == 0);
        // Narrowed to a point: the centre, each channel at -3 dB (the same sound in both stays as loud).
        p.width = 0;
        g = surroundGains("5.1", p);
        QVERIFY(std::fabs(g.left[2] - M_SQRT1_2) < 1e-6 && std::fabs(g.right[2] - M_SQRT1_2) < 1e-6);
        // Hard right (90 degrees): between R (30) and Rs (110), three quarters of the way to Rs.
        p.x = 1, p.y = 0;
        g = surroundGains("5.1", p);
        QVERIFY(std::fabs(g.left[1] - M_SQRT1_2 * std::cos(0.75 * M_PI / 2)) < 1e-5 && std::fabs(g.left[5] - M_SQRT1_2 * std::sin(0.75 * M_PI / 2)) < 1e-5);
        // The middle of the room: evenly over the five main speakers.
        p.x = 0, p.y = 0;
        g = surroundGains("5.1", p);
        for (int c : {0, 1, 2, 4, 5}) QVERIFY(std::fabs(g.left[size_t(c)] - M_SQRT1_2 / std::sqrt(5.0)) < 1e-5);
        p.lfeDb = -6;
        QVERIFY(std::fabs(surroundGains("5.1", p).lfe - std::pow(10.0, -6.0 / 20)) < 1e-6);
        // The fold-down: centre and surrounds at -3 dB into their side, no LFE.
        const float frame[6] = {0.1f, 0.2f, 0.4f, 0.9f, 0.3f, 0.5f};
        float lr[2];
        downmixToStereo("5.1", frame, 1, lr);
        QVERIFY(std::fabs(lr[0] - (0.1f + 0.7071f * 0.4f + 0.7071f * 0.3f)) < 1e-6);
        QVERIFY(std::fabs(lr[1] - (0.2f + 0.7071f * 0.4f + 0.7071f * 0.5f)) < 1e-6);
        // Saved with the sequence and its tracks and buses.
        Fixture fx;
        fx.s().audioLayout = "7.1";
        fx.s().audioTracks[0].surround = {0.5, -0.5, 0.25, -9};
        Bus b;
        b.id = fx.p.newId();
        b.surround.x = -1;
        fx.s().buses.push_back(b);
        Project back;
        QVERIFY(projectFromJson(projectToJson(fx.p), back));
        QCOMPARE(back.active()->audioLayout, std::string("7.1"));
        QCOMPARE(back.active()->audioTracks[0].surround, fx.s().audioTracks[0].surround);
        QCOMPARE(back.active()->buses[0].surround.x, -1.0);
        QCOMPARE(back.active()->audioTracks[1].surround, SurroundPan{});
    }

    void bezierKeyframes() {
        // Automatic handles on two keys are flat at the ends: an S-curve through the middle.
        Param p;
        p.addKey(0, 0, Interp::Bezier);
        p.addKey(30, 100);
        QVERIFY(std::fabs(p.at(15) - 50) < 1e-6);
        QVERIFY(p.at(5) < 100.0 * 5 / 30 - 5);
        QVERIFY(p.at(25) > 100.0 * 25 / 30 + 5);
        for (FrameTime t = 1; t < 30; ++t) QVERIFY(p.at(t) > p.at(t - 1));  // no overshoot, always rising
        // Handles along the straight line give the straight line.
        p.keys[0].outDt = 10, p.keys[0].outDv = 100.0 / 3;
        p.keys[1].inDt = -10, p.keys[1].inDv = -100.0 / 3;
        for (FrameTime t : {3, 7, 18, 29}) QVERIFY(std::fabs(p.at(t) - 100.0 * double(t) / 30) < 1e-6);
        // Handles reaching past the segment are held inside it (the curve stays a function of time).
        p.keys[0].outDt = 500, p.keys[0].outDv = 0;
        QVERIFY(p.at(29) <= 100 + 1e-9 && p.at(1) >= 0);
        // Three keys in a row: the middle one is passed straight through; at a peak it is flat.
        Param line;
        for (FrameTime t : {0, 10, 20}) line.addKey(t, double(t), Interp::Bezier);
        QVERIFY(std::fabs((line.at(11) - line.at(9)) / 2 - 1) < 0.1);          // about the line's slope at the middle key
        QVERIFY(std::fabs(line.at(5) + line.at(15) - 20) < 1e-6);             // and the curve symmetric about it
        Param peak;
        peak.addKey(0, 0, Interp::Bezier);
        peak.addKey(10, 10, Interp::Bezier);
        peak.addKey(20, 0);
        QCOMPARE(peak.at(10), 10.0);
        QVERIFY(peak.at(9) < 10 && peak.at(9) > 9.5 && std::fabs(peak.at(9) - peak.at(11)) < 1e-9);

        // Ease In on the end of a straight move: it leaves as before and arrives slowly.
        Param move;
        move.addKey(0, 0);
        move.addKey(30, 90);
        QVERIFY(easeKey(move, 30, true, false));
        QCOMPARE(move.keys[0].interp, Interp::Bezier);
        QVERIFY(std::fabs(move.at(1) - 3) < 0.6);    // the straight start (3 a frame)
        QVERIFY(90 - move.at(29) < 1.0);             // flat at the end
        QVERIFY(!easeKey(move, 12, true, true));     // no key there
        // Ease Out of the first key of a smooth move: both ends flat now.
        Param smooth;
        smooth.addKey(0, 0, Interp::Smooth);
        smooth.addKey(30, 90);
        QVERIFY(easeKey(smooth, 0, false, true));
        QVERIFY(std::fabs(smooth.at(15) - 45) < 1e-6);
        QVERIFY(smooth.at(1) < 1.0 && 90 - smooth.at(29) < 1.0);

        // Dragging a handle: linked, the other side takes the same slope (keeping its length).
        Param three;
        for (FrameTime t : {0, 15, 30}) three.addKey(t, 0, Interp::Bezier);
        QVERIFY(setKeyHandle(three, 15, true, 5, 10, true));
        QCOMPARE(three.keys[1].outDt, 5.0);
        QCOMPARE(three.keys[1].outDv, 10.0);
        QCOMPARE(three.keys[1].inDt, -5.0);
        QCOMPARE(three.keys[1].inDv, -10.0);
        QVERIFY(three.at(20) > 0 && three.at(10) < 0);
        QVERIFY(setKeyHandle(three, 15, false, -100, 3, false));  // held within the segment, the other side kept
        QCOMPARE(three.keys[1].inDt, -15.0);
        QCOMPARE(three.keys[1].outDv, 10.0);
        QVERIFY(!setKeyHandle(three, 0, false, -1, 0, false));    // the first key has no incoming segment

        // Saved and read back with the handles; older files without them still read.
        Fixture fx;
        const Id clip = fx.put(V1, 0, 40);
        edit::clipById(fx.s(), clip)->motion.params["opacity"] = three;
        Project back;
        QVERIFY(projectFromJson(projectToJson(fx.p), back));
        QCOMPARE(edit::clipById(*back.active(), clip)->motion.params.at("opacity"), three);
        Clip old;
        QVERIFY(clipFromJsonString(R"({"id":5,"name":"x","start":0,"duration":10,"motion":{"type":"motion","params":{"opacity":{"value":1,"keys":[[0,1,"smooth"],[9,0]]}}}})", old));
        QCOMPARE(old.motion.params.at("opacity").keys[0].interp, Interp::Smooth);
        QCOMPARE(old.motion.params.at("opacity").keys[0].outDt, 0.0);
    }

    void scriptCut() {
        // A screenplay: cues above the words, "Name:" lines, and things to skip.
        const std::string script = "INT. KITCHEN - DAY\n\nJOHN\nI never thought we would make it this far.\n\n"
                                   "MARY (V.O.)\n(quietly)\nNeither did I, but here we are.\n\nCUT TO:\n\n"
                                   "Narrator: The end of a long road.\n\n[Music swells]\n\nSomething nobody said at all.\n";
        const auto lines = parseScript(script);
        QCOMPARE(lines.size(), size_t(4));
        QCOMPARE(lines[0], (ScriptLine{"John", "I never thought we would make it this far."}));
        QCOMPARE(lines[1], (ScriptLine{"Mary", "Neither did I, but here we are."}));
        QCOMPARE(lines[2], (ScriptLine{"Narrator", "The end of a long road."}));
        QCOMPARE(lines[3].speaker, std::string());
        // A long paragraph is split at sentence ends.
        std::string para;
        for (int i = 0; i < 6; ++i) para += "This is sentence number " + std::to_string(i) + " of a long speech. ";
        const auto split = parseScript(para);
        QVERIFY(split.size() >= 2);
        for (const auto& l : split) QVERIFY(QString::fromStdString(l.text).endsWith('.'));

        // Two takes: the first flubs line one ("we we") and has line two; the second is clean
        // and has line three, with "road" heard as "roads".
        auto said = [](Transcript& t, double at, const std::string& text, int speaker = -1) {
            TranscriptSegment seg;
            seg.start = at;
            seg.text = text;
            seg.speaker = speaker;
            for (const QString& w : QString::fromStdString(text).split(' ')) {
                TranscriptWord word;
                word.start = at;
                word.end = at + 0.35;
                word.text = w.toStdString();
                seg.words.push_back(word);
                at += 0.4;
            }
            seg.end = at;
            t.segments.push_back(seg);
        };
        Fixture fx;
        Project& p = fx.p;
        Transcript t1, t2;
        said(t1, 1.0, "um I never thought we we would make it this far", 0);
        said(t1, 6.0, "neither did I but here we are", 0);
        said(t2, 2.0, "I never thought we would make it this far", 0);
        said(t2, 8.0, "the end of a long roads", 0);
        p.findMedia(fx.media)->transcript = std::make_shared<const Transcript>(t1);
        MediaItem m2 = *p.findMedia(fx.media);
        m2.id = p.newId();
        m2.name = "take2.mov";
        m2.duration = 20;
        m2.transcript = std::make_shared<const Transcript>(t2);
        p.media.push_back(m2);

        auto matches = matchScript(p, lines);
        QCOMPARE(matches.size(), size_t(4));
        QCOMPARE(matches[0].takes.size(), size_t(2));
        QCOMPARE(matches[0].takes[0].mediaId, m2.id);  // the clean reading
        QCOMPARE(matches[0].takes[0].extraWords, 0);
        QCOMPARE(matches[0].takes[0].start, 2.0);
        QVERIFY(std::fabs(matches[0].takes[0].end - (2.0 + 8 * 0.4 + 0.35)) < 1e-9);
        QCOMPARE(matches[0].takes[1].mediaId, fx.media);
        QCOMPARE(matches[0].takes[1].extraWords, 1);
        QCOMPARE(matches[0].takes[1].start, 1.4);  // after the "um"
        QCOMPARE(matches[1].takes.size(), size_t(1));
        QCOMPARE(matches[1].takes[0].mediaId, fx.media);
        QCOMPARE(matches[2].takes.size(), size_t(1));
        QCOMPARE(matches[2].takes[0].coverage, 1.0);  // a near miss still counts
        QVERIFY(matches[3].takes.empty());
        // Only the media asked for is searched.
        ScriptCutOptions only;
        only.media = {fx.media};
        QCOMPARE(matchScript(p, lines, only)[0].takes.size(), size_t(1));

        // The cut: the chosen readings back to back on V1/A1, the alternate disabled above.
        const auto res = buildScriptCut(p, matches, "Script Cut");
        QVERIFY(res.sequence);
        QCOMPARE(res.placed, 3);
        QCOMPARE(res.missing, 1);
        QCOMPARE(res.alternates, 1);
        const Sequence* s = p.findSequence(res.sequence);
        QVERIFY(s);
        QCOMPARE(s->name, std::string("Script Cut"));
        QCOMPARE(s->width, p.active()->width);
        const auto& v1 = s->videoTracks[0].clips;
        QCOMPARE(v1.size(), size_t(3));
        QCOMPARE(v1[0].mediaId, m2.id);
        QCOMPARE(v1[1].mediaId, fx.media);
        QCOMPARE(v1[2].mediaId, m2.id);
        QCOMPARE(v1[0].start, FrameTime(0));
        QVERIFY(std::fabs(v1[0].sourceIn - (2.0 - 0.15) * 30) < 1e-6);
        for (size_t i = 1; i < v1.size(); ++i) QCOMPARE(v1[i].start, v1[i - 1].start + v1[i - 1].duration);
        for (const Clip& c : v1) QVERIFY(c.enabled);
        QCOMPARE(s->audioTracks[0].clips.size(), size_t(3));
        QVERIFY(s->videoTracks.size() >= 2);
        QCOMPARE(s->videoTracks[1].clips.size(), size_t(1));
        QCOMPARE(s->videoTracks[1].clips[0].mediaId, fx.media);
        QVERIFY(!s->videoTracks[1].clips[0].enabled);
        QVERIFY(!s->audioTracks[1].clips[0].enabled);
        // A marker per line; the missing line marked where it would have gone.
        QCOMPARE(s->markers.size(), size_t(4));
        QCOMPARE(s->markers[0].name, std::string("John: I never thought we would make it this far."));
        QVERIFY(QString::fromStdString(s->markers[3].name).startsWith("Missing: "));
        QCOMPARE(s->markers[3].t, s->duration());
        QVERIFY(std::any_of(p.media.begin(), p.media.end(), [&](const MediaItem& m) { return m.sequenceId == res.sequence; }));

        // Named speakers: the script's John is the first take's speaker, so his reading wins
        // despite the flub.
        t1.speakerNames = {"John"};
        t2.speakerNames = {"Mary"};
        p.findMedia(fx.media)->transcript = std::make_shared<const Transcript>(t1);
        p.findMedia(m2.id)->transcript = std::make_shared<const Transcript>(t2);
        matches = matchScript(p, lines);
        QCOMPARE(matches[0].takes[0].mediaId, fx.media);
        QCOMPARE(matches[0].takes[0].speaker, std::string("John"));
        QCOMPARE(matches[2].takes.size(), size_t(1));  // still found, just not preferred
        // Nothing found: no sequence.
        const auto none = buildScriptCut(p, matchScript(p, {ScriptLine{"", "words nobody spoke"}}), "Empty");
        QCOMPARE(none.sequence, Id(0));
        QCOMPARE(none.missing, 1);
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

    void adjustmentLayersStayOutOfInterchange() {
        Fixture fx;
        fx.put(V1, 0, 60);
        Clip adj = makeGeneratorClip(fx.p, "adjustment", 30);
        adj.start = 10;
        QVERIFY(overwrite(fx.p, fx.s(), V2, adj).ok);
        Clip after = makeGeneratorClip(fx.p, "color", 20);
        after.start = 40;
        QVERIFY(overwrite(fx.p, fx.s(), V2, after).ok);
        QVERIFY(addTransition(fx.p, fx.s(), adj.id, Edge::Out, "cross_dissolve", 4).ok);
        const Sequence out = interchangeSequence(fx.s());
        QCOMPARE(out.videoTracks[1].clips.size(), size_t(1));
        QCOMPARE(out.videoTracks[1].clips[0].generator.type, std::string("color"));
        QVERIFY(out.videoTracks[1].transitions.empty());
        QCOMPARE(out.videoTracks[0].clips.size(), size_t(1));
        for (const std::string& xml : {exportFcp7Xml(fx.p, fx.s()), exportFcpXml(fx.p, fx.s()), exportOtio(fx.p, fx.s())})
            QVERIFY(QString::fromStdString(xml).indexOf("Adjustment") < 0);
    }

    void editingStaples() {
        // Q and W: linked clips a [0,90) and b [90,180), playhead at 30 and then 120.
        {
            Fixture fx;
            QVERIFY(placeMedia(fx.p, fx.s(), fx.media, 0, 0, 90, V1, A1, false).ok);
            QVERIFY(placeMedia(fx.p, fx.s(), fx.media, 90, 100, 190, V1, A1, false).ok);
            QCOMPARE(previousClipEdge(fx.s(), 30), FrameTime(0));
            QCOMPARE(nextClipEdge(fx.s(), 30), FrameTime(90));
            QCOMPARE(previousClipEdge(fx.s(), 0), FrameTime(-1));
            // Q at 30: frames 0-29 go from every track; the first clip now starts 30 frames into its source.
            Result r = rippleTrimToPlayhead(fx.p, fx.s(), 30, true);
            QVERIFY(r.ok);
            QCOMPARE(r.applied, FrameTime(30));
            QCOMPARE(fx.s().duration(), FrameTime(150));
            QCOMPARE(fx.v1().clips[0].sourceIn, 30.0);
            QCOMPARE(fx.a1().clips[0].sourceIn, 30.0);
            // W at 90 (30 into b): b's first 30 frames stay, the rest of b up to its end goes.
            r = rippleTrimToPlayhead(fx.p, fx.s(), 90, false);
            QVERIFY(r.ok);
            QCOMPARE(fx.s().duration(), FrameTime(90));
            QVERIFY(!rippleTrimToPlayhead(fx.p, fx.s(), 90, false).ok);  // nothing after the end
        }
        // Paste and Remove Attributes.
        {
            Fixture fx;
            QVERIFY(placeMedia(fx.p, fx.s(), fx.media, 0, 0, 30, V1, A1, false).ok);
            QVERIFY(placeMedia(fx.p, fx.s(), fx.media, 30, 0, 30, V1, A1, false).ok);
            Clip& src = fx.v1().clips[0];
            src.motion.params["scale"] = Param(150.0);
            src.motion.params["pos_x"].addKey(0, 0, Interp::Linear);
            src.motion.params["pos_x"].addKey(20, 100, Interp::Linear);
            src.motion.params["opacity"] = Param(40.0);
            src.blendMode = "screen";
            src.effects.push_back(makeEffect(fx.p, "gaussian_blur"));
            fx.a1().clips[0].audio.params["gain_db"] = Param(-6.0);
            const Clip from = src, fromAudio = fx.a1().clips[0];
            const Id v2 = fx.v1().clips[1].id, a2 = fx.a1().clips[1].id;
            // Motion only: scale and the position keys, not opacity.
            QVERIFY(pasteAttributes(fx.p, fx.s(), from, TrackKind::Video, {v2, a2}, AttrMotion).ok);
            const Clip* t = clipById(fx.s(), v2);
            QCOMPARE(t->motion.p("scale", 0), 150.0);
            QCOMPARE(t->motion.params.at("pos_x").keys.size(), size_t(2));
            QCOMPARE(t->motion.p("opacity", 0, 100), 100.0);
            QVERIFY(t->effects.empty());
            QVERIFY(t->motion.id != from.motion.id);
            // Opacity and effects; effects get fresh ids.
            QVERIFY(pasteAttributes(fx.p, fx.s(), from, TrackKind::Video, {v2}, AttrOpacity | AttrEffects).ok);
            t = clipById(fx.s(), v2);
            QCOMPARE(t->motion.p("opacity", 0, 100), 40.0);
            QCOMPARE(t->blendMode, std::string("screen"));
            QCOMPARE(t->effects.size(), size_t(1));
            QVERIFY(t->effects[0].id != from.effects[0].id);
            // Volume onto the audio clip only.
            QVERIFY(pasteAttributes(fx.p, fx.s(), fromAudio, TrackKind::Audio, {v2, a2}, AttrVolume).ok);
            QCOMPARE(clipById(fx.s(), a2)->audio.p("gain_db", 0), -6.0);
            QVERIFY(!pasteAttributes(fx.p, fx.s(), from, TrackKind::Video, {a2}, AttrMotion).ok);  // nothing applies
            // Remove: effects and motion back to defaults, opacity kept.
            QVERIFY(removeAttributes(fx.p, fx.s(), {v2}, AttrMotion | AttrEffects).ok);
            t = clipById(fx.s(), v2);
            QCOMPARE(t->motion.p("scale", 0, 100), 100.0);
            QVERIFY(!t->motion.params.at("pos_x").animated());
            QCOMPARE(t->motion.p("opacity", 0, 100), 40.0);
            QVERIFY(t->effects.empty());
            QVERIFY(removeAttributes(fx.p, fx.s(), {v2}, AttrOpacity).ok);
            QCOMPARE(clipById(fx.s(), v2)->blendMode, std::string("normal"));
        }
        // Frame Hold: the rest of the clip shows the frame at the split.
        {
            Fixture fx;
            QVERIFY(placeMedia(fx.p, fx.s(), fx.media, 0, 60, 120, V1, A1, false).ok);
            const Id v = fx.v1().clips[0].id;
            Result r = addFrameHold(fx.p, fx.s(), v, 20);
            QVERIFY2(r.ok, r.error.c_str());
            QCOMPARE(fx.v1().clips.size(), size_t(2));
            const Clip& held = fx.v1().clips[1];
            QCOMPARE(held.id, r.created.at(0));
            QCOMPARE(held.start, FrameTime(20));
            QCOMPARE(held.sourceFrameAt(20), 80.0);
            QCOMPARE(held.sourceFrameAt(55), 80.0);
            QCOMPARE(fx.a1().clips.size(), size_t(1));  // the sound plays on
            // A held clip can be trimmed out as far as wanted, and slipped frame by frame.
            QVERIFY(trim(fx.p, fx.s(), held.id, Edge::Out, 400, TrimMode::Ripple, false).ok);
            QCOMPARE(clipById(fx.s(), held.id)->duration, FrameTime(440));
            QVERIFY(slip(fx.p, fx.s(), held.id, 5).ok);
            QCOMPARE(clipById(fx.s(), held.id)->sourceFrameAt(30), 85.0);
            QVERIFY(!addFrameHold(fx.p, fx.s(), fx.a1().clips[0].id, 10).ok);  // not sound
        }
        // Replace Edit and Fit to Fill.
        {
            Fixture fx;
            MediaItem other = *fx.p.findMedia(fx.media);
            other.id = fx.p.newId();
            other.name = "other.mov";
            other.path = "/nonexistent/other.mov";
            fx.p.media.push_back(other);
            QVERIFY(placeMedia(fx.p, fx.s(), fx.media, 30, 0, 60, V1, A1, false).ok);
            Clip& c = fx.v1().clips[0];
            c.effects.push_back(makeEffect(fx.p, "invert"));
            c.motion.params["scale"] = Param(120.0);
            // Source frame 100 lands on timeline frame 45 (15 frames into the clip).
            Result r = replaceClip(fx.p, fx.s(), c.id, other.id, 100, 45);
            QVERIFY2(r.ok, r.error.c_str());
            QCOMPARE(r.created.size(), size_t(2));  // picture and sound
            const Clip& v = fx.v1().clips[0];
            QCOMPARE(v.mediaId, other.id);
            QCOMPARE(v.sourceIn, 85.0);
            QCOMPARE(v.start, FrameTime(30));
            QCOMPARE(v.duration, FrameTime(60));
            QCOMPARE(v.effects.size(), size_t(1));
            QCOMPARE(v.motion.p("scale", 0), 120.0);
            QCOMPARE(fx.a1().clips[0].mediaId, other.id);
            QVERIFY(!replaceClip(fx.p, fx.s(), v.id, other.id, 5, 45).ok);  // does not reach back to the start
            // Fit to Fill: 90 source frames into 45 timeline frames: double speed.
            r = fitToFill(fx.p, fx.s(), fx.media, 0, 89, 100, 144, V1, A1);
            QVERIFY2(r.ok, r.error.c_str());
            const Clip* f = clipAt(fx.s(), V1, 120);
            QVERIFY(f && f->start == 100 && f->duration == 45);
            QCOMPARE(f->speed, 2.0);
            QCOMPARE(f->sourceFrameAt(144), 88.0);
            // Slower: 15 source frames over 60 timeline frames, over what was there.
            r = fitToFill(fx.p, fx.s(), other.id, 0, 14, 40, 99, V1, A1);
            QVERIFY(r.ok);
            f = clipAt(fx.s(), V1, 50);
            QVERIFY(f && f->mediaId == other.id && f->start == 40 && f->duration == 60);
            QCOMPARE(f->speed, 0.25);
            QVERIFY(!fitToFill(fx.p, fx.s(), other.id, 10, 5, 0, 10, V1, A1).ok);
        }
        // Track Select Forward.
        {
            Fixture fx;
            QVERIFY(placeMedia(fx.p, fx.s(), fx.media, 0, 0, 30, V1, A1, false).ok);
            QVERIFY(placeMedia(fx.p, fx.s(), fx.media, 50, 0, 30, V1, A1, false).ok);
            fx.put(V2, 60, 10);
            QCOMPARE(clipsFrom(fx.s(), 40).size(), size_t(3));
            QCOMPARE(clipsFrom(fx.s(), 40, V1).size(), size_t(1));
            QCOMPARE(clipsFrom(fx.s(), 0).size(), size_t(5));
        }
    }

    void duplicateSequences() {
        Fixture fx;
        QVERIFY(placeMedia(fx.p, fx.s(), fx.media, 0, 30, 90, V1, A1, false).ok);  // linked picture and sound
        Id a = fx.s().videoTracks[0].clips.at(0).id;
        fx.put(V1, 60, 60, 120);
        addTransition(fx.p, fx.s(), a, Edge::Out, "cross_dissolve", 10);
        const Sequence& src = fx.s();
        const size_t mediaBefore = fx.p.media.size();
        std::map<Id, Id> ids;
        const Id copy = duplicateSequence(fx.p, src.id, {}, &ids);
        QVERIFY(copy && copy != fx.p.active()->id);
        const Sequence& orig = *fx.p.active();
        const Sequence& dup = *fx.p.findSequence(copy);
        QCOMPARE(dup.name, orig.name + " Copy");
        QCOMPARE(fx.p.media.size(), mediaBefore + 1);  // its bin item
        QCOMPARE(fx.p.media.back().sequenceId, copy);
        // Same timeline, fresh ids, references remapped.
        QCOMPARE(dup.videoTracks[0].clips.size(), orig.videoTracks[0].clips.size());
        QCOMPARE(dup.duration(), orig.duration());
        for (size_t i = 0; i < orig.videoTracks[0].clips.size(); ++i) {
            const Clip& o = orig.videoTracks[0].clips[i];
            const Clip& d = dup.videoTracks[0].clips[i];
            QVERIFY(d.id != o.id && ids.at(o.id) == d.id);
            QVERIFY(d.motion.id != o.motion.id);
            QCOMPARE(d.start, o.start);
        }
        const Transition& tr = dup.videoTracks[0].transitions.at(0);
        QCOMPARE(tr.clipA, ids.at(orig.videoTracks[0].transitions[0].clipA));
        QCOMPARE(tr.clipB, ids.at(orig.videoTracks[0].transitions[0].clipB));
        // Linked picture and sound stay linked to each other, not to the original's.
        const Clip& dv = dup.videoTracks[0].clips[0];
        const Clip& da = dup.audioTracks[0].clips[0];
        QVERIFY(dv.linkGroup != 0 && dv.linkGroup == da.linkGroup && dv.linkGroup != orig.videoTracks[0].clips[0].linkGroup);
        QCOMPARE(duplicateSequence(fx.p, 987654), Id(0));
    }

    void spokenSlates() {
        // A transcript of words said one after another, a third of a second each, from `at`.
        auto said = [](const char* text, double at = 0.5) {
            Transcript t;
            TranscriptSegment seg;
            double time = at;
            for (const QString& w : QString(text).split(' ', Qt::SkipEmptyParts)) {
                seg.words.push_back({time, time + 0.3, w.toStdString()});
                time += 0.35;
            }
            seg.start = at, seg.end = time;
            t.segments.push_back(seg);
            return t;
        };
        auto check = [&](const char* text, const char* scene, const char* shot, const char* take) {
            const auto s = slateFromTranscript(said(text));
            QVERIFY2(s, text);
            QCOMPARE(QString::fromStdString(s->scene), QString(scene));
            QCOMPARE(QString::fromStdString(s->shot), QString(shot));
            QCOMPARE(QString::fromStdString(s->take), QString(take));
        };
        check("Scene twelve apple, take three.", "12", "A", "3");
        check("Slate 42, take 1.", "42", "", "1");
        check("12B take 2", "12", "B", "2");
        check("Scene one hundred and four bravo, take twenty one.", "104", "B", "21");
        check("Shot C, take 4.", "", "C", "4");
        check("Okay. Scene 7 Charlie. Take 11. Action!", "7", "C", "11");
        check("scene thirty-five take two", "35", "", "2");
        // When it was said.
        QCOMPARE(slateFromTranscript(said("Rolling. Scene 3 take 1", 1.0))->at, 1.35);
        // No slate: ordinary speech, "take" without a number, or a slate called too late.
        QVERIFY(!slateFromTranscript(said("Hello and welcome to the show.")));
        QVERIFY(!slateFromTranscript(said("Let's take a break and come back.")));
        QVERIFY(!slateFromTranscript(said("Scene 4 take 2", 30.0)));
        QVERIFY(slateFromTranscript(said("Scene 4 take 2", 30.0), 40.0));
    }

    void trackAutomation() {
        // Playing a lane: Read follows it (between whole frames too); Off and Write use the fader.
        Track t;
        t.kind = TrackKind::Audio;
        t.volumeDb = -6;
        t.volumeAuto.addKey(0, -60);
        t.volumeAuto.addKey(30, 0);
        QCOMPARE(trackVolumeAt(t, 15), -30.0);
        QCOMPARE(trackVolumeAt(t, 15.5), -29.0);
        QCOMPARE(trackPanAt(t, 15), 0.0);
        t.automation = int(AutomationMode::Off);
        QCOMPARE(trackVolumeAt(t, 15), -6.0);
        t.automation = int(AutomationMode::Write);
        QCOMPARE(trackVolumeAt(t, 15), -6.0);
        t.automation = int(AutomationMode::Touch);
        QCOMPARE(trackVolumeAt(t, 15), -30.0);
        // Thinning keeps only the turns.
        std::vector<Keyframe> ramp;
        for (int i = 0; i <= 100; ++i) ramp.push_back({i, i <= 50 ? i * 0.1 : 10 - i * 0.1});
        thinKeys(ramp, 0.01);
        QCOMPARE(ramp.size(), size_t(3));
        QCOMPARE(ramp[1].t, FrameTime(50));

        // Write: from play to stop, with the level either side kept.
        {
            AutomationRecorder r(AutomationMode::Write, Param(), -6, 10);
            for (FrameTime f = 10; f <= 40; ++f) QCOMPARE(r.tick(f, f < 20 ? -6.0 : -12.0, false), f < 20 ? -6.0 : -12.0);
            QVERIFY(r.writing());
            Param lane = r.finish(40, 0.05);
            QVERIFY(lane.animated());
            QCOMPARE(lane.at(5), -6.0);
            QCOMPARE(lane.at(15), -6.0);
            QCOMPARE(lane.at(30), -12.0);
            QCOMPARE(lane.at(40), -12.0);
            QCOMPARE(lane.at(41), -6.0);
            QVERIFY(lane.keys.size() <= 6);  // thinned
        }
        // An untouched pan written flat adds no lane.
        {
            AutomationRecorder r(AutomationMode::Write, Param(), 0.25, 0);
            for (FrameTime f = 0; f < 20; ++f) r.tick(f, 0.25, false);
            const Param lane = r.finish(20, 0.005);
            QVERIFY(!lane.animated());
            QCOMPARE(lane.value, 0.25);
        }
        Param flat;
        flat.addKey(0, -10);
        flat.addKey(100, -10);
        // Latch: plays the lane until the fader moves, then writes to the stop.
        {
            AutomationRecorder r(AutomationMode::Latch, flat, 0, 0);
            for (FrameTime f = 0; f <= 60; ++f) {
                const bool held = f >= 20 && f <= 30;
                const double shown = r.tick(f, f < 20 ? 0.0 : -3.0, held);
                if (f < 20) QCOMPARE(shown, -10.0);
                else QCOMPARE(shown, -3.0);  // latched after letting go
            }
            const Param lane = r.finish(60, 0.05);
            QCOMPARE(lane.at(10), -10.0);
            QCOMPARE(lane.at(25), -3.0);
            QCOMPARE(lane.at(55), -3.0);
            QCOMPARE(lane.at(61), -10.0);
            QVERIFY(lane.keyAt(100));
        }
        // Touch: writes while held, then glides back to the lane over the glide time.
        {
            AutomationRecorder r(AutomationMode::Touch, flat, 0, 0, 10);
            for (FrameTime f = 0; f <= 60; ++f) {
                const bool held = f >= 20 && f <= 30;
                const double shown = r.tick(f, held ? -3.0 : 0.0, held);
                QCOMPARE(shown, held ? -3.0 : -10.0);
                QCOMPARE(r.writing(), held);
            }
            const Param lane = r.finish(60, 0.05);
            QCOMPARE(lane.at(19), -10.0);
            QCOMPARE(lane.at(25), -3.0);
            QCOMPARE(lane.at(30), -3.0);
            QVERIFY(lane.at(35) < -3.0 && lane.at(35) > -10.0);
            QCOMPARE(lane.at(40), -10.0);
            QCOMPARE(lane.at(60), -10.0);
        }
        // Read never writes.
        {
            AutomationRecorder r(AutomationMode::Read, flat, 0, 0);
            for (FrameTime f = 0; f < 30; ++f) QCOMPARE(r.tick(f, 5.0, true), -10.0);
            QVERIFY(!r.wrote());
            QCOMPARE(r.finish(30, 0.05), flat);
        }
        // Saved with the project.
        Project p = makeDefaultProject();
        Track& a1 = p.active()->audioTracks.at(0);
        a1.volumeAuto = flat;
        a1.panAuto.addKey(5, -0.5);
        a1.panAuto.addKey(10, 0.5);
        a1.automation = int(AutomationMode::Latch);
        Project back;
        QVERIFY(projectFromJson(projectToJson(p), back));
        QCOMPARE(back.active()->audioTracks.at(0), a1);
        QCOMPARE(back.active()->audioTracks.at(1).automation, int(AutomationMode::Read));
    }

    void clipMarkers() {
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.fps = Rational{30, 1};
        Clip c = makeGeneratorClip(p, "color", 100);
        c.start = 50;
        edit::overwrite(p, s, {TrackKind::Video, 0}, c);
        const Id id = s.videoTracks[0].clips.front().id;
        auto clip = [&]() -> Clip& { return *edit::clipById(s, id); };
        // On the moment of the source shown at frame 80: 30 frames into the clip.
        QVERIFY(edit::addClipMarker(s, id, 80, Marker{0, 0, "Look", "", 0}));
        QVERIFY(!edit::addClipMarker(s, id, 10, Marker{}));  // not on the clip
        QCOMPARE(clip().markers.size(), size_t(1));
        QCOMPARE(clip().markers[0].t, FrameTime(30));
        QCOMPARE(clip().markerFrame(clip().markers[0]), FrameTime(80));
        // It travels with the clip, stays on its moment through a head trim, and hides when trimmed off.
        clip().start = 200;
        QCOMPARE(clip().markerFrame(clip().markers[0]), FrameTime(230));
        clip().sourceIn = 10, clip().start = 210, clip().duration = 90;
        QCOMPARE(clip().markerFrame(clip().markers[0]), FrameTime(230));
        clip().sourceIn = 40, clip().start = 240, clip().duration = 60;
        QCOMPARE(clip().markerFrame(clip().markers[0]), FrameTime(-1));
        // Speed and reverse.
        clip().sourceIn = 0, clip().start = 0, clip().duration = 50, clip().speed = 2;
        QCOMPARE(clip().markerFrame(clip().markers[0]), FrameTime(15));
        clip().speed = 1, clip().duration = 100, clip().reverse = true;
        QCOMPARE(clip().markerFrame(clip().markers[0]), FrameTime(69));
        clip().reverse = false;
        // A split: both halves keep it, only the half showing its moment shows it.
        QVERIFY(edit::razor(p, s, {TrackKind::Video, 0}, 20).ok);
        const auto& halves = s.videoTracks[0].clips;
        QCOMPARE(halves.size(), size_t(2));
        QCOMPARE(halves[0].markerFrame(halves[0].markers.at(0)), FrameTime(-1));
        QCOMPARE(halves[1].markerFrame(halves[1].markers.at(0)), FrameTime(30));
        // Removed where it shows; a second one at the same moment replaces the first.
        const Id second = halves[1].id;
        QVERIFY(edit::addClipMarker(s, second, 30, Marker{0, 0, "Again", "", 3}));
        QCOMPARE(edit::clipById(s, second)->markers.size(), size_t(1));
        QCOMPARE(QString::fromStdString(edit::clipById(s, second)->markers[0].name), QString("Again"));
        QVERIFY(edit::addClipMarker(s, second, 40, Marker{0, 5, "Chorus", "a note", 0, true}));
        QVERIFY(!edit::removeClipMarkerAt(s, second, 41));
        // Saved with the project.
        Project back;
        QVERIFY(projectFromJson(projectToJson(p), back));
        QCOMPARE(back.active()->videoTracks[0].clips[1].markers, edit::clipById(s, second)->markers);
        // OpenTimelineIO keeps them on the clip (and chapter markers as chapters).
        s.markers.push_back(Marker{5, 0, "Part 1", "", 0, true});
        Project otio = makeDefaultProject();
        const ImportResult r = importOtio(otio, exportOtio(p, s));
        QVERIFY2(r.ok, r.error.c_str());
        const Sequence& imported = *otio.active();
        QCOMPARE(imported.videoTracks.at(0).clips.at(1).markers, edit::clipById(s, second)->markers);
        QVERIFY(imported.markers.at(0).chapter);
        // FCPXML: clip markers inside their clip, chapter markers as chapter-marker, read back as chapters.
        const std::string fcpx = exportFcpXml(p, s);
        QVERIFY(fcpx.find("<chapter-marker") != std::string::npos);
        QVERIFY(fcpx.find("value=\"Again\"") != std::string::npos);
        Project fromXml = makeDefaultProject();
        const ImportResult rx = importXmlTimeline(fromXml, fcpx);
        QVERIFY2(rx.ok, rx.error.c_str());
        const auto& xm = fromXml.active()->markers;
        QVERIFY(std::any_of(xm.begin(), xm.end(), [](const Marker& m) { return m.chapter && m.name == "Part 1" && m.t == 5; }));
        QVERIFY(edit::removeClipMarkerAt(s, second, 40));
        QCOMPARE(edit::clipById(s, second)->markers.size(), size_t(1));
    }

    void markerLists() {
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.fps = Rational{25, 1};
        edit::overwrite(p, s, {TrackKind::Video, 0}, makeGeneratorClip(p, "color", 25 * 60));
        s.markers = {Marker{50, 25, "Intro, part 1", "Say \"hi\"", 11, false}, Marker{500, 0, "Chapter two", "", 0, true}};
        // CSV with Premiere's columns, quoted where needed, and read back exactly.
        const std::string csv = markersToCsv(s);
        QVERIFY(csv.rfind("Marker Name,Description,In,Out,Duration,Marker Type,Color\n", 0) == 0);
        QVERIFY(csv.find("\"Intro, part 1\",\"Say \"\"hi\"\"\",00:00:02:00,00:00:03:00,00:00:01:00,Comment,Red") != std::string::npos);
        QVERIFY(csv.find("Chapter two,,00:00:20:00,00:00:20:00,00:00:00:00,Chapter,") != std::string::npos);
        std::vector<Marker> back;
        std::string err;
        QVERIFY2(parseMarkerList(csv, s, back, &err), err.c_str());
        QCOMPARE(back, s.markers);
        // Avid locators, and back (name and colour; the comment joins the name).
        const std::string avid = markersToAvidLocators(s);
        QVERIFY(avid.find("Montage\t00:00:02:00\tV1\tred\tIntro, part 1: Say \"hi\"\t1\n") != std::string::npos);
        QVERIFY(parseMarkerList(avid, s, back, &err));
        QCOMPARE(back.size(), size_t(2));
        QCOMPARE(back[0].t, FrameTime(50));
        QCOMPARE(back[0].color, 11);
        QCOMPARE(QString::fromStdString(back[1].name), QString("Chapter two"));
        // A marker EDL Resolve reads.
        const std::string edl = markersToResolveEdl(s);
        QVERIFY(edl.find("001  001      V     C        00:00:02:00 00:00:02:01 00:00:02:00 00:00:02:01") != std::string::npos);
        QVERIFY(edl.find(" |C:ResolveColorRed |M:Intro, part 1 |D:25") != std::string::npos);
        // A review tool's notes, on a timeline that starts at 01:00:00:00, with a byte-order mark.
        QVERIFY(parseMarkerList("\xEF\xBB\xBFTimecode,Comment,Commenter\n01:00:04:00,Fix the colour,Sam\n01:00:10:12,\"Cut, here\",Ana\n", s, back, &err));
        QCOMPARE(back.size(), size_t(2));
        QCOMPARE(back[0].t, FrameTime(100));
        QCOMPARE(QString::fromStdString(back[0].comment), QString("Fix the colour"));
        QCOMPARE(back[1].t, FrameTime(262));
        QCOMPARE(QString::fromStdString(back[1].comment), QString("Cut, here"));
        // Nothing usable.
        QVERIFY(!parseMarkerList("", s, back, &err));
        QVERIFY(!parseMarkerList("Name,In\nA,soon\n", s, back, &err));
    }

    void chapterMarkers() {
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.fps = Rational{30, 1};
        const FrameTime sec = 30;
        edit::overwrite(p, s, {TrackKind::Video, 0}, makeGeneratorClip(p, "color", 4000 * sec));  // over an hour
        // Ordinary markers are not chapters.
        edit::addMarker(s, Marker{5 * sec, 0, "Note", "", 0});
        std::string warning;
        QVERIFY(youtubeChapters(s, 0, -1, &warning).empty());
        QVERIFY(!warning.empty());
        edit::addMarker(s, Marker{65 * sec, 0, "Setup", "", 0, true});
        edit::addMarker(s, Marker{20 * sec, 0, "Hook", "", 0, true});
        edit::addMarker(s, Marker{3725 * sec, 0, "", "", 0, true});
        // In time order, each running to the next, with an intro at 0:00 (YouTube needs one) and unnamed ones numbered.
        std::vector<Chapter> ch = chaptersOf(s);
        QCOMPARE(ch.size(), size_t(4));
        QCOMPARE(QString::fromStdString(ch[0].title), QString("Intro"));
        QCOMPARE(ch[0].end, 20 * sec);
        QCOMPARE(QString::fromStdString(ch[1].title), QString("Hook"));
        QCOMPARE(ch[2].end, 3725 * sec);
        QCOMPARE(QString::fromStdString(ch[3].title), QString("Chapter 4"));
        QCOMPARE(ch[3].end, s.duration());
        QCOMPARE(QString::fromStdString(youtubeChapters(s, 0, -1, &warning)), QString("0:00 Intro\n0:20 Hook\n1:05 Setup\n1:02:05 Chapter 4\n"));
        QVERIFY2(warning.empty(), warning.c_str());
        // A range: times from its start, chapters outside it left out, and no intro when one starts it.
        ch = chaptersOf(s, 20 * sec, 3725 * sec);
        QCOMPARE(ch.size(), size_t(2));
        QCOMPARE(ch[0].start, FrameTime(0));
        QCOMPARE(QString::fromStdString(ch[0].title), QString("Hook"));
        QCOMPARE(ch[1].start, 45 * sec);
        QCOMPARE(ch[1].end, 3705 * sec);
        // YouTube's rules: at least three chapters, each at least ten seconds.
        QCOMPARE(QString::fromStdString(youtubeChapters(s, 20 * sec, 3725 * sec, &warning)), QString("0:00 Hook\n0:45 Setup\n"));
        QVERIFY(QString::fromStdString(warning).contains("three"));
        edit::addMarker(s, Marker{70 * sec, 0, "Blip", "", 0, true});
        youtubeChapters(s, 0, -1, &warning);
        QVERIFY2(QString::fromStdString(warning).contains("Setup"), warning.c_str());
        // Saved with the project.
        Project back;
        QVERIFY(projectFromJson(projectToJson(p), back));
        QCOMPARE(back.active()->markers, s.markers);
        QCOMPARE(int(std::count_if(back.active()->markers.begin(), back.active()->markers.end(), [](const Marker& m) { return m.chapter; })), 4);
    }

    void throughEditsJoin() {
        // A clip with its sound, cut in two: the cut is a through edit on both tracks.
        Fixture fx;
        QVERIFY(placeMedia(fx.p, fx.s(), fx.media, 0, 10, 110, V1, A1, false).ok);
        const Id v = fx.v1().clips[0].id, a = fx.a1().clips[0].id;
        clipById(fx.s(), v)->markers.push_back(Marker{70, 0, "late", "", 0, false});
        QVERIFY(razorAll(fx.p, fx.s(), 40).ok);
        QCOMPARE(fx.v1().clips.size(), size_t(2));
        QCOMPARE(throughEdits(fx.s()), (std::vector<Id>{v, a}));
        // Moved a frame, cut to other material, at another speed or with a dissolve: not through any more.
        {
            Fixture g = fx;
            g.v1().clips[1].start += 1;  // a frame later, with a gap
            QCOMPARE(throughEdits(g.s()), std::vector<Id>{a});
        }
        {
            Fixture g = fx;
            g.v1().clips[1].sourceIn += 3;
            g.v1().clips[1].speed = 1.0;
            QCOMPARE(throughEdits(g.s()), std::vector<Id>{a});
            g.v1().clips[1].sourceIn -= 3;
            g.v1().clips[1].speed = 2.0;
            QCOMPARE(throughEdits(g.s()), std::vector<Id>{a});
        }
        {
            Fixture g = fx;
            QVERIFY(addTransition(g.p, g.s(), v, Edge::Out, "cross_dissolve", 10).ok);
            QCOMPARE(throughEdits(g.s()), std::vector<Id>{a});
        }
        // Joined: one clip on each track again, as before the cut, the marker kept.
        QVERIFY(!joinThroughEdit(fx.p, fx.s(), fx.v1().clips[1].id).ok);  // nothing after the second half
        QVERIFY(joinThroughEdit(fx.p, fx.s(), v).ok);
        QCOMPARE(fx.v1().clips.size(), size_t(1));
        QCOMPARE(fx.a1().clips.size(), size_t(1));
        const Clip& joined = fx.v1().clips[0];
        QCOMPARE(joined.id, v);
        QCOMPARE(joined.start, FrameTime(0));
        QCOMPARE(joined.duration, FrameTime(100));
        QCOMPARE(joined.sourceIn, 10.0);
        QCOMPARE(joined.markers.size(), size_t(1));
        QCOMPARE(fx.a1().clips[0].duration, FrameTime(100));
        QVERIFY(throughEdits(fx.s()).empty());
        // Several at once.
        QVERIFY(razorAll(fx.p, fx.s(), 20).ok && razorAll(fx.p, fx.s(), 60).ok);
        QCOMPARE(joinThroughEdits(fx.p, fx.s()), 2);
        QCOMPARE(fx.v1().clips.size(), size_t(1));
        QCOMPARE(fx.v1().clips[0].duration, FrameTime(100));
    }

    void duplicateFrameMarkers() {
        // The same file used three times: frames 0-40, then 20-60 (sharing 20-40), then 100-120 (shared with nothing).
        Fixture fx;
        const Id a = fx.put(V1, 0, 40, 0), b = fx.put(V1, 50, 40, 20), c = fx.put(V1, 100, 20, 100);
        auto d = duplicateFrames(fx.s());
        QCOMPARE(d.size(), size_t(2));
        QCOMPARE(d[a], (std::vector<DuplicateSpan>{{20, 40, 0}}));
        QCOMPARE(d[b], (std::vector<DuplicateSpan>{{50, 70, 0}}));
        QVERIFY(!d.count(c));
        // At double speed, its shared source frames take half as long on the timeline.
        clipById(fx.s(), b)->speed = 2.0;
        d = duplicateFrames(fx.s());
        QCOMPARE(d[b], (std::vector<DuplicateSpan>{{50, 60, 0}}));
        // Audio and titles are not counted; other media are a group of their own.
        MediaItem other = *fx.p.findMedia(fx.media);
        other.id = fx.p.newId();
        fx.p.media.push_back(other);
        Clip o1 = makeClip(fx.p, other, TrackKind::Video, fx.s()), o2 = o1;
        o1.start = 200, o1.duration = 10, o2.start = 220, o2.duration = 10;
        o2.id = fx.p.newId();
        QVERIFY(overwrite(fx.p, fx.s(), V1, o1).ok && overwrite(fx.p, fx.s(), V1, o2).ok);
        Clip sound = makeClip(fx.p, *fx.p.findMedia(fx.media), TrackKind::Audio, fx.s());
        sound.start = 300, sound.duration = 40;
        QVERIFY(overwrite(fx.p, fx.s(), A1, sound).ok);
        d = duplicateFrames(fx.s());
        QCOMPARE(d.size(), size_t(4));
        QCOMPARE(d[fx.v1().clips[3].id].front().group, 1);
    }

    void closeUpFraming() {
        // A 1920 x 1080 clip in a 1920 x 1080 sequence, a face three quarters across.
        Fixture fx;
        fx.s().width = 1920, fx.s().height = 1080;
        const Id clip = fx.put(V1, 0, 100, 10);
        while (fx.s().videoTracks.size() > 1) fx.s().videoTracks.pop_back();
        Result r = closeUp(fx.p, fx.s(), clip, 20, 60, 1.5, 0.75, 0.5);
        QVERIFY2(r.ok, r.error.c_str());
        QCOMPARE(fx.s().videoTracks.size(), size_t(2));  // a track added above
        const Clip* c = clipById(fx.s(), r.created.at(0));
        QVERIFY(c);
        QCOMPARE(c->start, FrameTime(20));
        QCOMPARE(c->duration, FrameTime(40));
        QCOMPARE(c->sourceIn, 30.0);  // the same pictures
        QCOMPARE(c->linkGroup, Id(0));
        QCOMPARE(c->motion.p("scale", 0), 150.0);
        // Moved towards the face, but no further than the picture still covers the frame (2880 wide: 480 each way).
        QCOMPARE(c->motion.p("pos_x", 0), -480.0);
        QCOMPARE(c->motion.p("pos_y", 0), -81.0);
        // Within the clip only; titles and audio refused.
        QVERIFY(!closeUp(fx.p, fx.s(), clip, 200, 300, 1.5, 0.5, 0.5).ok);
        const Id sound = fx.put(A1, 0, 10);
        QVERIFY(!closeUp(fx.p, fx.s(), sound, 0, 10, 1.5, 0.5, 0.5).ok);
        // Again over the same stretch: V2 is taken, so another track.
        QVERIFY(closeUp(fx.p, fx.s(), clip, 20, 60, 2.0, 0.5, 0.5).ok);
        QCOMPARE(fx.s().videoTracks.size(), size_t(3));
    }

    void sourceEditsOnTopAndRipple() {
        // Two linked 60-frame clips (an edit at 60), then 20-frame source ranges edited in around them.
        Fixture fx;
        QVERIFY(placeMedia(fx.p, fx.s(), fx.media, 0, 0, 60, V1, A1, false).ok);
        const Result second = placeMedia(fx.p, fx.s(), fx.media, 60, 0, 60, V1, A1, false);
        QVERIFY(second.ok && second.created.size() == 2);
        // Place on Top over 30-50: the track above V1, and the first audio track free there (A1 is taken).
        Result top = placeOnTop(fx.p, fx.s(), fx.media, 30, 100, 120, V1, A1);
        QVERIFY2(top.ok, top.error.c_str());
        QCOMPARE(top.created.size(), size_t(2));
        for (Id id : top.created) {
            const auto loc = locate(fx.s(), id);
            QVERIFY(loc && loc->track.index == 1);
            QCOMPARE(clipById(fx.s(), id)->start, FrameTime(30));
            QCOMPARE(clipById(fx.s(), id)->duration, FrameTime(20));
        }
        QCOMPARE(fx.v1().clips.size(), size_t(2));
        QCOMPARE(fx.a1().clips.size(), size_t(2));
        QCOMPARE(fx.s().duration(), FrameTime(120));
        // Again over the same stretch: the next tracks up (made if missing).
        top = placeOnTop(fx.p, fx.s(), fx.media, 35, 100, 120, V1, A1);
        QVERIFY(top.ok);
        for (Id id : top.created) QCOMPARE(locate(fx.s(), id)->track.index, 2);
        // Ripple Overwrite of the second clip: its video and sound give way to the 20 frames and the end pulls in.
        const Result ro = rippleOverwrite(fx.p, fx.s(), second.created[0], fx.media, 200, 220, V1, A1);
        QVERIFY2(ro.ok, ro.error.c_str());
        QCOMPARE(fx.v1().clips.size(), size_t(2));
        QCOMPARE(fx.v1().clips[1].start, FrameTime(60));
        QCOMPARE(fx.v1().clips[1].duration, FrameTime(20));
        QCOMPARE(fx.v1().clips[1].sourceIn, 200.0);
        QCOMPARE(fx.a1().clips[1].duration, FrameTime(20));
        QVERIFY(fx.v1().clips[1].linkGroup && fx.v1().clips[1].linkGroup == fx.a1().clips[1].linkGroup);
        QCOMPARE(fx.s().duration(), FrameTime(80));
        QVERIFY(!rippleOverwrite(fx.p, fx.s(), 999999, fx.media, 0, 20, V1, A1).ok);
        // Smart Insert's point: the nearest clip edge, or the frame itself on an empty track.
        QCOMPARE(nearestEdit(fx.s(), V1, 55), FrameTime(60));
        QCOMPARE(nearestEdit(fx.s(), V1, 25), FrameTime(0));
        QCOMPARE(nearestEdit(fx.s(), V1, 75), FrameTime(80));
        fx.s().videoTracks.push_back(Track{});
        QCOMPARE(nearestEdit(fx.s(), {TrackKind::Video, int(fx.s().videoTracks.size()) - 1}, 42), FrameTime(42));
    }

    void audioRoles() {
        // A video clip with linked sound, and a second sound clip: roles go on the sound only.
        Fixture fx;
        const Result placed = placeMedia(fx.p, fx.s(), fx.media, 0, 0, 60, V1, A1, false);
        QVERIFY(placed.ok && placed.created.size() == 2);
        const Id video = placed.created[0], sound = placed.created[1];
        const Id music = fx.put({TrackKind::Audio, 1}, 0, 30);
        QCOMPARE(setClipRole(fx.s(), {video}, "Dialogue"), 1);
        QCOMPARE(clipById(fx.s(), sound)->role, std::string("Dialogue"));
        QVERIFY(clipById(fx.s(), video)->role.empty());
        QCOMPARE(setClipRole(fx.s(), {video}, "Dialogue"), 0);  // no change
        QCOMPARE(setClipRole(fx.s(), {music}, "Score"), 1);
        // The standard roles first, then the sequence's own.
        QCOMPARE(sequenceRoles(fx.s()), (std::vector<std::string>{"Dialogue", "Music", "Effects", "Score"}));
        QVERIFY(!roleMuted(fx.s(), "Score"));
        setRoleMuted(fx.s(), "Score", true);
        setRoleMuted(fx.s(), "Score", true);
        QCOMPARE(fx.s().mutedRoles.size(), size_t(1));
        QVERIFY(roleMuted(fx.s(), "Score") && !roleMuted(fx.s(), "") && !roleMuted(fx.s(), "Dialogue"));
        // Saved and read back.
        Project back;
        QVERIFY(projectFromJson(projectToJson(fx.p), back));
        QCOMPARE(clipById(*back.active(), music)->role, std::string("Score"));
        QCOMPARE(clipById(*back.active(), sound)->role, std::string("Dialogue"));
        QVERIFY(roleMuted(*back.active(), "Score"));
        setRoleMuted(fx.s(), "Score", false);
        QVERIFY(fx.s().mutedRoles.empty());
        QCOMPARE(setClipRole(fx.s(), {music, video}, ""), 2);  // cleared
        QCOMPARE(sequenceRoles(fx.s()).size(), size_t(3));
    }

    void effectPresets() {
        // A clip's blur and a keyframed brightness saved as a preset, read back and put on another clip.
        Fixture fx;
        const Id a = fx.put(V1, 0, 50), b = fx.put(V1, 60, 50);
        Clip* ca = clipById(fx.s(), a);
        Effect blur = makeEffect("gaussian_blur", fx.p.newId());
        blur.params["radius"] = Param(7.5);
        Effect bc = makeEffect("brightness_contrast", fx.p.newId());
        bc.params["brightness"].addKey(0, 0.0);
        bc.params["brightness"].addKey(20, 30.0);
        ca->effects = {blur, bc};
        EffectPreset preset{"Dreamy", true, ca->effects};
        const std::string json = presetToJson(preset);
        EffectPreset back;
        QVERIFY(presetFromJson(json, back));
        QCOMPARE(back.name, std::string("Dreamy"));
        QVERIFY(back.video);
        QCOMPARE(back.effects.size(), size_t(2));
        QCOMPARE(back.effects[0].params.at("radius").value, 7.5);
        QCOMPARE(back.effects[1].params.at("brightness").keys.size(), size_t(2));
        std::string why;
        QVERIFY(!presetFromJson("{}", back, &why));
        QVERIFY(!presetFromJson("not json", back, &why));
        // Applied: added after the clip's own effects, with new ids, keyframes where they were.
        Clip* cb = clipById(fx.s(), b);
        cb->effects.push_back(makeEffect("invert", fx.p.newId()));
        QCOMPARE(applyPreset(fx.p, *cb, preset), 2);
        QCOMPARE(cb->effects.size(), size_t(3));
        QCOMPARE(cb->effects[1].type, std::string("gaussian_blur"));
        QVERIFY(cb->effects[1].id != blur.id && cb->effects[2].id != bc.id);
        QCOMPARE(cb->effects[2].params.at("brightness").at(20), 30.0);
    }

    void keyframeCopyPasteCore() {
        Fixture fx;
        const Id a = fx.put(V1, 0, 100), b = fx.put(V1, 100, 100);
        Clip& ca = *clipById(fx.s(), a);
        ca.motion.params["scale"].addKey(20, 100);
        ca.motion.params["scale"].addKey(40, 150, Interp::Bezier);
        ca.motion.params["scale"].keys[1].inDt = -5;
        ca.motion.params["scale"].keys[1].inDv = 3;
        Effect blur = makeEffect("gaussian_blur", fx.p.newId());
        blur.params["radius"].addKey(30, 4);
        ca.effects.push_back(blur);
        const ParamAddress scale{ParamSlot::Motion, 0, "scale"}, radius{ParamSlot::Effect, blur.id, "radius"};
        const CopiedKeys copied = copyKeys(ca, {{scale, 20}, {scale, 40}, {radius, 30}, {scale, 99}});  // 99: no key
        QCOMPARE(copied.lanes.size(), size_t(2));
        QCOMPARE(copied.lanes[0].keys.front().t, FrameTime(0));  // from the earliest copied key
        QCOMPARE(copied.lanes[1].effectType, std::string("gaussian_blur"));
        QCOMPARE(copied.lanes[1].keys.front().t, FrameTime(10));
        // Onto b at its frame 50: scale goes in; b has no blur, so the radius lane is skipped.
        Clip& cb = *clipById(fx.s(), b);
        cb.motion.params["scale"].addKey(70, 50);  // replaced by the pasted key at 70
        QCOMPARE(pasteKeys(cb, copied, 50), 2);
        const Param& sc = cb.motion.params.at("scale");
        QCOMPARE(sc.keys.size(), size_t(2));
        QCOMPARE(sc.keys[0].t, FrameTime(50));
        QCOMPARE(sc.keys[1].t, FrameTime(70));
        QCOMPARE(sc.keys[1].v, 150.0);
        QCOMPARE(sc.keys[1].interp, Interp::Bezier);
        QCOMPARE(sc.keys[1].inDt, -5.0);
        // With a blur of its own (another id), the radius goes there.
        cb.effects.push_back(makeEffect("gaussian_blur", fx.p.newId()));
        QCOMPARE(pasteKeys(cb, copied, 0), 3);
        QCOMPARE(cb.effects[0].params.at("radius").keys.at(0).t, FrameTime(10));
        QCOMPARE(sc.keys.size(), size_t(4));
        // Into the same clip, later: a second fade.
        QCOMPARE(pasteKeys(ca, copied, 60), 3);
        QCOMPARE(ca.motion.params.at("scale").keys.size(), size_t(4));
        QCOMPARE(ca.effects[0].params.at("radius").keys.size(), size_t(2));
        QCOMPARE(pasteKeys(ca, CopiedKeys{}, 0), 0);
    }

    void reverseMatchFrameUses() {
        // Source frames 30-89 at 0; 50-69 at 100 (twice as fast: 40 frames of source in 20); a disabled clip at 200.
        Fixture fx;
        const Id a = fx.put(V1, 0, 60, 30), b = fx.put(V1, 100, 20, 50);
        clipById(fx.s(), b)->speed = 2.0;
        const Id off = fx.put(V1, 200, 60, 30);
        clipById(fx.s(), off)->enabled = false;
        const Id sound = fx.put(A1, 300, 60, 30);
        auto uses = sourceFrameUses(fx.s(), fx.media, 60);
        QCOMPARE(uses.size(), size_t(3));
        QCOMPARE(uses[0].clip, a);
        QCOMPARE(uses[0].at, FrameTime(30));
        QCOMPARE(uses[1].clip, b);
        QCOMPARE(uses[1].at, FrameTime(105));  // 50 + 2 * 5
        QCOMPARE(uses[2].clip, sound);
        QCOMPARE(uses[2].track.kind, TrackKind::Audio);
        QCOMPARE(uses[2].at, FrameTime(330));
        // Frame 61 falls between two of b's frames (60, 62): the nearest one.
        uses = sourceFrameUses(fx.s(), fx.media, 61);
        QCOMPARE(uses[1].at, FrameTime(105));
        // Outside every clip's stretch; and another media item.
        QVERIFY(sourceFrameUses(fx.s(), fx.media, 5).empty());
        QVERIFY(sourceFrameUses(fx.s(), 999, 60).empty());
        // Reversed: the stretch runs backwards.
        clipById(fx.s(), a)->reverse = true;
        uses = sourceFrameUses(fx.s(), fx.media, 88);
        QVERIFY(!uses.empty() && uses[0].clip == a && uses[0].at == 1);
    }

    void videoLayoutGeometry() {
        // 1920 x 1080 clips in a 1920 x 1080 sequence.
        Fixture fx;
        fx.s().width = 1920, fx.s().height = 1080;
        while (fx.s().videoTracks.size() < 2) addTrack(fx.p, fx.s(), TrackKind::Video);
        const Id a = fx.put(V1, 0, 100), b = fx.put(V2, 0, 100);
        clipById(fx.s(), b)->motion.params["scale"].addKey(0, 50);  // keys are replaced
        auto m = [&](Id id, const char* k) { return clipById(fx.s(), id)->motion.p(k, 0); };
        // Side by side: each covers its 960 x 1080 half at 100 %, a quarter cropped off each side, V1 on the left.
        QVERIFY(arrangeLayout(fx.p, fx.s(), {b, a}, Layout::SideBySide).ok);
        QCOMPARE(m(a, "scale"), 100.0);
        QCOMPARE(m(a, "pos_x"), -480.0);
        QCOMPARE(m(b, "pos_x"), 480.0);
        QCOMPARE(m(a, "crop_left"), 25.0);
        QCOMPARE(m(a, "crop_right"), 25.0);
        QCOMPARE(m(a, "crop_top"), 0.0);
        QVERIFY(!clipById(fx.s(), b)->motion.params.at("scale").animated());
        // Top and bottom: 1920 x 540 cells, so 100 % with a quarter off top and bottom.
        QVERIFY(arrangeLayout(fx.p, fx.s(), {a, b}, Layout::TopAndBottom).ok);
        QCOMPARE(m(a, "pos_y"), -270.0);
        QCOMPARE(m(b, "pos_y"), 270.0);
        QCOMPARE(m(a, "crop_top"), 25.0);
        QCOMPARE(m(a, "crop_left"), 0.0);
        // The grid: 960 x 540 cells, 50 %, no crop (the same shape).
        QVERIFY(arrangeLayout(fx.p, fx.s(), {a, b}, Layout::Grid).ok);
        QCOMPARE(m(a, "scale"), 50.0);
        QCOMPARE(m(b, "pos_x"), 480.0);
        QCOMPARE(m(b, "pos_y"), -270.0);
        QCOMPARE(m(b, "crop_left"), 0.0);
        // Picture in picture: V1 full frame; V2 at 30 %, bottom right, 43.2 px (4 % of 1080) in from the edges.
        QVERIFY(arrangeLayout(fx.p, fx.s(), {a, b}, Layout::PictureInPicture).ok);
        QCOMPARE(m(a, "scale"), 100.0);
        QCOMPARE(m(a, "pos_x"), 0.0);
        QVERIFY(std::fabs(m(b, "scale") - 30) < 1e-9);
        QVERIFY(std::fabs(m(b, "pos_x") - (1920 - 43.2 - 288 - 960)) < 1e-6);
        QVERIFY(std::fabs(m(b, "pos_y") - (1080 - 43.2 - 162 - 540)) < 1e-6);
        LayoutOptions topLeft;
        topLeft.corner = 0;
        QVERIFY(arrangeLayout(fx.p, fx.s(), {a, b}, Layout::PictureInPicture, topLeft).ok);
        QVERIFY(std::fabs(m(b, "pos_x") - (43.2 + 288 - 960)) < 1e-6);
        QVERIFY(std::fabs(m(b, "pos_y") - (43.2 + 162 - 540)) < 1e-6);
        // Three across: 640 x 1080 cells, a third of the width kept.
        QVERIFY(arrangeLayout(fx.p, fx.s(), {a, b}, Layout::ThreeAcross).ok);
        QVERIFY(std::fabs(m(a, "crop_left") - 100.0 / 3) < 1e-9);
        QCOMPARE(m(a, "pos_x"), -640.0);
        QCOMPARE(m(b, "pos_x"), 0.0);
        // Back to full frame; audio clips and nothing refused.
        QVERIFY(arrangeLayout(fx.p, fx.s(), {a, b}, Layout::FullFrame).ok);
        QCOMPARE(m(b, "scale"), 100.0);
        QCOMPARE(m(b, "crop_left"), 0.0);
        QCOMPARE(m(b, "pos_y"), 0.0);
        const Id sound = fx.put(A1, 0, 10);
        QVERIFY(!arrangeLayout(fx.p, fx.s(), {sound}, Layout::Grid).ok);
        QVERIFY(!arrangeLayout(fx.p, fx.s(), {}, Layout::Grid).ok);
        QCOMPARE(layoutCells(Layout::Grid, 4, 1920, 1080, {20}).at(3).x, 20 + 930 + 20.0);  // (1920 - 3 gaps) / 2 wide
    }

    void matchFrameThroughNesting() {
        // Two cameras in a multicam (B a second after A), cut in at 100; a 60 fps nested sequence of A at 300 on V2.
        Project p = makeDefaultProject();
        auto addMedia = [&](const char* name) {
            MediaItem m;
            m.id = p.newId();
            m.kind = MediaKind::Video;
            m.name = name;
            m.path = std::string("/nonexistent/") + name;
            m.duration = 30.0;
            m.width = 1920, m.height = 1080;
            m.fps = {30, 1};
            m.hasVideo = true;
            p.media.push_back(m);
            return m.id;
        };
        const Id camA = addMedia("A.mov"), camB = addMedia("B.mov");
        std::string err;
        const Id mcMedia = makeMulticam(p, {camA, camB}, {0.0, 1.0}, "Interview", &err);
        QVERIFY2(mcMedia, err.c_str());
        Sequence& s = *p.active();
        const Result mcClip = placeMedia(p, s, mcMedia, 100, 0, 200, V1, A1, false);
        QVERIFY(mcClip.ok);
        Sequence nested = makeSequence(p, "Nest", 1920, 1080, {60, 1});
        Clip inner = makeClip(p, *p.findMedia(camA), TrackKind::Video, nested);
        inner.start = 0, inner.duration = 600;
        nested.videoTracks[0].clips.push_back(inner);
        MediaItem nm;
        nm.id = p.newId();
        nm.kind = MediaKind::Sequence;
        nm.name = "Nest";
        nm.sequenceId = nested.id;
        nm.hasVideo = true;
        nm.duration = 10;
        p.sequences.push_back(nested);
        p.media.push_back(nm);
        Sequence& top = *p.active();
        QVERIFY(placeMedia(p, top, nm.id, 300, 0, 100, V2, A1, false).ok);
        // The multicam shows angle A: frame 160 is A's frame 60; switched to B, B's frame 30 (B starts a second in).
        auto m = matchSource(p, top, 160);
        QVERIFY(m && m->media == camA);
        QCOMPARE(m->frame, 60.0);
        QCOMPARE(m->clip, mcClip.created.at(0));
        clipById(top, mcClip.created.at(0))->angle = 1;
        m = matchSource(p, top, 160);
        QVERIFY(m && m->media == camB);
        QCOMPARE(m->frame, 30.0);
        // Through the 60 fps nest: 10 frames in is a third of a second, A's frame 10 in this sequence's frames.
        m = matchSource(p, top, 310);
        QVERIFY(m && m->media == camA);
        QVERIFY2(std::fabs(m->frame - 10) < 1e-9, qPrintable(QString::number(m->frame)));
        // Nothing there; and a generator on top is looked through.
        QVERIFY(!matchSource(p, top, 1000));
        Clip title = makeGeneratorClip(p, "title", 20);
        title.start = 150;
        QVERIFY(overwrite(p, top, {TrackKind::Video, 2}, title).ok);
        m = matchSource(p, top, 160);
        QVERIFY(m && m->media == camB);
    }

    void timelineCompare() {
        // The old cut: three shots of the same file on V1 (the middle one with its sound), a title on V2 and an extra
        // shot on V2 later on.
        Fixture fx;
        Sequence& old = fx.s();
        const Id c1 = fx.put(V1, 0, 60, 0);
        const Result mid = placeMedia(fx.p, old, fx.media, 60, 100, 160, V1, A1, false);
        QVERIFY(mid.ok);
        const Id c2 = mid.created[0];
        const Id c3 = fx.put(V1, 120, 60, 200);
        Clip title = makeGeneratorClip(fx.p, "title", 30);
        title.start = 10;
        title.generator.strings["text"] = "Hello";
        const Id titleId = title.id;
        QVERIFY(overwrite(fx.p, old, V2, title).ok);
        const Id extra = fx.put(V2, 200, 20, 250);
        QVERIFY(compareSequences(fx.p, old, old).empty());  // nothing changed
        // The new cut: shot 1 trimmed 10 frames at the head and everything rippled up; shots 2 and 3 swapped; shot 3
        // blurred; the title on V3; the extra shot gone; a new shot at the end.
        Sequence cut = old;
        cut.id = fx.p.newId();
        while (cut.videoTracks.size() < 3) cut.videoTracks.push_back(makeTrack(fx.p, TrackKind::Video, "V3"));
        Clip* n1 = clipById(cut, c1);
        n1->sourceIn = 10, n1->duration = 50;
        clipById(cut, c3)->start = 50;
        clipById(cut, c2)->start = 110;
        for (Clip& a : cut.audioTracks[0].clips) a.start = 110;
        cut.videoTracks[0].clips = {*clipById(cut, c1), *clipById(cut, c3), *clipById(cut, c2)};
        cut.videoTracks[0].clips[1].effects.push_back(makeEffect("gaussian_blur", fx.p.newId()));
        Clip movedTitle = *clipById(cut, titleId);
        cut.videoTracks[1].clips.clear();
        cut.videoTracks[2].clips = {movedTitle};
        Clip added = makeClip(fx.p, *fx.p.findMedia(fx.media), TrackKind::Video, cut);
        added.start = 170, added.duration = 30, added.sourceIn = 280;
        cut.videoTracks[0].clips.push_back(added);
        const auto changes = compareSequences(fx.p, old, cut);
        auto find = [&](Id before, Id after) -> const TimelineChange* {
            for (const auto& c : changes)
                if (c.before == before && c.after == after) return &c;
            return nullptr;
        };
        const TimelineChange* t1 = find(c1, c1);
        QVERIFY(t1 && t1->kind == ChangeKind::Trimmed);
        QCOMPARE(t1->details, std::string("in +10"));
        const TimelineChange* t3 = find(c3, c3);
        QVERIFY(t3 && t3->kind == ChangeKind::Changed && t3->details == "effects");  // slid up, but kept its place
        const TimelineChange* t2 = find(c2, c2);
        QVERIFY(t2 && t2->kind == ChangeKind::Moved);
        QCOMPARE(t2->details, std::string("changed places"));
        const TimelineChange* tt = find(titleId, titleId);
        QVERIFY(tt && tt->kind == ChangeKind::Moved && tt->details == "V2 → V3");
        const TimelineChange* gone = find(extra, 0);
        QVERIFY(gone && gone->kind == ChangeKind::Removed && gone->at == 200);
        const TimelineChange* fresh = find(0, added.id);
        QVERIFY(fresh && fresh->kind == ChangeKind::Added && fresh->at == 170);
        // Shot 2's sound only slid along: nothing more. In time order.
        QCOMPARE(changes.size(), size_t(6));
        QVERIFY(std::is_sorted(changes.begin(), changes.end(), [](const auto& a, const auto& b) { return a.at < b.at; }));
        // A retitled card is a change; the linked sound of a removed shot is not listed again.
        clipById(cut, titleId)->generator.strings["text"] = "Goodbye";
        const auto retitled = compareSequences(fx.p, old, cut);
        QCOMPARE(retitled.size(), size_t(6));
        for (const auto& c : retitled)
            if (c.after == titleId) QCOMPARE(c.details, std::string("V2 → V3, title"));
        Sequence noMid = old;
        QVERIFY(removeClips(fx.p, noMid, linkedClips(noMid, c2), false).ok);
        const auto lost = compareSequences(fx.p, old, noMid);
        QCOMPARE(lost.size(), size_t(1));
        QVERIFY(lost[0].kind == ChangeKind::Removed && lost[0].before == c2);
        QCOMPARE(std::string(changeKindName(ChangeKind::Trimmed)), std::string("Trimmed"));
    }

    void maskPathModel() {
        auto near = [](double a, double b, double tol = 1e-9) { return std::fabs(a - b) <= tol; };
        QVERIFY(isMaskPathParam("mask.p0.x") && isMaskPathParam("mask.p12.oy"));
        QVERIFY(!isMaskPathParam("mask.x") && !isMaskPathParam("mask.p.x") && !isMaskPathParam("mask.p0.z") && !isMaskPathParam("mask.p0"));
        // A square, then a triangle: the fourth point's parameters go.
        Effect e;
        e.type = "invert";
        const std::vector<PathPoint> sq = {{-0.5, -0.5}, {0.5, -0.5}, {0.5, 0.5}, {-0.5, 0.5}};
        setMaskPath(e, 0, sq);
        QCOMPARE(maskPathCount(e), 4);
        QVERIFY(maskPath(e, 0) == sq);
        setMaskPath(e, 0, {sq[0], sq[1], sq[2]});
        QCOMPARE(maskPathCount(e), 3);
        QVERIFY(!e.params.count("mask.p3.x"));
        QCOMPARE(maskPathParams(e).size(), size_t(18));

        // Animated as a whole: every coordinate keyed together, a change on another frame keys it there.
        setMaskPath(e, 0, sq);
        setMaskPathAnimated(e, 0, true);
        QVERIFY(maskPathAnimated(e));
        for (const std::string& n : maskPathParams(e)) QVERIFY(e.params[n].keyAt(0));
        std::vector<PathPoint> moved = sq;
        for (PathPoint& pt : moved) pt.x += 0.2;
        setMaskPath(e, 20, moved);
        QCOMPARE(maskPathKeyTimes(e), (std::vector<FrameTime>{0, 20}));
        QVERIFY(near(maskPath(e, 10)[1].x, 0.6));
        // A point added on the straight top edge: a corner on it at every key.
        QCOMPARE(insertMaskPoint(e, 10, 0, 0.5), 1);
        QCOMPARE(maskPathCount(e), 5);
        QCOMPARE(maskPathKeyTimes(e), (std::vector<FrameTime>{0, 20}));
        const auto at0 = maskPath(e, 0);
        QVERIFY(near(at0[1].y, -0.5) && at0[1].x > -0.5 && at0[1].x < 0.5 && !at0[1].smooth());
        QVERIFY(near(maskPath(e, 20)[1].x, at0[1].x + 0.2));
        QVERIFY(removeMaskPoint(e, 0, 1));
        QVERIFY(maskPath(e, 0) == sq);
        QVERIFY(maskPath(e, 20) == moved);
        // Stopping keeps the path as it is at the playhead.
        setMaskPathAnimated(e, 10, false);
        QVERIFY(!maskPathAnimated(e));
        QVERIFY(maskPathKeyTimes(e).empty());
        QVERIFY(near(maskPath(e, 0)[0].x, -0.4));

        // Smooth points; a point added on a curve keeps its shape (de Casteljau).
        Effect c;
        setMaskPath(c, 0, {{0, -0.5}, {0.5, 0.5}, {-0.5, 0.5}});
        toggleMaskPointSmooth(c, 0, 0);
        const auto before = maskPath(c, 0);
        QVERIFY(before[0].smooth() && near(before[0].oy, 0) && before[0].ox > 0 && near(before[0].ix, -before[0].ox));
        QCOMPARE(insertMaskPoint(c, 0, 0, 0.3), 1);
        const auto after = maskPath(c, 0);
        const auto split = segmentPoint(before[0], before[1], 0.3);
        QVERIFY(near(after[1].x, split.first) && near(after[1].y, split.second));
        for (double s : {0.1, 0.2}) {
            const auto o = segmentPoint(before[0], before[1], s), n = segmentPoint(after[0], after[1], s / 0.3);
            QVERIFY(near(o.first, n.first, 1e-12) && near(o.second, n.second, 1e-12));
        }
        for (double s : {0.5, 0.8}) {
            const auto o = segmentPoint(before[0], before[1], s), n = segmentPoint(after[1], after[2], (s - 0.3) / 0.7);
            QVERIFY(near(o.first, n.first, 1e-12) && near(o.second, n.second, 1e-12));
        }
        toggleMaskPointSmooth(c, 0, 0);
        QVERIFY(!maskPath(c, 0)[0].smooth());
        QVERIFY(removeMaskPoint(c, 0, 1));
        QVERIFY(!removeMaskPoint(c, 0, 0));  // a path keeps three points
        QCOMPARE(maskPathCount(c), 3);

        // Fitting the box to the path keeps it where it is in the frame (a turned box, a 16:9 frame).
        const MaskBox box{0.4, 0.6, 0.5, 0.3, 30};
        const std::vector<PathPoint> pts = {{-0.2, -0.3, 0, 0, 0.1, 0}, {0.4, -0.1}, {0.1, 0.45}};
        MaskBox fitted = box;
        std::vector<PathPoint> fp = pts;
        QVERIFY(fitMaskBox(fitted, fp, 1920, 1080));
        QCOMPARE(fitted.rotation, 30.0);
        for (size_t i = 0; i < pts.size(); ++i) {
            double u0, v0, u1, v1;
            boxToFrame(box, 1920, 1080, pts[i].x, pts[i].y, u0, v0);
            boxToFrame(fitted, 1920, 1080, fp[i].x, fp[i].y, u1, v1);
            QVERIFY(near(u0, u1, 1e-9) && near(v0, v1, 1e-9));
            double bx, by;
            frameToBox(fitted, 1920, 1080, u1, v1, bx, by);
            QVERIFY(near(bx, fp[i].x, 1e-9) && near(by, fp[i].y, 1e-9));
        }
        double hu0, hv0, hu1, hv1;
        boxToFrame(box, 1920, 1080, pts[0].x + 0.1, pts[0].y, hu0, hv0);
        boxToFrame(fitted, 1920, 1080, fp[0].x + fp[0].ox, fp[0].y + fp[0].oy, hu1, hv1);
        QVERIFY(near(hu0, hu1, 1e-9) && near(hv0, hv1, 1e-9));
        double x0 = 1, x1 = -1, y0 = 1, y1 = -1;
        for (const auto& [x, y] : flattenMaskPath(fp, true)) {
            x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
        }
        QVERIFY(near(x0, -0.5) && near(x1, 0.5) && near(y0, -0.5) && near(y1, 0.5));

        // From frame fractions: the box frames the path; each point lands where it was given.
        Effect f;
        const std::vector<PathPoint> given = {{0.2, 0.2}, {0.6, 0.25}, {0.4, 0.7}};
        setMaskPathFromFrame(f, given, 1920, 1080, false);
        QCOMPARE(f.p("mask.shape", 0), 5.0);
        const MaskBox fb = maskBox(f, 0);
        QVERIFY(near(fb.x, 0.4) && near(fb.y, 0.45) && near(fb.w, 0.4) && near(fb.h, 0.5));
        const auto got = maskPath(f, 0);
        for (size_t i = 0; i < given.size(); ++i) {
            double u, v;
            boxToFrame(fb, 1920, 1080, got[i].x, got[i].y, u, v);
            QVERIFY(near(u, given[i].x) && near(v, given[i].y));
        }
        setMaskPathFromFrame(f, given, 1920, 1080, true);
        QVERIFY(maskPath(f, 0)[0].smooth());

        // Closing a path being drawn fits the box around it.
        Effect d;
        d.params["mask.open"] = Param(1);
        setMaskPath(d, 0, {{-0.4, -0.4}, {0, -0.4}, {0, 0}});
        QVERIFY(closeMaskPath(d, 0, 1920, 1080));
        QVERIFY(!d.params.count("mask.open"));
        const MaskBox db = maskBox(d, 0);
        QVERIFY(near(db.w, 0.16) && near(db.h, 0.16) && near(db.x, 0.42) && near(db.y, 0.42));
        QVERIFY(!closeMaskPath(d, 0, 1920, 1080));
    }

    void transitionDurationLimits() {
        // A 60-frame clip cut to a 20-frame one, with a dissolve; a fade-in on a third clip later.
        Fixture fx;
        const Id a = fx.put(V1, 0, 60), b = fx.put(V1, 60, 20), c = fx.put(V1, 200, 30);
        const Result d = addTransition(fx.p, fx.s(), a, Edge::Out, "cross_dissolve", 10);
        const Result f = addTransition(fx.p, fx.s(), c, Edge::In, "cross_dissolve", 10);
        QVERIFY(d.ok && f.ok);
        // Centred on the cut, half in each clip: at most twice the shorter clip's length, 40 here.
        QCOMPARE(setTransitionDuration(fx.s(), d.created[0], 30).applied, FrameTime(30));
        QCOMPARE(setTransitionDuration(fx.s(), d.created[0], 300).applied, FrameTime(40));
        QCOMPARE(setTransitionDuration(fx.s(), d.created[0], 0).applied, FrameTime(2));
        FrameTime from = 0, to = 0;
        setTransitionDuration(fx.s(), d.created[0], 40);
        QVERIFY(transitionRange(fx.v1(), *transitionById(fx.s(), d.created[0]), from, to));
        QVERIFY(from == 40 && to == 80);  // exactly fills the short clip
        // A fade fits its clip.
        QCOMPARE(setTransitionDuration(fx.s(), f.created[0], 100).applied, FrameTime(30));
        QVERIFY(!setTransitionDuration(fx.s(), 999999, 10).ok);
        fx.v1().locked = true;
        QVERIFY(!setTransitionDuration(fx.s(), f.created[0], 10).ok);
        (void)b;
    }

    void keyframeRepeat() {
        // 0 at frame 10, 10 at frame 20.
        Param p;
        p.addKey(10, 0.0);
        p.addKey(20, 10.0);
        QCOMPARE(p.at(25), 10.0);  // held
        p.repeat = Repeat::Loop;
        QCOMPARE(p.at(25), 5.0);
        QCOMPARE(p.at(30), 0.0);
        QCOMPARE(p.at(5), 0.0);  // before the first key: still held
        p.repeat = Repeat::PingPong;
        QCOMPARE(p.at(25), 5.0);
        QCOMPARE(p.at(28), 2.0);  // on the way back
        QCOMPARE(p.at(32), 2.0);  // forwards again
        p.repeat = Repeat::Offset;
        QCOMPARE(p.at(25), 15.0);
        QCOMPARE(p.at(42), 32.0);
        // Saved and read back; one key alone holds whatever the setting.
        Fixture fx;
        const Id clip = fx.put(V1, 0, 100);
        Clip* c = clipById(fx.s(), clip);
        c->motion.params["rotation"] = p;
        Project back;
        QVERIFY(projectFromJson(projectToJson(fx.p), back));
        QCOMPARE(clipById(*back.active(), clip)->motion.params.at("rotation").repeat, Repeat::Offset);
        Param one;
        one.addKey(10, 3.0);
        one.repeat = Repeat::Loop;
        QCOMPARE(one.at(50), 3.0);
    }

    void trackFolders() {
        Fixture fx;
        for (int k = 0; k < 3; ++k) addTrack(fx.p, fx.s(), TrackKind::Audio);
        QVERIFY(fx.s().audioTracks.size() >= 4);
        // A2 and A3 in "Dialogue".
        QVERIFY(!setTrackFolder(fx.s(), {}, "Dialogue").ok);
        QVERIFY(!setTrackFolder(fx.s(), {{TrackKind::Audio, 1}}, "A/B").ok);
        QVERIFY(setTrackFolder(fx.s(), {{TrackKind::Audio, 1}, {TrackKind::Audio, 2}}, "Dialogue").ok);
        QCOMPARE(folderTracks(fx.s(), TrackKind::Audio, "Dialogue"), (std::vector<int>{1, 2}));
        QVERIFY(folderTracks(fx.s(), TrackKind::Video, "Dialogue").empty());
        // Collapsed, renamed (still collapsed), saved and read back.
        setFolderCollapsed(fx.s(), TrackKind::Audio, "Dialogue", true);
        QVERIFY(folderCollapsed(fx.s(), TrackKind::Audio, "Dialogue"));
        QVERIFY(!folderCollapsed(fx.s(), TrackKind::Video, "Dialogue"));
        QVERIFY(renameFolder(fx.s(), TrackKind::Audio, "Dialogue", "Dial").ok);
        QVERIFY(folderCollapsed(fx.s(), TrackKind::Audio, "Dial"));
        QCOMPARE(fx.s().audioTracks[1].folder, std::string("Dial"));
        QVERIFY(setTrackFolder(fx.s(), {{TrackKind::Audio, 3}}, "Music").ok);
        QVERIFY(!renameFolder(fx.s(), TrackKind::Audio, "Dial", "Music").ok);  // taken
        Project back;
        QVERIFY(projectFromJson(projectToJson(fx.p), back));
        QCOMPARE(back.active()->audioTracks[2].folder, std::string("Dial"));
        QVERIFY(folderCollapsed(*back.active(), TrackKind::Audio, "Dial"));
        // A folder fader, saved with the sequence.
        setFolderGain(fx.s(), TrackKind::Audio, "Dial", -3.5);
        Project gained;
        QVERIFY(projectFromJson(projectToJson(fx.p), gained));
        QCOMPARE(folderGain(*gained.active(), TrackKind::Audio, "Dial"), -3.5);
        QCOMPARE(folderGain(*gained.active(), TrackKind::Video, "Dial"), 0.0);
        // Emptied, the folder is forgotten.
        QVERIFY(setTrackFolder(fx.s(), {{TrackKind::Audio, 1}, {TrackKind::Audio, 2}}, "").ok);
        QVERIFY(fx.s().collapsedFolders.empty());
        QVERIFY(fx.s().folderGains.empty());
    }

    void auditions() {
        // The clip plays the fixture's media from 10; take B (picture and sound) and C (sound only) are other files.
        Fixture fx;
        auto media = [&](const char* name, bool video) {
            MediaItem m = *fx.p.findMedia(fx.media);
            m.id = fx.p.newId();
            m.name = name;
            m.path = std::string("/nonexistent/") + name;
            m.hasVideo = video;
            if (!video) m.kind = MediaKind::Audio;
            fx.p.media.push_back(m);
            return m.id;
        };
        const Id b = media("take2.mov", true), c = media("music.wav", false);
        QVERIFY(placeMedia(fx.p, fx.s(), fx.media, 20, 10, 70, V1, A1, false).ok);
        const Id clip = fx.v1().clips[0].id, sound = fx.a1().clips[0].id;
        clipById(fx.s(), clip)->effects.push_back(makeEffect("brightness_contrast", fx.p.newId()));

        // Takes need pictures on a video track; titles have none.
        QVERIFY(!addTakes(fx.p, fx.s(), clip, {{c, 0.0}}).ok);
        QVERIFY(!pickTake(fx.p, fx.s(), clip, 1).ok);  // not an audition yet
        Result r = addTakes(fx.p, fx.s(), clip, {{b, 50.0}});
        QVERIFY2(r.ok, r.error.c_str());
        Clip* a = clipById(fx.s(), clip);
        QCOMPARE(a->takes.size(), size_t(2));
        QCOMPARE(a->takes[0].mediaId, fx.media);
        QCOMPARE(a->takes[1].offset, 40.0);
        QCOMPARE(a->take, 0);

        // Picking B plays it from 50 in the same place, with the same effect; the linked sound follows.
        QVERIFY(pickTake(fx.p, fx.s(), clip, 1).ok);
        a = clipById(fx.s(), clip);
        QCOMPARE(a->mediaId, b);
        QCOMPARE(a->sourceIn, 50.0);
        QCOMPARE(a->start, FrameTime(20));
        QCOMPARE(a->duration, FrameTime(60));
        QCOMPARE(a->name, std::string("take2.mov"));
        QCOMPARE(a->effects.size(), size_t(1));
        QCOMPARE(clipById(fx.s(), sound)->mediaId, b);
        QCOMPARE(clipById(fx.s(), sound)->sourceIn, 50.0);
        QCOMPARE(a->takes[0].offset, -40.0);

        // A trimmed head carries the takes along: the first take now starts 5 frames later too.
        QVERIFY(trim(fx.p, fx.s(), clip, Edge::In, 5, TrimMode::Normal, false).ok);
        QCOMPARE(clipById(fx.s(), clip)->sourceIn, 55.0);
        QVERIFY(cycleTake(fx.p, fx.s(), clip, 1).ok);  // round to the first
        a = clipById(fx.s(), clip);
        QCOMPARE(a->mediaId, fx.media);
        QCOMPARE(a->sourceIn, 15.0);
        QCOMPARE(a->take, 0);
        QCOMPARE(a->name, std::string("clip.mov"));
        QVERIFY(cycleTake(fx.p, fx.s(), clip, -1).ok);
        QCOMPARE(clipById(fx.s(), clip)->take, 1);

        // Split, both halves stay auditions with their own place in each take.
        const Id right = splitClip(fx.p, fx.v1(), 0, 50);
        QVERIFY(right);
        QCOMPARE(clipById(fx.s(), right)->takes.size(), size_t(2));
        QVERIFY(pickTake(fx.p, fx.s(), right, 0).ok);
        QCOMPARE(clipById(fx.s(), right)->sourceIn, 15.0 + 25.0);

        // Saved and read back.
        Project back;
        QVERIFY(projectFromJson(projectToJson(fx.p), back));
        QCOMPARE(clipById(*back.active(), clip)->takes, clipById(fx.s(), clip)->takes);
        QCOMPARE(clipById(*back.active(), clip)->take, 1);

        // Finalized, the pick stays and the rest go.
        QVERIFY(finalizeAudition(fx.p, fx.s(), clip).ok);
        QVERIFY(clipById(fx.s(), clip)->takes.empty());
        QCOMPARE(clipById(fx.s(), clip)->mediaId, b);
        QVERIFY(!cycleTake(fx.p, fx.s(), clip, 1).ok);
    }

    void swapClips() {
        // A (picture and sound), B (picture and sound), C (picture): A and B change places.
        Fixture fx;
        QVERIFY(placeMedia(fx.p, fx.s(), fx.media, 0, 20, 50, V1, A1, false).ok);  // handles either side, for a dissolve
        QVERIFY(placeMedia(fx.p, fx.s(), fx.media, 30, 60, 110, V1, A1, false).ok);
        const Id a = fx.v1().clips[0].id, b = fx.v1().clips[1].id, aSound = fx.a1().clips[0].id, bSound = fx.a1().clips[1].id;
        const Id c = fx.put(V1, 80, 20);
        QVERIFY(addTransition(fx.p, fx.s(), a, Edge::Out, "cross_dissolve", 10).ok);
        Result r = swapClip(fx.p, fx.s(), a, true);
        QVERIFY2(r.ok, r.error.c_str());
        QCOMPARE(clipById(fx.s(), b)->start, FrameTime(0));
        QCOMPARE(clipById(fx.s(), a)->start, FrameTime(50));
        QCOMPARE(clipById(fx.s(), bSound)->start, FrameTime(0));
        QCOMPARE(clipById(fx.s(), aSound)->start, FrameTime(50));
        QCOMPARE(clipById(fx.s(), c)->start, FrameTime(80));
        QCOMPARE(clipById(fx.s(), a)->sourceIn, 20.0);  // the same pictures, only moved
        // The dissolve goes to the new edit, from B into A.
        QCOMPARE(fx.v1().transitions.size(), size_t(1));
        QCOMPARE(fx.v1().transitions[0].clipA, b);
        QCOMPARE(fx.v1().transitions[0].clipB, a);
        // And back.
        QVERIFY(swapClip(fx.p, fx.s(), a, false).ok);
        QCOMPARE(clipById(fx.s(), a)->start, FrameTime(0));
        QCOMPARE(clipById(fx.s(), b)->start, FrameTime(30));
        QCOMPARE(fx.v1().transitions[0].clipA, a);
        // A gap between two stays between them.
        const Id d = fx.put(V1, 120, 20), e = fx.put(V1, 150, 10);
        QVERIFY(swapClip(fx.p, fx.s(), e, false).ok);
        QCOMPARE(clipById(fx.s(), e)->start, FrameTime(120));
        QCOMPARE(clipById(fx.s(), d)->start, FrameTime(140));
        // Nothing beyond the ends.
        QVERIFY(!swapClip(fx.p, fx.s(), a, false).ok);
        QVERIFY(!swapClip(fx.p, fx.s(), d, true).ok);
        // A locked track refuses.
        fx.v1().locked = true;
        QVERIFY(!swapClip(fx.p, fx.s(), c, true).ok);
        fx.v1().locked = false;
        // Linked sound that would land on another clip refuses, changing nothing.
        Fixture f2;
        QVERIFY(placeMedia(f2.p, f2.s(), f2.media, 0, 0, 30, V1, A1, false).ok);
        const Id pa = f2.v1().clips[0].id;
        const Id pb = f2.put(V1, 30, 50);
        const Id x = f2.put(A1, 50, 30);
        const Sequence before = f2.s();
        QVERIFY(!swapClip(f2.p, f2.s(), pa, true).ok);
        QCOMPARE(clipById(f2.s(), pb)->start, FrameTime(30));
        QCOMPARE(clipById(f2.s(), x)->start, FrameTime(50));
        QVERIFY(f2.s().videoTracks[0].clips == before.videoTracks[0].clips);
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
            // Our own transition types come back as themselves; unknown custom ones as dissolves.
            Sequence typed = original;
            QVERIFY(!typed.videoTracks[0].transitions.empty());
            typed.videoTracks[0].transitions[0].type = "smooth_cut";
            ImportResult r2 = importOtio(fx.p, exportOtio(fx.p, typed));
            QVERIFY2(r2.ok, r2.error.c_str());
            QCOMPARE(fx.p.findSequence(r2.sequence)->videoTracks[0].transitions.at(0).type, std::string("smooth_cut"));
            typed.videoTracks[0].transitions[0].type = "page_curl";
            ImportResult r3 = importOtio(fx.p, exportOtio(fx.p, typed));
            QCOMPARE(fx.p.findSequence(r3.sequence)->videoTracks[0].transitions.at(0).type, std::string("cross_dissolve"));
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
