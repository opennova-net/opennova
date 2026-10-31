#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <unordered_map>
#include <vector>

#include <functional>

#include <base/io/crt_rand.h>                   // the process's CRT stream (process_crt_rand)

#include <runtime/inmatch/character_registry.h> // the host's Avatars.def combos (g_AvatarDefs)
#include <runtime/inmatch/chat_flood.h>        // the console chat's flood table
#include <runtime/inmatch/file_transfer_session.h> // the process's S2C 0x02 probe words
#include <runtime/inmatch/gate_metrics_reports.h> // the METPROTOCOL clocks and sink
#include <runtime/inmatch/game_config.h>       // inmatch::GameConfig — the ONE consolidated server-state config
#include <runtime/inmatch/mission_exit.h>      // ProcessDisconnectLatch (the own client's callback)
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/server_ban_lists.h>  // banlist.txt / banned.txt (the join refusals)
#include <runtime/inmatch/server_console.h>    // the client-less host's CHAT ring
#include <runtime/inmatch/server_designations.h> // the designation table (S2C 0x6B)
#include <runtime/inmatch/server_files.h>      // ServerLogs (the /PROFILE and punt log devices)
#include <runtime/replication/net_quality.h>   // the CNetQuality window (the host send half)
#include <runtime/hud/net_quality_indicators.h> // a Serve Only host's g_NetQuality
#include <runtime/world/entity.h>              // world::EntityHandle (the deployable spawner seam)
#include <runtime/world/entity_update_transactions.h> // the entity update's inline transactions
#include <runtime/world/match.h>               // world::MatchStats (the host slot's stats)

// Forward declarations — the runtime holds non-owning pointers to the authoritative world and
// the in-match replication seam. No World/codec headers are pulled into this header, and there
// is NO socket and NO Godot code anywhere under engine/runtime/inmatch (engine/CLAUDE.md).
namespace opennova {
struct Protocol; // net/protocol/protocol.h
} // namespace opennova

namespace opennova::world {
class World;
}
// The loaded mission, read by the P3 initial-state burst for the S2C 0x0B BMS-header body
// (bms::encode_loaded_header_blob). Forward-declared (NOT included) so bms.h stays out of this light header.
namespace opennova::bms {
struct File;
}

namespace opennova::inmatch {

class ChatFilter;
class ClientRuntime;
struct HostRotation;

// [orig +0x5C] The host/client connection mode written by [orig: CGameSession_SetConnectionMode
// @0x4c49f0] (§5.0 / §6.3). It decomposes into the two booleans is_authority (is_host) and
// is_mp_session_peer (is_client). Single player / co-op listen server = HostClient.
enum class ConnectionMode : uint32_t {
	None = 0,       // is_host 0, is_client 0
	HostOnly = 1,   // is_host 1, is_client 0 — dedicated server
	ClientOnly = 2, // is_host 0, is_client 1 — join a remote host
	HostClient = 3, // is_host 1, is_client 1 — single-player / co-op listen server
};

// [orig +0x50] transport_mode — the NETWORK TYPE (NovaWorld vs LAN) (§6.3). Distinct from
// socket_state below: the UI maps NovaWorld -> SetTransportMode(4) and LAN -> SetTransportMode(2),
// while a single-player host calls SetTransportMode(1) directly (§5.0 step 2). NovaWorld-only
// AppId/JoinTicket gates branch on this == NovaWorld. The session setter stores it from its
// argument, the menu's connect type on a host or a join, 0 on SP and every reset; a joiner
// carries it on its join record (the Godot JoinTarget.network_type), where the squad talk
// row reads it [orig: CNapiNetwork_SetNetworkType @0x4c4a50: +0x50 = state @0x4c4a82 (and +0x58
// is_in_session = state in 1..3 @0x4c4a85); the join leg passes
// g_GameConfigState.networkConnectType_480 @0x558314..0x55831f, SinglePlayer_StartMission 0
// @0x561bd4; the squad gate @0x49ba13].
enum class NetworkType : uint32_t {
	NovaWorld = 1,
	Lan = 2,
};

// [orig +0x54] socket_state — the value [orig: CNapiNetwork_SetTransportMode @0x4c8750] writes.
// 1 = socketless (the host's own local client is in-process); 2/3/4 reach
// [orig: CNapiNetwork_OpenTransportSocket @0x4c6a40] and open a UDP socket (§5.0 step 2). SP = 1.
//
// NOTE (refines the plan's "reuse replication::TransportMode at +0x50"): the witness puts the
// loopback-vs-socket selector in socket_state (+0x54), and the network-type enum in
// transport_mode (+0x50) — two separate fields (§6.3). replication::TransportMode stays the
// per-connection mode on NapiNPConnection.link, which is the right home for it.
enum class SocketMode : uint32_t {
	Socketless = 1,       // single player / in-process loopback (no socket)
	Lan = 2,              // LAN socket
	HostClientSocket = 3, // host + client over a socket
	NovaWorldSocket = 4,  // NovaWorld-routed socket
};

// [orig: g_NapiNPCtx.np_protocol @+0xE5C] NapiNPProtocol (§6.5) — the host state block reached
// from the singleton. Only the fields the in-match runtime needs now are modeled; offsets cited.
struct NapiNPProtocol {
	// The title's protocol the network init installs: CO +0x18, AP +0x58, BDAT +0x98, PN +0xDC,
	// PG +0x11C, PV1 +0x12C, PV2 +0x16C, PV3 +0x1AC (net/protocol/protocol.h). The host's 0x81
	// writes it and its 0x41 / 0x42 gates compare against it. start_server installs the session's
	// (SessionStartup::protocol); with none (null), no hello is answered and no join admitted.
	// [orig: CNapiNetwork_Init @0x4ca4a0 - the stores @0x4ca72f..0x4ca8c2]
	const Protocol *identity = nullptr;
	bool reject_new_connections = false; // [orig: +0x1F7, NapiNPProtocol_HandleClientJoin's JFC6 leg @0x62be8f]
	uint32_t server_flags = 0;          // [orig +0x500] P1 TLV (server config flag word)
	uint32_t build_flags = 0;           // [orig +0x504] P2 TLV (CNapiServerConfig_BuildFlags)
	uint32_t max_players = 1;           // [orig +0x524] MP TLV; clamped 1..251
	// The magic packet's reflection: the session create sets +0x52C and draws the key +0x530 for
	// a NovaWorld authority in a session, else zeroes both; the manager's magic-packet handler
	// answers a packet carrying the key. No list, TLV or page carries the key (D-NET-818).
	// [orig: CNapiGameSession_CreateSession @0x4C9B05..0x4C9B3F; NapiNPTimer_GenerateRandomId
	//  @0x61E510; NapiNP_HandleMagicPacket @0x622EA6..0x622EB8, its one reader]
	uint32_t magic_reflect = 0;         // [orig +0x52C]
	uint32_t magic_key = 0;             // [orig +0x530]
	uint32_t host_key = 0;              // [orig +0x534] HK TLV; NapiNP_GenerateSessionKey @0x61ea70
	uint32_t host_running = 0;          // [orig +0x538] 1 once StartServer succeeds; Hello rejects 0
	uint32_t host_start_tick = 0;       // [orig +0x53C] GetTickCount at StartServer (uptime base)
	uint32_t host_run_duration_ms = 0;  // [orig +0x544] live elapsed ms; frozen at stop
	// The host's millisecond clock at this pump's receive (HostOwner::clock_ms, GetTickCount's
	// stand-in): the stamp an in-order receive or a ping writes on its connection (D-NET-256).
	// [orig: CNapiNPConnection_ParseMessages @0x625D54; Nwu_HandlePing @0x623C56]
	uint32_t receive_clock_ms = 0;

	// +0xECC (protocol[947]), the dcb source, is not this protocol's word in ours: retail's game
	// protocol is created once per process and zeroes it there, so it is the process's connection
	// counter, next_connection_index (net/npwire/nw_session_framing.h), which no session resets.
	// CNapiNPConnection_Create steps it into each new connection (+0x14) and the state change
	// copies that to connection_id (+0x18): the dcb a LAN listen host ASSIGNS, ships in the 0x82
	// ServerAuth MI TLV and stamps into the joiner's 0x0C ownerConnectionId so the client's
	// Player_FindLocalPlayerEntity @0x4e0090 numeric self-match (entity+0x78 ==
	// NapiNP_GetLocalConnectionId @0x4c6d40) succeeds (D-NET-713).
	// [orig: NapiNPProtocol_Create @0x625730 - +0xECC zeroed @0x625A81, its game-protocol call
	//  CNapiNetwork_Init @0x4CA6CC (once, Game_ParseCommandLineAndInit @0x4A7346), destroyed only
	//  by NapiLib_Shutdown @0x4C95F8; CNapiNPConnection_Create @0x62acb0 - @0x62B282..0x62B296;
	//  CNapiNPConnection_OnStateChange @0x626060 - @0x626199..0x62619C;
	//  CNapiNPConnection_SendSessionInit @0x620ef0 - MI = +0x18 @0x620FD4..0x620FE8]

