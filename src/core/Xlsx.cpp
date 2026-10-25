#include "Xlsx.h"

#include <QFile>
#include <QSaveFile>
#include <QXmlStreamReader>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#include "Zip.h"

namespace montage {

XlsxCell XlsxCell::num(double v) {
    XlsxCell c;
    c.number = true;
    c.value = v;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.15g", v);
    c.text = buf;
    return c;
}

std::string xlsxColumn(int index) {
    std::string s;
    for (int n = index + 1; n > 0; n = (n - 1) / 26) s.insert(s.begin(), char('A' + (n - 1) % 26));
    return s;
}

namespace {

std::string escaped(const std::string& t) {
    std::string o;
    o.reserve(t.size());
    for (unsigned char c : t) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            case '\t':
            case '\n':
            case '\r': o.push_back(char(c)); break;
            default:
                if (c >= 0x20) o.push_back(char(c));  // (control characters are not allowed in XML 1.0)
        }
    }
    return o;
}

std::string sheetName(std::string n, int index, std::vector<std::string>& used) {
    std::erase_if(n, [](char c) { return std::string("[]:*?/\\").find(c) != std::string::npos; });
    if (n.size() > 31) n.resize(31);
    if (n.empty()) n = "Sheet" + std::to_string(index + 1);
    std::string base = n;
    for (int k = 2; std::find(used.begin(), used.end(), n) != used.end(); ++k) {
        const std::string tail = " (" + std::to_string(k) + ")";
        n = base.substr(0, 31 - tail.size()) + tail;
    }
    used.push_back(n);
    return n;
}

std::string sheetXml(const XlsxSheet& s) {
    std::string x = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
                    "\n<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">";
    if (s.header && !s.rows.empty())
        x += R"(<sheetViews><sheetView workbookViewId="0"><pane ySplit="1" topLeftCell="A2" activePane="bottomLeft" state="frozen"/></sheetView></sheetViews>)";
    bool cols = false;
    for (size_t i = 0; i < s.widths.size(); ++i) {
        if (s.widths[i] <= 0) continue;
        if (!cols) x += "<cols>", cols = true;
        char buf[96];
        std::snprintf(buf, sizeof buf, R"(<col min="%zu" max="%zu" width="%.2f" customWidth="1"/>)", i + 1, i + 1, s.widths[i]);
        x += buf;
    }
    if (cols) x += "</cols>";
    x += "<sheetData>";
    for (size_t r = 0; r < s.rows.size(); ++r) {
        const std::string row = std::to_string(r + 1);
        x += "<row r=\"" + row + "\">";
        const bool bold = s.header && r == 0;
        for (size_t c = 0; c < s.rows[r].size(); ++c) {
            const XlsxCell& cell = s.rows[r][c];
            if (!cell.number && cell.text.empty()) continue;
            const std::string ref = xlsxColumn(int(c)) + row;
            const std::string style = bold ? " s=\"1\"" : "";
            if (cell.number && std::isfinite(cell.value)) {
                x += "<c r=\"" + ref + "\"" + style + "><v>" + cell.text + "</v></c>";
            } else {
                const bool keep = !cell.text.empty() && (cell.text.front() == ' ' || cell.text.back() == ' ' ||
                                                         cell.text.find('\n') != std::string::npos);
                x += "<c r=\"" + ref + "\"" + style + " t=\"inlineStr\"><is><t" + (keep ? " xml:space=\"preserve\"" : "") + ">" +
                     escaped(cell.text) + "</t></is></c>";
            }
        }
        x += "</row>";
    }
    x += "</sheetData></worksheet>";
    return x;
}

}  // namespace

