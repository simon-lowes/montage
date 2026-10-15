#include "Teletext.h"

#include <algorithm>
#include <cstring>
#include <deque>

namespace montage {
namespace {

// Hamming 8/4 (ETS 300 706 §8.2): each four bits as a byte with its protection bits.
constexpr uint8_t kHam84[16] = {0x15, 0x02, 0x49, 0x5E, 0x64, 0x73, 0x38, 0x2F, 0xD0, 0xC7, 0x8C, 0x9B, 0xA1, 0xB6, 0xFD, 0xEA};

int unham(uint8_t b) {
    for (int i = 0; i < 16; ++i)
        if (kHam84[i] == b) return i;
    // One bit wrong is corrected: the code word one bit away.
    for (int i = 0; i < 16; ++i)
        for (int bit = 0; bit < 8; ++bit)
            if (uint8_t(kHam84[i] ^ (1 << bit)) == b) return i;
    return -1;
}

uint8_t oddParity(uint8_t c) {
    c &= 0x7F;
    int ones = 0;
    for (int i = 0; i < 7; ++i) ones += (c >> i) & 1;
    return (ones & 1) ? c : uint8_t(c | 0x80);
}

bool parityOk(uint8_t c) {
    int ones = 0;
    for (int i = 0; i < 8; ++i) ones += (c >> i) & 1;
    return ones & 1;
}

// The magazine and row address: magazine 8 is sent as 0.
void setMrag(TeletextPacket& p, int magazine, int row) {
    p[0] = kHam84[(magazine & 7) | ((row & 1) << 3)];
    p[1] = kHam84[(row >> 1) & 15];
}

// A page header: page number (two hex digits), subcode, control bits C4..C14 (bit 0 = C4), 32 spaces to show.
TeletextPacket header(int magazine, int pageHex, int subcode, int control) {
    TeletextPacket p;
    setMrag(p, magazine, 0);
    p[2] = kHam84[pageHex & 15];
    p[3] = kHam84[(pageHex >> 4) & 15];
    p[4] = kHam84[subcode & 15];
    p[5] = kHam84[((subcode >> 4) & 7) | ((control & 1) << 3)];  // S2, C4
    p[6] = kHam84[(subcode >> 8) & 15];
    p[7] = kHam84[((subcode >> 12) & 3) | (((control >> 1) & 3) << 2)];  // S4, C5, C6
    p[8] = kHam84[(control >> 3) & 15];                                // C7..C10
    p[9] = kHam84[(control >> 7) & 15];                                // C11..C14
    for (int i = 10; i < 42; ++i) p[size_t(i)] = oddParity(' ');
    return p;
}

constexpr int kErase = 1, kSubtitle = 4;  // C4, C6

uint32_t nextCodePoint(const std::string& s, size_t& i) {
    const auto b = static_cast<unsigned char>(s[i++]);
    if (b < 0x80) return b;
    int extra = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : b >= 0xC0 ? 1 : 0;
    uint32_t cp = b & (0x3F >> extra);
    for (; extra > 0 && i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80; --extra) cp = (cp << 6) | (s[i++] & 0x3F);
    return extra ? 0xFFFD : cp;
}

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += char(cp);
    } else if (cp < 0x800) {
        out += char(0xC0 | (cp >> 6));
        out += char(0x80 | (cp & 0x3F));
    } else {
        out += char(0xE0 | (cp >> 12));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    }
}

// The English set's own characters where ASCII has others.
struct Special {
    uint8_t code;
    uint32_t cp;
};
constexpr Special kSpecials[] = {{0x23, 0x00A3}, {0x5B, 0x2190}, {0x5C, 0x00BD}, {0x5D, 0x2192}, {0x5E, 0x2191}, {0x5F, 0x0023},
                                 {0x60, 0x2014}, {0x7B, 0x00BC}, {0x7C, 0x2016}, {0x7D, 0x00BE}, {0x7E, 0x00F7}};