	// [orig +0x288] the session/server name ("HOST STARTED \"%s\"" log; lobby-visible): the
	// hello's SN [orig: NapiNPProtocol_SendServerInfoPacket @0x6207B2..0x6207DA].
	std::string session_name;
	// [orig +0x300] the join password: the hello's SF (set when nonempty) and the 0x42's PW gate
	// (reject 4). The create copies game_settings' and the admin SET's ServerPassword arms write
	// it, nothing else (D-NET-1102). [orig: NapiNPProtocol_SendServerInfoPacket
	//  @0x620872..0x620898; NapiNPProtocol_HandleClientJoin @0x62BE01..0x62BE18;
	//  CNapiGameSession_CreateSession @0x4C99F3; CAdminServer_HandleSetCommand @0x405AD6 /
	//  @0x4061F8]
	std::string server_password;

	// [orig +0xE44 / +0xE80] the cs_dir1 / cs_dir0 connection-template blocks — the reap window
	// (CS field 0) and outbound pool bound (CS field 11) every server-side node is created with
	// and the host's 0x82 advertises. CNapiNetwork_Init seeds 120000 / 1200 and a loose
	// `_NSTMOUT.TXT` overrides both (session_timeout_config.h); start_host_session installs it.
	CsConfig connection_template{};

	// [orig +0xEBC] connection_list — the NapiListHead SendFiltered / timeouts walk. Modeled as a
	// vector of nodes (faithful structural translation of the intrusive list).
	std::vector<NapiNPConnection> connection_list;
};

// [orig: g_NapiNPCtx @0xB5CBC8] NapiNPServerCtx (§6.3) — the game-level singleton, the full
// in-match game-server state. CNapiNetwork-shaped header + game fields + np_protocol. Named
// fields with cited offsets; idiomatic C++ types (no byte-exact padding — see fidelity decision).
// The host-side rtxt "Server" strings (GameText section "Server"). Each is the
// sprintf format the retail handler fills; empty means the string is absent.
struct ServerTextTable {
	std::string medic_request_format; // STRSRV_MEDREQ: "%s" = the requester's name
	// The team change's system lines, "%s" = the switching player's name
	// [orig: GameText_GetString("server", "C2Blue" / "C2Red") @0x51902E / @0x51909C].
	std::string change_to_blue_format;
	std::string change_to_red_format;
	// STRSRV19 ("Host"): a Serve Only host's own slot-0 name (HostSlotRow) [orig:
	// Server_InitNewRoundState @0x51C9CD; CNapiGameSession_BuildAndCreateSession @0x5696EE].
	std::string host_name;
	// STRSRV18: the in-game ban's kick text, its punt's detail and the head of its SYSTEM
	// line [orig: Server_BanPlayerSlot @0x5082D0 - GameText_GetString("Server",
	//  "STRSRV18") @0x5083C6 / @0x5083FE].
	std::string kicked;
};

// A Serve Only host's own player-slot row, slot 0 of retail's table: the
// authority's local row (+4 active, +5 local) with no net player (+28 clear),
// which this port keeps beside the connection list because no connection holds
// it. The session's round init keeps it active and names it Server/STRSRV19;
// the session create's PlayerSession_InitFromProfile then writes it again from
// the session's own net config: the name, which for a host that is not a
// session peer is STRSRV19 too, the team from the net config's side byte
// (cfg side_req: 1 or 2 is taken, anything else is 1 for a slot that is no
// spectator), and the class from the host's player profile's side class.
// Its hidden owner-0 body is not modeled (D-NET-352).
// [orig: Server_InitNewRoundState @0x51C99A..0x51CA3C (active, local, the name
//  @0x51C9C3..0x51C9D8); CNapiGameSession_BuildAndCreateSession @0x5696E2..0x5696FE
//  (the net config name), @0x5697C3 (jsp[60] = side_req); CNapiGameSession_CreateSession
//  @0x4C9C9A..0x4C9CCE (a non-peer jumps into the call), @0x4C9952..0x4C996C (the class);
//  PlayerSession_InitFromProfile @0x50CA80 — the team @0x50CA92..0x50CAB8, the name
//  @0x50CB53, +416 @0x50CB72, the class +0x15EDC @0x50CB7C]
struct HostSlotRow {
	bool active = false;
	std::string name;             // +0x28
	uint8_t team = 0;             // +0x1A0
	int32_t player_class = 0;     // +0x15EDC, the admin tables' Class column
	world::MatchStats stats;      // +0x48 CPlayerStats: Kills (5) and Deaths (7)
	// The session create's profile init's side bytes: the char ids (+0x1B8 / +0x1BC) and
	// avatars (+0x15F01..+0x15F03) the host's player profile carries, a fresh record's on a
	// host with no profile of its own (PlayerSession_InitHostRowFromProfile).
	CharacterJoinVars char_vars;
	// +0: the row's hidden body, every mission start's round init's spawn (D-NET-352;
	// Server_SpawnHostSlotBody).
	world::EntityHandle body{};
};

// One node of the authority's NovaWorld squad registry (the list at 0x24E0E54, inside the
// curated `g_SpawnWaveList`): a joined account's squad id, name and tag, one node per id.
// [orig: CLinkedList_FindOrCreateByNetId @0x52B540 — node +12 id, +16 name[65], +81 tag[9]]
struct SquadRegistryNode {
	int32_t squad_id = 0;
	std::string name;
	std::string tag;
};

struct NapiNPServerCtx;

// The host session's half of the transactions its world's entity update raises
// inline (world/entity_update_transactions.h), armed on that world around each
// server tick's entity pass (Server_TickUpdate). Defined in server_player_death.cpp.
struct ServerEntityUpdateTransactions final : world::IEntityUpdateTransactions {
	NapiNPServerCtx *ctx = nullptr;
	void player_death(world::World &world, world::Entity &body) override;
	void medic_interaction(world::World &world,
			const world::MedicInteraction &interaction) override; // server_medic.cpp
};

// A send an entity-update transaction made, held for the next server tick's head:
// the recipient's connection (its join-order id and its transport, both
// re-checked at the release) and the message.
struct StagedEntityUpdateSend {
	uint32_t connection_id = 0;
	replication::ISessionTransport *transport = nullptr;
	uint8_t tag = 0;
	std::vector<uint8_t> body;
};

// One transaction's sends, named by its queue record (RoundDeath::staged_sends).
struct StagedEntityUpdateBatch {
	uint32_t seq = 0;
	std::vector<StagedEntityUpdateSend> sends;
};

// One count per player-slot row (at most 251 rows, Server_InitNewRoundState's clamp).
using PlayerRowFreeTicks = std::array<int32_t, 256>;

// A player row a later add of the same connection left behind: a repeated C2S 0x02 lists the
// node again and the spawn pump's add picks it a fresh row and rebinds the net player's server
// slot there, so the earlier row stays active (+4) with its net player (+0x1C) and its body,
// which no disconnect reaches (the leave frees the net player's current row). It counts in the
// free-row pick, the free-tick walk, the alive count and the team counts (D-NET-821), and
// as a slot row it is a 0x16 row and keys its squad (D-NET-821, L1-s15). Every slot walk
// takes it as any row: the reset's 0x25 walk, the round init's re-spawn and the tick's per-slot
// block (L1-s16); the auto-balance, the A&D swap arm and the list builder's hide test (L1-s17).
// Ours keeps a row inside its connection, so the orphaned row is the connection as the re-add
// found it: `row` holds the row's own words (its body, slot, state word, burst cursors, slot
// team, spectator latch, squad key, kit, the 1 Hz cooldown); its net-player words are never
// read from the copy but through the live connection orphan_net_player finds by the net
// player's identity `row` keeps (net_player_serial).
// Once that connection leaves, retail's +0x1C names a freed net player and its later reads
// through the row read freed memory; ours reads nothing there, a deliberate fix of a retail
// bug (the maintainer, 2026-10-21, via main, Q7-36 (a)).
// [orig: Server_PlayerAdd @0x51CBC0 - PlayerSlot_PickFreeSlot (the call @0x51CC95), +0x1C
//  @0x51CD4E, +0xC0 rebound @0x51CD51; Server_CountPlayersOnTeam @0x501630 (@0x501650,
//  @0x501656); Server_BuildAndBroadcastScoreboard @0x50D960 (the slot walk, state 6 @0x50D9F6);
//  CSlotArray152_CountActiveByKey @0x500660 (+4 @0x500690, +0x188DC @0x500699);
//  Server_ForwardEventToSession @0x51B8E0 (the leave frees +0xC0's row alone, @0x51B8ED)]
struct OrphanedPlayerRow {
	NapiNPConnection row;
};

struct NapiNPServerCtx {
	NetworkType transport_mode = NetworkType::Lan; // [orig +0x50]
	SocketMode socket_state = SocketMode::Socketless; // [orig +0x54]
	uint32_t is_in_session = 0;        // [orig +0x58] gates the entire replication loop
	ConnectionMode connection_mode = ConnectionMode::None; // [orig +0x5C]
	uint32_t is_authority = 0;          // [orig +0x60] is_host bit of connection_mode
	uint32_t is_mp_session_peer = 0;    // [orig +0x64] is_client bit of connection_mode

