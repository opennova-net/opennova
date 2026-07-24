// NovaSimulation — the net roles (ADR 0009/0011/0012): per-load listen-host
// bring-up + host pump, the LAN joiner pump family + wire proxies/events, the
// host session config FFI, and the joiner preload/session API.
#include "simulation/nova_simulation_internal.h"

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
	// Serve-and-play (default) vs dedicated. SP / editor preview are ALWAYS serve-and-play (they render
	// the host's own player); a LAN host honors the UI server-type (host_serve_and_play_, from
	// configure_host_session). A dedicated host (serve_and_play=false) skips the own-player spawn + the
	// local view below and lets host_session_pump discard the host loopback (step 5) — mirroring
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
		// read @0x555940, dispatch @0x556d00, mode switch @0x4c49f0]. We instead keep the single mode-3
		// listen-server path (ADR 0011) with serve_and_play=false — wire-equivalent to the joiner (mode 1
		// vs 3 changes only the host's OWN client bookkeeping, never the S2C stream a peer receives),
		// tracked as a divergence (docs/net/novaworld-net-re.md, D-NET-131). No host player spawns (a slot
		// fills lazily via tick_connections if a peer needs it) and host_session_pump discards the host
		// loopback (step 5): there is no local view, so runtime_ stays null — host_pump's fold and the
		// present snapshot both guard on it.
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
		char ipbuf[32];
		std::snprintf(ipbuf, sizeof(ipbuf), "%u.%u.%u.%u", to.ip & 0xFFu, (to.ip >> 8) & 0xFFu,
		              (to.ip >> 16) & 0xFFu, (to.ip >> 24) & 0xFFu);
		PackedByteArray bytes;
		bytes.resize(static_cast<int64_t>(len));
		std::memcpy(bytes.ptrw(), data, len);
		pump_->send_to(String(ipbuf), to.port, bytes);
	}

private:
	NovaUdpPump *pump_;
};
} // namespace

// P7/A5: the per-frame host owner loop is now a THIN delegation to the shared core host_session_pump
// (libs/npruntime) — the SAME loop apps/nw_server runs, so the headless server and the Godot host can no
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
		if (dg.tag != 0x25) {
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
	if (runtime_) runtime_->Client_ProcessNetworkFrame(now); // fold host_loop_ -> ClientState (HostClient view)
}

// host_pump's dispatch_event + admit_peer were promoted into libs/npruntime (np::dispatch_event /
// np::admit_peer over host_owner_, driven by host_session_pump) — the SAME code apps/nw_server runs, so
// the Godot host and the headless server can no longer drift.

// P7: the per-frame non-authority client loop — the Godot equivalent of the joiner half of
// Client_ProcessNetworkFrame (§5.44). The recv-fold + the C2S 0x0C uplink are fused inside the
// runtime. Retail dispatches received messages before the entity/weapon-action pumps, so decoded
// consequences are applied to L before this frame's local World tick. In particular, an S2C 0x49
// arriving on a reload's DONE boundary must refill the slot before IDLE can observe the stale empty
// magazine and queue a second C2S 0x25. [orig: Game_ProcessMainFrame @0x5263f0;
// Client_ProcessNetworkFrame @0x42c180]
void NovaSimulation::joiner_pump() {
	if (!runtime_) return;
	joiner_send_hello_once();
	joiner_deposit_inbound();
	// Retail dispatches received messages before the entity/weapon-action pumps,
	// so decoded consequences are applied to L before this frame's local World
	// tick. In particular, an S2C 0x49 arriving on a reload's DONE boundary must
	// refill the slot before IDLE can observe the stale empty magazine and queue
	// a second C2S 0x25. [orig: Game_ProcessMainFrame @0x5263f0;
	// Client_ProcessNetworkFrame @0x42c180]
	const JoinerFrameSignals decoded = joiner_run_client_net_frame();
	joiner_spawn_and_arm_local_player();
	if (decoded.health) joiner_apply_authoritative_health();

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
	// (the world reveals at local-load completion, D-LOADSCR-3) discharged the
	// pre-armed FSM with no shooter and the joiner deployed a round short.
	// [orig: WeaponAction_ProcessAllEntities @ 0x526786]
	if (joiner_local_spawned_) tick_local_player_weapon();
	++now_tick_;
}

