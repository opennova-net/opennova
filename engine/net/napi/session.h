#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <net/napi/tlv.h>

namespace opennova {

// Session-level shapes for the NAPI NovaWorld handshake. Witnessed in
// jodemo.exe:
//   CNapiGameSession_ConnectOrHost@0x4b08e0  (state polling + error map)
//   CNapiGameSession_SendConnected@0x4add40  ("ClientConnected" sender)
//   CNapiGameSession_SendHostRequest@0x4af7d0 ("ClientHostRequest")
//   CNetClient_SendHostUpdate@0x4af8b0       ("ClientHostUpdate")
//   CNapiGameSession_SendPlayRequest@0x4d3920 ("ClientPlayRequest", retail —
//     the prior 0x4af990 citation was Entity_CheckTripleLOS, a wrong anchor)
//   CNapiGameSession_SendStopHosting@0x4ae320 ("ClientStopHosting")
//   CNapiGameSession_SendStopPlaying@0x4ae3b0 ("ClientStopPlaying")
//   CNapiNetwork_RandomizeTimeout@0x4a6d50   (the per-session AppId random;
//     retail Jointops equivalent is @0x4c4d80 — see SESSION_APPID_RANDOM_*)

// Session state codes as witnessed in `dword_989574` polling loops inside
// CNapiGameSession_ConnectOrHost. The values match the jodemo globals so
// reviewers can cross-reference them to the original state machine.
// State transitions are driven by CNapiGameSession_ProcessConnect (not
// ported here — runtime/polling work belongs to the apps layer).
enum class SessionState : int {
	Idle = 0,              // pre-connect (value is a convenience; jodemo uses other codes here)
	HostStarting = 5,      // local side is bringing up a listening host
	HostEstablished = 6,   // host is up and accepting joiners
	Connecting = 7,        // client is mid-handshake with the host
	Connected = 8,         // client handshake succeeded
};

// Handshake timing constants (witnessed as immediates in the binary).
// [D-NET-23] The connect/host poll loop in [orig: CNapiGameSession_ConnectOrHost @0x4d4f10] times out
// at 0xEA60 = 60000 ms (BOTH the start-playing and start-hosting GetTickCount loops). The 20000 ms
// (0x4E20) value is the SEPARATE periodic-update background timeout [orig: ProcessPeriodicUpdate
// @0x4d4400] — kept below as its own named constant (the old 20000 here mis-cited the connect poll).
constexpr uint32_t SESSION_CONNECT_TIMEOUT_MS = 60000u;          // 0xEA60 — ConnectOrHost poll
constexpr uint32_t SESSION_PERIODIC_UPDATE_TIMEOUT_MS = 20000u;  // 0x4E20 — ProcessPeriodicUpdate
// [D-NET-24] 1300 is a MESSAGE CHUNK SIZE in bytes (the QueueMessage fragment limit), not a ms
// interval — renamed from the misnomer SESSION_HANDSHAKE_RETRANSMIT_MS. [orig: CNapiNPConnection_QueueMessage @0x628640]
constexpr uint32_t SESSION_MESSAGE_CHUNK_BYTES = 1300u;
// The per-session HostSetup/Host "AppId": (GetTickCount() + rand()) % 0x2328 + 1000, minted once
// per host registration. It is NOT a timeout (the old SESSION_TIMEOUT_RANDOM_* name misread the
// function): its only readers are the two var-list builders, the server status screen, the
// join validator and the session creator. [orig: CNapiNetwork_RandomizeTimeout @0x4c4d80
//  (`% 0x2328` @0x4c4d9a, `+ 1000` @0x4c4da3); sole caller
//  CNapiGameSession_BuildHostVarLists @0x4D0B50 (the call @0x4d0b6e), which
//  reads it back through the getter sub_4C4DB0 @0x4C4DB0 (the call @0x4d0c6a)]
constexpr uint32_t SESSION_APPID_RANDOM_MIN = 1000u;
constexpr uint32_t SESSION_APPID_RANDOM_MAX = 9999u;  // 0x2328 == 9000; +1000 -> [1000,9999]
inline uint32_t make_session_app_id(uint32_t tick_count, int rand_value) {
	return (static_cast<uint32_t>(rand_value) + tick_count) % 0x2328u + 1000u;
}
// The stage-retransmit cadence of the connect legs: the enumerator re-announces the 0x41 every
// 3000 ms and the connecting NP connection re-sends its 0x42 join on the same interval while
// its connect deadline runs; the gate probe worker repeats the probe every 3000 ms for 30000 ms.
// [orig: CNapiGameSession_InitTransportConnection @0x4c9f9d (announce_interval_ms = 3000);
//  CNapiNPConnection_PumpStateMachine @0x6292e0 case 3 @0x629570..0x629595 (the join re-send);
//  CNapiGateManager_Init @0x633f90 stores @0x634052 (3000) / @0x63405c (30000);
//  CNapiGateManager_ProbeThreadProc @0x6339e0 @0x633a5e (deadline) / @0x633a70 (retry)]
constexpr uint32_t SESSION_CONNECT_RETRANSMIT_MS = 3000u;
constexpr uint32_t SESSION_GATE_PROBE_RETRY_MS = 3000u;
constexpr uint32_t SESSION_GATE_PROBE_TIMEOUT_MS = 30000u;
// The periodic-update GLSVSS poll: once the NP connection is established (conn_state 5) and the
// lobby session sits in state 4 (verified), the session checks its GLSVSS deadline at most once
// per 1000 ms. [orig: CNapiGameSession_ProcessPeriodicUpdate @0x4d4400 @0x4d44e3]
constexpr uint32_t SESSION_GLSVSS_POLL_MS = 1000u;
// The host's server-info refresh: Server_TickUpdate reloads g_serverinfo_update_timer to 0x744
// logic ticks, advances the PCID cookie-key ring and republishes the Host list.
// [orig: Server_TickUpdate @0x51d7e0 @0x51d91d..0x51d948]
constexpr uint32_t SESSION_HOST_INFO_REFRESH_TICKS = 0x744u;

// The six-slot cookie-key ring behind the Host list's "PCIDKey": every refresh advances the
// index (wrapping at 6), mints ((rand() + GetTickCount()) & 0xFFFFFF) + 0x1000000 into that
// slot and caps the population count at 6. PCIDKey is the CURRENT slot's key.
// [orig: CSessionIdRing_AdvanceAndGenerate @0x4dbb80; the PCIDKey reader
//  Lobby_UpdateServerInfo @0x4ff1ab (`keys[keys[6]]`)]
struct SessionIdRing {
	std::array<uint32_t, 6> keys{};
	uint32_t index = 0;
	uint32_t count = 0;
	uint32_t advance(uint32_t tick_count, int rand_value) {
		if (++index >= 6) index = 0;
		keys[index] = ((static_cast<uint32_t>(rand_value) + tick_count) & 0xFFFFFFu) + 0x1000000u;
		if (count < 6) ++count;
		return count;
	}
	uint32_t current() const { return keys[index]; }
};

// Gate-server error codes as observed in dword_989588 after a failed
// handshake, and their user-facing NWEC strings (per jodemo's mapping).
enum class NovaWorldError : int {
	Ok = 0,

