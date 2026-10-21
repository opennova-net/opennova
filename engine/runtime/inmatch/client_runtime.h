#pragma once

#include <runtime/inmatch/chat_flood.h>
#include <runtime/inmatch/client_console.h> // the S2C 0x06 echo's command walk (D-NET-710)
#include <runtime/inmatch/file_transfer_session.h> // the process's S2C 0x02 probe words
#include <runtime/inmatch/joiner_connection.h>
#include <runtime/inmatch/mission_exit.h> // ProcessDisconnectLatch (the process latch)
#include <runtime/devtools/tick_profile.h>
#include <runtime/hud/hud_chat_entry.h> // ChatSendResult (the C2S 0x0D sender's outcome)
#include <runtime/hud/net_quality_indicators.h> // g_NetQuality's display state
#include <runtime/hud/feed_format.h>    // FeedPost (the client-raised ring lines)
#include <runtime/hud/squad_feed.h>     // SquadFeedLine (the squad folds' HUD lines)

#include <runtime/replication/client_replica_pipeline.h> // ClientReplicaPipeline / ClientState
#include <runtime/replication/net_quality.h>             // the CNetQuality window (the client half)
#include <runtime/inmatch/session_transport.h>        // ISessionTransport
#include <runtime/inmatch/vehicle_menu.h>             // VehicleMenu (vehicle.mnu VEHICLE)
#include <runtime/world/weapon_inventory.h>          // LoadoutWeaponNameList (the process's name list)

#include <net/npwire/ingame_decode.h>     // PlayerExtendedUplink (the §5.10 0x0C body)

#include <cstddef>
#include <cstdint>
#include <array>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// P5 — the headless, socket-free CLIENT runtime. The faithful reimpl of the per-frame client net
// role [orig: Client_ProcessNetworkFrame @0x42c180] composed with the connect-leg state machine
// (inmatch::JoinerConnection) and the S2C->ClientState fold (replication::ClientReplicaPipeline). The original runs
// the SAME per-frame function on every machine (it is NOT authority-gated); only the 0x0C uplink
// inside it is gated !is_authority. So this runtime has two roles, mirroring that:
//
//   Role::Joiner     — a remote client joining a host. Drives the in-match connect legs
//                      (Hello->Auth->Driving spawn-gate burst->InMatch via JoinerConnection; the
//                      lobby GATE->VERIFY->READY->PLAY legs are the SEPARATE ADR-0010 ClientSession
//                      matchmaking flow, owned by the binding, NOT here), then per frame folds
//                      inbound S2C into ClientState and emits the C2S 0x0C uplink.
//   Role::HostClient — the SP listen-server host's OWN local view (is_authority == 1). Its player
//                      is a server-side entity (ADR 0011/0012), so there is NO handshake and NO 0x0C
//                      uplink (the witnessed !is_authority gate). Per frame it ONLY folds its own
//                      0x0A off the in-process loopback the host's Server_TickUpdate emits onto.
//
// FRAMING LAYERS (kept strictly separate — the P5-review fix):
//   - Joiner traffic is whole NWU-framed datagrams (0x41/0x42/0x43/0x81/0x82/0x83). The owner ships
//     start()/Client_ProcessNetworkFrame() return values over the socket and feeds received
//     datagrams to receive(). JoinerConnection does the NWU+SCRK decode and surfaces the inner
//     {tag,body} bodies, which we fold via ClientReplicaPipeline::apply (NOT through a transport).
//   - HostClient inbound is the inner {tag,body} the host's Server_TickUpdate host_send()s onto the
//     in-process LoopbackChannel (the ADR-0011 §3 SP crypto bypass — no 0x83 framing on the
//     loopback), folded via ClientReplicaPipeline::pump(transport).
//
// [orig: Client_ProcessNetworkFrame @0x42c180; PumpClientProtocolRecv @0x42c228 / Send @0x42c4bc;
//  Player_BuildTag0CInputBody @0x42a550; docs/net §5.44]. No socket I/O lives here.
namespace opennova::world { class World; }
namespace opennova::audio { struct DialogLineRef; }
namespace opennova::def { struct DefItemsFile; }
namespace opennova::gamecfg { struct GameCfg; }

namespace opennova::inmatch {

class ConnectLog; // punt_log.h

// The client half's process-lived words: retail keeps the 0x4C report's frame count, the
// revive countdown's frame count and the vehicle screen's state in globals for the whole
// process, so they run on across sessions, map changes and roles. The embedder that outlives
// its sessions owns one and hands it to every client runtime (ClientRuntime::use_process_state);
// a runtime with none keeps its own (a test fixture's).
// [orig: g_NetQualityReportTimer @0xA85B84 - its only writers Client_ProcessNetworkFrame
//  @0x42C239 (the step) and @0x42C246 (the zero at the report)]
struct ClientProcessState {
	// The report's period: the 311th step fires. [orig: Client_ProcessNetworkFrame @0x42C23E]
	static constexpr uint32_t kNetQualityReportInterval = 310;
	uint32_t net_quality_report_timer = 0;
	// The 1 Hz revive countdown's frame count: stepped by every client net frame and zeroed
	// only by its own 63rd step, so its phase runs on across sessions.
	// [orig: g_SlotRefreshTimer @0xA85B80 -- its only accesses Client_ProcessNetworkFrame
	//  @0x42C27E..0x42C288 (the step) and @0x42C2DA (the zero)]
	uint32_t slot_refresh_timer = 0;
	// The vehicle screen's state (vehicle_menu.h): the poll flag, its frame counter, the
	// selection and the list live with the menu scene, which only the expansion reload or
	// the quit destroys, so a later session in the same process polls from its start.
	// [orig: g_VehicleMenuOpen @0x25DCF24, g_VehicleMenuPollCounter @0x25DCF28,
	//  g_VehicleMenuSelection @0x25DCF2C; VehicleMenu_Close @0x563AC0, dispatched by
	//  CUIScene_DestroyAllContent alone (@0x63C4DC)]
	VehicleMenu vehicle_menu;
	// The S2C 0x70 table and its received flag, which no session or mission start touches
	// (the client runtime points its replica's store here: use_vehicle_spawn_store).
	// [orig: g_VehicleSpawnAvailabilityReceived 0xA81BB0, the table 0xA81BC0]
	replication::ClientVehicleSpawnAvailability vehicle_spawn_availability;
	// The process's file-transfer session words (file_transfer_session.h): the id each S2C 0x02
	// probe carries and its step countdown, stepped by every authority mission start (HostRole's
	// bring-ups, a map change's and a single player's included) and by the server tick's head,
	// which the host context reaches through NapiNPServerCtx::file_transfer_session.
	// [orig: g_FileTransferSessionId @0xC86FCC, g_FileTransferSessionStepCountdown @0xC86FD0;
	//  Game_StartMission @0x524360 (the add @0x5244B4, the store @0x5244C6); Server_TickUpdate
	//  @0x51D7E0 (@0x51D809..0x51D820); CNetPlayer_SendFileTransferChunk @0x4C9440 (the read
	//  @0x4C9468)]
	FileTransferSession file_transfer_session;
	// One client net frame's step, which every mission frame of every role runs (a Serve Only
	// host's included, with no client runtime): true at the 311th, which zeroes the word before
	// the session test, so the caller's report (in session, a session peer) is its own.
	// [orig: Game_ProcessMainFrame @0x5263F0 (the unconditional call @0x526692); Client_ProcessNetworkFrame
	//  @0x42C22D..0x42C23E (the step), @0x42C246 (the zero, between the session compare @0x42C240
	//  and its branch @0x42C24C), the peer compare @0x42C24E (its branch @0x42C254)]
	bool step_net_quality_report() {
		if (++net_quality_report_timer <= kNetQualityReportInterval) return false;
		net_quality_report_timer = 0;
		return true;
	}
	// g_LocalPlayerEntity as the pool slot it points at (JoinerConnection::
	// local_player_entity): bound by each mission start's Player_InitPlayer, never cleared.
	// [orig: Player_InitPlayer @0x4E15F0 - the store @0x4E17EB]
	uint16_t local_player_entity = 0xFFFFu;
	// g_LoadoutWeaponNameList (world/weapon_inventory.h): the image's initializer until an
	// armory ACCEPT or a 0x5A apply writes it; no session or mission start touches it. The
	// host role binds its worlds to it (World::loadout_weapon_names).
	// [orig: g_LoadoutWeaponNameList @0x8333F8; its writers WeaponLoadout_ApplyFromBuffer
	//  @0x565D46 and NapiNPClientMsg_HandleWeaponLoadoutSync @0x42920C..0x4292F0]
	world::LoadoutWeaponNameList loadout_weapon_names;
	// g_SpawnLoadoutBuffer (world/weapon_inventory.h SpawnLoadoutBuffer): empty in a fresh
	// process, its writers' alone; the roles bind their kernels' loadouts to it
	// (LocalPlayerLoadout::process_buffer). [orig: g_SpawnLoadoutBuffer @0x24D4E00]
	world::SpawnLoadoutBuffer spawn_loadout_buffer;
	// g_RoundStateInitTick as the last session create's round init stamped it
	// (NapiNPServerCtx::round_state_init_tick): a single player's restart, which runs no
	// round init, keeps it. [orig: Server_InitNewRoundState @0x51CBA0; Game_RestartRoundSP
	//  @0x5263A0 -> Game_StartMission(1) @0x5263DB, no round init]
	uint32_t round_state_init_tick = 0;
	// The process's cfg block (g_GameConfigState, the embedder's game.cfg, which the host
	// dialog and an admin SET write): every mission start copies its timeLimit into
	// g_RoundTimeLimitMinutes, which a joiner's start seeds its round clock from
	// (ClientRuntime::push_mission_start_end). Null: none (a fixture's), no seed.
	// [orig: Game_ApplySessionSettingsToGlobals @0x551500 (the copy @0x551CCC..0x551CD2), called
	//  by Game_StartMission @0x524360 (@0x524662)]
	const gamecfg::GameCfg *config_block = nullptr;
	// ctx+0xD94, the client callback's disconnect latch (mission_exit.h ProcessDisconnectLatch):
	// process-lived, first-wins, cleared at a join's connect start. The host's own client writes
	// it at its teardown (NapiNPServerCtx::client_disconnect_latch), a joiner at its connected
	// teardown (JoinerConnection::use_disconnect_latch) (D-NET-NEW-L1-26).
	ProcessDisconnectLatch disconnect_latch;
};

class ClientRuntime {
public:
	// The process-lived client words this runtime steps (null: its own).
	void use_process_state(ClientProcessState *state) {
		external_process_ = state;
		if (joiner_) joiner_->use_local_player_entity_word(state != nullptr ? &state->local_player_entity : nullptr);
		// The joiner's leave callback writes the process latch (D-NET-NEW-L1-26).
		if (joiner_) joiner_->use_disconnect_latch(&process_state().disconnect_latch);
		view_.use_vehicle_spawn_store(&process_state().vehicle_spawn_availability);
	}
	enum class Role : uint8_t { Joiner, HostClient };
	// The process's _connectlog.txt (punt_log.h ConnectLog; null: off): every S2C 0x61
	// this client handles closes it (ConnectLog::disable_at_client_session_key); while it
	// is on a joiner writes its client callbacks' lines (log_client_sends, the connection's
	// receive-side lines).
	void set_connect_log(ConnectLog *log) {
		connect_log_ = log;
		if (joiner_) joiner_->set_connect_log(log);
	}

