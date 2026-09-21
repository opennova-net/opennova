#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace opennova {

// The proxy-assisted join: when a NovaWorld .joi carries NI/NP/BK next to NK, the client
// installs the six proxy fields on its NP connection — the game NODE (NI:NP, the
// rendezvous target), the relay COOKIE (atol(BK)) and the RELAY endpoint (the NK-decoded
// host:port the ordinary dial would use) — and, on every enumerator pump while all six are
// set, sends one 48-byte rendezvous datagram to the node immediately before its announce
// burst. The datagram is unencrypted and outside the NP opcode namespace.
// [orig: CNapiGameSession_InitTransportConnection @0x4c9e10 — the install @0x4ca051..0x4ca0c7
//  (proxy_enabled, cookie = atol(BK), node = inet_addr(NI) / atol(NP), relay = inet_addr(NK)
//  / atol(NK port)); CNapiNPConnection_SendPingPacket @0x61f8c0 — memset 48 @0x61f9a1,
//  cookie @+12 @0x61f9bb, dword 0 @+0 @0x61f9c2, "@" @+4 @0x61f9ca, u16 relay port @+6
//  @0x61f9d1, relay addr @+8 @0x61f9d6, SendTo(node addr, node port, 48) ;
//  CNapiNPConnection_PumpEnumeratorAndSend @0x6290c0 @0x629159 (the all-six gate on the
//  3000 ms enumerator interval)]
struct ProxyRendezvousConfig {
	uint32_t node_addr = 0;   // NI as inet_addr (network byte order)
	uint32_t node_port = 0;   // atol(NP)
	uint32_t cookie = 0;      // atol(BK)
	uint32_t relay_addr = 0;  // the NK-decoded host as inet_addr (network byte order)
	uint32_t relay_port = 0;  // the NK-decoded port
	bool enabled() const {
		return node_addr != 0 && node_port != 0 && cookie != 0 && relay_addr != 0 && relay_port != 0;
	}
};

inline constexpr size_t kProxyRendezvousDatagramSize = 48;
inline constexpr uint32_t kProxyRendezvousIntervalMs = 3000;

// The 48-byte rendezvous body: [0..3] = 0, [4] = '@', [5] = 0, [6..7] = u16 relay port
// (little-endian, the LOWORD store), [8..11] = relay addr (the inet_addr dword as stored),
// [12..15] = cookie (u32 little-endian), [16..47] = 0.
inline std::array<uint8_t, kProxyRendezvousDatagramSize> build_proxy_rendezvous_datagram(
		const ProxyRendezvousConfig &cfg) {
	std::array<uint8_t, kProxyRendezvousDatagramSize> packet{};
	packet[4] = '@';
	packet[6] = static_cast<uint8_t>(cfg.relay_port & 0xFFu);
	packet[7] = static_cast<uint8_t>((cfg.relay_port >> 8) & 0xFFu);
	for (size_t i = 0; i < 4; ++i) {
		packet[8 + i] = static_cast<uint8_t>((cfg.relay_addr >> (8 * i)) & 0xFFu);
		packet[12 + i] = static_cast<uint8_t>((cfg.cookie >> (8 * i)) & 0xFFu);
	}
	return packet;
}

// inet_addr over a dotted quad: the four octets in network order packed into the u32 as
// the platform stores it (first octet in the lowest-addressed byte). Returns 0 when the
// text is not a dotted quad (retail's INADDR_NONE also disables the proxy install).
inline uint32_t proxy_inet_addr(const std::string &dotted) {
	uint32_t packed = 0;
	int octets = 0;
	uint32_t value = 0;
	bool digits = false;
	for (size_t i = 0; i <= dotted.size(); ++i) {
		const char c = i < dotted.size() ? dotted[i] : '.';
		if (c >= '0' && c <= '9') {
			value = value * 10u + static_cast<uint32_t>(c - '0');
			if (value > 255u) return 0;
			digits = true;
		} else if (c == '.') {
			if (!digits || octets >= 4) return 0;
			packed |= value << (8 * octets);
			++octets;
			value = 0;
			digits = false;
		} else {
			return 0;
		}
	}
	return octets == 4 ? packed : 0;
}

} // namespace opennova
