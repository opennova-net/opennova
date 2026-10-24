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
// The gates are per verb, as retail's arms test them: every verb but
// SetMPReset needs the authority with its hosted session up (retail's
// is_authority and ctx+0x68; is_in_session here, standing in for ctx+0x68
// only, so a context that has no session yet keeps it clear); the
// player-targeted verbs and ChangeTeam / SwapTeam also need the player table
// (a World here); a verb that reads an argument it cannot default needs its
// token; SetMPReset needs only its argument, so it runs on any receiver this
// executor is handed. Both hosts hand it a session-less context for a command
// that arrives before their match binds (host_config.h run_host_server_command:
// opennova-serve's ServeListing::on_command, the game shell's
// SessionDrive / Simulation::execute_server_command), where only SetMPReset
// passes, as retail's hosting wait runs it. The World-acting verbs
// retail leaves off the player-table gate (Cycle / EndMission / GameOver,
// Earthquake, Lightning, TimeOfDay) still need a World to act on
// (docs/net/novaworld-net-re.md D-NET-383). Numbers are the CRT atol's, 32-bit
// saturating on every host, and an index past the slot capacity is no slot
// (D-NET-387). A targeted verb
// resolves its slot ByIndex (atol -> roster slot), ByIpAndPort ("host:port"
// split at the first ':', the host through inet_addr with DNS off and the port
// the napi literal, against every slot connection's UDP source, the listen
// host's own loopback 0.0.0.0:0 included; D-NET-467), ByName ("*NN" -> slot NN, else the
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
// HostSetup / Host vars; the next session create ends the process on a
// nonzero mpreset, create_session's ProcessExit), ChangeTeam /
// SwapTeam (the team 1 <-> 2 swap through Server_ChangeEntityTeam, then the
// "Changing team...." chat to the slot).
// ReloadPlayer (Entity_UpdateWeaponOverlayFrameState) and DisarmPlayer are
// not modeled on this host and return handled = false.
// [orig: the ServerCommand handler CNapiGameSession_HandleServerCommand @0x4D22F0 —
//  PuntPlayer's gates @0x4D23C0..0x4D23E7 and each arm's own copy, SetMPReset's lone token
//  gate @0x4D2E12,
//  the target suffixes @0x4D23F2..0x4D2505, PuntPlayer @0x4D2515..0x4D254D,
//  TextChatServer @0x4D25A5..0x4D25E6, TextChatPlayer @0x4D2738..0x4D2765,
//  CmdEchoPlayer @0x4D287F..0x4D28AC, KillPlayer @0x4D29C6..0x4D29EC,
//  Cycle/EndMission/GameOver @0x4D30DE..0x4D31CA, Earthquake
//  @0x4D2AC2..0x4D2B13, Lightning @0x4D2B5D..0x4D2BAC, TimeOfDay
//  @0x4D2BE3..0x4D2CA1, SetServerName @0x4D2CF5..0x4D2DDF, SetServerMsg
//  @0x4D2D8A..0x4D2DDF, SetMPReset @0x4D2E1B..0x4D2E2D, ChangeTeam / SwapTeam
//  @0x4D31EA..0x4D3360]
struct ServerCommandOutcome {
	bool handled = false;        // the verb matched and passed its gates
	bool stop_hosting = false;   // PuntPlayer aimed at the host's own slot
	bool config_changed = false; // persist game.cfg + republish the host vars
};

ServerCommandOutcome Server_ExecuteServerCommand(NapiNPServerCtx &ctx, world::World *world,
		std::string_view verb, std::string_view target_suffix,
		const std::vector<std::string> &args);

// The environment clock word an HHMM sets: hours = floor(hhmm * 0.01f) (the float constant
// widened, at the 53-bit precision the port keeps: an exact hundred reads one hour short, as a
// dedicated host's 53-bit and JO:CA's 64-bit x87 compute it, where JO's 24-bit mode rounds to
// the hour), minutes = hhmm % 100, then the word
// ((minutes + 60 * hours) / 60) << 24 | (minutes % 60) * 0x44400, every step 32-bit and signed
// as retail's. The ServerCommand TimeOfDay and the tod input row pack the same word.
// [orig: CNapiGameSession_HandleServerCommand @0x4D22F0 - `fmul flt_7C56A8` @0x4D2C24, _floor
//  called @0x4D2C3F, the packing @0x4D2C49..0x4D2C91, Environment_SetCurrentTime called
//  @0x4D2C94; Input_HandleActionBinding @0x49AD40 - case 438 the same @0x49BDC5..0x49BE42;
//  flt_7C56A8 = 0x3C23D70A (0.01f)]
uint32_t environment_time_from_hhmm(int32_t hhmm);

// The authority's input rows, each its catalog row's case past the dispatcher's head
// (Role::dispatch_record carries the gates of the role it runs on):
//
// netdelay (row 89, code 103): in a session on the authority, a delay of 0..100 ticks
// (unsigned) goes out twice to every in-match slot but the host's own (mask 0x90), reliable:
// S2C 0x1B [i32 delay], then the S2C 0x14 chat [0][0xFF]["Delay set to <n>ms"], n = 1000 *
// delay * the send holdoff / 62, 32-bit signed. The authority's own store of the word is not
// kept: its only other access is the client's 0x1B handler, and nothing reads it
// (NapiNPClientMsg_HandleNetworkDelay @0x426080, xrefs @0x42609C / @0x4260A4 / @0x50CC08).
// False when nothing went.
// [orig: Input_HandleActionBinding @0x49AD40 - case 103 @0x49BB73; Server_SetNetworkDelay
//  @0x50CBC0 - the gates @0x50CBC9..0x50CBF0, g_NetworkDelayTicks @0x50CC08, mask 0x90
//  @0x50CC0E, SendFiltered(0x1B, 1) @0x50CC28, dword_24D2124 (NapiNPServer_GetSendHoldoffTicks,
//  stored @0x4C9CE3) @0x50CC2D..0x50CC4D, sprintf @0x50CC5A, NetPacket_WriteTwoBytesAndCString
//  (.., 0xFF, 0, text) @0x50CC7F (byte2 first @0x5047C1, byte1 @0x5047CE), SendFiltered(0x14, 1)
//  @0x50CC9D]
bool Server_SetNetworkDelay(NapiNPServerCtx &ctx, int32_t delay_ticks);

// resetgames (row 92, code 106), in a session on the authority: the round ends with winner 0
// (Server_ProcessRoundEnd, its sends at the call), then team 1's and team 2's round wins and
// the rounds played clear (teams 3 and 4 keep theirs).
// [orig: Input_HandleActionBinding @0x49AD40 - case 106 @0x49BB82..0x49BB9A;
//  Server_ForceRoundEndAndClearState @0x5175A0 - Server_ProcessRoundEnd(0) @0x5175A2,
//  g_RoundWinsTeam1 @0x5175AA, g_RoundWinsTeam2 @0x5175B4, g_TotalRoundsPlayed @0x5175BE]
void Server_ForceRoundEndAndClearState(NapiNPServerCtx &ctx, world::World &world);

} // namespace opennova::inmatch
