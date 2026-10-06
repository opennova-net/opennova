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
#include <fcntl.h>
#include <netdb.h>
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

void set_nonblocking(intptr_t fd, bool on) {
#if defined(_WIN32)
	u_long value = on ? 1 : 0;
	::ioctlsocket(native_socket(fd), FIONBIO, &value);
#else
	const int flags = ::fcntl(native_socket(fd), F_GETFL, 0);
	::fcntl(native_socket(fd), F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
#endif
}

bool connect_pending() {
#if defined(_WIN32)
	return ::WSAGetLastError() == WSAEWOULDBLOCK;
#else
	return errno == EINPROGRESS;
#endif
}

void set_io_timeout(intptr_t fd, int timeout_ms) {
#if defined(_WIN32)
	const DWORD value = static_cast<DWORD>(timeout_ms);
#else
	const timeval value{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
#endif
	::setsockopt(native_socket(fd), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&value),
			sizeof(value));
	::setsockopt(native_socket(fd), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&value),
			sizeof(value));
}

sockaddr_in to_sockaddr(const Endpoint &ep) {
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(ep.port);
	addr.sin_addr.s_addr = static_cast<uint32_t>(ep.ip[0]) |
			(static_cast<uint32_t>(ep.ip[1]) << 8) |
			(static_cast<uint32_t>(ep.ip[2]) << 16) |
			(static_cast<uint32_t>(ep.ip[3]) << 24);
	return addr;
}

