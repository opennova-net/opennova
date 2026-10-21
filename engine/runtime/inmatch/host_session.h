#pragma once

// The in-match HOST owner loop: exactly ONE implementation, which godot/src/simulation
// delegates to. The faithful reimpl of the original engine's per-frame host
// pump [orig: CNapiNetwork_PumpManagerReceive @0x4c4d10 (recv, mgr flag 4, 250ms) +
// CNapiNetwork_SendUDPPacket @0x4c4d30 (CNapiNPManager_SendTo) wrapped around Server_TickUpdate].
//
// Socket-free: the loop holds NO socket — the owner supplies a opennova::IDatagramSocket and this
// pumps it (godot/src wraps UdpPump; apps/ wrap net::Socket via
// apps/common/net_datagram_socket.h). ALL protocol/crypto/framing stay in the libs (.agents/README.md). The loop
// speaks PeerAddr; the recv timeout (0 = non-blocking busy loop; ~30 ms for a single-threaded
// poll-pump test) is a property of the embedder, not this loop.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <runtime/replication/connection.h>            // replication::TransportMode
#include <net/npwire/idatagram_socket.h>      // opennova::IDatagramSocket
#include <runtime/inmatch/session_transport.h>     // replication::ISessionTransport / Datagram
#include <runtime/inmatch/udp_session_transport.h> // replication::UdpSessionTransport

#include <net/npwire/peer_addr.h> // opennova::PeerAddr
#include <runtime/inmatch/server_files.h> // ServerLogs
#include <net/npwire/protocol_message.h>

#include <runtime/inmatch/character_registry.h>
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_protocol.h>  // HostAcceptEvent + the server protocol entry points
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_session.h> // CreateSessionResult
#include <runtime/inmatch/server_tick.h> // Server_TickUpdate

