#pragma once
// The witnessed retail port constants every C++ endpoint shares. The GDScript
// twin is HostSessionConfig (DEFAULT_LAN_PORT / DEFAULT_GATE_PORT /
// GAME_TYPE_COOP) — godot cannot read these, so the two homes cross-reference
// each other and the maturity lint hard-fails NEW bare literals of these
// values outside the canonical homes.
#include <cstdint>
#include <vector>

namespace opennova {

// The retail LAN host port range [orig: game.cfg mplanserverportmin/max
// 32768-32787, JO_SERVER]. The first port of the range is the default a host
// binds and the mpnovaworldport default a client reflects.
inline constexpr uint16_t kRetailLanPortMin = 32768;
inline constexpr uint16_t kRetailLanPortMax = 32787;
// The scan stride [orig: game.cfg mplanserverportdelta, default 1 — the cfg
// registration table @ 0x8331b8].
inline constexpr uint16_t kRetailLanPortDelta = 1;

// The LAN host's bind scan (D-NET-210): the authority arm feeds
// {mplanserverportmin/max/delta, random=0} into the socket open
// [orig: CNapiNetwork_OpenTransportSocket @ 0x4c6a40, authority arm
// @ 0x4c6aa2], which clamps (step 0 -> 1, max < min -> max = min, both
// <= 0xFFFF [orig: NapiSocket_ClampBufferParams @ 0x62e180]) and then tries
// (max - min + 1) / step ports: the first bind at min, each retry stepping
// by delta and WRAPPING back to min past max
// [orig: NapiUdpSocket_CreateAndBind @ 0x62d2a0 — the bind-failure loop
// @ 0x62d440; the random-start leg is the CLIENT arm's, the host passes
// random=0]. The authored min maps to our session bind_port; max/delta stay
// the shipped defaults.
inline std::vector<uint16_t> lan_host_bind_ports(uint32_t min_port,
		uint32_t max_port = kRetailLanPortMax,
		uint32_t delta = kRetailLanPortDelta) {
	if (min_port > 0xFFFF) min_port = 0xFFFF;
	if (delta == 0) delta = 1;
	if (max_port < min_port) max_port = min_port;
	if (max_port > 0xFFFF) max_port = 0xFFFF;
	const uint32_t attempts = (max_port - min_port + 1) / delta;
	std::vector<uint16_t> ports;
	uint32_t port = min_port;
	ports.push_back(static_cast<uint16_t>(port));
	for (int32_t remaining = static_cast<int32_t>(attempts) - 1; remaining > 0;
			--remaining) {
		port += delta;
		if (port > max_port) port = min_port;
		ports.push_back(static_cast<uint16_t>(port));
	}
	return ports;
}

// The NovaWorld gate's UDP port (novaworld_gate; the same value as
// engine/net/novaworld gate_probe.h GATE_DEFAULT_PORT, which stays the service-side
// canonical — novaworld layers ON npwire, so the wire lib carries its own).
inline constexpr uint16_t kNovaWorldGatePort = 7597;

} // namespace opennova
