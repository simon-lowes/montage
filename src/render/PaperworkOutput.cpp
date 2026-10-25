#include "PaperworkOutput.h"

#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QImage>
#include <QPageLayout>
#include <QPageSize>
#include <QPdfWriter>
#include <QSaveFile>
#include <QTextDocument>
#include <algorithm>
#include <cmath>

#include "core/EditOps.h"
#include "media/MusicTags.h"
#include "media/TextReader.h"
#include "render/Compositor.h"

namespace montage {

namespace {

bool fail(std::string* error, const std::string& why) {
    if (error) *error = why;
    return false;
}

bool cancelled(const std::atomic<bool>* cancel) { return cancel && cancel->load(); }

bool writeBytes(const std::string& path, const std::string& bytes, std::string* error) {
    QSaveFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::WriteOnly) || f.write(bytes.data(), qint64(bytes.size())) != qint64(bytes.size()) || !f.commit())
        return fail(error, "Could not write " + path);
    return true;
}

QString html(const std::string& s) { return QString::fromStdString(s).toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")); }

bool printPdf(QTextDocument& doc, const std::string& title, const std::string& path, std::string* error) {
    {
        QPdfWriter writer(QString::fromStdString(path));
        writer.setPageLayout(QPageLayout(QPageSize(QPageSize::A4), QPageLayout::Landscape, QMarginsF(12, 12, 12, 12), QPageLayout::Millimeter));
        writer.setTitle(QString::fromStdString(title));
        writer.setCreator(QStringLiteral("Montage"));
        doc.print(&writer);
    }
    if (QFileInfo(QString::fromStdString(path)).size() == 0) return fail(error, "Could not write " + path);
    return true;
}

}  // namespace

const char* paperworkListId(PaperworkList l) {
    switch (l) {
        case PaperworkList::CueSheet: return "cue_sheet";
        case PaperworkList::DialogueList: return "dialogue_list";
        case PaperworkList::AsBroadcast: return "as_broadcast";
        case PaperworkList::Ccsl: return "ccsl";
    }
    return "";
}

bool paperworkListFromId(const std::string& id, PaperworkList& out) {
    for (PaperworkList l : {PaperworkList::CueSheet, PaperworkList::DialogueList, PaperworkList::AsBroadcast, PaperworkList::Ccsl})
        if (id == paperworkListId(l)) {
            out = l;
            return true;
        }
    return false;
}

const char* paperworkListName(PaperworkList l) {
    switch (l) {
        case PaperworkList::CueSheet: return "Music Cue Sheet";
        case PaperworkList::DialogueList: return "Dialogue List";
        case PaperworkList::AsBroadcast: return "As-Broadcast Script";
        case PaperworkList::Ccsl: return "CCSL";
    }
    return "";
}

bool buildPaperwork(const Project& p, const Sequence& seq, PaperworkList list, const PaperworkOptions& o, XlsxSheet& table,
                    std::string* script, std::string* error) {
    switch (list) {
        case PaperworkList::CueSheet: {
            std::vector<MusicCue> cues = musicCues(p, seq, o.tags ? o.tags : MusicTagReader(musicTagsFor));
            if (o.suggestVocal)
                for (MusicCue& c : cues) {
                    if (cancelled(o.cancel)) return fail(error, "Cancelled");
                    bool vocal = false;
                    if (c.vocal.empty() && cueIsVocal(p, seq, c, vocal)) c.vocal = vocal ? "vocal" : "instrumental";
                }
            table = cueSheetTable(cues, seq);
            return true;
        }
        case PaperworkList::DialogueList:
        case PaperworkList::AsBroadcast: {
            std::vector<OnScreenText> onScreen;
            if (o.onScreenText && !readOnScreenText(p, seq, onScreen, error, o.cancel)) return false;
            std::vector<DialogueEntry> entries = dialogueList(p, seq, {}, onScreen);
            if (o.refineTimes && !refineDialogueTimes(p, seq, entries, 0.4, error, o.cancel)) return false;
            table = dialogueTable(entries, seq);
            if (list == PaperworkList::AsBroadcast) table.name = "As-Broadcast Script";
            if (script) *script = asBroadcastScript(entries, seq);
            return true;
        }
        case PaperworkList::Ccsl: table = ccslTable(ccsl(p, seq, o.gauge), seq); return true;
    }
    return fail(error, "No such list");
}

