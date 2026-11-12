#include "ImfEvents.h"

#include <QCryptographicHash>
#include <QRegularExpression>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <algorithm>
#include <limits>
#include <set>

#include "DcpMxf.h"
#include "core/MediaLog.h"

namespace montage {

namespace {

const QString kResourceNs = QString::fromLatin1(kImfSimpleSchemeResource);
const QString kMarkerNs = QString::fromLatin1(kImfMarkerScheme);

// A count as A.5 writes it: a whole number, as a string, without leading zeros.
bool countText(const QString& s) {
    static const QRegularExpression re(QStringLiteral("^-?(0|[1-9][0-9]*)$"));
    return re.match(s).hasMatch();
}
bool editRateText(const QString& s) {
    static const QRegularExpression re(QStringLiteral("^[1-9][0-9]* [1-9][0-9]*$"));
    return re.match(s).hasMatch();
}
// (a URI: a scheme, then anything without spaces)
bool uriText(const QString& s) {
    static const QRegularExpression re(QStringLiteral("^[A-Za-z][A-Za-z0-9+.-]*:\\S+$"));
    return re.match(s).hasMatch();
}

}  // namespace

std::vector<ImfEvent> imfEventsOfMarkers(const std::vector<Marker>& markers, FrameTime first, int64_t programme, int64_t frames) {
    std::vector<ImfEvent> out;
    for (const Marker& m : markers) {
        if (m.shotChange || m.t < first || m.t >= first + programme) continue;
        ImfEvent e;
        e.start = m.t - first;
        e.end = std::min<int64_t>(e.start + std::max<FrameTime>(m.duration, 0), frames);
        e.name = m.name;
        e.comment = m.comment;
        e.colour = m.color;
        e.chapter = m.chapter;
        e.adBreak = m.adBreak;
        out.push_back(std::move(e));
    }
    std::stable_sort(out.begin(), out.end(), [](const ImfEvent& a, const ImfEvent& b) { return a.start < b.start; });
    for (size_t i = 0; i < out.size(); ++i) out[i].id = "M" + std::to_string(i + 1);
    return out;
}

Marker markerOfImfEvent(const ImfEvent& e, int64_t at, int64_t length) {
    Marker m;
    m.t = at;
    m.duration = std::max<int64_t>(0, length);
    m.name = e.name;
    m.comment = e.comment;
    m.color = e.colour;
    m.chapter = e.chapter;
    m.adBreak = e.adBreak;
    return m;
}

std::string imfEventsDocument(const std::vector<ImfEvent>& events, uint32_t num, uint32_t den) {
    auto writeEvents = [&](QXmlStreamWriter& x) {
        x.writeStartElement(kResourceNs, "events");
        for (size_t i = 0; i < events.size(); ++i) {
            const ImfEvent& e = events[i];
            x.writeStartElement(kResourceNs, "event");
            x.writeTextElement(kResourceNs, "id", QString::fromStdString(e.id.empty() ? "M" + std::to_string(i + 1) : e.id));
            x.writeTextElement(kResourceNs, "startCount", QString::number(e.start));
            x.writeTextElement(kResourceNs, "endCount", QString::number(std::max(e.end, e.start)));
            if (!e.name.empty()) x.writeTextElement(kMarkerNs, "name", QString::fromStdString(e.name));
            if (!e.comment.empty()) x.writeTextElement(kMarkerNs, "comment", QString::fromStdString(e.comment));
            if (e.colour > 0) x.writeTextElement(kMarkerNs, "colour", QString::fromLatin1(labelName(e.colour)));
            if (e.chapter) x.writeTextElement(kMarkerNs, "kind", "chapter");
            if (e.adBreak) x.writeTextElement(kMarkerNs, "kind", "ad-break");
            x.writeEndElement();
        }
        x.writeEndElement();
    };
    // The document's id: a name-based UUID of the events and the rate.
    QByteArray body;
    {
        QXmlStreamWriter x(&body);
        writeEvents(x);
    }
    const QByteArray hash = QCryptographicHash::hash(body + QByteArray::number(num) + ' ' + QByteArray::number(den), QCryptographicHash::Md5);
    dcp::Uuid id{};
    std::copy(hash.begin(), hash.begin() + 16, id.begin());
    id[6] = uint8_t((id[6] & 0x0f) | 0x30);
    id[8] = uint8_t((id[8] & 0x3f) | 0x80);
    const QString uuid = QString::fromStdString(dcp::uuidString(id));
    QByteArray out;
    QXmlStreamWriter x(&out);
    x.setAutoFormatting(true);
    x.setAutoFormattingIndent(2);
    x.writeStartDocument(QStringLiteral("1.0"));
    x.writeNamespace(kMarkerNs, "m");
    x.writeDefaultNamespace(kResourceNs);
    x.writeStartElement(kResourceNs, "document");
    x.writeTextElement(kResourceNs, "id", "urn:uuid:" + uuid);
    x.writeTextElement(kResourceNs, "editRate", QStringLiteral("%1 %2").arg(num).arg(den));
    x.writeEmptyElement(kResourceNs, "objects");
    writeEvents(x);
    x.writeEndElement();  // document
    x.writeEndDocument();
    return out.toStdString();
}

bool parseImfEventsDocument(const std::string& xml, std::vector<ImfEvent>& events, uint32_t& num, uint32_t& den, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    events.clear();
    num = den = 0;
    QXmlStreamReader r(QByteArray::fromStdString(xml));
    if (!r.readNextStartElement() || r.name() != QLatin1String("document") || r.namespaceUri() != kResourceNs)
        return fail("not a Simple Scheme Timed Events Resource (ST 2067-206 Annex A)");
    while (r.readNextStartElement()) {
        if (r.namespaceUri() != kResourceNs) {
            r.skipCurrentElement();
            continue;
        }
        if (r.name() == QLatin1String("editRate")) {
            const QStringList parts = r.readElementText().trimmed().split(' ', Qt::SkipEmptyParts);
            if (parts.size() == 2) num = parts[0].toUInt(), den = parts[1].toUInt();
        } else if (r.name() == QLatin1String("events")) {
            while (r.readNextStartElement()) {
                if (r.name() != QLatin1String("event") || r.namespaceUri() != kResourceNs) {
                    r.skipCurrentElement();
                    continue;
                }
                ImfEvent e;
                e.start = std::numeric_limits<int64_t>::min();
                e.end = std::numeric_limits<int64_t>::max();
                while (r.readNextStartElement()) {
                    const QString name = r.name().toString();
                    const bool own = r.namespaceUri() == kResourceNs, marker = r.namespaceUri() == kMarkerNs;
                    const QString text = r.readElementText(QXmlStreamReader::IncludeChildElements);
                    if (own && name == "id") e.id = text.trimmed().toStdString();
                    else if (own && name == "startCount") e.start = text.trimmed().toLongLong();
                    else if (own && name == "endCount") e.end = text.trimmed().toLongLong();
                    else if (marker && name == "name") e.name = text.toStdString();
                    else if (marker && name == "comment") e.comment = text.toStdString();
                    else if (marker && name == "colour") e.colour = std::max(0, labelFromName(text.trimmed().toStdString()));
                    else if (marker && name == "kind") {
                        e.chapter = e.chapter || text.trimmed() == "chapter";
                        e.adBreak = e.adBreak || text.trimmed() == "ad-break";
                    }
                }
                if (events.size() >= 1000000) return fail("too many events");
                events.push_back(std::move(e));
            }
        } else {
            r.skipCurrentElement();
        }
    }
    if (r.hasError()) return fail("does not read: " + r.errorString().toStdString());
    if (!num || !den) return fail("has no editRate");
    return true;
}

std::vector<std::string> checkImfEventsDocument(const std::string& xml) {
    std::vector<std::string> out;
    QXmlStreamReader r(QByteArray::fromStdString(xml));
    if (!r.readNextStartElement() || r.name() != QLatin1String("document") || r.namespaceUri() != kResourceNs) {
        out.push_back("its document element is not the Simple Scheme's document (ST 2067-206 A.3)");
        return out;
    }
    // id, editRate, objects and events in that order, then only other namespaces' elements.
    static const char* order[] = {"id", "editRate", "objects", "events"};
    size_t next = 0;
    auto ids = [&](const char* list, const char* item) {
        std::set<QString> seen;
        while (r.readNextStartElement()) {
            if (r.namespaceUri() != kResourceNs || r.name() != QLatin1String(item)) {
                out.push_back(std::string("its ") + list + " holds " + r.name().toString().toStdString() + ", not only " + item + " elements");
                r.skipCurrentElement();
                continue;
            }
            QString id;
            bool haveStart = false, haveEnd = false;
            int64_t start = 0, end = 0;
            int at = 0;  // id, startCount, endCount: in that order, before any other namespace's
            bool foreign = false;
            while (r.readNextStartElement()) {
                const QString name = r.name().toString();
                if (r.namespaceUri() != kResourceNs) {
                    foreign = true;
                    r.skipCurrentElement();
                    continue;
                }
                const QString text = r.readElementText(QXmlStreamReader::IncludeChildElements);
                const int place = name == "id" ? 1 : name == "startCount" && std::string(item) == "event" ? 2 : name == "endCount" && std::string(item) == "event" ? 3 : 0;
                if (!place || place <= at || foreign) {
                    out.push_back(std::string("an ") + item + " has " + name.toStdString() + " out of place (A.5)");
                    continue;
                }
                at = place;
                if (place == 1) id = text;
                else if (!countText(text)) out.push_back(std::string("an ") + item + "'s " + name.toStdString() + " (" + text.toStdString() + ") is not a whole number (A.5)");
                else if (place == 2) haveStart = true, start = text.toLongLong();
                else haveEnd = true, end = text.toLongLong();
            }
            if (at < 1) out.push_back(std::string("an ") + item + " has no id (A.5)");
            else if (id.contains(',')) out.push_back(std::string("the ") + item + " id \"" + id.toStdString() + "\" holds a comma (A.5)");
            else if (!seen.insert(id).second) out.push_back(std::string("the ") + item + " id \"" + id.toStdString() + "\" is not unique (A.5)");
            if (haveStart && haveEnd && end < start) out.push_back("event " + id.toStdString() + " ends before it starts (A.5: endCount >= startCount)");
        }
    };
    while (r.readNextStartElement()) {
        const QString name = r.name().toString();
        if (r.namespaceUri() != kResourceNs) {
            if (next < 4) out.push_back("another namespace's " + name.toStdString() + " comes before the document's " + order[next]);
            r.skipCurrentElement();
            continue;
        }
        if (next >= 4 || name != QLatin1String(order[next])) {
            out.push_back("the document has " + name.toStdString() + " where " + (next < 4 ? order[next] : "nothing of its own") + " belongs (A.3)");
            r.skipCurrentElement();
            continue;
        }
        if (next == 0) {
            const QString id = r.readElementText().trimmed();
            if (!uriText(id)) out.push_back("its id (" + id.toStdString() + ") is not a URI (A.5)");
        } else if (next == 1) {
            const QString rate = r.readElementText().trimmed();
            if (!editRateText(rate)) out.push_back("its editRate (" + rate.toStdString() + ") is not two positive whole numbers (A.5)");
        } else if (next == 2) {
            ids("objects", "object");
        } else {
            ids("events", "event");
        }
        ++next;
    }
    if (r.hasError()) out.push_back("it does not read: " + r.errorString().toStdString());
    else if (next < 4) out.push_back(std::string("it has no ") + order[next] + " (A.3)");
    return out;
}

std::vector<std::pair<int64_t, int64_t>> imfChangedEventStretches(const std::vector<ImfEvent>& was, const std::vector<ImfEvent>& now, int64_t frames) {
    // Where an event lies: an instant on its frame.
    auto span = [](const ImfEvent& e) { return std::pair<int64_t, int64_t>{e.start, std::max(e.end, e.start + 1)}; };
    auto in = [](const ImfEvent& e, const std::vector<ImfEvent>& list) {
        return std::any_of(list.begin(), list.end(), [&](const ImfEvent& o) { return o.same(e); });
    };
    std::vector<std::pair<int64_t, int64_t>> out;
    for (const ImfEvent& e : now)
        if (!in(e, was)) out.push_back(span(e));
    for (const ImfEvent& e : was)
        if (!in(e, now)) out.push_back(span(e));
    // Widened over any event either has that crosses an end, until none does.
    for (bool grew = true; grew;) {
        grew = false;
        for (auto& [from, to] : out)
            for (const std::vector<ImfEvent>* list : {&was, &now})
                for (const ImfEvent& e : *list) {
                    const auto [a, b] = span(e);
                    if (a < to && b > from && (a < from || b > to)) {
                        from = std::min(from, a), to = std::max(to, b);
                        grew = true;
                    }
                }
        std::sort(out.begin(), out.end());
        std::vector<std::pair<int64_t, int64_t>> joined;
        for (const auto& s : out) {
            if (!joined.empty() && s.first <= joined.back().second) {
                joined.back().second = std::max(joined.back().second, s.second);
            } else {
                joined.push_back(s);
            }
        }
        out = std::move(joined);
    }
    for (auto& [from, to] : out) from = std::clamp<int64_t>(from, 0, frames), to = std::clamp<int64_t>(to, 0, frames);
    out.erase(std::remove_if(out.begin(), out.end(), [](const auto& s) { return s.second <= s.first; }), out.end());
    return out;
}

}  // namespace montage
