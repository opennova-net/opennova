#include "net_sockets.h"

#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
using socklen_t_compat = int;
using native_socket_t = SOCKET;
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
using socklen_t_compat = socklen_t;
using native_socket_t = int;
constexpr int INVALID_SOCKET = -1;
constexpr int SOCKET_ERROR = -1;
#endif

namespace opennova::net {

namespace {

bool g_started = false;

inline native_socket_t native_socket(intptr_t fd) {
	return static_cast<native_socket_t>(fd);
}

inline int select_nfds(intptr_t fd) {
#if defined(_WIN32)
	(void)fd; // Winsock ignores nfds.
	return 0;
#else
	return native_socket(fd) + 1;
#endif
}

inline int close_fd(intptr_t fd) {
#if defined(_WIN32)
	return ::closesocket(native_socket(fd));
#else
	return ::close(native_socket(fd));
#endif
}

} // namespace

int startup() {
	if (g_started) {
		return 0;
	}
#if defined(_WIN32)
	WSADATA wsa;
	const int rc = WSAStartup(MAKEWORD(1, 1), &wsa);
	if (rc != 0) {
		return rc;
	}
#endif
	g_started = true;
	return 0;
}

void shutdown() {
	if (!g_started) {
		return;
	}
#if defined(_WIN32)
	::WSACleanup();
#endif
	g_started = false;
}

std::string endpoint_to_string(const Endpoint &ep) {
	char buf[64];
	std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u:%u",
			ep.ip[0], ep.ip[1], ep.ip[2], ep.ip[3], ep.port);
	return std::string(buf);
}