namespace opennova::inmatch {

struct ClientProcessState;

// Per-remote-peer owner state: the socket-free transport the owner pumps (owns its lifetime; the
// connection's link.transport is a non-owning pointer into it) + the once-latch for the joiner's
// named dcb-bearing organic-spawn announce.
struct PeerLink {
	std::unique_ptr<replication::UdpSessionTransport> transport;
	bool announced = false;
};

// std::map ordering for PeerAddr (which has operator== but no operator<). Lexicographic (ip, port).
struct PeerAddrLess {
	bool operator()(const PeerAddr &a, const PeerAddr &b) const {
		return a.ip != b.ip ? a.ip < b.ip : a.port < b.port;
	}
};

// The in-match host: the inmatch ctx + the per-peer transports + the host's own loopback drain.
// The ctx.world / ctx.mission are wired by the owner (main.cpp / the Godot binding / the test)
// before the first pump.
struct HostOwner {
	NapiNPServerCtx ctx;
	replication::ISessionTransport *host_loopback = nullptr; // non-owning; the host's own dcb-2 client (Listen)
	std::map<PeerAddr, PeerLink, PeerAddrLess> peers;
	// In-match semantic replies collected while the S2C send block is closed.
	// They retain message order and are framed with transport output on the next
	// configured boundary.
	std::map<PeerAddr, std::vector<ProtocolMessage>, PeerAddrLess>
			pending_session_messages;
	// Already-framed established-session 0x83 packets (the admission replies and
	// the initial stream) also wait on the peer's send boundary. Their sequence
	// numbers and ciphertext are already fixed. The receive pump's own sends (the
	// 0x83 rebuilds a 0x44 asks for, the 0x84 missing-sequence request, the 0x85
	// pong) never wait here (D-NET-229).
	std::map<PeerAddr, std::vector<std::vector<uint8_t>>, PeerAddrLess>
			pending_session_datagrams;
	uint32_t now_tick = 0;
	// The host's millisecond clock, GetTickCount's: the send-interval legs and the receive reap
	// measure it, so a long mission load, which runs no tick, still ages them (D-NET-256).
	// From HostConfig::clock_ms at the session start; empty is the tick clock (now_tick x 16 ms,
	// the deterministic stand-in a test harness runs on). host_clock_ms reads it.
	std::function<uint32_t()> clock_ms;
	// false (dedicated/headless): HostOnly registers no local-player connection; an embedder-supplied
	// unused channel may be defensively drained. true (serve-and-play): HostClient registers the
	// loopback and the owner folds it into ClientState, so the pump must preserve it.
	bool serve_and_play = false;
};

// The process's monotonic millisecond clock, never 0 (GetTickCount's stand-in): the
// HostConfig::clock_ms an embedder supplies.
uint32_t host_monotonic_milliseconds();

// The owner's clock now: its supplied clock, else the tick clock (now_tick x 16 ms).
uint32_t host_clock_ms(const HostOwner &owner);

// Attach a UdpSessionTransport to `peer`'s connection (idempotent) and, ONCE the spawn pipeline has
// bound owned_entity, latch the peer announced exactly once. The joiner's NAMED dcb-bearing S2C 0x0C
// organic-spawn (ownerConnectionId == ServerAuth.MI, its wire handle H; D.0 / §5.23; the F3
// dcb-timing contract) ships IN-PHASE with the initial-state world stream, not from here.
void admit_peer(HostOwner &owner, const PeerAddr &peer);

// React to one HostAcceptEvent surfaced by handle_server_datagram / tick_connections.
void dispatch_event(HostOwner &owner, const PeerAddr &peer, const HostAcceptEvent &ev);

// One full owner iteration over `sock`, in retail's frame order:
//   (0) the previous frame's entity-update records, routed ahead of everything this frame queues
//   (3) Server_TickUpdate, whose head (the timers, the per-player walk) runs before the receive
//       pump this owner hands it (NapiNPServerCtx::receive_pump, D-NET-686):
//       (1) the host's own client's requests (`own_client_drain`), then the recv-drain:
//           recv_from -> handle_server_datagram -> queue replies + react to events; the
//           receive pump's own sends (0x44 rebuilds, the 0x84 request, the 0x85 pong) leave
//           at once; then the send-boundary clocks
//       (2) tick_connections: pre-spawn §5.2a bursts + surface F3/spawned, then
//           `before_server_tick`, then the records the handlers raised, behind their sends
//       then the tick's single C2S drain, one logic tick, on the periodic second the
//       spawn pump (after the scoreboard), and the per-connection 0x0A fan
//   (4) S2C flush: on each peer's send boundary, ship pre-framed 0x83 packets,
//       then pop its transport's identity [tag][body] -> frame/batch as 0x83 -> send
//   (5) drain an unused embedder loopback for HostOnly; preserve the HostClient local view
// A tick that does not run (no world, or not the authority) leaves (1) and (2) to the
// owner, after it. `before_server_tick`, when supplied, runs after tick_connections and
// before the tick's logic update / 0x0A fan; a body the tick's spawn pump adds binds at its
// spawn (NapiNPServerCtx::bind_spawned_body).
// `own_client_drain`, when supplied, dispatches the listen host's own client's queued
// requests: they reach retail's protocol FIFO through the manager's loopback FIFO, which the
// manager's receive drains ahead of the socket, so the receive pump's FIFO pass, in arrival
// order, handles them first. The opaque contexts keep this shared owner loop
// independent of embedder-specific state. Advances owner.now_tick by 1.
using HostBeforeServerTickFn = void (*)(void *context);
using HostOwnClientDrainFn = void (*)(void *context);
using HostEventObserverFn = void (*)(void *context, const HostAcceptEvent &event);

// The phases lap onto the SIM_HOST_* / SIM_SERVER_TICK rows of the world's
// profile (ADR 0043 d5); an inactive profile reads no clock.
void host_session_pump(HostOwner &owner, opennova::IDatagramSocket &sock,
		HostBeforeServerTickFn before_server_tick = nullptr,
		void *before_server_tick_context = nullptr,
		HostEventObserverFn event_observer = nullptr,
		void *event_observer_context = nullptr,
		HostOwnClientDrainFn own_client_drain = nullptr,
		void *own_client_drain_context = nullptr);

// Host bring-up config. The owner sets owner.ctx.world / owner.ctx.mission and owner.host_loopback (its
// dcb-2 LoopbackChannel) BEFORE start_host_session; this fills the rest.
struct HostConfig {
	// The title identity the host speaks (net/title/title_identity.h): the 0x81 it writes and the
	// 0x41 / 0x42 it admits. The embedder resolves a complete one (resolve_title_identity) and
	// refuses to host without; with none, the host answers no hello and admits no join.
	const TitleIdentity *title = nullptr;
	// The ONE consolidated GameConfig (ADR 0013): §6.4 identity (server name / max players / passwords /
	// game type) + the §6.9 rule globals + the §5.1 reactive-reply config (mission / player / spawn).
	GameConfig config;
	SocketMode socket_mode = SocketMode::Lan; // Lan (real socket) / Socketless (SP in-process loopback)
	// The network type the host runs under: NovaWorld for a host the NovaWorld menu's session
	// hosts (the menu's SetNetworkType(1) stands through the match; its host leg is
	// ConnectOrHost's, net/novaworld/connect_or_host.h), Lan otherwise
	// [orig: UI_ProcessLANSessionStateMachine @0x558e85..0x558e8c -> CNapiNetwork_SetNetworkType @0x4c4a50;
	//  UI_HandleHostSessionStart @0x556d00 leaves the type alone for connect type 1].
	NetworkType network_type = NetworkType::Lan;
	// Nonzero values are deterministic overrides for golden/tests. Zero asks the production helper
	// to mint the volatile retail startup value.
	uint32_t host_key = 0;
	uint32_t host_start_tick = 0;
	uint32_t session_seed_id = 0;
	// The process's monotonic millisecond clock (GetTickCount's), which every embedder supplies
	// (host_monotonic_milliseconds) and a test injects; empty runs the owner on its tick clock.
	// [orig: CNapiNPConnection_PumpSendIntervals @0x628FD0 and CNapiNPConnection_PumpStateMachine
	//  @0x6292E0 read GetTickCount]
	std::function<uint32_t()> clock_ms;
	bool serve_and_play = false; // true: HostClient + local player; false: HostOnly, no local player
	// The host's game directory — where CNapiNetwork_Init looks for the loose
	// `_NSTMOUT.TXT` reap/pool override (session_timeout_config.h). Empty = no
	// override lookup, the 120000 ms / 1200-record template stands.
	std::string game_root;
	// The listen host's OWN per-side character selection (weapon.sav / PLAYER_INFO),
	// installed on its type-2 loopback connection before the local player add — the
	// same fields a joiner uploads in ClientAuth. Retail fills its local player
	// record from the profile the same way [orig: PlayerSession_InitFromProfile
	// @0x50ca80 validates + stores both side ids; Player_InitPlayer @0x4e15f0 <-
	// g_AvatarTeam1/2, g_CharClassTeam1/2 <- Game_ApplySessionSettingsToGlobals
	// @0x551500]. The shell installs it from the mounted Avatars.def + weapon.sav;
	// unset, the host boot computes a fresh profile's over the mounted Avatars.def
	// (fresh_profile_character_vars), and a session started with neither takes the
	// no-table form.
	std::optional<CharacterJoinVars> local_character_vars;
	// The host's character table (the mounted Avatars.def; empty without the
	// file), installed on the server ctx before any player add (D-NET-137).
	CharacterRegistry characters;
	// The listen host's OWN inverse OPTIONS_AUTOMEDIC word, its profile's +1660:
	// the round init copies it into the host's own player slot +372, where a
	// joiner's arrives by C2S 0x03; 0 (a fresh record's) is automatic requests.
	// [orig: Server_InitNewRoundState @0x51ca4d..0x51ca61 — under
	//  is_mp_session_peer, slot +0x174 = g_curPlayerProfile +0x67C]
	uint32_t local_auto_medic_disabled = 0;
	// The host process's log devices (server_files.h) and its socket's bound
	// address for the /INOUT lines; the session start installs both on the
	// fresh context.
	ServerLogs logs;
	// The process's client words the host's own client runtime steps (ClientProcessState,
	// client_runtime.h): null keeps the role's own across its map changes.
	ClientProcessState *client_process = nullptr;
	PeerAddr local_address{};
	bool local_address_known = false;
	// The mount's expansion text-override table (null = none), the mission start's session
	// title lookup searches first (mission_session_title); it outlives the session's
	// create or continue. HostRole's bring-up sets it from its bring-up record's mission
	// text. [orig: Game_ApplySessionSettingsToGlobals @0x552112..0x55219E]
	const rtxt::File *text_override = nullptr;
	// The directory banned.txt and banlist.txt live in (ServerBanLists::directory): retail's
	// working directory, "" for the bare names there (opennova-serve's). Unset, the lists
	// load empty and no file is read or written (the game's hosts, D-NET-295's residual).
	// The session start installs it ahead of the round init that loads both files.
	std::optional<std::string> ban_directory;
};

// Stand `owner` up through the shared in-match host bring-up the Godot host uses. `serve_and_play` selects HostClient and registers the type-2 loopback; otherwise it selects
// HostOnly and creates no local player. When serve-and-play also has a World, spawn the host player
// and queue its initial-state burst before the first per-frame 0x0A.
// ProcessExit when the session create refused on `cfg.config.multiplayer_reset`
// (server_session.h): no session stands and nothing past the create ran.
CreateSessionResult start_host_session(HostOwner &owner, const HostConfig &cfg);

// The map change's bring-up (net-re §5.70.6): the next mission inside the SAME
// session. The owner keeps its context, its connections, their keys and
// sequencing, its clock and its staged sends; the session config takes the
// next map's identity, the session-variable stream and the mission-data
// block rebuild with bumped transfer tokens, the round-end state clears, and
// the new World (`owner.ctx.world`, set by the caller) is configured as a
// fresh session's is. No player spawns here: the kept slots spawn after the
// mission load (inmatch/map_change.h). [orig: Game_StartMission's authority
// arm @0x524492..0x52452B, the stream rebuild @0x5247EB / its token
// @0x5247F3, CNapiGameSession_InitRandomSeedOrRequest @0x51E8F0 (the call
// @0x5248A7)]
void continue_host_session(HostOwner &owner, const HostConfig &cfg);

// The host's network pump while it loads a mission, on an in-session authority
// only: the receive drain with its handlers (a joiner's 0x33 / 0x37 / 0x47 / 0x48
// are answered from the kept slot, an admission leg from the in-load tick; the
// C2S 0x28 / 0x29 / 0x32 meet the load's spawn gate, D-NET-116) and the send
// flush, with no connection burst, no server tick (so no timeout walk and no
// spawn pump), no own-client drain, and the owner's clock not advanced.
// [orig: Server_PumpNetworkTransport @0x4FD960 -- is_authority / is_in_session
//  @0x4FD960..0x4FD970, CNapiNetwork_PumpManagerReceive @0x4FD977,
//  CNapiNetwork_PumpServerProtocolRecv @0x4FD981, CNapiNetwork_PumpServerProtocolSend
//  @0x4FD98B; a session host's Game_StartMission calls it @0x5245E8, @0x524A63,
//  @0x524F76 / @0x524F8C (the .bms arm), @0x525256, @0x5253EA, @0x52589D, @0x5258E5,
//  @0x5259EC (not a dedicated host's), @0x525A5C and @0x525AC5; the single
//  player's restart, Game_StartMission(1) out of a session, calls it @0x524B36 /
//  @0x524D73, where its session test returns. The load's gate-session updates
//  (CNapiGameSession_ProcessPeriodicUpdate on g_GateManager, ecx @0x5262F3, the
//  call @0x5262F8) only queue what they receive: no game handler runs in them]
void host_session_load_pump(HostOwner &owner, opennova::IDatagramSocket &sock);

// The mission teardown's server pump on a session's authority (the caller tests retail's
// is_in_session, the rules' mp_session): the load pump's receive and flush, with the protocol
// pump's pending walk between them for the listen host's own client, whose goodbye an exit from
// the frame's input pass held for this pump (OwnGoodbye::TeardownPump): the twin is torn down
// and the host's player removed over the destroyed bodies, its leave records flushed here.
// [orig: Game_TeardownMission @0x522350 - Server_PumpNetworkTransport called @0x522588, after
//  Entity_Destroy over pools 0..2 @0x52236F..0x5223C0 and CServerLog_CloseAndFree @0x5223F0;
//  Server_PumpNetworkTransport @0x4FD960 - the manager receive @0x4FD977, the server receive
//  @0x4FD981, the send @0x4FD98B; NapiNPProtocol_Pump @0x62A650 - the pending walk
//  @0x62A6FD..0x62A793] (D-NET-NEW-L8-21 path B)
void host_session_teardown_pump(HostOwner &owner, opennova::IDatagramSocket &sock);

} // namespace opennova::inmatch