	// Generic timeout/polling outcomes (set by the polling loop, not by a
	// server response):
	TimeoutPoll = -1,      // loop timed out without reaching state 6/8 -> NWEC02
	UserCancelled = -2,    // ESC pressed during client-side polling -> NWEC03

	// Server-side rejection codes (from dword_989588):
	Banned = 200,          // -> NWEC11 (matches novaworld §3.4)
	Restricted = 203,      // -> NWEC12 (matches novaworld §3.4)
	Reject3000 = 3000,     // -> NWEC04
	Reject3001 = 3001,     // -> NWEC05
	Reject3002 = 3002,     // -> NWEC06
	Reject3003 = 3003,     // -> NWEC07
	Reject3004 = 3004,     // -> NWEC08
	Reject3005 = 3005,     // -> NWEC09
	Reject3006 = 3006,     // -> NWEC10
	Reject1009 = 1009,     // -> NWEC14 (witnessed special-case reject; [D-NET-25] @0x4d4f10)
	UnknownReject = 0x7FFFFFFF, // sentinel for an unrecognized NONZERO reject code -> NWEC13 (the
	                            // dword_B60110 default branch, distinct from the NWEC02 poll timeout)
};

// Map an error to its user-facing NWEC tag, or empty string if unknown.
std::string novaworld_error_tag(NovaWorldError err);

// Map a raw server reject code to its NovaWorldError enum (identity for
// known codes, returns Reject3000 for unknown >= 3000 and TimeoutPoll for
// others to match the binary's default branch).
NovaWorldError novaworld_error_from_code(int code);

// The start-playing leg's other outcomes: the poll timeout (NWEC02), the user escape
// (NWEC03), a rejected SendPlayRequest (NWEC59) and the not-in-the-right-state gate
// (NWEC49). [orig: CNapiGameSession_ConnectOrHost @0x4d4f10 — NWEC02 @0x4d553e, NWEC03
//  @0x4d558a, NWEC59 @0x4d549c, NWEC49 @0x4d56f6]
inline constexpr char NWEC_PLAY_TIMEOUT[] = "NWEC02";
inline constexpr char NWEC_PLAY_CANCELLED[] = "NWEC03";
inline constexpr char NWEC_PLAY_START_FAILED[] = "NWEC59";
inline constexpr char NWEC_WRONG_SESSION_STATE[] = "NWEC49";
// The start-hosting leg: gate info not OK (NWEC50), SendHostRequest failed (NWEC51), the 60 s
// poll timeout (NWEC52), and the rejection map over the ServerHostResult MsgCode stored at
// session+304: 0x3E9 -> NWEC53, 0x3EC -> NWEC54, 0x3ED -> NWEC55, 0x3F0 -> NWEC56,
// 0x3F1 -> NWEC57, 0x3F3 -> NWEC60, anything else NWEC58.
// [orig: ConnectOrHost @0x4d4f10 — NWEC50 @0x4d50dc, NWEC51 @0x4d5165, NWEC52 @0x4d52a4,
//  the switch @0x4d5207..0x4d52f1]
inline constexpr char NWEC_HOST_GATE_NOT_OK[] = "NWEC50";
inline constexpr char NWEC_HOST_START_FAILED[] = "NWEC51";
inline constexpr char NWEC_HOST_TIMEOUT[] = "NWEC52";
std::string novaworld_host_error_tag(int msg_code);

// The gate-probe failure map of the LAN/NovaWorld UI state machine: a negative
// CNapiGateManager_WaitForConnect result in state 1 maps -2..-8 -> NWEC18..NWEC24 with the
// NWEC15 default; the gate_state check in state 3 maps the same seven plus -9 -> NWEC25
// with the NWEC16 default. `response_phase` selects the state-3 table.
// [orig: UI_ProcessLANSessionStateMachine @0x558de0 — state 1 switch @0x558ec6..0x55901b,
//  state 3 switch @0x5590c5..0x559109]
std::string novaworld_gate_error_tag(int gate_result, bool response_phase);

// The error-display fallback: a null or empty tag shows the "CVUNKNOWN" menutxt entry.
// [orig: sub_5583E0 @0x5583e0]
inline constexpr char NWEC_UNKNOWN_TAG[] = "CVUNKNOWN";
// The menutxt key the punt notification substitutes the MsgCode into ("[[$]]").
// [orig: CNapiGameSession_HandlePuntNotification @0x4d20b0]
inline constexpr char MENUTXT_PUNTED_FROM_NOVAWORLD[] = "ERR_PUNTEDFROMNOVAWORLD";

// The 52-entry ServerStopHosting MsgCode -> NWUSERVERMSGCODE_* key table (the string the
// client latches for its message log; unknown codes yield NWUSERVERMSGCODE_UNKNOWNERROR).
// [orig: CNapiGameSession_HandleServerMessage @0x4d1c50 walks dword_7CB960[2*i] against
//  the code (52 rows, `>= 0x34` -> the unknown key) and reads off_7CB964[2*i]]
std::string novaworld_server_msg_code_key(int msg_code);

// The Success/MsgCode/MsgParam1/MsgParam2 quartet every Server*Result and the three
// server notifications carry as top-level params, parsed with atol (any nonzero integer is
// success). Names are matched case-insensitively (Napi_StrCaseEqual).
// [orig: HandleHostVerifyResponse @0x4d59d0, HandleVerifyResponse @0x4d1e00,
//  HandleServerMessage @0x4d1c50, HandleServerDisconnectMsg @0x4d1fa0,
//  HandlePuntNotification @0x4d20b0 — all four atol the same four params]
struct ServerResultFields {
	int success = 0;
	int msg_code = 0;
	int msg_param1 = 0;
	int msg_param2 = 0;
};
ServerResultFields parse_server_result_fields(const NapiMessage &container);

// The "HostCommands" ServerVarList inside a successful ServerHostResult: every ServerVar
// child's VarName/VarValue (VarFNum-typed, but the two consumers read fnum 0), from which the
// client reads HostRequiresJoinTicket (int) and GSID (128-char cap).
// [orig: CNapiGameSession_HandleHostCommandVarList @0x4d3440]
std::map<std::string, std::string> parse_host_commands(const NapiMessage &result);

// Handshake message builders. These construct the TLV tree that would be
// serialized and sent via CNapiSession_SendMessage. The runtime layer is
// expected to wrap them with the envelope + retransmit bookkeeping.

NapiMessage make_client_connected();
NapiMessage make_client_stop_hosting();
NapiMessage make_client_stop_playing();

// One ClientVar entry inside a ClientVarList: VarFNum (a field number) + VarName
// + VarValue — the wire shape NapiStatement_SerializeVarList @ 0x4d0660 emits
// and our server (lobby_session extract_var_lists) parses.
struct ClientVar {
	int fnum = 0;
	std::string name;
	std::string value;
};

// Build a "ClientVarList" container: a "VarList" field carrying the list name
// ("Cookie"/"PlaySetup"/...) then one "ClientVar" child per entry (VarFNum/
// VarName/VarValue). [orig: NapiStatement_SerializeVarList @ 0x4d0660]
NapiMessage make_client_var_list(const std::string &list_name,
                                 const std::vector<ClientVar> &vars);

// ClientPlayRequest: a top-level "CurrentlyPlaying" field (decimal of the flag)
// FIRST, then the "Cookie" var-list, then the "PlaySetup" var-list, in that
// order — NOT two containers literally named PlaySetup/Cookie.
// [orig: CNapiGameSession_SendPlayRequest @ 0x4d3920]
NapiMessage make_client_play_request(int currently_playing,
                                     const std::vector<ClientVar> &cookie,
                                     const std::vector<ClientVar> &play_setup);

// The PlaySetup var-list the start-playing leg fills before SendPlayRequest: ServerName (the
// browsed row's name), IpAddress / PortNumber (the .joi-decoded NK host and port strings),
// AppId (the decoded CK join token) and Lan (the .joi LN lobby number, an int var).
// [orig: CNapiGameSession_ConnectOrHost @0x4d53d1..0x4d542b — the list at session+676]
std::vector<ClientVar> make_play_setup_vars(const std::string &server_name,
                                            const std::string &ip_address,
                                            const std::string &port_number,
                                            const std::string &app_id, int lan);

// ClientHostRequest: a top-level "CurrentlyHosting" field (decimal of the flag)
// FIRST, then "VarCheck"="1", then the Cookie, HostSetup, Host, and PlayerList
// var-lists in that order (each a ClientVarList carrying a VarList name + one
// ClientVar child per entry) — NOT containers literally named HostSetup/Host.
// [orig: CNapiGameSession_SendHostRequest @ 0x4d3700 — two NapiStatementParam_Create
// (CurrentlyHosting, VarCheck) then four NapiStatement_SerializeVarList @ 0x4d0660
// for "Cookie"(this+388) / "HostSetup"(+460) / "Host"(+532) / "PlayerList"(+604)]
NapiMessage make_client_host_request(int currently_hosting,
                                     const std::vector<ClientVar> &cookie,
                                     const std::vector<ClientVar> &host_setup,
                                     const std::vector<ClientVar> &host,
                                     const std::vector<ClientVar> &player_list);

// ClientHostUpdate: the Host and PlayerList var-lists only — no params, no
// Cookie/HostSetup (the heartbeat refresh). [orig: CNapiGameSession_SendHostUpdate
// @ 0x4d3860 — NapiStatement_SerializeVarList for "Host"(this+532) / "PlayerList"(+604)]
NapiMessage make_client_host_update(const std::vector<ClientVar> &host,
                                    const std::vector<ClientVar> &player_list);

// ClientHostPlayerAdded: the host's per-player roster notification — PlayerNumber (the
// slot index), PlayerName, PlayerIpAndPort ("a.b.c.d:port" of the player's game endpoint),
// PlayerPCID, PlayerTeam and PlayerType, in that order. Fired only while the session is in
// state 6 (hosting established). [orig: CNapiGameSession_SendPlayerAdded @0x4cfec0; the
//  state-6 wrapper CNapiGameSession_OnPlayerChat @0x4d0e20 (a Kong misnomer)]
NapiMessage make_client_host_player_added(int player_number, const std::string &player_name,
                                          const std::string &ip_and_port,
                                          const std::string &pcid, const std::string &team,
                                          const std::string &type);
// ClientHostPlayerRemoved: PlayerNumber only. [orig: CNapiGameSession_SendPlayerRemoved
//  @0x4d01a0; the state-6 wrapper @0x4d0e40]
NapiMessage make_client_host_player_removed(int player_number);

// ClientPlayerEnterRequest: the HOST's admission statement for a player entering its game —
// ConnectionId (the joiner's dcb), IpAddress (the joiner's game address as a decimal u32),
// PortNumber, and the JoinTicket the joiner's "<localaddr>JOINTICKET" key-value carried
// (empty when absent). Answered by ServerPlayerEnterResult.
// [orig: CNapiGameSession_SendPlayEnterRequest @0x4d02a0]
NapiMessage make_client_player_enter_request(uint32_t connection_id, uint32_t ip_address,
                                             uint32_t port_number,
                                             const std::string &join_ticket);

// ClientGLSVSSRequest: the gate-configured GLSVSSREQUEST string as the "GLSVSSRequest"
// param plus the Cookie var-list; answered by ServerGLSVSSResults.
// [orig: CNapiGameSession_SendGLSVSSRequest @0x4d3a40]
NapiMessage make_client_glsvss_request(const std::string &request,
                                       const std::vector<ClientVar> &cookie);

// ---- ServerCommand (the NovaWorld -> host administrative channel) --------
//
// The msginfo entry for "ServerCommand" reads the statement's "Cmd" param (512-char cap),
// tokenizes it (double-quoted runs are one token, quotes stripped) and dispatches the first
// token against nineteen verbs. Player-targeted verbs match by PREFIX and take one of four
// target suffixes selecting the lookup: ByIndex (atol -> slot index), ByIpAndPort ("host:port"),
// ByName (a callsign, or "*NN" for a slot) or ByPCID (the entity type name). Every verb is
// gated on the receiver being the authority (`is_authority`) and in a session; TextChatServer /
// Cycle / EndMission / GameOver / Earthquake / Lightning / TimeOfDay / SetServerName /
// SetServerMsg / SetMPReset compare the whole token case-insensitively.
// [orig: the ServerCommand handler loc_4D22F0 — "Cmd" read @0x4d2333, tokenize @0x4d2392,
//  PuntPlayer @0x4d23aa, TextChatServer @0x4d256e, TextChatPlayer @0x4d2628, CmdEchoPlayer
//  @0x4d276f, KillPlayer @0x4d28b6, ChangeTeam @0x4d2a11, SwapTeam @0x4d2a2c, Cycle @0x4d2a46,
//  EndMission @0x4d2a60, GameOver @0x4d2a7a, Earthquake @0x4d2a94, Lightning @0x4d2b33,
//  TimeOfDay @0x4d2bb5, SetServerName @0x4d2cc2, SetServerMsg @0x4d2d53, SetMPReset @0x4d2e00,
//  ReloadPlayer @0x4d2e4f, DisarmPlayer @0x4d2f99; the suffix strings @0x7cc65c/0x7cc650/
//  0x7cc648/0x7cc640; String_TokenizeQuotedToArray @0x616d60; StrStartsWithNoCase @0x616f40;
//  String_MatchSuffix @0x617040]
enum class ServerCommandVerb : uint8_t {
	None = 0,
	PuntPlayer,
	TextChatServer,
	TextChatPlayer,
	CmdEchoPlayer,
	KillPlayer,
	ChangeTeam,
	SwapTeam,
	Cycle,
	EndMission,
	GameOver,
	Earthquake,
	Lightning,
	TimeOfDay,
	SetServerName,
	SetServerMsg,
	SetMPReset,
	ReloadPlayer,
	DisarmPlayer,
};
enum class ServerCommandTarget : uint8_t {
	None = 0,
	ByIndex,
	ByIpAndPort,
	ByName,
	ByPCID,
};
struct ServerCommand {
	ServerCommandVerb verb = ServerCommandVerb::None;
	ServerCommandTarget target = ServerCommandTarget::None;
	std::string command;             // the first token verbatim (verb + suffix)
	std::vector<std::string> args;   // the tokens after the verb
};
const char *server_command_verb_name(ServerCommandVerb verb);
// The retail tokenizer: whitespace splits outside double quotes, quotes toggle an in-quote
// run and are dropped, a backslash is copied verbatim. [orig: String_TokenizeQuotedToArray @0x616d60]
std::vector<std::string> tokenize_quoted(std::string_view text);
// Parse a "ServerCommand" container into `out`; false when it carries no Cmd param or the
// verb is none of the nineteen (retail falls through to the no-op tail).
bool parse_server_command(const NapiMessage &container, ServerCommand &out);

} // namespace opennova
