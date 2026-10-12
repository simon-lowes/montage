#include "Cea708.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <deque>
#include <random>
#include <sstream>

#include "History.h"

namespace montage {

namespace {

constexpr double kLeadSeconds = 1.0;  // a caption is loaded into its window this far ahead of its start
constexpr int kMaxColumns = 42;       // 16:9 windows
constexpr int kMaxRows = 4;

// G2 characters reached through EXT1, both ways.
const std::pair<uint8_t, uint32_t> kG2[] = {{0x25, 0x2026}, {0x2A, 0x0160}, {0x2C, 0x0152}, {0x30, 0x2588}, {0x31, 0x2018},
                                            {0x32, 0x2019}, {0x33, 0x201C}, {0x34, 0x201D}, {0x35, 0x2022}, {0x39, 0x2122},
                                            {0x3A, 0x0161}, {0x3C, 0x0153}, {0x3D, 0x2120}, {0x3F, 0x0178}};

std::vector<uint32_t> codePoints(const std::string& s) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < s.size();) {
        const uint8_t b = uint8_t(s[i]);
        const int extra = b < 0x80 ? 0 : (b >> 5) == 6 ? 1 : (b >> 4) == 14 ? 2 : (b >> 3) == 30 ? 3 : -1;
        if (extra < 0 || i + size_t(extra) >= s.size() + (extra ? 0 : 1)) {
            ++i;  // (a broken or cut-off sequence)
            continue;
        }
        uint32_t c = extra == 0 ? b : extra == 1 ? (b & 0x1F) : extra == 2 ? (b & 0x0F) : (b & 0x07);
        for (int k = 1; k <= extra; ++k) c = (c << 6) | (uint8_t(s[i + size_t(k)]) & 0x3F);
        out.push_back(c);
        i += size_t(extra) + 1;
    }
    return out;
}

void putUtf8(std::string& out, uint32_t c) {
    if (c < 0x80) {
        out += char(c);
    } else if (c < 0x800) {
        out += char(0xC0 | (c >> 6));
        out += char(0x80 | (c & 0x3F));
    } else {
        out += char(0xE0 | (c >> 12));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    }
}

// A character as service bytes: G0 (ASCII, 0x7F the music note), G1 (Latin-1), G2 through EXT1, else '?'.
std::vector<uint8_t> charBytes(uint32_t c) {
    if (c >= 0x20 && c < 0x7F) return {uint8_t(c)};
    if (c == 0x266A) return {0x7F};
    if (c >= 0xA0 && c <= 0xFF) return {uint8_t(c)};
    for (const auto& [code, cp] : kG2)
        if (cp == c) return {0x10, code};
    return {uint8_t('?')};
}

uint8_t rgb2(double r, double g, double b) {
    auto q = [](double v) { return uint8_t(std::clamp(int(std::lround(v * 3)), 0, 3)); };
    return uint8_t((q(r) << 4) | (q(g) << 2) | q(b));
}

using Command = std::vector<uint8_t>;

