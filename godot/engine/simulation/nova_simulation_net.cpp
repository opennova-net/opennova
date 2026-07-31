// NovaSimulation — the net roles (ADR 0009/0011/0012): per-load listen-host
// bring-up + host pump, the LAN joiner pump family + wire proxies/events, the
// host session config FFI, and the joiner preload/session API.
#include "simulation/nova_simulation_internal.h"

#include <npwire/ingame_message_id.h>
#include <world/entity_spawn.h> // entity_reset_to_spawn_state (redeploy release)
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace novasim;

// P7: per-load host bring-up — the faithful §5.0 mode-3 in-process listen server
// [orig: SinglePlayer_StartMission @0x561af0], mirroring apps/nw_server/main.cpp. The host's own
// player AUTO-spawns through the real pipeline (Server_ProcessPendingPlayerSpawns ->
// select_player_spawn start marker), and its own loopback client renders the per-frame 0x0A.
void NovaSimulation::bringup_host_runtime(const opennova::bms::File &file) {
	namespace np = opennova::np;
	// Persist the mission so ctx_.mission (read by the §5.1 0x0B BMS-header burst for LAN joiners)
	// outlives the match — the load-local bms::File would dangle.
	mission_file_ = file;
	host_loop_.clear();
	// Reload: a fresh host_owner_ drops any stale connections / peers from a prior mission. A reload is a
	// new match (Stop -> load), so configure_session_runtime runs once per match (never mid-match,
	// D-NET-124). serve_and_play: host_session_pump must NOT discard the host's own loopback 0x0A — we
	// fold it into ClientState (runtime_) to render the host's own view.
	// Serve-and-play (default) vs dedicated. Standalone SP is ALWAYS serve-and-play (it renders the
	// host's own player); isolated test/tooling MissionRuntime instantiations keep that default too.
	// ONED has no live editor-preview branch: MainGame/GameWorld is its sole live mission runtime
	// (ADR 0025). A LAN host honors the UI server-type (host_serve_and_play_, from
	// configure_host_session). A dedicated host (serve_and_play=false) skips the own-player spawn +
	// the local view below and lets host_session_pump discard the host loopback (step 5) — mirroring
	// start_host_session's gating [orig: SinglePlayer_StartMission @0x561af0].
	const bool serve_and_play = host_listen_ ? host_serve_and_play_ : true;
	host_owner_ = np::HostOwner{};
	host_owner_.host_loopback = &host_loop_;
	host_owner_.serve_and_play = serve_and_play;
	ctx_.world = world_.get();
	ctx_.mission = &mission_file_;
	ctx_.terrain_til_data = terrain_til_data_; // S2C 0x45 terrain-tile load source (empty => skipped, §5.37)
	// Server_TickUpdate owns the per-frame C2S drain + S2C fan over connection_list; there is no
	// separate net ISystem (retired P8).

	// The ONE consolidated GameConfig for create_session (ADR 0013): a LAN host takes its lobby name /
	// gametype / mission + the §5.1 reply slice from the GDScript-configured host_session_config_; SP is
	// the faithful "SINGLEPLAYERGAME" / 1 player. game_type (g_GameType) now feeds BOTH the S2C 0x08
	// block dword[3] AND the 0x7B/0x60 bodies (§6.9; NapiNPMsg_0x7B_BuildPayload @0x507740).
	np::GameConfig host_config;
	if (host_listen_) {
		host_config = host_session_config_; // mission/player/spawn + game_type/mp_attributes from the UI
		if (host_config.server_name.empty()) host_config.server_name = "OpenNova LAN Host";
		host_config.max_players = host_max_players_; // the UI player cap (configure_host_session clamped 1..65)
	} else {
		host_config.server_name = "SINGLEPLAYERGAME";
		host_config.max_players = 1;
	}
	if (world_) {
		world_->fat_bullets = host_config.fat_bullets;
		world_->one_shot_kill = host_config.one_shot_kill;
	}

	// The witnessed §5.0 listen-host bring-up, dedup'd to the ONE shared helper start_host_session
	// (mode 3 -> set_transport_mode -> create_session(&host_loop_) [+ Server_InitNewRoundState] ->
	// configure_session_runtime; then, when serve_and_play, FAITHFUL auto-spawn of the host's own player
	// at the start marker + latch its loopback in-match so Server_TickUpdate fans it the per-frame
	// whole-world 0x0A its local view renders from). host_owner_.host_loopback / .serve_and_play + ctx_.world
	// were set above; this replaces the copy that had drifted out of the helper. [orig: SinglePlayer_StartMission
	// @0x561af0]. The one GameConfig carries the §5.1 reactive-reply config for the joiner replies too.
	np::HostConfig host_cfg;
	host_cfg.config = host_config;
	host_cfg.socket_mode = host_listen_ ? np::SocketMode::Lan : np::SocketMode::Socketless;
	host_cfg.serve_and_play = serve_and_play;
	np::start_host_session(host_owner_, host_cfg);
	if (serve_and_play) {
		// The host's own client view (HostClient role: recv-fold only, 0x0C suppressed). Folds host_loop_
		// each frame into the ClientState the present pass reads.
		runtime_ = std::make_unique<np::ClientRuntime>(host_loop_);
		// Phase-3 0x0A objective width is gated by the same g_GameType
		// carried to remote clients in 0x7B extra; the local loopback has no
		// handshake, so seed its view directly from the consolidated config.
		runtime_->view().set_game_type(host_config.game_type);

		// Seed the look heading from the auto-spawned player's facing so the body starts aligned (the
		// motor drives entity Yaw from player_input_.look_heading each frame, else input snaps it to 0).
		player_input_ = opennova::world::PlayerInput{};
		stance_latch_ = 0;
		look_px_accum_x_ = look_px_accum_y_ = 0.0f;
		if (world_->ai && world_->cached.local_player.valid()) {
			if (const AiEntity *pe = world_->ai->for_handle(world_->cached.local_player)) {
				player_input_.look_heading = pe->heading;
			}
		}
	} else {
		// Dedicated (UI "serve only"). The witnessed original makes this a true host-only session
		// [orig: HG_SERVEONLY -> CGameSession_SetConnectionMode(1), is_host=1/is_client=0; HostDialog
		// read @0x555940, dispatch @0x556d00, mode switch @0x4c49f0]. start_host_session now selects
		// that exact HostOnly row, passes no type-2 loopback to create_session, and creates no local
		// player. There is therefore no local client view: runtime_ stays null, and host_pump's fold
		// plus the present snapshot both guard on it (D-NET-131 fixed 2026-07-24).
		runtime_.reset();
	}
}

namespace {
// NovaUdpPump-backed netsim::IDatagramSocket — the Godot adapter the shared host owner loop pumps. A
// null/closed pump (pure SP) yields recv 0 / send no-op, so the loop's socket legs go inert exactly as
// the old host_listen_-gated code did. PeerAddr <-> "a.b.c.d" uses the LE octet packing PeerAddr
// documents (octet 0 in the low byte; 127.0.0.1 -> 0x0100007F) — the conversion formerly in
// peer_from_addr / send_datagram.
class NovaUdpPumpDatagramSocket : public opennova::netsim::IDatagramSocket {
public:
	explicit NovaUdpPumpDatagramSocket(NovaUdpPump *pump) : pump_(pump) {}

	int recv_from(uint8_t *buf, std::size_t cap, opennova::PeerAddr &from) override {
		if (pump_ == nullptr || !pump_->is_open()) return 0;
		if (!pump_->has_inbound()) {
			pump_->poll();
			if (!pump_->has_inbound()) return 0;
		}
		const Dictionary d = pump_->take_inbound();
		const String ip = d.get("ip", String());
		const int port = d.get("port", 0);
		const PackedByteArray bytes = d.get("bytes", PackedByteArray());
		uint32_t packed = 0;
		const PackedStringArray parts = ip.split(".");
		if (parts.size() == 4) {
			packed = static_cast<uint32_t>(parts[0].to_int() & 0xFF) |
			         (static_cast<uint32_t>(parts[1].to_int() & 0xFF) << 8) |
			         (static_cast<uint32_t>(parts[2].to_int() & 0xFF) << 16) |
			         (static_cast<uint32_t>(parts[3].to_int() & 0xFF) << 24);
		}
		from = opennova::PeerAddr{packed, static_cast<uint16_t>(port)};
		const std::size_t n = std::min(cap, static_cast<std::size_t>(bytes.size()));
		if (n > 0) std::memcpy(buf, bytes.ptr(), n);
		return static_cast<int>(n);
	}

	void send_to(const opennova::PeerAddr &to, const uint8_t *data, std::size_t len) override {
		if (pump_ == nullptr || !pump_->is_open() || len == 0) return;
		const std::string ip = opennova::peer_addr_ip_to_string(to);
		PackedByteArray bytes;
		bytes.resize(static_cast<int64_t>(len));
		std::memcpy(bytes.ptrw(), data, len);
		pump_->send_to(String(ip.c_str()), to.port, bytes);
	}

private:
	NovaUdpPump *pump_;
};
constexpr const char *kJoinerNetDiagnosticsEnv = "OPENNOVA_NET_DIAGNOSTICS";

bool environment_flag_enabled(const char *name) {
	const String value =
			OS::get_singleton()->get_environment(name).strip_edges().to_lower();
	return !value.is_empty() && value != "0" && value != "false" &&
			value != "no" && value != "off";
}


} // namespace

// P7/A5: the per-frame host owner loop is now a THIN delegation to the shared core host_session_pump
// (libs/npruntime) — the SAME loop apps/nw_server runs, so the headless server and the Godot binding can no
// longer drift. NovaSimulation supplies the socket (a NovaUdpPump adapter; SP passes a null pump and the
// loop's socket legs go inert) and folds the host's own loopback 0x0A into ClientState for the present
// pass (serve_and_play: host_session_pump skips the loopback discard so we can read it here).
void NovaSimulation::resolve_infantry_adm_before_server_tick(void *p_context) {
	if (p_context == nullptr) return;
	static_cast<NovaSimulation *>(p_context)->resolve_new_infantry_adm_ids();
}

