#pragma once

#include <runtime/inmatch/joiner_connection.h>
#include <runtime/devtools/tick_profile.h>
#include <runtime/hud/hud_chat_entry.h> // ChatSendResult (the C2S 0x0D sender's outcome)
#include <runtime/hud/net_quality_indicators.h> // g_NetQuality's display state
#include <runtime/hud/feed_format.h>    // FeedPost (the client-raised ring lines)
#include <runtime/hud/squad_feed.h>     // SquadFeedLine (the squad folds' HUD lines)

#include <runtime/replication/client_replica_pipeline.h> // ClientReplicaPipeline / ClientState
#include <runtime/replication/net_quality.h>             // the CNetQuality window (the client half)
#include <runtime/inmatch/session_transport.h>        // ISessionTransport

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

namespace opennova::inmatch {

class ClientRuntime {
public:
	enum class Role : uint8_t { Joiner, HostClient };

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
	// Begin the handshake; returns the framed ClientHello to ship (Idle->Hello). Empty for HostClient.
	std::vector<uint8_t> start();

	// Deposit one received FRAMED datagram (off the socket) for the next frame's recv pump to drain.
	// Mirrors CNapiNetwork_PumpManagerReceive feeding the byte recv FIFO ahead of the frame's recv
	// pump. No-op for HostClient (it reads its loopback transport directly).
	void receive(const uint8_t *raw, std::size_t len);

	// Frame the retail leave burst (0x46 ClientGoodBye x4) for the owner to ship before dropping
	// the socket. Empty for HostClient, before ServerAuth, or on a repeat call.
	// [orig: CNapiNPConnection_TeardownActiveConnection @0x6253c0]
	std::vector<std::vector<uint8_t>> disconnect();

	// Queue one reliable C2S 0x1D stance change (0xA9 crouch / 0xAA prone / 0xAC stand) from
	// the stance key SELECT; it leaves inside the next open send boundary in queue order.
	// False for HostClient or before in-match. [orig: cases 169/170/172 @0x4e0d77/@0x4e0df3/
	// @0x4e0e3e -> CNapiNetwork_QueueReliableMessage @0x4e0de7]
	bool queue_stance_change(uint16_t action_id);
	// Queue one reliable C2S 0x1A door section request {handle, record state,
	// section} the joiner world's door callback raised; it leaves inside the
	// next open send boundary in queue order. False for HostClient or before
	// in-match. [orig: NetPacket_SendWeaponSwitch @0x42D0C0 ->
	// CNapiNetwork_QueueReliableMessage @0x42d169]
	bool queue_door_request(uint16_t handle, int16_t state, uint8_t section);