// Loads caption `c` into hidden window `w`: defined where the caption sits, cleared, its colours, then its rows.
std::vector<Command> loadWindow(const Caption& c, int w, const CaptionStyle& style) {
    // Rows longer than a window or more than it holds: wrapped again, as the 608 captions are.
    std::string text = c.text;
    {
        int lines = 1;
        size_t longest = 0, from = 0;
        for (size_t nl; (nl = text.find('\n', from)) != std::string::npos; from = nl + 1, ++lines)
            longest = std::max(longest, codePoints(text.substr(from, nl - from)).size());
        longest = std::max(longest, codePoints(text.substr(from)).size());
        if (longest > size_t(kMaxColumns) || lines > kMaxRows) {
            std::replace(text.begin(), text.end(), '\n', ' ');
            text = wrapCaptionText(text, kMaxColumns, kMaxRows);
        }
    }
    std::vector<std::vector<uint32_t>> rows;
    size_t from = 0;
    while (rows.size() < size_t(kMaxRows)) {
        const size_t nl = text.find('\n', from);
        std::vector<uint32_t> row = codePoints(text.substr(from, nl == std::string::npos ? std::string::npos : nl - from));
        if (row.size() > size_t(kMaxColumns)) row.resize(size_t(kMaxColumns));
        rows.push_back(std::move(row));
        if (nl == std::string::npos) break;
        from = nl + 1;
    }
    size_t columns = 1;
    for (const auto& r : rows) columns = std::max(columns, r.size());
    // Anchor: the window's corner or edge (0-8, top left to bottom right) placed at a share of the screen.
    const int vrow = c.vertical == kCaptionTop ? 0 : c.vertical == kCaptionMiddle ? 1 : 2;
    const int hcol = c.align == kCaptionLeft ? 0 : c.align == kCaptionRight ? 2 : 1;
    const int anchorV = vrow == 0 ? 10 : vrow == 1 ? 50 : std::clamp(int(std::lround(style.position * 100)), 0, 99);
    const int anchorH = hcol == 0 ? 10 : hcol == 1 ? 50 : 90;
    const uint8_t justify = hcol == 0 ? 0 : hcol == 2 ? 1 : 2;
    std::vector<Command> out;
    out.push_back({uint8_t(0x98 + w), 0x18 /* hidden, rows and columns locked, priority 0 */, uint8_t(0x80 | anchorV), uint8_t(anchorH),
                   uint8_t(((vrow * 3 + hcol) << 4) | (rows.size() - 1)), uint8_t(columns - 1), 0x09 /* window and pen style 1 */});
    out.push_back({0x88, uint8_t(1 << w)});  // ClearWindows
    // SetWindowAttributes: a transparent window, left to right, scrolling up, justified as the caption is placed.
    out.push_back({0x97, 0xC0, 0x00, uint8_t((3 << 2) | justify), 0x00});
    // SetPenAttributes: standard size, normal offset; an edge when the style has an outline.
    out.push_back({0x90, 0x05, uint8_t((style.outline > 0 ? 3 : 0) << 3)});
    // SetPenColor: the text solid, the box solid, translucent or transparent, the outline's colour.
    const uint8_t boxOpacity = style.boxOpacity >= 0.9 ? 0 : style.boxOpacity > 0.05 ? 2 : 3;
    out.push_back({0x91, rgb2(style.textR, style.textG, style.textB), uint8_t((boxOpacity << 6) | rgb2(style.boxR, style.boxG, style.boxB)),
                   rgb2(style.outlineR, style.outlineG, style.outlineB)});
    out.push_back({0x92, 0x00, 0x00});  // SetPenLocation: row 0, column 0
    for (size_t r = 0; r < rows.size(); ++r) {
        if (r) out.push_back({0x0D});  // CR
        for (uint32_t ch : rows[r]) out.push_back(charBytes(ch));
    }
    return out;
}

struct Message {
    int64_t frame = 0;
    int order = 0;     // at one frame: hide, then load, then show (so a caption loaded and shown at once is in order)
    int caption = -1;  // whose
    std::vector<Command> commands;
};

struct Queued {
    Command command;
    int loads = -1;  // the last command of this caption's load
    int shows = -1;  // the last command of this caption's display
};

// One DTVCC packet of at most `maxBytes` (header included; even, up to 128) of service 1 blocks: the urgent commands
// first, then the others. False when there is nothing to send or no room.
bool packetize(std::deque<Queued>& urgent, std::deque<Queued>& fifo, size_t maxBytes, int& sequence, std::vector<CcTriplet>& out,
               std::vector<char>& loaded, std::vector<char>& shown) {
    std::vector<uint8_t> data;
    const size_t limit = std::min<size_t>(maxBytes, 128) - 1;
    while (true) {
        std::deque<Queued>& q = !urgent.empty() ? urgent : fifo;
        if (q.empty()) break;
        size_t n = 0, take = 0;
        while (take < q.size() && n + q[take].command.size() <= 31 && data.size() + 1 + n + q[take].command.size() <= limit)
            n += q[take++].command.size();
        if (take == 0) break;
        data.push_back(uint8_t((1 << 5) | n));  // service 1, block size
        for (size_t k = 0; k < take; ++k) {
            const Queued& c = q.front();
            data.insert(data.end(), c.command.begin(), c.command.end());
            if (c.loads >= 0) loaded[size_t(c.loads)] = 1;
            if (c.shows >= 0) shown[size_t(c.shows)] = 1;
            q.pop_front();
        }
    }
    if (data.empty()) return false;
    std::vector<uint8_t> packet{0};
    packet.insert(packet.end(), data.begin(), data.end());
    if (packet.size() % 2) packet.push_back(0);  // a null block header pads it
    const size_t code = packet.size() == 128 ? 0 : packet.size() / 2;
    packet[0] = uint8_t(((sequence++ & 3) << 6) | code);
    for (size_t i = 0; i < packet.size(); i += 2) out.push_back({uint8_t(i == 0 ? 0xFF : 0xFE), packet[i], packet[i + 1]});
    return true;
}

}  // namespace

