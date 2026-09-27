#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::net {

// Thin cross-platform socket layer for the apps/ binaries (novaworld_server,
// nw_lan_probe); engine/ carries no socket code of its own.
// Winsock2 on Windows, POSIX BSD sockets elsewhere.

// Initialize the underlying networking subsystem. On Windows this is
// WSAStartup(1,1) — matching jodemo's CNapiWinsock_Init@0x5f8960 which
// uses WSA version 1.1. On POSIX this is a no-op. Returns 0 on success.
int startup();

// Tear down. Idempotent; safe to call multiple times.
void shutdown();

// Opaque socket handle (INVALID_HANDLE == -1 on both platforms).
struct Socket {
	intptr_t fd = -1;
	bool is_valid() const noexcept { return fd != -1; }
};

struct Endpoint {
	std::array<uint8_t, 4> ip{0, 0, 0, 0}; // IPv4 dotted quad, bytes[0]=MSO
	uint16_t port = 0;
};

// Format / parse helpers.
std::string endpoint_to_string(const Endpoint &ep);

// Open a UDP socket and bind it to `port` on all interfaces. Pass port=0
// for an ephemeral port (the bound port is reported back in `out_bound`).
Socket udp_bind(uint16_t port, uint16_t *out_bound = nullptr);

// Send a datagram.
int udp_send_to(Socket &s, const Endpoint &to, const uint8_t *data, size_t len);

// Blocking recvfrom with a timeout (ms). Returns the number of bytes
// read, 0 on timeout, -1 on error. The `from` endpoint is populated on
// success.
int udp_recv_from(Socket &s, uint8_t *buf, size_t buf_cap, Endpoint &from,
                  int timeout_ms = 100);

// Close an open socket. Sets fd=-1.
void close_socket(Socket &s);

// RAII wrapper. Moves are fine; copies are deleted.
class ScopedSocket {
public:
	explicit ScopedSocket(Socket s = {}) noexcept : s_(s) {}
	~ScopedSocket() { close_socket(s_); }
	ScopedSocket(const ScopedSocket &) = delete;
	ScopedSocket &operator=(const ScopedSocket &) = delete;
	ScopedSocket(ScopedSocket &&other) noexcept : s_(other.s_) { other.s_ = {}; }
	ScopedSocket &operator=(ScopedSocket &&other) noexcept {
		if (this != &other) { close_socket(s_); s_ = other.s_; other.s_ = {}; }
		return *this;
	}
	Socket &get() noexcept { return s_; }
	const Socket &get() const noexcept { return s_; }
	bool is_valid() const noexcept { return s_.is_valid(); }

private:
	Socket s_;
};

} // namespace opennova::net
