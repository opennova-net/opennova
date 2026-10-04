#include "udp.h"
#include "log.h"
#include "socket_compat.h"

#include <cstdio>
#include <cstring>

namespace opennova::lister {
namespace {
bool g_allow_public = false;
}

bool net_startup() {
#ifdef _WIN32
	WSADATA wsa;
	return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
	return true;
#endif
}

void net_shutdown() {
#ifdef _WIN32
	WSACleanup();
#endif
}

void net_set_allow_public(bool allow) { g_allow_public = allow; }
bool net_allow_public() { return g_allow_public; }

std::string Addr::str() const {
	char buf[32];
	const uint8_t *b = reinterpret_cast<const uint8_t *>(&ip_be);
	std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u:%u", b[0], b[1], b[2], b[3], port);
	return buf;
}

bool resolve(const std::string &host, uint16_t port, Addr &out) {
	in_addr a{};
	if (inet_pton(AF_INET, host.c_str(), &a) == 1) {
		out.ip_be = a.s_addr;
		out.port = port;
		return true;
	}
	// A name lookup would itself be outbound traffic to a DNS server; only do
	// it when public destinations are allowed ("localhost" is special-cased).
	if (host == "localhost") {
		out.ip_be = htonl(INADDR_LOOPBACK);
		out.port = port;
		return true;
	}
	if (!g_allow_public) return false;
	addrinfo hints{};
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	addrinfo *res = nullptr;
	if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
	out.ip_be = reinterpret_cast<sockaddr_in *>(res->ai_addr)->sin_addr.s_addr;
	out.port = port;
	freeaddrinfo(res);
	return true;
}

bool resolve_checked(const std::string &host, uint16_t port, Addr &out, const char *what) {
	if (!resolve(host, port, out)) {
		logf("[net] REFUSED %s %s:%u (cannot resolve%s)", what, host.c_str(), port,
		     g_allow_public ? "" : " without --allow-public: name lookups are off");
		return false;
	}
	if (!out.is_loopback() && !g_allow_public) {
		logf("[net] REFUSED %s %s (%s) - not 127.0.0.0/8 and --allow-public not given", what,
		     out.str().c_str(), host.c_str());
		return false;
	}
	return true;
}

UdpSocket::~UdpSocket() { close(); }

bool UdpSocket::open(const std::string &bind_ip, uint16_t bind_port) {
	close();
	const SockHandle s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s == kInvalidSock) return false;
	sockaddr_in sa{};
	sa.sin_family = AF_INET;
	sa.sin_port = htons(bind_port);
	if (inet_pton(AF_INET, bind_ip.c_str(), &sa.sin_addr) != 1 ||
	    ::bind(s, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)) != 0) {
		close_sock(s);
		return false;
	}
#ifdef _WIN32
	// An ICMP port-unreachable would otherwise surface as a WSAECONNRESET on
	// the next recvfrom and look like a dead socket.
	BOOL off = FALSE;
	DWORD ret = 0;
	WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12) /*SIO_UDP_CONNRESET*/, &off, sizeof(off), nullptr, 0, &ret,
	         nullptr, nullptr);
#endif
	sockaddr_in got{};
	SockLen len = sizeof(got);
	getsockname(s, reinterpret_cast<sockaddr *>(&got), &len);
	local_port_ = ntohs(got.sin_port);
	fd_ = static_cast<intptr_t>(s);
	return true;
}

void UdpSocket::close() {
	if (is_open()) {
		close_sock(static_cast<SockHandle>(fd_));
		fd_ = -1;
	}
}

bool UdpSocket::send_to(const Addr &to, const std::vector<uint8_t> &data) {
	if (!is_open() || data.empty()) return false;
	if (!to.is_loopback() && !g_allow_public) {
		logf("[net] REFUSED send to %s - not loopback and --allow-public not given", to.str().c_str());
		return false;
	}
	sockaddr_in sa{};
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = to.ip_be;
	sa.sin_port = htons(to.port);
	const auto n = ::sendto(static_cast<SockHandle>(fd_), reinterpret_cast<const char *>(data.data()),
	                        static_cast<int>(data.size()), 0, reinterpret_cast<sockaddr *>(&sa), sizeof(sa));
	return n == static_cast<decltype(n)>(data.size());
}

int UdpSocket::recv_from(std::vector<uint8_t> &buf, Addr &from, int timeout_ms) {
	if (!is_open()) return -1;
	const SockHandle s = static_cast<SockHandle>(fd_);
	fd_set rf;
	FD_ZERO(&rf);
	FD_SET(s, &rf);
	timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
	const int r = ::select(select_nfds(s), &rf, nullptr, nullptr, &tv);
	if (r <= 0) return r == 0 ? 0 : -1;
	buf.resize(65536);
	sockaddr_in sa{};
	SockLen len = sizeof(sa);
	const auto n = ::recvfrom(s, reinterpret_cast<char *>(buf.data()), static_cast<int>(buf.size()), 0,
	                          reinterpret_cast<sockaddr *>(&sa), &len);
	if (n < 0) return -1;
	buf.resize(static_cast<size_t>(n));
	from.ip_be = sa.sin_addr.s_addr;
	from.port = ntohs(sa.sin_port);
	return static_cast<int>(n);
}

} // namespace opennova::lister