	// Effective gameplay readiness. Every C2S gameplay send (0x0C uplink,
	// 0x06 fire, 0x2C ping) requires both retail's dword_81474C hold to be open
	// and the independent authoritative spawn/health latch to be released.
	// Every valid 0x5A opens the literal gameplay gate even while a spawn-zone
	// deploy screen remains pending; queuing C2S 0x0E closes only that gate. A
	// complete recipient-local 0x0A tail with health <= 0 closes only the
	// authoritative spawn latch. Positive health does not reopen it: death
	// re-enters JoinerConnection's deployment-pick FSM, and only the applicable
	// post-pick release reopens spawn.
	bool is_deployed() const {
		return deployed_ && authoritative_spawn_released_;
	}
	// The literal dword_81474C state. Every valid S2C 0x5A opens it, including
	// an unrelated loadout echo while dead; that alone cannot resume gameplay.
	bool gameplay_gate_open() const { return deployed_; }
    // Local Game_InitNewRound clears the medic latch and gameplay hold.
    void reset_local_round_state();
	// Monotonic receive-side deployment-release edge. It advances when initial
	// establishment becomes applicable (even if deploy UI remains pending), and
	// again on an ACK-qualified post-pick release. Unrelated valid 0x5A grants
	// still open gameplay_gate_open(), but do not invent a pose/respawn edge.
	// Consumers use the revision,
	// rather than positive health alone, to distinguish a real respawn from a
	// stale 0x0A tail.
	uint64_t deployment_release_revision() const {
		return deployment_release_revision_;
	}
	// The independent authoritative spawn/health latch. Death closes it; an
	// applicable 0x5A reopens it. Input case 12 never changes it, so a player-paced
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
	// The chat flood table's 16 recent lines `[u32 frame][char[64]]`: a repeat
	// of a line within 0x500 main frames of its entry is refused, an older
	// repeat is moved to the newest slot. The clock is the per-main-frame
	// counter the talk debounce reads too, not wall time [orig:
	// Chat_CheckFloodControl @0x498F60 — the 16 x 68-byte table @0xB3B788,
	// `dword_A8705C - entry <= 0x500` @0x499028, the shift-down + append].
	struct ChatFloodEntry {
		uint32_t frame = 0;
		std::string text;
	};

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
	// g_SpawnSuccessGate` — the local and crew keys test `!g_DeathScreenActive`
	// alone; the flood table (a refusal is `Flooded`: the caller echoes the
	// line); then the `<...>` strip. A joiner queues it reliable with the
	// 310-flush lifetime; the listen host's own client (a peer too) sends it
	// over its loopback to its own server, which fans it like any peer's.
	// `text` is cut to 59 characters in place by the flood check; `frame` is
	// the per-main-frame counter.
	// [orig: sub_49A840 @0x49A840 (13), Chat_SendTeamMessage @0x49A900 (IDB
	//  misnomer; 1), Chat_SendGlobalMessage @0x49A6B0 (IDB misnomer; 2),
	//  Chat_SendSquadMessage @0x49AA50 (12), Chat_SendAdminMessage @0x49A780
	//  (11), Chat_SendAllMessage @0x49AC70 (4), sub_49ABA0 @0x49ABA0 (5) ->
	//  CNapiNetwork_QueueReliableMessage(0xD, 1, 310)]
	hud::ChatSendResult queue_chat_message(uint8_t channel, std::string &text, uint32_t frame);
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
	// The main loop's measured frame rate for the quality metric's
	// frame-pressure term [orig: g_StatsAvgFps (dword_24E1F10), read by
	// CNetQuality_UpdateMetrics @0x4C5643]: inmatch::Session hands over its
	// FR counter once per banked frame (Role::observe_frame_rate). 0 is
	// retail's mode-init value (the ceiling metric 255) until the first 2 s
	// window closes; any rate at or above 16 scores the floor. Joiner only.
	void set_observed_frame_rate(int32_t fps) { observed_frame_rate_ = fps; }
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
	// request (flag 2) [orig: NapiNP_HandleResendList cb_server_6 = sub_4C62A0
	// @0x623a0e; SendMissingSeqList cb_server_5 = @0x4c4681 @0x62379b ->
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
	std::vector<replication::ClientRoundEvent> drain_round_events();
	// S2C 0x1E game events the recv fold surfaced this frame — the kill /
	// objective / medic feed lane. The embedder formats each into a canned
	// sentence against its roster + string table [orig: 0x426270 -> 0x422DA0].
	std::vector<replication::ClientGameEvent> drain_game_events();
	std::vector<WeaponReload> drain_reload_notifications();
	// Consume sounds, explosions, and class death/state callbacks in receive
	// order. Pure joiners apply death callbacks to the world twin; listen
	// clients retain the authority's already-applied gameplay state.
	// [orig: NapiNPClientMsg_EntityDeath @0x42EB50 — cb(entity, 4, 0) @0x42ebf5]
	void apply_received_effects(world::World &world);

	// THE COMMAND MAP'S SQUAD AND WAYPOINT SENDS (client_squad.cpp): each
	// queues its one reliable C2S (a joiner on its held one-shot queue, the
	// listen host's client on its loopback to its own server); false when no
	// session carries it. [orig: NetPacket_SendChatMessage @0x42ddc0 (0x17),
	//  NetPacket_SendEntityUpdate @0x42de00 (0x4F), NetPacket_SendWeaponSlotSwitch
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
	// THE CMAP SCREEN'S OWN LEGS (client_squad.cpp): the world halves
	// (world/user_waypoints.h) plus their sends — the placed waypoint's C2S
	// 0x17 (target 0xFF) [orig: @0x54a1be], each removed one's C2S 0x4F
	// [orig: @0x5479e6; @0x548137]. A go-code button: C2S 0x4B [local
	// slot][code], then the leader's own sound set and line with no mute
	// [orig: sub_548290 @0x548290].
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
	// frame_c2s_uplink reproduces a captured C2S 0x0C datagram byte-for-byte. [ROADMAP "Determinism"]
	// `tick_seed` is the capture's live network-role tick (the value its S2C 0x61 seeded);
	// without it a replayed client's clock stays parked at zero and stamps 0 into every 0x06.
	void seed_session(uint32_t session_id, uint32_t client_key, std::string client_scrk,
	                  std::string server_scrk, uint32_t next_seq, uint32_t last_ack,
	                  uint16_t self_handle, uint16_t self_type, uint32_t game_type = 0,
	                  uint32_t tick_seed = 0, bool replay_mode = true);