	// Remote-joiner runtime. Transport-less: framed bytes in via receive(), framed bytes out via
	// the start()/Client_ProcessNetworkFrame() return values (the owner pumps the socket).
	explicit ClientRuntime(std::string player_name);
	// Same runtime with an injected monotonic wall clock for deterministic integration/tests.
	ClientRuntime(std::string player_name,
	              JoinerConnection::MonotonicMilliseconds monotonic_milliseconds);

	// SP host-as-client runtime. `host_loopback` is the in-process channel the host's
	// Server_TickUpdate emits S2C onto (non-owning). is_authority is implicit (no 0x0C uplink).
	explicit ClientRuntime(replication::ISessionTransport &host_loopback);

	Role role() const { return role_; }
	bool is_authority() const { return role_ == Role::HostClient; }

	// --- connect / recv (Joiner) ---
	// Begin the handshake; returns the framed ClientHello to ship (Idle->Hello). Empty for HostClient
	// and for a join from a discovered session, whose 0x42 the first pump past its start sends
	// (D-NET-NEW-L2-24; an enumerated join's 0x81 leads to the same start a pass later,
	// D-NET-NEW-L2-25).
	std::vector<uint8_t> start();

	// The joiner's in-place reload into the host's next mission, which the
	// shell runs when the main frame's verdict on the stored exit (4, in
	// session, not the authority) is the Game Loop (inmatch::main_frame_exit):
	// the connection stays, the exit reason clears, the replica and the
	// per-mission runtime state start over with the round gate held, and the
	// reload's first legs queue (JoinerConnection::begin_mission_reload).
	// False for HostClient or out of a session.
	// [orig: Game_ProcessMainFrame @0x52681F..0x526841 pushes the Game Loop;
	//  GameLoop_Init @0x526370 -> Game_StartMission(0)]
	bool begin_mission_reload();

	// Deposit one received FRAMED datagram (off the socket) for the next frame's recv pump to drain.
	// Mirrors CNapiNetwork_PumpManagerReceive feeding the byte recv FIFO ahead of the frame's recv
	// pump. No-op for HostClient (it reads its loopback transport directly).
	void receive(const uint8_t *raw, std::size_t len);

	// Frame the retail leave burst (0x46 ClientGoodBye x4) for the owner to ship before dropping
	// the socket. Empty for HostClient, before ServerAuth, or on a repeat call.
	// [orig: CNapiNPConnection_TeardownActiveConnection @0x6253c0]
	// `leave_tag`: JoinerConnection::disconnect's.
	std::vector<std::vector<uint8_t>> disconnect(std::string_view leave_tag = {});
	// The session destroy, FullDestroy(reason): the leave burst latching "N.C:SWWAD[1]:<reason>"
	// (disconnect), then the transport's teardown, which writes NET TEARING DOWN SOCKET while the
	// connect log is on, unless the join runs on NovaWorld (`novaworld_join`: the menu's connect
	// type 1 keeps the transport).
	// [orig: CNapiGameSession_FullDestroy @0x4C96A0 - the sprintf @0x4C96CC, the leave @0x4C96DB,
	//  `networkConnectType_480 != 1` @0x4C9722, the socket state @0x4C974E,
	//  CNapiNetwork_TearDownSocket (the calls @0x4C9763 / @0x4C976F); CNapiNetwork_TearDownSocket
	//  @0x4C4C90 - the line @0x4C4CBE..0x4C4CE7 ahead of NapiNPManager_CloseSocket @0x4C4CF5]
	std::vector<std::vector<uint8_t>> destroy_session(int32_t reason, bool novaworld_join);

	// Queue one reliable C2S 0x1D stance change (0xA9 crouch / 0xAA prone / 0xAC stand) from
	// the stance key SELECT; it leaves inside the next open send boundary in queue order.
	// False for HostClient or before in-match. [orig: cases 169/170/172 @0x4e0d77/@0x4e0df3/
	// @0x4e0e3e -> CNapiNetwork_QueueReliableMessage @0x4e0de7]
	bool queue_stance_change(uint16_t action_id);
	// Queue one reliable C2S 0x1A door section request {handle, record state,
	// section} the joiner world's door callback raised; it leaves inside the
	// next open send boundary in queue order. False for HostClient or before
	// in-match. [orig: NetPacket_SendDoorSlotRequest @0x42D0C0 ->
	// CNapiNetwork_QueueReliableMessage @0x42d169]
	bool queue_door_request(uint16_t handle, int16_t state, uint8_t section);

	// Effective gameplay readiness. The C2S gameplay sends (0x0C uplink, 0x06 fire)
	// require both retail's g_ClientDeployHold hold to be open and the independent
	// authoritative spawn/health latch to be released; the frame's 0x2C ping needs the
	// hold alone, so a dead player still pings (D-NET-529).
	// Every valid 0x5A opens the literal gameplay gate even while a spawn-zone
	// deploy screen remains pending; queuing C2S 0x0E closes only that gate. A
	// complete recipient-local 0x0A tail with health <= 0 closes only the
	// authoritative spawn latch. Positive health does not reopen it: death
	// re-enters JoinerConnection's deployment-pick FSM, and only the applicable
	// post-pick release reopens spawn.
	bool is_deployed() const {
		return deployed_ && authoritative_spawn_released_;
	}
	// The literal g_ClientDeployHold state. Every valid S2C 0x5A opens it, including
	// an unrelated loadout echo while dead; that alone cannot resume gameplay.
	bool gameplay_gate_open() const { return deployed_; }
    // Local Game_InitNewRound clears the medic latch and gameplay hold.
    void reset_local_round_state();
	// Monotonic receive-side deployment-release edge. It advances when initial
	// establishment becomes applicable (even if deploy UI remains pending), and
	// again on an ACK-qualified post-pick release or the self record's respawn
	// edge (a medic revive's deploy sends no 0x5A). Unrelated valid 0x5A grants
	// still open gameplay_gate_open(), but do not invent a pose/respawn edge.
	// Consumers use the revision,
	// rather than positive health alone, to distinguish a real respawn from a
	// stale 0x0A tail.
	uint64_t deployment_release_revision() const {
		return deployment_release_revision_;
	}
	// The independent authoritative spawn/health latch. Death closes it; an
	// applicable 0x5A or the self record's respawn edge reopens it. Input case 12
	// never changes it, so a player-paced
	// pick cannot manufacture a local death while its gameplay hold is armed.
	bool authoritative_spawn_released() const {
		return authoritative_spawn_released_;
	}
	uint64_t authoritative_spawn_release_revision() const {
		return authoritative_spawn_release_revision_;
	}

	// The inbound ordered frontier (our echoed ack_count) and our outbound sequence.
	// A frontier that stops advancing while the socket still carries traffic is the
	// replication-freeze signature: the peer's reliable records never retire, so it
	// stops emitting new semantic messages, and we stop admitting new ones.
	uint32_t inbound_frontier_seq() const {
		return (role_ == Role::Joiner && joiner_ != nullptr)
				? joiner_->connection().seq.last_inbound_seq
				: 0;
	}
	uint32_t outbound_seq() const {
		return (role_ == Role::Joiner && joiner_ != nullptr)
				? joiner_->connection().seq.next_outbound_seq
				: 0;
	}

	// Inbound-gap / retention diagnostics (0 for HostClient) — the frozen-session
	// signature is a gap queue that never drains + retention that only grows.
	std::size_t inbound_gap_depth() const {
		return (role_ == Role::Joiner && joiner_ != nullptr) ? joiner_->inbound_gap_depth() : 0;
	}
	std::size_t retained_outbound_depth() const {
		return (role_ == Role::Joiner && joiner_ != nullptr) ? joiner_->retained_outbound_depth()
		                                                     : 0;
	}
	uint32_t send_flush_counter() const {
		return (role_ == Role::Joiner && joiner_ != nullptr)
				? joiner_->send_flush_counter()
				: 0;
	}

	// Anti-cheat challenge counters and the last structured reject/disconnect
	// records (defaults for HostClient) — the live-join punt diagnostics.
	JoinerConnection::ChallengeDiagnostics challenge_diagnostics() const {
		return (role_ == Role::Joiner && joiner_ != nullptr)
				? joiner_->challenge_diagnostics()
				: JoinerConnection::ChallengeDiagnostics{};
	}
	JoinerConnection::JoinRejectRecord last_join_reject() const {
		return (role_ == Role::Joiner && joiner_ != nullptr)
				? joiner_->last_join_reject()
				: JoinerConnection::JoinRejectRecord{};
	}
	// The joiner connection's error record (empty for any other role).
	ConnectionErrorRecord connection_error_record() const {
		return (role_ == Role::Joiner && joiner_ != nullptr)
				? joiner_->connection_error_record()
				: ConnectionErrorRecord{};
	}
	// The join screen's status line inputs (pre_game_menu.h); Joining with an
	// empty queue for any other role.
	JoinScreenStage join_screen_stage() const {
		return (role_ == Role::Joiner && joiner_ != nullptr) ? joiner_->join_screen_stage()
		                                                     : JoinScreenStage::Joining;
	}
	JoinQueueRecord join_queue() const {
		return (role_ == Role::Joiner && joiner_ != nullptr) ? joiner_->join_queue()
		                                                     : JoinQueueRecord{};
	}
	// The joiner's monotonic clock (the queue record's queued-since time base).
	uint64_t join_clock_ms() const {
		return (role_ == Role::Joiner && joiner_ != nullptr) ? joiner_->monotonic_now_ms() : 0;
	}
	bool has_disconnect_event() const {
		return role_ == Role::Joiner && joiner_ != nullptr &&
				joiner_->has_disconnect_event();
	}
	DisconnectEvent last_disconnect_event() const {
		return (role_ == Role::Joiner && joiner_ != nullptr)
				? joiner_->last_disconnect_event()
				: DisconnectEvent{};
	}

