#pragma once

#include <cstdint>

#include <formats/gamecfg/game_cfg.h>
#include <net/novaworld/lobby_vars.h>

namespace godot {

// Samples the device facts the NWHWI cookie prints (engine HardwareIdentity: the adapter, its
// display memory, the physical memory, the primary desktop) once per process, as retail's boot
// reads them after its game.cfg read (`cfg`'s hw3d_deviceno names the adapter); later calls keep
// the first sample. GameConfigBlock's first main-menu entry runs it.
void sample_device_identity(const opennova::gamecfg::GameCfg &cfg);

// Snapshot the platform-owned locale and hardware telemetry used by retail's
// verify Cookie: NWHWI formats the boot's device facts with `cfg`'s display
// mode (the process's game.cfg block). Protocol shape stays in
// engine/net/novaworld; OS/renderer access stays at this Godot binding boundary.
opennova::LobbyIdentityParams collect_lobby_identity_params(const opennova::gamecfg::GameCfg &cfg);

} // namespace godot
