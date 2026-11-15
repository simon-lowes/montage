// Montage — a browser review's TURN relay (RFC 8656), as Montage itself uses it: logins that run out, made from a
// secret the relay shares with Montage, so no viewer's page is ever given a password that lasts.
#pragma once

#include <cstdint>
#include <string>

namespace montage::output {

struct TurnLogin {
    std::string user, password;
};

// The login a relay that shares `secret` with Montage takes until `expires` (seconds since 1970, UTC), as the REST
// scheme for TURN has it (draft-uberti-behave-turn-rest; coturn's use-auth-secret): the user name is the expiry and a
// name, "1700000000:montage", and the password the Base64 of HMAC-SHA1 of the user name under the secret. The relay
// works the same out for itself and refuses a login whose time has passed.
TurnLogin turnRestLogin(const std::string& secret, int64_t expires, const std::string& name = "montage");

}  // namespace montage::output