	// The host's character table (Avatars.def's combos; empty without the
	// file): the player add validates each side's character id against it.
	// [orig: g_AvatarDefs, read by Server_PlayerAdd @0x51CF82 / @0x51CFB4]
	CharacterRegistry characters;

	// The ONE consolidated server-state config (ADR 0013 / §6.9): §6.4 identity + the §6.9 rule globals
	// (S2C 0x08) + the §5.1 reactive-reply mission/player/spawn. Merged from the former game_settings +
	// rules + session_config. Seeded by create_session (see server_session.h).
	// Live admission controls. The IP ban compares the connection's UDP source
	// address (conn+0x30, the datagram source stored @0x62bf28), never the
	// client-reported SIP, against banned.txt's entries, which use PeerAddr::ip's
	// LE octet packing (the same as BanList_ParseIPEntry @0x4fd5c9); banlist.txt's
	// PCIDs and banned.txt's names refuse at the game-layer join (codes 29 / 31).
	// [orig: CNapiNetwork_ValidateJoinRequest @0x4c61b0, the compare @0x4c6210]
	// The join lock (g_ServerJoinLocked): its writers are the lockgame toggle
	// (Server_ToggleJoinLock, server_info.h) and the round init's clear.
	// [orig: the validate's test @0x4C61C3; Server_ToggleJoinLock @0x50D75D;
	//  Server_InitNewRoundState @0x51CAAA]
	bool join_locked = false;
	// The listing's GameServerBaffleKey, the round init's draw (Server_InitNewRoundState): a
	// process global the Host list reads at each server-info update. Not the protocol's magic
	// key (+0x530, its own draw), which no list carries.
	// [orig: g_GameServerBaffleKey @0xC8FC70 - its one writer @0x51CB88, its one reader
	//  Lobby_UpdateServerInfo @0x4FF1CC]
	uint32_t game_server_baffle_key = 0;
	// The process's file-transfer session words (ClientProcessState::file_transfer_session), which
	// the host role's mission starts and Server_TickUpdate's head step and each S2C 0x02 probe
	// reads live as its second word; null on a context no host role brought up (its probes carry 0).
	// [orig: g_FileTransferSessionId @0xC86FCC, stepped by Game_StartMission @0x524360 (the add
	//  @0x5244B4) and Server_TickUpdate @0x51D7E0 (the add @0x51D820), read by
	//  CNetPlayer_SendFileTransferChunk @0x4C9440 (the read @0x4C9468)]
	FileTransferSession *file_transfer_session = nullptr;
	// The process's client disconnect latch (ClientProcessState::disconnect_latch), which the
	// host's own client's disconnected callback writes at its teardown and maps its exit reason
	// from (D-NET-823); null on a context no host role brought up (no callback runs).
	// [orig: CNapiNetwork_OnDisconnectedFromServer @0x4C63D0, installed on the session's own
	//  type-2 by CNapiGameSession_CreateSession @0x4C9BEC]
	ProcessDisconnectLatch *client_disconnect_latch = nullptr;
	// The process's NovaWorld game session's leg of a player's teardown
	// (ClientProcessState::nwu_player_removed: ClientHostPlayerRemoved in state 6 and the slot's
	// PlayerList vars dropped), which remove_connection_player runs first for a slot whose player
	// was added, the listen host's own included; null on a context no host role brought up, an
	// unset function when no NovaWorld session is held.
	// [orig: Server_HandlePlayerDisconnect @0x51B5C0 - CNapiGameSession_SendPlayerRemovedIfHosting
	//  called @0x51B5F3, NapiLinkedList_RemoveByUserData on the PlayerList called @0x51B601]
	const std::function<void(int player_number)> *nwu_player_removed = nullptr;
	// The process's CRT stream (ClientProcessState::crt_rand), which the session's draws take
	// while no World is bound: a host's create, ahead of its first mission's World, seeds it and
	// draws its keys there (session_crt_rand); null on a context no host role brought up (its
	// draws read 0 without a World).
	// [orig: CNapiGameSession_BuildAndCreateSession @0x5694D0 - Server_InitNewRoundState called
	//  @0x5695B3 ahead of the Game Loop's Game_StartMission (GameLoop_Init's call @0x526377)]
	io::CrtRand *process_crt_rand = nullptr;
	// The process's PuntCRC counters (ClientProcessState::punt_crc_counters), which the
	// CRC-mismatch punt's record steps (Server_CountCRCMismatchPunt); null on a context no host
	// role brought up (nothing counts).
	std::array<int32_t, 8> *punt_crc_counters = nullptr;
	// ctx+0xE60 past the own client's leave: retail's own type-2 connection stays allocated in
	// state 6 (its pump state machine has no state-6 arm and nothing destroys it before the
	// session reset), holding the record its leave latched, which the post-mission router's
	// reason text reads. Ours's one own node leaves the list at the pump that drains the
	// goodbye, so the record it held stays here (HostRole::own_client_error_record;
	// D-NET-823).
	// [orig: CNapiNPConnection_SetState @0x626250 - the teardown called @0x62628F, state 6
	//  @0x626297; CNapiNPConnection_PumpStateMachine @0x6292E0 (no state-6 arm);
	//  CNapiGameSession_ResetActiveSession @0x4C8A70 - the +0xE60 destroy called @0x4C8A93]
	bool own_client_left = false;
	DisconnectEvent own_client_left_record;
	// The process's GetTickCount the host's wall-clock readers take (host_tick_count_ms): the
	// owner's HostConfig::clock_ms, which every embedder supplies (host_monotonic_milliseconds)
	// and a test injects, installed ahead of the create; empty is the deterministic stand-in.
	// The status report's UP-TIME, the S2C 0x58's uptime, the 0x81's UT and the S2C 0x02
	// probe's tick read it, so a load and a held start count, as retail's do (D-NET-1024);
	// so do the protocol timers (the ticket wait, the deploy-idle punt, the held joiner's
	// re-nag, the session ping's round trip; D-NET-1047..37). The log devices' flush
	// timers alone stay on the run clock (host_run_duration_ms): no peer sees them.
	// [orig: Server_BuildStatusReport @0x530B84; NapiNPProtocol_SendServerInfoPacket @0x620657;
	//  NetPacket_BuildPaddedTimestampPayload @0x502A4E; each a GetTickCount call]
	std::function<uint32_t()> process_clock_ms;
	// g_RoundStateInitTick, the round-state init's GetTickCount stamp in the host clock
	// (host_start_tick's milliseconds): the status report's UP-TIME counts from it
	// (session_uptime_ms). Every session create's round init stamps it; a single player's
	// restart runs no round init and keeps the first start's (HostRole::bring_up, from
	// ClientProcessState).
	// [orig: Server_InitNewRoundState - GetTickCount @0x51CB93, the store @0x51CBA0;
	//  Server_BuildStatusReport @0x530B84..0x530B8C; Lobby_UpdateServerInfo @0x4FEFAB]
	uint32_t round_state_init_tick = 0;
	// The server-info update (Lobby_UpdateServerInfo(0)) a toggle runs at once and the server
	// tick's refresh runs every 0x744 ticks: a NovaWorld embedder installs it while its session
	// hosts the match; empty, nothing runs, as retail's update returns outside a NovaWorld session.
	// [orig: Server_ToggleJoinLock @0x50D750 (the call @0x50D762); Server_TickUpdate @0x51D7E0
	//  (the call @0x51D943); Lobby_UpdateServerInfo @0x4FE8C0 (the gates @0x4FE8D8..0x4FE8FA)]
	std::function<void()> on_update_server_info;
	// The METPROTOCOL gate reports (gate_metrics_reports.h, D-NET-995): the process's two
	// phase clocks, the install's CC.BIN their constructs copy (the session create reads it), the
	// clocks' GetTickCount (the owner's host clock, host_clock_ms, stamped at each pump's head, the
	// session create and the mission start's legs) and the NovaWorld embedder's sink (unset: no
	// report). [orig: g_ProcessServerTick @0xC87020; g_MissionServerTick @0xC87598;
	//  Server_InitServerTicks @0x5018C0 - Game_ReadCCBinFile @0x5018E8]
	GateMetricsClocks gate_clocks;
	std::string cc_bin;
	uint32_t tick_count_ms = 0;
	GateMetricsSink gate_metrics;
	ServerBanLists bans;
	// The squad registry (server_squad_registry.h): the remote add files a node for a joiner
	// with a nonzero squad id and the disconnect of a squad's last slot frees it; a LAN
	// host's stays empty. Read by the C2S 0x4E walk, the chat line's tag and channel 12 and
	// the 0x56 row's tag. [orig: Server_PlayerAdd @0x51CE16; Server_HandlePlayerDisconnect
	// @0x51B7F5]
	std::vector<SquadRegistryNode> squad_registry;
	GameConfig config;            // [orig g_NapiNPCtx.game_settings @+0xE68 + the scattered g_* rule globals]
	// Two game_settings words the session's create copies and nothing after it writes, which
	// BuildFlags' team test reads: the create's game type (config.game_type is each mission's,
	// a rotation row's) and the create's copy of the cfg block's mp-attribute word
	// (config.mp_attributes is that word itself, which an admin SET's tail rewrites)
	// (D-NET-1076). [orig: CNapiGameSession_BuildAndCreateSession @0x5694D0 - the row's type
	//  into the settings +0xCC @0x569655 / @0x569678, multiplayerAttributeFlags_34C into +0xD0
	//  @0x569662 / @0x56967F, the create's call @0x56997D; CNapiServerConfig_BuildFlags @0x4C4DC0 -
	//  `test [ecx+0F34h], 10000h` @0x4C4E30, `test [ecx+0F38h], al` @0x4C4E3C]
	uint32_t session_game_type = 0;
	uint32_t session_mp_attributes = 0;
	NapiNPProtocol np_protocol;    // [orig +0xE5C] (pointer in the original; embedded here)