// A letter with an accent as the letter (the English set has none), and ASCII the set shows otherwise as near as it can.
char fold(uint32_t cp) {
    static const char* const latin1 =  // U+00C0..U+00FF
        "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTs"
        "aaaaaaaceeeeiiiidnooooo/ouuuuyty";
    if (cp >= 0xC0 && cp <= 0xFF) return latin1[cp - 0xC0];
    if (cp >= 0x100 && cp <= 0x17F) {  // Latin Extended-A: pairs of capital and small, most of them
        static const char* const extA =
            "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiJjJjKkkLlLlLlLlLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";
        return extA[cp - 0x100];
    }
    switch (cp) {
    case 0x2018: case 0x2019: case 0x201A: case 0x2032: case '`': return '\'';
    case 0x201C: case 0x201D: case 0x201E: case 0x2033: return '"';
    case 0x2013: case 0x2212: case '_': case '~': return '-';
    case 0x2026: return '.';
    case '[': case '{': return '(';
    case ']': case '}': return ')';
    case '\\': return '/';
    case '^': return ' ';
    case 0x00A0: case '\t': return ' ';
    default: return 0;
    }
}

// A caption's rows: its lines, each wrapped at word breaks to kTeletextRowChars, at most four.
std::vector<std::string> captionRows(const std::string& text) {
    std::vector<std::string> rows;
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        if (nl == std::string::npos) nl = text.size();
        std::string line = teletextText(text.substr(start, nl - start));
        while (!line.empty() && line.front() == ' ') line.erase(line.begin());
        while (!line.empty() && line.back() == ' ') line.pop_back();
        while (int(line.size()) > kTeletextRowChars) {
            size_t cut = line.rfind(' ', size_t(kTeletextRowChars));
            if (cut == std::string::npos || cut == 0) cut = size_t(kTeletextRowChars);
            rows.push_back(line.substr(0, cut));
            line.erase(0, cut);
            while (!line.empty() && line.front() == ' ') line.erase(line.begin());
        }
        if (!line.empty()) rows.push_back(line);
        start = nl + 1;
    }
    if (rows.size() > 4) rows.resize(4);
    return rows;
}

// A boxed double-height row in white, placed in the 40 columns as the caption is aligned.
TeletextPacket rowPacket(int magazine, int row, const std::string& text, int align) {
    TeletextPacket p;
    setMrag(p, magazine, row);
    std::string cells;
    cells += char(0x07);  // alpha white
    cells += char(0x0D);  // double height
    cells += "\x0B\x0B";  // start box (twice, as subtitles send it)
    cells += text;
    cells += "\x0A\x0A";  // end box
    const int spare = 40 - int(cells.size());
    const int left = align == kCaptionLeft ? 0 : align == kCaptionRight ? spare : spare / 2;
    std::string line(size_t(left), ' ');
    line += cells;
    line.resize(40, ' ');
    for (int i = 0; i < 40; ++i) p[size_t(2 + i)] = oddParity(static_cast<uint8_t>(line[size_t(i)]));
    return p;
}

int pageHexOf(int page) {
    const int units = page % 10, tens = (page / 10) % 10;
    return (tens << 4) | units;
}

}  // namespace

std::string teletextText(const std::string& utf8) {
    std::string out;
    size_t i = 0;
    while (i < utf8.size()) {
        const uint32_t cp = nextCodePoint(utf8, i);
        char c = 0;
        for (const Special& s : kSpecials)
            if (s.cp == cp) c = char(s.code);
        if (!c && cp == 0x2015) c = 0x60;
        if (!c && cp >= 0x20 && cp < 0x7F && !(cp == '#' || cp == '[' || cp == '\\' || cp == ']' || cp == '^' || cp == '_' || cp == '`' ||
                                               cp == '{' || cp == '}' || cp == '~'))
            c = char(cp);
        if (!c && cp == '|') c = 0x7C;
        if (!c) c = fold(cp);
        if (!c) c = '?';
        out += c;
    }
    return out;
}

std::string teletextToUtf8(const std::string& teletext) {
    std::string out;
    for (char ch : teletext) {
        const auto c = static_cast<uint8_t>(ch & 0x7F);
        uint32_t cp = c;
        for (const Special& s : kSpecials)
            if (s.code == c) cp = s.cp;
        if (c == 0x7F) cp = 0x25A0;
        if (c < 0x20) cp = ' ';
        appendUtf8(out, cp);
    }
    return out;
}

