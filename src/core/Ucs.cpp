#include "Ucs.h"

#include <QString>
#include <QStringList>
#include <algorithm>
#include <map>

namespace montage {

const std::vector<UcsCategory>& ucsCategories() {
    static const std::vector<UcsCategory> list = {
#include "UcsCategories.inc"
    };
    return list;
}

const UcsCategory* ucsCategory(const std::string& catId) {
    static const std::map<QString, const UcsCategory*> byId = [] {
        std::map<QString, const UcsCategory*> m;
        for (const UcsCategory& c : ucsCategories()) m[QString::fromLatin1(c.catId).toLower()] = &c;
        return m;
    }();
    const auto it = byId.find(QString::fromStdString(catId).toLower());
    return it == byId.end() ? nullptr : it->second;
}

bool parseUcsFileName(const std::string& fileName, UcsName& out) {
    QString name = QString::fromStdString(fileName);
    if (const int dot = name.lastIndexOf('.'); dot > 0) name.truncate(dot);
    const QStringList parts = name.split('_');
    if (parts.size() < 2) return false;
    const QString head = parts[0];
    const int dash = head.indexOf('-');
    const UcsCategory* c = ucsCategory((dash < 0 ? head : head.left(dash)).toStdString());
    if (!c) return false;
    UcsName n;
    n.category = c;
    n.catId = c->catId;
    if (dash >= 0) n.userCategory = head.mid(dash + 1).toStdString();
    n.fxName = parts[1].trimmed().toStdString();
    if (parts.size() > 2) n.creatorId = parts[2].trimmed().toStdString();
    if (parts.size() > 3) n.sourceId = parts[3].trimmed().toStdString();
    if (parts.size() > 4) n.userData = parts.mid(4).join('_').trimmed().toStdString();
    out = n;
    return true;
}

}  // namespace montage