	// [§5.2a] The advertised weapon-restriction set (S2C 0x66). The original reads a 255-entry
	// restriction table (unused6 @0x24D5600); weapon_restrictions holds only the RESTRICTED
	// (index,value) entries (value 0 or the title's armory-only value, ProtocolInMatch); empty = no
	// restrictions -> 0x66 emits a single count byte 0 (golden frame 160). The session's first
	// mission builds it (start_host_session, D-NET-693) and the admin's WEAPON SET edits it.
	std::vector<std::pair<uint8_t, uint8_t>> weapon_restrictions;

	// [orig +0x1198] the SendFiltered send mask (preserved name). Present but the
	// 2-peer MVP broadcasts the whole world (filter == 1); the per-connection cull
	// (retail's +0x119C target player / +0x11A0 target state) is not modelled.
	uint32_t send_mask = 0;

	// [orig: g_ScoreboardBroadcastTimer @0xC8D80C] One global mission
	// counter shared by the 0x16 scoreboard and 0x30/0x31 integrity broadcast.
	// Server_TickUpdate increments first; a value >0x136 fires and resets to 0,
	// so a fresh mission reaches its first boundary after 311 calls. Mission
	// start resets it through create_session; round init does not.
	uint32_t scoreboard_broadcast_timer = 0;
	// NOT MODELLED: retail's g_EndRoundLingerTimer [orig: @0xc8d820] -- MP-only,
	// set to 2790 (45 s at the 62 Hz tick) BEFORE the per-slot round-end loop and
	// gated on is_in_session [orig: @0x5166c4], drained by Server_TickUpdate
	// (authority) / the client frame; SP never drains it (the epilog owns the SP
	// exit). The round-end wire pass keys on World::match.outcome() instead.
	// [orig: dword_24C10C0] The process-global family toggle. Executable initial
	// storage is zero: false selects 0x31, true selects 0x30, then every boundary
	// XORs it even when no player is eligible. Neither session nor round init
	// resets it, so keep it as NapiNPServerCtx-lifetime state.
	bool integrity_entity_family_next = false;

	// The rtxt "Server" section strings the host formats into chat
	// (`GameText_GetString("Server", key)`), read from gametext.bin by the
	// host boot and installed by HostRole::bring_up (HostBringup::server_text).
	// An EMPTY string is the null lookup: the consumer no-ops exactly as retail
	// does when the text is absent. [orig: Server_BroadcastMedicRequest
	// @0x5153C9..0x5153D0]
	ServerTextTable server_text;

	// Host CNetQuality scalar sent as S2C 0x79. Retail derives this byte as
	// max(frame-rate pressure, mean ping, packet loss) over a five-sample window.
	// A local healthy LAN resolves to 1; the shell binding may replace it when
	// equivalent live telemetry is available.
	uint8_t host_network_quality = 1;
	// The host CNetQuality SEND window that derives the byte above, sampled
	// every 62 frames while in session [orig: Game_ProcessMainFrame @0x52658B,
	//  the g_NetQualityUpdateCountdown 62-frame countdown; CNetQuality_UpdateMetrics @0x4C52C0].
	replication::NetQualityWindow host_quality_window;
	uint32_t net_quality_sample_countdown = 0;
	// The authority's bucketed 0..4 level off that window (the receive window
	// never runs here, so the combined scalar is the send window's), stored
	// with each sample; the host role hands it to its own client's connection
	// indicators [orig: CNetQuality_UpdateMetrics's tail @0x4c585a..0x4c58b0 ->
	// CNetQuality_SetLevel(&g_NetQuality, level) @0x52659b].
	int32_t net_quality_level = 0;
	// The NovaWorld UDP (NWU) session the authority's NovaWorld registration
	// runs, as the shell reports it each tick: in use (dword_B5FD2C) and its
	// hosting/playing word (session+0x128, dword_B60108). The 62-frame block's
	// NovaWorld exit reads them beside transport_mode, and stores
	// g_MissionExitReason 12 here; so does the session's own punt handler.
	// [orig: Game_ProcessMainFrame @0x52655d..0x52657c]
	bool nwu_in_use = false;
	int32_t nwu_session_role = 0;
	// The NWU game session's flags (session+0x120, byte_B60100), the shell's report beside
	// those two: the connection indicators' N icon steps on its bits 2 and 8 (hud
	// net_quality_indicators.h nwu_session_ready) (D-NET-971).
	uint8_t nwu_session_flags = 0;
	int32_t mission_exit_reason = 0;
	// The server protocol's link-error callbacks since the host role last
	// drained them (kNetQualityLinkError* bits): a joiner's 0x44 resend list
	// that named a sequence, our 0x84 missing-sequence request that named one.
	uint32_t net_quality_link_errors = 0;
	// g_NetQuality on a host with no client of its own (a Serve Only host): every in-session
	// main frame stores the sample's level in it, steps its indicators and raises the link-error
	// flags, client or not, and the status page draws it; a listen host's lives on its own
	// client runtime (ClientRuntime::net_quality_indicators). Reset at the session create and
	// every mission start (D-NET-971).
	// [orig: Game_ProcessMainFrame @0x5263F0 - CNetQuality_SetLevel @0x52659B,
	//  CNetQuality_UpdateIndicators @0x52668D (no client test); CNetQuality_Reset @0x4C58C0]
	hud::NetQualityIndicators net_quality_indicators;
	// The main loop's FR counter (world::TickAccumulator::average_fps), handed
	// over by the session once per banked frame (HostRole::observe_frame_rate):
	// the send window's frame-pressure input and, copied at the head of every
	// Server_TickUpdate, the 0x0A server-status fps byte. 0 = retail's
	// mode-init value, until the first 2 s window closes.
	// [orig: g_StatsAvgFps; Server_TickUpdate @0x51D7E0..0x51D7E5 -> g_ServerFps]
	int32_t stats_avg_fps = 0;
	// The main loop's frames drawn in the last 62 logic updates and the frame
	// window's CPU share (Session::frame_statistics), handed over after every
	// frame (HostRole::observe_frame_statistics): the status page's bottom row.
	// The share also arrives before the drain (HostRole::observe_cpu_share),
	// where every Server_TickUpdate copies it, capped, into the 0x0A
	// server-status CPU byte.
	// [orig: dword_24C193C; g_StatsCpuPercent — Server_DrawStatusScreen
	//  @0x50afaf / @0x50b024; Server_TickUpdate @0x51D7EA..0x51D7FB]
	int32_t stats_frames_last_second = 0;
	int32_t stats_cpu_percent = 0;
	// The persistent slot cursor of the 1 Hz S2C 0x46 quality resend walk.
	// [orig: g_WeaponBroadcastSlotCursor, Server_TickUpdate @0x51DE79]
	int32_t quality_broadcast_slot_cursor = 0;
	// [orig: g_NetworkQualityBroadcastTimer] One global explicit countdown,
	// reset to zero by Server_InitNewRoundState @0x51CA9E. Server_TickUpdate
	// decrements a positive value, emits when it reaches/is zero, then reloads
	// 0x136. This state must not be derived from World::logic_tick: round reset
	// intentionally makes the next server boundary due immediately.
	uint32_t network_quality_broadcast_countdown = 0;