	// The per-frame client net role [orig: Client_ProcessNetworkFrame @0x42c180], in witnessed order:
	//   (1) recv pump: Joiner drains the recv FIFO -> JoinerConnection decodes -> drive the connect
	//       legs + fold inner S2C bodies into ClientState [orig: PumpClientProtocolRecv @0x42c228];
	//       HostClient pumps the loopback transport.
	//   (1b) connect-drive (Joiner): while Driving, pump() the next spawn-gate burst stage.
	//   (2) send (Joiner only): if InMatch && deployed -> frame_c2s_uplink(self_handle, type, uplink)
	//       [orig: Player_BuildTag0CInputBody->QueueReliableMessage(0x0C) @0x42c482]. The host
	//       (is_authority) never uplinks its own player (witnessed !is_authority gate); HostClient
	//       sends nothing.
	// Returns the framed datagrams to ship (handshake replies + burst + the per-frame housekeeping +
	// the 0x0C). `uplink` is the §5.10 0x0C extended body (the OUTPUT of Player_BuildTag0CInputBody; the
	// raw-input->entity motor, step 5a, runs host-side per ADR 0012 R1 and is NOT part of the headless
	// client). The witnessed per-frame housekeeping is now PORTED (P6, §5.44): the 0x34 keepalive
	// (29760-tick), the 0x4C net-quality report (310-tick), the 0x2C RTT ping (every deployed frame),
	// and the send_holdoff_countdown send-block gate — all on the Joiner role (HostClient's own-loopback
	// housekeeping stays deferred-and-logged). seed_session() replay mode suppresses them for byte-parity.
	std::vector<std::vector<uint8_t>> Client_ProcessNetworkFrame(const PlayerExtendedUplink &uplink,
	                                                             uint32_t now_tick = 0);
	// The uplink BUILT at the send block, after this frame's receive fold, as
	// the writer runs there: the builder fills the body (false = none this
	// frame) and is called only when the deployed send block opens.
	// [orig: Client_ProcessNetworkFrame -- PumpClientProtocolRecv @0x42C228
	//  ahead of Player_BuildTag0CInputBody @0x42C482]
	using UplinkBuilder = std::function<bool(PlayerExtendedUplink &)>;
	std::vector<std::vector<uint8_t>> Client_ProcessNetworkFrame(const UplinkBuilder &build_uplink,
	                                                             uint32_t now_tick = 0);
	// No-uplink frame (HostClient, or a pre-deploy Joiner): recv pump + connect-drive only, no 0x0C.
	std::vector<std::vector<uint8_t>> Client_ProcessNetworkFrame(uint32_t now_tick = 0);
	// The frame's phases (SIM_CLIENT_SETUP/RECEIVE/MAINTENANCE/SEND) lap onto
	// this profile (the embedder's, normally the world's; null = no clocks).
	void set_profile(devtools::TickProfile *profile) { profile_ = profile; }
	// Typed gameplay seams used by the simulation; protocol tags/framing remain
	// owned here. Fire is predicted locally before queueing C2S 0x06. The spent clip
	// remains unchanged until the host's S2C 0x49 echo appears in the reload drain.
	bool queue_fired_round(const ClientFiredRound &round);
	bool queue_reload_request(const WeaponReload &reload);
	// The dead player's medic call: reliable C2S 0x2E carrying the local
	// entity's packed index. The wire gates live here (in-session joiner
	// with a self handle); the dead test and the 310-tick cooldown are the
	// embedding sim's local-player facts [orig: Input_HandleActionBinding
	// case 217 @0x49b4b4..0x49b51b — is_in_session, local entity, Flags & 2,
	// dword_B76804 == 0; NetPacket_WriteEntityIndex32 -> QueueReliableMessage
	// (0x2E, param 0x136)].
	bool queue_medic_request();
	// The Respawn key's request: one bodiless reliable C2S 0x41, retained
	// until acknowledged, on an in-session joiner's connection or the listen
	// host's own (its server's 0x41 handler then names the host's own slot).
	// The live-player and round gates are the role's (Role::request_respawn)
	// [orig: NetPacket_SendRequestRespawn @0x42DBA0 --
	//  CNapiNetwork_QueueReliableMessage(0x41, 1, 0, payload, 0) @0x42DBBC].
	bool queue_respawn_request();
	// The listen host's own client queues a C2S on its local connection, as
	// every input-side sender does: its client frame sends it and the next
	// frame's server tick dispatches it (the queue_stance_change path). False
	// off the HostClient role. [orig: CNapiNetwork_QueueReliableMessage
	//  @0x4c4fa0 on the local connection]
	bool queue_host_message(uint8_t tag, std::vector<uint8_t> body);
	// The HostClient's send block: its held messages onto the loopback. The
	// client frame runs it after its receive fold, and the host frame again
	// after apply_received_effects, whose handler sends retail queues inside
	// the same receive pump, ahead of that frame's send.
	// [orig: Client_ProcessNetworkFrame -- PumpClientProtocolRecv @0x42c228,
	//  PumpClientProtocolSend @0x42c4bc]
	void flush_host_sends();
	// One C2S 0x0D chat line `[u8 channel][cstr text]` on the wire channel the
	// caller's sender picked (hud::chat_dispatch_channel: 13 local, 1 global,
	// 2 team, 12 squad, 11 crew; 4 red / 5 blue send only from a non-peer,
	// and every process with a HUD is a session peer, so they queue nothing).
	// The retail sender gates: non-empty; `!g_DeathScreenActive ||
	// g_RoundOver` — the local and crew keys test `!g_DeathScreenActive`
	// alone; the flood table (a refusal is `Flooded`: the caller echoes the
	// line); then the `<...>` strip. A joiner queues it reliable with the
	// 310-flush lifetime; the listen host's own client (a peer too) sends it
	// over its loopback to its own server, which fans it like any peer's.
	// `text` is cut to 59 characters in place by the flood check; `frame` is
	// the per-main-frame counter.
	// [orig: Chat_SendLocalMessage @0x49A840 (13), Chat_SendTeamMessage @0x49A900 (IDB
	//  misnomer; 1), Chat_SendGlobalMessage @0x49A6B0 (IDB misnomer; 2),
	//  Chat_SendSquadMessage @0x49AA50 (12), Chat_SendAdminMessage @0x49A780
	//  (11), Chat_SendAllMessage @0x49AC70 (4), Chat_SendBlueTeamMessage @0x49ABA0 (5) ->
	//  CNapiNetwork_QueueReliableMessage(0xD, 1, 310)]
	hud::ChatSendResult queue_chat_message(uint8_t channel, std::string &text, uint32_t frame);
	// The command entry's sender, Client_ProcessConsoleCommand's client half: the
	// one-second guard (which compares the tick with the last stamp minus 1000,
	// unsigned, so it drops a line only after the tick ran backwards or within the
	// tick's first second), Client_ProcessLocalCommand's arms (client_console.h),
	// then a session peer queues the raw line as reliable C2S 0x04 [line][NUL]
	// (user param 0): a joiner on its connection, the listen host's own client over
	// its loopback to its own server. An authority that is not a peer (a single-player
	// host's own client) runs the authority's arms (set_authority_console: the NETLOG
	// handler, BAN / PUNT) and then the client's walk (client_parse_and_execute), its
	// lines to the SYSTEM ring and its sends nowhere (no connection). True when the
	// line was taken. VERSION's version and title words default to the title's
	// (gameprofile's version text, g_TitleText) when the embedder sets none.
	// [orig: Client_ProcessConsoleCommand @0x520E80 - the guard @0x520ea4..0x520ec7,
	//  Client_ProcessLocalCommand @0x520BF0 (the call @0x520EEC), the peer test @0x520EF8,
	//  NetPacket_WriteNullableString @0x42A430 and QueueReliableMessage(4, 1, 0) @0x520f29; the authority test @0x520F49,
	//  Server_HandleAdminNetlogCommand @0x520F55, Server_HandleBanPuntCommand @0x520F63,
	//  NapiClientCmd_ParseAndExecute @0x520F70; Game_ParseCommandLineAndInit @0x4A7310 (the title
	//  sprintf @0x4A7D30..0x4A7D3C, g_TitleText)]
	bool console_command(const std::string &line);
	// The local arms' seams the embedder owns (SAVEKEYS, SAVENFO, VERSION's words);
	// in_session, version_line and queue_action are this runtime's.
	void set_local_command_seams(ClientLocalCommandSeams seams) {
		local_command_seams_ = std::move(seams);
	}
	// The authority's arms of a console line on an authority that is no session peer,
	// over the host's context (Server_HandleAdminNetlogCommand, then
	// Server_HandleBanPuntCommand, each with no sender): true when one took the line.
	// The embedder holding the context sets it; unset, the walk alone runs.
	void set_authority_console(std::function<bool(const std::string &line)> arms) {
		authority_console_ = std::move(arms);
	}
	// Input_QueueEvent's queue as the console fills it (the local command's binding
	// CRC arm and the S2C 0x06 walk's binding arm): a record per action, up to 32;
	// one past the cap is dropped (the queue's one live gate: no catalog record
	// carries 0x2000000 and the 0x1000000 gate is dead). The next input frame
	// (Role::apply_input) takes them in order and dispatches each by its code ahead of
	// that frame's own presses, as the devices queue theirs behind.
	// [orig: Input_QueueEvent @0x4979C0 - `cmp count, 20h; jge` @0x4979C8..0x4979CB,
	//  the gates @0x4979D1..0x497A0B, the store @0x497A11..0x497A67; Input_ProcessFrame
	//  @0x49D520 - Input_ProcessPlayerFrame @0x49D58B appends, the drain @0x49D599..0x49D5E7
	//  into Input_HandleActionBinding @0x49D5D8]
	struct ConsoleAction {
		int32_t code = 0;
		int32_t p1 = 0;
		int32_t p2 = 0;
		int32_t p3 = 0;
	};
	static constexpr std::size_t kInputEventQueueCap = 32;
	bool queue_console_action(int32_t code, int32_t p1, int32_t p2, int32_t p3);
	std::vector<ConsoleAction> take_console_actions();
	// Where a dispatched console action whose arm the role does not own goes: the
	// embedder's action dispatch by code (its screens' and devices' arms). Unset: the
	// action goes nowhere. The game's sink takes 3 the exit (input_action_exit), 12 the
	// deploy pick from the record's parameter (0 the default, 0xFFFE the auto pick, else
	// the spawn-zone list's index plus one), 34 the punt of the slot the parameter names
	// (reason 33, "ComIdPunt", the authority's alone), and past their rows' head
	// (Role::action_head_admits) the class callback's 6 the scope (a mounted gun's
	// C2S 0x16) and 182..191 the seat selects (Entity_FindAvailableSeat's index 0..9, a
	// joiner's C2S 0x26). The stance selects are the role's (Role::request_stance).
	// [orig: Input_HandleActionBinding @0x49AD40 - case 3 @0x49AF26..0x49AF42, case 12
	//  @0x49B024 (the pick @0x49B0C3..0x49B17B), case 34 @0x49B766..0x49B773, the class
	//  callback @0x49AEB6 -> Input_HandleActionBinding_0 @0x4E0420 - case 6
	//  @0x4E0492..0x4E04F0, case 182 @0x4E0B7E (the calls @0x4E0B81..0x4E0C22); the catalog rows 105
	//  (flags 0x4C000C01) and 17..26 (0x0C000C01)]
	void set_console_action_sink(ClientConsoleQueueAction sink) {
		console_action_sink_ = std::move(sink);
	}
	void dispatch_to_console_action_sink(const ConsoleAction &action) const {
		if (console_action_sink_) console_action_sink_(action.code, action.p1, action.p2, action.p3);
	}
	// An Emotes / Radio menu pick (1..10): C2S 0x14 / C2S 0x13 [i16 value].
	// The sender carries no gate of its own: CNapiNetwork_QueueReliableMessage
	// queues on any live connection. A joiner queues it with the one-send
	// lifetime (user param 1) on the held one-shot queue (the pick runs
	// outside the client net frame); the listen host's own client sends it
	// over its loopback to its own server. False when no connection carries it.
	// [orig: NetPacket_SendEmoteRequest @0x42C120 /
	//  NetPacket_SendRadioCallRequest @0x42C150 ->
	//  CNapiNetwork_QueueReliableMessage(tag, 0, 1, payload, 2) @0x4c4fa0]
	bool queue_voice_menu_pick(uint8_t tag, int16_t value);
	// The DEATH screen's SWAP_TEAMS click: one reliable C2S 0x4D with no body
	// and no finite lifetime (the joiner's held one-shot queue; the listen
	// host's own client over its loopback), then every minimap overlay timer
	// aged by 0x48A8 (18600) ticks. False when no connection carries it.
	// [orig: DeathScreen_OnSwapTeams @0x5535B0 — the click event 0x3000001
	//  @0x5535B0, NetPacket_SendPingRequest @0x5535BA (a misnomer:
	//  CNapiNetwork_QueueReliableMessage(0x4D, 1, 0, .., 0) @0x42DDB1),
	//  MapOverlay_UpdateTimers(0x48A8) @0x5535C4]
	bool queue_team_change_request();
	// THE VEHICLE SCREEN'S SENDS (inmatch/vehicle_menu.h): the availability ask
	// C2S 0x42 (no body) and the spawn pick C2S 0x40 [u16 bay][u8 group], each
	// one reliable message with no finite lifetime (a joiner's held one-shot
	// queue; the listen host's own client over its loopback to its own
	// server). False when no session carries it.
	// [orig: NetPacket_SendVehicleSpawnAvailabilityRequest @0x42DBE0 --
	//  QueueReliableMessage(0x42, 1, 0, .., 0) @0x42DBFC;
	//  NetPacket_SendVehicleSpawnRequest @0x42DB50 -- the body
	//  @0x42DB64..0x42DB77, QueueReliableMessage(0x40, 1, 0, .., 3) @0x42DB81]
	bool queue_vehicle_spawn_availability_request();
	bool queue_vehicle_spawn_request(uint16_t source_handle, uint8_t type_index);
	// The S2C 0x70 rows and received flag (process-lived: ClientProcessState).
	const replication::ClientVehicleSpawnAvailability &vehicle_spawn_availability() const {
		return view_.vehicle_spawn_availability();
	}
	// The S2C 0x70 received flag the screen's first open clears ahead of its
	// ask [orig: VehicleMenu_Open @0x56425D].
	void clear_vehicle_spawn_availability_received() {
		view_.vehicle_spawn_availability().received = false;
	}
	// The vehicle screen's state, the process's (ClientProcessState::vehicle_menu)
	// as retail's scene-lifetime globals are; its poll runs once per main frame
	// of the game loop, after the client net frame (the roles call
	// poll_vehicle_menu there).
	VehicleMenu &vehicle_menu() { return process_state().vehicle_menu; }
	// The process's loadout name list (g_LoadoutWeaponNameList, D-NET-143): a JoinerRole binds
	// its worlds to it and rebuilds it at each S2C 0x5A apply.
	world::LoadoutWeaponNameList &loadout_weapon_names() {
		return process_state().loadout_weapon_names;
	}
	const VehicleMenu &vehicle_menu() const { return process_state().vehicle_menu; }
	void poll_vehicle_menu(const world::World &world, const def::DefItemsFile *items);
	// The main loop's measured frame rate for the quality metric's
	// frame-pressure term [orig: g_StatsAvgFps (dword_24E1F10), read by
	// CNetQuality_UpdateMetrics @0x4C5643]: inmatch::Session hands over its
	// FR counter once per banked frame (Role::observe_frame_rate). 0 is
	// retail's mode-init value (the ceiling metric 255) until the first 2 s
	// window closes; any rate at or above 16 scores the floor. Joiner only.
	void set_observed_frame_rate(int32_t fps) { observed_frame_rate_ = fps; }
	// The client (RECEIVE) window the 62-frame fold samples into.
	const replication::NetQualityWindow &client_quality_window() const {
		return client_quality_window_;
	}
	// The local player's Position (fixed 16.16), the client handlers' read of
	// g_LocalPlayerEntity->Position: each role stamps its world's local player
	// ahead of the client frame (the joiner's L, the listen host's own body);
	// with none the last stamp stands. The receive's own Position writes move it in
	// wire order (a self record's respawn arm, a 0x0F), and the 0x53 adoption distance
	// reads it at the 0x53's place in the pump (D-NET-687)
	// [orig: NapiNPClientMsg_ZoneTimerWindow @0x428C19..0x428CB1].
	void set_local_player_position(const world::World &world);
	// The bucketed 0..4 quality level the C2S 0x4C report carries and the
	// client's own ping readings (0 before the first completed round trip).
	// The level is g_NetQuality's first dword [orig: `mov eax, g_NetQuality`
	// @0x42c256], the connection indicators' own.
	uint8_t net_quality_level() const { return static_cast<uint8_t>(net_indicators_.level); }
	// THE CONNECTION INDICATORS (hud/net_quality_indicators.h): the g_NetQuality
	// display state this client keeps, stepped once per client frame while in
	// a session, read by the HUD role facts.
	const hud::NetQualityIndicators &net_quality_indicators() const { return net_indicators_; }
	// The authority's level: the host's server tick samples its send window
	// and the host role stores the bucketed level here ahead of this client
	// frame [orig: CNetQuality_UpdateMetrics @0x4c52c0 -> CNetQuality_SetLevel
	// (&g_NetQuality, level) @0x52659b, both before CNetQuality_UpdateIndicators
	// @0x52668d]. The joiner's own fold stores its level itself.
	void set_net_quality_level(int32_t level);
	// The host protocol's link-error callbacks of one server tick, bit 0 a
	// joiner's resend request (flag 1), bit 1 our own missing-sequence
	// request (flag 2) [orig: NapiNP_HandleResendList cb_server_6 = NapiNPServer_OnResendListCallback
	// @0x4c62a0, its call @0x623a0e; SendMissingSeqList cb_server_5 = @0x4C4680 @0x62379b ->
	// the g_NetQuality flag stores]. The host role hands them over after
	// this client frame, as retail's server tick runs after the indicators'
	// update.
	void raise_net_quality_link_errors(uint32_t mask);
	// The NovaWorld link (hud::NovaWorldLinkFacts): the network type, the NWU
	// session in use, its state flags and its hosting/playing word, as the
	// shell's NovaWorld session reports them each tick. The N icon reads the
	// first three, the joiner's 62-frame block the exit below.
	void set_novaworld_link(const hud::NovaWorldLinkFacts &facts) { novaworld_link_ = facts; }
	const hud::NovaWorldLinkFacts &novaworld_link() const { return novaworld_link_; }
	// g_MissionExitReason as the client's legs store it: the joiner's main-frame
	// NovaWorld check, the NWU session's own stop-playing / punt handlers
	// (inmatch/novaworld_link.h), else what the in-match connection's latched
	// disconnect record maps to (inmatch/mission_exit.h); nonzero exits the
	// mission, and the post-mission router reads it.
	void set_mission_exit_reason(int32_t reason) { mission_exit_reason_ = reason; }
	int32_t mission_exit_reason() const;
	uint32_t client_ping_ms() const { return joiner_ ? joiner_->client_ping_ms() : 0; }
	uint32_t client_average_ping_ms() const {
		return joiner_ ? joiner_->client_average_ping_ms() : 0;
	}
	uint32_t session_ping_ms() const { return joiner_ ? joiner_->session_ping_ms() : 0; }
	// Action 6 on a designated-G mounted EWeap selects the child's embedded
	// MountSlot or its groundEntity vehicle slot. Authority confirms via the
	// ordinary compact player echo; this only queues the reliable C2S 0x16.
	bool queue_mounted_weapon_slot_selection(bool use_parent_slot);
	bool queue_vehicle_attach(uint16_t vehicle_handle, uint8_t model_bone_index);
	bool queue_vehicle_detach(uint16_t vehicle_handle);
	// Entity_SendDetachPacket itself: the reliable C2S 0x27 [self][target][2 B],
	// gated only on the target (the plyr class callback's seat leave; the mount
	// toggle's caller adds its own gates in queue_vehicle_detach).
	// [orig: Entity_SendDetachPacket @0x435510]
	bool queue_detach_packet(uint16_t target_handle);
	std::vector<replication::ClientRoundEvent> drain_round_events();
	// S2C 0x1E game events the recv fold surfaced this frame — the kill /
	// objective / medic feed lane. The embedder formats each into a canned
	// sentence against its roster + string table [orig: 0x426270 -> 0x422DA0].
	std::vector<replication::ClientGameEvent> drain_game_events();
	std::vector<WeaponReload> drain_reload_notifications();
	// S2C 0x35 powerup weapon grants folded this frame (the joiner role lands
	// and mounts the one naming its own player).
	std::vector<WeaponPickupNotice> drain_weapon_pickups() {
		return view_.drain_weapon_pickups();
	}
	// Consume sounds, explosions, and class death/state callbacks in receive
	// order. Pure joiners apply death callbacks to the world twin; listen
	// clients retain the authority's already-applied gameplay state.
	// [orig: NapiNPClientMsg_EntityDeath @0x42EB50 — cb(entity, 4, 0) @0x42ebf5]
	void apply_received_effects(world::World &world);

