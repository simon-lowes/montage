// Montage — a library of sound effects kept outside any project (Soundminer, BaseHead, Soundly and Resolve's Fairlight
// Sound Library do this): folders of thousands of files, indexed once into a SQLite file in Montage's data folder and
// rescanned as they change. Each sound is found by its name, its Universal Category System category (core/Ucs.h,
// from a UCS file name's CatID), the description its file carries (Broadcast Wave's bext, iXML's USER and ASWG
// fields, RIFF INFO, ID3 and other tags) and, once the CLAP model has listened to it (media/SoundSearch.h), by what
// it sounds like. A file renamed or moved keeps its entry (found again by its size and the bytes at its ends).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

struct sqlite3;

namespace montage {

struct SoundLibraryEntry {
    int64_t id = 0;
    std::string path, name, folder;  // name: the file's, with its extension
    int64_t size = 0;
    double duration = 0;  // seconds
    int channels = 0, sampleRate = 0;
    std::string description;                // what the file says it is
    std::string catId, category, subCategory, fxName, creatorId, sourceId;  // UCS
    std::string keywords;                   // its other tag words (library, keywords, title, genre...)
    bool heard = false;                     // CLAP has listened to it
};

struct SoundLibraryScan {
    int files = 0;                                // sound files in the folders
    int added = 0, updated = 0, moved = 0, removed = 0;
    double seconds = 0;
    std::string error;  // the index could not be written (what was written before stays)
};

// Sound files the library takes, by extension.
bool isSoundLibraryFile(const std::string& path);

class SoundLibrary {
public:
    SoundLibrary();
    ~SoundLibrary();
    SoundLibrary(const SoundLibrary&) = delete;
    SoundLibrary& operator=(const SoundLibrary&) = delete;

    // $MONTAGE_SOUND_LIBRARY, else sound-library.sqlite in Montage's data folder.
    static std::string defaultPath();
    // The library at defaultPath(), opened once and shared (nullptr if it cannot be opened).
    static std::shared_ptr<SoundLibrary> shared();

    bool open(const std::string& file, std::string* error = nullptr);
    bool isOpen() const;
    std::string file() const;

    std::vector<std::string> folders() const;
    bool addFolder(const std::string& folder, std::string* error = nullptr);  // (scan to index it)
    bool removeFolder(const std::string& folder);                              // and its sounds
    // Brings the index up to date with the folders: new files read, changed ones read again, renamed or moved ones
    // found again (keeping what CLAP heard), gone ones dropped.
    SoundLibraryScan scan(const std::function<void(double)>& progress = {}, const std::atomic<bool>* cancel = nullptr);

    int count() const;
    // Sounds matching every word (the start of a word in the name, description, UCS category, subcategory, CatID, FX
    // name or tag words), by name; all of them for no words.
    std::vector<SoundLibraryEntry> search(const std::string& words, size_t limit = 1000) const;
    std::optional<SoundLibraryEntry> entry(int64_t id) const;
    std::optional<SoundLibraryEntry> entryForPath(const std::string& path) const;

    // What it sounds like: CLAP's embeddings of each sound's windows, made for the sounds not yet heard (the model
    // loaded first). Returns how many were heard; -1 if the model cannot be loaded. A sound that cannot be heard (its
    // file not there or unreadable) is left unheard and not counted by unheard() again while the library is open.
    int unheard() const;
    int listen(const std::function<void(int done, int total)>& progress = {}, const std::atomic<bool>* cancel = nullptr,
               std::string* error = nullptr);
    struct Match {
        SoundLibraryEntry entry;
        float score = 0;  // cosine similarity at its best window
        double at = 0;    // seconds: where that window starts
    };
    // The heard sounds nearest a unit-length CLAP embedding (a text query's, SoundModel::text), best first.
    std::vector<Match> searchByMeaning(const std::vector<float>& query, size_t limit = 50) const;
    // The heard sounds most like one, leaving it out.
    std::vector<Match> moreLike(int64_t id, size_t limit = 50) const;

private:
    struct Row;
    bool exec(const char* sql) const;
    bool readFile(const std::string& path, Row& row) const;
    int64_t put(const Row& row, int64_t id);  // id 0: a new entry; returns its id (0 on failure)
    std::vector<SoundLibraryEntry> select(const std::string& where, const std::vector<std::string>& args, size_t limit) const;
    // The heard sounds nearest any of the unit-length queries, at their best window, leaving out one.
    std::vector<Match> nearest(const std::vector<std::vector<float>>& queries, int64_t leaveOut, size_t limit) const;
    sqlite3* db_ = nullptr;
    std::string file_;
    mutable std::recursive_mutex mutex_;  // the connection and its statements
    std::mutex scanMutex_;                // one scan at a time
    std::set<int64_t> unheardable_;       // sounds listen() could not hear
};

}  // namespace montage