	// The team-change list a late joiner's C2S 0x29 walk reads back as S2C
	// 0x51: every entity Server_ChangeEntityTeam retargeted (a player at an
	// admin ChangeTeam, a capture zone at its flip), each once, cleared at the
	// new-round init. [orig: g_TeamChangeEntityList — CBufferList_AddOrFind
	//  @0x518EEC; the clear in Server_InitNewRoundState @0x51C911; the read
	//  NapiNPServerMsg_0x029 @0x514F7C]
	std::vector<world::EntityHandle> team_change_entities;

	// The transactions the world's entity update raises inline (the player
	// body's death edge), and the sends they staged: retail's update queues them
	// after the frame's send pump, so they lead the next frame's queue, released
	// at the next tick's head at their records' places in the death queue.
	// `entity_update_staging` is the batch a running transaction sends into.
	// [orig: Game_ProcessMainFrame -- the Server_TickUpdate call @0x5266B6 (its
	//  send pump @0x51E487), then Entity_UpdateAllEntities @0x52674B]
	ServerEntityUpdateTransactions entity_update_transactions;
	std::vector<StagedEntityUpdateBatch> entity_update_sends;
	std::vector<StagedEntityUpdateSend> *entity_update_staging = nullptr;
	uint32_t entity_update_batch_seq = 0;

	// The session owner's receive pump (host_session.cpp), set for the length of its
	// Server_TickUpdate call, which runs it after the per-player walk: the walk reads what
	// the last frame left, and every handler's sends queue behind the walk's (D-NET-686).
	// [orig: Server_TickUpdate - Server_UpdateAllActivePlayerSlots @0x51D88B, then
	//  CNapiNetwork_PumpServerProtocolRecv @0x51D895]
	std::function<void()> receive_pump;

	// The designation table the radio calls fill and the per-player S2C 0x6B
	// batch reads, cleared at the new-round init
	// (inmatch/server_designations.h). [orig: g_ServerDesignations @0xC84810;
	//  the memset in Server_InitNewRoundState @0x51cb9b]
	ServerDesignationTable designations{};

	// The authoritative end-round transaction. The domain Match freezes the
	// result; these are only the once-only wire announcement and retail MP linger
	// clock (the title's world::TitleRules::round_end_linger_ticks). [orig:
	// Server_ProcessRoundEnd @0x5164F0; 2790 store @0x5166C4]
	bool round_end_announced = false;
	uint32_t round_end_linger_ticks = 0;
	// The mission load's spawn gate (g_NetSpawnSuspended): set for the length of a
	// host's mission load, whose in-load pumps run the server handlers (HostRole::pump_load),
	// so the C2S 0x28, 0x29 and 0x32 that arrive then are consumed and answered with
	// nothing (D-NET-116). Its other readers (the spawn pump, the timeout walk, the quality
	// sample) run only from the tick and the main frame, never inside the load. A failed
	// load leaves it set: no handler runs before the next load's head sets it again.
	// [orig: g_NetSpawnSuspended @0x24D1DE0 - Game_StartMission @0x524360 sets it @0x524398
	//  and clears it @0x52631B; the readers NapiNPServerMsg_0x029 @0x514F1E,
	//  NapiNPServerMsg_SendEmptySlots @0x51A609, Server_CollectValidWeaponSlots @0x516005]
	uint32_t net_spawn_suspended = 0;
	// The frozen end-of-round board stream (stru_C947D8): built once by the
	// round-end producer before the per-slot 0x61/0x1D push, then only READ by
	// the C2S 0x2B chunk service; empty until a round ends and cleared with the
	// other round-end fields at session creation.
	// [orig: Server_ProcessRoundEnd @0x5164F0 (the
	// Server_BuildEndOfRoundScoreboard(1, winTeam) call @0x516590);
	// NetPacket_WriteReplayStreamChunk @0x506F60]
	std::vector<uint8_t> round_end_board_stream;

	// The count of the loaded-model list this host froze at its mission start
	// (g_LoadedModelChallengeCount, which the IDB's CEffectWorld_GetLoadedModelPage
	// returns as a "height"), the wrap of the S2C 0x68's 50-row cursor.
	// The role stamps it every tick from the embedder's frozen count; focused
	// tests set it directly. [orig: CEffectWorld_GetLoadedModelPage @0x5B1578;
	//  Server_SendLoadedModelPageRequest @0x511375 / @0x511391]
	uint32_t loaded_model_list_count = 0;
	// --- reimpl-owned, NOT in the original singleton ---
	// The authoritative simulation. Non-owning. The in-match replication seam (the per-connection
	// C2S drain / S2C fan) is owned by Server_TickUpdate over connection_list — there is no separate
	// NetSystem (retired P8): the drain/emit primitives live in runtime/replication/connection_fan.h.
	world::World *world = nullptr;
	// The session-level rotation state the embedder owns for its whole run
	// (inmatch/mission_rotation.h): the round end's LASTGAME read, the team
	// assignment's side-to-team map, the round count. Non-owning; null (a
	// test context, the SP listen server) reads as LASTGAME off and sides
	// 1, 2.
	HostRotation *rotation = nullptr;
	// The host process's own client half (the listen host's loopback
	// ClientRuntime; null on a dedicated host): the client-side state a host
	// handler reads through the process globals retail shares, here the
	// active-zone overlay the radio key builder tests (the A&S context).
	// Non-owning; the role re-points it whenever it rebuilds that runtime.
	const ClientRuntime *host_client = nullptr;

	// The loaded mission, read by the initial-state burst for the S2C 0x0B BMS-header body
	// (bms::encode_loaded_header_blob). Non-owning; null on the P2 unit-test path (0x0B skipped + logged).
	const bms::File *mission = nullptr;

	// The mission's raw terrain-tile (.til) file bytes: `[u32 'til0'][u32 count][u32 res0][u32 res1]`
	// then count × 12-B entries. Streamed to a joiner as the S2C 0x45 terrain-tile load (phase 5) so the
	// client's g_LoadingProgress climbs 5 -> 6 and its terrain finishes loading [orig: Terrain_SerializeTiles
	// @0x6080F0 reads g_TerrainTileData; the 0x45 header magic/count/hdr2/hdr3 map 1:1 onto the .til
	// header]. Owning copy set by the host at mission load (Godot-free: the caller resolves the .til via
	// engine/formats/til). EMPTY => 0x45 is faithfully skipped (Terrain_SerializeTiles returns 0 with no tile data).
	std::vector<uint8_t> terrain_til_data;

	// The current mission text table's raw cp1252 briefing strings. The Godot/resource
	// binding resolves [info]/briefing3 and [info]/briefing2 (falling back to
	// [info]/briefing) before host bring-up. When loaded, phase 6 serializes these as
	// two consecutive C strings for S2C 0x7E. The explicit loaded bit distinguishes a
	// valid pair of empty strings from a missing/unparseable mission text resource.
	// [orig: NetPacket_WriteBriefingText @0x506620]
	bool mission_text_loaded = false;
	std::string mission_briefing3;
	std::string mission_briefing2;
	// The mission text's [info] title the NovaWorld listing's MissionName, the session
	// variables' MISSIONNAME and the S2C 0x7B summary read ("" with no mission table or no
	// entry; each then reads the mission header's name, session_mission_name).
	// [orig: MissionText_GetString("info", "title") from Lobby_UpdateServerInfo @0x4FE993;
	//  Game_SerializeMissionInfoToDataStream @0x5237df; NapiNPMsg_0x7B_BuildPayload @0x507835]
	std::string mission_text_title;
	// The session title g_SessionTitle (inmatch::mission_session_title carries the witness): every
	// mission start sets it from the map file (the override table's [TITLE] entry, else the map
	// file); SetServerName overwrites it. The status report's SERVER line, the Co-op 0x7B and the
	// stock Co-op MISSIONNAME read it. [orig: Game_ApplySessionSettingsToGlobals
	//  @0x552145..0x55219E; CNapiGameSession_HandleServerCommand @0x4D2D0D; CAdminServer_HandleStatus
	//  @0x402E83; NapiNPMsg_0x7B_BuildPayload @0x507824; Game_SerializeMissionInfoToDataStream
	//  @0x52375A]
	std::string session_title;
	// MissionText [Locations]/LOCATION%03i labels for each BMS type-2044
	// marker the spawn admitted, in spawn order, each keyed by its record's
	// name_index (install_mission_location_names). The S2C 0x0F writer copies
	// these onto the joining client's deploy map.
	std::vector<std::string> mission_location_names;