void NovaSimulation::drain_host_client_gameplay_requests() {
	if (!world_ || !host_owner_.serve_and_play) return;

	// The local player is a real type-2 connection over transport-mode-1. Its
	// socketless C2S FIFO still enters the SAME per-message server dispatcher as
	// a remote type-1 connection; only the outer 0x43/SCRK envelope is absent.
	// Server_TickUpdate's generic transport fan owns 0x0C and ignores other tags,
	// so consume the local gameplay messages at this recv-before-logic boundary.
	// [orig: WeaponAction_Reload @0x5430B0 ->
	// NapiNPServerMsg_HandleReloadRequest @0x514DF0]
	opennova::np::NapiNPConnection *local = nullptr;
	for (opennova::np::NapiNPConnection &conn :
			ctx_.np_protocol.connection_list) {
		if (conn.type == 2 && conn.link.transport == &host_loop_) {
			local = &conn;
			break;
		}
	}
	if (local == nullptr) return;

	opennova::netsim::Datagram dg;
	std::vector<opennova::netsim::Datagram> deferred;
	while (host_loop_.host_recv(dg)) {
		// This seam owns only the witnessed local reload producer. Preserve
		// every other C2S datagram, in FIFO order, for Server_TickUpdate's
		// authoritative transport drain (notably a future/local 0x0C).
		if (dg.tag != opennova::c2s::WEAPON_RELOAD_REQUEST) {
			deferred.push_back(std::move(dg));
			continue;
		}
		std::vector<opennova::ProtocolMessage> messages;
		messages.push_back(opennova::make_protocol_message(
				dg.tag, std::move(dg.body)));
		std::vector<opennova::ProtocolMessage> replies =
				opennova::np::dispatch_session_replies(
						ctx_.config, *local, messages, host_owner_.now_tick,
						ctx_.np_protocol.connection_list, world_.get(),
						ctx_.np_protocol.session_seed_id);
		// A loopback direct reply is already an inner {tag,body} datagram. The
		// 0x25 handler itself returns no direct reply; its 0x49 is broadcast to
		// every transport (including this one) inside the shared dispatcher.
		for (opennova::ProtocolMessage &reply : replies) {
			host_loop_.host_send(reply.tag, std::move(reply.payload));
		}
	}
	for (opennova::netsim::Datagram &preserved : deferred) {
		host_loop_.deliver_c2s(
				preserved.tag, std::move(preserved.body));
	}
}

void NovaSimulation::host_pump() {
	namespace np = opennova::np;
	const uint32_t now = host_owner_.now_tick;
	drain_host_client_gameplay_requests();
	apply_player_input_pre_tick(); // input -> the host player's body input, before logic (ADR 0009/0012)
	NovaUdpPumpDatagramSocket sock(host_listen_ ? pump_.ptr() : nullptr);
	np::host_session_pump(host_owner_, sock,
			&NovaSimulation::resolve_infantry_adm_before_server_tick, this);
	sync_local_mounted_input_heading();
	tick_local_player_view();   // retail promotes the per-frame view before weapon actions
	tick_local_player_weapon(); // the equipped-slot FSM pump, after the view promoter
	// The host's measurable net leg for the F3 Stats board: the ClientState
	// fold. The S2C serialize/emit half rides inside np::host_session_pump
	// (fused with the logic tick) and stays inside the Sim step number until
	// npruntime grows a phase seam.
	const uint64_t net_start =
			runtime_profiling_enabled_ ? perf_now_us() : 0;
	if (runtime_) runtime_->Client_ProcessNetworkFrame(now); // fold host_loop_ -> ClientState (HostClient view)
	if (runtime_profiling_enabled_)
		last_net_tick_us_ = perf_now_us() - net_start;
}

// P7: the per-frame non-authority client loop — the Godot equivalent of the joiner half of
// Client_ProcessNetworkFrame (§5.44). The recv-fold + the C2S 0x0C uplink are fused inside the
// runtime. Retail dispatches received messages before the entity/weapon-action pumps, so decoded
// consequences are applied to L before this frame's local World tick. In particular, an S2C 0x49
// arriving on a reload's DONE boundary must refill the slot before IDLE can observe the stale empty
// magazine and queue a second C2S 0x25. [orig: Game_ProcessMainFrame @0x5263f0;
// Client_ProcessNetworkFrame @0x42c180]
void NovaSimulation::sync_joiner_authoritative_mount() {
	if (!joiner_ || !runtime_ || !runtime_->has_self_handle() ||
			!joiner_local_spawned_ || !world_ ||
			!world_->cached.local_player.valid())
		return;
	const opennova::netsim::ClientEntityState *self =
			client_entity_for_handle(runtime_->state(), runtime_->self_handle());
	if (self == nullptr) return;
	opennova::world::Entity *local =
			world_->registry.get(world_->cached.local_player);
	if (local == nullptr) return;

	const bool wire_mounted =
			self->carrier_handle != opennova::world::EntityHandle::kInvalid &&
			self->mount_bone != 0;
	bool changed = false;
	if (!wire_mounted) {
		if (local->mounted)
			changed = opennova::world::entity_detach_from_vehicle(
					*world_, local->handle);
	} else if (!local->mounted ||
			local->mount_target.packed != self->carrier_handle ||
			local->mount_bone != self->mount_bone) {
		// Mission entities retain the same packed pool/slot identity in the
		// joiner's locally promoted world and the host's streamed world. The
		// server has already validated this exact carrier+bone pair.
		changed = opennova::world::entity_process_vehicle_attach(
				*world_, local->handle,
				opennova::world::EntityHandle{self->carrier_handle},
				self->mount_bone);
	}
	if (!changed) return;
	player_view_.binoculars_requested = false;
	binocular_yaw_offset_deg_ = 0.0f;
	binocular_pitch_offset_deg_ = 0.0f;
	refresh_local_player_view_effects();
	sync_local_mounted_input_heading();
	sync_local_usegun_weapon_transition();
}

// Mirror the decoded wire positions of mission entities (pools 1-3) back onto
// the joiner's locally promoted registry rows. The local sim is NOT authoritative
// for any of them — this write-back exists so position CONSUMERS of the local
// world stay truthful on a joiner: the occlusion frame evaluates entity
// visibility from registry positions (a driven-off vehicle must occlude at its
// live position, not its spawn point), and the collision tick tables pick up the
// same fix. Type-guarded on the same promote-order identity the mount reconcile
// above relies on; synthetic children (spawn_origin sentinel) are skipped.
// Orientation is deliberately NOT mirrored (nothing position-critical consumes
// it locally; the render pose rides the present rows).
void NovaSimulation::mirror_client_view_mission_entities() {
	if (!joiner_ || !world_ || runtime_ == nullptr) return;
	for (const opennova::netsim::ClientEntityState &es :
			runtime_->state().entities) {
		const opennova::world::EntityHandle h{es.handle};
		const int pool = h.pool();
		if (pool < 1 || pool > 3) continue;
		opennova::world::Entity *local = world_->registry.get(h);
		if (local == nullptr || local->spawn_origin == 0xFFFFFFFFu ||
				static_cast<uint16_t>(local->item_id) != es.type_id)
			continue;
		local->position.x = static_cast<float>(es.x) / 65536.0f;
		local->position.y = static_cast<float>(es.y) / 65536.0f;
		local->position.z = static_cast<float>(es.z) / 65536.0f;
	}
}

// host_pump's dispatch_event + admit_peer were promoted into libs/npruntime (np::dispatch_event /
// np::admit_peer over host_owner_, driven by host_session_pump) — the SAME code apps/nw_server runs, so
// the Godot binding and the headless server can no longer drift.

// The joiner's per-frame pump. Retail dispatches received messages before the
// entity/weapon-action pumps, so decoded consequences are applied to L before
// this frame's local World tick. In particular, an S2C 0x49 arriving on a
// reload's DONE boundary must refill the slot before IDLE can observe the
// stale empty magazine and queue a second C2S 0x25.
// [orig: Game_ProcessMainFrame @0x5263f0; Client_ProcessNetworkFrame @0x42c180]
// The pump itself is the phase sequence; each helper carries its witnesses.
void NovaSimulation::joiner_pump() {
	if (!runtime_) {
		if (runtime_profiling_enabled_) last_net_tick_us_ = 0;
		return;
	}
	// The joiner's wire leg for the F3 Stats board: recv pump + net frame +
	// uplink ship, ending where the local (non-authority) world work begins.
	const uint64_t net_start =
			runtime_profiling_enabled_ ? perf_now_us() : 0;
	joiner_send_hello_once();
	joiner_deposit_inbound();
	const JoinerFrameSignals decoded = joiner_run_client_net_frame(net_start);
	joiner_spawn_and_arm_local_player();
	if (decoded.health) joiner_apply_authoritative_health();
	sync_joiner_authoritative_mount();
	mirror_client_view_mission_entities();

	// Received projectile/reload gameplay and the decoded remote collision
	// proxies are live inputs to this frame's entity/round/weapon pumps. Applying
	// them here is the retail recv-before-actions boundary, not presentation work.
	refresh_joiner_projectile_proxies();
	apply_joiner_gameplay_events();

	apply_player_input_pre_tick();                  // input -> L's body input
	world_->run_logic_tick(/*is_authority=*/false); // local World tick: moves L's motor ONLY (never Server_TickUpdate)
	sync_local_mounted_input_heading();
	tick_local_player_view();   // retail promotes the per-frame view before weapon actions
	// The equipped-slot FSM pump, after the view promoter. Gated on L: retail
	// pumps weapon actions per-entity, so a joiner whose player has not spawned
	// has no slot to pump — without this gate a click during the join wait
	// discharged the pre-armed FSM with no shooter and the joiner deployed a
	// round short.
	// [orig: WeaponAction_ProcessAllEntities @ 0x526786]
	if (joiner_local_spawned_) tick_local_player_weapon();
	++now_tick_;
}

