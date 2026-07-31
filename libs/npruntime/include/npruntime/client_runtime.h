#pragma once

#include "npruntime/joiner_connection.h"

#include <netsim/net_client_view.h>     // NetClientView / ClientState
#include <netsim/session_transport.h>   // ISessionTransport

#include <npwire/ingame_decode.h>     // PlayerExtendedUplink (the §5.10 0x0C body)

#include <cstddef>
#include <cstdint>
#include <array>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// P5 — the headless, socket-free CLIENT runtime. The faithful reimpl of the per-frame client net
// role [orig: Client_ProcessNetworkFrame @0x42c180] composed with the connect-leg state machine
// (np::JoinerConnection) and the S2C->ClientState fold (netsim::NetClientView). The original runs
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
//     {tag,body} bodies, which we fold via NetClientView::apply (NOT through a transport).
//   - HostClient inbound is the inner {tag,body} the host's Server_TickUpdate host_send()s onto the
//     in-process LoopbackChannel (the ADR-0011 §3 SP crypto bypass — no 0x83 framing on the
//     loopback), folded via NetClientView::pump(transport).
//
// [orig: Client_ProcessNetworkFrame @0x42c180; PumpClientProtocolRecv @0x42c228 / Send @0x42c4bc;
//  Player_BuildTag0CInputBody @0x42a550; docs/net §5.44]. No socket I/O lives here.
namespace opennova::np {

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
	explicit ClientRuntime(netsim::ISessionTransport &host_loopback);

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

	// One C2S 0x1D stance-change datagram (0xA9 crouch / 0xAA prone / 0xAC stand), sent
	// immediately from the stance key SELECT. Empty for HostClient or pre-in-match.
	// [orig: @0x4e0d77/@0x4e0df3/@0x4e0e3e]
	std::vector<uint8_t> send_stance_change(uint16_t action_id) {
		if (role_ != Role::Joiner || joiner_ == nullptr) return {};
		return joiner_->frame_stance_change(action_id);
	}

