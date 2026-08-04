#pragma once

#include <cstdint>

#include <novaworld/lobby_vars.h>

namespace godot {

// Snapshot the platform-owned locale and hardware telemetry used by retail's
// verify Cookie. Protocol shape stays in libs/novaworld; OS/renderer access
// stays at this Godot binding boundary.
opennova::LobbyIdentityParams collect_lobby_identity_params(uint32_t client_index,
		uint32_t client_key);

} // namespace godot