// ClientHello once (Idle -> Hello) the first armed frame.
void NovaSimulation::joiner_send_hello_once() {
	if (!joiner_started_) {
		const std::vector<uint8_t> hello = runtime_->start();
		if (!hello.empty()) ship_to_host(hello);
		joiner_started_ = true;
	}
}

// Deposit received framed datagrams for this frame's recv pump.
void NovaSimulation::joiner_deposit_inbound() {
	if (pump_.is_valid()) {
		pump_->poll();
		while (pump_->has_inbound()) {
			const Dictionary d = pump_->take_inbound();
			const PackedByteArray bytes = d.get("bytes", PackedByteArray());
			runtime_->receive(bytes.ptr(), static_cast<std::size_t>(bytes.size()));
		}
	}
}

// Recv-fold + connect-drive + the gated C2S 0x0C uplink, then the decoded-state
// folds (loadout/kit, side assignment, deployment-release latch, the ~1 Hz
// freeze tripwire, objective sync into world subgoals). `net_start` is the
// pump's F3 Stats wire-leg clock; it stops right after the uplink ship, before
// the folds, so the Stats board measures exactly the wire leg. Returns what
// this frame's pump decoded (drives the later phases).
NovaSimulation::JoinerFrameSignals NovaSimulation::joiner_run_client_net_frame(
		uint64_t net_start) {
	const uint32_t now = now_tick_;
	// Run the client net frame first: recv-fold (-> ClientState) + connect-drive + the C2S 0x0C
	// uplink (gated InMatch && deployed inside the runtime). The uplink describes L's pose as left by
	// the previous entity update; this frame's raw-input/motor pass follows all inbound application.
	const uint32_t health_updates_before =
			runtime_->state().health_updates_applied;
	const uint32_t objective_updates_before =
			runtime_->state().objective_updates_applied;
	const bool local_existed_before_net = joiner_local_spawned_;
	std::vector<std::vector<uint8_t>> outs;
	const bool have_L = joiner_local_spawned_ && world_->ai && world_->cached.local_player.valid();
	const opennova::world::Entity *e = have_L ? world_->registry.get(world_->cached.local_player) : nullptr;
	const opennova::world::AiEntity *ae = have_L ? world_->ai->for_handle(world_->cached.local_player) : nullptr;
	// A release can arrive during this recv pump. Do not let that newly-opened
	// runtime gate transmit the corpse/stale pre-deploy pose in the same frame;
	// L resumes uplinking only after the simulation has consumed the release and
	// a later positive authoritative health tail.
	const bool can_offer_uplink =
			runtime_->is_deployed() && e != nullptr && ae != nullptr &&
			e->alive && e->health > 0 && (e->flags & 2u) == 0u;
	if (can_offer_uplink) {
		const opennova::PlayerExtendedUplink up = opennova::netsim::build_player_uplink(*e, *ae);
		outs = runtime_->Client_ProcessNetworkFrame(up, now);
	} else {
		outs = runtime_->Client_ProcessNetworkFrame(now);
	}
	for (const std::vector<uint8_t> &dg : outs) ship_to_host(dg);
	if (runtime_profiling_enabled_)
		last_net_tick_us_ = perf_now_us() - net_start;
	apply_joiner_authoritative_loadout();
	// The team selector may only just have become known (the S2C 0x04 latch landing
	// after the catalog) or may have moved us across the line (S2C 0x50). Either way the
	// resident kit buffer follows the side, exactly as retail re-copies restrictionData
	// for the newly assigned side [orig: NapiNPClientMsg_TeamAssign @0x431a9a].
	if (reseed_session_kit_on_side_change()) push_joiner_loadout_kit();
	// S2C 0x50 re-latched OUR OWN team. L's team is seeded once by spawn_from_self
	// (the join-time 0x0C record's team byte); a later assignment must move both the
	// entity's Team and the round sim's presenting-client team, or friend/foe styling
	// keeps rendering the old side. Retail does the same two writes: byte_A85B48 for
	// the latch and entity->Team for the entity itself.
	// [orig: NapiNPClientMsg_0x050 @0x431910 — @0x4319db / @0x4319ee; the round-spawn
	//  style select reads the local player's Team @0x4ec740]
	{
		const uint64_t self_team_revision = runtime_->self_team_revision();
		if (self_team_revision > joiner_self_team_revision_seen_) {
			joiner_self_team_revision_seen_ = self_team_revision;
			const uint8_t assigned = runtime_->assigned_team();
			if (opennova::world::Entity *L =
					world_->registry.get(world_->cached.local_player)) {
				L->team = assigned;
			}
			world_->round_sim.local_team = assigned;
			// The 0x50 handler also RE-SELECTS the newly assigned side's profile page
			// and re-submits it: profileData = (team==1||team==3) ? blue : red, class
			// = *profileData, page = profileData + {6,2054,4102,6150,8198}
			// [orig: NapiNPClientMsg_TeamAssign @0x431a35..@0x431a9e — the submit
			// passes slot 195 RAW]. The re-submission itself lives in the joiner
			// runtime; re-arm the seam from the NEW side so its content is the page
			// retail would have copied. The resident buffer itself moves with the side
			// in reseed_session_kit_on_side_change above (retail's qmemcpy replaces
			// restrictionData wholesale @0x431a9a) — a reassignment WITHIN one side
			// reaches only this re-arm, which is what retail's unconditional re-submit
			// does too.
			push_joiner_loadout_kit();
		}
	}
	const uint64_t deployment_release_revision =
			runtime_->deployment_release_revision();
	if (deployment_release_revision >
			joiner_deployment_release_revision_seen_) {
		joiner_deployment_release_revision_seen_ =
				deployment_release_revision;
		// The first release creates L below. A later release must revive that
		// existing identity, but only after a positive 0x0A tail observed after
		// this release frame (not a stale positive already queued ahead of it).
		if (local_existed_before_net) {
			joiner_redeploy_release_pending_ = true;
			joiner_redeploy_health_updates_at_release_ =
					runtime_->state().health_updates_applied;
		}
	}
	// Frozen-session tripwire (live-diagnosis aid, ~1 Hz): sample the signature
	// continuously, but print it only when OPENNOVA_NET_DIAGNOSTICS is enabled.
	// An unrecovered S2C sequence gap stalls the ordered frontier, the remote world
	// freezes, retained records stop retiring, and the host eventually reaps us.
	if ((now % 62u) == 0u) {
		const std::size_t gap_depth = runtime_->inbound_gap_depth();
		const std::size_t retained = runtime_->retained_outbound_depth();
		const uint32_t frontier = runtime_->inbound_frontier_seq();
		const uint32_t out_seq = runtime_->outbound_seq();
		const uint32_t records = runtime_->state().compact_records_applied;
		// A true ordered-replication stall requires an unresolved gap. Flat records
		// alone are normal before deployment and whenever the remote world is idle.
		// Continuing outbound sequence movement proves the local socket/frame pump
		// is still alive rather than paused.
		const bool frontier_flat =
				joiner_diagnostic_sampled_ && frontier == joiner_last_frontier_seq_;
		const bool records_flat =
				joiner_diagnostic_sampled_ && records == joiner_last_records_applied_;
		const bool outbound_advanced =
				joiner_diagnostic_sampled_ && out_seq > joiner_last_outbound_seq_;
		const bool stalled_sample =
				runtime_->in_match() && runtime_->is_deployed() && gap_depth > 0 &&
				frontier_flat && records_flat && outbound_advanced;
		if (stalled_sample) ++joiner_flat_seconds_;
		else joiner_flat_seconds_ = 0;
		joiner_freeze_suspected_ = joiner_flat_seconds_ >= 3;
		// L's LIVE pose — the exact source build_player_uplink transmits. A pose that
		// never changes while the player is moving on screen means the local motor is
		// not driving L (so the host renders us frozen at spawn and eventually stops
		// streaming records around a stale reference position).
		if (is_joiner_network_diagnostics_enabled()) {
			String local_state = " L=none";
			if (joiner_local_spawned_ && world_ && world_->ai) {
				const opennova::world::AiEntity *lae =
						world_->ai->for_handle(world_->cached.local_player);
				const opennova::world::Entity *le =
						world_->registry.get(world_->cached.local_player);
				if (lae != nullptr)
					local_state = vformat(
							" L=(%.1f,%.1f,%.1f) hdg=%d input=0x%02x",
							lae->pos[0] / 65536.0f, lae->pos[1] / 65536.0f,
							lae->pos[2] / 65536.0f,
							static_cast<int64_t>(lae->heading >> 16),
							static_cast<int64_t>(le ? le->net_move_input : 0));
			}
			UtilityFunctions::print_verbose(vformat(
					"joiner net: in=%d out=%d rec=%d gap=%d retained=%d match=%d deployed=%d%s%s",
					static_cast<int64_t>(frontier), static_cast<int64_t>(out_seq),
					static_cast<int64_t>(records), static_cast<int64_t>(gap_depth),
					static_cast<int64_t>(retained), runtime_->in_match() ? 1 : 0,
					runtime_->is_deployed() ? 1 : 0, local_state,
					joiner_freeze_suspected_
							? vformat(" *** REPLICATION FROZEN %ds ***",
									  static_cast<int64_t>(joiner_flat_seconds_))
							: String()));
		}
		joiner_last_frontier_seq_ = frontier;
		joiner_last_records_applied_ = records;
		joiner_last_outbound_seq_ = out_seq;
		joiner_last_gap_depth_ = gap_depth;
		joiner_diagnostic_sampled_ = true;
	}
	JoinerFrameSignals decoded;
	decoded.health =
			runtime_->state().health_updates_applied != health_updates_before;
	decoded.objectives =
			runtime_->state().objective_updates_applied != objective_updates_before;
	if (decoded.objectives) {
		const opennova::netsim::ClientState &client = runtime_->state();
		world_->subgoals.won = client.objective_won;
		world_->subgoals.lost = client.objective_lost;
		world_->subgoals.show_win = client.objective_show_win;
		world_->subgoals.show_lose = client.objective_show_lose;
	}
	return decoded;
}