	// The 180-byte mission/session block streamed by S2C 0x64. Retail builds it
	// once at mission start, including two random 32-byte regions and a nonzero
	// session id, then serves that same block to every joiner/re-request.
	// [orig: CNapiGameSession_InitRandomSeedOrRequest @0x51E8F0]
	std::array<uint8_t, 180> mission_metadata_blob{};
	// The live transfer identities the 0x60 / 0x64 chunk headers carry: retail's
	// two process-global mission counters, each incremented once per mission
	// start, so a fresh process's first mission serves id 1 on both. A C2S
	// 0x33 / 0x37 carrying another token restarts its transfer at offset 0.
	// [orig: g_ReplayBlockMagic @0xC86FC4 (`++` in Game_StartMission @0x5247F3);
	//  dword_C86FC8 (`++` in CNapiGameSession_InitRandomSeedOrRequest @0x51E9C1)]
	uint32_t server_info_transfer_id = 0;
	uint32_t mission_metadata_transfer_id = 0;
	// The server-info stream the 0x60 copies stands empty from a map change's teardown,
	// which resizes it to 0 ahead of the teardown's server pump, until the next mission
	// start rebuilds it after the load's first pump: a C2S 0x33 answered in between gets the
	// header alone, [the old token][0][0].
	// [orig: Game_TeardownMission - CDataStream_Resize(&stru_C9480C, 0) @0x5223D1, ahead of
	//  Server_PumpNetworkTransport @0x522588; Game_StartMission - the head pump @0x5245E8,
	//  the rebuild Game_SerializeMissionInfoToDataStream @0x5247EB and the token @0x5247F3;
	//  NetPacket_WriteReplayDataBlock @0x5071F0 (the size @0x50727F, the clamp
	//  @0x507221..0x50723F, no copy at 0 @0x5072A2)]
	bool server_info_stream_emptied = false;

	// Retail's overloaded g_RoundOver is deliberately not copied into
	// this host context. Per-connection InitialStateBurst owns load progress;
	// world::Match owns the round-over latch. The client retains the 0x1D header
	// that starts its end-round board transaction. [orig: §5.2/§5.68]

	// The per-registration NovaWorld AppId (CNapiNetwork_MintAppId's
	// value, net/napi/session.h make_session_app_id), installed by the
	// shell's NovaWorld host binding beside the GSID; 0 on a LAN host. The
	// authority's status page shows it on its NovaWorld server line.
	// [orig: ctx+0x1194 — CNapiNetwork_MintAppId @0x4c4da3, read back
	//  by CNapiNPCtx_GetAppId @0x4c4db0 (Server_DrawStatusScreen @0x50a7fa)]
	uint32_t novaworld_app_id = 0;
	// The host's own CD-key ids, the wire forms its registry key decodes to: a DFX2 LAN host
	// refuses a joiner sending either (code 12, server_join_check.h). The session start installs
	// them (HostConfig::host_cd_keys: the game's, its activation's). Empty for a host that holds
	// none, opennova-serve's. [orig: g_CDKeyInfo +16 (g_CDKeyInfoWireCdkiid) and +0x810
	//  (g_CDKeyInfoWireExp1), read by Server_ValidatePlayerJoinRequest @0x50f458 / @0x50f510
	//  (dfx2.exe)]
	cdkey::WireKeys host_cd_keys;
	// Every connection the host's NapiNP layer brought up since the server
	// started, the host's own local connection included: the status page's
	// total logins.
	// [orig: ctx+0x11A8 — `add` in NapiNPServer_HandleNewConnection
	//  @0x4c8203, the new-connection callback CNapiNPConnection_OnStateChange
	//  @0x6261c6 runs when a connection enters state 1; zeroed by the host
	//  start callback CNapiServer_OnHostStarted (ex
	//  CNapiServer_OnPlayerDisconnected) @0x4c94f0 that
	//  NapiNPProtocol_StartServer @0x62b640 runs; read @0x50b0e4]
	uint32_t total_logins = 0;
	// A 0x42 the manager's drain took: its new connection's callback turns the _connectlog.txt
	// log off in the server receive pump's queued pass, after every datagram of the drain has
	// written its lines (finish_manager_receive).
	// [orig: NapiNPProtocol_HandleClientJoin @0x62B750 (the socket pass queues the 0x42
	//  @0x62C323 and returns 1 @0x62C328); NapiNPProtocol_PumpRecvQueues @0x6266A0 (the FIFO pass
	//  @0x6266CA..0x6267B2) -> CNapiNPConnection_OnStateChange @0x626060 (the callback @0x6261C6)
	//  -> NapiNPServer_HandleNewConnection @0x4C8040 (@0x4C8054..0x4C80AF)]
	bool connect_log_disable_queued = false;
	// The round tallies the status page's team block shows: every round end
	// of Team Deathmatch, Team KOTH or CTF counts one round and one win for
	// the winning side 1..4. Only the Reset Game action clears them, team 1's,
	// team 2's and the rounds alone (Server_ForceRoundEndAndClearState, catalog
	// row 92 `resetgames`, code 106; Role::dispatch_record).
	// [orig: g_TotalRoundsPlayed @0xC8FF1C, g_RoundWinsTeam1..4
	//  @0xC8FF0C..0xC8FF18 — Server_ProcessRoundEnd @0x516883..0x5168d8; the
	//  reset @0x5175aa..0x5175be]
	std::array<int32_t, 4> round_wins{};
	int32_t rounds_played = 0;

	// The GSID the NovaWorld service returned in ServerHostResult HostCommands,
	// installed by the shell's NovaWorld host binding once registration
	// succeeds; empty on a pure LAN host. Advertised as the 0x81 SUS1.
	// [orig: CNapiGameSession_HandleHostVerifyResponse @0x4D59D0 — "GSID"
	//  @0x4D5BD6 -> byte_24D5A12 @0x4D5BEE -> np_protocol->server_user_string1
	//  @0x4D5C0F]
	std::string novaworld_gsid;

