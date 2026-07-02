#pragma once

#include <cstddef>
#include <cstdint>

namespace opennova {

// Identifies a remote UDP peer. NovaLogic protocol is IPv4-only on the wire.
//
// This is the in-match transport-address key. It lives in its own lean header (no <mutex>,
// no matchmaking registry) so the in-match net core (libs/netsim, libs/npruntime) and the
// socket owners (apps/nw_server, godot/engine) can key peers by address without pulling the
// NovaWorld matchmaking ConnectionRegistry (novaworld/connection/registry.h, which includes
// this header for the same two types).
struct PeerAddr {
	uint32_t ip = 0;    // LE octet packing: a.b.c.d -> a | b<<8 | c<<16 | d<<24
	                    // (octet 0 in the low byte; see ip_to_le in
	                    // nw_udp_listener.cpp). Format low->high to print a.b.c.d.
	uint16_t port = 0;

	bool operator==(const PeerAddr &other) const {
		return ip == other.ip && port == other.port;
	}
};

struct PeerAddrHash {
	std::size_t operator()(const PeerAddr &a) const noexcept {
		// Splash port into the upper bits of a 64-bit mix.
		return (static_cast<std::size_t>(a.ip) << 16) ^ a.port;
	}
};

} // namespace opennova