void from_in_addr(uint32_t raw, Endpoint &ep) {
	ep.ip[0] = static_cast<uint8_t>(raw & 0xFFu);
	ep.ip[1] = static_cast<uint8_t>((raw >> 8) & 0xFFu);
	ep.ip[2] = static_cast<uint8_t>((raw >> 16) & 0xFFu);
	ep.ip[3] = static_cast<uint8_t>((raw >> 24) & 0xFFu);
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

bool resolve_ipv4(const std::string &host, Endpoint &out, bool allow_names) {
	in_addr parsed{};
	if (::inet_pton(AF_INET, host.c_str(), &parsed) == 1) {
		from_in_addr(static_cast<uint32_t>(parsed.s_addr), out);
		return true;
	}
	if (!allow_names) {
		return false;
	}
	addrinfo hints{};
	hints.ai_family = AF_INET;
	addrinfo *found = nullptr;
	if (::getaddrinfo(host.c_str(), nullptr, &hints, &found) != 0 || found == nullptr) {
		return false;
	}
	from_in_addr(static_cast<uint32_t>(
			reinterpret_cast<const sockaddr_in *>(found->ai_addr)->sin_addr.s_addr), out);
	::freeaddrinfo(found);
	return true;
}

std::string local_host_name() {
	char name[256] = {};
	if (::gethostname(name, sizeof(name) - 1) != 0) {
		return {};
	}
	return name;
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
	const sockaddr_in addr = to_sockaddr(to);
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
	from_in_addr(static_cast<uint32_t>(src.sin_addr.s_addr), from);
	return n;
}

void close_socket(Socket &s) {
	if (s.is_valid()) {
		close_fd(s.fd);
		s.fd = -1;
	}
}

Socket tcp_connect(const Endpoint &to, int timeout_ms) {
	Socket s{};
	const native_socket_t native_fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (native_fd == INVALID_SOCKET) {
		return s;
	}
	const intptr_t fd = static_cast<intptr_t>(native_fd);
	const sockaddr_in addr = to_sockaddr(to);
	// Non-blocking for the connect, so a silent peer costs `timeout_ms`, not the OS default.
	set_nonblocking(fd, true);
	if (::connect(native_socket(fd), reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
		if (!connect_pending()) {
			close_fd(fd);
			return s;
		}
		fd_set writable;
		fd_set failed;
		FD_ZERO(&writable);
		FD_ZERO(&failed);
		FD_SET(native_socket(fd), &writable);
		FD_SET(native_socket(fd), &failed);
		timeval tv{};
		tv.tv_sec = timeout_ms / 1000;
		tv.tv_usec = (timeout_ms % 1000) * 1000;
		int error = 0;
		socklen_t_compat len = sizeof(error);
		if (::select(select_nfds(fd), nullptr, &writable, &failed, &tv) <= 0 ||
				!FD_ISSET(native_socket(fd), &writable) ||
				::getsockopt(native_socket(fd), SOL_SOCKET, SO_ERROR,
						reinterpret_cast<char *>(&error), &len) != 0 ||
				error != 0) {
			close_fd(fd);
			return s;
		}
	}
	set_nonblocking(fd, false);
	set_io_timeout(fd, timeout_ms);
	s.fd = fd;
	return s;
}

bool tcp_send_all(Socket &s, const uint8_t *data, size_t len) {
#if defined(MSG_NOSIGNAL)
	constexpr int kFlags = MSG_NOSIGNAL; // a reset peer must not raise SIGPIPE
#else
	constexpr int kFlags = 0;
#endif
	while (len > 0) {
		const int n = ::send(native_socket(s.fd), reinterpret_cast<const char *>(data),
				static_cast<int>(len), kFlags);
		if (n <= 0) {
			return false;
		}
		data += n;
		len -= static_cast<size_t>(n);
	}
	return true;
}

int tcp_recv(Socket &s, uint8_t *buf, size_t cap) {
	const int n = ::recv(native_socket(s.fd), reinterpret_cast<char *>(buf), static_cast<int>(cap), 0);
	return n < 0 ? -1 : n;
}

bool tcp_recv_exact(Socket &s, uint8_t *buf, size_t len) {
	while (len > 0) {
		const int n = tcp_recv(s, buf, len);
		if (n <= 0) {
			return false;
		}
		buf += n;
		len -= static_cast<size_t>(n);
	}
	return true;
}

Socket tcp_listen(uint16_t port, int backlog, uint16_t *out_bound, bool loopback_only) {
	Socket s{};
	const native_socket_t native_fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (native_fd == INVALID_SOCKET) {
		return s;
	}
	const intptr_t fd = static_cast<intptr_t>(native_fd);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(loopback_only ? INADDR_LOOPBACK : INADDR_ANY);
	addr.sin_port = htons(port);
	if (::bind(native_socket(fd), reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == SOCKET_ERROR ||
			::listen(native_socket(fd), backlog) == SOCKET_ERROR) {
		close_fd(fd);
		return s;
	}
	set_nonblocking(fd, true);
	if (out_bound) {
		sockaddr_in bound{};
		socklen_t_compat len = sizeof(bound);
		*out_bound = ::getsockname(native_socket(fd), reinterpret_cast<sockaddr *>(&bound), &len) == 0
				? ntohs(bound.sin_port)
				: port;
	}
	s.fd = fd;
	return s;
}

Socket tcp_accept(Socket &listener, Endpoint &from) {
	Socket s{};
	if (!listener.is_valid()) {
		return s;
	}
	sockaddr_in addr{};
	socklen_t_compat len = sizeof(addr);
	const native_socket_t native_fd =
			::accept(native_socket(listener.fd), reinterpret_cast<sockaddr *>(&addr), &len);
	if (native_fd == INVALID_SOCKET) {
		return s;
	}
	s.fd = static_cast<intptr_t>(native_fd);
	set_nonblocking(s.fd, true);
	from_in_addr(addr.sin_addr.s_addr, from);
	from.port = ntohs(addr.sin_port);
	return s;
}

int tcp_recv_nonblocking(Socket &s, uint8_t *buf, size_t cap) {
	const int n = ::recv(native_socket(s.fd), reinterpret_cast<char *>(buf), static_cast<int>(cap), 0);
	if (n >= 0) {
		return n;
	}
#if defined(_WIN32)
	return ::WSAGetLastError() == WSAEWOULDBLOCK ? TCP_RECV_WOULD_BLOCK : -1;
#else
	return (errno == EWOULDBLOCK || errno == EAGAIN) ? TCP_RECV_WOULD_BLOCK : -1;
#endif
}

void shutdown_socket(const Socket &s) {
	if (!s.is_valid()) {
		return;
	}
#if defined(_WIN32)
	::shutdown(native_socket(s.fd), SD_BOTH);
#else
	::shutdown(native_socket(s.fd), SHUT_RDWR);
#endif
}

void close_socket_reset(Socket &s) {
	if (!s.is_valid()) {
		return;
	}
	linger reset{};
	reset.l_onoff = 1;
	reset.l_linger = 0;
	::setsockopt(native_socket(s.fd), SOL_SOCKET, SO_LINGER, reinterpret_cast<const char *>(&reset),
			sizeof(reset));
	close_socket(s);
}

} // namespace opennova::net