	// THE COMMAND MAP'S SQUAD AND WAYPOINT SENDS (client_squad.cpp): each
	// queues its one reliable C2S (a joiner on its held one-shot queue, the
	// listen host's client on its loopback to its own server); false when no
	// session carries it. [orig: NetPacket_SendUserWaypointCreate @0x42ddc0 (0x17),
	//  NetPacket_SendUserWaypointRemove @0x42de00 (0x4F), NetPacket_SendWeaponSlotSwitch
	//  @0x42dc10 (0x43), NetPacket_SendCommandType44 @0x42dc70 (0x44),
	//  NetPacket_SendWeaponAction @0x42dcc0 (0x45), NetPacket_SendTeamChange
	//  @0x42dd00 (0x46), NetPacket_SendVoteKick @0x42dd50 (0x4B), the punt vote
	//  @0x5488ae (0x3F) — every IDB name a misnomer; all
	//  CNapiNetwork_QueueReliableMessage(tag, 1, 0)]
	bool queue_squad_message(uint8_t c2s_tag, std::vector<uint8_t> body);
	// The local player's roster slot (entity+0x154; the pipeline's
	// set_local_player_slot).
	int local_roster_slot() const { return view_.local_roster_slot(); }
	// The squad folds' HUD lines (hud/squad_feed.h) since the last drain.
	std::vector<hud::SquadFeedLine> drain_squad_lines();
	// Ring lines the client's own receive legs raise with their own sink and
	// colour (a dialog line's "EX Cannot load audio" and subtitle), drained
	// with the other feed lanes once per frame.
	std::vector<hud::FeedPost> drain_ring_posts();
	// The authority's own playback of a dialog line its queue loads (the listen host's or single
	// player's client): with no wave of the line's name in the dialog bank's sounds, "EX Cannot
	// load audio: <name>" on the SYSTEM ring in 0xFFFF2222; then the line's subtitle on the CHAT
	// ring in white, both 930 ticks, as the joiner's leg posts them.
	// [orig: Dialog_LoadAudioClip @0x44DCC0 - the missing wave @0x44DD5D..0x44DD74
	//  (Chat_AddMessageChannel2(msg, -56798, 930)), the subtitle @0x44DDCD..0x44DE3C and its
	//  hook @0x44DEC9 (Chat_AddSystemMessageIfValid)]
	void post_authority_dialog_line(const world::World &world, const audio::DialogLineRef &line);
	// THE CMAP SCREEN'S OWN LEGS (client_squad.cpp): the world halves
	// (world/user_waypoints.h) plus their sends — the placed waypoint's C2S
	// 0x17 (target 0xFF) [orig: @0x54a1be], each removed one's C2S 0x4F
	// [orig: @0x5479e6; @0x548137]. A go-code button: C2S 0x4B [local
	// slot][code], then the leader's own sound set and line with no mute
	// [orig: CMap_OnGoCodeButton @0x548290].
	bool place_user_waypoint(world::World &world, int32_t x, int32_t y, const std::string &name);
	bool delete_hovered_user_waypoint(world::World &world);
	void clear_user_waypoints(world::World &world);
	void send_go_code(world::World &world, uint8_t code);
	// THE CMAP TABLES' SENDS (menu/command_map_screen.h asks for them): the
	// recruit [local slot][target] (C2S 0x46), the join [leader] (0x43), the
	// fireteam assignment [fireteam][count][members] (0x45), the order
	// [kind][count][text][targets] (0x44) and the punt vote [target] (0x3F).
	// [orig: CMap_EntityWidgetHandler @0x54865c / @0x548616;
	//  CCommandMap_SendWeaponActionToTeammates @0x548d67;
	//  CMap_BuildAndSendOrderCommand @0x547839; CCommandMap_HandleOrderAction
	//  @0x548c25; CMap_HandlePlayerListCallback @0x54889b..0x5488b9]
	void send_squad_recruit(uint8_t target);
	void send_squad_join(uint8_t leader);
	void send_fireteam_assign(uint8_t fireteam, const std::vector<uint8_t> &members);
	void send_squad_order(uint8_t kind, const std::string &text,
			const std::vector<uint8_t> &targets);
	void send_punt_vote(uint8_t target);
	void tick_remote_stance_sounds(world::World &world);
	// S2C 0x23 WAC remote commands the recv fold surfaced this frame; the
	// joiner role runs each registry row's handler against its world.
	std::vector<ScriptRemoteCommand> drain_script_remote_commands();
	// S2C 0x3F HUD relays the recv fold surfaced this frame.
	std::vector<ObjectiveNotification> drain_objective_notifications();