// On the in-match edge (detected by the recv-fold): learn H + spawn L at the host-advertised
// pose. L is the joiner's OWN motor-driven pool-0 entity (publishes cached.local_player); H is the
// wire identity the host knows us by — the two stay distinct, reconciled by the name-match (§5.38b).
void NovaSimulation::joiner_spawn_and_arm_local_player() {
	namespace np = opennova::np;
	if (runtime_->in_match() && !joiner_local_spawned_ && world_->ai) {
		joiner_self_wire_handle_ = runtime_->self_handle();
		const np::JoinerConnection::SelfSpawn &sp = runtime_->spawn_pose();
		const opennova::world::PlayerSpawn spawn = spawn_from_self(sp);
		const opennova::world::EntityHandle h = opennova::world::spawn_player(*world_, spawn);
		joiner_local_spawned_ = h.valid();
		// Arm L the way the host's own spawn does at Player_InitPlayer time: the
		// shell applied the profile kit/class BEFORE L existed (the pre-spawn
		// apply latched it into the inventory), so stamp the deferred class +
		// damage classes + equipped adm on the fresh entity now. [orig:
		// Player_InitPlayer weapon leg @ 0x4e15f0; equippedAdmIndex stamp @ 0x4dd727]
		if (opennova::world::Entity *L = world_->registry.get(h)) {
			if (pending_local_player_class_ >= 5 && pending_local_player_class_ <= 9)
				L->player_class = static_cast<uint8_t>(pending_local_player_class_);
			sync_local_player_damage_classes();
			if (local_inventory_valid_ && local_inventory_.equipped_combo >= 0) {
				const opennova::world::WeaponInventorySlot *slot =
						local_inventory_.slot(local_inventory_.equipped_combo);
				if (slot != nullptr && slot->adm_index >= 0) {
					L->equipped_adm_index = static_cast<uint8_t>(slot->adm_index);
					// Replay the deferred PRESENTATION half of every selection that
					// committed while L did not exist (the 0x5A grant applies before
					// the spawn). Retail has no entity precondition there: the
					// selection restamps equippedAdmIndex and the FP viewmodel is
					// re-resolved per frame off EquippedSlot [orig: the 0x5A tail
					// @0x4296E3 -> Player_SelectWeaponSlot @0x4DD680 @0x4dd727;
					// Player_RenderFirstPersonViewModel @0x4DED60]. Exactly ONE event
					// is queued, naming the weapon the inventory actually selected —
					// without it the viewmodel and the weapon FSM kept running the
					// SUBMITTED weapon while the entity and the wire followed the
					// granted one.
					// weapon_start_in_switchto_ is deliberately left as the last
					// rebuild settled it — the spawn mount plays no switch actions
					// (D-WPN-21), and this replay only restores the notification.
					if (weapon_presentation_pending_) {
						weapon_presentation_pending_ = false;
						const opennova::world::WeaponTableEntry *def =
								world_->weapons.by_index(
										static_cast<uint8_t>(slot->adm_index));
						PendingWeaponEvent event;
						event.tick = world_->logic_tick;
						event.world_position = get_local_player_position();
						event.switch_to_weapon = def != nullptr
								? String::utf8(def->name.c_str())
								: String();
						pending_weapon_events_.push_back(std::move(event));
					}
				}
			}
		}
		resolve_new_infantry_adm_ids();
		player_input_ = opennova::world::PlayerInput{};
		// Retail polls live keys; a press during the join wait must not cross
		// the spawn edge as a queued shot/reload. Clear the consume-latches too.
		weapon_fire_held_ = false;
		weapon_fire_pressed_ = false;
		weapon_reload_pressed_ = false;
		player_input_.look_heading = opennova::world::bam_heading_from_mission_yaw_deg(spawn.yaw);
		stance_latch_ = 0;
		look_px_accum_x_ = look_px_accum_y_ = 0.0f;
	}
}

// The 0x0A tail is the authoritative health source for the recipient's OWN
// player. H belongs to the host's handle space; apply that recipient-local
// scalar to the joiner's distinct motor entity L (the pump gates this phase on
// the frame's decoded health signal). A fresh-frame guard prevents
// ClientState's pre-frame zero default from killing L during the handshake.
// Once L is dead, positive health revives it only after the separate
// ACK-qualified deployment release latched by the net-frame folds, and only
// from a later tail. That edge also snaps L to H's redeployed authoritative
// pose before its next uplink can run.
// [orig: tail health read @0x430428; store to local Health @0x4305df]
void NovaSimulation::joiner_apply_authoritative_health() {
	if (joiner_local_spawned_ &&
			world_->cached.local_player.valid()) {
		const opennova::world::EntityHandle local_h =
				world_->cached.local_player;
		opennova::world::Entity *local =
				world_->registry.get(local_h);
		AiEntity *local_ai =
				world_->ai ? world_->ai->for_handle(local_h) : nullptr;
		if (local != nullptr && local_ai != nullptr) {
			// ClientRuntime latches deployment closed on any decoded zero tail,
			// even if a later packet in this recv pump carries stale positive HP.
			const int16_t health = runtime_->deployed()
					? runtime_->state().local_health : 0;
			if (health <= 0) {
				local->health = health;
				local_ai->health = health;
				local->alive = false;
				local->flags |= 2u;
			} else if (joiner_redeploy_release_pending_ &&
					runtime_->state().health_updates_applied >
							joiner_redeploy_health_updates_at_release_) {
				// The recipient's own compact is deliberately priority-boosted
				// by the host. Use its redeployed pose, not the corpse pose,
				// before movement/uplink resumes.
				const opennova::netsim::ClientEntityState *self =
						client_entity_for_handle(
								runtime_->state(), runtime_->self_handle());
				if (self != nullptr && self->seen_this_frame) {
					const int32_t heading =
							static_cast<int32_t>(
									static_cast<uint32_t>(self->yaw_byte) << 24);
					const int32_t pitch =
							static_cast<int32_t>(
									static_cast<uint32_t>(self->pitch_byte) << 24);
					local->position = {
							static_cast<float>(
									static_cast<double>(self->x) / kFixed16),
							static_cast<float>(
									static_cast<double>(self->y) / kFixed16),
							static_cast<float>(
									static_cast<double>(self->z) / kFixed16),
					};
					local->yaw = static_cast<int16_t>(std::lround(
							opennova::world::mission_yaw_deg_from_bam_heading(
									heading)));
					local->pitch = static_cast<int16_t>(std::lround(
							static_cast<double>(pitch) *
							opennova::world::kDegreesPerBam));
					local->roll = 0;
					local->health = health;
					local->alive = true;
					local->hidden = false;
					local->flags &= ~1u;
					local->death_anim_state = 0;
					local->corpse_timer = 0;
					local->net_move_input = 0;
					local->net_analog_x = 0;
					local->net_analog_y = 0;
					local->net_analog_z = 0;
					opennova::world::entity_reset_to_spawn_state(*local);

					local_ai->pos[0] = self->x;
					local_ai->pos[1] = self->y;
					local_ai->pos[2] = self->z;
					local_ai->heading = heading;
					local_ai->pitch = pitch;
					local_ai->roll = 0;
					local_ai->body_pitch = 0;
					local_ai->health = health;
					local_ai->vel_x = 0;
					local_ai->vel_z = 0;
					local_ai->net_smooth_target[0] = self->x;
					local_ai->net_smooth_target[1] = self->y;
					local_ai->net_smooth_target[2] = self->z;
					local_ai->net_smooth_heading = heading;
					local_ai->net_smooth_pitch = pitch;
					local_ai->net_interp_progress = 0;
					local_ai->net_interp_steps = 0;
					local_ai->collide_state = {};

					opennova::world::InfantryState &inf = local_ai->inf;
					inf.active = true;
					inf.is_local_player = true;
					inf.player_moving = false;
					inf.player_move_dir_index = 0;
					inf.move_mode = 0;
					inf.target_dist = 0;
					inf.reset_body_animation(opennova::world::anim_state::kIdle);
					inf.reload_anim_ticks = 0;
					inf.arms_dip_ticks = 0;
					inf.pitch_kick_accum = 0;
					inf.recoil_pitch = 0;
					inf.weapon_weight_spread = 0;
					inf.aimed_shot_available = false;
					inf.idle_counter = 0;
					inf.lean_left = false;
					inf.lean_right = false;
					inf.lean_angle = 0;
					inf.torso_roll = 0;
					inf.body_heading = heading;
					inf.target_heading = heading;
					inf.leg_yaw[0] = inf.leg_yaw[1] = heading;
					inf.leg_target[0] = inf.leg_target[1] = heading;
					inf.vel[0] = inf.vel[1] = inf.vel[2] = 0;
					inf.stance =
							opennova::world::InfantryState::Stance::kStand;
					inf.standing_on_entity = false;
					inf.airborne = false;
					inf.jump_requested = false;
					inf.jump_cooldown = 0;
					inf.ground_cache_valid = false;

					player_input_ = opennova::world::PlayerInput{};
					player_input_.look_heading = heading;
					weapon_fire_held_ = false;
					weapon_fire_pressed_ = false;
					weapon_reload_pressed_ = false;
					stance_latch_ = 0;
					look_px_accum_x_ = look_px_accum_y_ = 0.0f;
					respawn_local_player_loadout();
					joiner_redeploy_release_pending_ = false;
					joiner_redeploy_health_updates_at_release_ = 0;
				}
			} else if (local->alive && (local->flags & 2u) == 0u) {
				local->health = health;
				local_ai->health = health;
			}
		}
	}
}