std::string writeXlsx(const std::vector<XlsxSheet>& in) {
    std::vector<XlsxSheet> sheets = in;
    if (sheets.empty()) sheets.push_back(XlsxSheet{});
    std::vector<std::string> used, names;
    for (size_t i = 0; i < sheets.size(); ++i) names.push_back(sheetName(sheets[i].name, int(i), used));

    ZipWriter z;
    std::string types = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
                        "\n<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
                        R"(<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>)"
                        R"(<Default Extension="xml" ContentType="application/xml"/>)"
                        R"(<Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/>)"
                        R"(<Override PartName="/xl/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml"/>)";
    for (size_t i = 0; i < sheets.size(); ++i)
        types += "<Override PartName=\"/xl/worksheets/sheet" + std::to_string(i + 1) +
                 ".xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>";
    types += "</Types>";
    z.add("[Content_Types].xml", types);
    z.add("_rels/.rels", R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
                         "\n<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                         R"(<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/>)"
                         "</Relationships>");
    std::string wb = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
                     "\n<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
                     "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"><sheets>";
    std::string rels = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
                       "\n<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">";
    for (size_t i = 0; i < sheets.size(); ++i) {
        const std::string n = std::to_string(i + 1);
        wb += "<sheet name=\"" + escaped(names[i]) + "\" sheetId=\"" + n + "\" r:id=\"rId" + n + "\"/>";
        rels += "<Relationship Id=\"rId" + n +
                "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet" + n +
                ".xml\"/>";
    }
    const std::string styles = std::to_string(sheets.size() + 1);
    rels += "<Relationship Id=\"rId" + styles +
            "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/></Relationships>";
    wb += "</sheets></workbook>";
    z.add("xl/workbook.xml", wb);
    z.add("xl/_rels/workbook.xml.rels", rels);
    // Two fonts (regular, bold) and the cell formats using them.
    z.add("xl/styles.xml", R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
                           "\n<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
                           R"(<fonts count="2"><font><sz val="11"/><name val="Calibri"/></font><font><b/><sz val="11"/><name val="Calibri"/></font></fonts>)"
                           R"(<fills count="2"><fill><patternFill patternType="none"/></fill><fill><patternFill patternType="gray125"/></fill></fills>)"
                           R"(<borders count="1"><border><left/><right/><top/><bottom/><diagonal/></border></borders>)"
                           R"(<cellStyleXfs count="1"><xf numFmtId="0" fontId="0" fillId="0" borderId="0"/></cellStyleXfs>)"
                           R"(<cellXfs count="2"><xf numFmtId="0" fontId="0" fillId="0" borderId="0" xfId="0"/>)"
                           R"(<xf numFmtId="0" fontId="1" fillId="0" borderId="0" xfId="0" applyFont="1"/></cellXfs>)"
                           R"(<cellStyles count="1"><cellStyle name="Normal" xfId="0" builtinId="0"/></cellStyles>)"
                           "</styleSheet>");
    for (size_t i = 0; i < sheets.size(); ++i) z.add("xl/worksheets/sheet" + std::to_string(i + 1) + ".xml", sheetXml(sheets[i]));
    return z.finish();
}

bool writeXlsxFile(const std::string& path, const std::vector<XlsxSheet>& sheets, std::string* error) {
    const std::string bytes = writeXlsx(sheets);
    QSaveFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::WriteOnly) || f.write(bytes.data(), qint64(bytes.size())) != qint64(bytes.size()) || !f.commit()) {
        if (error) *error = "Could not write " + path;
        return false;
    }
    return true;
}