	// Deterministic golden replay (Joiner): seed the connection keys + seq/ack + self handle/type so
	// frame_c2s_uplink reproduces a captured C2S 0x0C datagram byte-for-byte.
	// `tick_seed` is the capture's live network-role tick (the value its S2C 0x61 seeded);
	// without it a replayed client's clock stays parked at zero and stamps 0 into every 0x06.
	// `self_team` is the captured self record's team byte, the team L spawns with.
	void seed_session(uint32_t session_id, uint32_t client_key, std::string client_scrk,
	                  std::string server_scrk, uint32_t next_seq, uint32_t last_ack,
	                  uint16_t self_handle, uint16_t self_type, uint32_t game_type = 0,
	                  uint32_t tick_seed = 0, bool replay_mode = true, uint8_t self_team = 0);

	// The shell's kit for the mission start's loadout-submission pair: a joiner's
	// (JoinerConnection::set_loadout_kit, released by the S2C 0x1A), or the listen
	// host's own client's, whose pair HostRole queues at its mission start's end
	// (D-NET-695) [orig: Game_StartMission @0x525836 / @0x525c2e, the session peer's
	// gates @0x525767..0x525779 / @0x525BC4..0x525BD2].
	void set_loadout_kit(JoinerConnection::LoadoutKit kit) {
		if (joiner_) {
			joiner_->set_loadout_kit(std::move(kit));
			return;
		}
		host_loadout_kit_ = std::move(kit);
		host_loadout_kit_set_ = true;
	}
	// The listen host's own kit, null until the shell set one.
	const JoinerConnection::LoadoutKit *host_loadout_kit() const {
		return host_loadout_kit_set_ ? &host_loadout_kit_ : nullptr;
	}
	// The session start's per-side class and avatar words (g_CharClassTeam1/2,
	// g_AvatarTeam1/2) a joiner holds; null on the host's own view.
	const CharacterJoinVars *character_join_vars() const {
		return joiner_ ? &joiner_->character_join_vars() : nullptr;
	}
	void set_character_join_vars(CharacterJoinVars vars) {
		if (joiner_) joiner_->set_character_join_vars(vars);
	}
	void set_auto_medic_preference(uint32_t disabled) {
		if (joiner_) joiner_->set_auto_medic_preference(disabled);
	}
	void set_join_request(JoinRole role, std::string spectator_password,
			std::string server_password, std::string join_password = {}) {
		if (joiner_) {
			joiner_->set_join_request(role, std::move(spectator_password),
					std::move(server_password), std::move(join_password));
		}
	}
	bool is_spectator() const {
		return joiner_ != nullptr && joiner_->spectator();
	}
	// The install root the JOIN VERSIONCRCSTRING checksum reads its loose
	// expansion/<name>/version.txt from (D-NET-166; see
	// JoinerConnection::set_expansion_version_root). Joiner only.
	void set_expansion_version_root(std::string game_root) {
		if (joiner_) joiner_->set_expansion_version_root(std::move(game_root));
	}
	// The .joi-recovered game-session APPID join token (JoinerConnection::set_app_id).
	// Joiner only; "0" is the LAN default.
	void set_app_id(std::string token) {
		if (joiner_) joiner_->set_app_id(std::move(token));
	}
	// The browse row's 0x81 record (JoinerConnection::set_discovered_session).
	void set_discovered_session(JoinerConnection::DiscoveredSession session) {
		if (joiner_) joiner_->set_discovered_session(std::move(session));
	}
	// The joiner connection's index, the CI its 0x41 / 0x42 carry (0 without a joiner).
	uint32_t joiner_client_index() const { return joiner_ ? joiner_->client_index() : 0u; }
	// The dialed host (JoinerConnection::set_host_address). Joiner only.
	void set_host_address(const PeerAddr &host) {
		if (joiner_) joiner_->set_host_address(host);
	}
	// The title identity the joiner speaks (JoinerConnection::set_title), and the
	// one its view folds the in-match bodies by (ClientReplicaPipeline::set_title).
	void set_title(const TitleIdentity *title) {
		view_.set_title(title);
		if (joiner_) joiner_->set_title(title);
	}
	// The CD identity cookie (packed PUB* blob) for the 0x00 JOIN. Joiner only.
	void set_join_cd_cookie(std::vector<uint8_t> cookie) {
		if (joiner_) joiner_->set_cd_cookie(std::move(cookie));
	}
	// The CD-key ids a LAN 0x00 JOIN leads with (JoinerConnection::set_cd_key_ids). Joiner only.
	void set_cd_key_ids(cdkey::WireKeys ids) {
		if (joiner_) joiner_->set_cd_key_ids(std::move(ids));
	}
	// The gate's GLSVSSREQUEST a NovaWorld 0x00 JOIN carries
	// (JoinerConnection::set_glsvss_request). Joiner only.
	void set_glsvss_request(std::string request) {
		if (joiner_) joiner_->set_glsvss_request(std::move(request));
	}
	void set_charattr_table(CharAttrTable table) {
		if (joiner_) joiner_->set_charattr_table(std::move(table));
	}
	void clear_charattr_table() {
		if (joiner_) joiner_->clear_charattr_table();
	}
	// The joiner's live charattr table (null without a joiner: a HostClient
	// reads its embedder's boot copy, which its own restriction step alone
	// mutates).
	const CharAttrTable *charattr_table() const {
		return joiner_ ? &joiner_->charattr_table() : nullptr;
	}
	// The joiner's anti-cheat challenge sources (JoinerConnection::
	// set_integrity_sources); a HostClient has no joiner and answers none.
	void set_integrity_sources(IntegritySources sources) {
		if (joiner_) joiner_->set_integrity_sources(std::move(sources));
	}
	void set_loaded_model_challenge_snapshot(std::vector<uint32_t> values) {
		if (joiner_)
			joiner_->set_loaded_model_challenge_snapshot(std::move(values));
	}
	// The mission start's end (JoinerConnection::set_mission_start_loading): an embedder
	// whose load runs after the S2C 0x1A declares it, and once its load has ended calls
	// push_mission_start_end for the datagrams to send. While the released mission start
	// loads, the frame neither receives nor sends: retail is inside Game_StartMission's
	// load, which pumps only at its own points. Without the declaration the frame that
	// received the 0x1A pushes at once.
	void set_mission_start_loading(bool loading) {
		if (joiner_) joiner_->set_mission_start_loading(loading);
	}
	bool mission_start_released() const { return joiner_ && joiner_->mission_start_released(); }
	// Game_StartMission's network pumps from the 0x1A to the final wait for the S2C 0x0F,
	// in its order: its receive pumps step the send-holdoff countdown, its send pumps build
	// only at a spent countdown and reload it, so the first 0x2F, the second and the 0x0B
	// leave in one packet or two by where the countdown stood; what it still holds at the
	// last send leaves from the next frame's final wait, after that frame's receive
	// (D-NET-NEW-L2-6).
	std::vector<std::vector<uint8_t>> push_mission_start_end();
	void set_weapon_table(const world::WeaponTable *weapons) {
		if (joiner_) joiner_->set_weapon_table(weapons);
	}
	// Queue the armory ACCEPT's C2S 0x2F (inmatch::armory_accept_in_session builds it), as
	// NetPacket_SendLoadoutSubmit queues it at the ACCEPT, so it leaves at the next open
	// boundary in queue order [orig: @0x42d085]: a joiner's through its connection's stage
	// gate (JoinerConnection::prepare_loadout_submit), a listen host's own client's on its
	// local connection. False when nothing was queued.
	bool queue_loadout_submit(const LoadoutSubmit &submit);

