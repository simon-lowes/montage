// Montage — Open Sound Control 1.0 messages (opensoundcontrol.stanford.edu/spec-1_0), the protocol Bitfocus Companion
// (Stream Deck, Loupedeck, X-keys...), TouchOSC, QLab and lighting desks send: an address ("/montage/play"), type tags
// and big-endian arguments, alone or in (possibly nested) bundles. This file only reads and writes them;
// app/OscControl listens for them and does what they say.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace montage::osc {

struct Value {
    char type = 'N';     // i (int32), h (int64), f (float32), d (double), s / S (string), b (blob), T, F, N (nil), I
    int64_t i = 0;       // i, h
    double f = 0;        // f, d
    std::string s;       // s, S; b's bytes
    // As a number (ints, floats, true/false, and strings that are numbers); `ok` false otherwise.
    double number(bool* ok = nullptr) const;
    // As text (strings as they are, numbers written out, true/false).
    std::string text() const;
    bool truthy() const;  // T, a non-zero number, "1"/"true"/"on"
};

Value int32(int32_t v);
Value float32(float v);
Value string(std::string v);
Value boolean(bool v);

struct Message {
    std::string address;
    std::vector<Value> args;
};

// The messages in a packet, bundles opened (nested ones too; their time tags are not waited for); false with the error
// for anything malformed (nothing is returned then).
bool parse(const std::string& packet, std::vector<Message>& out, std::string* error = nullptr);
// A message as a packet.
std::string encode(const Message& m);

}  // namespace montage::osc