int ccCountForRate(double fps) { return fps > 0 ? std::clamp(int(600.0 / fps + 1e-6), 1, 31) : 20; }

std::vector<std::vector<CcTriplet>> captionCcData(const std::vector<Caption>& captionsIn, Rational fps, int64_t frames, bool cea608,
                                                 bool cea708, const CaptionStyle& style) {
    std::vector<std::vector<CcTriplet>> out(size_t(std::max<int64_t>(0, frames)));
    if (out.empty()) return out;
    std::vector<Caption> captions = captionsIn;
    normalizeCaptions(captions);
    const double fpsv = fps.valid() ? fps.toDouble() : 30.0;
    const std::vector<Cc608Pair> cc608 = cea608 ? captionsTo608(captions, fps) : std::vector<Cc608Pair>{};
    size_t next608 = 0;
    // Eight windows in turn: a caption's window is free once the one seven before it is shown (hiding all others), so
    // it can be loaded well ahead even when the captions before it are short.
    std::vector<Message> messages;
    if (cea708) {
        const int64_t lead = int64_t(std::llround(kLeadSeconds * fpsv));
        for (size_t i = 0; i < captions.size(); ++i) {
            const Caption& c = captions[i];
            const int w = int(i % 8);
            const int64_t loadAt = std::max<int64_t>(i >= 7 ? captions[i - 7].start : 0, c.start - lead);
            messages.push_back({std::max<int64_t>(0, loadAt), 1, int(i), loadWindow(c, w, style)});
            messages.push_back({c.start, 2, int(i), {{0x8A, uint8_t(0xFF & ~(1 << w))}, {0x89, uint8_t(1 << w)}}});  // Hide the others, Display this
            if (i + 1 == captions.size() || captions[i + 1].start > c.end) messages.push_back({c.end, 0, int(i), {{0x8A, uint8_t(1 << w)}}});
        }
        std::stable_sort(messages.begin(), messages.end(),
                         [](const Message& a, const Message& b) { return a.frame != b.frame ? a.frame < b.frame : a.order < b.order; });
    }
    const int count = ccCountForRate(fpsv);
    std::deque<Queued> urgent, fifo;  // display and hide; loads (and a display waiting on its load)
    std::vector<char> loaded(captions.size(), 0), shown(captions.size(), 0);
    int sequence = 0;
    size_t nextMessage = 0;
    const double lineRate = 30000.0 / 1001.0;
    for (int64_t f = 0; f < frames; ++f) {
        std::vector<CcTriplet>& cc = out[size_t(f)];
        if (cea608) {  // the pairs due in this frame's span of the 29.97 Hz line-21 clock, padding between
            const int64_t from = int64_t(std::floor(double(f) / fpsv * lineRate + 1e-6)), to = int64_t(std::floor(double(f + 1) / fpsv * lineRate + 1e-6));
            for (int64_t slot = from; slot < to; ++slot) {
                uint16_t pair = 0x8080;  // null characters with parity
                while (next608 < cc608.size() && cc608[next608].frame < slot) ++next608;
                if (next608 < cc608.size() && cc608[next608].frame == slot) pair = cc608[next608++].pair;
                cc.push_back({0xFC, uint8_t(pair >> 8), uint8_t(pair & 0xff)});  // valid, field 1
                cc.push_back({0xFD, 0x80, 0x80});                                  // field 2: nulls
            }
        }
        if (!cea708) continue;
        // What is due: a display goes ahead of the loads once its own load is out, a hide once its display is.
        for (; nextMessage < messages.size() && messages[nextMessage].frame <= f; ++nextMessage) {
            Message& m = messages[nextMessage];
            const size_t i = size_t(m.caption);
            const bool now = m.order == 0 ? bool(shown[i]) : m.order == 2 ? bool(loaded[i]) : false;
            std::deque<Queued>& q = now ? urgent : fifo;
            for (size_t k = 0; k < m.commands.size(); ++k) {
                const bool last = k + 1 == m.commands.size();
                q.push_back({std::move(m.commands[k]), last && m.order == 1 ? m.caption : -1, last && m.order == 2 ? m.caption : -1});
            }
        }
        // Packets small enough to go out whole in this frame, so a display is never held up behind a load.
        while (count - int(cc.size()) >= 2 &&
               packetize(urgent, fifo, size_t(count - int(cc.size())) * 2, sequence, cc, loaded, shown)) {
        }
        while (int(cc.size()) < count) cc.push_back({0xFA, 0x00, 0x00});  // not valid: padding
    }
    return out;
}