Socket udp_bind(uint16_t port, uint16_t *out_bound) {
	Socket s{};
	const native_socket_t native_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
	if (native_fd == INVALID_SOCKET) {
		return s;
	}
	const intptr_t fd = static_cast<intptr_t>(native_fd);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(port);
	if (::bind(native_socket(fd), reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == SOCKET_ERROR) {
		close_fd(fd);
		return s;
	}
	if (out_bound) {
		sockaddr_in bound{};
		socklen_t_compat len = sizeof(bound);
		if (::getsockname(native_socket(fd), reinterpret_cast<sockaddr *>(&bound), &len) == 0) {
			*out_bound = ntohs(bound.sin_port);
		} else {
			*out_bound = port;
		}
	}
	s.fd = fd;
	return s;
}

int udp_send_to(Socket &s, const Endpoint &to, const uint8_t *data, size_t len) {
	if (!s.is_valid() || !data || len == 0) {
		return -1;
	}
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(to.port);
	addr.sin_addr.s_addr = static_cast<uint32_t>(to.ip[0]) |
			(static_cast<uint32_t>(to.ip[1]) << 8) |
			(static_cast<uint32_t>(to.ip[2]) << 16) |
			(static_cast<uint32_t>(to.ip[3]) << 24);
	const int sent = ::sendto(native_socket(s.fd),
			reinterpret_cast<const char *>(data),
			static_cast<int>(len), 0,
			reinterpret_cast<const sockaddr *>(&addr), sizeof(addr));
	return sent;
}

int udp_recv_from(Socket &s, uint8_t *buf, size_t buf_cap, Endpoint &from, int timeout_ms) {
	if (!s.is_valid() || !buf || buf_cap == 0) {
		return -1;
	}
	fd_set rfds;
	FD_ZERO(&rfds);
	FD_SET(native_socket(s.fd), &rfds);
	timeval tv{};
	tv.tv_sec = timeout_ms / 1000;
	tv.tv_usec = (timeout_ms % 1000) * 1000;
	const int ready = ::select(select_nfds(s.fd), &rfds, nullptr, nullptr, &tv);
	if (ready <= 0) {
		return ready; // 0=timeout, <0=error
	}
	sockaddr_in src{};
	socklen_t_compat slen = sizeof(src);
	const int n = ::recvfrom(native_socket(s.fd),
			reinterpret_cast<char *>(buf),
			static_cast<int>(buf_cap), 0,
			reinterpret_cast<sockaddr *>(&src), &slen);
	if (n <= 0) {
		return n;
	}
	from.port = ntohs(src.sin_port);
	const uint32_t raw = static_cast<uint32_t>(src.sin_addr.s_addr);
	from.ip[0] = static_cast<uint8_t>(raw & 0xFFu);
	from.ip[1] = static_cast<uint8_t>((raw >> 8) & 0xFFu);
	from.ip[2] = static_cast<uint8_t>((raw >> 16) & 0xFFu);
	from.ip[3] = static_cast<uint8_t>((raw >> 24) & 0xFFu);
	return n;
}

void close_socket(Socket &s) {
	if (s.is_valid()) {
		close_fd(s.fd);
		s.fd = -1;
	}
}

Socket tcp_listen(uint16_t port) {
	Socket s{};
	const native_socket_t native_fd = ::socket(AF_INET, SOCK_STREAM, 0);
	if (native_fd == INVALID_SOCKET) {
		return s;
	}
	const intptr_t fd = static_cast<intptr_t>(native_fd);
	int yes = 1;
	::setsockopt(native_socket(fd), SOL_SOCKET, SO_REUSEADDR,
			reinterpret_cast<const char *>(&yes), sizeof(yes));
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(port);
	if (::bind(native_socket(fd), reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == SOCKET_ERROR) {
		close_fd(fd);
		return s;
	}
	if (::listen(native_socket(fd), 8) == SOCKET_ERROR) {
		close_fd(fd);
		return s;
	}
	s.fd = fd;
	return s;
}

Socket tcp_accept(Socket &listener, Endpoint &from, int timeout_ms) {
	Socket accepted{};
	if (!listener.is_valid()) {
		return accepted;
	}
	fd_set rfds;
	FD_ZERO(&rfds);
	FD_SET(native_socket(listener.fd), &rfds);
	timeval tv{};
	tv.tv_sec = timeout_ms / 1000;
	tv.tv_usec = (timeout_ms % 1000) * 1000;
	const int ready = ::select(select_nfds(listener.fd), &rfds, nullptr, nullptr, &tv);
	if (ready <= 0) {
		return accepted;
	}
	sockaddr_in src{};
	socklen_t_compat slen = sizeof(src);
	const native_socket_t native_fd = ::accept(native_socket(listener.fd),
			reinterpret_cast<sockaddr *>(&src), &slen);
	if (native_fd == INVALID_SOCKET) {
		return accepted;
	}
	const intptr_t fd = static_cast<intptr_t>(native_fd);
	from.port = ntohs(src.sin_port);
	const uint32_t raw = static_cast<uint32_t>(src.sin_addr.s_addr);
	from.ip[0] = static_cast<uint8_t>(raw & 0xFFu);
	from.ip[1] = static_cast<uint8_t>((raw >> 8) & 0xFFu);
	from.ip[2] = static_cast<uint8_t>((raw >> 16) & 0xFFu);
	from.ip[3] = static_cast<uint8_t>((raw >> 24) & 0xFFu);
	accepted.fd = fd;
	return accepted;
}

int tcp_recv(Socket &s, uint8_t *buf, size_t buf_cap, int timeout_ms) {
	if (!s.is_valid() || !buf || buf_cap == 0) {
		return -1;
	}
	fd_set rfds;
	FD_ZERO(&rfds);
	FD_SET(native_socket(s.fd), &rfds);
	timeval tv{};
	tv.tv_sec = timeout_ms / 1000;
	tv.tv_usec = (timeout_ms % 1000) * 1000;
	const int ready = ::select(select_nfds(s.fd), &rfds, nullptr, nullptr, &tv);
	if (ready <= 0) {
		return ready;
	}
	return ::recv(native_socket(s.fd), reinterpret_cast<char *>(buf),
			static_cast<int>(buf_cap), 0);
}

int tcp_send(Socket &s, const uint8_t *data, size_t len) {
	if (!s.is_valid() || !data || len == 0) {
		return -1;
	}
	return ::send(native_socket(s.fd), reinterpret_cast<const char *>(data),
			static_cast<int>(len), 0);
}

} // namespace opennova::net
