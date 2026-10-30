#pragma once

#include <cstdint>

#include <formats/gamecfg/game_cfg.h>
#include <net/novacrypto/cdkey.h>
#include <net/novaworld/lobby_vars.h>

#include <godot_cpp/variant/vector2i.hpp>

namespace opennova {
struct Protocol;
} // namespace opennova

namespace godot {

// The primary desktop as retail samples it (engine: read_primary_screen_size, the logical size of
// a scaled desktop, as retail's DPI-unaware process reads it) on Windows; elsewhere, or where it
// reads nothing, the display server's primary screen stands in ((0, 0) with no display server).
// The first launch's video test (GameConfigBlock.primary_desktop_size, the menu's entry) and
// NWHWI's renderer desktop both take it.
Vector2i primary_desktop_size();

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

// This machine's NWPSSK and NWUSID: retail's encoders over the volume and first Ethernet adapter
// it reads (engine make_retail_machine_tokens) where the platform has them (Windows), else over a
// hash of Godot's platform-stable id and the renderer's adapter name (fallback_machine_tokens,
// D-NET-981). The verify Cookie's two tokens, and the fingerprint the DFX2 activation is bound to
// (engine make_retail_machine_tokens carries the witness).
opennova::LobbyMachineTokens machine_tokens();

// The CD-key ids this machine's activation gives a session of `protocol` (engine
// activation_cd_keys over the process's menu-browser jar and machine_tokens().nwpssk): the CDKIID
// a LAN join sends and the one a LAN host refuses as its own. None for a protocol with no CD key,
// or with no activation in the jar (engine activation_cd_keys carries the witness).
opennova::cdkey::WireKeys machine_activation_cd_keys(const opennova::Protocol *protocol);

} // namespace godot