// ---- Reading -----------------------------------------------------------------------

bool captionsFrom708(const std::vector<std::pair<double, CcTriplet>>& triplets, Rational fps, std::vector<Caption>& out, std::string* error,
                     bool* painted) {
    const double fpsv = fps.valid() ? fps.toDouble() : 30.0;
    struct Window {
        bool defined = false, visible = false;
        std::vector<std::string> rows;
        int row = 0;
        int rowCount = 15;
        int anchorPoint = 7;
    };
    bool paintedOn = false;  // text written into a window on screen (roll-up, paint-on)
    int generation = 0;      // a window put on screen
    std::array<Window, 8> windows;
    int current = 0;
    auto clear = [](Window& w) {
        w.rows.clear();
        w.row = 0;
    };
    auto put = [&](uint32_t c) {
        Window& w = windows[size_t(current)];
        paintedOn = paintedOn || w.visible;
        if (w.rows.size() <= size_t(w.row)) w.rows.resize(size_t(w.row) + 1);
        putUtf8(w.rows[size_t(w.row)], c);
    };
    auto forMask = [&](uint8_t mask, auto&& f) {
        for (int k = 0; k < 8; ++k)
            if (mask & (1 << k)) f(windows[size_t(k)]);
    };
    // Service 1's bytes in one block.
    auto interpret = [&](const uint8_t* b, size_t n) {
        for (size_t j = 0; j < n;) {
            const uint8_t c = b[j++];
            if (c < 0x20) {
                if (c == 0x10 && j < n) {  // EXT1
                    const uint8_t e = b[j++];
                    if (e < 0x20) j += e <= 0x07 ? 0 : e <= 0x0F ? 1 : e <= 0x17 ? 2 : 3;
                    else if (e < 0x80) {
                        uint32_t cp = '?';
                        for (const auto& [code, point] : kG2)
                            if (code == e) cp = point;
                        if (e == 0x20 || e == 0x21) cp = ' ';  // transparent and non-breaking spaces
                        put(cp);
                    } else if (e < 0xA0) {
                        j = e <= 0x87 ? j + 4 : e <= 0x8F ? j + 5 : n;  // C3 (variable-length ones end the block)
                    }
                } else if (c >= 0x11 && c <= 0x17) {
                    j += 1;
                } else if (c >= 0x18) {
                    j += 2;
                } else if (c == 0x0D) {  // CR: the next row, scrolling up past the last
                    Window& w = windows[size_t(current)];
                    if (++w.row >= w.rowCount) {
                        if (!w.rows.empty()) w.rows.erase(w.rows.begin());
                        w.row = w.rowCount - 1;
                    }
                } else if (c == 0x0C) {
                    clear(windows[size_t(current)]);
                } else if (c == 0x08) {
                    Window& w = windows[size_t(current)];
                    if (size_t(w.row) < w.rows.size() && !w.rows[size_t(w.row)].empty()) w.rows[size_t(w.row)].pop_back();
                } else if (c == 0x0E) {
                    Window& w = windows[size_t(current)];
                    if (size_t(w.row) < w.rows.size()) w.rows[size_t(w.row)].clear();
                }
            } else if (c < 0x80) {
                put(c == 0x7F ? 0x266A : c);
            } else if (c < 0xA0) {
                const uint8_t p = j < n ? b[j] : 0;
                switch (c) {
                    case 0x88: forMask(p, clear), ++j; break;
                    case 0x89:
                        forMask(p, [&](Window& w) {
                            if (w.defined && !w.visible) ++generation;
                            w.visible = w.defined;
                        }),
                            ++j;
                        break;
                    case 0x8A: forMask(p, [](Window& w) { w.visible = false; }), ++j; break;
                    case 0x8B:
                        forMask(p, [&](Window& w) {
                            if (w.defined && !w.visible) ++generation;
                            w.visible = w.defined && !w.visible;
                        }),
                            ++j;
                        break;
                    case 0x8C: forMask(p, [&](Window& w) { w = Window{}; }), ++j; break;
                    case 0x8D: ++j; break;
                    case 0x8F: windows = {}; break;
                    case 0x90: j += 2; break;
                    case 0x91: j += 3; break;
                    case 0x92: windows[size_t(current)].row = p & 0x0F, j += 2; break;
                    case 0x97: j += 4; break;
                    default:
                        if (c <= 0x87) current = c - 0x80;
                        else if (c >= 0x98) {
                            if (j + 6 > n) return;
                            current = c - 0x98;
                            Window& w = windows[size_t(current)];
                            if (!w.defined) clear(w);
                            w.defined = true;
                            if ((b[j] & 0x20) && !w.visible) ++generation;
                            w.visible = (b[j] & 0x20) != 0;
                            w.anchorPoint = std::min(8, b[j + 3] >> 4);
                            w.rowCount = (b[j + 3] & 0x0F) + 1;
                            j += 6;
                        }
                }
            } else {
                put(c);  // G1: Latin-1
            }
        }
    };
    // What the windows shown say, and where the first of them sits.
    auto shown = [&](int& anchor) {
        std::string text;
        anchor = 7;
        bool first = true;
        for (const Window& w : windows) {
            if (!w.defined || !w.visible) continue;
            for (std::string row : w.rows) {
                while (!row.empty() && row.back() == ' ') row.pop_back();
                if (row.empty()) continue;
                if (first) anchor = w.anchorPoint;
                first = false;
                if (!text.empty()) text += '\n';
                text += row;
            }
        }
        return text;
    };
    std::vector<Caption> caps;
    std::string onScreen;
    double since = 0;
    int onAnchor = 7, onGeneration = 0;
    auto change = [&](double t) {
        int anchor = 7;
        const std::string now = shown(anchor);
        const bool sameShow = generation == onGeneration;
        if (now == onScreen && sameShow) return;
        onGeneration = generation;
        // Text growing in a window already on screen (paint-on, a roll-up row being typed) stays one caption.
        if (sameShow && !onScreen.empty() && now.size() > onScreen.size() && now.compare(0, onScreen.size(), onScreen) == 0) {
            onScreen = now;
            return;
        }
        if (!onScreen.empty()) {
            Caption c;
            c.start = FrameTime(std::llround(since * fpsv));
            c.end = FrameTime(std::llround(t * fpsv));
            c.text = onScreen;
            c.vertical = onAnchor / 3 == 0 ? kCaptionTop : onAnchor / 3 == 1 ? kCaptionMiddle : kCaptionBottom;
            c.align = onAnchor % 3 == 0 ? kCaptionLeft : onAnchor % 3 == 2 ? kCaptionRight : kCaptionCentre;
            caps.push_back(std::move(c));
        }
        onScreen = now;
        onAnchor = anchor;
        since = t;
    };
    std::vector<uint8_t> packet;
    size_t want = 0;
    double last = 0;
    for (const auto& [t, cc] : triplets) {
        last = t;
        if (!(cc[0] & 0x04)) continue;  // not valid
        const int type = cc[0] & 0x03;
        if (type == 3) {
            packet = {cc[1], cc[2]};
            const size_t code = cc[1] & 0x3F;
            want = code == 0 ? 128 : code * 2;
        } else if (type == 2 && !packet.empty()) {
            packet.push_back(cc[1]);
            packet.push_back(cc[2]);
        } else {
            continue;
        }
        if (packet.size() < want) continue;
        for (size_t i = 1; i < want;) {
            const uint8_t header = packet[i++];
            if (header == 0) break;
            int service = header >> 5;
            const size_t size = header & 0x1F;
            if (service == 7 && i < want) service = packet[i++] & 0x3F;
            if (i + size > want) break;
            if (service == 1) interpret(packet.data() + i, size);
            i += size;
        }
        packet.clear();
        change(t);
    }
    windows = {};
    change(last + 1.0 / fpsv);
    normalizeCaptions(caps);
    if (painted) *painted = paintedOn;
    if (caps.empty()) {
        if (error) *error = "No CTA-708 captions found (service 1)";
        return false;
    }
    out = std::move(caps);
    return true;
}

