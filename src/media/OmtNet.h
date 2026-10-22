// Montage — the TCP sockets Open Media Transport runs on (media/Omt.h): BSD sockets, or Winsock on Windows.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace montage::omt::net {

#ifdef _WIN32
using Socket = uintptr_t;  // (Winsock's SOCKET)
constexpr Socket kInvalid = ~Socket(0);
#else
using Socket = int;
constexpr Socket kInvalid = -1;
#endif

void init();  // (Winsock started; once)
void closeSocket(Socket s);
void shutdownSocket(Socket s);  // a blocked read or write on it returns
// No delay, kept alive, and the send and receive buffers OMT uses (8 MB to receive).
void configure(Socket s, bool receiver);
bool sendAll(Socket s, const uint8_t* data, size_t size);
// False when it ended (or `stop` was set and the socket shut down).
bool recvAll(Socket s, uint8_t* data, size_t size, const std::atomic<bool>* stop = nullptr);
// Listens on every address (IPv6 and IPv4) at the first free port from `first` to `last`.
Socket listenOnFirstFree(int first, int last, int* port, std::string* why);
// A connection made to `listener` within `ms`, else kInvalid.
Socket acceptWithin(Socket listener, int ms);
// Connects to `host` (a name or an address; IPv4 tried first) within `ms` an address.
Socket connectTo(const std::string& host, int port, int ms, std::string* why, const std::atomic<bool>* stop = nullptr);
// This computer's addresses other computers can reach it at (IPv4, then IPv6; no loopback).
std::vector<std::string> localAddresses();

}  // namespace montage::omt::net
