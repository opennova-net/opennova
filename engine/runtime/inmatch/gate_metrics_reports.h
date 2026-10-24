#pragma once

// THE AUTHORITY'S METPROTOCOL GATE REPORTS (D-NET-NEW-L9-22): the CServerTick phase clocks the
// SERVER, SERVERMISSION, PLAYER and PLAYERMISSION reports print (the process's and its mission's
// on the host context, each player slot's two on its row), their writers at retail's call
// positions, and the reports' sends through the sink a NovaWorld embedder installs. The texts are
// net/novaworld/gate_metrics.h's encoders; the datagram and the gate address are the NovaWorld
// session's (NwuLobbySession::send_gate_metrics).
// [orig: CServerTick_Construct @0x5017D0; CServerTick_SetState @0x4FA090; CServerTick_Reset
//  @0x4F9F40; Server_SendServerMetricsToGate @0x4FA4B0; Server_SendMissionMetrics @0x4FB3A0;
//  Server_SendPlayerMetricsToGate @0x4FAA30; Server_SendPlayerMissionMetrics @0x4FB8F0]

#include <net/novaworld/gate_metrics.h> // GateServerTick, GateMetricsHost, GateMetricsPlayer
#include <runtime/hud/game_text_lookup.h>

#include <cstdint>
#include <functional>
#include <string>

namespace opennova {
struct ClientAuth; // net/npwire/session_hello.h
}