// ---- MCC ---------------------------------------------------------------------------

namespace {

struct MccRate {
    int num, den;
    const char* name;  // Time Code Rate
    int cdpCode;       // SMPTE 334-2 cdp_frame_rate
};
const MccRate kMccRates[] = {{24000, 1001, "24", 1}, {24, 1, "24", 2},   {25, 1, "25", 3},          {30000, 1001, "30DF", 4},
                             {30, 1, "30", 5},       {50, 1, "50", 6},   {60000, 1001, "60DF", 7},  {60, 1, "60", 8}};

const MccRate& mccRateFor(Rational fps) {
    for (const MccRate& r : kMccRates)
        if (fps.valid() && std::fabs(fps.toDouble() - double(r.num) / r.den) < 1e-3) return r;
    return kMccRates[3];
}

const char* const kMccHeader =
    "File Format=MacCaption_MCC V1.0\n"
    "\n"
    "///////////////////////////////////////////////////////////////////////////////////\n"
    "// Computer Prompting and Captioning Company\n"
    "// Ancillary Data Packet Transfer File\n"
    "//\n"
    "// Permission to generate this format is granted provided that\n"
    "//   1. This ANC Transfer file format is used on an as-is basis and no warranty is given, and\n"
    "//   2. This entire descriptive information text is included in a generated .mcc file.\n"
    "//\n"
    "// General file format:\n"
    "//   HH:MM:SS:FF(tab)[Hexadecimal ANC data in groups of 2 characters]\n"
    "//     Hexadecimal data starts with the Ancillary Data Packet DID (Data ID defined in S291M)\n"
    "//       and concludes with the Check Sum following the User Data Words.\n"
    "//     Each time code line must contain at most one complete ancillary data packet.\n"
    "//     To transfer additional ANC Data successive lines may contain identical time code.\n"
    "//     Time Code Rate=[24, 25, 30, 30DF, 50, 60]\n"
    "//\n"
    "//   ANC data bytes may be represented by one ASCII character according to the following schema:\n"
    "//     G  FAh 00h 00h\n"
    "//     H  2 x (FAh 00h 00h)\n"
    "//     I  3 x (FAh 00h 00h)\n"
    "//     J  4 x (FAh 00h 00h)\n"
    "//     K  5 x (FAh 00h 00h)\n"
    "//     L  6 x (FAh 00h 00h)\n"
    "//     M  7 x (FAh 00h 00h)\n"
    "//     N  8 x (FAh 00h 00h)\n"
    "//     O  9 x (FAh 00h 00h)\n"
    "//     P  FBh 80h 80h\n"
    "//     Q  FCh 80h 80h\n"
    "//     R  FDh 80h 80h\n"
    "//     S  96h 69h\n"
    "//     T  61h 01h\n"
    "//     U  E1h 00h 00h 00h\n"
    "//     Z  00h\n"
    "//\n"
    "///////////////////////////////////////////////////////////////////////////////////\n"
    "\n";

// Hex with MCC's one-letter runs.
std::string mccHex(const std::vector<uint8_t>& b) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    auto at = [&](size_t i, std::initializer_list<uint8_t> seq) {
        if (i + seq.size() > b.size()) return false;
        size_t k = 0;
        for (uint8_t v : seq)
            if (b[i + k++] != v) return false;
        return true;
    };
    for (size_t i = 0; i < b.size();) {
        int runs = 0;
        while (runs < 9 && at(i + size_t(runs) * 3, {0xFA, 0x00, 0x00})) ++runs;
        if (runs) {
            out += char('G' + runs - 1);
            i += size_t(runs) * 3;
        } else if (at(i, {0xFB, 0x80, 0x80})) out += 'P', i += 3;
        else if (at(i, {0xFC, 0x80, 0x80})) out += 'Q', i += 3;
        else if (at(i, {0xFD, 0x80, 0x80})) out += 'R', i += 3;
        else if (at(i, {0x96, 0x69})) out += 'S', i += 2;
        else if (at(i, {0x61, 0x01})) out += 'T', i += 2;
        else if (at(i, {0xE1, 0x00, 0x00, 0x00})) out += 'U', i += 4;
        else if (b[i] == 0x00) out += 'Z', ++i;
        else {
            out += hex[b[i] >> 4];
            out += hex[b[i] & 15];
            ++i;
        }
    }
    return out;
}

