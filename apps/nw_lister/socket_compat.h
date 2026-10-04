#pragma once

// The few BSD-socket spellings that differ between Winsock2 and POSIX, so the
// lister's UDP and HTTP transports stay one source on both platforms.

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

namespace opennova::lister {
using SockHandle = SOCKET;
using SockLen = int;
constexpr SockHandle kInvalidSock = INVALID_SOCKET;
inline void close_sock(SockHandle s) { closesocket(s); }
inline int select_nfds(SockHandle) { return 0; } // ignored by Winsock
} // namespace opennova::lister
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace opennova::lister {
using SockHandle = int;
using SockLen = socklen_t;
constexpr SockHandle kInvalidSock = -1;
inline void close_sock(SockHandle s) { ::close(s); }
inline int select_nfds(SockHandle s) { return s + 1; }
} // namespace opennova::lister
#endif