NovaSimulation::WireCollisionShape NovaSimulation::wire_collision_shape_for_type(
		uint16_t p_type_id) {
	const auto cached = wire_collision_shape_by_type_.find(p_type_id);
	if (cached != wire_collision_shape_by_type_.end()) return cached->second;
	WireCollisionShape shape;
	if (collision_item_db_.is_valid() && collision_placer_.is_valid()) {
		// The same items.def graphic resolution the registry sweep runs
		// (resolve_collision_instances), keyed by the WIRE type id. Sharing the
		// by-graphic caches means a mission whose local load already registered
		// this graphic reuses the exact model id the ghost had.
		const int def_id =
				static_cast<int>(p_type_id) + opennova::mission::kItemIdOffset;
		const String graphic = collision_item_db_->get_graphic(def_id);
		if (!graphic.is_empty()) {
			const std::string key(graphic.utf8().get_data());
			auto it = collision_model_by_graphic_.find(key);
			if (it == collision_model_by_graphic_.end()) {
				int32_t model_id = -1;
				int32_t occlusion_id = -1;
				float bound_radius = 0.0f;
				Ref<NovaObjectData> data =
						collision_placer_->call("object_data_for", graphic);
				if (data.is_valid()) {
					opennova::world::CollisionModel model;
					if (collision_model_from_ir(data->native_ir().collision, model,
							data->has_collision())) {
						model_id = collision_world_.add_model(std::move(model));
						if (data->has_live_panm_for_lod(0))
							collision_pose_data_[model_id] = data;
					}
					opennova::world::OcclusionModel occ;
					if (occlusion_model_from_ir(data->native_ir().occlusion, occ))
						occlusion_id = occlusion_world_.add_model(std::move(occ));
					bound_radius = model_bound_radius_from_ir(data->native_ir());
				}
				it = collision_model_by_graphic_.emplace(key, model_id).first;
				collision_occlusion_by_graphic_.emplace(key, occlusion_id);
				collision_radius_by_graphic_.emplace(key, bound_radius);
			}
			shape.model_id = it->second;
			shape.bound_radius = collision_radius_by_graphic_[key];
		}
	}
	wire_collision_shape_by_type_.emplace(p_type_id, shape);
	return shape;
}

void NovaSimulation::refresh_joiner_projectile_proxies() {
	std::vector<opennova::world::ProjectilePersonProxy> person_proxies;
	std::vector<opennova::world::ProjectileDynamicProxy> dynamic_proxies;
	uint16_t self_wire_handle = opennova::world::EntityHandle::kInvalid;
	if (runtime_ && runtime_->has_self_handle())
		self_wire_handle = runtime_->self_handle();
	if (joiner_ && runtime_ && runtime_->in_match()) {
		for (const opennova::netsim::ClientEntityState &entity :
				runtime_->state().entities) {
			if (entity.handle == opennova::world::EntityHandle::kInvalid ||
					(self_wire_handle != opennova::world::EntityHandle::kInvalid &&
					 entity.handle == self_wire_handle) ||
					(entity.state_flags_known &&
					 (entity.state_flags & 0x01u) != 0))
				continue;

			// Pool-0 organics (players AND non-player infantry) join the person
			// walk at the decoded position; retail's client walks its wire-built
			// pool 0 the same way [orig: Physics_RaycastAgainstProximityList
			// @ 0x4e4a30 over the client-built person table].
			if (entity.cls == opennova::EntityClass::Player ||
					entity.cls == opennova::EntityClass::Infantry) {
				opennova::world::ProjectilePersonProxy proxy;
				proxy.wire_handle = entity.handle;
				proxy.position_q16 = opennova::world::FixedVec3{
						entity.x, entity.y, entity.z};
				person_proxies.push_back(proxy);
				continue;
			}

			// Pool-1 movers (vehicles, emplacements, runtime items) project
			// their authored collision geometry at the decoded pose. Pool-2
			// statics keep colliding through the locally loaded mission set.
			if (((entity.handle >> 12) & 0xF) != 1) continue;
			const WireCollisionShape shape =
					wire_collision_shape_for_type(entity.type_id);
			if (shape.model_id < 0 && shape.bound_radius <= 0.0f) continue;
			opennova::world::ProjectileDynamicProxy proxy;
			proxy.wire_handle = entity.handle;
			proxy.model_id = shape.model_id;
			proxy.position_q16 = opennova::world::FixedVec3{
					entity.x, entity.y, entity.z};
			// The decoded pose mirrors the retail client entity fields: the compact
			// heading sample plus locally integrated sub-byte body motion, and retained
			// spawn/dead pitch/roll samples (entity+20/+24, live compacts omit
			// both for vehicles).
			proxy.heading_bam = entity.heading_bam;
			proxy.pitch_bam = entity.pitch_bam;
			proxy.roll_bam = entity.roll_bam;
			proxy.bound_radius_q16 = shape.bound_radius > 0.0f
					? static_cast<int32_t>(shape.bound_radius * 65536.0f)
					: 0;
			dynamic_proxies.push_back(proxy);
		}
	}
	// ClientState is persistent and frame-budgeted; omission from one 0x0A is
	// not a despawn signal, so this intentionally does not read seen_this_frame.
	// Known-dead bit 1 is retained too: retail dead bodies remain person blockers
	// and a destroyed vehicle's shell keeps blocking (husk-model substitution for
	// wire proxies is a tracked residual).
	collision_world_.replace_projectile_person_proxies(
			std::move(person_proxies), self_wire_handle);
	collision_world_.replace_projectile_dynamic_proxies(
			std::move(dynamic_proxies));
}