	// The uplink gate: true once admitted+spawned, cleared on a death sample. Every C2S
	// gameplay send (0x0C uplink, 0x06 fire, 0x2C ping) rides it.
	bool is_deployed() const { return deployed_; }
	// Monotonic receive-side deployment release edge. It advances only when
	// JoinerConnection accepts the ACK-qualified 0x5A that completes either the
	// initial deployment or a later re-deployment. Consumers use the revision,
	// rather than positive health alone, to distinguish a real respawn from a
	// stale 0x0A tail.
	uint64_t deployment_release_revision() const {
		return deployment_release_revision_;
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
	// No-uplink frame (HostClient, or a pre-deploy Joiner): recv pump + connect-drive only, no 0x0C.
	std::vector<std::vector<uint8_t>> Client_ProcessNetworkFrame(uint32_t now_tick = 0);

	// Typed gameplay seams used by the simulation; protocol tags/framing remain
	// owned here. Fire is predicted locally before queueing C2S 0x06. The spent clip
	// remains unchanged until the host's S2C 0x49 echo appears in the reload drain.
	bool queue_fired_round(const ClientFiredRound &round);
	bool queue_reload_request(const WeaponReload &reload);
	bool queue_vehicle_attach(uint16_t vehicle_handle, uint8_t model_bone_index);
	bool queue_vehicle_detach(uint16_t vehicle_handle);
	std::vector<netsim::ClientRoundEvent> drain_round_events();
	std::vector<WeaponReload> drain_reload_notifications();

	// Deterministic golden replay (Joiner): seed the connection keys + seq/ack + self handle/type so
	// frame_c2s_uplink reproduces a captured C2S 0x0C datagram byte-for-byte. [ROADMAP "Determinism"]
	// `tick_seed` is the capture's live network-role tick (the value its S2C 0x61 seeded);
	// without it a replayed client's clock stays parked at zero and stamps 0 into every 0x06.
	void seed_session(uint32_t session_id, uint32_t client_key, std::string client_scrk,
	                  std::string server_scrk, uint32_t next_seq, uint32_t last_ack,
	                  uint16_t self_handle, uint16_t self_type, uint32_t game_type = 0,
	                  uint32_t tick_seed = 0, bool replay_mode = true);

	// The "deployed" predicate gating the 0x0C uplink. It defaults true on reaching InMatch;
	// a complete recipient-local 0x0A tail with health <= 0 closes it before the same frame's send.
	// Positive health does not reopen it: death re-enters JoinerConnection's deployment-pick FSM,
	// and only the ACK-qualified post-pick 0x5A release returns to InMatch and reopens the gate.
	// set_deployed remains an explicit simulation/test override.
	void set_deployed(bool v) { deployed_ = v; }
	bool deployed() const { return deployed_; }

	// The shell's kit for the 0x1A-released loadout-submission pair (Joiner only; see
	// JoinerConnection::set_loadout_kit). HostClient has no 0x2F leg — its player fills
	// server-side [orig: Server_InitAllPlayerEntitiesForRound @0x516aa0].
	void set_loadout_kit(JoinerConnection::LoadoutKit kit) {
		if (joiner_) joiner_->set_loadout_kit(std::move(kit));
	}
	void set_character_join_vars(CharacterJoinVars vars) {
		if (joiner_) joiner_->set_character_join_vars(vars);
	}
	void set_charattr_challenge_table(CharAttrChallengeTable table) {
		if (joiner_) joiner_->set_charattr_challenge_table(std::move(table));
	}
	void clear_charattr_challenge_table() {
		if (joiner_) joiner_->clear_charattr_challenge_table();
	}
	void set_loaded_model_challenge_snapshot(std::vector<uint32_t> values) {
		if (joiner_)
			joiner_->set_loaded_model_challenge_snapshot(std::move(values));
	}
	// Queue the armory-ACCEPT loadout re-submission; the next frame's send boundary emits
	// one C2S 0x2F from the current kit seam (see JoinerConnection::frame_loadout_resubmit).
	void queue_loadout_resubmit() {
		if (joiner_) pending_loadout_resubmit_ = true;
	}

	// Player-paced deployment (the deploy-map screen; see JoinerConnection).
	void set_player_paced_deployment(bool paced) {
		if (joiner_) joiner_->set_player_paced_deployment(paced);
	}
	bool deployment_pick_pending() const {
		return joiner_ && joiner_->deployment_pick_pending();
	}
	// The joiner's server-assigned team (the S2C 0x04 latch) — the deploy screen's
	// row filter and color source [orig: byte_A85B48].
	uint8_t assigned_team() const { return joiner_ ? joiner_->assigned_team() : 0; }
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
	// Queue the player's C2S 0x0E pick (0xFFFF default, 0xFFFE auto, else a spawn-target
	// handle); the next frame emits it. Re-picks while awaiting the release are allowed.
	void queue_deployment_pick(uint16_t wire_value) {
		if (!joiner_) return;
		pending_deployment_pick_ = wire_value;
		pending_deployment_pick_set_ = true;
	}

	// Pre-load join seam. Handshake and admission traffic continue through terminal S2C 0x11 while
	// false; only C2S 0x0A and the resulting world/deployment stream are held.
	void set_world_ready(bool ready) {
		if (joiner_) joiner_->set_world_ready(ready);
	}
	bool world_ready() const { return joiner_ ? joiner_->world_ready() : true; }
	bool mission_known() const { return joiner_ && joiner_->mission_known(); }
	// The terminal pre-world sync marker was received and its ACK reached the send boundary.
	bool preload_ready() const { return joiner_ && joiner_->preload_ready(); }
	// Admission-stage name for diagnostics (the shell's post-load join watchdog).
	const char *admission_stage_name() const {
		return joiner_ ? joiner_->post_auth_stage_name() : "no session";
	}

	// Joiner state passthrough (HostClient: never InMatch, no self handle).
	bool in_match() const { return joiner_ && joiner_->in_match(); }
	bool has_self_handle() const { return joiner_ && joiner_->has_self_handle(); }
	uint16_t self_handle() const { return joiner_ ? joiner_->self_handle() : 0; }
	JoinerConnection::Phase phase() const {
		return joiner_ ? joiner_->phase() : JoinerConnection::Phase::Idle;
	}
	// The host-advertised self-spawn pose learned at the name-match — full precision (not the lossy
	// ClientState position), so the binding spawns its local player L from it. Joiner-only; valid once
	// in_match() (the caller gates on that). Mirrors the self_handle() passthrough.
	const JoinerConnection::SelfSpawn &spawn_pose() const { return joiner_->spawn_pose(); }
	uint32_t game_type() const { return joiner_ ? joiner_->game_type() : view_.game_type(); }
	const std::string &server_name() const;
	const std::string &mission_name() const;
	const std::string &map_file() const;
	const std::string &expansion() const;
	const std::string &last_error() const;

	struct ZoneState {
		bool has_value = false;
		ZoneTimerValue value;
		bool has_window = false;
		ZoneTimerWindow window;

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
	uint32_t send_holdoff_countdown() const { return send_holdoff_countdown_; }

	const netsim::ClientState &state() const { return view_.state(); }
	netsim::ClientState &state() { return view_.state(); }
	// Called by the joiner simulation after decoded round events have stamped
	// remote recoil, preserving retail's receive -> body-decay frame order.
	void tick_remote_recoil() { view_.tick_recoil(); }
	netsim::NetClientView &view() { return view_; }
	std::size_t unknown_tags() const { return view_.unknown_tags(); }

private:
	// Shared body for both Client_ProcessNetworkFrame overloads. `uplink` is nullptr for a no-uplink
	// frame.
	std::vector<std::vector<uint8_t>> run_frame(const PlayerExtendedUplink *uplink, uint32_t now_tick);
	bool apply_zone_timer_body(uint8_t tag, const std::vector<uint8_t> &body);
	void apply_zone_timer_value(const ZoneTimerValue &value);
	void apply_zone_timer_window(const ZoneTimerWindow &window);
	void advance_zone_timers();

	Role role_;
	std::unique_ptr<JoinerConnection> joiner_;        // Joiner only
	netsim::NetClientView view_;
	netsim::ISessionTransport *loopback_ = nullptr;   // HostClient only (non-owning)
	std::deque<std::vector<uint8_t>> recv_fifo_;      // Joiner: framed inbound awaiting the recv pump
	std::deque<ProtocolMessage> gameplay_send_queue_; // Joiner: typed C2S 0x06/0x25 awaiting SEND
	// Some receive handlers must allocate an exact wire packet immediately: handshake/admission
	// packets preserve their retail grouping, and 0x84 reconstruction must reuse an old sequence.
	// They still belong to PumpClientProtocolSend, so hold the already-framed datagrams behind the
	// same field-3 gate and flush them before any later sequence allocated at the open boundary.
	std::deque<std::vector<uint8_t>> framed_send_queue_;
	// 0x34/0x4C and semantic receive replies are produced before retail reaches the holdoff-gated
	// send pump. Keep them across held frames, then batch them at the first open boundary beside
	// one-shots/gameplay.
	std::deque<ProtocolMessage> pre_send_queue_;
	std::unordered_map<uint16_t, ZoneState> zone_states_;
	WeaponLoadout authoritative_loadout_;
	uint64_t authoritative_loadout_revision_ = 0;
	bool deployed_ = false;
	uint64_t deployment_release_revision_ = 0;
	uint64_t self_team_revision_ = 0;           // S2C 0x50 self re-latch edges
	std::vector<uint8_t> cleared_player_slots_; // S2C 0x46 bit15 roster clears
	bool pending_loadout_resubmit_ = false;     // one-shot: armory-ACCEPT 0x2F re-send
	bool pending_deployment_pick_set_ = false;  // one-shot: the player's 0x0E pick below
	uint16_t pending_deployment_pick_ = 0xFFFF;

	// --- §5.44 per-frame housekeeping counters (P6) — mirror the witnessed per-instance globals of
	// [orig: Client_ProcessNetworkFrame @0x42c180]. The 0x34 keepalive / 0x4C net-quality / 0x2C RTT
	// emits and the send-holdoff send-block gate, deferred-and-logged at P5, ported here. ---
	uint32_t current_tick_ = 0;          // [orig: currentTick @0xA8229C] bumped once per run_frame
	uint32_t last_keepalive_tick_ = 0;   // [orig: g_lastKeepaliveTick @0xA822A0] 0x34 send latch
	uint32_t net_quality_timer_ = 0;     // [orig: g_netQualityReportTimer @0xA85B84] 0x4C cadence
	uint32_t tag2c_send_cooldown_ = 0;   // [orig: g_tag2CSendCooldown @0xA860D8] set 62 on a 0x2C send
	                                     // and decremented, but never compared in @0x42C180;
	                                     // vestigial/telemetry state, not a send throttle.
	uint8_t  net_quality_ = 0;           // [orig: g_netQuality byte @0x82BF88] 0 = best (host clamps 0..4)
	uint32_t send_holdoff_countdown_ = 0;// [orig: NapiNPConnection+0x648] 0 = send block open (default)

	// seed_session() golden-replay mode: suppress the live per-frame housekeeping (0x34/0x4C/0x2C) so a
	// seeded single-frame emission reproduces ONLY the captured 0x0C datagram byte-for-byte (the
	// determinism contract npruntime_golden_client asserts — seed_session is "for replay/parity only").
	bool replay_mode_ = false;
};

} // namespace opennova::np
