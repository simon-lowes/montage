#include "render/TurnRelay.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QMessageAuthenticationCode>

namespace montage::output {

TurnLogin turnRestLogin(const std::string& secret, int64_t expires, const std::string& name) {
    TurnLogin login;
    login.user = std::to_string(expires) + ":" + name;
    login.password = QMessageAuthenticationCode::hash(QByteArray::fromStdString(login.user), QByteArray::fromStdString(secret), QCryptographicHash::Sha1)
                         .toBase64()
                         .toStdString();
    return login;
}

}  // namespace montage::output