void NovaSimulation::apply_joiner_gameplay_events() {
	if (!joiner_ || !runtime_ || !world_) return;

	// Retail's S2C 0x0A tag-2 record is a fired-round descriptor. Re-run the
	// normal round spawner so tracers and physical impacts are produced locally;
	// World::run_logic_tick admits this pool only under the explicit
	// mp_session && !projectile_authority visual-client gate. Every descriptor
	// is spawned VisualOnly below, and that mode gates every gameplay consequence.
	for (const opennova::netsim::ClientRoundEvent &ev :
			runtime_->drain_round_events()) {
		// The retail deserializer dispatches only the alt/projectile bit or the
		// standard adm-indexed bit [orig: @0x42f2a8]. Other flag shapes do not
		// enter RoundData_SpawnRound.
		if ((ev.flags & 0x03u) == 0) continue;
		const opennova::world::WeaponTableEntry *adm =
				world_->weapons.by_index(ev.adm_index);
		if (adm == nullptr || adm->ammo_index < 0) continue;
		opennova::world::RoundSpawnParams round;
		opennova::world::RoundSourceState source;
		round.owner = opennova::world::EntityHandle{};
		round.shooter_handle = ev.shooter_handle;
		// The mounted shooter's own vehicle joins the trace exclusion exactly
		// like retail's mount rule — see wire_carrier_exclusion_for.
		round.shooter_carrier_handle = wire_carrier_exclusion_for(
				runtime_->state(), ev.shooter_handle, item_seat_specs_);
		// Retail resolves the wire shooter entity and copies its TEAM into the
		// spawned round — the friend/enemy throwable item and tracer styling key
		// on it. Without this every remote grenade wore the enemy variant (and a
		// variant with no motor row froze mid-air). [orig: @0x4ec705]
		if (opennova::netsim::ClientEntityState *shooter_row =
					runtime_->state().find(ev.shooter_handle)) {
			round.shooter_team = shooter_row->team;
			const int anim = shooter_row->anim_state_id;
			// The category follows the retail animation-flags table, not a
			// hand-maintained list of familiar locomotion clips. In particular,
			// 170/171 remain crouched, 172 is prone, and idle_mortar (46) is
			// neither. [orig: g_animStateFlagsTable @0x8139E8; category read in
			// RoundData_SpawnRound @0x4EC252..0x4EC27A]
			const uint32_t anim_flags =
					opennova::world::infantry_anim_flags(anim);
			const bool prone = (anim_flags & 0x200u) != 0;
			const bool crouched = (anim_flags & 0x100u) != 0;
			const bool swimming = anim == 36 || anim == 37 || anim == 154;
			const bool mounted = round.shooter_carrier_handle != 0xFFFF;
			// Retail tests eye Z (Position.Z + CameraOffset.Z) against the fixed
			// water plane [orig: RoundData_SpawnRound @0x4EC2DE..0x4EC2EA]. The
			// decoded row has no CameraOffset carrier, so raw fixed position Z is
			// the bounded projection; the swimming anim remains an independent
			// positive witness. Do not invent a standing-eye constant here.
			const bool below_water = world_->env.water_z != 0 &&
					shooter_row->z < world_->env.water_z;
			source.person_with_item_def =
					shooter_row->cls == opennova::EntityClass::Player ||
					shooter_row->cls == opennova::EntityClass::Infantry;
			source.player = shooter_row->cls == opennova::EntityClass::Player;
			source.underwater = swimming || below_water;
			// Mounted is the later retail override and therefore wins even while
			// below water; otherwise the underwater predicate forces standing row 2.
			source.stance_category = mounted ? 1 : (source.underwater ? 2 :
					(prone ? 0 : (crouched ? 1 : 2)));
			source.scope_raised =
					(shooter_row->state_flags &
					 opennova::world::kEntityFlagScopeRaised) != 0;
			source.recoil_pitch = &shooter_row->recoil_pitch;
			round.source_state = &source;
		}
		round.origin.x = static_cast<float>(ev.origin_x) / kFixed16;
		round.origin.y = static_cast<float>(ev.origin_y) / kFixed16;
		round.origin.z = static_cast<float>(ev.origin_z) / kFixed16;
		round.dir_yaw_bam = ev.dir_yaw_bam;
		round.dir_pitch_bam = ev.dir_pitch_bam;
		round.ammo_index = adm->ammo_index;
		round.adm_index = ev.adm_index;
		round.shot_seq = ev.shot_seq;
		round.subtype = ev.subtype;
		round.charge = ev.slot_byte;
		// Carry the arm through so presentation can honour retail's split: the
		// adm-indexed arm spawns no ammo-def effect at the wire position (which is
		// the shooter's EYE — Position + CameraOffset), it executes the addressed
		// def's action rows at the weapon's own userpoint instead.
		// [orig: @0x42f521 / @0x42f6ce]
		round.wire_round_flags = static_cast<uint8_t>(ev.flags & 0x03u);
		world_->round_sim.spawn(
				*world_, round,
				opennova::world::RoundConsequenceMode::VisualOnly);
	}

	for (const opennova::WeaponReload &reload :
			runtime_->drain_reload_notifications()) {
		++weapon_reload_received_serial_;
		weapon_reload_received_entity_ = reload.entity_handle;
		weapon_reload_received_param_ = reload.reload_param;
		// The REMOTE branch. Retail's 0x49 handler splits on the addressed entity's
		// item type: a remote PERSON gets the arms-dip stamp and nothing else — it
		// returns before the ammo refill, so the peer never enters the 65/66 reload
		// pose on a pure client. Our rows carry an EntityClass rather than an ItemDef
		// type, so Player/Infantry stands in for ItemType_Person (3); the same
		// analogue tick_lean already uses. Retail's NON-person remote branch (the real
		// refill on a vehicle/emplaced weapon) is deliberately not ported here.
		// [orig: NapiNPClientMsg_WeaponReload_0x049 @0x42c0a0 — type test @0x42c105,
		//  stamp entity+0x371 = 80 @0x42c10b, early return @0x42c113]
		if (!runtime_->has_self_handle() ||
				reload.entity_handle != runtime_->self_handle()) {
			opennova::netsim::ClientEntityState *peer =
					runtime_->state().find(reload.entity_handle);
			if (peer != nullptr &&
					(peer->cls == opennova::EntityClass::Player ||
							peer->cls == opennova::EntityClass::Infantry))
				peer->arms_dip_ticks = 80;
		}
		if (!local_inventory_valid_ || !runtime_->has_self_handle() ||
				reload.entity_handle != runtime_->self_handle())
			continue;

		const int32_t combo = reload.reload_param;
		opennova::world::WeaponInventorySlot *reloaded =
				local_inventory_.slot(combo);
		const opennova::world::WeaponTableEntry *def =
				(reloaded != nullptr && reloaded->adm_index >= 0)
						? world_->weapons.by_index(
								static_cast<uint8_t>(reloaded->adm_index))
						: nullptr;
		if (reloaded == nullptr || def == nullptr) continue;

		// WeaponSlot_ReloadAmmo is run only on the echoed notification: refund
		// the payload-addressed slot's remaining clip to its ammo-class pool and
		// draw a full clip. A late echo still reloads that exact slot after a switch;
		// only the currently active personal slot has an FSM mirror to update here.
		opennova::world::weapon_inventory_reload_slot(
				world_->weapons, local_inventory_, combo);
		if (weapon_active_ && !local_usegun_slot_active_ &&
				local_inventory_.equipped_combo >= 0) {
			opennova::world::WeaponSlotState &active_slot =
					*active_local_weapon_slot();
			const opennova::world::WeaponInventorySlot *equipped =
					local_inventory_.slot(local_inventory_.equipped_combo);
			const opennova::world::WeaponTableEntry *equipped_def =
					(equipped != nullptr && equipped->adm_index >= 0)
							? world_->weapons.by_index(
									static_cast<uint8_t>(equipped->adm_index))
							: nullptr;
			if (equipped_def != nullptr) {
				active_slot.reserve = opennova::world::weapon_pool_get(
						local_inventory_, equipped_def->ammo_class_id);
			}
			if (combo == local_inventory_.equipped_combo) {
				active_slot.clip = reloaded->clip;
				active_slot.phase = static_cast<uint8_t>(
						active_slot.phase &
						~opennova::world::weapon_phase::kReloadPendingBit);
			}
		}
		++weapon_reload_applied_serial_;
		AiEntity *player = world_->ai
				? world_->ai->for_handle(world_->cached.local_player)
				: nullptr;
		if (player != nullptr && player->inf.active)
			player->inf.reload_anim_ticks = 80;
	}

	// Decoded shots above stamp the peer accumulator first; retail's client body
	// update then decays it in this same frame. Locally predicted fire is pumped
	// later, after the local World body tick, so it begins decaying next frame.
	runtime_->tick_remote_recoil();
}

void NovaSimulation::enable_listen_server(bool p_enable) {
	listen_server_ = p_enable;
	// P7: the SP listen server now rides the npruntime in-match runtime (ctx_ / host_loop_ /
	// runtime_), stood up per-load in bringup_host_runtime — there is no net ISystem and
	// no legacy loopback seam here. (The LAN host still builds the legacy seam in enable_host_listen
	// until A3; a sim is SP listen XOR LAN host XOR joiner.)
}

void NovaSimulation::set_terrain_til_data(const PackedByteArray &p_til_bytes) {
	terrain_til_data_.assign(p_til_bytes.ptr(), p_til_bytes.ptr() + p_til_bytes.size());
}

bool NovaSimulation::enable_host_listen(int p_port) {
	listen_server_ = true;
	if (pump_.is_null()) pump_.instantiate();
	if (pump_->bind_listen(p_port) != 0) {
		host_listen_ = false;
		return false;
	}
	host_listen_ = true;
	if (world_) {
		world_->projectile_authority = true;
		world_->mp_session = true;
	}
	// P7: the LAN host rides the npruntime runtime (ctx_ over a real UDP socket), stood up per-load in
	// bringup_host_runtime with SocketMode::Lan. NovaUdpPump owns the socket; all protocol/crypto/
	// framing stays in libs (ADR 0010). host_session_config_ keeps the GDScript-facing session options
	// (the Dictionary getter + the §5.1 reactive-reply config fed to configure_session_runtime).
	host_bind_port_ = static_cast<uint16_t>(std::clamp(p_port, 0, 0xFFFF));
	return true;
}

int NovaSimulation::get_host_listen_port() const {
	return (host_listen_ && pump_.is_valid()) ? pump_->local_port() : 0;
}

int NovaSimulation::get_host_peer_count() const {
	// Count the type-1 (remote-joiner) connections in the npruntime table. The host's own type-2
	// loopback is excluded; a pre-Hello garbage datagram registers no node (handle_server_datagram
	// drops bad envelopes), so it stays 0 until a real JointOperations peer handshakes.
	int n = 0;
	for (const opennova::np::NapiNPConnection &c : ctx_.np_protocol.connection_list) {
		if (c.type == 1) ++n;
	}
	return n;
}

void NovaSimulation::configure_host_session(Dictionary p_options) {
	opennova::np::GameConfig config = host_session_config_;
	host_bind_port_ = dictionary_u16(p_options, "bind_port", host_bind_port_);
	apply_dictionary_string(p_options, "server_name", config.server_name);
	apply_dictionary_string(p_options, "mission_name", config.mission_name);
	apply_dictionary_string(p_options, "mission_file", config.mission_file);
	apply_dictionary_string(p_options, "player_name", config.player_name);
	apply_dictionary_string(p_options, "expansion", config.expansion);
	if (p_options.has("gametype")) {
		config.game_type = dictionary_u32(p_options, "gametype", config.game_type);
	} else if (p_options.has("game_type")) {
		config.game_type = dictionary_u32(p_options, "game_type", config.game_type);
	}
	config.mp_attributes = dictionary_u32(p_options, "mpattrib", config.mp_attributes);
	if (p_options.has("fat_bullets"))
		config.fat_bullets = static_cast<bool>(p_options["fat_bullets"]);
	if (p_options.has("one_shot_kill"))
		config.one_shot_kill = static_cast<bool>(p_options["one_shot_kill"]);
	if (p_options.has("spawn_x") || p_options.has("spawn_y") || p_options.has("spawn_z")) {
		config.spawn_x = dictionary_u32(p_options, "spawn_x", config.spawn_x);
		config.spawn_y = dictionary_u32(p_options, "spawn_y", config.spawn_y);
		config.spawn_z = dictionary_u32(p_options, "spawn_z", config.spawn_z);
	}
	if (p_options.has("spawn_names")) {
		config.spawn_names.clear();
		const Variant names_v = p_options.get("spawn_names", Array());
		if (names_v.get_type() == Variant::ARRAY) {
			const Array names = names_v;
			for (int64_t i = 0; i < names.size(); ++i) {
				const String name = names[i];
				if (!name.is_empty()) {
					config.spawn_names.emplace_back(name.utf8().get_data());
				}
			}
		}
	}
	// Server type + player cap (UI host config): serve_and_play gates the host's own-player spawn +
	// loopback fold at bring-up; max_players is the lobby-advertised cap, clamped to the witnessed 1..65.
	if (p_options.has("serve_and_play")) {
		host_serve_and_play_ = static_cast<bool>(p_options["serve_and_play"]);
	}
	if (p_options.has("max_players")) {
		uint32_t mp = dictionary_u32(p_options, "max_players", host_max_players_);
		if (mp < 1u) {
			mp = 1u;
		} else if (mp > 65u) {
			mp = 65u;
		}
		host_max_players_ = mp;
	}
	host_session_config_ = std::move(config);
	if (world_ && host_listen_) {
		world_->fat_bullets = host_session_config_.fat_bullets;
		world_->one_shot_kill = host_session_config_.one_shot_kill;
	}
}