namespace opennova::inmatch {

struct NapiNPServerCtx;
struct NapiNPConnection;

// The reports' sink: a NovaWorld embedder installs it while its session hosts the match and
// clears it as the match goes; unset (no `send`), no report is built, as retail's reporters return
// off the NovaWorld network type. `host` reads the strings every report prints (the gate reply's
// METLABEL, LOBBYNAME and METEXT, the version text and the machine locale), `game_text` resolves
// the GAMETYPE row's GateTypeAbbrev text, and `send` hands one report's text to the session, which
// sends nothing while no gate address resolved (the reporters' g_GateAddress test).
// [orig: Server_SendServerMetricsToGate @0x4FA4B0 - the gates @0x4FA4EA (is_in_session, the
//  NovaWorld network type, g_GateAddress), GameType_GetAbbreviation(g_GameType, 1) @0x4FA5F8]
struct GateMetricsSink {
	std::function<GateMetricsHost()> host;
	hud::GameTextLookup game_text;
	std::function<void(const std::string &text)> send;
};

// The process's two phase clocks: SERVER's, which runs from the session create to the session's
// end, and SERVERMISSION's, which each linger expiry constructs again for the next mission.
// [orig: g_ProcessServerTick @0xC87020; g_MissionServerTick @0xC87598; each built at static init by
//  CServerTick_Ctor @0x511B50 (the zeroing @0x511BA2, CServerTick_Construct @0x511BAC)]
struct GateMetricsClocks {
	GateServerTick process;
	GateServerTick mission;
};

// A joiner's ClientAuth strings the player reports print: VERSIONSTRING (31 characters), COUNTRYCODE
// (7), CNT (31), LNG (31) and TZB (atol), the tag list applied in order, a later duplicate over an
// earlier one, the tags compared case-insensitively. The PID is the join's PCID cookie
// (NapiNPConnection::account), filled at the report.
// [orig: NapiNetConfig_LoadFromConnTags @0x4C7260 - VERSIONSTRING @0x4C73B6 ->
//  NapiNetConfig_SetVersionString @0x4C2650 (Napi_CopyString 32 @0x4C265B), COUNTRYCODE @0x4C73DB ->
//  NapiNetConfig_SetCountryCode @0x4C2670 (8 @0x4C267B), CNT @0x4C7669 -> NapiNetConfig_SetCnt
//  @0x4C26F0 (32 @0x4C26FE), LNG @0x4C768B -> NapiNetConfig_SetLng @0x4C2710 (32 @0x4C271E), TZB
//  @0x4C76AD (atol @0x4C76C2)]
GateMetricsPlayer parse_client_gate_metrics_player(const ClientAuth &auth);

// The slot counts CServerTick_UpdateMaxPlayerCounts takes: every active slot but a bot, the
// local slot only on a host that is a session peer; under a team game type, its team byte's
// counts too. An orphaned row is an active row, and a Serve Only host's own slot 0 a local one.
// [orig: CServerTick_UpdateMaxPlayerCounts @0x4FA130 - the walk @0x4FA15C..0x4FA1B1 (+4, the bot
//  byte +96483, +5 unless is_mp_session_peer @0x4FA180), g_GameType & 0x10000 @0x4FA18B, the team
//  byte +416 @0x4FA197 / @0x4FA19C]
void gate_update_max_player_counts(const NapiNPServerCtx &ctx, GateServerTick &tick);

// The session create's: both process clocks constructed over the install's CC.BIN, which the
// context keeps for every later construct (retail reads the same file at each one), and the
// config's player count. The authority's session reset ahead of the create constructs them the
// same way. Every clock reads the context's tick_count_ms.
// [orig: CNapiGameSession_CreateSession @0x4C97C0 - CServerTick_Construct on g_ProcessServerTick
//  @0x4C9CE8 and g_MissionServerTick @0x4C9CF2; Session_ResetFrameCounters @0x51E7F0 ->
//  Server_InitServerTicks @0x5018C0 (the call @0x51E838) ahead of it]
void gate_clocks_session_create(NapiNPServerCtx &ctx, std::string cc_bin);

// The authority's mission start: both process clocks into phase 1, loading the mission.
// [orig: Game_StartMission @0x524360 - the authority test @0x52448C, CServerTick_SetState(1) on
//  g_ProcessServerTick @0x5244A4 and g_MissionServerTick @0x5244AF]
void gate_clocks_mission_start(NapiNPServerCtx &ctx);

// The authority's mission start past the load: both process clocks into phase 2, waiting to
// begin, while the pre-round delay runs, else 3, playing.
// [orig: Game_StartMission - the authority test @0x525E19, g_PreRoundDelayTimer @0x525E31,
//  CServerTick_SetState on g_ProcessServerTick @0x525E42 and g_MissionServerTick @0x525E4D]
void gate_clocks_mission_loaded(NapiNPServerCtx &ctx, uint32_t preround_delay_seconds);

// A remote add's row: both slot clocks constructed once the row is active.
// [orig: Server_PlayerAdd @0x51CBC0 - the row memset @0x51CD06, the active byte @0x51CD14,
//  CServerTick_Construct on +0x17D20 @0x51CD1B and +0x18294 @0x51CD26]
void gate_slot_clocks_player_added(NapiNPServerCtx &ctx, NapiNPConnection &slot);

// A remote add's high marks on both process clocks, late in the add, its row holding its team.
// [orig: Server_PlayerAdd - CServerTick_UpdateMaxPlayerCounts on g_ProcessServerTick @0x51D2EE
//  and g_MissionServerTick @0x51D2F8, after the team's assignment and ahead of the /PROFILE row
//  @0x51D2FD]
void gate_clocks_player_added_marks(NapiNPServerCtx &ctx);

// A net player's game state change, called ahead of the state's store with the clock now (the
// context's tick_count_ms, or in a record's handler the connection's receive stamp, the host
// clock at the datagram's receive) and the pre-round delay: on a change, entering 9 sets both of
// its row's clocks to phase 1, 10 to 2 while the pre-round delay runs else 3, and 11 to 4. The
// same state is no change.
// [orig: CNetPlayer_SetGameState @0x4C4060 - the unchanged-state return @0x4C407D, the row +0xC0
//  @0x4C4084; 9: CServerTick_SetState(1) on +97568 @0x4C41BD and +98964 @0x4C41CA; 10:
//  g_PreRoundDelayTimer @0x4C41F2, 2 @0x4C41F6 / @0x4C41FD, 3 @0x4C4201 / @0x4C420E; 11: 4
//  @0x4C4257 / @0x4C4264]
void gate_slot_clocks_game_state(NapiNPConnection &net_player, uint8_t new_state, uint32_t now_ms,
                                 uint32_t preround_delay_seconds);

// The session's end on the authority (the Post Menu's destroy arm): both process clocks' high
// marks, both phases closed, the SERVER report over the process clock, then both reset (the
// process clock's GCC and MAXPLAYERS go with it, so the session's later reports print GCC "").
// [orig: PostMenu_RouteMissionExit @0x568460 - Server_EndMissionSession called @0x568672;
//  Server_EndMissionSession @0x51E850 - is_authority @0x51E865, the call @0x51E867;
//  Server_ProcessPeriodicTimerUpdate @0x4FBF50 - the marks @0x4FBF56 / @0x4FBF60, the phases
//  closed @0x4FBF72..0x4FC040, Server_SendServerMetricsToGate @0x4FC07F, CServerTick_Reset
//  @0x4FC089 / @0x4FC093]
void Server_ProcessPeriodicTimerUpdate(NapiNPServerCtx &ctx);

// A player's leave on the authority, a slot not the local one: its slot clock to phase 0, then the
// PLAYER report over it.
// [orig: Server_HandlePlayerDisconnect @0x51B5C0 - is_authority @0x51B5D0, the active byte
//  @0x51B5DC, the local byte's return @0x51B606 and test @0x51B680, CServerTick_SetState(0) on
//  +0x17D20 @0x51B690, Server_SendPlayerMetricsToGate @0x51B696]
void Server_ReportPlayerLeave(NapiNPServerCtx &ctx, NapiNPConnection &slot);

// The same leave for a held server's player, torn down by the next host's session reset ahead of
// its session create, so no context of the new session reads it yet: the new host's setup runs
// once its session hosts, as the authority (HostDialog_StartSession's connection mode), with the
// menu's network type standing and is_in_session set with it. The reports read what that setup
// left: g_GameType the host's first selected mission's (`game_type`), and the process clock's
// GCC the install's CC.BIN, which Server_InitServerTicks read again ahead of the reset
// (`process_gcc`). The slot clock to phase 0 at `now_ms`, then the PLAYER report through `sink`
// on a NovaWorld host (`novaworld`).
// [orig: HostDialog_StartSession @0x558760 - CGameSession_SetConnectionMode @0x558787 /
//  @0x55878B, g_GameType @0x5587AE and the first selected mission's @0x5588A8,
//  Session_ResetForStart (the tail jump @0x5588C5) -> Session_ResetFrameCounters @0x51E7F0 (the
//  authority test @0x51E830) -> Server_InitServerTicks @0x5018C0 (Game_ReadCCBinFile @0x5018E8,
//  byte_C87044 @0x501900); CNapiNetwork_SetNetworkType @0x4C4A50 - +0x50 @0x4C4A82, is_in_session
//  @0x4C4A85; PreMenu_HostSetup @0x5691F0 - CNapiGameSession_ResetActiveSession called @0x5691F5;
//  Server_HandlePlayerDisconnect @0x51B5C0 - CServerTick_SetState(0) @0x51B690,
//  Server_SendPlayerMetricsToGate @0x51B696 (its gates @0x4FAA5B..0x4FAA8F)]
void Server_ReportHeldPlayerLeave(const GateMetricsSink &sink, bool novaworld, uint32_t game_type,
                                  const std::string &process_gcc, uint32_t now_ms,
                                  NapiNPConnection &slot);

// The round-end linger's expiry on an in-session authority: the mission clock's phase closed, the
// mission file copied in, the SERVERMISSION report, the mission clock constructed again over the
// process clock's GCC and MAXPLAYERS; then per slot in slot order its mission clock's phase
// closed, the PLAYERMISSION report (a joined slot's), and the clock constructed again.
// [orig: Server_TickUpdate @0x51D7E0 - the expiry @0x51D9C7, the mission clock @0x51D9D4..0x51DA3B,
//  Napi_CopyString(g_MapFileName, 0x20) @0x51DA52, Server_SendMissionMetrics @0x51DA5A,
//  CServerTick_Construct @0x51DA64, the file again @0x51DA7B, byte_C87044 @0x51DA92, dword_C870A4
//  @0x51DA97..0x51DAA5, the marks @0x51DAAB; the slot walk @0x51DAD0..0x51DB45 (stride 0x188E8):
//  the phase closed @0x51DAD0..0x51DB1B, Server_SendPlayerMissionMetrics @0x51DB1F,
//  CServerTick_Construct @0x51DB29]
void Server_ReportLingerExpiry(NapiNPServerCtx &ctx);

} // namespace opennova::inmatch
