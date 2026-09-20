#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <runtime/inmatch/napi_np_server_ctx.h>

namespace opennova::world {
class World;
}

namespace opennova::inmatch {

// The in-match side of a NovaWorld "ServerCommand" (the service's Cmd string,
// parsed by net/napi parse_server_command into verb + target suffix + args):
// the host executes the verb against its own roster. `verb` and
// `target_suffix` are the token spellings ("PuntPlayer", "ByName", ...),
// compared case-insensitively as retail does; `args` are the tokens after the
// verb, so args[0] is the player-target token of a targeted verb.
//
// Every verb needs the authority in a session with a World (retail's
// is_authority / is_in_session / the player-slot table); a targeted verb
// resolves its slot ByIndex (atol -> roster slot), ByIpAndPort ("a.b.c.d:port"
// against the connection's UDP source), ByName ("*NN" -> slot NN, else the
// unique case-insensitive callsign) or ByPCID (the slot's entity type name —
// every pool-0 player shares one items.def name on this host, so it resolves
// nothing), and no suffix is a no-op.
//
// Live verbs: PuntPlayer (the host's own slot -> stop_hosting for the shell,
// a remote -> the chat-coded punt 39 naming the target token), TextChatServer
// / TextChatPlayer / CmdEchoPlayer (0x14 channel 10 / 10 / 14 from slot 0xFF),
// KillPlayer (health 0 -> the death transaction), Cycle / EndMission /
// GameOver (the round end with the parsed winner, linger 620), Earthquake,
// Lightning (the flash + the S2C 0x24 "SETFLASH1 16" text command),
// TimeOfDay (HHMM), SetServerName / SetServerMsg / SetMPReset (config;
// config_changed asks the shell to save game.cfg and republish the NovaWorld
// HostSetup / Host vars). ChangeTeam / SwapTeam (Server_ChangeEntityTeam),
// ReloadPlayer (Entity_UpdateWeaponOverlayFrameState) and DisarmPlayer are
// not modeled on this host and return handled = false.
// [orig: the ServerCommand handler loc_4D22F0 — gates @0x4D23C0..0x4D23E7,
//  the target suffixes @0x4D23F2..0x4D2505, PuntPlayer @0x4D2515..0x4D254D,
//  TextChatServer @0x4D25A5..0x4D25E6, TextChatPlayer @0x4D2738..0x4D2765,
//  CmdEchoPlayer @0x4D287F..0x4D28AC, KillPlayer @0x4D29C6..0x4D29EC,
//  Cycle/EndMission/GameOver @0x4D30DE..0x4D31CA, Earthquake
//  @0x4D2AC2..0x4D2B13, Lightning @0x4D2B5D..0x4D2BAC, TimeOfDay
//  @0x4D2BE3..0x4D2CA1, SetServerName @0x4D2CF5..0x4D2DDF, SetServerMsg
//  @0x4D2D8A..0x4D2DDF, SetMPReset @0x4D2E1B..0x4D2E2D]
struct ServerCommandOutcome {
	bool handled = false;        // the verb matched and passed its gates
	bool stop_hosting = false;   // PuntPlayer aimed at the host's own slot
	bool config_changed = false; // persist game.cfg + republish the host vars
};

ServerCommandOutcome Server_ExecuteServerCommand(NapiNPServerCtx &ctx, world::World *world,
		std::string_view verb, std::string_view target_suffix,
		const std::vector<std::string> &args);

} // namespace opennova::inmatch
