// Montage — logging media: bins, ratings, colour labels, keywords and
// metadata, and smart bins (saved searches) over them. The fields here are
// what the media bin's list view shows as columns and what smart bin rules
// test, so both stay in step.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "Model.h"

namespace montage {

// ---------------------------------------------------------------------------
// Colour labels (shared by clips and media; the app's theme gives the colours).

int labelCount();
const char* labelName(int index);  // "None" for 0 and out-of-range indexes
int labelFromName(const std::string& name);  // case-insensitive; -1 if unknown

// ---------------------------------------------------------------------------
// Bins. A bin is a path of names joined by '/'; "" is the project root.

std::string binParent(const std::string& bin);
std::string binLeaf(const std::string& bin);
std::string joinBin(const std::string& parent, const std::string& leaf);
// True if `bin` is `ancestor` or inside it.
bool binWithin(const std::string& bin, const std::string& ancestor);
// Every bin of the project, sorted, with the parents of nested ones (root excluded).
std::vector<std::string> projectBins(const Project& p);
// A name for a new bin in `parent` that is not taken ("Bin", "Bin 2", ...).
std::string uniqueBinName(const Project& p, const std::string& parent, const std::string& base);
// Edits (each returns whether anything changed).
bool addBin(Project& p, const std::string& bin);
// Renames a bin and everything inside it; false if `to` exists or is inside `from`.
bool renameBin(Project& p, const std::string& from, const std::string& to);
// Removes a bin; its media and bins move up into its parent.
bool removeBin(Project& p, const std::string& bin);
bool moveMediaToBin(Project& p, const std::vector<Id>& media, const std::string& bin);
// Moves a bin (and its contents) into another one, keeping its name.
bool moveBin(Project& p, const std::string& bin, const std::string& into);

// ---------------------------------------------------------------------------
// Keywords.

// Splits "a, b; c" into keywords, trimmed, without duplicates (case-insensitive).
std::vector<std::string> parseKeywords(const std::string& text);
std::string joinKeywords(const std::vector<std::string>& keywords);
// Adds or removes keywords (case-insensitive); returns whether the list changed.
bool addKeywords(std::vector<std::string>& to, const std::vector<std::string>& keywords);
bool removeKeywords(std::vector<std::string>& from, const std::vector<std::string>& keywords);
std::vector<std::string> projectKeywords(const Project& p);  // every keyword in use, sorted

// ---------------------------------------------------------------------------
// Fields.

enum class FieldType { Text, Number, Rating, Label, Kind, Keywords };

struct MediaField {
    const char* key;
    const char* label;
    FieldType type;
    bool editable;  // in the list view and through setMediaField
    bool column;    // offered as a list view column
    bool rule;      // offered in smart bin rules
};

// Every field, in the list view's column order.
const std::vector<MediaField>& mediaFields();
const MediaField* mediaField(const std::string& key);
// The free-form metadata fields (stored in MediaItem::metadata).
const std::vector<std::string>& metadataKeys();

// How many clips use each media item, across every sequence.
std::map<Id, int> mediaUsage(const Project& p);

// The field as text, as the list view shows it.
std::string mediaFieldText(const MediaItem& m, const std::string& key, const std::map<Id, int>* usage = nullptr);
// The field as a number for sorting and comparisons (0 for text fields).
double mediaFieldNumber(const MediaItem& m, const std::string& key, const std::map<Id, int>* usage = nullptr);
// Sets an editable field from text ("4" or "****" for a rating, a label's name,
// "a, b" for keywords); false if the field is not editable or the value is invalid.
bool setMediaField(MediaItem& m, const std::string& key, const std::string& value);

// ---------------------------------------------------------------------------
// Search and smart bins.

// The search box: every word must appear (case-insensitively) in the name,
// keywords, metadata or transcript.
bool mediaMatchesSearch(const MediaItem& m, const std::string& query);

struct RuleOp {
    const char* id;
    const char* label;
    bool needsValue;
};
// The operators a field's rules can use.
std::vector<RuleOp> ruleOps(FieldType type);
bool ruleMatches(const SmartRule& r, const MediaItem& m, const std::map<Id, int>& usage);
bool smartBinMatches(const SmartBin& b, const MediaItem& m, const std::map<Id, int>& usage);
std::vector<Id> smartBinMedia(const Project& p, const SmartBin& b);
SmartBin* findSmartBin(Project& p, Id id);
const SmartBin* findSmartBin(const Project& p, Id id);

}  // namespace montage