	bool deployment_pick_pending() const {
		return joiner_ && joiner_->deployment_pick_pending();
	}
	bool initial_admission_complete() const {
		return joiner_ && joiner_->initial_admission_complete();
	}
	// Whether the joiner's mission start owes its input reset (the world
	// stream's S2C 0x1A, JoinerConnection::PollResult::game_start_acked);
	// reading clears it. The embedder runs the reset over its devices.
	bool take_input_reset() {
		const bool due = input_reset_pending_;
		input_reset_pending_ = false;
		return due;
	}
	// The joiner's server-assigned team (the S2C 0x04 latch) — the deploy screen's
	// row filter and color source [orig: g_LocalTeam].
	uint8_t assigned_team() const { return joiner_ ? joiner_->assigned_team() : 0; }
	// Host policy learned from S2C 0x76. HostClient/offline views use the stock
	// all-ten-classes default; Simulation exposes its configured host value.
	uint16_t class_allow_mask() const {
		return joiner_ ? joiner_->class_allow_mask() : 0x03FFu;
	}
	// This client's own roster slot id (S2C 0x04 byte 17). Bits 9.. of every fired
	// round's hit_part word [orig: @0x50bda5]; 0 attributes our shots to the host.
	uint8_t local_player_slot() const { return joiner_ ? joiner_->local_player_slot() : 0; }
	// Monotonic edge counter for the S2C 0x50 re-latch of OUR OWN team. The
	// simulation re-styles friend/foe when it advances (the 0x04 latch at join is
	// consumed through the spawn instead). [orig: g_LocalTeam store @0x4319db]
	uint64_t self_team_revision() const { return self_team_revision_; }
	// The team the last self S2C 0x50 stored on the joiner's own entity (its leg 2,
	// whether or not that entity is the local one yet) [orig: @0x4319EE].
	uint8_t self_assigned_team() const { return self_assigned_team_; }
	// Bumps on every S2C 0x50 whose local arm ran (the per-side reselect and the 0x2F)
	// [orig: NapiNPClientMsg_TeamAssign @0x431A1B..0x431A9E].
	uint64_t local_resubmit_revision() const { return local_resubmit_revision_; }
	// Roster slots whose bookkeeping the host cleared via S2C 0x46 bit15. Draining
	// keeps it a one-shot; the associated ENTITY row is deliberately untouched
	// (only S2C 0x5D retires an entity). [orig: @0x431411..0x43144c]
	std::vector<uint8_t> drain_cleared_player_slots() {
		std::vector<uint8_t> out;
		out.swap(cleared_player_slots_);
		return out;
	}

	// Session loss (Joiner only): empty while healthy, else a player-facing reason.
	// Either the host explicitly closed the session (its connection-description punt,
	// terminal at any stage) or nothing arrived for the reap window while in-match.
	// Retail exits the mission with a reason here; there is no in-world dialog.
	// [orig: CNapiNPConnection_HandleDescriptionPacket @0x621ae0; the
	//  cs_dir0.timeout_ms = 120000 reap @0x4ca4a0 ->
	//  CNapiNetwork_OnDisconnectedFromServer @0x4c63d0]
	std::string session_loss_reason() const {
		return joiner_ ? joiner_->session_loss_reason() : std::string();
	}
	bool session_lost() const { return joiner_ && joiner_->session_lost(); }
	// The session clock of the last S2C 0x58 (g_SessionStatus): 0 while none is held (the
	// mission start's reset), else its uptime plus the GetTickCount time since its receipt,
	// in u32 arithmetic. The status report's UP-TIME on a joiner, and its CMAP RULES text.
	// [orig: CSessionTimer_GetElapsedMS @0x52D5F0 (`+0x70 - +0x74 + GetTickCount()`
	//  @0x52D609); SessionStatus_ParseFromBuffer @0x530ED0 (the receipt stamp +0x74
	//  @0x53109F..0x5310A7)]
	uint32_t session_elapsed_ms() const;
	// Accept the player's C2S 0x0E pick (0xFFFF default, 0xFFFE auto, else a
	// spawn-target handle). Input case 12 closes retail's gameplay dword and queues
	// the 0x0E now [orig: @0x49b17b]; framing waits for the next open send boundary,
	// in queue order. Re-picks while awaiting the release are allowed. False means
	// the deployment UI/state cannot accept a pick. The listen host's own client
	// sends the same request over its loopback to its own server.
	bool queue_deployment_pick(uint16_t wire_value);
	// The deploy action's convert arm (Role::request_deployment_pick): one
	// reliable C2S 0x51 [i16 0xFFFF], the stock client's (no target), closing
	// the gameplay dword as the pick does. The listen host's own client sends
	// it over its loopback to its own server. False out of a session.
	bool queue_spectator_convert();

	bool mission_known() const { return joiner_ && joiner_->mission_known(); }
	bool has_mission_header() const {
		return joiner_ && joiner_->has_mission_header();
	}
	const std::vector<uint8_t> &mission_header_bytes() const {
		static const std::vector<uint8_t> empty;
		return joiner_ ? joiner_->mission_header_bytes() : empty;
	}
	bool has_terrain_til() const {
		return joiner_ && joiner_->has_terrain_til();
	}
	TerrainTilState terrain_til_state() const {
		return joiner_ ? joiner_->terrain_til_state() : TerrainTilState::Absent;
	}
	const std::vector<uint8_t> &terrain_til_bytes() const {
		static const std::vector<uint8_t> empty;
		return joiner_ ? joiner_->terrain_til_bytes() : empty;
	}
	// The terminal pre-world sync marker was received and answered by the C2S 0x0A
	// (JoinerConnection::preload_ready).
	bool preload_ready() const { return joiner_ && joiner_->preload_ready(); }
	// Admission-stage name for diagnostics (the shell's post-load join watchdog).
	const char *admission_stage_name() const {
		return joiner_ ? joiner_->post_auth_stage_name() : "no session";
	}

	// Joiner state passthrough (HostClient: never InMatch, no self handle).
	bool in_match() const { return joiner_ && joiner_->in_match(); }
	bool in_session() const { return joiner_ && joiner_->in_session(); }
	// The game loop runs (JoinerConnection::in_game_loop): alive, dead or redeploying.
	// It is also the joiner's gate for every input-driven sender: the input frame,
	// the HUD and the in-game screens that call them run in the main frame alone,
	// which neither the join screen, the mission start's load nor its final wait
	// runs (that wait reads the key queue against ESC only), so nothing input-driven
	// is queued before the 0x0F ends the wait. A joiner's session-gated senders
	// (the medic call, the Respawn key, the deploy pick and convert, the voice
	// picks, the team change, the vehicle spawn screen) test this, not in_session.
	// [orig: Game_ProcessMainFrame @0x5263F0 - Input_ProcessFrame @0x52661D;
	//  Game_StartMission @0x524360 - the final wait @0x526295..0x5262E5, its ESC
	//  test @0x5262A2..0x5262AD]
	bool in_game_loop() const { return joiner_ && joiner_->in_game_loop(); }
	// The final wait's ESC: it leaves the wait for the game loop and does nothing else
	// (JoinerConnection::cancel_final_wait carries the witness); false outside the wait.
	bool cancel_final_wait() { return joiner_ != nullptr && joiner_->cancel_final_wait(); }
	bool in_mission_start() const { return joiner_ && joiner_->in_mission_start(); }
	bool in_pre_game() const { return joiner_ && joiner_->in_pre_game(); }
	bool mission_start_aborted() const { return joiner_ && joiner_->mission_start_aborted(); }
	uint32_t mp_attributes_revision() const {
		return joiner_ ? joiner_->mp_attributes_revision() : 0u;
	}
	bool awaiting_deploy_pick() const { return joiner_ && joiner_->awaiting_deploy_pick(); }
	bool has_self_handle() const { return joiner_ && joiner_->has_self_handle(); }
	uint16_t self_handle() const { return joiner_ ? joiner_->self_handle() : 0; }
	// The joiner's read of its own dead bit, the local entity's Flags & 2 the input
	// dispatcher's head reads (and nothing else): live from the mission start's
	// Player_InitPlayer, latched by the body's death edge from a Health of 0 or less
	// (the recipient-local 0x0A tail, or a dead self record's zero) on a frame whose
	// entity update runs it (not in the pre-round, not under the session's round-over
	// hold, not under the local death screen, whose body tick returns at its head), and
	// cleared by a self record's respawn arm (the record alive while the bit is set).
	// So before the first tail the joiner reads alive (D-NET-562).
	// [orig: Input_HandleActionBinding @0x49AD40 - a6 @0x49AD76..0x49AD7E;
	//  Entity_UpdateInfantryPlayerBody @0x4B40E0 - the death-screen return
	//  @0x4B40F8..0x4B411A, the death edge @0x4B4BF1..0x4B4C02; Game_ProcessMainFrame
	//  @0x5263F0 - the entity update's skip @0x526703..0x526742;
	//  NetPacket_SerializePlayerState @0x4C09C0 - the dead record's Health zero
	//  @0x4C10F1..0x4C10FB, the respawn arm @0x4C1109]
	bool local_player_dead() const { return local_dead_latch_; }
	// The local entity's Health word as this frame's receive left it: each 0x0A's
	// recipient tail stores it, then the record naming the local entity zeroes it when
	// dead or, alive, raises a Health of 0 or less to 1 (the receive loop carries the
	// witness). The dead latch and L's stored Health read it.
	int16_t local_entity_health() const { return local_entity_health_; }
	// The joiner's local player entity exists: a mission start's Player_InitPlayer
	// bound it (JoinerConnection::local_player_entity), L spawned or not.
	bool local_entity_bound() const {
		return joiner_ && joiner_->local_player_entity() != 0xFFFFu;
	}
	JoinerConnection::Phase phase() const {
		return joiner_ ? joiner_->phase() : JoinerConnection::Phase::Idle;
	}
	// The host-advertised self-spawn pose learned at the owner-ID match — full precision (not the lossy
	// ClientState position), so the binding spawns its local player L from it. Joiner-only; valid once
	// in_match() (the caller gates on that). Mirrors the self_handle() passthrough.
	const JoinerConnection::SelfSpawn &spawn_pose() const { return joiner_->spawn_pose(); }
	// One g_GameType: every forwarded 0x08 / 0x7B lands in the view; the joiner's
	// own fold covers only the handshake window before the view has seen one.
	uint32_t game_type() const {
		return view_.game_type_known() || joiner_ == nullptr ? view_.game_type() : joiner_->game_type();
	}
	// The joined session's published player cap (the S2C 0x64 block's dword at
	// offset 36; 0 until that transfer completes). Joiner-only: the embedder's
	// own full-BMS load admits against it.
	uint32_t session_max_players() const { return joiner_ ? joiner_->session_max_players() : 0; }
	const std::string &server_name() const;
	const std::string &mission_name() const;
	// The joined session's variable list off the S2C 0x60 server-info transfer
	// (JoinerConnection::session_vars); empty on the host's own view, whose
	// copies come from its own serializer (role_feeds.h
	// scoreboard_session_vars).
	const SessionVars &session_vars() const;
	const std::string &map_file() const;
	const std::string &expansion() const;
	const std::string &last_error() const;

