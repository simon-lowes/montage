// Montage — image (bitmap) subtitles in and out. Blu-ray (PGS), DVB and DVD (VobSub) subtitles are pictures, not
// text: FFmpeg decodes them into bitmaps with their times and places, each is flattened to dark text on light and
// read by OCR (media/TextReader.h), and becomes a caption with its time and place on a new caption track in the
// stream's language, the track taking the subtitles' colour and size; words read with little confidence that the
// spell checker does not know are listed for review. Out, each caption is drawn as the track's style draws it
// (render/Compositor.h captionImage) and reduced to the format's colours: PGS (presentation, window, palette and
// run-length object segments, 255 colours) written by Montage itself, as .sup or in Matroska; DVB through FFmpeg's
// dvbsub encoder (16 colours), in MPEG-TS or Matroska; DVD through dvdsub (4 colours from a 16-colour palette) as
// .idx and .sub; and the IMSC 1.1 Image profile, a PNG per caption with a TTML document that places them (also in
// IMF packages, render/Imf.h).
#pragma once

#include <QImage>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/Captions.h"
#include "core/Model.h"

namespace montage {

enum class ImageSubtitleFormat { Pgs, Dvb, Vobsub, ImscImage };
// "pgs", "dvb", "vobsub", "imsc_image".
const char* imageSubtitleFormatId(ImageSubtitleFormat f);
bool imageSubtitleFormatFromId(const std::string& id, ImageSubtitleFormat& out);
// The format a file extension names (".sup", ".idx" or ".sub", ".ts" or ".m2ts"), false for others (Matroska and TTML
// hold several).
bool imageSubtitleFormatFor(const std::string& extension, ImageSubtitleFormat& out);

// ---- In --------------------------------------------------------------------------------------------------------

struct ImageSubtitleStream {
    int index = 0;          // the stream's index in the file
    std::string codec;      // "pgs", "dvb", "dvd"
    std::string language;   // ISO 639-1 when the file says ("" when not)
    std::string title;
};
// The image subtitle streams of a file (a video, .sup, .idx, .mks, .ts...).
std::vector<ImageSubtitleStream> imageSubtitleStreams(const std::string& path);

// A subtitle as its pictures came: when (seconds of the file's time) and the picture on the frame.
struct SubtitlePicture {
    double start = 0, end = 0;
    int x = 0, y = 0;  // where the picture goes on the frame
    QImage image;      // ARGB32, not premultiplied
};
struct DecodedSubtitles {
    int width = 0, height = 0;  // the frame the pictures are placed on
    std::string codec, language;
    std::vector<SubtitlePicture> pictures;  // in time order; several at once are joined into one
};
// Decodes stream `stream` (-1: the first image subtitle stream) of `path` through FFmpeg, or an IMSC 1.1 Image
// document (.ttml / .xml) and the PNGs it names. A subtitle lasts until it is cleared, its stated end, or the next.
bool decodeImageSubtitles(const std::string& path, DecodedSubtitles& out, int stream = -1, std::string* error = nullptr,
                          const std::atomic<bool>* cancel = nullptr);
// An IMSC 1.1 Image document's pictures, `image(name)` giving each PNG it names (beside it, or in an IMF track file).
bool decodeImscImage(const std::string& xml, const std::function<QByteArray(const std::string&)>& image, DecodedSubtitles& out,
                     std::string* error = nullptr);

struct OcrReview {
    size_t caption = 0;   // its index in the track
    std::string word;
    std::vector<std::string> suggestions;
    float confidence = 0;  // the recogniser's, for the line the word is in
};
struct ImageSubtitleImport {
    CaptionTrack track;               // named after the file, in the stream's language, styled as the pictures look
    std::vector<OcrReview> review;    // words to check
    std::string codec;
    int pictures = 0;
    int unread = 0;                   // pictures in which no text was found
};
// The subtitles of `path` (as decodeImageSubtitles) read by OCR into a caption track, timed in frames at `fps` of the
// file's time. `language` overrides the stream's ("" keeps it; English when neither says). False with `error` when
// there are none, the OCR model is missing, or when cancelled.
bool readImageSubtitles(const std::string& path, Rational fps, ImageSubtitleImport& out, int stream = -1,
                        const std::string& language = {}, const std::function<void(double)>& progress = {},
                        const std::atomic<bool>* cancel = nullptr, std::string* error = nullptr);
// The same for pictures already decoded (the track named "Subtitles").
bool readSubtitlePictures(const DecodedSubtitles& pictures, Rational fps, ImageSubtitleImport& out, const std::string& language = {},
                          const std::function<void(double)>& progress = {}, const std::atomic<bool>* cancel = nullptr,
                          std::string* error = nullptr);

// ---- Out -------------------------------------------------------------------------------------------------------

// Each caption as the track's style draws it on a `width` x `height` frame (still: word animations left out), cut
// to what is drawn: its picture (ARGB32, not premultiplied) and where it goes.
struct CaptionPicture {
    FrameTime start = 0, end = 0;
    int x = 0, y = 0;
    QImage image;
};
std::vector<CaptionPicture> captionPictures(const CaptionTrack& track, int width, int height);

// A picture in at most `colours` colours, index 0 transparent (median cut on colour and opacity). `alphaSteps`
// (> 1) rounds the opacity to that many levels.
struct IndexedPicture {
    int width = 0, height = 0;
    std::vector<uint8_t> pixels;
    std::vector<uint32_t> palette;  // ARGB, not premultiplied; [0] = transparent
};
IndexedPicture reduceColours(const QImage& argb, int colours, int alphaSteps = 0);

// The caption track's subtitles coded as `format` (not IMSC) for a `width` x `height` frame, as packets in time
// order with times in 90 kHz units from frame 0 at `fps` (captions from `from` to `to` frames, shifted to start at
// 0): PGS display sets (a set clears each caption), DVB subtitles (an empty one clears each), DVD subpictures (each
// with its own end). `extradata` gets what the stream's header needs (DVD: the .idx header; DVB: the page ids).
struct SubtitlePacket {
    int64_t pts = 0;
    int64_t duration = 0;  // 0 for a clearing packet
    std::vector<uint8_t> data;
};
bool encodeImageSubtitles(const CaptionTrack& track, Rational fps, ImageSubtitleFormat format, int width, int height,
                          std::vector<SubtitlePacket>& out, std::vector<uint8_t>* extradata = nullptr,
                          std::string* error = nullptr, FrameTime from = 0, FrameTime to = -1);

// An IMSC 1.1 Image document for the pictures (times from `fps` frames), each image referred to as `names[i]` (file
// names beside the document, or "urn:uuid:..." in an IMF track file).
std::string imscImageDocument(const std::vector<CaptionPicture>& pictures, const std::vector<std::string>& names,
                              Rational fps, int width, int height, const std::string& language);
// What is wrong with an IMSC 1.1 Image document (its profile, the root extent, every region on the frame, each div
// timed and with an image that `image(name)` gives as a PNG no larger than its region): empty when nothing is.
std::vector<std::string> checkImscImage(const std::string& xml, const std::function<QByteArray(const std::string&)>& image,
                                        int width = 0, int height = 0);
bool isImscImage(const std::string& xml);

// The caption track written to `path` as `format`, its frame `width` x `height` (0: the sequence's): PGS as .sup or
// Matroska (.mks, .mkv); DVB as MPEG-TS (.ts, .m2ts) or Matroska; DVD as .idx with its .sub beside it (or Matroska);
// IMSC 1.1 Image as a TTML document (.ttml, .xml) with "<name>-0001.png"... beside it. False with `error`.
struct ImageSubtitleResult {
    int subtitles = 0;
    int colours = 0;  // the most used in a picture
    std::vector<std::string> files;
};
bool writeImageSubtitles(const CaptionTrack& track, const Sequence& seq, const std::string& path, ImageSubtitleFormat format,
                         std::string* error = nullptr, int width = 0, int height = 0, ImageSubtitleResult* result = nullptr);

// ISO 639-1 for a language as files tag it ("eng", "fre", "deu", "en"...): "" when unknown.
std::string languageFromTag(const std::string& tag);

}  // namespace montage