// ClientHello once (Idle -> Hello) the first armed frame.
void NovaSimulation::joiner_send_hello_once() {
	if (joiner_started_) return;
	const std::vector<uint8_t> hello = runtime_->start();
	if (!hello.empty()) ship_to_host(hello);
	joiner_started_ = true;
}

// Deposit received framed datagrams for this frame's recv pump.
void NovaSimulation::joiner_deposit_inbound() {
	if (!pump_.is_valid()) return;
	pump_->poll();
	while (pump_->has_inbound()) {
		const Dictionary d = pump_->take_inbound();
		const PackedByteArray bytes = d.get("bytes", PackedByteArray());
		runtime_->receive(bytes.ptr(), static_cast<std::size_t>(bytes.size()));
	}
}

// Run the client net frame: recv-fold (-> ClientState) + connect-drive + the C2S 0x0C
// uplink (gated InMatch && deployed inside the runtime). The uplink describes L's pose as left by
// the previous entity update; this frame's raw-input/motor pass follows all inbound application.
NovaSimulation::JoinerFrameSignals NovaSimulation::joiner_run_client_net_frame() {
	const uint32_t health_updates_before =
			runtime_->state().health_updates_applied;
	const uint32_t objective_updates_before =
			runtime_->state().objective_updates_applied;
	std::vector<std::vector<uint8_t>> outs;
	const bool have_L = joiner_local_spawned_ && world_->ai && world_->cached.local_player.valid();
	const opennova::world::Entity *e = have_L ? world_->registry.get(world_->cached.local_player) : nullptr;
	const opennova::world::AiEntity *ae = have_L ? world_->ai->for_handle(world_->cached.local_player) : nullptr;
	if (e && ae) {
		const opennova::PlayerExtendedUplink up = opennova::netsim::build_player_uplink(*e, *ae);
		outs = runtime_->Client_ProcessNetworkFrame(up, now_tick_);
	} else {
		outs = runtime_->Client_ProcessNetworkFrame(now_tick_);
	}
	for (const std::vector<uint8_t> &dg : outs) ship_to_host(dg);
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

// On reaching in-match (detected by the recv-fold): learn H + spawn L at the host-advertised
// pose. L is the joiner's OWN motor-driven pool-0 entity (publishes cached.local_player); H is the
// wire identity the host knows us by — the two stay distinct, reconciled by the name-match (§5.38b).
void NovaSimulation::joiner_spawn_and_arm_local_player() {
	namespace np = opennova::np;
	if (!runtime_->in_match() || joiner_local_spawned_ || !world_->ai) return;
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
			if (slot != nullptr && slot->adm_index >= 0)
				L->equipped_adm_index = static_cast<uint8_t>(slot->adm_index);
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

// The 0x0A tail is the authoritative health source for the recipient's OWN
// player. H belongs to the host's handle space; apply that recipient-local
// scalar to the joiner's distinct motor entity L without touching L's
// predicted pose. A fresh-frame guard prevents ClientState's pre-frame zero
// default from killing L during the handshake. Once L is dead, a later stale
// positive tail is ignored: retail requires the separate deploy edge before
// clearing Flags bit 1 and restoring health.
// [orig: tail health read @0x430428; store to local Health @0x4305df]
void NovaSimulation::joiner_apply_authoritative_health() {
	if (!joiner_local_spawned_ || !world_->cached.local_player.valid()) return;
	const opennova::world::EntityHandle local_h =
			world_->cached.local_player;
	opennova::world::Entity *local =
			world_->registry.get(local_h);
	AiEntity *local_ai =
			world_->ai ? world_->ai->for_handle(local_h) : nullptr;
	if (local == nullptr || local_ai == nullptr) return;
	// ClientRuntime latches deployment closed on any decoded zero tail,
	// even if a later packet in this recv pump carries stale positive HP.
	const int16_t health = runtime_->deployed()
			? runtime_->state().local_health : 0;
	if (health <= 0) {
		local->health = health;
		local_ai->health = health;
		local->alive = false;
		local->flags |= 2u;
	} else if (local->alive && (local->flags & 2u) == 0u) {
		local->health = health;
		local_ai->health = health;
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
			// The decoded pose mirrors the retail client entity fields: live
			// coarse heading (compact high byte -> entity+16) plus the retained
			// spawn/dead pitch/roll samples (entity+20/+24, live compacts omit
			// both for vehicles).
			proxy.heading_bam = static_cast<int32_t>(
					static_cast<uint32_t>(entity.yaw_byte) << 24);
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
		round.owner = opennova::world::EntityHandle{};
		round.shooter_handle = ev.shooter_handle;
		// The mounted shooter's own vehicle joins the trace exclusion exactly
		// like retail's mount rule (Controller/Gunner/Driver seats only, the
		// ray[18] leg) — resolved from the decoded carrier + the host-fed seat
		// table instead of live mount pointers. [orig: the ignored-mount select
		// feeding Physics_RaycastAgainstBoneCollision @ 0x4e4cb0]
		const opennova::netsim::ClientEntityState *shooter_row =
				client_entity_for_handle(runtime_->state(), ev.shooter_handle);
		if (shooter_row != nullptr && shooter_row->carrier_handle != 0xFFFFu &&
				shooter_row->mount_bone != 0) {
			const opennova::netsim::ClientEntityState *carrier =
					client_entity_for_handle(runtime_->state(),
							shooter_row->carrier_handle);
			const opennova::mission::ItemSeatSpec *spec = carrier != nullptr
					? item_seat_spec_for_type(item_seat_specs_, carrier->type_id)
					: nullptr;
			if (spec != nullptr) {
				for (const opennova::world::Seat &seat : spec->seats) {
					if (seat.bone_index != shooter_row->mount_bone) continue;
					if (seat.type == opennova::world::SeatType::Controller ||
							seat.type == opennova::world::SeatType::Gunner ||
							seat.type == opennova::world::SeatType::Driver)
						round.shooter_carrier_handle =
								shooter_row->carrier_handle;
					break;
				}
			}
		}
		round.origin.x = static_cast<float>(ev.origin_x) / kFixed16;
		round.origin.y = static_cast<float>(ev.origin_y) / kFixed16;
		round.origin.z = static_cast<float>(ev.origin_z) / kFixed16;
		round.dir_yaw_bam = ev.dir_yaw_bam;
		round.dir_pitch_bam = ev.dir_pitch_bam;
		round.ammo_index = adm->ammo_index;
		round.adm_index = ev.adm_index;
		round.shot_seq = ev.shot_seq;
		round.charge = ev.slot_byte;
		world_->round_sim.spawn(
				*world_, round,
				opennova::world::RoundConsequenceMode::VisualOnly);
	}

	for (const opennova::WeaponReload &reload :
			runtime_->drain_reload_notifications()) {
		++weapon_reload_received_serial_;
		weapon_reload_received_entity_ = reload.entity_handle;
		weapon_reload_received_param_ = reload.reload_param;
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
	install_item_class_resolver();
	joiner_ = true;
	if (world_) {
		world_->projectile_authority = false;
		world_->mp_session = true;
	}
	joiner_started_ = false;
	joiner_local_spawned_ = false;
	joiner_self_wire_handle_ = 0;
	return true;
}

void NovaSimulation::set_join_world_ready(bool p_ready) {
	if (joiner_ && runtime_) runtime_->set_world_ready(p_ready);
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

bool NovaSimulation::is_joined_in_match() const {
	return joiner_ && runtime_ && runtime_->in_match();
}

int NovaSimulation::get_joiner_phase() const {
	return (joiner_ && runtime_) ? static_cast<int>(runtime_->phase()) : -1;
}

int NovaSimulation::get_joiner_self_handle() const {
	return joiner_ ? static_cast<int>(joiner_self_wire_handle_) : 0;
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
	spawn.net_id = s.net_id; // the host-assigned joiner SSN (NOT the host's own 0xFFF0)
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
