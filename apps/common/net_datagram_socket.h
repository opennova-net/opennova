#pragma once

#include <cstddef>
#include <cstdint>

#include <netsim/idatagram_socket.h> // netsim::IDatagramSocket
#include <npwire/peer_addr.h>     // opennova::PeerAddr

#include "net_sockets.h" // opennova::net (apps/common)

namespace opennova::net {

// net::Socket-backed netsim::IDatagramSocket — the real-UDP adapter the headless host
// (apps/nw_server) and the two-endpoint socket test plug into the libs/npruntime owner loop.
//
// net::Endpoint.ip is MSO-first (ip[0] = a in a.b.c.d); PeerAddr.ip is LE octet packing (a in the
// low byte). The conversion is a straight pack/unpack, NOT a byte swap — verified: 127.0.0.1 ->
// PeerAddr.ip 0x0100007F (the value client_runtime_test hard-codes). Moved here from the inline
// to_peer/to_endpoint in apps/nw_server/host_owner_loop.h.
class NetDatagramSocket : public netsim::IDatagramSocket {
public:
	// `sock` is NON-OWNING (the caller owns the socket lifetime). `recv_timeout_ms` is the per-recv
	// wait: 0 (default) is non-blocking — right for main.cpp's busy 62 Hz loop, which paces with
	// sleep_until; a small value (e.g. 30) lets a single-threaded poll-pump driver (the two-endpoint
	// test) block briefly for the peer's datagram instead of spinning.
	explicit NetDatagramSocket(Socket &sock, int recv_timeout_ms = 0)
	    : sock_(sock), recv_timeout_ms_(recv_timeout_ms) {}

	int recv_from(uint8_t *buf, std::size_t cap, PeerAddr &from) override {
		Endpoint e{};
		const int n = udp_recv_from(sock_, buf, cap, e, recv_timeout_ms_);
		if (n > 0) from = to_peer(e);
		return n;
	}

	void send_to(const PeerAddr &to, const uint8_t *data, std::size_t len) override {
		udp_send_to(sock_, to_endpoint(to), data, len);
	}

	// PeerAddr <-> net::Endpoint (the pack/unpack formerly inline in host_owner_loop.h).
	static PeerAddr to_peer(const Endpoint &e) {
		return PeerAddr{static_cast<uint32_t>(e.ip[0]) | (static_cast<uint32_t>(e.ip[1]) << 8) |
		                        (static_cast<uint32_t>(e.ip[2]) << 16) |
		                        (static_cast<uint32_t>(e.ip[3]) << 24),
		                e.port};
	}
	static Endpoint to_endpoint(const PeerAddr &p) {
		Endpoint e;
		e.ip[0] = static_cast<uint8_t>(p.ip & 0xFFu);
		e.ip[1] = static_cast<uint8_t>((p.ip >> 8) & 0xFFu);
		e.ip[2] = static_cast<uint8_t>((p.ip >> 16) & 0xFFu);
		e.ip[3] = static_cast<uint8_t>((p.ip >> 24) & 0xFFu);
		e.port = p.port;
		return e;
	}

private:
	Socket &sock_;
	int recv_timeout_ms_ = 0;
};

} // namespace opennova::net
