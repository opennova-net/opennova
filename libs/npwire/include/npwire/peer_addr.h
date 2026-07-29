#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

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
	                    // (octet 0 in the low byte; build it with
	                    // peer_addr_from_octets, print it with the two
	                    // formatters below -- never re-derive either by hand).
	uint16_t port = 0;

	bool operator==(const PeerAddr &other) const {
		return ip == other.ip && port == other.port;
	}
};

// Pack the four dotted-quad octets (as the socket layer reports them) into the
// LE layout above. Retail's `_connectlog.txt` reader treats the four payload
// bytes positionally as octets, so packing the other way (big-endian) makes it
// print "1.0.0.127" for 127.0.0.1 (verified against retail 2026-04-27) -- which
// is why this packing, and the low->high printing below, are one pair of
// functions rather than a convention each caller re-implements.
inline PeerAddr peer_addr_from_octets(const std::array<uint8_t, 4> &octets, uint16_t port) {
	PeerAddr addr;
	addr.ip = static_cast<uint32_t>(octets[0])
	        | (static_cast<uint32_t>(octets[1]) << 8)
	        | (static_cast<uint32_t>(octets[2]) << 16)
	        | (static_cast<uint32_t>(octets[3]) << 24);
	addr.port = port;
	return addr;
}

// "a.b.c.d". This is a WIRE-VISIBLE and DB-VISIBLE rendering: the host_players
// rows are keyed by it, so a second, differently-derived spelling of the same
// address silently leaks rows (remove_player_by_peer stops matching).
inline std::string peer_addr_ip_to_string(const PeerAddr &addr) {
	std::string out;
	out.reserve(15);
	for (int shift = 0; shift <= 24; shift += 8) {
		if (shift != 0) out.push_back('.');
		out += std::to_string((addr.ip >> shift) & 0xFFu);
	}
	return out;
}

// "a.b.c.d:port" -- the peer label the connection layer keys sessions by.
inline std::string peer_addr_to_string(const PeerAddr &addr) {
	return peer_addr_ip_to_string(addr) + ":" + std::to_string(addr.port);
}

struct PeerAddrHash {
	std::size_t operator()(const PeerAddr &a) const noexcept {
		// Splash port into the upper bits of a 64-bit mix.
		return (static_cast<std::size_t>(a.ip) << 16) ^ a.port;
	}
};

} // namespace opennova