namespace {

bool fail(std::string* error, const std::string& why) {
    if (error) *error = why;
    return false;
}

// "xl/worksheets/../x.xml" to "xl/x.xml"; a target from the root ("/xl/...") as it is.
std::string resolvePart(const std::string& base, const std::string& target) {
    std::string path = !target.empty() && target.front() == '/' ? target.substr(1) : base + target;
    std::vector<std::string> parts;
    size_t at = 0;
    while (at <= path.size()) {
        const size_t slash = path.find('/', at);
        const std::string part = path.substr(at, slash == std::string::npos ? std::string::npos : slash - at);
        if (part == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        if (slash == std::string::npos) break;
        at = slash + 1;
    }
    std::string out;
    for (const std::string& p : parts) out += (out.empty() ? "" : "/") + p;
    return out;
}

// The text of an <si> or <is>: its <t>, or its runs' <t>s (phonetic readings left out).
QString stringItem(QXmlStreamReader& r) {
    const QString end = r.name().toString();
    QString text;
    int skip = 0;
    while (!r.atEnd()) {
        r.readNext();
        if (r.isStartElement()) {
            if (r.name() == u"rPh") ++skip;
            else if (r.name() == u"t" && !skip) text += r.readElementText();
        } else if (r.isEndElement()) {
            if (r.name() == u"rPh") --skip;
            else if (r.name() == end) break;
        }
    }
    return text;
}

int columnOf(const QString& ref) {
    int c = 0;
    for (QChar ch : ref) {
        if (!ch.isLetter()) break;
        c = c * 26 + (ch.toUpper().unicode() - 'A' + 1);
    }
    return c - 1;
}

}  // namespace

bool readXlsx(const std::string& bytes, std::vector<XlsxSheet>& out, std::string* error) {
    out.clear();
    ZipReader z;
    if (!z.open(bytes)) return fail(error, "Not an XLSX workbook (not a zip archive)");
    std::string xml;
    // The workbook part, from the package's relationships.
    std::string workbook = "xl/workbook.xml";
    if (z.read("_rels/.rels", xml)) {
        QXmlStreamReader r(QByteArray::fromStdString(xml));
        while (!r.atEnd()) {
            r.readNext();
            if (r.isStartElement() && r.name() == u"Relationship" && r.attributes().value("Type").endsWith(u"/officeDocument"))
                workbook = resolvePart("", r.attributes().value("Target").toString().toStdString());
        }
    }
    if (!z.read(workbook, xml)) return fail(error, "Not an XLSX workbook (no workbook part)");
    const std::string dir = workbook.substr(0, workbook.rfind('/') + 1);
    std::vector<std::pair<std::string, std::string>> sheets;  // name, relationship id
    {
        QXmlStreamReader r(QByteArray::fromStdString(xml));
        while (!r.atEnd()) {
            r.readNext();
            if (r.isStartElement() && r.name() == u"sheet") {
                QString id;
                for (const QXmlStreamAttribute& a : r.attributes())
                    if (a.name() == u"id") id = a.value().toString();  // (r:id, whatever its prefix)
                sheets.emplace_back(r.attributes().value("name").toString().toStdString(), id.toStdString());
            }
        }
        if (r.hasError()) return fail(error, "The workbook part is not well-formed XML");
    }
    std::map<std::string, std::string> targets;
    std::string sharedPart = dir + "sharedStrings.xml";
    const std::string relsPath = dir + "_rels/" + workbook.substr(dir.size()) + ".rels";
    if (z.read(relsPath, xml)) {
        QXmlStreamReader r(QByteArray::fromStdString(xml));
        while (!r.atEnd()) {
            r.readNext();
            if (!r.isStartElement() || r.name() != u"Relationship") continue;
            const std::string target = resolvePart(dir, r.attributes().value("Target").toString().toStdString());
            targets[r.attributes().value("Id").toString().toStdString()] = target;
            if (r.attributes().value("Type").endsWith(u"/sharedStrings")) sharedPart = target;
        }
    }
    std::vector<QString> shared;
    if (z.read(sharedPart, xml)) {
        QXmlStreamReader r(QByteArray::fromStdString(xml));
        while (!r.atEnd()) {
            r.readNext();
            if (r.isStartElement() && r.name() == u"si") shared.push_back(stringItem(r));
        }
    }
    for (size_t i = 0; i < sheets.size(); ++i) {
        XlsxSheet sheet;
        sheet.name = sheets[i].first;
        sheet.header = false;
        auto t = targets.find(sheets[i].second);
        const std::string part = t != targets.end() ? t->second : dir + "worksheets/sheet" + std::to_string(i + 1) + ".xml";
        std::string err;
        if (!z.read(part, xml, &err)) return fail(error, "Sheet \"" + sheet.name + "\" could not be read: " + err);
        QXmlStreamReader r(QByteArray::fromStdString(xml));
        int row = -1, col = -1;
        while (!r.atEnd()) {
            r.readNext();
            if (!r.isStartElement()) continue;
            if (r.name() == u"row") {
                const int rr = r.attributes().value("r").toInt();
                row = rr > 0 ? rr - 1 : row + 1;
                col = -1;
                if (int(sheet.rows.size()) <= row) sheet.rows.resize(size_t(row) + 1);
            } else if (r.name() == u"c" && row >= 0) {
                const QString ref = r.attributes().value("r").toString();
                col = ref.isEmpty() ? col + 1 : columnOf(ref);
                const QString type = r.attributes().value("t").toString();
                XlsxCell cell;
                QString value;
                bool inlineText = false;
                while (!r.atEnd()) {
                    r.readNext();
                    if (r.isStartElement() && r.name() == u"v") value = r.readElementText();
                    else if (r.isStartElement() && r.name() == u"is") value = stringItem(r), inlineText = true;
                    else if (r.isEndElement() && r.name() == u"c") break;
                }
                if (type == u"s") {
                    const int k = value.toInt();
                    cell.text = k >= 0 && k < int(shared.size()) ? shared[size_t(k)].toStdString() : std::string();
                } else if (type == u"inlineStr" || inlineText || type == u"str" || type == u"e") {
                    cell.text = value.toStdString();
                } else if (type == u"b") {
                    cell.text = value == u"1" ? "TRUE" : "FALSE";
                } else if (!value.isEmpty()) {
                    bool ok = false;
                    const double v = value.toDouble(&ok);
                    cell = ok ? XlsxCell::num(v) : XlsxCell(value.toStdString());
                    if (ok) cell.text = value.toStdString();
                }
                if (col < 0) continue;
                auto& cells = sheet.rows[size_t(row)];
                if (int(cells.size()) <= col) cells.resize(size_t(col) + 1);
                cells[size_t(col)] = cell;
            }
        }
        if (r.hasError()) return fail(error, "Sheet \"" + sheet.name + "\" is not well-formed XML");
        out.push_back(std::move(sheet));
    }
    if (out.empty()) return fail(error, "The workbook has no sheets");
    return true;
}

bool readXlsxFile(const std::string& path, std::vector<XlsxSheet>& out, std::string* error) {
    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::ReadOnly)) return fail(error, "Could not open " + path);
    const QByteArray bytes = f.readAll();
    return readXlsx(bytes.toStdString(), out, error);
}

}  // namespace montage
