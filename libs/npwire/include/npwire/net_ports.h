#pragma once
// The witnessed retail port constants every C++ endpoint shares. The GDScript
// twin is HostSessionConfig (DEFAULT_LAN_PORT / DEFAULT_GATE_PORT /
// GAME_TYPE_COOP) — godot cannot read these, so the two homes cross-reference
// each other and the maturity lint hard-fails NEW bare literals of these
// values outside the canonical homes.
#include <cstdint>

namespace opennova {

// The retail LAN host port range [orig: game.cfg mplanserverportmin/max
// 32768-32787, JO_SERVER]. The first port of the range is the default a host
// binds and the mpnovaworldport default a client reflects.
inline constexpr uint16_t kRetailLanPortMin = 32768;
inline constexpr uint16_t kRetailLanPortMax = 32787;

// The NovaWorld gate's UDP port (novaworld_gate; the same value as
// libs/novaworld gate_probe.h GATE_DEFAULT_PORT, which stays the service-side
// canonical — novaworld layers ON npwire, so the wire lib carries its own).
inline constexpr uint16_t kNovaWorldGatePort = 7597;

} // namespace opennova