Dictionary NovaSimulation::get_host_session_config() const {
	const opennova::np::GameConfig &session = host_session_config_;
	Dictionary out;
	out["bind_port"] = static_cast<int>(host_bind_port_);
	out["server_name"] = String(session.server_name.c_str());
	out["mission_name"] = String(session.mission_name.c_str());
	out["mission_file"] = String(session.mission_file.c_str());
	out["player_name"] = String(session.player_name.c_str());
	out["expansion"] = String(session.expansion.c_str());
	out["gametype"] = static_cast<int64_t>(session.game_type);
	out["mpattrib"] = static_cast<int64_t>(session.mp_attributes);
	// UI host-config values held on the sim (not in GameConfig): the lobby player
	// cap and the serve-and-play/dedicated selector, for the F3 Net tab.
	out["max_players"] = static_cast<int64_t>(host_max_players_);
	out["serve_and_play"] = host_serve_and_play_;
	out["fat_bullets"] = session.fat_bullets;
	out["one_shot_kill"] = session.one_shot_kill;
	out["spawn_x"] = static_cast<int64_t>(session.spawn_x);
	out["spawn_y"] = static_cast<int64_t>(session.spawn_y);
	out["spawn_z"] = static_cast<int64_t>(session.spawn_z);
	out["mission_header_size"] = static_cast<int64_t>(session.mission_header_blob.size());
	Array spawn_names;
	for (const std::string &name : session.spawn_names) {
		spawn_names.push_back(String(name.c_str()));
	}
	out["spawn_names"] = spawn_names;
	return out;
}

void NovaSimulation::set_join_character_profile(const Dictionary &p_profile) {
	const Array ids = p_profile.get("character_ids", Array());
	const Array classes = p_profile.get("player_classes", Array());
	const Array avatars = p_profile.get("avatars", Array());
	if (ids.size() < 2 || classes.size() < 2 || avatars.size() < 2) return;

	opennova::np::CharacterJoinVars vars{};
	for (int side = 0; side < 2; ++side) {
		vars.char_id[side] = static_cast<uint16_t>(std::clamp(
				static_cast<int>(ids[side]), 0, 0xFFFF));
		vars.char_class[side] = static_cast<uint8_t>(std::clamp(
				static_cast<int>(classes[side]), 0, 0xFF));
		vars.avatar[side] = static_cast<uint8_t>(std::clamp(
				static_cast<int>(avatars[side]), 0, 0xFF));
	}
	const int requested_team =
			static_cast<int>(p_profile.get("team_request", -1));
	vars.team_request =
			(requested_team == 0 || requested_team == 1)
			? static_cast<uint8_t>(requested_team)
			: 0xFF;
	join_character_vars_ = vars;
	join_character_vars_set_ = true;
	install_character_join_vars();
}

// ---- co-op LAN joiner (D.2) -------------------------------------------------

bool NovaSimulation::enable_join(const String &p_host_ip, int p_port, const String &p_player_name) {
	// P7: the joiner is a non-authority np::ClientRuntime (Joiner role) built per-load in finish_load;
	// it owns the connect-leg state machine + the S2C->ClientState fold internally. Here we only dial
	// the socket + store the player name (the ClientAuth.NA the host echoes for the name-match). Leave
	// listen_server_ false (the present gate adds || joiner_); a sim is host XOR joiner.
	if (pump_.is_null()) pump_.instantiate();
	if (pump_->dial(p_host_ip, p_port) != 0) {
		joiner_ = false;
		return false;
	}
	joiner_player_name_ = std::string(p_player_name.utf8().get_data());
	// Build the Joiner runtime now so get_joiner_phase reads Idle before the first load (the contract
	// the legacy joiner_session_ held); finish_load rebuilds it fresh on each (re)load.
	runtime_ = std::make_unique<opennova::np::ClientRuntime>(joiner_player_name_);
	install_charattr_challenge_table();
	install_character_join_vars();
	install_item_class_resolver();
	joiner_ = true;
	if (world_) {
		world_->projectile_authority = false;
		world_->mp_session = true;
	}
	joiner_started_ = false;
	joiner_local_spawned_ = false;
	joiner_deployment_release_revision_seen_ = 0;
	joiner_redeploy_release_pending_ = false;
	joiner_redeploy_health_updates_at_release_ = 0;
	joiner_self_wire_handle_ = 0;
	joiner_applied_loadout_revision_ = 0;
	joiner_last_gap_depth_ = 0;
	joiner_last_frontier_seq_ = 0;
	joiner_last_records_applied_ = 0;
	joiner_last_outbound_seq_ = 0;
	joiner_diagnostic_sampled_ = false;
	joiner_flat_seconds_ = 0;
	joiner_freeze_suspected_ = false;
	return true;
}

bool NovaSimulation::load_charattr_challenge(
		const Ref<NovaResourceRoot> &p_resource_root) {
	// Game_Run clears all 0x7C0 bytes before attempting the boot-soft load.
	// Preserve that failure result: missing/empty input is not replaced with a
	// synthetic row, and joining continues with the checksum's inactive zero.
	charattr_challenge_table_ = {};
	charattr_challenge_loaded_ = false;
	if (p_resource_root.is_valid() &&
	    p_resource_root->has_file("charattr.def")) {
		const PackedByteArray bytes =
				p_resource_root->read_file("charattr.def");
		if (!bytes.is_empty()) {
			charattr_challenge_loaded_ =
					opennova::np::parse_charattr_challenge_table(
							bytes.ptr(),
							static_cast<std::size_t>(bytes.size()),
							charattr_challenge_table_);
		}
	}
	install_charattr_challenge_table();
	return charattr_challenge_loaded_;
}

void NovaSimulation::set_join_world_ready(bool p_ready) {
	if (joiner_ && runtime_) runtime_->set_world_ready(p_ready);
}

void NovaSimulation::finalize_loaded_model_challenge_snapshot() {
	if (!joiner_ || !runtime_) return;
	const int64_t count = NovaObjectData::network_challenge_model_count();
	if (count <= 0) {
		runtime_->set_loaded_model_challenge_snapshot({});
		return;
	}
	// NetPacket_WriteEntityIndexList writes ONE dword per frozen model-def row —
	// a 5x-unrolled loop capped at 50 entries after the [u32 startIndex] header
	// [orig: @0x42d950; the per-entry store @0x42d9f0, the 0x32 cap @0x42da7d].
	// The complete writer/xref audit for this binary found no surviving writer to
	// the source field after model resolution, so each included definition
	// contributes one zero dword. Keep the npruntime seam value-based so a future
	// witnessed writer can supply its actual row without changing paging.
	runtime_->set_loaded_model_challenge_snapshot(
			std::vector<uint32_t>(static_cast<std::size_t>(count), 0u));
}

bool NovaSimulation::poll_join_preload() {
	if (!joiner_ || !runtime_) return false;

	// This is the pre-mission subset of joiner_pump: the same UDP socket and
	// ClientRuntime advance the retail connect exchange, but no World exists yet
	// to tick and the runtime's world-ready gate suppresses the load/spawn drive.
	if (!joiner_started_) {
		const std::vector<uint8_t> hello = runtime_->start();
		if (!hello.empty()) ship_to_host(hello);
		joiner_started_ = true;
	}
	if (pump_.is_valid()) {
		pump_->poll();
		while (pump_->has_inbound()) {
			const Dictionary d = pump_->take_inbound();
			const PackedByteArray bytes = d.get("bytes", PackedByteArray());
			runtime_->receive(bytes.ptr(), static_cast<std::size_t>(bytes.size()));
		}
	}
	for (const std::vector<uint8_t> &dg :
			runtime_->Client_ProcessNetworkFrame(now_tick_)) {
		ship_to_host(dg);
	}
	++now_tick_;
	return true;
}

bool NovaSimulation::is_join_preload_ready() const {
	return joiner_ && runtime_ && runtime_->preload_ready();
}

String NovaSimulation::get_join_admission_stage() const {
	if (!joiner_ || !runtime_) {
		return String();
	}
	return String(runtime_->admission_stage_name());
}

bool NovaSimulation::has_join_mission() const {
	return joiner_ && runtime_ && runtime_->mission_known();
}

// String::utf8, not the Latin-1 const char* constructor: the LAN browser rows
// decode the same wire fields as UTF-8, and both presentations of one host's
// metadata must agree byte-for-byte.
String NovaSimulation::get_join_server_name() const {
	return (joiner_ && runtime_) ? String::utf8(runtime_->server_name().c_str()) : String();
}

String NovaSimulation::get_join_mission_name() const {
	return (joiner_ && runtime_) ? String::utf8(runtime_->mission_name().c_str()) : String();
}

String NovaSimulation::get_join_mission_file() const {
	return (joiner_ && runtime_) ? String::utf8(runtime_->map_file().c_str()) : String();
}

String NovaSimulation::get_join_expansion() const {
	return (joiner_ && runtime_) ? String::utf8(runtime_->expansion().c_str()) : String();
}

int64_t NovaSimulation::get_join_game_type() const {
	return (joiner_ && runtime_) ? static_cast<int64_t>(runtime_->game_type()) : -1;
}

String NovaSimulation::get_join_error() const {
	return (joiner_ && runtime_) ? String(runtime_->last_error().c_str()) : String();
}

// The in-match analog of get_join_error: the host closed the session on its own terms
// (the punt record), or an established session went silent past the witnessed connection
// reap window. Either way the disconnect event maps a reason code onto g_mission_exit_reason
// — retail EXITS THE MISSION with a reason rather than raising an in-world dialog, so the
// shell's analog is return-to-menu with the reason surfaced.
// [orig: CNapiNetwork_Init @0x4ca4a0 (timeout stores @0x4caa81/@0x4cab54) and
//  CNapiNPConnection_HandleDescriptionPacket @0x621ae0, both ->
//  CNapiNetwork_OnDisconnectedFromServer @0x4c63d0]
String NovaSimulation::get_session_loss_reason() const {
	if (!joiner_ || !runtime_) return String();
	return String::utf8(runtime_->session_loss_reason().c_str());
}

bool NovaSimulation::is_session_lost() const {
	return joiner_ && runtime_ && runtime_->session_lost();
}