	// The client's ONE tracked timed-capture window — the nearest-zone
	// cluster dword_A85B88..A85BA0 the 0x53 handler seeds and the 0x6C handler
	// re-rates. NO presentation consumer reads progress/target/limit in retail
	// (an exhaustive immediate scan over 0x400000..0x7A0000 finds only the two
	// handlers and the frame pump); the capture bar reads the 0x0A phase-0
	// word_A85B7C instead. Kept as the exact retail image (net-re 0x6C).
	// [orig: NapiNPClientMsg_ZoneTimerWindow @0x428ae0 — same entity + same
	//  modeB keeps progress, a different modeB re-seeds it @0x428d09..0x428d0c;
	//  a different entity is adopted only when at least as near as the current
	//  one and within 0x140000 = 20.0 u @0x428cf5; then rate = byte, entity,
	//  modeA, modeB, target = 62*start, limit = 62*end, and start >= end zeroes
	//  the whole cluster but progress @0x428d40..0x428d60;
	//  NapiNPClientMsg_0x06C @0x428fc0 — rate = byte only when the handle is
	//  the tracked entity @0x42902d..0x429038;
	//  Client_ProcessNetworkFrame @0x42c2eb..0x42c347 — while limit != 0:
	//  target == limit resets (entity/target/limit/progress = 0, rate = 1),
	//  else progress += rate with the clamp progress <= target when rate > 0
	//  and progress >= 0 when rate < 0 — so a positive 1..32 count never moves
	//  a progress the 0x53 already parked at target]
	struct TrackedCaptureWindow {
		uint16_t zone = 0xFFFF; // dword_A85B88 (entity pointer; 0 = none)
		int32_t mode_a = 0;     // dword_A85B8C
		int32_t mode_b = 0;     // dword_A85B90
		int32_t target = 0;     // dword_A85B94 (62 * start_s)
		int32_t limit = 0;      // dword_A85B98 (62 * end_s)
		int32_t progress = 0;   // dword_A85B9C
		int32_t rate = 0;       // dword_A85BA0
		bool tracked() const { return zone != 0xFFFF; }
	};
	const TrackedCaptureWindow &tracked_capture_window() const { return tracked_window_; }

	struct ZoneState {
		bool has_value = false;
		ZoneTimerValue value;
		bool has_window = false;
		ZoneTimerWindow window;
		bool has_presence = false;
		uint8_t presence_count = 0;

		// Exact semantic image of the retail 13-DWORD shared timer-list entry
		// (the map key supplies DWORD 0). The raw latest records above remain for
		// existing UI callers; these fields are the live, per-frame state.
		struct Entry {
			int32_t mode_a = 0;              // DWORD 1
			int32_t mode_b = 0;              // DWORD 2
			int32_t window_current = 0;      // DWORD 3
			int32_t window_target = 0;       // DWORD 4
			int32_t window_limit = 0;        // DWORD 5
			int32_t window_rate = 0;         // DWORD 6
			bool window_active = false;      // DWORD 7
			int32_t value_current = 0;       // DWORD 8
			int32_t value_target = 0;        // DWORD 9
			int32_t value_limit = 0;         // DWORD 10
			int32_t value_rate = 0;          // DWORD 11
			bool value_active = false;       // DWORD 12
			// The two in-radius contest counts the 0x6F value carries, stored
			// on the zone ENTITY by the retail handler (+0x220 the owning
			// side's, +0x221 the other side's) and read by the AAS status
			// panel's marker [orig: NapiNPClientMsg_ZoneTimerValue stores
			//  @0x428e79/@0x428e7f; HUD_DrawZoneMarker reads @0x598866/@0x599009].
			uint8_t contest_owner = 0;
			uint8_t contest_other = 0;
		} entry;
	};
	const std::unordered_map<uint16_t, ZoneState> &zone_states() const {
		return zone_states_;
	}
	// Generic-world control-register consumer. False means retail has no entry
	// and therefore performs no LFP_CAMPPERCENT write. A zero limit is the
	// witnessed full-scale special case.
	bool lfp_cam_percent(uint16_t zone_handle, int32_t &out) const;
	uint64_t authoritative_loadout_revision() const {
		return authoritative_loadout_revision_;
	}
	const WeaponLoadout &authoritative_loadout() const {
		return authoritative_loadout_;
	}
	// The last S2C 0x0F ammo-pool image (the authority's serverPlayer+88664 copy
	// retail lands in g_LocalAmmoPools) and its monotonic revision; the embedder
	// applies it to the local inventory after the 0x5A slot rebuild.
	uint64_t authoritative_ammo_pools_revision() const {
		return authoritative_ammo_pools_revision_;
	}
	const std::array<int32_t, kWorldStateAmmoPoolCount> &authoritative_ammo_pools() const {
		return authoritative_ammo_pools_;
	}
	uint32_t send_holdoff_countdown() const { return send_holdoff_countdown_; }
	// The network-role clock [orig: g_ClientCurrentTick @0xA8229C]: 0 until a
	// tick seed lands, then one tick per client frame.
	uint32_t current_tick() const { return current_tick_; }
	uint32_t send_holdoff_ticks() const { return send_holdoff_ticks_; }
	// Whether the next Client_ProcessNetworkFrame opens its send block: the
	// receive pump decrements a nonzero countdown before the gate tests it for
	// zero, so the block opens when the countdown reads 0 or 1 here. The local
	// input pack keys on it, since retail packs only inside that block.
	// [orig: PumpFlags 0x10 decrement @0x62979a..0x6297ac via
	//  PumpClientProtocolRecv @0x42c228; the gate `cmp [conn+648h], 0; ja`
	//  @0x42c3dd..0x42c3e3 ahead of Player_PackInputStateToEntity @0x42c3e9]
	bool send_block_opens_this_frame() const { return send_holdoff_countdown_ <= 1; }

	const replication::ClientState &state() const { return view_.state(); }
	replication::ClientState &state() { return view_.state(); }
	// Called by the joiner simulation after decoded round events have stamped
	// remote recoil, preserving retail's receive -> body-decay frame order.
	void tick_remote_recoil() { view_.tick_recoil(); }
	// Called by an embedding simulation after its world-side vehicle movers have
	// mirrored their final poses into the decoded rows. The library-only remote
	// mover already runs this phase internally; the explicit seam prevents
	// riders/attachments from observing the previous world-mover tick.
	void refresh_remote_attachments() { view_.refresh_carried_entities(); }
	replication::ClientReplicaPipeline &view() { return view_; }
	const replication::ClientReplicaPipeline &view() const { return view_; }
	std::size_t unknown_tags() const { return view_.unknown_tags(); }

private:
	// Shared body for the Client_ProcessNetworkFrame overloads. `build_uplink` is nullptr for a
	// no-uplink frame.
	std::vector<std::vector<uint8_t>> run_frame(
			const UplinkBuilder *build_uplink, uint32_t now_tick);
	// A joiner's datagrams as they leave, their connect-log lines written (NET SENDING each,
	// then NET DISCONNECTING when the call ended the session that was up at its start).
	std::vector<std::vector<uint8_t>> log_client_sends(
			std::vector<std::vector<uint8_t>> datagrams, bool session_lost_before) const;
	devtools::TickProfile *profile_ = nullptr;
	void stage_reload_notifications_before_body_tick();
	bool apply_zone_timer_body(uint8_t tag, const std::vector<uint8_t> &body);
	void apply_zone_timer_value(const ZoneTimerValue &value);
	void apply_zone_timer_window(const ZoneTimerWindow &window);
	void apply_zone_presence_count(const ZonePresenceCount &presence);
	void advance_zone_timers();
	// The tracked-window cluster's 0x53 adoption, 0x6C re-rate, and per-frame
	// advance (TrackedCaptureWindow above).
	void adopt_tracked_window(const ZoneTimerWindow &window);
	void advance_tracked_window();
	TrackedCaptureWindow tracked_window_;
	// g_LocalPlayerEntity's Position as the receive's handlers read it: stamped from
	// L at the frame head (set_local_player_position), then moved by the receive's
	// own writes (the self record's respawn arm, the 0x0F) once the mission start
	// bound the entity (local_entity_bound).
	int32_t local_player_position_[3] = {0, 0, 0};
	// local_player_dead's latch (the local entity's Flags & 2).
	bool local_dead_latch_ = false;
	// The local entity's Health word as the receive leaves it (local_entity_health).
	int16_t local_entity_health_ = 0;
	ConnectLog *connect_log_ = nullptr; // set_connect_log
	// The body tick's death edge over this frame's fold (local_player_dead).
	void step_local_dead_latch();
	// The client-side 1 Hz revive countdown over the roster: every 63rd frame
	// each active slot with an entity and a nonzero revive window loses one
	// second [orig: Client_ProcessNetworkFrame @0x42C27E..0x42C2DA —
	// g_SlotRefreshTimer > 62 -> PlayerSlot_SetDownedState(slot+0x10 - 1,
	// slot+0x2C) per slot, then the timer resets to 0]. The host's own
	// loopback view runs it too (retail's client frame is role-agnostic).
	void tick_roster_revive_countdown();
	// Queues the C2S 0x22 + 0x23 refresh pairs the 0x4D / 0x50 folds produced.
	void drain_visible_refreshes();
	// The once-per-62-frames CNetQuality update + level fold that precedes the
	// client net frame in the main frame [orig: Game_ProcessMainFrame — the
	// g_NetQualityUpdateCountdown countdown (reload 62) gated is_in_session ->
	// CNetQuality_UpdateMetrics @0x4C52C0 + CNetQuality_SetLevel @0x4C3060].
	void update_net_quality();
	void reset_mission_start_quality();
	// The link-error callbacks a mask carries (kNetQualityLinkError*), onto
	// the indicators at this frame's clock.
	void apply_net_quality_link_errors(uint32_t mask);
	// Chat_CheckFloodControl @0x498F60: truncates `text` to 59 characters in
	// place first, then the table walk; true = the line may go out.
	bool chat_flood_control(std::string &text, uint32_t frame);

