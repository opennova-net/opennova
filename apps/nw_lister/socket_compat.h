#pragma once

// The few BSD-socket spellings that differ between Winsock2 and POSIX, so the
// lister's UDP, HTTP and admin transports stay one source on both platforms.

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
inline void set_nonblocking(SockHandle s, bool on) {
	u_long v = on ? 1 : 0;
	ioctlsocket(s, FIONBIO, &v);
}
inline bool connect_in_progress() { return WSAGetLastError() == WSAEWOULDBLOCK; }
inline void set_io_timeout(SockHandle s, int ms) {
	const DWORD v = static_cast<DWORD>(ms);
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&v), sizeof(v));
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&v), sizeof(v));
}
} // namespace opennova::lister
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
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
inline void set_nonblocking(SockHandle s, bool on) {
	const int flags = fcntl(s, F_GETFL, 0);
	fcntl(s, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
}
inline bool connect_in_progress() { return errno == EINPROGRESS; }
inline void set_io_timeout(SockHandle s, int ms) {
	timeval v{ms / 1000, (ms % 1000) * 1000};
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &v, sizeof(v));
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &v, sizeof(v));
}
} // namespace opennova::lister
#endif
