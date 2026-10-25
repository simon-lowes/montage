// Montage — delivery paperwork (core/Paperwork.h) written out: the music cue sheet, the dialogue list, the
// as-broadcast script and the CCSL as XLSX, CSV or PDF (the script also as text). What only the picture and the sound
// can tell is found here: the on-screen text, read from the middle of each shot, and each line's in and out, moved
// from the transcript's word times to where the dialogue starts and stops in the mix.
#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "core/Model.h"
#include "core/Paperwork.h"

namespace montage {

enum class PaperworkList { CueSheet, DialogueList, AsBroadcast, Ccsl };
// "cue_sheet", "dialogue_list", "as_broadcast", "ccsl".
const char* paperworkListId(PaperworkList l);
bool paperworkListFromId(const std::string& id, PaperworkList& out);
// "Music Cue Sheet", "Dialogue List", "As-Broadcast Script", "CCSL".
const char* paperworkListName(PaperworkList l);

struct PaperworkOptions {
    std::string gauge;          // the CCSL's film gauge (core/FilmCount.h); "" = the sequence's, else 35 mm 4-perf
    bool onScreenText = false;  // the dialogue lists: read the on-screen text (needs the OCR model)
    bool suggestVocal = false;  // the cue sheet: cues not marked vocal or instrumental told by their vocal stem
    bool refineTimes = true;    // time each line to the dialogue heard
    MusicTagReader tags;        // the cue sheet's file tags; empty = media/MusicTags.h musicTagsFor
    const std::atomic<bool>* cancel = nullptr;
};

// The list as a table (and the as-broadcast script as text). False with `error` when cancelled.
bool buildPaperwork(const Project& p, const Sequence& seq, PaperworkList list, const PaperworkOptions& o, XlsxSheet& table,
                    std::string* script = nullptr, std::string* error = nullptr);

// The list written to `path`, as its extension says: .xlsx, .csv (UTF-8, marked as such), .pdf, or (the dialogue
// lists) .txt: the as-broadcast script as it reads, the dialogue list a line a row, tab between columns. The table
// written goes to `table` and the script to `script` when given.
bool writePaperwork(const Project& p, const Sequence& seq, PaperworkList list, const std::string& path,
                    const PaperworkOptions& o = {}, std::string* error = nullptr, XlsxSheet* table = nullptr,
                    std::string* script = nullptr);

// A table as a PDF: a heading, then the rows (the first the header, repeated on each page), on landscape A4.
bool writeTablePdf(const XlsxSheet& t, const std::string& heading, const std::string& path, std::string* error = nullptr);

// The text on screen in each shot as seen (core/Paperwork.h ccsl), read from its middle frame; a reading carried on
// through shots one after another is one. False with `error` when the OCR model is missing.
bool readOnScreenText(const Project& p, const Sequence& seq, std::vector<OnScreenText>& out, std::string* error = nullptr,
                      const std::atomic<bool>* cancel = nullptr);

// Each line's in and out moved to where the dialogue heard (the mix without its Music and Effects roles) starts and
// stops: the first sound within 30 dB of the line's loudest after a silence of 150 ms, from `reach` seconds before the
// transcript's start (or the previous line's end), and the last before such a silence up to `reach` after its end
// (short of the next line's start).
bool refineDialogueTimes(const Project& p, const Sequence& seq, std::vector<DialogueEntry>& list, double reach = 0.4,
                         std::string* error = nullptr, const std::atomic<bool>* cancel = nullptr);

}  // namespace montage
