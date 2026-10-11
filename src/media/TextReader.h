// Montage — reading text in pictures (OCR): burned-in subtitles become a caption track (Subtitle Edit's OCR, the
// subtitle rip every editor is asked for), slates and title cards are read for logging, and frames can be searched
// for what they say. PP-OCR (PaddlePaddle, Apache-2.0): the PP-OCRv4 mobile detector finds lines of text, and the
// PP-OCRv5 recogniser for English, or for Latin-script languages, reads each one. Runs locally on ONNX Runtime; the
// models (21 MB) are downloaded on first use. Text is read on horizontal lines (as subtitles, slates and titles are).
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Image.h"
#include "ModelFiles.h"
#include "core/Captions.h"
#include "core/Model.h"
#include "core/Slate.h"

class QImage;

namespace montage {

// $MONTAGE_OCR_MODEL names another folder, $MONTAGE_OCR_MODEL_URL a mirror.
const ModelPack& ocrModel();
bool ocrAvailable();  // built with ONNX Runtime

struct TextLine {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // fractions of the picture
    std::string text;
    float confidence = 0;  // the recogniser's mean probability over the characters read
};

// A region of the picture to read, as fractions (the whole picture by default).
struct TextRegion {
    double x0 = 0, y0 = 0, x1 = 1, y1 = 1;
};

class TextReader {
public:
    // English, or the Latin-script recogniser for other languages written in it ("fr", "de", "es"...).
    static std::shared_ptr<TextReader> load(const std::string& language = "en", std::string* error = nullptr);
    ~TextReader();
    // The lines of text in a picture, top to bottom and left to right; pieces of one line are joined.
    std::vector<TextLine> read(const QImage& picture, const TextRegion& region = {}, std::string* error = nullptr) const;
    std::vector<TextLine> read(const Frame16& frame, const TextRegion& region = {}, std::string* error = nullptr) const;
    struct Impl;

private:
    TextReader();
    std::unique_ptr<Impl> d_;
};

// The text of a picture's lines, top to bottom ('\n' between lines), leaving out lines read with less confidence.
std::string textOf(const std::vector<TextLine>& lines, float minConfidence = 0.6f);

// Burned-in subtitles of a video between `from` and `to` (media seconds; `to` <= `from` = to its end), sampled
// `perSecond` times a second in `region`: each reading held while it stays the same (small misreadings joined),
// timed in media frames at `fps`. False with `error` if the video or the models cannot be read, or when cancelled.
bool readBurnedInSubtitles(const std::string& path, double from, double to, const TextRegion& region, Rational fps,
                           std::vector<Caption>& out, const std::string& language = "en", double perSecond = 4,
                           const std::function<void(double)>& progress = {}, const std::atomic<bool>* cancel = nullptr,
                           std::string* error = nullptr);

// The slate on a clapperboard or slate card in the first `seconds` of a video (core/Slate.h slateFromText), read
// twice a second until one is found. False with `error` when none is (or the video cannot be read).
bool readSlateFromPicture(const std::string& path, SlateInfo& out, std::string* error = nullptr, double seconds = 8,
                          const std::atomic<bool>* cancel = nullptr);

}  // namespace montage