bool writeTablePdf(const XlsxSheet& t, const std::string& heading, const std::string& path, std::string* error) {
    QString body = QStringLiteral("<h2>%1</h2><table cellspacing='0' cellpadding='3' border='1' style='border-collapse:collapse' "
                                  "width='100%'>")
                       .arg(html(heading));
    for (size_t r = 0; r < t.rows.size(); ++r) {
        const bool head = r == 0 && t.header;
        if (head) body += QStringLiteral("<thead>");
        body += head ? QStringLiteral("<tr style='background:#e8e8e8'>") : QStringLiteral("<tr>");
        for (const XlsxCell& c : t.rows[r]) body += (head ? QStringLiteral("<th>%1</th>") : QStringLiteral("<td>%1</td>")).arg(html(c.text));
        body += QStringLiteral("</tr>");
        if (head) body += QStringLiteral("</thead>");
    }
    body += QStringLiteral("</table>");
    QTextDocument doc;
    doc.setDefaultStyleSheet(QStringLiteral("body { font-size: 7pt } th { text-align: left }"));
    doc.setHtml(body);
    return printPdf(doc, heading, path, error);
}

bool writePaperwork(const Project& p, const Sequence& seq, PaperworkList list, const std::string& path, const PaperworkOptions& o,
                    std::string* error, XlsxSheet* tableOut) {
    const QString ext = QFileInfo(QString::fromStdString(path)).suffix().toLower();
    const bool text = ext == QLatin1String("txt");
    if (ext != QLatin1String("xlsx") && ext != QLatin1String("csv") && ext != QLatin1String("pdf") && !text)
        return fail(error, "Paperwork is written as .xlsx, .csv, .pdf or .txt");
    if (text && list != PaperworkList::AsBroadcast && list != PaperworkList::DialogueList)
        return fail(error, "Only the dialogue list and the as-broadcast script are written as text");
    XlsxSheet table;
    std::string script;
    if (!buildPaperwork(p, seq, list, o, table, &script, error)) return false;
    if (tableOut) *tableOut = table;
    const std::string heading = std::string(paperworkListName(list)) + (seq.name.empty() ? "" : ": " + seq.name);
    if (ext == QLatin1String("xlsx")) return writeXlsxFile(path, {table}, error);
    if (ext == QLatin1String("csv")) return writeBytes(path, tableCsv(table), error);
    if (text) return writeBytes(path, script, error);
    if (list == PaperworkList::AsBroadcast) {
        // The script as it reads, in a typewriter face.
        QTextDocument doc;
        QFont font(QStringLiteral("Courier"));
        font.setStyleHint(QFont::Monospace);
        font.setPointSizeF(9);
        doc.setDefaultFont(font);
        doc.setPlainText(QString::fromStdString(script));
        return printPdf(doc, heading, path, error);
    }
    return writeTablePdf(table, heading, path, error);
}

bool readOnScreenText(const Project& p, const Sequence& seq, std::vector<OnScreenText>& out, std::string* error,
                      const std::atomic<bool>* cancel) {
    out.clear();
    std::string why;
    const std::shared_ptr<TextReader> reader = TextReader::load("en", &why);
    if (!reader) return fail(error, why.empty() ? std::string("The text-reading model is not installed") : why);
    RenderOptions ro;
    ro.scale = std::min(1.0, 1280.0 / std::max(1, seq.width));
    ro.highQuality = true;
    ro.displaySpace = "rec709";
    for (const CcslShot& s : ccsl(p, seq)) {
        if (cancelled(cancel)) return fail(error, "Cancelled");
        const QImage picture = renderProgramFrameRgba8(p, seq, s.in + (s.out - s.in) / 2, ro);
        if (picture.isNull()) continue;
        std::string text = textOf(reader->read(picture));
        if (text.empty()) continue;
        std::replace(text.begin(), text.end(), '\n', ' ');
        if (!out.empty() && out.back().out == s.in && out.back().text == text)
            out.back().out = s.out;
        else
            out.push_back({s.in, s.out, text});
    }
    return true;
}