std::vector<std::vector<TeletextPacket>> teletextSubtitleFields(const std::vector<Caption>& captions, Rational fps, int64_t frames,
                                                                int page, bool interlaced) {
    (void)fps;
    page = std::clamp(page, 100, 899);
    const int magazine = (page / 100) % 8;  // (magazine 8 is sent as 0)
    const int pageHex = pageHexOf(page);
    const int perFrame = interlaced ? 2 : 1;
    std::vector<std::vector<TeletextPacket>> fields(size_t(std::max<int64_t>(frames, 0) * perFrame));
    std::vector<Caption> sorted = captions;
    std::sort(sorted.begin(), sorted.end(), [](const Caption& a, const Caption& b) { return a.start < b.start; });
    // What changes at each frame: a caption to show (its index) or the page to clear (-1).
    std::vector<std::pair<int64_t, int>> changes;
    changes.push_back({0, -1});  // a clear at the start, as subtitled programmes begin
    for (size_t i = 0; i < sorted.size(); ++i) {
        const Caption& c = sorted[i];
        if (c.end <= c.start || c.end <= 0 || c.start >= frames) continue;
        changes.push_back({std::max<int64_t>(c.start, 0), int(i)});
        const bool next = i + 1 < sorted.size() && sorted[i + 1].start <= c.end && sorted[i + 1].end > sorted[i + 1].start;
        if (!next && c.end < frames) changes.push_back({c.end, -1});
    }
    std::stable_sort(changes.begin(), changes.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::deque<TeletextPacket> queue;
    size_t next = 0;
    const TeletextPacket dummy = header(magazine, 0xFF, 0x3F7E, 0);
    for (int64_t f = 0; f < frames; ++f) {
        // (Only the last change at a frame counts: a caption starting where another clears.)
        int show = -2;
        while (next < changes.size() && changes[next].first <= f) show = changes[next++].second;
        if (show != -2) {
            queue.clear();  // whatever had not gone yet is out of date
            queue.push_back(header(magazine, pageHex, 0, kErase | kSubtitle));
            if (show >= 0) {
                const Caption& c = sorted[size_t(show)];
                const std::vector<std::string> rows = captionRows(c.text);
                const int n = int(rows.size());
                // Double-height rows take two: from row 22 up at the bottom, from row 2 down at the top, about row 12
                // in the middle.
                const int first = c.vertical == kCaptionTop ? 2 : c.vertical == kCaptionMiddle ? 12 - (n - 1) : 22 - 2 * (n - 1);
                for (int r = 0; r < n; ++r) queue.push_back(rowPacket(magazine, first + 2 * r, rows[size_t(r)], c.align));
            }
        }
        for (int k = 0; k < perFrame; ++k) {
            std::vector<TeletextPacket>& out = fields[size_t(f * perFrame + k)];
            while (!queue.empty() && out.size() < 5) {
                out.push_back(queue.front());
                queue.pop_front();
            }
            // The dummy header ends the page sent (and keeps packets coming, as OP-47 asks); a page still being sent
            // goes on in the next field, and one filling this field ends with the next field's dummy.
            if (queue.empty() && out.size() < 5) out.push_back(dummy);
        }
    }
    return fields;
}

std::vector<uint8_t> op47Sdp(const std::vector<TeletextPacket>& packets, uint16_t sequence, bool field1) {
    std::vector<uint8_t> d{0x51, 0x15, 0, 0x02};
    const size_t n = std::min<size_t>(packets.size(), 5);
    for (size_t i = 0; i < 5; ++i) {
        // Lines from 21 back (the last packet on line 21, where OP-47 puts the dummy header), in this field.
        const int line = 21 - int(n) + 1 + int(i);
        d.push_back(i < n ? uint8_t((field1 ? 0x80 : 0) | line) : 0);
    }
    for (size_t i = 0; i < n; ++i) {
        d.insert(d.end(), {0x55, 0x55, 0x27});
        d.insert(d.end(), packets[i].begin(), packets[i].end());
    }
    d.push_back(0x74);
    d.push_back(uint8_t(sequence >> 8));
    d.push_back(uint8_t(sequence & 0xFF));
    d.push_back(0);
    d[2] = uint8_t(d.size());
    unsigned sum = 0;
    for (uint8_t v : d) sum += v;
    d.back() = uint8_t((256 - (sum & 0xFF)) & 0xFF);
    return d;
}

bool parseOp47Sdp(const uint8_t* data, size_t size, std::vector<TeletextPacket>& out) {
    if (size < 13 || data[0] != 0x51 || data[1] != 0x15 || data[3] != 0x02) return false;
    const size_t len = data[2];
    if (len > size || len < 13) return false;
    unsigned sum = 0;
    for (size_t i = 0; i < len; ++i) sum += data[i];
    if ((sum & 0xFF) != 0) return false;
    size_t at = 9;
    for (int i = 0; i < 5; ++i) {
        if (!(data[4 + i] & 0x1F)) continue;
        if (at + 45 > len - 4) return false;
        if (data[at] != 0x55 || data[at + 1] != 0x55 || data[at + 2] != 0x27) return false;
        TeletextPacket p;
        std::memcpy(p.data(), data + at + 3, 42);
        out.push_back(p);
        at += 45;
    }
    return data[at] == 0x74;
}

bool teletextSubtitles(const std::vector<std::vector<TeletextPacket>>& fields, Rational fps, int page, std::vector<Caption>& out,
                       bool interlaced, std::string* error) {
    (void)fps;
    const int magazine = (page / 100) % 8;
    const int pageHex = pageHexOf(page);
    const int perFrame = interlaced ? 2 : 1;
    bool seen = false, collecting = false;
    std::array<std::string, 25> rows{}, shown{};  // the page being received, and the one on screen
    struct Showing {
        std::string text;
        int64_t start = 0;
        int firstRow = 0, align = kCaptionCentre;
    } current;
    // The boxed text on screen, its first row and alignment.
    auto boxed = [&](const std::array<std::string, 25>& page, int& firstRow, int& align) {
        std::string text;
        firstRow = 0;
        align = kCaptionCentre;
        for (int r = 1; r < 25; ++r) {
            const std::string& row = page[size_t(r)];
            if (row.empty()) continue;
            std::string line;
            bool inBox = false;
            int startCol = -1, endCol = -1;
            for (size_t i = 0; i < row.size(); ++i) {
                const char c = row[i];
                if (c == 0x0B) {
                    inBox = true;
                    if (startCol < 0) startCol = int(i);
                } else if (c == 0x0A) {
                    if (inBox) endCol = int(i);
                    inBox = false;
                } else if (inBox && uint8_t(c) >= 0x20) {
                    line += c;
                }
            }
            while (!line.empty() && line.back() == ' ') line.pop_back();
            while (!line.empty() && line.front() == ' ') line.erase(line.begin());
            if (line.empty()) continue;
            if (!firstRow) {
                firstRow = r;
                // Where the box sits: against the left edge, the right, or about the middle.
                const int before = std::max(0, startCol - 3), after = endCol < 0 ? 0 : 39 - endCol - 1;
                align = before <= 1 && after > 4 ? kCaptionLeft : after <= 1 && before > 4 ? kCaptionRight : kCaptionCentre;
            }
            if (!text.empty()) text += '\n';
            text += teletextToUtf8(line);
        }
        return text;
    };
    auto change = [&](int64_t frame) {
        int firstRow = 0, align = kCaptionCentre;
        const std::string text = boxed(shown, firstRow, align);
        if (text == current.text) return;
        if (!current.text.empty() && frame > current.start) {
            Caption c;
            c.start = current.start;
            c.end = frame;
            c.text = current.text;
            c.vertical = current.firstRow && current.firstRow < 12 ? kCaptionTop : kCaptionBottom;
            c.align = current.align;
            out.push_back(std::move(c));
        }
        current = {text, frame, firstRow, align};
    };
    int64_t frame = 0;
    for (size_t fi = 0; fi < fields.size(); ++fi) {
        frame = int64_t(fi) / perFrame;
        for (const TeletextPacket& p : fields[fi]) {
            const int a = unham(p[0]), b = unham(p[1]);
            if (a < 0 || b < 0) continue;
            const int mag = a & 7, row = (a >> 3) | (b << 1);
            if (mag != magazine) continue;
            if (row == 0) {
                // A header ends the page before it in this magazine.
                if (collecting) {
                    shown = rows;
                    change(frame);
                }
                collecting = false;
                const int units = unham(p[2]), tens = unham(p[3]), s2c4 = unham(p[5]);
                if (units < 0 || tens < 0 || s2c4 < 0) continue;
                if (((tens << 4) | units) != pageHex) continue;
                seen = collecting = true;
                if (s2c4 & 8) {  // erase: the page starts empty
                    rows = {};
                    // (An erased page with nothing sent after it clears the screen when it ends.)
                }
            } else if (collecting && row < 25) {
                std::string text(40, ' ');
                for (int i = 0; i < 40; ++i) {
                    const uint8_t c = p[size_t(2 + i)];
                    text[size_t(i)] = parityOk(c) ? char(c & 0x7F) : ' ';
                }
                rows[size_t(row)] = text;
            }
        }
    }
    if (collecting) {
        shown = rows;
        change(frame);
    }
    shown = {};
    change(int64_t(fields.size() + perFrame - 1) / perFrame);
    if (!seen) {
        if (error) *error = "No teletext page " + std::to_string(page);
        return false;
    }
    return true;
}

}  // namespace montage
