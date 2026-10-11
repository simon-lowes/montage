// Montage — what DCPs and IMF packages share: SHA-1 hashes of files as packing lists give them, UTC timestamps, and a
// flat reading of their XML documents.
#pragma once

#include <QString>
#include <functional>
#include <utility>
#include <vector>

namespace montage {

// SHA-1 of a file, base64. `read` (if given) hears of each mebibyte read and may return false to stop (the hash is
// then empty).
QString packageFileHash(const QString& path, qint64* size = nullptr, const std::function<bool(qint64)>& read = {});
// Now, as xs:dateTime in UTC ("2026-10-09T20:13:40+00:00").
QString packageTimestamp();
// The elements of an XML file as (path of element names joined by '/', text) pairs; empty if it does not parse.
std::vector<std::pair<QString, QString>> packageXmlItems(const QString& file, QString* root = nullptr);
// "urn:uuid:ABC..." as "abc...".
QString packageBareId(QString id);

}  // namespace montage
