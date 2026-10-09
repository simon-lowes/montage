#include "MarkerList.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <sstream>

#include "History.h"
#include "MediaLog.h"
#include "ReviewPage.h"

namespace montage {

namespace {

std::string csvField(const std::string& v) {
    if (v.find_first_of(",\"\n\r") == std::string::npos) return v;
    std::string q = "\"";
    for (char c : v) q += c == '"' ? std::string("\"\"") : std::string(1, c);
    return q + "\"";
}

std::vector<std::string> csvRow(const std::string& line, char sep) {
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quoted) {
            if (c == '"' && i + 1 < line.size() && line[i + 1] == '"') cur += '"', ++i;
            else if (c == '"') quoted = false;
            else cur += c;
        } else if (c == '"') {
            quoted = true;
        } else if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else if (c != '\r') {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// Avid's eight locator colours, and Resolve's marker colours, for Montage's labels.
const char* avidColour(int label) {
    static const char* names[] = {"red", "magenta", "blue", "cyan", "magenta", "blue", "green", "magenta", "yellow", "yellow", "white", "red"};
    return names[std::clamp(label, 0, 11)];
}
const char* resolveColour(int label) {
    static const char* names[] = {"Blue", "Purple", "Blue", "Cyan", "Lavender", "Sky", "Green", "Rose", "Yellow", "Lemon", "Sand", "Red"};
    return names[std::clamp(label, 0, 11)];
}
int labelFor(const std::string& colour) {
    const std::string c = lower(colour);
    for (int i = 1; i < labelCount(); ++i)
        if (lower(labelName(i)) == c) return i;
    static const std::pair<const char*, int> others[] = {{"red", 11},    {"green", 6}, {"blue", 2},    {"cyan", 3},   {"magenta", 7},
                                                         {"yellow", 9},  {"purple", 1}, {"orange", 8}, {"pink", 7},   {"white", 0},
                                                         {"black", 0},   {"rose", 7},  {"lemon", 9},   {"sand", 10},  {"sky", 5}};
    for (const auto& [name, label] : others)
        if (c == name) return label;
    return 0;
}

}  // namespace

std::string markersToCsv(const Sequence& s) {
    auto tc = [&](FrameTime f) { return formatTimecode(f, s.fps); };
    std::string out = "Marker Name,Description,In,Out,Duration,Marker Type,Color\n";
    for (const Marker& m : s.markers)
        out += csvField(m.name) + "," + csvField(m.comment) + "," + tc(m.t) + "," + tc(m.t + m.duration) + "," + tc(m.duration) + "," +
               (m.chapter ? "Chapter" : "Comment") + "," + (m.color ? labelName(m.color) : "") + "\n";
    return out;
}

std::string markersToAvidLocators(const Sequence& s, const std::string& user) {
    std::string out;
    for (const Marker& m : s.markers) {
        std::string text = m.name;
        if (!m.comment.empty()) text += (text.empty() ? "" : ": ") + m.comment;
        std::replace(text.begin(), text.end(), '\t', ' ');
        std::replace(text.begin(), text.end(), '\n', ' ');
        out += user + "\t" + formatTimecode(m.t, s.fps) + "\tV1\t" + avidColour(m.color) + "\t" + text + "\t1\n";
    }
    return out;
}

std::string markersToResolveEdl(const Sequence& s) {
    std::ostringstream out;
    out << "TITLE: " << s.name << "\n";
    out << "FCM: " << (isDropFrameRate(s.fps) ? "DROP FRAME" : "NON-DROP FRAME") << "\n\n";
    int n = 1;
    char buf[160];
    for (const Marker& m : s.markers) {
        const std::string a = formatTimecode(m.t, s.fps), b = formatTimecode(m.t + 1, s.fps);
        std::snprintf(buf, sizeof buf, "%03d  001      V     C        %s %s %s %s  \n", n++, a.c_str(), b.c_str(), a.c_str(), b.c_str());
        out << buf << " |C:ResolveColor" << resolveColour(m.color) << " |M:" << m.name << " |D:" << std::max<FrameTime>(1, m.duration) << "\n\n";
    }
    return out.str();
}

bool parseMarkerList(const std::string& text, const Sequence& s, std::vector<Marker>& out, std::string* error) {
    if (isReviewNotes(text)) {  // a notes file saved from a review page (core/ReviewPage.h)
        std::vector<ReviewNote> notes;
        if (!parseReviewNotes(text, s.fps, notes, nullptr, error)) return false;
        out = reviewNotesToMarkers(notes);
        return true;
    }
    out.clear();
    std::vector<std::string> lines;
    {
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!trim(line).empty()) lines.push_back(line);
        }
    }
    if (lines.empty()) {
        if (error) *error = "The file is empty";
        return false;
    }
    if (lines[0].size() >= 3 && static_cast<unsigned char>(lines[0][0]) == 0xEF) lines[0] = lines[0].substr(3);  // UTF-8 BOM
    auto time = [&](const std::string& v, FrameTime& f) { return parseTimecode(trim(v), s.fps, f); };
    const char sep = lines[0].find('\t') != std::string::npos && lines[0].find(',') == std::string::npos ? '\t' : ',';
    const std::vector<std::string> header = csvRow(lines[0], sep);
    auto column = [&](std::initializer_list<const char*> names) {
        for (size_t i = 0; i < header.size(); ++i)
            for (const char* n : names)
                if (lower(trim(header[i])) == n) return int(i);
        return -1;
    };
    const int cName = column({"marker name", "name", "title"}), cComment = column({"description", "comment", "comments", "notes", "note"});
    const int cIn = column({"in", "start", "timecode", "source in", "record in"}), cOut = column({"out", "end"});
    const int cDur = column({"duration"}), cType = column({"marker type", "type"}), cColour = column({"color", "colour"});
    if (cIn >= 0) {
        for (size_t i = 1; i < lines.size(); ++i) {
            const std::vector<std::string> f = csvRow(lines[i], sep);
            auto at = [&](int c) { return c >= 0 && c < int(f.size()) ? f[size_t(c)] : std::string(); };
            Marker m;
            if (!time(at(cIn), m.t)) continue;
            FrameTime v = 0;
            if (cDur >= 0 && time(at(cDur), v)) m.duration = std::max<FrameTime>(0, v);
            else if (cOut >= 0 && time(at(cOut), v)) m.duration = std::max<FrameTime>(0, v - m.t);
            m.name = trim(at(cName));
            m.comment = trim(at(cComment));
            m.chapter = lower(trim(at(cType))) == "chapter";
            m.color = labelFor(trim(at(cColour)));
            out.push_back(m);
        }
    } else {
        // Avid locators: user <tab> timecode <tab> track <tab> colour <tab> comment ...
        for (const std::string& line : lines) {
            const std::vector<std::string> f = csvRow(line, '\t');
            Marker m;
            if (f.size() < 2 || !time(f[1], m.t)) continue;
            m.color = f.size() > 3 ? labelFor(trim(f[3])) : 0;
            m.name = f.size() > 4 ? trim(f[4]) : std::string();
            out.push_back(m);
        }
    }
    if (out.empty()) {
        if (error) *error = "No markers with timecodes were found";
        return false;
    }
    // Lists made on timelines that start at 01:00:00:00.
    const FrameTime hour = FrameTime(std::llround(3600 * s.fpsValue()));
    if (s.duration() < hour && std::all_of(out.begin(), out.end(), [&](const Marker& m) { return m.t >= hour; }))
        for (Marker& m : out) m.t -= hour;
    std::sort(out.begin(), out.end(), [](const Marker& a, const Marker& b) { return a.t < b.t; });
    return true;
}

}  // namespace montage
