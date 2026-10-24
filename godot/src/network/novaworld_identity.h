#pragma once

#include <cstdint>

#include <formats/gamecfg/game_cfg.h>
#include <net/novaworld/lobby_vars.h>

namespace godot {

// Snapshot the platform-owned locale and hardware telemetry used by retail's
// verify Cookie (NWHWI's display mode from `cfg`, the process's game.cfg block).
// Protocol shape stays in engine/net/novaworld; OS/renderer access stays at this
// Godot binding boundary.
opennova::LobbyIdentityParams collect_lobby_identity_params(const opennova::gamecfg::GameCfg &cfg);

} // namespace godot