	// The NovaWorld join-ticket arm (byte_B60100 & 0x40): set by the shell on a
	// NovaWorld-registered host whose ServerHostResult carried
	// HostRequiresJoinTicket. Armed, every validated joiner is announced to the
	// service as a ClientPlayerEnterRequest and held in game state 4 until the
	// ServerPlayerEnterResult (Server_ApplyPlayerEnterResult) or the 120 s
	// NWJTICKTMOUT reap; a pure LAN host leaves it clear.
	// [orig: CNapiNetwork_CheckPlayerTimeouts @0x4C8AD0 — the arm reads
	//  @0x4C8B88 / @0x4C8CA9, SendPlayEnterRequest @0x4C8BC1, SetGameState(4)
	//  @0x4C8BCA]
	bool novaworld_join_tickets_armed = false;
	// A second NovaWorld listing of the same match (opennova-serve's --also-master-host; D-NET-1385,
	// none of retail's, whose host lists on one service): its registration's own AppId (0: none)
	// and its ServerHostResult's ticket arm. A joiner whose APPID is this AppId is that listing's
	// (NapiNPConnection::novaworld_listing 1): the code-9 leg admits it, its ticket gate and
	// watchdog read this arm, and its ClientPlayerEnterRequest goes to that listing's service.
	uint32_t novaworld_second_app_id = 0;
	bool novaworld_second_join_tickets_armed = false;
	// The host's own address string on a NovaWorld transport: it prefixes every
	// key the host looks up in a joiner's CD identity cookie (PUBPCID,
	// PUBSQUADINFO, PUBJOINTICKET). get_local_address() below hands it out.
	// [orig: g_LocalNetAddressStr @0x7CA298]
	static constexpr const char *kNovaWorldLocalAddress = "PUB";
	// The cookie-key table the joiners' CD cookies are decrypted under: the
	// six-slot ring whose current key the Host list advertises as PCIDKey. The
	// round init starts it from the session's CRT stream (Server_InitNewRoundState,
	// right after the listing's key) and the server tick's server-info refresh
	// advances it (run_server_info_refresh), on every authority, a LAN host's
	// included (whose ring no reader reads).
	// [orig: g_NapiCookieKeyTable @0xC87000 — Server_InitNewRoundState @0x51C8E0
	//  (the CSessionIdRing_Init call @0x51CB8E), Server_TickUpdate @0x51D7E0 (the
	//  CSessionIdRing_AdvanceAndGenerate call @0x51D93D); read by
	//  Lobby_UpdateServerInfo @0x4FE8C0 (@0x4FF196), the join's checks
	//  Server_ValidatePlayerJoinRequest @0x512100 (@0x512641, @0x5126F7, @0x5128D3)
	//  and its cookie decrypt NapiNPServer_HandlePlayerJoinMessage @0x512AA0
	//  (@0x512D18 / @0x512D74)]
	SessionIdRing cookie_key_table;
	// The server-info refresh's countdown (g_ServerInfoUpdateTimer): the round
	// init clears it, each server tick steps it down, and at zero or below the
	// refresh reloads it to 0x744 (SESSION_HOST_INFO_REFRESH_TICKS), so a round's
	// first server tick refreshes. Signed, as retail's `jg` reads it.
	// [orig: g_ServerInfoUpdateTimer @0xC8D818 — Server_InitNewRoundState @0x51CA86;
	//  Server_TickUpdate @0x51D91D..0x51D933; Nbstat_StartupInit @0x4FDE30 (@0x4FDE46)]
	int32_t server_info_update_timer = 0;
	// One ClientPlayerEnterRequest: the joiner's connection id, its UDP source
	// and the JOINTICKET its JOIN carried. The shell binds the hook to its
	// NovaWorld host session (ClientSession::send_player_enter_request); an
	// unbound hook drops the request, and the joiner then reaps on the ticket
	// deadline exactly as a service that never answered.
	// [orig: CNapiGameSession_SendPlayEnterRequest @0x4D02A0 — ConnectionId
	//  @0x4D0396, IpAddress @0x4D03D0, PortNumber @0x4D040B, JoinTicket @0x4D046B]
	struct PlayerEnterRequest {
		uint32_t connection_id = 0;
		PeerAddr peer{};
		std::string join_ticket;
		// The listing whose service sent the joiner (NapiNPConnection::novaworld_listing): its
		// session is the one the request goes to.
		uint8_t listing = 0;
	};
	std::function<void(const PlayerEnterRequest &)> on_player_enter_request;
	// The C2S 0x02's LAN-only leg (lan_listing_address_owed): the admitted joiner's reported
	// address and port (NapiNPConnection::reported_ip / reported_port) go to the hosting
	// session's Host list as LANIP / LANPORT, the listing whose service sent the joiner the one
	// (PlayerEnterRequest::listing). The NovaWorld embedder installs it; unset, nothing is listed.
	// [orig: NapiNPServerMsg_0x002 @0x512FD0 - CNapiVarList_SetOrCreate(dword_B601F4, "LANIP")
	//  @0x51317A, CNapiVarList_SetOrCreateFormatted("LANPORT") @0x51318C]
	std::function<void(uint8_t listing, uint32_t ip, uint32_t port)> on_lan_listing_address;

	// The host EntityLimit table behind the vehicle-spawn availability reply
	// (C2S 0x42 -> S2C 0x70) and the C2S 0x40 spawn gate: one row per spawnable
	// items.def id, at most 128. Retail's 73-dword rows carry the id (row[0]), the
	// type cap (row[1], -1 = unlimited), the per-team flag (row[2], -1 = no per-team
	// cap) and the 70 per-team slot counts (row[3 + team], -1 = unlimited). Every
	// mission start rebuilds it from the 23 rows Server_InitEntityLimitTable holds
	// (server_vehicle_spawn.h); empty, the reply carries only its terminator and
	// every spawn is refused, as an empty g_EntityLimitTable.
	// [orig: g_EntityLimitTable @0xC7B480 / g_EntityLimitCount @0xC84680;
	//  NetPacket_SerializeWeaponOverlaySlots_0 @0x5105A0; EntityLimit_TryConsume @0x5104C0;
	//  EntityLimit_SetEntry @0x500E50 (128 rows @0x500E8B, 292-byte rows, 0x46 slots @0x500EBC)]
	static constexpr std::size_t kEntityLimitRows = 128;
	static constexpr std::size_t kEntityLimitTeamSlots = 70;
	struct VehicleSpawnLimitRow {
		uint16_t type_id = 0;
		int32_t type_cap = -1;                // row[1]
		int32_t per_team_flag = -1;           // row[2]
		std::array<int32_t, kEntityLimitTeamSlots> team_slots{}; // row[3 + team]
	};
	// The table is bypassed while the host config's unlimited_vehicles is set
	// (GameConfig::unlimited_vehicles, stock 1): 0xFF/0xFF rows on the wire and
	// no limit check. [orig: g_RulesUnlimitedVehicles @0x5105F5..0x5105FF, @0x51C5E2..0x51C5E9]
	std::vector<VehicleSpawnLimitRow> vehicle_spawn_limits;
	// The world-side spawner Entity_SpawnDeployable @0x51C2B0 delegates to: the
	// embedder that owns the items.def traits sweep installs it (HostRole, over
	// mission::MissionKernel::spawn_deployable); the C2S 0x40 handler refuses the
	// spawn when unset. `position` is the handler's six-word block (x, y, z 16.16,
	// then Yaw, Pitch, Roll). Returns the new pool-1 handle.
	std::function<world::EntityHandle(world::World &, uint16_t item_id, uint8_t team,
			const int32_t position[6])> deployable_spawner;
	// The embedder's definition bind for a player body the authority spawns,
	// run at the spawn (Server_BuildPlayerInfoAndAdd): its items.def row's
	// traits, sound-profile pair, organic weapons and collision instance
	// (mission::MissionKernel::bind_spawned_body), as retail's spawn binds every
	// body to its row [orig: Entity_SpawnFromAnimSlotProperty @0x43C390 -- the
	// ItemList_FindIndexByTypeId call @0x43C3CA, EntityDef_LoadModelsAndCallbacks
	// @0x43C45F, Entity_InitFromModel @0x43C492]. The HostRole installs it after
	// its bring-up's own spawn, whose body the boot's later steps bind; unset =
	// no bind (a bare test host).
	std::function<void(world::EntityHandle)> bind_spawned_body;

	// The host process's log devices — the /PROFILE recorder and the punt
	// logs — installed from HostConfig::logs at the session start; a null
	// pointer is a device retail's switch left off (server_files.h).
	ServerLogs logs;
	// `/NOPUNT` (base/resource_index boot_policy.h RetailSwitches::nopunt), installed from
	// HostConfig::nopunt at the session start. Its ten tests gate these punts, each with the
	// punt-log line ahead of it: the checksum replies' (WCRC / ACRC), the control quartet's
	// silences (t16 / t24), the join's map checksums (HMCRC / CMCRC), the violation sweep's team
	// kills and suicides (t6) and the periodic walk's deploy idle (t35) and dead too long (t7).
	// Every other automatic punt carries no test (the ping kick, the client CRC validation's
	// tN punts of Server_HandleClientCRCValidation @0x519110), so a /NOPUNT host still makes it.
	// [orig: Game_ParseCommandLineAndInit - dword_B4C698 = 1 @0x4A747F; its reads
	//  Server_ValidateWeaponCRC @0x501FFA, NapiNPServerMsg_HandleAntiCheatCRCCheck @0x5021C6,
	//  Server_UpdateAllActivePlayerSlots @0x518A07 / @0x518A2D, Server_OnPlayerJoin @0x51A8C6 /
	//  @0x51A907, Server_CheckPlayerViolations @0x51ACCC / @0x51ACFA, Server_TickUpdate
	//  @0x51E130 / @0x51E181; dfx2.exe's the same ten, dword_B45DD4 = 1 @0x4A74CF, read
	//  @0x4FF13A, @0x4FF306, @0x515A47 / @0x515A6D, @0x5178E7 / @0x517928, @0x517CFC /
	//  @0x517D2A, @0x51B140 / @0x51B191]
	bool nopunt = false;
	// The host socket's bound address the /INOUT host lines print
	// [orig: the protocol manager's socket +8 / +12, CNapiNPConnection_LogHostStarted
	//  @0x61e6c6]; unknown prints retail's no-socket placeholder.
	PeerAddr local_address{};
	bool local_address_known = false;
	// The CHAT ring of a host with no client of its own and the flood table its
	// console chat checks (server_console.h) [orig: the raw slots byte_B3EA38;
	//  Chat_CheckFloodControl's table @0xB3B788].
	std::vector<ServerConsoleLine> console_chat;
	ChatFloodTable console_chat_flood{};
	// That host's chat display buffer, the lines CHAT GET lists, and its wrap: the
	// bold label font the embedder loads and the chat box's width
	// (server_console.h) [orig: byte_B3FE3C, 40 x 128].
	ChatDisplayBuffer console_chat_display;
	ChatDisplayWrap console_chat_wrap;
	// The process's chat filter, filter.txt's rules (chat_filter.h), which the chat relay tests each
	// line against; null (no filter.txt) filters nothing, as an empty dictionary does. The host boot
	// loads it once and installs it on every session's context.
	// [orig: g_ChatFilterTTSContext @0x3347EE0 and its tables byte_32A6A10 / byte_32F2A10, loaded by
	//  Game_InitSubsystems @0x4A6DEC]
	std::shared_ptr<const ChatFilter> chat_filter;
	// A Serve Only host's own slot 0 (HostSlotRow); inactive on a listen host,
	// whose loopback connection holds slot 0.
	HostSlotRow host_slot;
	// Each player-slot row's free-tick count (+0x180): the server ticks since the
	// row was freed, or since the table was allocated, saturating at 0x7FFFFFFF.
	// The table's allocation and a row's add zero it; the tick's tail walk steps
	// every inactive row's (Server_AdvanceFreeRowTicks); a remote add takes the
	// row free the longest (D-NET-351).
	// [orig: slot +0x180 - Server_TickUpdate @0x51E3A0..0x51E3A4, @0x51E457..0x51E467;
	//  PlayerSlot_PickFreeSlot @0x4FD7D0..0x4FD7EE; the memsets
	//  Server_AllocatePlayerSlotTable @0x51C1CB, Server_PlayerAdd @0x51CD06]
	PlayerRowFreeTicks player_row_free_ticks{};
	// The rows a re-add orphaned (OrphanedPlayerRow); the table's allocation clears them.
	std::vector<OrphanedPlayerRow> orphaned_player_rows;
	// The last net-player identity drawn for an orphaned row (NapiNPConnection::net_player_serial).
	uint32_t last_net_player_serial = 0;

