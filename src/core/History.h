// Montage — undo / redo.
//
// Snapshot based: before an edit the whole Project is copied onto the undo
// stack. The model is metadata only (no pixels), so a snapshot is small and
// this makes every operation undoable without per-command inverse logic.
#pragma once

#include <deque>
#include <string>

#include "Model.h"

namespace montage {

class History {
public:
    explicit History(size_t limit = 500) : limit_(limit) {}

    // Records `before` as the state to return to when undoing `label`.
    void push(const std::string& label, const Project& before);
    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    std::string undoLabel() const { return undo_.empty() ? std::string() : undo_.back().label; }
    std::string redoLabel() const { return redo_.empty() ? std::string() : redo_.back().label; }
    // Swaps `current` with the previous / next state. Returns false if none.
    bool undo(Project& current);
    bool redo(Project& current);
    void clear();
    size_t undoCount() const { return undo_.size(); }
    // Increments whenever the history changes; used for "modified" tracking.
    uint64_t revision() const { return revision_; }
    // Marks a change that is merged into the latest undo step (or not undoable).
    void touch() { ++revision_; }

private:
    struct Entry {
        std::string label;
        Project state;
    };
    std::deque<Entry> undo_;
    std::deque<Entry> redo_;
    size_t limit_;
    uint64_t revision_ = 0;
};

// Timecode helpers (SMPTE, with drop-frame for 29.97 / 59.94).
std::string formatTimecode(FrameTime frame, Rational fps, bool dropFrame = true);
// Parses "HH:MM:SS:FF", "HH:MM:SS;FF", "+FF", plain frame counts, or "N s".
bool parseTimecode(const std::string& text, Rational fps, FrameTime& out);
bool isDropFrameRate(Rational fps);

}  // namespace montage
