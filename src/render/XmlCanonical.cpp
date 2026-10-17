// Montage — Canonical XML (render/XmlCanonical.h).
#include "XmlCanonical.h"

#include <QXmlStreamReader>
#include <algorithm>
#include <map>
#include <vector>

namespace montage {

namespace {

const QString kXmlDsig = QStringLiteral("http://www.w3.org/2000/09/xmldsig#");

// The characters between escapes go out as UTF-8 in runs, so a pair of surrogates (a character past U+FFFF) stays
// one character.
template <typename Escape>
void escapeWith(std::string& out, const QString& s, Escape escape) {
    qsizetype from = 0;
    for (qsizetype i = 0; i < s.size(); ++i) {
        const char* e = escape(s[i].unicode());
        if (!e) continue;
        out += s.mid(from, i - from).toStdString();
        out += e;
        from = i + 1;
    }
    out += s.mid(from).toStdString();
}

void escapeText(std::string& out, const QString& s) {
    escapeWith(out, s, [](char16_t c) -> const char* {
        switch (c) {
        case '&': return "&amp;";
        case '<': return "&lt;";
        case '>': return "&gt;";
        case '\r': return "&#xD;";
        default: return nullptr;
        }
    });
}

void escapeAttribute(std::string& out, const QString& s) {
    escapeWith(out, s, [](char16_t c) -> const char* {
        switch (c) {
        case '&': return "&amp;";
        case '<': return "&lt;";
        case '"': return "&quot;";
        case '\t': return "&#x9;";
        case '\n': return "&#xA;";
        case '\r': return "&#xD;";
        default: return nullptr;
        }
    });
}

QString prefixOf(QStringView qualified) {
    const qsizetype colon = qualified.indexOf(':');
    return colon < 0 ? QString() : qualified.left(colon).toString();
}

}  // namespace

std::string canonicalXml(const std::string& xml, const CanonicalOptions& o, std::string* error) {
    QXmlStreamReader x(QByteArray(xml.data(), qsizetype(xml.size())));
    x.setNamespaceProcessing(true);
    using Namespaces = std::map<QString, QString>;  // prefix ("" the default) to URI
    std::vector<Namespaces> scope{{}};              // in scope at each open element
    std::vector<Namespaces> rendered;               // as the output's open elements declared them
    std::string out;
    int depth = 0;
    const bool subset = !o.id.empty() || !o.element.empty();
    int outputFrom = subset ? -1 : 1;  // the depth the output starts at (-1: not reached yet)
    int skipFrom = -1;                       // inside a signature left out, from this depth
    bool done = false;
    bool seenRoot = false;
    const QString wanted = QString::fromStdString(o.id);
    while (!x.atEnd() && !done) {
        x.readNext();
        if (x.isStartElement()) {
            ++depth;
            seenRoot = true;
            Namespaces ns = scope.back();
            for (const auto& d : x.namespaceDeclarations()) ns[d.prefix().toString()] = d.namespaceUri().toString();
            scope.push_back(ns);
            if (outputFrom < 0 && !o.element.empty() &&
                QStringLiteral("{%1}%2").arg(x.namespaceUri(), x.name()) == QString::fromStdString(o.element))
                outputFrom = depth;
            if (outputFrom < 0 && !o.id.empty()) {
                for (const auto& a : x.attributes())
                    if (a.namespaceUri().isEmpty() && a.name().compare(QLatin1String("id"), Qt::CaseInsensitive) == 0 && a.value() == wanted)
                        outputFrom = depth;
            }
            if (outputFrom < 0 || depth < outputFrom) continue;
            if (skipFrom < 0 && o.envelopedSignature && x.namespaceUri() == kXmlDsig && x.name() == QLatin1String("Signature")) skipFrom = depth;
            if (skipFrom >= 0) continue;
            const Namespaces parent = rendered.empty() ? Namespaces{} : rendered.back();
            Namespaces mine = parent;
            std::vector<std::pair<QString, QString>> decls;
            auto declare = [&](const QString& prefix) {
                const auto it = ns.find(prefix);
                const QString uri = it == ns.end() ? QString() : it->second;
                const auto was = parent.find(prefix);
                const QString before = was == parent.end() ? QString() : was->second;
                if (uri == before) return;
                if (prefix.isEmpty() && uri.isEmpty() && was == parent.end()) return;  // no default anywhere yet
                if (!prefix.isEmpty() && uri.isEmpty()) return;  // (prefixes cannot be undeclared in XML 1.0)
                decls.emplace_back(prefix, uri);
                mine[prefix] = uri;
            };
            if (o.exclusive) {
                std::vector<QString> used{prefixOf(x.qualifiedName())};
                for (const auto& a : x.attributes())
                    if (const QString p = prefixOf(a.qualifiedName()); !p.isEmpty() && p != QLatin1String("xml")) used.push_back(p);
                std::sort(used.begin(), used.end());
                used.erase(std::unique(used.begin(), used.end()), used.end());
                for (const QString& p : used) declare(p);
            } else {
                for (const auto& [p, uri] : ns)
                    if (p != QLatin1String("xml")) declare(p);
                if (!ns.count(QString())) declare(QString());  // a default of the parent's undone
            }
            std::sort(decls.begin(), decls.end());
            rendered.push_back(mine);
            out += '<';
            out += x.qualifiedName().toString().toStdString();
            for (const auto& [p, uri] : decls) {
                out += p.isEmpty() ? " xmlns=\"" : " xmlns:" + p.toStdString() + "=\"";
                escapeAttribute(out, uri);
                out += '"';
            }
            struct Attr {
                QString uri, local, qualified, value;
            };
            std::vector<Attr> attrs;
            for (const auto& a : x.attributes())
                attrs.push_back({a.namespaceUri().toString(), a.name().toString(), a.qualifiedName().toString(), a.value().toString()});
            std::sort(attrs.begin(), attrs.end(), [](const Attr& a, const Attr& b) { return a.uri != b.uri ? a.uri < b.uri : a.local < b.local; });
            for (const Attr& a : attrs) {
                out += ' ' + a.qualified.toStdString() + "=\"";
                escapeAttribute(out, a.value);
                out += '"';
            }
            out += '>';
        } else if (x.isEndElement()) {
            const bool inside = outputFrom >= 0 && depth >= outputFrom && skipFrom < 0;
            if (inside) {
                out += "</" + x.qualifiedName().toString().toStdString() + ">";
                rendered.pop_back();
            }
            if (skipFrom == depth) skipFrom = -1;
            if (depth == outputFrom && subset) done = true;
            scope.pop_back();
            --depth;
        } else if (x.isCharacters()) {
            if (outputFrom >= 0 && depth >= outputFrom && skipFrom < 0) escapeText(out, x.text().toString());
        } else if (x.isComment()) {
            if (o.comments && !subset && depth == 0) {  // outside the root: on a line of its own
                if (seenRoot) out += '\n';
                out += "<!--" + x.text().toString().toStdString() + "-->";
                if (!seenRoot) out += '\n';
            } else if (o.comments && outputFrom >= 0 && depth >= outputFrom && skipFrom < 0)
                out += "<!--" + x.text().toString().toStdString() + "-->";
        } else if (x.isProcessingInstruction()) {
            if (outputFrom >= 0 && depth >= outputFrom && skipFrom < 0) {
                out += "<?" + x.processingInstructionTarget().toString().toStdString();
                if (!x.processingInstructionData().isEmpty()) out += ' ' + x.processingInstructionData().toString().toStdString();
                out += "?>";
            }
        }
    }
    if (x.hasError() && !done) {
        if (error) *error = "Not well-formed XML: " + x.errorString().toStdString();
        return {};
    }
    if (outputFrom < 0) {
        if (error) *error = o.id.empty() ? "No element " + o.element : "No element with the id " + o.id;
        return {};
    }
    return out;
}

}  // namespace montage
