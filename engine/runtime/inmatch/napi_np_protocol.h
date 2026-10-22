#pragma once

#include <runtime/inmatch/napi_np_server_ctx.h>

#include <net/npwire/peer_addr.h> // opennova::PeerAddr
#include <net/npwire/protocol_message.h>     // opennova::ProtocolMessage
#include <net/npwire/session_hello.h>        // ClientHello / ServerHello (host_server_hello)
// inmatch::GameConfig (the consolidated server-state config) arrives via napi_np_server_ctx.h (ADR 0013).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace opennova {
class IDatagramSocket;
struct ConnectionKeys; // net/npwire/nw_session_framing.h
}

// P2 — Per-connection handshake. The proven server legs of novaworld::HostSessionAccept, promoted
// onto inmatch free functions over NapiNPServerCtx / NapiNPProtocol.connection_list (the legs
// the original walks as NapiNPProtocol methods over its NapiListHead of NapiNPConnection nodes).
// Socket-free and Godot-agnostic: the owner (the Godot binding) does socket I/O
// and pumps raw datagrams through here.
//
//   0x41 ClientHello -> 0x81 ServerHello   [orig: NapiNPProtocol_HandleClientHello @0x6213b0]
//   0x42 ClientAuth  -> 0x82 ServerAuth     [orig: NapiNPProtocol_HandleClientJoin  @0x62b750]
//   0x43 SESSION     -> 0x83 SESSION        [orig: NapiNPProtocol_HandleSessionPacket @0x626A00]
//   0x46 ClientGoodbye                      [orig: Nwu_HandleClientGoodbye @0x624250]
//
// The per-connection SCRK/seq/ack + handshake latches live ON NapiNPConnection (folded from the
// old PeerState). The 0x43 reactive §5.1 replies are produced by the gameplay-message dispatcher
// (server_message_dispatch.h, dispatch_session_replies) over ctx.config + the node's reply
// state (P8 — the retired ctx.game_runtime / GameServerRuntime); the one-shot world-stream/spawn burst
// is Server_SendInitialGameStateToPlayer over conn.burst. The F3 dcb-timing fix and the D.0 owner-ID match
// are ported VERBATIM. Wire bytes stay byte-exact.
//
// [orig: NapiNPProtocol_HandleSessionPacket @0x626A00; CNapiNPConnection_ParseMessages @0x625BC0;
//  accept loop apps/novaworld_lobby/nw_udp_listener.cpp run_loop]
namespace opennova::inmatch {

// Joiner pose recovered when a peer reaches Spawned. Position is i32 16.16 world: the joiner's own
// decoded C2S 0x0C uplink when one has arrived, else the host-advertised spawn from the session
// config.
struct HostJoinerPose {
	uint16_t entity_handle = 0;     // joiner's claimed pool<<12|slot (informational)
	uint16_t item_type_id = 0x14B9; // player infantry template type
	int32_t  pos_x = 0, pos_y = 0, pos_z = 0;
	int16_t  heading = 0;
	int16_t  pitch = 0;
	uint8_t  team = 1;
};

// One event surfaced by handle_server_datagram / tick_connections. Owner reacts:
// PeerEnteredWorldStreaming -> admit the joiner EARLY (spawn its pool-0 entity + stream its own
// dcb-bearing 0x0C) so the record is present in the client's load pump before its Player_InitPlayer
// runs; PeerSpawned -> bind the connection so the per-frame S2C 0x0A starts; PeerC2SInMatch ->
// route each 0x0C into the live sim; PeerGoodbye -> drop the connection.
//
// PeerEnteredWorldStreaming is the F3 dcb-timing fix: the retail client's load-time
// Player_InitPlayer @0x4e15f0 -> Player_FindLocalPlayerEntity @0x4e0090 scans pool 0 for its own
// entity (Flags&0x100 && entity+0x78 == its ConnectionId); that entity is created by the S2C 0x0C
// organic-spawn, which therefore must arrive DURING world streaming, before the game-start bundle.
// Emitting it reactively on PeerSpawned (after the bundle) is too late -> "Could not find player
// dcb" fatal @0x4dff60. (Relocated from novaworld::HostAcceptEvent, unchanged.)
struct HostAcceptEvent {
	enum class Kind {
		PeerHandshakeAdvanced,
		PeerEnteredWorldStreaming,
		PeerSpawned,
		PeerC2SInMatch,
		PeerGoodbye
	};
	Kind kind = Kind::PeerHandshakeAdvanced;
	PeerAddr peer;
	HostJoinerPose pose;                       // valid for PeerEnteredWorldStreaming / PeerSpawned
	uint32_t self_id = 0;                      // valid for PeerEnteredWorldStreaming / PeerSpawned:
	                                           // the joiner's own ConnectionId (NapiNPConnection's
	                                           // connection_id / unk_18 = its dcb): the create's
	                                           // index the 0x82's MI carried (D-NET-713), never its
	                                           // 0x48's body (D-NET-NEW-L1-36). The add stamps it
	                                           // into the joiner's 0x0C entity_flags (entity+0x78),
	                                           // which the client's Player_FindLocalPlayerEntity
	                                           // self-scan matches against the MI it stored.
	std::string peer_name;                     // valid for PeerEnteredWorldStreaming / PeerSpawned:
	                                           // the joiner's game ClientAuth.NA callsign, streamed
	                                           // back as the S2C 0x0C organic entity_name (the
	                                           // displayed callsign only; the joiner self-identifies
	                                           // by the record's owner_connection_id == ServerAuth.MI,
	                                           // never by this name).
	std::vector<ProtocolMessage> in_match_c2s; // valid when kind == PeerC2SInMatch
};

struct HandleResult {
	std::vector<std::vector<uint8_t>> outbound; // fully-framed datagrams to send to `peer`
	// Datagrams retail writes from INSIDE the server receive pump, which every Server_TickUpdate
	// runs ungated: the 0x83 packets a client 0x44 asks for, each rebuilt from its retained
	// records with the current ACK. They never wait for the connection's S2C send boundary; the
	// owner ships them at their datagram's receive position.
	// [orig: Server_TickUpdate -> CNapiNetwork_PumpServerProtocolRecv @0x51d895 (flags 0x19:
	//  0x8 = NapiNPProtocol_PumpRecvQueues @0x6266a0, queue_mask & 1 dispatch @0x6266ca..0x6267ad);
	//  NapiNP_HandleResendList @0x623800 -> CNapiNPConnection_SendSessionPacket @0x6239b6 ->
	//  CNapiNPManager_SendTo called @0x61F032]
	std::vector<std::vector<uint8_t>> immediate_outbound;
	// Datagrams to the address a packet names, sent from inside the receive at the datagram's
	// position: the magic packet's reflection (D-NET-NEW-L1-20).
	// [orig: NapiNP_HandleMagicPacket @0x622E70 (the CNapiNPManager_SendTo call @0x622EF8)]
	struct Addressed {
		PeerAddr to;
		std::vector<uint8_t> datagram;
	};
	std::vector<Addressed> addressed_outbound;
	// When the host owner asks to defer an in-match reply, these ordinary ProtocolMessages are
	// framed at the end of the same owner tick beside the per-frame transport output. This is the
	// retail send-boundary batching seam (for example S2C 0x57 + 0x0A).
	std::vector<ProtocolMessage>       deferred_session_replies;
	std::vector<HostAcceptEvent>      events;
	// Whether an opcode handler took the unpacked packet: the NAPI manager's dispatch
	// result, which its _connectlog.txt PROTOCOL or TOSSED [4] line reports (punt_log.h),
	// and whether that line went out.
	// [orig: NapiNPManager_HandlePacket @0x622F10 - NapiNPProtocol_DispatchOpcode
	//  @0x622F96, cb_on_dispatch @0x622FCD, cb_on_error(.., 4) @0x622FF9]
	bool taken = false;
	bool dispatch_logged = false;
};

struct TickOut {
	PeerAddr peer;
	std::vector<std::vector<uint8_t>> outbound;
	std::vector<HostAcceptEvent> events;
};

// Decode + dispatch one raw inbound datagram from `peer` (the bytes off the socket, envelope+NWU
// still on). `now_tick` feeds the game-session tag clock. Returns the outbound datagrams to ship
// back + events to react to. [orig: CNapiNPConnection_ParseMessages @0x625BC0 / the accept switch]
HandleResult handle_server_datagram(NapiNPServerCtx &ctx, const PeerAddr &peer,
                                    const uint8_t *raw, std::size_t len, uint32_t now_tick = 0,
                                    bool defer_in_match_replies = false);

// The receive pump's connection-list walk, after every datagram of the pump: each server-side
// connection in list order replays the queued session packets whose gap a datagram closed, one
// packet at a time, through the same parse as an in-order arrival (NEW-L1-9). Each result is
// the owner's to consume like a datagram's.
// [orig: NapiNPProtocol_PumpRecvQueues @0x6266A0 - the FIFO pass @0x6266CA..0x6267B2, then the
//  walk @0x626910..0x6269E8: the type gate @0x626939, the head test @0x626960,
//  CNapiNPConnection_ParseMessages @0x62696B; the list's order is creation order,
//  CNapiNPConnection_Create appending each node @0x62B17D..0x62B1AE]
struct ReplayedSessionPacket {
	PeerAddr peer;
	HandleResult result;
};
std::vector<ReplayedSessionPacket> replay_ready_session_packets(
		NapiNPServerCtx &ctx, uint32_t now_tick, bool defer_in_match_replies = false);

// The end of one manager receive drain: the server receive pump's queued pass handles the 0x42s
// the drain queued, whose new connection turns the _connectlog.txt log off, so every datagram
// of the drain has written its lines first (NapiNPServerCtx::connect_log_disable_queued).
// [orig: Server_PumpNetworkTransport - the manager receive @0x4FD977 ahead of the server
//  receive @0x4FD981; NapiNPProtocol_PumpRecvQueues @0x6266A0 (the FIFO pass @0x6266CA..
//  0x6267B2); NapiNPServer_HandleNewConnection @0x4C8040 (@0x4C8054..0x4C80AF)]
void finish_manager_receive(NapiNPServerCtx &ctx);

// The host's 0x81 to one 0x41: the hello's CI and external triple echoed, the sender's address
// and port reflected (each written when nonzero), the protocol's own identity and the live
// session's fields. A remote probe's answer (handle_client_hello) and the listen host's own
// client's, at its session create (create_session). Needs the protocol's identity.
// [orig: NapiNPProtocol_SendServerInfoPacket @0x6204B0, from NapiNPProtocol_HandleClientHello
//  @0x6213B0 (the call @0x6218FC)]
ServerHello host_server_hello(const NapiNPServerCtx &ctx, const ClientHello &hello,
		const PeerAddr &peer);

// Input action 3 on a host with its own client (a listen host): the own client disconnects as
// a user leave, {2, 2, 0, 0, "I.C:CIDEMIS", 0, ""} latched on the own node (the first latch
// wins), its /INOUT CLIENT line written, and its goodbye queued on the manager's loopback for
// `drain`: the next tick's receive pump (tick_connections' pending walk then tears down the own
// pair's server-side twin: its /INOUT SERVER line and the host player's removal, the leave
// line and the 0x46 removal to the others), or the mission teardown's pump. A host with no own
// client (a dedicated host) and a second call do nothing. (D-NET-NEW-L8-21)
void request_own_client_disconnect(NapiNPServerCtx &ctx, OwnGoodbye drain);

// The pending walk of a pump that drains the own client's goodbye held for `drain`: the own
// pair's twin is torn down (its /INOUT SERVER line, the host player's removal, the leave records
// to the others) and our one own node leaves the list. False when no goodbye is held for it.
// [orig: NapiNPProtocol_Pump @0x62A650 - the pending walk @0x62A6FD..0x62A793] (D-NET-NEW-L8-21)
bool drain_own_goodbye(NapiNPServerCtx &ctx, OwnGoodbye drain);

// The client probe (C2S NWU 0x47), the last client-side opcode of the manager's table
// [orig: g_NPOpcodeHandlers @0x849D90, index 6; Nwu_HandleClientProbe @0x624280].
inline constexpr uint8_t kClientProbeOpcode = 0x47;

// The session legs the live host's handlers and the held server (unstopped_server.cpp) share,
// defined with the handlers in napi_np_protocol.cpp: a node's 0x86 burst, the framing of its
// records, and the resend list's, the ping's and the goodbye's legs past the connection gates.
std::vector<std::vector<uint8_t>> host_goodbye_burst(bool host_running,
		const NapiNPConnection &conn);
std::vector<uint8_t> frame_session_replies(NapiNPConnection &conn,
		const std::vector<ProtocolMessage> &replies);
bool resend_session_packets(NapiNPConnection &conn, const std::vector<uint8_t> &body,
		bool &named, std::vector<std::vector<uint8_t>> &sends);
bool answer_session_ping(NapiNPConnection &conn, const std::vector<uint8_t> &body,
		uint32_t stamp_ms, uint32_t clock_ms, std::vector<std::vector<uint8_t>> &sends);
bool latch_client_goodbye(NapiNPConnection &conn, const std::vector<uint8_t> &body);

// The 0x42's shared legs, the live host's join handler's and the held server's queued pass:
// the connection template a server-side create copies (the protocol's cs_dir template, the
// configured packet ceiling its field 13); the accept's node fill past the gates and the validate
// callback (the names, the client's keys and ids, the reconnect counts and the CU vars, the dcb
// and the keys the create drew, then the new-connection callback's MPS negotiation: true when it
// negotiated the ceiling, whose two CS updates, make_game_session_initial_settings, are the
// node's first sequenced records); the node's 0x82 from its current words; and a refusal's CR=0
// 0x82 (the family JFC, the reason JFP).
// [orig: NapiNPProtocol_HandleClientJoin @0x62B750 - the create @0x62BF06, the keys @0x62BF5E /
//  @0x62BF99, the template copies @0x62BFC9 / @0x62BFE3, the state change @0x62C2D7;
//  CNapiNPConnection_OnStateChange @0x626060 - cb_server_1 called @0x6261C6, SendSessionInit
//  @0x6261FC;
//  NapiNPProtocol_SendJoinRejection @0x620CD0]
CsConfig server_node_template(const NapiNPServerCtx &ctx);
bool fill_joined_node(NapiNPConnection &conn, const ClientAuth &auth, const PeerAddr &peer,
		const TitleIdentity &identity, uint32_t connection_id, const ConnectionKeys &server_keys,
		uint32_t receive_stamp_ms);
std::vector<ProtocolMessage> make_game_session_initial_settings(int32_t max_packet_bytes);
std::vector<uint8_t> make_server_auth_datagram(const ClientAuth &auth, const PeerAddr &peer,
		const NapiNPConnection &conn);
std::vector<uint8_t> join_rejection_datagram(const ClientAuth &auth, uint32_t failure_family,
		uint32_t failure_reason);

// The next session reset's StopServer over a server an error-arm exit left unstopped
// (UnstoppedServer, napi_np_connection.h): a held own client first (its CLIENT removal line;
// its goodbye is the loopback's), then each node takes the STOP record (the first latch
// wins), its /INOUT removal line and its 0x86 burst over `socket` (null: none open, nothing on
// the wire; the own pair's twin at address 0 bursts into the loopback), in list order; then
// HOST STOPPED over `local_address` (null: the placeholder)
// and the old session name. Nothing when no server is held. The reset points: a host's
// pre-game setup and a session create (HostRole's bring-ups), and the process's network
// shutdown (the shells' exits). (D-NET-NEW-L1-26)
// `player_removed` (a host's bring-up passes the process's NovaWorld legs,
// ClientProcessState::nwu_player_removed and Server_ReportHeldPlayerLeave over
// ClientProcessState::nwu_gate_metrics) takes each node, after its burst, for a node whose player
// was added: the destroy's removal callback reaches the slot's teardown on an authority, whose
// first leg is the NovaWorld session's (D-NET-NEW-L9-21) and whose later one the slot clock's
// PLAYER report (D-NET-NEW-L9-22). The reset's own Lobby_UpdateServerInfo(1) sends nothing: a
// nonzero argument returns at once.
// [orig: CNapiGameSession_ResetActiveSession @0x4C8A70 from PreMenu_HostSetup @0x5691F5,
//  CNapiGameSession_CreateSession @0x4C97CC and NapiLib_Shutdown @0x4C958B (the
//  Lobby_UpdateServerInfo(1) call @0x4C8A7B; Lobby_UpdateServerInfo @0x4FE8C0 - the argument
//  test @0x4FE8F2..0x4FE8FA); NapiNPProtocol_StopServer @0x62A820]
void stop_unstopped_server(UnstoppedServer &server, const ServerLogs &logs,
		opennova::IDatagramSocket *socket, const PeerAddr *local_address,
		const std::function<void(NapiNPConnection &slot)> &player_removed = {});

// The browse's or a join's connection create over a held own pair (UnstoppedServer::own_client
// and its twin, the first node, at address 0): the create destroys the session connection it
// finds first, which after an error-arm exit is the held listen host's own client. The client
// latches {2, 2, 0, 0, "N.C:CSSAC", 0, ""} if no record is latched, writes its /INOUT CLIENT
// removal line, sends its 0x46s into the manager's loopback (no wire), and its disconnected
// callback hands the process latch `latch` its record, first wins (null: none); the twin takes
// that 0x46 through the loopback, latching the client's record, and goes pending, so the next
// pump_unstopped_server destroys it (its SERVER line, no wire). The own pair is gone from the
// held server's 0x81 counts from then on. Nothing when no own client is held.
// (D-NET-NEW-L1-26)
// [orig: CNapiGameSession_CreateServerConnection @0x4C9D30 - the +0xE60 test @0x4C9D6B,
//  CNapiNetwork_DisconnectActiveConnection(.., "N.C:CSSAC") called @0x4C9D77, its
//  CNapiNPConnection_Destroy @0x4C9D7D; the callers UI_StartLANSessionSearch @0x55836E /
//  @0x55837E, UI_EnumerateAndJoinSession @0x56A011, CNapiGameSession_InitTransportConnection
//  @0x4C9E16]
void release_unstopped_own_client(UnstoppedServer &server, const ServerLogs &logs,
		ProcessDisconnectLatch *latch);

// A held node's destroy (the stop's and the held pump's reap): its /INOUT removal line with the
// latched record, then its 0x86 burst over `socket` (null: none open, nothing on the wire).
void teardown_unstopped_node(const NapiNPConnection &conn, const ServerLogs &logs,
		opennova::IDatagramSocket *socket);

// The manager's read of one datagram from `from` over a held server (the LAN browse's socket, a
// join's, the joined match's: every socket of the process reads into the one manager, whose
// opcode dispatch finds a held node by the source). A held node's datagram of the server
// opcodes 0x43..0x47 is the held server's: the manager's lines through logs.connect (RAW,
// COOKED, then PROTOCOL or TOSSED [4] by the handler's result; the connect log is off for the
// process once a host took its first connection, so these write only under a log still on),
// then the socket leg at `now_ms` (the host clock's, host_monotonic_milliseconds): a 0x43 or a
// 0x46 whose key is the node's is queued for the next queue pass (UnstoppedServer::queue); a
// resend list resends its packets over `socket` at once; a ping stamps the node's receive and
// answers 0x85 over `socket` when it asks. `taken` is false for every other datagram (the
// caller's own dispatch writes its lines); `net_quality_link_errors` carries the resend
// list's outgoing flag (kNetQualityLinkErrorOutgoing) for the process's indicators.
// A 0x41 from any source is the held protocol's too (its hello handler gates on host_running,
// not on is_authority): its lines, and a hello the identity gate admits takes the held session's
// 0x81 over `socket` (the protocol's words as the exit left them, the uptime at `now_ms`, NP
// and NC the held nodes), its dispatch line PROTOCOL for a PM-less hello, else TOSSED [4].
// A 0x42 from any source is the held protocol's as well (its join handler's head gate is
// host_running alone): its lines, and one whose TLV walk holds the protocol's identity is queued
// for the next queue pass (PROTOCOL), one whose walk ends on a CU bound is taken unqueued
// (PROTOCOL), any other is tossed [4]. (D-NET-NEW-L1-26)
struct UnstoppedReceive {
	bool taken = false;
	uint32_t net_quality_link_errors = 0;
};
UnstoppedReceive receive_unstopped_server(UnstoppedServer &server, const ServerLogs &logs,
		opennova::IDatagramSocket &socket, const PeerAddr &from, const uint8_t *raw,
		std::size_t len, uint32_t now_ms);

// The LAN browse's and a join's protocol pump over a held server, NapiNPProtocol_Pump(-1), at
// `now_ms` (the host clock's, host_monotonic_milliseconds). First the receive's queue pass, in
// arrival order: a queued 0x43 of an active node is parsed (in order: the receive stamp the reap
// measures, the peer's ACK, the ACK owed; a future packet waits on the node's ordered queue and
// latches the missing-sequence check; the records reach no handler: every handler of the
// server table returns at is_authority, which the router's error arm cleared), a queued 0x46
// latches its record and leaves its node pending, and a queued 0x42 takes the join handler's
// queued pass: the protocol's gates (HK 3, the password 4, PV2 7, NA 5, the refusal byte 6, each a
// CR=0 0x82 over `socket`), a held node at its address sent its 0x82 again (the same CI and CK)
// or destroyed, the create (next_connection_index, the manager's keys), the validate callback
// over the held words (the join lock 14 / 2, the address bans 14 / 3, the held nodes against
// the capacity 14 / 4 or 14 / 5), then the accept: the node filled as the live host's
// (fill_joined_node), the connect log's disabling line while it is on, the 0x82 over `socket`,
// its /INOUT line, the MPS's two CS updates queued, and the node appended to the held list,
// where the same pump's legs build them; then each node in list order replays its
// ready successors and, latched with its ordered queue still nonempty, sends its 0x84 over
// `socket`. Then every held node, in list order, takes PumpFlags' legs: its holdoff step; at a
// zero holdoff the build of its queue (the packets framed before the exit, then the messages
// not yet built: the teardown's 0x25s, or a header-only packet when an ACK is owed) over
// `socket`; its send intervals (the keepalive and the retained-records rebuild); the
// receive-timeout reap past cs_dir0's timeout since its last receive ("NP.C:PT:SERTMOUT",
// latched if none is); the flush counter, the prune and the holdoff's reload. Right behind each
// node's legs, before the next node's, the protocol pump's pending test destroys a pending node
// (a reap's, a goodbye's): teardown_unstopped_node over `socket`, and the node leaves the held
// list, so the next reset's StopServer stops only the nodes still held. Nothing when no server
// is held. A held node's slot teardown reaches no handler (is_authority). The callers: the LAN
// browse's start and frames (godot/src/network/lan_session.cpp) and a join attempt's enumerate
// and connect frames (JoinerRole::poll_preload), each after its receive. Returns the 0x84's
// incoming flag (kNetQualityLinkErrorIncoming) when one named a sequence. (D-NET-NEW-L1-26)
uint32_t pump_unstopped_server(UnstoppedServer &server, const ServerLogs &logs,
		opennova::IDatagramSocket &socket, uint32_t now_ms);

// Finalize one host socket receive batch. Future packets latch a missing-sequence check while
// handle_server_datagram drains; only here, after later datagrams could have closed the gap, does
// the host emit one 0x84 per connection whose ordered queue is still nonempty. This is the
// receive pump's own tail, every server tick: the owner sends the result at once, never behind
// the connection's S2C send boundary.
// [orig: recv latch in HandleSessionPacket @0x626c3a; the PumpRecvQueues per-connection tail
//  @0x6269bb..0x6269d6 -> SendMissingSeqList(conn, 0) @0x6269ce -> CNapiNPManager_SendTo
//  @0x62376d, run from PumpServerProtocolRecv @0x51d895 ahead of PumpFlags' 0x10 decrement]
std::vector<TickOut> flush_server_missing_requests(NapiNPServerCtx &ctx);

// Drive the periodic emitter for every connection NOT yet Spawned (so entity_batch_count climbs and
// the spawn gate opens) and frame each session's replies. PeerEnteredWorldStreaming surfaces here
// the tick a peer's batches start (F3). Spawned peers are skipped — Server_TickUpdate owns their 0x0A.
// `run_spawn_pump` runs the pending-player spawn pump first on the periodic second; the host's
// own frame passes false, since Server_TickUpdate runs the pump in its periodic block.
// `run_initial_state_steps` steps each row's §5.2a track here (step_initial_state_row, the rows
// in slot order); the host's own frame passes false, since Server_TickUpdate steps them in its
// tail per-slot block (D-NET-NEW-L1-29).
std::vector<TickOut> tick_connections(
		NapiNPServerCtx &ctx, int elapsed_ms, uint32_t now_tick,
		bool respect_s2c_send_boundary = false, bool run_spawn_pump = true,
		bool run_initial_state_steps = true);

// One row's §5.2a step, the call Server_TickUpdate's per-slot block makes for a row short of
// state 6 (Server_SendInitialGameStateToPlayer): a connection's own row (`net_player` is the row
// itself) or an orphaned row (D-NET-NEW-L1-24; `net_player` is orphan_net_player's, null past the
// player's leave, when nothing runs). The step waits while the net player's send boundary is
// closed (`respect_s2c_send_boundary`), and its records go to the net player's transport; a
// connection with no transport (a protocol-only test seam) has them framed into `outbound`.
// [orig: Server_TickUpdate's per-slot block @0x51E385..0x51E47C - the row's +0x1C @0x51E3AA,
//  its connection @0x51E3B1, the boundary @0x51E3D6, the call @0x51E3E4]
void step_initial_state_row(NapiNPServerCtx &ctx, NapiNPConnection &row,
		NapiNPConnection *net_player, uint32_t now_tick, bool respect_s2c_send_boundary,
		std::vector<std::vector<uint8_t>> &outbound);

// Wrap one in-match S2C inner message (e.g. the per-frame 0x0A body) into a fully-framed 0x83 SESSION
// datagram for `peer`, using that connection's live SCRK + seq (advances its outbound seq). False
// if the peer is unknown / pre-auth.
bool frame_in_match_s2c(NapiNPServerCtx &ctx, const PeerAddr &peer, uint8_t inner_tag,
                        const std::vector<uint8_t> &inner_body, std::vector<uint8_t> &out_datagram);

// Multi-message form used by the host owner to preserve one retail send boundary. The caller is
// responsible for MTU-aware grouping; this advances the connection's sequence exactly once.
bool frame_in_match_s2c_batch(NapiNPServerCtx &ctx, const PeerAddr &peer,
                              const std::vector<ProtocolMessage> &messages,
                              std::vector<uint8_t> &out_datagram);

// P5 — the production PeerC2SInMatch consumer. Route each decoded in-match C2S 0x0C carried by a
// PeerC2SInMatch `event` onto its owning connection's transport (ISessionTransport::deliver_c2s), so
// the NEXT Server_TickUpdate's drain_connection_c2s read-applies it. This is the owner-boundary form
// of the manual push the since-removed joiner_connection_test did inline. Deliberately SEPARATE from
// handle_client_session (which only SURFACES the event): the single C2S drain/apply is
// Server_TickUpdate (D-NET-125), so the consumer stages into that drain rather than applying inline
// (which would be a double-apply trap). A null/unbound owning transport or a non-0x0C inner message
// is skipped. Returns the number of 0x0C uplinks staged. [D-NET-126; orig: NapiNPServerMsg_0x00C
// @0x501c30 -> NetPacket_DispatchEntityPacketCallback @0x4D6A80; docs/net/novaworld-net-re.md §5.44]
std::size_t apply_in_match_c2s(NapiNPServerCtx &ctx, const HostAcceptEvent &event);

// Test seams: the handshake/spawn tests read and pin a node through these three; no production
// caller uses them. connection_spawned is true once `peer` has completed handshake + spawn (its
// connection is live); bind_connection_player binds a slot + entity onto an authenticated node;
// connection_count is the node count.
bool connection_spawned(const NapiNPServerCtx &ctx, const PeerAddr &peer);

bool bind_connection_player(NapiNPServerCtx &ctx, const PeerAddr &peer, uint8_t player_slot,
                            uint16_t entity_handle);

// The host-initiated destroy of `peer`'s node: the S2C 0x86 SERVER_GOODBYE burst is appended to
// `goodbye_out` (when non-null) BEFORE the player/entity/roster teardown and the node erase —
// retail sends the burst, then fires the removal callback, then clears keys and queues.
// Producers: the receive reap (SERTMOUT), the pending-disconnect pump, StopServer, same-endpoint
// replacement, a received 0x46 (its record echoed back). A null `goodbye_out` is the owner's
// dead-endpoint eviction (the transport already knows the address is gone): the same complete
// teardown with no wire output. The owner is responsible for releasing its own (non-owning)
// transport for that peer and for shipping the datagrams. Returns true if a node was dropped.
// [orig: CNapiNPConnection_Destroy @0x62A4B0 -> TeardownActiveConnection @0x6253C0 (burst
//  @0x6253ef, removal callback @0x625426, keys/queues cleared @0x625535..0x625574) ->
//  Server_HandlePlayerDisconnect @0x51B5C0]
bool destroy_connection(NapiNPServerCtx &ctx, const PeerAddr &peer,
		std::vector<std::vector<uint8_t>> *goodbye_out);

std::size_t connection_count(const NapiNPServerCtx &ctx);

} // namespace opennova::inmatch