	Role role_;
	std::unique_ptr<JoinerConnection> joiner_;        // Joiner only
	replication::ClientReplicaPipeline view_;
	replication::ISessionTransport *loopback_ = nullptr;   // HostClient only (non-owning)
	std::deque<std::vector<uint8_t>> recv_fifo_;      // Joiner: framed inbound awaiting the recv pump
	// The handshake/admission receive handlers allocate exact wire packets immediately to keep
	// their retail grouping. They still belong to PumpClientProtocolSend, so hold the already-framed
	// datagrams behind the same field-3 gate and flush them before any later sequence allocated at
	// the open boundary. (A 0x84 reconstruction and a 0x45 pong are not held: retail transmits them
	// from the receive pump, PollResult::immediate_outbound.)
	std::deque<std::vector<uint8_t>> framed_send_queue_;
	// The connection's one C2S queue: every producer (the 0x34 keepalive, the receive handlers'
	// replies, the 0x4C report, the input-side one-shots, the deploy pick, the loadout
	// re-submit, the fire / reload / mount gameplay records) appends at the moment it runs, as
	// each retail producer calls QueueReliableMessage; the next open send boundary frames the
	// whole queue in that order, then the 0x2C and 0x0C it builds itself.
	// [orig: CNapiNetwork_QueueReliableMessage @0x4c4fa0 -> CNapiNPConnection_QueueMessage
	//  @0x628640, one outgoing list per connection]
	std::deque<ProtocolMessage> send_queue_;
	// S2C 0x49 handlers run inside the receive pump. Preserve their decoded
	// notifications for the embedding simulation after applying the remote-Person
	// handler side effect before this frame's body tick.
	std::vector<WeaponReload> pending_reload_notifications_;
	// The squad folds' consequences once applied (client_squad.cpp).
	void apply_squad_event(world::World &world, const replication::ClientSquadEvent &event);
	std::vector<hud::SquadFeedLine> pending_squad_lines_;
	std::vector<hud::FeedPost> pending_ring_posts_;
	std::unordered_map<uint16_t, ZoneState> zone_states_;
	WeaponLoadout authoritative_loadout_;
	uint64_t authoritative_loadout_revision_ = 0;
	// The S2C 0x5A lists the listen host's own client received this frame, in receive
	// order: apply_received_effects rebuilds the process's loadout name list from each
	// (apply_weapon_loadout_name_list), at the fold between two server ticks, as retail's
	// own client applies the list at the next frame's head (D-NET-143).
	// [orig: Game_ProcessMainFrame @0x5263F0 - Client_ProcessNetworkFrame @0x526692 ->
	//  CNapiNetwork_PumpClientProtocolRecv @0x42C228 -> NapiNPClientMsg_HandleWeaponLoadoutSync
	//  @0x4290E0, ahead of Server_TickUpdate @0x5266B6]
	std::vector<WeaponLoadout> pending_own_weapon_loadouts_;
	std::array<int32_t, kWorldStateAmmoPoolCount> authoritative_ammo_pools_{};
	// The HostClient's kit (set_loadout_kit).
	JoinerConnection::LoadoutKit host_loadout_kit_;
	bool host_loadout_kit_set_ = false;
	uint64_t authoritative_ammo_pools_revision_ = 0;
	bool deployed_ = false;
	bool input_reset_pending_ = false;
	uint64_t deployment_release_revision_ = 0;
	bool authoritative_spawn_released_ = false;
	uint64_t authoritative_spawn_release_revision_ = 0;
	uint64_t self_team_revision_ = 0;           // S2C 0x50 self re-latch edges
	uint8_t self_assigned_team_ = 0;            // self_assigned_team
	uint64_t local_resubmit_revision_ = 0;      // local_resubmit_revision
	std::vector<uint8_t> cleared_player_slots_; // S2C 0x46 bit15 roster clears

	// --- §5.44 per-frame housekeeping counters (P6) — mirror the witnessed per-instance globals of
	// [orig: Client_ProcessNetworkFrame @0x42c180]. The 0x34 keepalive / 0x4C net-quality / 0x2C RTT
	// emits and the send-holdoff send-block gate, deferred-and-logged at P5, ported here. ---
	uint32_t current_tick_ = 0;          // [orig: g_ClientCurrentTick @0xA8229C] bumped once per run_frame
	uint32_t last_keepalive_tick_ = 0;   // [orig: g_LastKeepaliveTick @0xA822A0] 0x34 send latch
	// [orig: g_NetQualityReportTimer @0xA85B84] the 0x4C cadence: the embedder's
	// process-lived word (use_process_state), else this runtime's own.
	ClientProcessState own_process_;
	ClientProcessState *external_process_ = nullptr;
	ClientProcessState &process_state() {
		return external_process_ != nullptr ? *external_process_ : own_process_;
	}
	const ClientProcessState &process_state() const {
		return external_process_ != nullptr ? *external_process_ : own_process_;
	}
	// The joiner's input-driven producers queue only in the game loop: a 0x5A that
	// lands inside the mission start leaves the joiner InMatch in the final wait, which
	// resets the key queue and reads ESC alone, so no action queues there.
	// [orig: Game_StartMission @0x524360 - Input_ResetKeyQueue @0x526288, the loop's
	//  Input_DequeueKeyEvent for ESC @0x5262a2..0x5262ad]
	bool joiner_input_frame() const { return joiner_->in_match() && joiner_->in_game_loop(); }
	uint32_t tag2c_send_cooldown_ = 0;   // [orig: g_Tag2CSendCooldown @0xA860D8] set 62 on a 0x2C send
	                                     // and decremented, but never compared in @0x42C180;
	                                     // vestigial/telemetry state, not a send throttle.
	// [orig: g_NetQuality @0x82BF88] the 0..4 level CNetQuality_SetLevel folds
	// every 62 frames (0 = no measurement) and the connection indicators.
	hud::NetQualityIndicators net_indicators_;
	hud::NovaWorldLinkFacts novaworld_link_;
	int32_t mission_exit_reason_ = 0;                    // [orig: g_MissionExitReason]
	// The client (RECEIVE) window of the CNetQuality object and its inputs
	// [orig: CNetQuality_UpdateMetrics @0x4C52C0, the `is_mp_session_peer &&
	//  !is_authority` half]. The host (SEND) window lives with the host's tick.
	replication::NetQualityWindow client_quality_window_;
	int32_t observed_frame_rate_ = 0;                    // [orig: dword_24E1F10, 0 @0x52B727]
	int32_t quality_update_countdown_ = 0;               // [orig: g_NetQualityUpdateCountdown]
	ChatFloodTable chat_flood_{};                        // [orig: @0xB3B788] (chat_flood.h)
	uint32_t send_holdoff_countdown_ = 0;// [orig: NapiNPConnection+0x648] 0 = send block open (default)
	// The host-dictated send period (CS dir-0 field 3, H:0x00 mask 8): the
	// countdown re-arms from this at every open boundary [orig: cs_dir0.
	// send_holdoff_ticks; PumpFlags 0x200 reload @0x629802]. 0 = per-tick
	// (the protocol template default). NovaWorld hosts dictate 12 (~5.2 Hz).
	uint32_t send_holdoff_ticks_ = 0;
	// The retail loop the joiner's last frame ran in (a change is that loop's entry) and the
	// GetTickCount stamp of a timed loop's last send-pump call (step_send_pump_loop).
	JoinerConnection::SendPumpLoop send_pump_loop_ = JoinerConnection::SendPumpLoop::NetworkFrame;
	uint32_t send_pump_loop_last_ms_ = 0;
	// The receive pump's countdown step plus whether this frame's send pump call builds,
	// per the loop the joiner's admission stage runs in.
	bool step_send_pump_loop();
	// One PumpClientProtocolSend build: the pre-framed packets, the connection's queue,
	// then what `append` adds behind it (the frame's own 0x2C / 0x0C), the boundary's pump
	// (its ACK) and the one flush.
	void build_send_boundary(std::vector<std::vector<uint8_t>> &outbound, uint32_t now_tick,
			const std::function<void(std::vector<ProtocolMessage> &)> &append = {});
	// The C2S 0x0F self-heal requests the replica fold and the stale-carrier sweep raised,
	// appended to the one queue where their producer ran.
	void queue_carrier_repair_requests();
	// The S2C 0x06 echo of a console line: the client's command walk over it, its
	// sends queued on this client's connection at the message's place, its lines
	// posted to the SYSTEM ring (client_console.h, D-NET-710).
	void run_console_echo(const std::vector<uint8_t> &body);
	ClientConsoleState console_state_; // the process's console debug words
	uint32_t console_guard_ms_ = 0;   // [orig: g_ConsoleCommandLastTick @0x24D1E04]
	ClientLocalCommandSeams local_command_seams_;
	ClientConsoleQueueAction console_action_sink_;
	std::function<bool(const std::string &line)> authority_console_;
	std::vector<ConsoleAction> console_actions_; // Input_QueueEvent's (queue_console_action)
	// GetTickCount (the console guard, the S2C 0x58's receipt stamp): the joiner's
	// monotonic clock, else the process's steady clock.
	uint32_t console_tick_ms() const;
	// The GetTickCount stamp of the S2C 0x58 the replica last folded (session_elapsed_ms).
	uint32_t session_status_receipt_ms_ = 0;

	// seed_session() golden-replay mode: suppress the live per-frame housekeeping (0x34/0x4C/0x2C) so a
	// seeded single-frame emission reproduces ONLY the captured 0x0C datagram byte-for-byte (the
	// determinism contract of the replay path — seed_session is "for replay/parity only").
	bool replay_mode_ = false;
};

// The voice-macro key's A&S context: the speaker stands inside the nearest
// active, map-visible capture entry with a nonzero Q16 coverage
// (client_effects.cpp carries the witness). The contextual radio calls and
// the F9/F10 menus share it.
// [orig: Entity_FindNearestProximityEntity @0x5380C0, consumed by
//  VMacros_BuildShaderPassName @0x5BF5D0's 0x10010 arm @0x5BF9D4..0x5BF9DE]
bool in_active_radio_zone(const world::World &world, const world::Entity &speaker,
		const ClientRuntime &runtime);

} // namespace opennova::inmatch