bool NovaSimulation::is_joined_in_match() const {
	return joiner_ && runtime_ && runtime_->in_match();
}

bool NovaSimulation::is_joiner_network_diagnostics_enabled() const {
	return environment_flag_enabled(kJoinerNetDiagnosticsEnv);
}

Dictionary NovaSimulation::get_joiner_network_diagnostics() const {
	Dictionary out;
	out["enabled"] = is_joiner_network_diagnostics_enabled();
	out["frontier_seq"] = static_cast<int64_t>(
			runtime_ ? runtime_->inbound_frontier_seq() : 0u);
	out["outbound_seq"] = static_cast<int64_t>(
			runtime_ ? runtime_->outbound_seq() : 0u);
	out["records_applied"] = static_cast<int64_t>(
			runtime_ ? runtime_->state().compact_records_applied : 0u);
	out["gap_depth"] = static_cast<int64_t>(
			runtime_ ? runtime_->inbound_gap_depth() : 0u);
	out["retained_outbound"] = static_cast<int64_t>(
			runtime_ ? runtime_->retained_outbound_depth() : 0u);
	out["flat_seconds"] = joiner_flat_seconds_;
	out["freeze_suspected"] = joiner_freeze_suspected_;
	out["in_match"] = runtime_ && runtime_->in_match();
	out["deployed"] = runtime_ && runtime_->is_deployed();
	return out;
}

bool NovaSimulation::is_join_deploy_pick_pending() const {
	return joiner_ && runtime_ && runtime_->deployment_pick_pending();
}

int NovaSimulation::get_join_assigned_team() const {
	return joiner_ && runtime_ ? runtime_->assigned_team() : 0;
}

const opennova::world::SpawnZoneRegistry &NovaSimulation::deploy_zone_registry() {
	// Built once per load; the zone set is authored (the world stream upserts state,
	// not membership). finish_load resets the flag. [orig: Entity_BuildSpawnZoneList
	// @0x43EAE0 — rebuilt at mission start]
	if (!deploy_zone_registry_built_ && world_) {
		deploy_zone_registry_ = opennova::world::build_spawn_zone_list(*world_);
		deploy_zone_registry_built_ = true;
	}
	return deploy_zone_registry_;
}

TypedArray<Dictionary> NovaSimulation::get_deploy_spawn_zones() {
	// The DEATH screen's zone rows [orig: UI_UpdateDeathScreenContent @0x5536a0 —
	// def present, team match, SECURED (a numbered zone lists only at full control:
	// the zone-timer EntryById[9] >= [10] gate), attrib 0x40000; letter = 'A' +
	// registry index, name = WPNames/STRWPNAME%03d(index+1)]. The local BMS owns
	// membership/letter identity; live S2C 0x6F/0x53 owns team + control.
	TypedArray<Dictionary> rows;
	if (!world_ || !joiner_ || !runtime_) return rows;
	const opennova::world::SpawnZoneRegistry &reg = deploy_zone_registry();
	const uint8_t team = runtime_->assigned_team();
	for (size_t i = 0; i < reg.entries.size(); ++i) {
		const opennova::world::Entity *e = world_->registry.get(reg.entries[i]);
		if (e == nullptr || !e->has_item_def || !e->is_spawn_point) continue;
		uint8_t effective_team = e->team;
		int32_t effective_control = e->zone_control;
		int32_t effective_limit = 0x10000;
		const auto live = runtime_->zone_states().find(e->handle.packed);
		if (live != runtime_->zone_states().end()) {
			// The DEATH list reads the value entry's team and exact value >= limit
			// gate. 0x53 is the separate timed-capture window; retaining an old
			// window after a later 0x6F must not overwrite this ownership channel.
			// [orig: UI_UpdateDeathScreenContent @0x5536a0; §5.49/§5.61]
			if (live->second.has_value) {
				effective_team = live->second.value.mode;
				effective_control = live->second.value.value_s;
				effective_limit = live->second.value.limit_s;
			}
		}
		if (effective_team != team) continue;
		if (e->zone_number != 0 && effective_control < effective_limit) continue;
		Dictionary row;
		row["param"] = static_cast<int>(i) + 1;
		row["letter"] = String::chr('A' + static_cast<int>(i));
		row["name_key"] = vformat("STRWPNAME%03d", static_cast<int>(i) + 1);
		rows.push_back(row);
	}
	return rows;
}

bool NovaSimulation::send_deployment_pick(int p_param) {
	// [orig: Input_HandleActionBinding case 12 @0x49b0c5-0x49b17b — param 0 -> 0xFFFF,
	// 65534 -> 0xFFFE, else SpawnZoneList_GetByIndex(param-1) -> the entity handle;
	// an index that resolves no entity falls through to 0xFFFF @0x49b17b LABEL_70]
	if (!joiner_ || !runtime_) return false;
	uint16_t wire = 0xFFFF;
	if (p_param == 65534) {
		wire = 0xFFFE;
	} else if (p_param > 0) {
		const opennova::world::SpawnZoneRegistry &reg = deploy_zone_registry();
		const size_t idx = static_cast<size_t>(p_param - 1);
		if (idx < reg.entries.size()) wire = reg.entries[idx].packed;
	}
	runtime_->queue_deployment_pick(wire);
	return true;
}

int NovaSimulation::get_joiner_phase() const {
	return (joiner_ && runtime_) ? static_cast<int>(runtime_->phase()) : -1;
}

int NovaSimulation::get_joiner_self_handle() const {
	return joiner_ ? static_cast<int>(joiner_self_wire_handle_) : 0;
}

// [orig: CNapiNPConnection_TeardownActiveConnection @0x6253c0 — the leave sends a burst of
// 0x46 disconnect packets (SendDisconnectPacket @0x61f2a0) before the key material clears; the
// host's non-timeout teardown fires only on that opcode (Nwu_HandleClientGoodbye @0x624250)]
void NovaSimulation::leave_net_session() {
	if (!joiner_ || runtime_ == nullptr) return;
	for (const std::vector<uint8_t> &dg : runtime_->disconnect()) ship_to_host(dg);
}


void NovaSimulation::ship_to_host(const std::vector<uint8_t> &dg) {
	if (pump_.is_null() || dg.empty()) return;
	PackedByteArray bytes;
	bytes.resize(static_cast<int64_t>(dg.size()));
	std::memcpy(bytes.ptrw(), dg.data(), dg.size());
	pump_->send_to_host(bytes);
}

opennova::world::PlayerSpawn NovaSimulation::spawn_from_self(
		const opennova::np::JoinerConnection::SelfSpawn &s) const {
	opennova::world::PlayerSpawn spawn;
	// SelfSpawn position is mission i32 16.16; PlayerSpawn.position is float mission units.
	spawn.position = {static_cast<float>(s.pos_x / kFixed16),
	                  static_cast<float>(s.pos_y / kFixed16),
	                  static_cast<float>(s.pos_z / kFixed16)};
	// orientation is ALREADY a full 32-bit BAM (unlike HostJoinerPose.heading, an i16
	// the host shifts << 16) -> pass it straight to the (90 - heading) mission-degree map.
	spawn.yaw = static_cast<int16_t>(
			std::lround(opennova::world::mission_yaw_deg_from_bam_heading(s.orientation)));
	spawn.team = s.team;
	// The named player record carries the host-stamped character selector and
	// packed minimap/character id. Preserve both on local L just as retail's
	// Player_InitPlayer does; dropping animSlot recreated the D-NET-146
	// DBuggy-shadow association on the joiner's own presentation.
	spawn.anim_slot = s.anim_slot;
	spawn.minimap_net_id = s.net_id;
	// [D-NET-112] The packed id belongs to the wire NetId ONLY. A player carries no SSN, so L
	// takes net_id 0 exactly like the host's own spawn (server_spawn.cpp) — that keeps it out of
	// the WAC/BMS find_by_net_id space, which PlayerSpawn.net_id's 0xFFF0 default would otherwise
	// join, so the zero must be written explicitly. [orig: Server_PlayerAdd @0x51cbc0 slot+440 ->
	// entity+0x15C (the wire NetId); EntityPool_FindByNetId @0x4f0a20 keys entity+0x7C, left 0
	// for players]
	spawn.net_id = 0;
	return spawn;
}

// (P7 A4: joiner_net_poll / joiner_net_flush deleted — the joiner now runs through joiner_pump
//  over an np::ClientRuntime; the legacy JoinerSession path is retired here.)

bool NovaSimulation::admit_test_remote_peer(Vector3 p_position, float p_yaw_deg, int p_team) {
	if (!host_listen_ || !world_ || !world_->ai) return false;
	opennova::world::PlayerSpawn spawn;
	// Godot (x,y,z) -> mission (x,-z,y), the inverse of the present remap (same as spawn_local_player).
	spawn.position = {static_cast<float>(p_position.x), static_cast<float>(-p_position.z),
	                  static_cast<float>(p_position.y)};
	spawn.yaw = static_cast<int16_t>(p_yaw_deg);
	spawn.team = static_cast<uint8_t>(p_team);
	// A synthetic loopback peer; distinct port per call so repeated admits don't alias. Own a transport
	// so the connection is well-formed. The synthetic admit (no handshake) mirrors the post-PeerSpawned
	// state; with the host already at net_id 0xFFF0 the joiner allocates 0xFFF1.
	const opennova::PeerAddr peer{0x0100007Fu, static_cast<uint16_t>(40000 + host_owner_.peers.size())};
	opennova::np::PeerLink &link = host_owner_.peers[peer];
	if (!link.transport) {
		link.transport = std::make_unique<opennova::netsim::UdpSessionTransport>(
				opennova::netsim::UdpSessionTransport::Role::Host);
	}
	const opennova::world::EntityHandle h =
			opennova::np::admit_synthetic_peer(
					ctx_, *world_, peer, spawn, link.transport.get());
	resolve_new_infantry_adm_ids();
	return h.valid();
}