bool refineDialogueTimes(const Project& p, const Sequence& seq, std::vector<DialogueEntry>& list, double reach, std::string* error,
                         const std::atomic<bool>* cancel) {
    std::vector<DialogueEntry*> lines;
    for (DialogueEntry& e : list)
        if (e.kind == DialogueEntry::Kind::Line) lines.push_back(&e);
    if (lines.empty()) return true;
    const double fps = seq.fpsValue();
    const int rate = seq.sampleRate > 0 ? seq.sampleRate : 48000;
    // The dialogue as heard: the mix without its music and effects.
    Sequence dialogue = seq;
    edit::setRoleMuted(dialogue, "Music", true);
    edit::setRoleMuted(dialogue, "Effects", true);
    // Its loudness every 5 ms (dB of the RMS), over the lines and the reach either side.
    const int hop = std::max(1, rate / 200);
    const double hopSeconds = double(hop) / rate;
    FrameTime first = lines.front()->in, last = lines.front()->out;
    for (const DialogueEntry* e : lines) first = std::min(first, e->in), last = std::max(last, e->out);
    const int64_t from = std::max<int64_t>(0, int64_t(std::floor((first / fps - reach - 0.5) * rate / hop)) * hop);
    const int64_t to = int64_t(std::ceil((last / fps + reach + 0.5) * rate));
    std::vector<float> db;
    {
        AudioMixer mixer;
        const int chunk = hop * 200;
        std::vector<float> buf(size_t(chunk) * 2);
        for (int64_t at = from; at < to; at += chunk) {
            if (cancelled(cancel)) return fail(error, "Cancelled");
            mixer.mix(p, dialogue, at, chunk, buf.data());
            for (int h = 0; h < 200; ++h) {
                double sum = 0;
                for (int i = 0; i < hop; ++i) {
                    const size_t k = size_t(h * hop + i) * 2;
                    const double m = 0.5 * (double(buf[k]) + buf[k + 1]);
                    sum += m * m;
                }
                db.push_back(float(10 * std::log10(sum / hop + 1e-12)));
            }
        }
    }
    const auto hopAt = [&](double seconds) {
        return std::clamp<int64_t>(int64_t(std::floor((seconds * rate - double(from)) / hop)), 0, int64_t(db.size()) - 1);
    };
    const auto secondsAt = [&](int64_t h) { return (double(from) + double(h) * hop) / rate; };
    const int silence = int(std::lround(0.15 / hopSeconds));
    double previousEnd = -1e9;
    for (size_t i = 0; i < lines.size(); ++i) {
        DialogueEntry& e = *lines[i];
        const double a = e.in / fps, b = e.out / fps;
        const double nextStart = i + 1 < lines.size() ? lines[i + 1]->in / fps : 1e9;
        const int64_t ha = hopAt(a), hb = std::max(ha, hopAt(b));
        float peak = -200;
        for (int64_t h = ha; h <= hb; ++h) peak = std::max(peak, db[size_t(h)]);
        if (peak < -70) {  // nothing heard where the transcript puts it
            previousEnd = b;
            continue;
        }
        // The quiet around it: what most of the reach is at its quietest.
        std::vector<float> around(db.begin() + hopAt(a - reach), db.begin() + hopAt(b + reach) + 1);
        std::nth_element(around.begin(), around.begin() + around.size() / 10, around.end());
        const float floorDb = around[around.size() / 10];
        const float threshold = std::max(peak - 30.0f, floorDb + 6.0f);
        const auto loud = [&](int64_t h) { return h >= 0 && h < int64_t(db.size()) && db[size_t(h)] >= threshold; };
        const auto quietBefore = [&](int64_t h) {
            for (int64_t k = h - 1; k >= h - silence; --k)
                if (k >= 0 && loud(k)) return false;
            return true;
        };
        const auto quietAfter = [&](int64_t h) {
            for (int64_t k = h + 1; k <= h + silence; ++k)
                if (k < int64_t(db.size()) && loud(k)) return false;
            return true;
        };
        // The start: the first sound after a silence from `reach` before the transcript's (speech-to-text often puts
        // a first word early, so further into the line when there is none).
        double in = a;
        {
            const int64_t lo = hopAt(std::max(a - reach, previousEnd)), hi = hopAt(b);
            for (int64_t h = lo; h <= hi; ++h)
                if (loud(h) && quietBefore(h)) {
                    in = secondsAt(h);
                    break;
                }
        }
        // The end: the last sound before a silence up to `reach` after the transcript's, short of where the next line
        // starts (back into the line when there is none).
        double out = b;
        {
            const double limit = nextStart > b ? std::min(b + reach, nextStart) : b;
            const int64_t lo = hopAt(in), hi = hopAt(limit);
            for (int64_t h = hi; h >= lo; --h)
                if (loud(h) && quietAfter(h)) {
                    out = secondsAt(h + 1);
                    break;
                }
        }
        e.in = FrameTime(std::floor(in * fps + 1e-6));
        e.out = std::max(e.in + 1, FrameTime(std::ceil(out * fps - 1e-6)));
        previousEnd = out;
    }
    std::stable_sort(list.begin(), list.end(), [](const DialogueEntry& x, const DialogueEntry& y) {
        return x.in != y.in ? x.in < y.in : int(x.kind) < int(y.kind);
    });
    return true;
}

}  // namespace montage