	// The shell's kit for the 0x1A-released loadout-submission pair (Joiner only; see
	// JoinerConnection::set_loadout_kit). HostClient has no 0x2F leg — its player fills
	// server-side [orig: Server_InitAllPlayerEntitiesForRound @0x516aa0].
	void set_loadout_kit(JoinerConnection::LoadoutKit kit) {
		if (joiner_) joiner_->set_loadout_kit(std::move(kit));
	}
	void set_character_join_vars(CharacterJoinVars vars) {
		if (joiner_) joiner_->set_character_join_vars(vars);
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
	// The CD identity cookie (packed PUB* blob) for the 0x00 JOIN. Joiner only.
	void set_join_cd_cookie(std::vector<uint8_t> cookie) {
		if (joiner_) joiner_->set_cd_cookie(std::move(cookie));
	}
	void set_charattr_challenge_table(CharAttrChallengeTable table) {
		if (joiner_) joiner_->set_charattr_challenge_table(std::move(table));
	}
	void clear_charattr_challenge_table() {
		if (joiner_) joiner_->clear_charattr_challenge_table();
	}
	// The joiner's live charattr table (null without a joiner: a HostClient
	// reads its embedder's boot copy, which no 0x41 ever mutates).
	const CharAttrChallengeTable *charattr_challenge_table() const {
		return joiner_ ? &joiner_->charattr_challenge_table() : nullptr;
	}
	bool set_integrity_challenge_profile(std::string_view id) {
		return joiner_ != nullptr &&
		       joiner_->set_integrity_challenge_profile(id);
	}
	void clear_integrity_challenge_profile() {
		if (joiner_) joiner_->clear_integrity_challenge_profile();
	}
	void set_loaded_model_challenge_snapshot(std::vector<uint32_t> values) {
		if (joiner_)
			joiner_->set_loaded_model_challenge_snapshot(std::move(values));
	}
	// Queue the armory-ACCEPT loadout re-submission: one C2S 0x2F from the current kit seam,
	// queued now as NetPacket_SendLoadoutSubmit queues it at the ACCEPT, so it leaves at the
	// next open boundary in queue order [orig: @0x42d085] (JoinerConnection::prepare_loadout_resubmit).
	void queue_loadout_resubmit() {
		ProtocolMessage resubmit;
		if (joiner_ && joiner_->prepare_loadout_resubmit(resubmit))
			send_queue_.push_back(std::move(resubmit));
	}

	bool deployment_pick_pending() const {
		return joiner_ && joiner_->deployment_pick_pending();
	}
	bool initial_admission_complete() const {
		return joiner_ && joiner_->initial_admission_complete();
	}
	// The joiner's server-assigned team (the S2C 0x04 latch) — the deploy screen's
	// row filter and color source [orig: byte_A85B48].
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
	// consumed through the spawn instead). [orig: byte_A85B48 store @0x4319db]
	uint64_t self_team_revision() const { return self_team_revision_; }
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
	// Accept the player's C2S 0x0E pick (0xFFFF default, 0xFFFE auto, else a
	// spawn-target handle). Input case 12 closes retail's gameplay dword and queues
	// the 0x0E now [orig: @0x49b17b]; framing waits for the next open send boundary,
	// in queue order. Re-picks while awaiting the release are allowed. False means
	// the deployment UI/state cannot accept a pick.
	bool queue_deployment_pick(uint16_t wire_value) {
		if (!joiner_) return false;
		// Initial admission may finish while the authority still holds the
		// deploy-map overlay. Its selection sends the same request as a death
		// re-pick, even though the player is alive and no pick is in flight yet.
		// [orig: Input_HandleActionBinding @0x49AD40, case 12 @0x49B0C5..0x49B17B]
		const bool initial_overlay = joiner_->in_match() &&
				joiner_->initial_admission_complete() && view_.state().deploy_overlay_active;
		if (!joiner_->deployment_pick_pending() && !initial_overlay) return false;
		ProtocolMessage pick;
		if (!joiner_->prepare_deployment_pick(wire_value, pick)) return false;
		send_queue_.push_back(std::move(pick));
		deployed_ = false;
		return true;
	}

	// Pre-load join seam. Handshake and admission traffic continue through terminal S2C 0x11 while
	// false; only C2S 0x0A and the resulting world/deployment stream are held.
	void set_world_ready(bool ready) {
		if (joiner_) joiner_->set_world_ready(ready);
	}
	bool world_ready() const { return joiner_ ? joiner_->world_ready() : true; }
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
	// The terminal pre-world sync marker was received and its ACK reached the send boundary.
	bool preload_ready() const { return joiner_ && joiner_->preload_ready(); }
	// Admission-stage name for diagnostics (the shell's post-load join watchdog).
	const char *admission_stage_name() const {
		return joiner_ ? joiner_->post_auth_stage_name() : "no session";
	}

	// Joiner state passthrough (HostClient: never InMatch, no self handle).
	bool in_match() const { return joiner_ && joiner_->in_match(); }
	bool in_session() const { return joiner_ && joiner_->in_session(); }
	bool awaiting_deploy_pick() const { return joiner_ && joiner_->awaiting_deploy_pick(); }
	bool has_self_handle() const { return joiner_ && joiner_->has_self_handle(); }
	uint16_t self_handle() const { return joiner_ ? joiner_->self_handle() : 0; }
	// The joiner's read of its own dead bit: the recipient-local 0x0A health
	// tail (the client stores it as its own Health) or the self record's dead
	// bit (byte13 bit 0x02 -> Flags & 2 in the local apply [orig: @0x4c1005]).
	bool local_player_dead() const {
		const replication::ClientState &cs = state();
		if (cs.local_health <= 0) return true;
		if (!has_self_handle()) return false;
		const replication::ClientEntityState *self = cs.find(self_handle());
		return self != nullptr && self->state_flags_known && (self->state_flags & 0x02u) != 0;
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
	// dword_24D1DDC countdown (reload 62) gated is_in_session ->
	// CNetQuality_UpdateMetrics @0x4C52C0 + CNetQuality_SetLevel @0x4C3060].
	void update_net_quality();
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
	std::array<int32_t, kWorldStateAmmoPoolCount> authoritative_ammo_pools_{};
	uint64_t authoritative_ammo_pools_revision_ = 0;
	bool deployed_ = false;
	uint64_t deployment_release_revision_ = 0;
	bool authoritative_spawn_released_ = false;
	uint64_t authoritative_spawn_release_revision_ = 0;
	uint64_t self_team_revision_ = 0;           // S2C 0x50 self re-latch edges
	std::vector<uint8_t> cleared_player_slots_; // S2C 0x46 bit15 roster clears

	// --- §5.44 per-frame housekeeping counters (P6) — mirror the witnessed per-instance globals of
	// [orig: Client_ProcessNetworkFrame @0x42c180]. The 0x34 keepalive / 0x4C net-quality / 0x2C RTT
	// emits and the send-holdoff send-block gate, deferred-and-logged at P5, ported here. ---
	uint32_t current_tick_ = 0;          // [orig: g_ClientCurrentTick @0xA8229C] bumped once per run_frame
	uint32_t slot_refresh_frames_ = 0;   // [orig: g_SlotRefreshTimer @0xA85B80] the 1 Hz revive countdown
	uint32_t last_keepalive_tick_ = 0;   // [orig: g_LastKeepaliveTick @0xA822A0] 0x34 send latch
	uint32_t net_quality_timer_ = 0;     // [orig: g_NetQualityReportTimer @0xA85B84] 0x4C cadence
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
	int32_t quality_update_countdown_ = 62;              // [orig: dword_24D1DDC]
	std::array<ChatFloodEntry, 16> chat_flood_{};       // [orig: @0xB3B788]
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
	// The C2S 0x0F self-heal requests the replica fold and the stale-carrier sweep raised,
	// appended to the one queue where their producer ran.
	void queue_carrier_repair_requests();

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