bool mccBytes(const std::string& text, std::vector<uint8_t>& out) {
    out.clear();
    auto nibble = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = char(std::toupper(static_cast<unsigned char>(text[i])));
        if (std::isspace(static_cast<unsigned char>(c))) continue;
        if (c >= 'G' && c <= 'O') {
            for (int k = 0; k <= c - 'G'; ++k) out.insert(out.end(), {0xFA, 0x00, 0x00});
        } else if (c == 'P') out.insert(out.end(), {0xFB, 0x80, 0x80});
        else if (c == 'Q') out.insert(out.end(), {0xFC, 0x80, 0x80});
        else if (c == 'R') out.insert(out.end(), {0xFD, 0x80, 0x80});
        else if (c == 'S') out.insert(out.end(), {0x96, 0x69});
        else if (c == 'T') out.insert(out.end(), {0x61, 0x01});
        else if (c == 'U') out.insert(out.end(), {0xE1, 0x00, 0x00, 0x00});
        else if (c == 'Z') out.push_back(0x00);
        else {
            const int hi = nibble(c), lo = i + 1 < text.size() ? nibble(char(std::toupper(static_cast<unsigned char>(text[i + 1])))) : -1;
            if (hi < 0 || lo < 0) return false;
            out.push_back(uint8_t((hi << 4) | lo));
            ++i;
        }
    }
    return true;
}

}  // namespace

