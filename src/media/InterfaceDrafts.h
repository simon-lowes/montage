// Montage — first drafts of a translation of Montage's own interface (translations/montage_<language>.ts, Qt
// Linguist's format), from Opus-MT on this computer (media/Translator.h), for a translator to check and finish.
//
// Each text with no translation yet gets one, still marked unfinished so Qt Linguist lists it for review; finished
// translations are left as they are. A draft is kept only when it keeps every placeholder (%1, %L2, %n), and the
// keyboard mnemonic (&File) is put back on its first letter. Rich text and plural forms are left to the translator.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace montage {

struct InterfaceDraftStats {
    int messages = 0;   // in the file
    int finished = 0;   // translated and checked
    int drafted = 0;    // drafts added now
    int existing = 0;   // drafts there before
    int skipped = 0;    // left untranslated (rich text, plural forms, a draft that lost a placeholder)
};

// Fills the untranslated texts of a .ts file's XML with drafts from `translate` (texts in, translations out, in order;
// false on failure). `limit` > 0 drafts at most that many.
bool draftInterfaceTranslation(const std::string& ts, const std::function<bool(const std::vector<std::string>&, std::vector<std::string>&)>& translate,
                               std::string& out, InterfaceDraftStats* stats = nullptr, int limit = 0, std::string* error = nullptr);

// How far a .ts file is translated.
InterfaceDraftStats interfaceTranslationStatus(const std::string& ts);

// The texts the engine gives the interface to show: effects' names, groups, settings and choices, ready-made titles,
// colour spaces, export presets and their descriptions, OpenEXR compressions, ADR cue states. Sorted, each once. The
// interface translates them under the name "Engine" (app/Language.h engine()); app/EngineTexts.cpp lists them for
// lupdate, written by `montage-cli interface-texts`.
std::vector<std::string> engineInterfaceTexts();
// That listing's C++ source.
std::string engineInterfaceTextsSource(const std::vector<std::string>& texts);

}  // namespace montage