	// All members are complete + movable now that the unique_ptr<GameServerRuntime> is gone (P8), so the
	// compiler-default special members suffice.
	NapiNPServerCtx() = default;
	NapiNPServerCtx(NapiNPServerCtx &&) noexcept = default;
	NapiNPServerCtx &operator=(NapiNPServerCtx &&) noexcept = default;
};

// An orphaned row's net player (+0x1C): the live connection the re-add left the row on, found by
// the net player's identity (net_player_serial; never the connection id, which a C2S 0x48
// re-stamps), with the row's transport pointed at that connection's for the walk; none once the
// connection has left (identities are never reused), when the row's transport is cleared, where
// retail reads a freed net player (Q7-36 (a), OrphanedPlayerRow).
// [orig: NapiNPServer_SendFiltered @0x4C87E0 - the walk's row filter +0xC0 @0x4C888C, the 0x20 /
//  0xA0 target arm's +0x1C @0x4C8A1F]
inline NapiNPConnection *orphan_net_player(NapiNPServerCtx &ctx, OrphanedPlayerRow &orphan) {
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (conn.net_player_serial == 0 || conn.net_player_serial != orphan.row.net_player_serial)
			continue;
		orphan.row.link.transport = conn.link.transport;
		return &conn;
	}
	orphan.row.link.transport = nullptr;
	return nullptr;
}

// GetTickCount on the host context: the process clock (process_clock_ms), else the
// deterministic stand-in a clockless test runs on, the start tick plus the host's run.
inline uint32_t host_tick_count_ms(const NapiNPServerCtx &ctx) {
	return ctx.process_clock_ms
			? ctx.process_clock_ms()
			: ctx.np_protocol.host_start_tick + ctx.np_protocol.host_run_duration_ms;
}

// The status report's UP-TIME: GetTickCount now less the round-state init's stamp, in u32
// arithmetic, so the wall time since the create's round init, a load and a held start
// included (D-NET-1024). [orig: Server_BuildStatusReport - GetTickCount @0x530B84, `sub
//  eax, g_RoundStateInitTick` @0x530B86, +0x70 @0x530B8C]
inline uint32_t session_uptime_ms(const NapiNPServerCtx &ctx) {
	return host_tick_count_ms(ctx) - ctx.round_state_init_tick;
}

// Install the host's "Server" strings (HostRole::bring_up, from the host
// boot's gametext.bin read).
inline void set_server_text(NapiNPServerCtx &ctx, ServerTextTable text) {
	ctx.server_text = std::move(text);
}

// The join-ticket gate: in session, on the NovaWorld transport, join tickets
// required. A joiner's C2S 0x01 enters NetPlayer game state 3 (the validation
// the timeout watchdog runs) only under it, else state 6; the validator's
// code-28 leg reads the same three words (D-NET-702). Ours carries retail's
// two ticket words (the NovaWorld session's +0x4FC the 0x01 tests and its
// +0x120 bit 0x40 the watchdog tests) as the one novaworld_join_tickets_armed.
// A joiner a second listing's service sent (novaworld_listing 1, D-NET-1385) reads
// that listing's arm instead (join_tickets_armed_for).
// [orig: the C2S 0x01 handler @0x512ED0 - is_in_session @0x512F87, the
//  transport @0x512F90, g_HostRequiresJoinTicketMaybe @0x512F99, state 3 @0x512FA2..0x512FA6,
//  state 6 @0x512FAF..0x512FB3; Server_ValidatePlayerJoinRequest
//  @0x5128E2..0x5128F8]
inline bool join_tickets_armed_for(const NapiNPServerCtx &ctx, const NapiNPConnection &conn) {
	return conn.novaworld_listing == 1 ? ctx.novaworld_second_join_tickets_armed
	                                   : ctx.novaworld_join_tickets_armed;
}
inline bool join_tickets_required(const NapiNPServerCtx &ctx, const NapiNPConnection &conn) {
	return ctx.is_in_session != 0 && ctx.transport_mode == NetworkType::NovaWorld &&
			join_tickets_armed_for(ctx, conn);
}

// The C2S 0x02 lists the joiner's reported address on a LAN-only NovaWorld host: in session on
// the NovaWorld transport, with the session's LAN-only word (game.cfg's, the /LAN switch's) and
// the NovaWorld UDP session in use (D-GAMECFG-8).
// [orig: NapiNPServerMsg_0x002 @0x512FD0 - is_in_session @0x513123, the transport @0x51312C,
//  hostLanOnly @0x513134, dword_B5FD2C @0x51313D, the connection @0x513146..0x51314E]
inline bool lan_listing_address_owed(const NapiNPServerCtx &ctx) {
	return ctx.is_in_session != 0 && ctx.transport_mode == NetworkType::NovaWorld &&
			ctx.config.server_lan_only != 0 && ctx.nwu_in_use;
}

// The host's local address: "PUB" while it is in a session on the NovaWorld
// transport; false (no address, no cookie lookups) otherwise.
// [orig: CNapiNetwork_GetLocalAddress @0x4C4F60 — the gate @0x4c4f75, the copy
//  of g_LocalNetAddressStr @0x4c4f8f]
inline bool get_local_address(const NapiNPServerCtx &ctx, std::string &out) {
	out.clear();
	if (!ctx.is_in_session || ctx.transport_mode != NetworkType::NovaWorld) return false;
	out = NapiNPServerCtx::kNovaWorldLocalAddress;
	return true;
}

// The JOIN handler's cookie leg for a remote connection: on a NovaWorld session
// its CD cookie's PCID and SQUADINFO land in the connection's account fields.
// The host's own loopback skips it (its account is the shell's login PCID).
// True when the cookie's PCID decrypted (load_join_account).
// [orig: NapiNPServer_HandlePlayerJoinMessage @0x512AA0 — the loopback gate
//  `!connection+0x2E` @0x512ca2, GetLocalAddress && cookie @0x512cdc]
inline bool load_join_account_from_cookie(const NapiNPServerCtx &ctx, NapiNPConnection &conn) {
	std::string local_address;
	if (conn.type == NapiNPConnection::kTypeClientSide || !get_local_address(ctx, local_address) ||
			conn.join_identity_pairs.empty())
		return false;
	return load_join_account(
			conn.join_identity_pairs, local_address, ctx.cookie_key_table, conn.account);
}

// The NovaWorld host binding's account fact: the host's own login PCID on its
// loopback connection when it is non-empty (the cookie-key table is the
// context's own, cookie_key_table).
// [orig: NapiNPServer_HandleNewConnection @0x4c8213..0x4c829f — the local
//  connection on a NovaWorld session reads the PCID cookie into its net config]
inline void set_novaworld_account_facts(NapiNPServerCtx &ctx, const std::string &login_pcid) {
	if (login_pcid.empty()) return;
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (conn.type == NapiNPConnection::kTypeClientSide) set_join_account_pcid(conn.account, login_pcid);
	}
}

} // namespace opennova::inmatch