std::string captionsToMcc(const std::vector<Caption>& captions, Rational fps, const CaptionStyle& style) {
    const MccRate& mr = mccRateFor(fps);
    const Rational rate{mr.num, mr.den};
    const double from = fps.valid() ? fps.toDouble() : 30.0, to = rate.toDouble();
    std::vector<Caption> timed = captions;
    int64_t frames = 1;
    for (Caption& c : timed) {
        c.start = FrameTime(std::llround(double(c.start) / from * to));
        c.end = FrameTime(std::llround(double(c.end) / from * to));
        frames = std::max<int64_t>(frames, c.end + int64_t(std::lround(to)));
    }
    const std::vector<std::vector<CcTriplet>> data = captionCcData(timed, rate, frames, true, true, style);
    std::string out = kMccHeader;
    if (std::string(mr.name) == "60DF") out.replace(out.find("V1.0"), 4, "V2.0");  // (V1.0 has no 59.94 drop frame)
    {
        std::random_device rd;
        char uuid[64];
        std::snprintf(uuid, sizeof uuid, "%08X-%04X-4%03X-%04X-%04X%08X", rd(), rd() & 0xFFFF, rd() & 0xFFF, (rd() & 0x3FFF) | 0x8000,
                      rd() & 0xFFFF, rd());
        const std::time_t now = std::time(nullptr);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &now);
#else
        localtime_r(&now, &tm);
#endif
        char date[64], clock[32];
        std::strftime(date, sizeof date, "%A, %B %d, %Y", &tm);
        std::strftime(clock, sizeof clock, "%H:%M:%S", &tm);
        out += std::string("UUID=") + uuid + "\nCreation Program=Montage\nCreation Date=" + date + "\nCreation Time=" + clock +
               "\nTime Code Rate=" + mr.name + "\n\n";
    }
    const bool drop = isDropFrameRate(rate);
    for (int64_t f = 0; f < frames; ++f) {
        const std::vector<CcTriplet>& cc = data[size_t(f)];
        const uint16_t seq = uint16_t(f & 0xFFFF);
        std::vector<uint8_t> cdp{0x96, 0x69, 0, uint8_t((mr.cdpCode << 4) | 0x0F), 0x43, uint8_t(seq >> 8), uint8_t(seq & 0xFF), 0x72,
                                 uint8_t(0xE0 | cc.size())};
        for (const CcTriplet& t : cc) cdp.insert(cdp.end(), t.begin(), t.end());
        cdp.insert(cdp.end(), {0x74, uint8_t(seq >> 8), uint8_t(seq & 0xFF), 0});
        cdp[2] = uint8_t(cdp.size());
        unsigned sum = 0;
        for (uint8_t v : cdp) sum += v;
        cdp.back() = uint8_t((256 - (sum & 0xFF)) & 0xFF);  // the packet's bytes add up to 0
        std::vector<uint8_t> anc{0x61, 0x01, uint8_t(cdp.size())};
        anc.insert(anc.end(), cdp.begin(), cdp.end());
        unsigned ancSum = 0;
        for (uint8_t v : anc) ancSum += v;
        anc.push_back(uint8_t(ancSum & 0xFF));
        out += formatTimecode(f, rate, drop) + "\t" + mccHex(anc) + "\n";
    }
    return out;
}

