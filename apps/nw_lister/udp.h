#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace opennova::lister {

// Winsock lifecycle.
bool net_startup();
void net_shutdown();

// Destination policy. Until allow_public is set (the --allow-public flag) every
// destination must resolve to 127.0.0.0/8; anything else is refused before a
// single byte leaves the box. The gate reply / SessionInit can name other hosts
// (UDPNOVAWORLD, POSTIPADDRESS, the web domain) - those go through the same check.
void net_set_allow_public(bool allow);
bool net_allow_public();

struct Addr {
	uint32_t ip_be = 0;   // network byte order
	uint16_t port = 0;
	std::string str() const;
	bool is_loopback() const { return (ip_be & 0xFFu) == 127u; }
};

// Resolve "host" (dotted quad or name) + port. False on failure.
bool resolve(const std::string &host, uint16_t port, Addr &out);
// resolve() + the destination policy. Logs and returns false when refused.
bool resolve_checked(const std::string &host, uint16_t port, Addr &out, const char *what);

class UdpSocket {
public:
	UdpSocket() = default;
	~UdpSocket();
	UdpSocket(const UdpSocket &) = delete;
	UdpSocket &operator=(const UdpSocket &) = delete;

	bool open(const std::string &bind_ip, uint16_t bind_port);
	void close();
	bool is_open() const { return fd_ != -1; }
	uint16_t local_port() const { return local_port_; }
	// Refuses (returns false) a non-loopback destination unless allow_public.
	bool send_to(const Addr &to, const std::vector<uint8_t> &data);
	// Wait up to timeout_ms; returns the datagram size, 0 on timeout, -1 on error.
	int recv_from(std::vector<uint8_t> &buf, Addr &from, int timeout_ms);

private:
	intptr_t fd_ = -1;
	uint16_t local_port_ = 0;
};

} // namespace opennova::lister