bool parseMcc(const std::string& text, Rational fps, std::vector<Caption>& out, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    const size_t skip = text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
    if (text.compare(skip, 26, "File Format=MacCaption_MCC") != 0) return fail("Not a MacCaption MCC file");
    Rational rate{30000, 1001};
    std::vector<std::pair<double, CcTriplet>> triplets;
    std::vector<std::pair<double, uint16_t>> pairs;
    std::istringstream lines(text.substr(skip));
    std::string line;
    std::vector<uint8_t> bytes;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("Time Code Rate=", 0) == 0) {
            const std::string r = line.substr(15);
            for (const MccRate& m : kMccRates)
                if (r == m.name) {
                    rate = {m.num, m.den};
                    if (r == "24") rate = {24, 1};
                    break;
                }
            // Timecodes label frames: at the rate asked for when it is the nominal one (23.976 as "24", 29.97 as "30").
            if (fps.valid() && r.find("DF") == std::string::npos && std::lround(fps.toDouble()) == std::lround(rate.toDouble())) rate = fps;
            continue;
        }
        const size_t tab = line.find('\t');
        if (tab == std::string::npos || line.rfind("//", 0) == 0) continue;
        std::string tc = line.substr(0, tab);
        if (const size_t dot = tc.find('.'); dot != std::string::npos && dot > 8) tc.erase(dot);  // (a field flag)
        FrameTime frame = 0;
        if (!parseTimecode(tc, rate, frame) || !mccBytes(line.substr(tab + 1), bytes)) continue;
        size_t at = bytes.size() >= 3 && bytes[0] == 0x61 && bytes[1] == 0x01 ? 3 : 0;
        if (at + 9 > bytes.size() || bytes[at] != 0x96 || bytes[at + 1] != 0x69) continue;
        const uint8_t flags = bytes[at + 4];
        size_t i = at + 7;
        if (flags & 0x80) i += 5;  // time code section
        if (!(flags & 0x40) || i + 2 > bytes.size() || bytes[i] != 0x72) continue;
        const size_t count = bytes[i + 1] & 0x1F;
        i += 2;
        const double t = double(frame) / rate.toDouble();
        int k = 0;
        for (size_t n = 0; n < count && i + 3 <= bytes.size(); ++n, i += 3) {
            const CcTriplet cc{bytes[i], bytes[i + 1], bytes[i + 2]};
            triplets.push_back({t, cc});
            if ((cc[0] & 0x04) && (cc[0] & 0x03) == 0) pairs.push_back({t + double(k++) * 1001.0 / 30000.0, uint16_t((cc[1] << 8) | cc[2])});
        }
    }
    std::string why;
    std::vector<Caption> from708;
    bool painted = false;
    const bool have708 = captionsFrom708(triplets, fps, from708, &why, &painted);
    std::stable_sort(pairs.begin(), pairs.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    // Roll-up and paint-on read better from the 608, which knows them.
    if (have708 && !painted) return out = std::move(from708), true;
    if (!pairs.empty() && captionsFrom608(pairs, fps, out, &why)) return true;
    if (have708) return out = std::move(from708), true;
    return fail("No captions found in the MCC file");
}

}  // namespace montage
