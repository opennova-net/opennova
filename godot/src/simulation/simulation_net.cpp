// Simulation — the net roles (ADR 0009/0011/0012): per-load listen-host
// bring-up + host pump, the LAN joiner pump family + wire proxies/events, the
// host session config FFI, and the joiner preload/session API.
#include "simulation/simulation_internal.h"

#include <cmath>
#include <cstring>

#include <net/npruntime/server_initial_state.h> // install_mission_location_names
#include <net/npruntime/server_spawn.h> // Server_SetPlayerSpectator
#include <net/npruntime/session_status.h>
#include <runtime/terrain_query/surface_tiles.h> // surface_tiles_from_til_bytes (D-SND-15)
#include <formats/threedi/threedi_panm_pose.h> // the native PANM liveness gate (S3, ADR 0028)
#include <net/npwire/ingame_decode.h> // kRoundEventFlag* (the fire-mode byte)
#include <net/npwire/ingame_message_id.h>
#include <runtime/hud/feed_format.h> // the witnessed feed line/color policy
#include <net/netsim/client_scoreboard_view.h> // the Tab board's draw-time projection
#include <runtime/world/wire_body_sound.h> // the wire-fed remote body's footstep/foley consume
#include <net/npwire/net_ports.h> // lan_host_bind_ports (the D-NET-210 bind scan)
#include <base/vfs/vfs.h> // vfs_expansion_version_checksum (the D-NET-166 JOIN CRC)
#include <net/npwire/wire_handle.h>  // pool()/kPoolItem/kInvalid (the wire handle home)
#include <runtime/world/infantry.h>      // kAnimStanceFlag* (the witnessed stance bits)
#include <runtime/world/spawn_select.h>  // kDeployPickNone/AutoTeam (C2S 0x2C sentinels)
#include <runtime/world/vehicle_motor.h> // carrier_pose_fixed (the deck-ride pose reader)
#include <formats/rtxt/rtxt.h>
#include <runtime/world/destruction.h>  // destruction_notify_item_damage (S2C 0x13 net kill)
#include <runtime/world/entity_spawn.h> // entity_reset_to_spawn_state (redeploy release)
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace sim_internal;

namespace {

bool character_vars_from_profile(const Dictionary &p_profile,
		opennova::np::CharacterJoinVars &r_vars) {
	const Array ids = p_profile.get("character_ids", Array());
	const Array classes = p_profile.get("player_classes", Array());
	const Array avatars = p_profile.get("avatars", Array());
	if (ids.size() < 2 || classes.size() < 2 || avatars.size() < 2) {
		return false;
	}

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
	r_vars = vars;
	return true;
}

} // namespace

// P7: per-load host bring-up — the faithful §5.0 mode-3 in-process listen server
// [orig: SinglePlayer_StartMission @0x561af0], mirroring apps/nw_server/main.cpp. The host's own
// player AUTO-spawns through the real pipeline (Server_ProcessPendingPlayerSpawns ->
// resolve_player_spawn_pose marker chain), and its own loopback client renders the per-frame 0x0A.
void Simulation::bringup_host_runtime() {
	namespace np = opennova::np;
	// ctx_.mission (read by the §5.1 0x0B BMS-header burst for LAN joiners)
	// points straight at the kernel's adopted document, which outlives the match.
	host_loop_.clear();
	// Reload: a fresh host_owner_ drops any stale connections / peers from a prior mission. A reload is a
	// new match (Stop -> load), so configure_session_runtime runs once per match (never mid-match,
	// D-NET-124). serve_and_play: host_session_pump must NOT discard the host's own loopback 0x0A — we
	// fold it into ClientState (runtime_) to render the host's own view.
	// Serve-and-play (default) vs dedicated. Standalone SP is ALWAYS serve-and-play (it renders the
	// host's own player); isolated test/tooling MissionPresentation instantiations keep that default too.
	// ONED has no live editor-preview branch: MainGame/GameWorld is its sole live mission runtime
	// (ADR 0025). A LAN host honors the UI server-type (host_serve_and_play_, from
	// configure_host_session). A dedicated host (serve_and_play=false) skips the own-player spawn +
	// the local view below and lets host_session_pump discard the host loopback (step 5) — mirroring
	// start_host_session's gating [orig: SinglePlayer_StartMission @0x561af0].
	const bool serve_and_play = host_listen_ ? host_serve_and_play_ : true;
	host_owner_ = np::HostOwner{};
	host_owner_.host_loopback = &host_loop_;
	host_owner_.serve_and_play = serve_and_play;
	ctx_.world = &kernel_->world;
	ctx_.mission = &kernel_->mission;
	ctx_.terrain_til_data = terrain_til_data_; // S2C 0x45 terrain-tile load source (empty => skipped, §5.37)
	ctx_.mission_text_loaded = mission_text_loaded_;
	ctx_.mission_briefing3 = mission_briefing3_;
	ctx_.mission_briefing2 = mission_briefing2_;
	np::install_mission_location_names(ctx_, kernel_->mission, mission_location_texts_);
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
		// SP has no host dialog: g_GameType is the mission's own mode word (no multiplayer
		// bit -> stock Co-op 0x10020). The auto-spawn below resolves the retail marker chain
		// by this word — left at the default 0 it walks the DM 6095/6002 chain, finds none
		// of a campaign mission's 6001 starts, and parks the player at the origin.
		// [orig: AI_GetTaskTypeFromFlags @0x40DAE0 -> Game_StartMission @0x524360,
		// see docs/net/novaworld-net-re.md 5.2c]
		host_config.game_type = mission_game_type();
	}
	if (kernel_) {
		kernel_->world.fat_bullets = host_config.fat_bullets;
		kernel_->world.one_shot_kill = host_config.one_shot_kill;
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
	// The shell's resolved PLAYER_INFO selection for the host's own player; the
	// HostConfig default is the stock fresh-profile seed until one is installed.
	if (local_character_vars_set_) {
		host_cfg.local_character_vars = local_character_vars_;
	}
	np::start_host_session(host_owner_, host_cfg);
	kernel_->session_open = true; // the retail is_in_session fact
	if (serve_and_play) {
		// The host's own replica pipeline (HostClient role: recv-fold only, 0x0C suppressed). Folds host_loop_
		// each frame into the ClientState the present pass reads.
		runtime_ = std::make_unique<np::ClientRuntime>(host_loop_);
		// Phase-3 0x0A objective width is gated by the same g_GameType
		// carried to remote clients in 0x7B extra; the local loopback has no
		// handshake, so seed its view directly from the consolidated config.
		runtime_->view().set_game_type(host_config.game_type);
		// The 0x1D header-form session half: the loopback replica stands in
		// world.mp_session for the retail is_in_session (SP listen stays the
		// 7-byte team form). [orig: NapiNPClientMsg_0x01D @0x43086c]
		runtime_->view().set_mp_session(kernel_ != nullptr && kernel_->world.mp_session);

		// Seed the look heading from the auto-spawned player's facing so the body starts aligned (the
		// motor drives entity Yaw from kernel_->input.look_heading each frame, else input snaps it to 0).
		kernel_->reset_local_player_input_to_player_facing();
	} else {
		// Dedicated (UI "serve only"). The witnessed original makes this a true host-only session
		// [orig: HG_SERVEONLY -> CGameSession_SetConnectionMode(1), is_host=1/is_client=0; HostDialog
		// read @0x555940, dispatch @0x556d00, mode switch @0x4c49f0]. start_host_session now selects
		// that exact HostOnly row, passes no type-2 loopback to create_session, and creates no local
		// player. There is therefore no local replica pipeline: runtime_ stays null, and host_pump's fold
		// plus the present snapshot both guard on it (D-NET-131 fixed 2026-07-24).
		runtime_.reset();
	}
}

namespace {
// UdpPump-backed netsim::IDatagramSocket — the Godot adapter the shared host owner loop pumps. A
// null/closed pump (pure SP) yields recv 0 / send no-op, so the loop's socket legs go inert exactly as
// the old host_listen_-gated code did. PeerAddr <-> "a.b.c.d" uses the LE octet packing PeerAddr
// documents (octet 0 in the low byte; 127.0.0.1 -> 0x0100007F) — the conversion formerly in
// peer_from_addr / send_datagram.
class UdpPumpDatagramSocket : public opennova::netsim::IDatagramSocket {
public:
	explicit UdpPumpDatagramSocket(UdpPump *pump) : pump_(pump) {}

	int recv_from(uint8_t *buf, std::size_t cap, opennova::PeerAddr &from) override {
		if (pump_ == nullptr || !pump_->is_open()) return 0;
		if (!pump_->has_inbound()) {
			pump_->poll();
			if (!pump_->has_inbound()) return 0;
		}
		PackedByteArray bytes;
		if (!pump_->take_inbound_native(from, bytes)) return 0;
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
	UdpPump *pump_;
};

} // namespace

// P7/A5: the per-frame host owner loop is now a THIN delegation to the shared core host_session_pump
// (engine/net/npruntime) — the SAME loop apps/nw_server runs, so the headless server and the Godot binding can no
// longer drift. Simulation supplies the socket (a UdpPump adapter; SP passes a null pump and the
// loop's socket legs go inert) and folds the host's own loopback 0x0A into ClientState for the present
// pass (serve_and_play: host_session_pump skips the loopback discard so we can read it here).
void Simulation::host_pump() {
	namespace np = opennova::np;
	const bool profiling = runtime_profiling_enabled_;
	const uint64_t prep_start = profiling ? opennova::io::perf_now_us() : 0;
	// Server_SendRandomSeedSync's non-dedicated S2C 0x68 cursor advances by 50
	// and wraps against the current renderer viewport height [orig:
	// Server_SendRandomSeedSync @ 0x511360 — CEffectWorld_GetViewportDimensions
	// @ 0x5b1560 (call @ 0x511375), wrap @ 0x511391]. Resolve the render
	// window the way retail's CEffectWorld query does — the live window (the
	// runtime owns this node without parenting it into the tree, so
	// get_viewport() alone is null on every production host). A headless
	// DisplayServer has no renderer (the dedicated-host analogue); a missing/
	// non-drawable viewport hands listen_host::frame 0 and npruntime
	// suppresses 0x68 instead of inventing a screen size (D-NET-206).
	Viewport *viewport = get_viewport();
	if (viewport == nullptr) {
		DisplayServer *display = DisplayServer::get_singleton();
		if (display != nullptr && display->get_name() != "headless") {
			SceneTree *tree = Object::cast_to<SceneTree>(
					Engine::get_singleton()->get_main_loop());
			if (tree != nullptr)
				viewport = tree->get_root();
		}
	}
	const double viewport_height = viewport != nullptr
			? viewport->get_visible_rect().size.y : 0.0;
	const uint32_t now = host_owner_.now_tick;
	// What the view arbiter reads from the session (death screen, end round,
	// the death camera): sampled pre-fold, exactly the value the old inline
	// view tick consumed at this point in the frame.
	kernel_->view_session_inputs = local_view_session_inputs();
	UdpPumpDatagramSocket sock(host_listen_ ? pump_.ptr() : nullptr);
	if (profiling)
		frame_phase_perf_.host_prep_us +=
				static_cast<int64_t>(opennova::io::perf_now_us() - prep_start);
	// The ONE listen frame (ADR 0042 d3): the local C2S drain, the pre-tick
	// input apply, host_session_pump (Server_TickUpdate's owner loop), the
	// local view/weapon pumps, and the new-soldier .adm ground — over this
	// sim's kernel and host state. [orig: Game_ProcessMainFrame @0x5263f0]
	np::HostSessionPerf host_perf;
	opennova::inmatch::listen_host::frame(*kernel_, host_state_, sock,
			static_cast<int32_t>(viewport_height),
			profiling ? &host_perf : nullptr);
	if (profiling)
		frame_phase_perf_.host_session += host_perf;
	kernel_->tick_medic_cooldown(local_player_dead()); // Player_UpdatePerFrame's cooldown leg
	// The kernel pump's wire-facing reload outcome relays onto the loopback so
	// the shared dispatcher broadcasts the S2C 0x49 to every client next frame
	// (the authority already performed WeaponSlot_ReloadAmmo inside the pump;
	// the server handler's local-connection gate prevents a second refill).
	if (kernel_->last_reload.valid && host_owner_.serve_and_play) {
		opennova::WeaponReload reload;
		reload.entity_handle = kernel_->last_reload.entity_handle;
		reload.reload_param = kernel_->last_reload.reload_param;
		host_loop_.client_send(0x25, opennova::encode_weapon_reload(reload));
		kernel_->last_reload = opennova::world::LocalWeaponReloadWire{};
	}
	// The host's measurable net leg for the F3 Stats board: the ClientState
	// fold. The S2C serialize/emit half rides inside np::host_session_pump
	// (fused with the logic tick) and stays inside the Sim step number until
	// npruntime grows a phase seam.
	const uint64_t net_start = profiling ? opennova::io::perf_now_us() : 0;
	np::ClientFramePerf client_perf;
	if (runtime_)
		runtime_->Client_ProcessNetworkFrame(
				now, profiling ? &client_perf : nullptr); // fold host_loop_ -> ClientState
	if (profiling) {
		last_net_tick_us_ = opennova::io::perf_now_us() - net_start;
		frame_phase_perf_.client_decode_us +=
				static_cast<int64_t>(last_net_tick_us_);
		frame_phase_perf_.client += client_perf;
	}
}

// host_pump's dispatch_event + admit_peer were promoted into engine/net/npruntime (np::dispatch_event /
// np::admit_peer over host_owner_, driven by host_session_pump) — the SAME code apps/nw_server runs, so
// the Godot binding and the headless server can no longer drift.

// Stamp each decoded Player/Infantry row's .adm registry id from its wire
// type (visual item -> anim_def -> register_adm), once per row; -1 = no adm
// (the row stays chase-only, truthful). Cached per type so late-joining
// peers and respawns cost one map lookup.
void Simulation::resolve_client_row_adm_ids() {
	// The host never presents a decoded row (D-NET-140 closed).
	if (!joiner_) return;
	if (runtime_ == nullptr || kernel_->root_motion.empty() ||
			infantry_adm_resource_root_.is_null() ||
			infantry_adm_item_db_.is_null())
		return;
	for (opennova::netsim::ClientEntityState &es :
			runtime_->state().entities) {
		if (es.rm_adm_id != -2) continue;
		if (es.cls != opennova::EntityClass::Player &&
				es.cls != opennova::EntityClass::Infantry)
			continue;
		if (es.type_id == 0) continue;
		const auto cached = client_row_adm_by_type_.find(es.type_id);
		if (cached != client_row_adm_by_type_.end()) {
			es.rm_adm_id = static_cast<int16_t>(cached->second);
			continue;
		}
		const int visual_item_id = visual_item_id_for_runtime_type(
				es.type_id, infantry_adm_item_db_);
		String adm = infantry_adm_item_db_->get_anim_def(visual_item_id);
		int adm_id = -1;
		if (!adm.is_empty()) {
			if (!adm.to_lower().ends_with(".adm")) adm += ".adm";
			adm_id = kernel_->root_motion.register_adm(
					infantry_adm_resource_root_.is_valid()
							? &infantry_adm_resource_root_->native_index()
							: nullptr,
					std::string(adm.utf8().get_data()));
		}
		if (adm_id < 0 && !kernel_->root_motion.empty()) adm_id = 0; // the default set
		client_row_adm_by_type_[es.type_id] = adm_id;
		es.rm_adm_id = static_cast<int16_t>(adm_id);
	}
}

// The shell-asset leg of the bridge's materialize phase (S10a): a streamed
// topology/world change landed in the registry. Retire the changed rows'
// collision/occlusion instances and caches, refresh traits + seat specs, then
// re-fold the retained 0x0D mountHandles image through the bridge materializer.
void Simulation::on_replica_world_changed(
		const opennova::netsim::ClientWorldSyncResult &p_sync) {
	for (const opennova::world::EntityLifetime lifetime : p_sync.retired) {
		if (!lifetime.valid()) continue;
		const auto cached =
				kernel_->collision_state.resolution_attempted.find(lifetime.handle.packed);
		// A later allocation at the same packed handle may already have had
		// its caches rebuilt. The retired lifetime cannot erase those.
		if (cached != kernel_->collision_state.resolution_attempted.end() &&
				cached->second != lifetime.registry_spawn_id)
			continue;
		const opennova::world::EntityHandle handle = lifetime.handle;
		kernel_->collision.remove_entity_instance(handle);
		kernel_->occlusion.remove_entity_instance(handle);
		kernel_->collision_pose.remove_entity(handle);
		kernel_->collision_state.resolution_attempted.erase(handle.packed);
	}

	// Zone/deploy lookup is a registry-derived cache. Any streamed topology
	// change can add/remove an eligible zone and must invalidate it before UI
	// or a C2S 0x0E pick consults the world.
	deploy_zone_registry_built_ = false;
	if (item_traits_db_.is_valid())
		resolve_item_traits(item_traits_db_);

	auto refresh_seats = [&](
			const std::vector<opennova::world::EntityLifetime> &rows) {
		for (const opennova::world::EntityLifetime lifetime : rows) {
			if (lifetime.handle.pool() != 1) continue;
			if (opennova::world::Entity *entity =
					kernel_->world.registry.get(lifetime))
				refresh_item_seat_spec(*entity);
		}
	};
	refresh_seats(p_sync.spawned);
	refresh_seats(p_sync.updated);
	// The wire materializer deliberately does not fabricate seat
	// definitions. Item/model resolution above installs them, then this
	// idempotent fold projects the retained 0x0D mountHandles image by each
	// seat's fixed retail_slot.
	(void)joiner_bridge_.materializer().sync(runtime_->state(), kernel_->world);

	if (collision_item_db_.is_valid())
		resolve_collision_instances(collision_item_db_);
}

void Simulation::joiner_pump() {
	if (!runtime_) {
		if (runtime_profiling_enabled_) last_net_tick_us_ = 0;
		return;
	}
	// The joiner's wire leg for the F3 Stats board: recv pump + net frame +
	// uplink ship, ending where the local (non-authority) world work begins.
	// The bridge fires on_wire_leg_complete at exactly that boundary.
	const uint64_t net_start =
			runtime_profiling_enabled_ ? opennova::io::perf_now_us() : 0;
	// The frame itself — provider wiring, hello, recv-fold + uplink + folds,
	// the decoded-consequence application, the local World tick, and the
	// post-tick recompose — lives in the engine bridge (S10a, ADR 0028). This
	// binding supplies the shell legs: the socket, the render-coupled asset
	// resolution, the loadout profile seams (S7b disposition), device input,
	// the view/weapon pumps shared with the host path, and the clocks.
	opennova::np::JoinerWorldBridge::PumpContext ctx{
			kernel_->world, *runtime_, kernel_->weapon, kernel_->loadout,
			kernel_->inventory, kernel_->inventory_valid, kernel_->seat_specs,
			kernel_->root_motion.empty() ? nullptr : &kernel_->root_motion};
	opennova::np::JoinerWorldBridge::PumpHooks hooks;
	hooks.send = [this](const std::vector<uint8_t> &dg) { ship_to_host(dg); };
	hooks.deposit_inbound = [this] { joiner_deposit_inbound(); };
	hooks.resolve_row_adm_ids = [this] { resolve_client_row_adm_ids(); };
	if (runtime_profiling_enabled_)
		hooks.on_wire_leg_complete = [this, net_start] {
			last_net_tick_us_ = opennova::io::perf_now_us() - net_start;
		};
	hooks.apply_authoritative_loadout =
			[this] { apply_joiner_authoritative_loadout(); };
	hooks.reseed_kit_on_side_change =
			[this] { return reseed_session_kit_on_side_change(); };
	hooks.push_loadout_kit = [this] { push_joiner_loadout_kit(); };
	hooks.on_diagnostic_sample =
			[this] { print_joiner_net_diagnostic_sample(); };
	hooks.on_replica_world_changed =
			[this](const opennova::netsim::ClientWorldSyncResult &sync) {
				on_replica_world_changed(sync);
			};
	hooks.on_replica_world_static_ready = [this] { occlusion_init_mission(); };
	hooks.on_local_player_spawned = [this](int32_t look_heading_bam) {
		on_joiner_local_player_spawned(look_heading_bam);
	};
	hooks.on_local_player_redeployed = [this](int32_t look_heading_bam) {
		on_joiner_local_player_redeployed(look_heading_bam);
	};
	hooks.on_mount_changed = [this] {
		kernel_->view.binoculars_requested = false;
		kernel_->view_tracker.binocular_yaw_offset_deg = 0.0f;
		kernel_->view_tracker.binocular_pitch_offset_deg = 0.0f;
		refresh_local_player_view_effects();
		kernel_->sync_local_mounted_input_heading();
	};
	hooks.wire_collision_shape = [this](uint16_t type_id) {
		return wire_collision_shape_for_type(type_id);
	};
	hooks.apply_input_pre_tick = [this] { kernel_->apply_player_input_pre_tick(); };
	hooks.sync_mounted_input_heading =
			[this] { kernel_->sync_local_mounted_input_heading(); };
	hooks.tick_view = [this] { tick_local_player_view(); };
	hooks.tick_weapon = [this] {
		tick_local_player_weapon();
		kernel_->tick_medic_cooldown(local_player_dead());
	};
	hooks.tick_weather = [this] { kernel_->tick_weather(); };
	joiner_bridge_.pump(ctx, hooks);
	// An S2C 0x41 applied inside the pump mutated the live charattr table; the
	// World's per-class ATTRIBUTES words follow it the same frame [orig: the
	// HUD reads g_CharAttr directly, AnimMap_IsSlotActive @0x4125e0, so the
	// clear is visible on the next draw; see docs/interface/hud-re.md].
	sync_class_attribute_flags();
}

// Deposit received framed datagrams for this frame's recv pump.
void Simulation::joiner_deposit_inbound() {
	if (pump_.is_valid()) {
		pump_->poll();
		while (pump_->has_inbound()) {
			const Dictionary d = pump_->take_inbound();
			const PackedByteArray bytes = d.get("bytes", PackedByteArray());
			runtime_->receive(bytes.ptr(), static_cast<std::size_t>(bytes.size()));
		}
	}
}

// The ~1 Hz frozen-session diagnostic print (the bridge computes the sampled
// state; this shell leg owns the env-gated print_verbose channel). L's LIVE
// pose is the exact source build_player_uplink transmits: a pose that never
// changes while the player is moving on screen means the local motor is not
// driving L (so the host renders us frozen at spawn and eventually stops
// streaming records around a stale reference position).
void Simulation::print_joiner_net_diagnostic_sample() {
	if (!is_joiner_network_diagnostics_enabled() || !runtime_) return;
	String local_state = " L=none";
	if (joiner_bridge_.local_spawned() && kernel_ && kernel_->world.ai) {
		const opennova::world::AiEntity *lae =
				kernel_->world.ai->for_handle(kernel_->world.cached.local_player);
		const opennova::world::Entity *le =
				kernel_->world.registry.get(kernel_->world.cached.local_player);
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
			static_cast<int64_t>(runtime_->inbound_frontier_seq()),
			static_cast<int64_t>(runtime_->outbound_seq()),
			static_cast<int64_t>(runtime_->state().compact_records_applied),
			static_cast<int64_t>(runtime_->inbound_gap_depth()),
			static_cast<int64_t>(runtime_->retained_outbound_depth()),
			runtime_->in_match() ? 1 : 0,
			runtime_->is_deployed() ? 1 : 0, local_state,
			joiner_bridge_.freeze_suspected()
					? vformat(" *** REPLICATION FROZEN %ds ***",
							  static_cast<int64_t>(joiner_bridge_.flat_seconds()))
					: String()));
}

// L spawned on the in-match edge: fresh adm resolution, then the join-wait
// input latches die and the look heading seeds from the spawn facing (retail
// polls live keys; a press during the join wait must not cross the spawn edge).
void Simulation::on_joiner_local_player_spawned(int32_t p_look_heading_bam) {
	kernel_->resolve_new_infantry_adm_ids();
	kernel_->reset_local_player_input(p_look_heading_bam);
}

// L revived on an ACK-qualified redeploy release: the same input-latch reset
// from the redeployed authoritative heading, plus the respawn loadout rebuild.
void Simulation::on_joiner_local_player_redeployed(int32_t p_look_heading_bam) {
	kernel_->reset_local_player_input(p_look_heading_bam);
	respawn_local_player_loadout();
}

opennova::world::ResolvedCollisionShape Simulation::wire_collision_shape_for_type(
		uint16_t p_type_id) {
	const auto cached = wire_collision_shape_by_type_.find(p_type_id);
	if (cached != wire_collision_shape_by_type_.end()) return cached->second;
	opennova::world::ResolvedCollisionShape shape;
	if (collision_item_db_.is_valid()) {
		const opennova::simassets::CollisionResolveDeps deps{
				kernel_->collision, kernel_->occlusion, kernel_->collision_pose,
				kernel_->models};
		shape = opennova::simassets::collision_shape_for_runtime_type(
				static_cast<int>(p_type_id), collision_item_db_->native_items(),
				kernel_->collision_state, deps);
	}
	wire_collision_shape_by_type_.emplace(p_type_id, shape);
	return shape;
}

void Simulation::enable_listen_server(bool p_enable) {
	listen_server_ = p_enable;
	// P7: the SP listen server now rides the npruntime in-match runtime (ctx_ / host_loop_ /
	// runtime_), stood up per-load in bringup_host_runtime — there is no net ISystem and
	// no legacy loopback seam here. (The LAN host still builds the legacy seam in enable_host_listen
	// until A3; a sim is SP listen XOR LAN host XOR joiner.)
}

void Simulation::set_terrain_til_data(const PackedByteArray &p_til_bytes) {
	terrain_til_data_.assign(p_til_bytes.ptr(), p_til_bytes.ptr() + p_til_bytes.size());
	// The same bytes feed the sim's placed-tile surface array (D-SND-15);
	// the fold and its witness live engine-side (terrain_query
	// surface_tiles_from_til_bytes).
	surface_tiles_ =
			opennova::terrain::surface_tiles_from_til_bytes(terrain_til_data_);
	apply_terrain_to_ai();
}

void Simulation::set_mission_text_data(const PackedByteArray &p_rtxt_bytes) {
	mission_text_loaded_ = false;
	mission_briefing3_.clear();
	mission_briefing2_.clear();
	mission_location_texts_.clear();
	mission_people_names_.clear();
	if (p_rtxt_bytes.is_empty()) return;

	opennova::rtxt::File table;
	std::string error;
	if (!opennova::rtxt::parse(p_rtxt_bytes.ptr(),
	                           static_cast<std::size_t>(p_rtxt_bytes.size()),
	                           table, error)) {
		UtilityFunctions::push_warning(String("Simulation: mission text RTXT rejected: ") +
		                               String::utf8(error.c_str()));
		return;
	}

	// Preserve the table's raw cp1252 bytes. Retail uses an empty briefing2 as
	// the signal to fall back to briefing [orig: @0x506649..0x506660].
	if (const opennova::rtxt::Entry *e = table.find_in_section("info", "briefing3"))
		mission_briefing3_ = e->text;
	if (const opennova::rtxt::Entry *e = table.find_in_section("info", "briefing2"))
		mission_briefing2_ = e->text;
	if (mission_briefing2_.empty()) {
		if (const opennova::rtxt::Entry *e = table.find_in_section("info", "briefing"))
			mission_briefing2_ = e->text;
	}

	// The numeric-key section harvests, raw cp1252 values with only the ASCII
	// section/key interpreted: [Locations] LOCATION%03i (type-2044 markers by
	// one-based spawn order, the S2C 0x0F deploy-map labels) and [PeopleNames]
	// STRNAME%03i (the D-HUD-20 authored entity display names promote resolves
	// from each record's name_index — the witnessed resolve is cited at the
	// promote.cpp port site).
	const auto fold = [](char c) {
		return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
	};
	const auto harvest_indexed = [&](const char *section_lc,
			std::size_t section_len, const char *prefix_lc,
			std::size_t prefix_len,
			std::unordered_map<int32_t, std::string> &out_map) {
		for (std::size_t section_index = 0;
		     section_index < table.sections.size(); ++section_index) {
			const std::string &section_name = table.sections[section_index].name;
			if (section_name.size() != section_len) continue;
			bool is_match = true;
			for (std::size_t i = 0; i < section_len; ++i) {
				if (fold(section_name[i]) != section_lc[i]) {
					is_match = false;
					break;
				}
			}
			if (!is_match) continue;

			for (const opennova::rtxt::Entry *entry :
			     table.get_section_entries(static_cast<uint32_t>(section_index))) {
				if (entry == nullptr || entry->key.size() <= prefix_len) continue;
				bool valid = true;
				for (std::size_t i = 0; i < prefix_len; ++i) {
					if (fold(entry->key[i]) != prefix_lc[i]) {
						valid = false;
						break;
					}
				}
				int32_t index = 0;
				for (std::size_t i = prefix_len; valid && i < entry->key.size();
				     ++i) {
					const char digit = entry->key[i];
					if (digit < '0' || digit > '9' || index > 214748364) {
						valid = false;
						break;
					}
					index = index * 10 + (digit - '0');
				}
				if (valid) out_map.emplace(index, entry->text);
			}
			break;
		}
	};
	harvest_indexed("locations", 9, "location", 8, mission_location_texts_);
	harvest_indexed("peoplenames", 11, "strname", 7, mission_people_names_);
	mission_text_loaded_ = true;
}

bool Simulation::set_score_config_data(const PackedByteArray &p_score_ini_bytes) {
	if (p_score_ini_bytes.is_empty()) return false;
	const std::string text(
			reinterpret_cast<const char *>(p_score_ini_bytes.ptr()),
			static_cast<std::size_t>(p_score_ini_bytes.size()));
	return opennova::np::load_session_score_config(host_session_config_, text);
}

bool Simulation::enable_host_listen(int p_port) {
	listen_server_ = true;
	if (pump_.is_null()) pump_.instantiate();
	pump_->set_capture_path(capture_pcap_path_);
	// The LAN host scans the retail port range from the requested port
	// (D-NET-210; the sequence policy is npwire net_ports.h
	// lan_host_bind_ports). Port 0 keeps the OS-assigned bind — the dev/test
	// seam retail has no analog for.
	bool bound = false;
	if (p_port <= 0) {
		bound = pump_->bind_listen(0) == 0;
	} else {
		for (const uint16_t port : opennova::lan_host_bind_ports(
					 static_cast<uint32_t>(p_port))) {
			if (pump_->bind_listen(port) == 0) {
				bound = true;
				break;
			}
		}
	}
	if (!bound) {
		host_listen_ = false;
		return false;
	}
	host_listen_ = true;
	if (kernel_) {
		kernel_->world.projectile_authority = true;
		kernel_->world.mp_session = true;
	}
	// P7: the LAN host rides the npruntime runtime (ctx_ over a real UDP socket), stood up per-load in
	// bringup_host_runtime with SocketMode::Lan. UdpPump owns the socket; all protocol/crypto/
	// framing stays in libs (ADR 0010). host_session_config_ keeps the GDScript-facing session options
	// (the Dictionary getter + the §5.1 reactive-reply config fed to configure_session_runtime).
	// The port actually bound (the scan may have stepped past the requested
	// one) is what the session advertises.
	host_bind_port_ = static_cast<uint16_t>(pump_->local_port());
	return true;
}

int Simulation::get_host_listen_port() const {
	return (host_listen_ && pump_.is_valid()) ? pump_->local_port() : 0;
}

int Simulation::get_host_peer_count() const {
	// Count the type-1 (remote-joiner) connections in the npruntime table. The host's own type-2
	// loopback is excluded; a pre-Hello garbage datagram registers no node (handle_server_datagram
	// drops bad envelopes), so it stays 0 until a real JointOperations peer handshakes.
	int n = 0;
	for (const opennova::np::NapiNPConnection &c : ctx_.np_protocol.connection_list) {
		if (c.type == 1) ++n;
	}
	return n;
}

void Simulation::configure_host_session(Dictionary p_options) {
	opennova::np::GameConfig config = host_session_config_;
	host_bind_port_ = dictionary_u16(p_options, "bind_port", host_bind_port_);
	apply_dictionary_string(p_options, "server_name", config.server_name);
	apply_dictionary_string(p_options, "mission_name", config.mission_name);
	apply_dictionary_string(p_options, "mission_file", config.mission_file);
	apply_dictionary_string(p_options, "custom_text", config.custom_text);
	apply_dictionary_string(p_options, "player_name", config.player_name);
	apply_dictionary_string(p_options, "expansion", config.expansion);
	apply_dictionary_string(
			p_options, "spectator_password", config.spectator_password);
	if (p_options.has("spectator_slots")) {
		config.spectator_slots = dictionary_i32(
				p_options, "spectator_slots", config.spectator_slots);
		if (config.spectator_slots < -1) config.spectator_slots = -1;
	}
	// D-NET-166: the host's g_expansion_checksum analog. When the caller names
	// its install root, compute the CRC of the loose
	// expansion/<name>/version.txt so the join gate can run retail's compare
	// (the witnessed producer/gate live in vfs_expansion_version_checksum and
	// validates_join_request).
	if (p_options.has("game_root")) {
		const String root = p_options["game_root"];
		config.expansion_version_checksum =
				opennova::vfs_expansion_version_checksum(
						std::string(root.utf8().get_data()), config.expansion);
	}
	if (p_options.has("integrity_profile")) {
		const String requested = String(p_options["integrity_profile"]).strip_edges();
		const std::string id(requested.utf8().get_data());
		if (id.empty() ||
				opennova::np::find_integrity_challenge_profile(id) != nullptr) {
			config.integrity_profile = id;
		} else {
			UtilityFunctions::push_warning(
					String("Unknown authority integrity profile: ") + requested);
			config.integrity_profile.clear();
		}
	}
	if (p_options.has("gametype")) {
		config.game_type = dictionary_u32(p_options, "gametype", config.game_type);
	} else if (p_options.has("game_type")) {
		config.game_type = dictionary_u32(p_options, "game_type", config.game_type);
	}
	config.mp_attributes = dictionary_u32(p_options, "mpattrib", config.mp_attributes);
	config.class_allow_mask = static_cast<uint16_t>(dictionary_u32(
			p_options, "class_allow_mask", config.class_allow_mask) & 0xFFFFu);
	config.respawn_time = dictionary_u32(p_options, "respawn_time", config.respawn_time);
	config.time_limit_minutes = dictionary_u32(
			p_options, "time_limit_minutes", config.time_limit_minutes);
	config.replay_enabled = dictionary_u32(
			p_options, "replay_enabled", config.replay_enabled);
	config.max_team_lives = dictionary_u32(
			p_options, "max_team_lives", config.max_team_lives);
	config.score_limit = dictionary_u32(p_options, "score_limit", config.score_limit);
	config.max_score = dictionary_u32(p_options, "max_score", config.max_score);
	config.koth_delta = dictionary_u32(p_options, "koth_delta", config.koth_delta);
	config.flag_return_ticks = dictionary_u32(
			p_options, "flag_return_ticks", config.flag_return_ticks);
	config.capture_duration_seconds = dictionary_i32(
			p_options, "capture_duration_seconds", config.capture_duration_seconds);
	config.capture_speed_setting = dictionary_i32(
			p_options, "capture_speed_setting", config.capture_speed_setting);
	config.spawn_wave_time_base = dictionary_i32(
			p_options, "spawn_wave_time_base", config.spawn_wave_time_base);
	config.spawn_wave_time_zone = dictionary_i32(
			p_options, "spawn_wave_time_zone", config.spawn_wave_time_zone);
	config.default_spawn_requires_no_team_zone = dictionary_u32(
			p_options, "default_spawn_requires_no_team_zone",
			config.default_spawn_requires_no_team_zone);
	config.num_teams = static_cast<uint8_t>(dictionary_u32(
			p_options, "num_teams", config.num_teams) & 0xFFu);
	config.respawn_timeout = dictionary_u32(
			p_options, "respawn_timeout", config.respawn_timeout);
	config.start_delay = dictionary_u32(p_options, "start_delay", config.start_delay);
	config.destroy_buildings = dictionary_u32(
			p_options, "destroy_buildings", config.destroy_buildings);
	config.death_messages = dictionary_u32(
			p_options, "death_messages", config.death_messages);
	// The witnessed BANDWIDTH server command (100-1600, clamped at apply):
	// lowers the per-frame 0x0A byte cap so entity records rotate across frames
	// [orig: g_entity_send_budget @0xC8FC50].
	config.entity_send_budget =
			dictionary_u32(p_options, "bandwidth", config.entity_send_budget);
	// Retail selects its default send divider from the session family, then
	// from g_LanMode for an authority LAN host. Explicit test/tool overrides
	// remain available through send_holdoff_ticks.
	// [orig: NapiNPServer_GetSendHoldoffTicks @0x4c4ab0]
	if (p_options.has("channel")) {
		const String channel = String(p_options["channel"]);
		config.session_channel = channel.nocasecmp_to("NovaWorld") == 0
				? opennova::np::GameSessionChannel::NovaWorld
				: opennova::np::GameSessionChannel::Lan;
	}
	config.lan_mode = dictionary_u32(p_options, "lan_mode", config.lan_mode);
	if (p_options.has("send_holdoff_ticks")) {
		const Variant holdoff = p_options["send_holdoff_ticks"];
		if (holdoff.get_type() == Variant::NIL)
			config.send_holdoff_ticks.reset();
		else
			config.send_holdoff_ticks =
					dictionary_u32(p_options, "send_holdoff_ticks", 1);
	}
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
		} else if (mp > opennova::np::kMaxPlayersCap) {
			mp = opennova::np::kMaxPlayersCap;
		}
		host_max_players_ = mp;
	}
	host_session_config_ = std::move(config);
	if (kernel_ && host_listen_) {
		kernel_->world.fat_bullets = host_session_config_.fat_bullets;
		kernel_->world.one_shot_kill = host_session_config_.one_shot_kill;
	}
}

Dictionary Simulation::get_host_session_config() const {
	const opennova::np::GameConfig &session = host_session_config_;
	Dictionary out;
	out["bind_port"] = static_cast<int>(host_bind_port_);
	out["server_name"] = String(session.server_name.c_str());
	out["mission_name"] = String(session.mission_name.c_str());
	out["mission_file"] = String(session.mission_file.c_str());
	out["player_name"] = String(session.player_name.c_str());
	out["expansion"] = String(session.expansion.c_str());
	out["integrity_profile"] = String(session.integrity_profile.c_str());
	out["spectator_slots"] = static_cast<int64_t>(session.spectator_slots);
	out["spectator_password"] = String(session.spectator_password.c_str());
	out["gametype"] = static_cast<int64_t>(session.game_type);
	out["mpattrib"] = static_cast<int64_t>(session.mp_attributes);
	out["class_allow_mask"] = static_cast<int64_t>(session.class_allow_mask);
	out["respawn_time"] = static_cast<int64_t>(session.respawn_time);
	out["time_limit_minutes"] = static_cast<int64_t>(session.time_limit_minutes);
	out["replay_enabled"] = static_cast<int64_t>(session.replay_enabled);
	out["max_team_lives"] = static_cast<int64_t>(session.max_team_lives);
	out["score_limit"] = static_cast<int64_t>(session.score_limit);
	out["max_score"] = static_cast<int64_t>(session.max_score);
	out["koth_delta"] = static_cast<int64_t>(session.koth_delta);
	out["flag_return_ticks"] = static_cast<int64_t>(session.flag_return_ticks);
	out["capture_duration_seconds"] =
			static_cast<int64_t>(session.capture_duration_seconds);
	out["capture_speed_setting"] =
			static_cast<int64_t>(session.capture_speed_setting);
	out["spawn_wave_time_base"] =
			static_cast<int64_t>(session.spawn_wave_time_base);
	out["spawn_wave_time_zone"] =
			static_cast<int64_t>(session.spawn_wave_time_zone);
	out["default_spawn_requires_no_team_zone"] =
			static_cast<int64_t>(session.default_spawn_requires_no_team_zone);
	out["num_teams"] = static_cast<int64_t>(session.num_teams);
	out["respawn_timeout"] = static_cast<int64_t>(session.respawn_timeout);
	out["start_delay"] = static_cast<int64_t>(session.start_delay);
	out["destroy_buildings"] = static_cast<int64_t>(session.destroy_buildings);
	out["death_messages"] = static_cast<int64_t>(session.death_messages);
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

void Simulation::set_join_character_profile(const Dictionary &p_profile) {
	opennova::np::CharacterJoinVars vars{};
	if (!character_vars_from_profile(p_profile, vars)) return;
	join_character_vars_ = vars;
	join_character_vars_set_ = true;
	install_character_join_vars();
}

void Simulation::set_local_character_profile(const Dictionary &p_profile) {
	opennova::np::CharacterJoinVars vars{};
	if (!character_vars_from_profile(p_profile, vars)) return;
	local_character_vars_ = vars;
	local_character_vars_set_ = true;
}

bool Simulation::set_join_integrity_profile(const String &p_profile_id) {
	const std::string id = std::string(p_profile_id.strip_edges().utf8().get_data());
	if (id.empty()) {
		join_integrity_profile_id_.clear();
		install_join_integrity_profile();
		return true;
	}
	if (opennova::np::find_integrity_challenge_profile(id) == nullptr) {
		join_integrity_profile_id_.clear();
		install_join_integrity_profile();
		return false;
	}
	join_integrity_profile_id_ = id;
	install_join_integrity_profile();
	return true;
}

void Simulation::set_join_token(const String &p_token) {
	// The .joi-recovered game-session BT join token a NovaWorld host validates
	// (reject code 9). Empty/"0" is the LAN default. Applied to the live joiner
	// runtime immediately and re-applied on each (re)load via install_join_token.
	join_token_ = std::string(p_token.strip_edges().utf8().get_data());
	if (join_token_.empty()) join_token_ = "0";
	install_join_token();
}

void Simulation::set_join_cd_cookie(const PackedByteArray &p_cookie) {
	// The CD identity cookie (packed PUB* blob) for the C2S 0x00 JOIN. Retained
	// and re-applied to the joiner runtime on each (re)load via install_join_cd_cookie.
	join_cd_cookie_.assign(p_cookie.ptr(), p_cookie.ptr() + p_cookie.size());
	install_join_cd_cookie();
}

void Simulation::set_join_expansion_version_root(const String &p_game_root) {
	// D-NET-166: the JOIN VERSIONCRCSTRING checksum source. The runtime CRCs
	// the loose expansion/<SUS2>/version.txt under this root at JOIN-build
	// time (see JoinerConnection::set_expansion_version_root).
	join_expansion_version_root_ = std::string(p_game_root.utf8().get_data());
	install_expansion_version_root();
}

// ---- co-op LAN joiner (D.2) -------------------------------------------------

bool Simulation::enable_join(const String &p_host_ip, int p_port,
		const String &p_player_name, int p_join_role,
		const String &p_spectator_password) {
	// P7: the joiner is a non-authority np::ClientRuntime (Joiner role) built per-load by the boot's role hook;
	// it owns the connect-leg state machine + the S2C->ClientState fold internally. Here we only dial
	// the socket + store the player name (the ClientAuth.NA the host echoes for the name-match). Leave
	// listen_server_ false (the present gate adds || joiner_); a sim is host XOR joiner.
	// Validate the lifecycle transition before opening a socket. Re-dialing a
	// live mission is rejected without partially replacing its transport.
	const opennova::inmatch::TransitionResult role = session_.configure_role(
			opennova::inmatch::Role::Joiner);
	if (role.code != opennova::inmatch::TransitionCode::Applied &&
			role.code != opennova::inmatch::TransitionCode::NoOp) {
		return false;
	}
	if (pump_.is_null()) pump_.instantiate();
	pump_->set_capture_path(capture_pcap_path_);
	if (pump_->dial(p_host_ip, p_port) != 0) {
		joiner_ = false;
		return false;
	}
	joiner_player_name_ = std::string(p_player_name.utf8().get_data());
	join_role_ = p_join_role == static_cast<int>(
			opennova::np::JoinRole::Spectator)
			? opennova::np::JoinRole::Spectator
			: opennova::np::JoinRole::Player;
	join_spectator_password_ =
			std::string(p_spectator_password.utf8().get_data());
	// Build the Joiner runtime now so get_joiner_phase reads Idle before the first load (the contract
	// the legacy joiner_session_ held); each (re)load's role hook rebuilds it fresh.
	runtime_ = std::make_unique<opennova::np::ClientRuntime>(joiner_player_name_);
	runtime_->set_join_request(join_role_, join_spectator_password_);
	install_charattr_challenge_table();
	install_character_join_vars();
	install_join_integrity_profile();
	install_expansion_version_root();
	install_join_token();
	install_join_cd_cookie();
	install_item_class_resolver();
	joiner_ = true;
	if (kernel_) {
		kernel_->world.projectile_authority = false;
		kernel_->world.mp_session = true;
	}
	joiner_bridge_.reset_for_join();
	joiner_applied_loadout_revision_ = 0;
	if (!session_.begin_connect().applied()) {
		joiner_ = false;
		return false;
	}
	return true;
}

bool Simulation::is_local_spectator() const {
	if (joiner_) return runtime_ != nullptr && runtime_->is_spectator();
	for (const opennova::np::NapiNPConnection &connection :
			ctx_.np_protocol.connection_list) {
		if (connection.type ==
				opennova::np::NapiNPConnection::kTypeClientSide) {
			return connection.link.spectator;
		}
	}
	return false;
}

bool Simulation::set_local_spectator(bool p_spectator) {
	// A joiner is non-authoritative: its S2C 0x75 state is intentionally
	// read-only. F3 mutates only the in-process SP/listen-host player.
	if (joiner_ || kernel_ == nullptr || !ctx_.is_authority) return false;
	for (opennova::np::NapiNPConnection &connection :
			ctx_.np_protocol.connection_list) {
		if (connection.type !=
				opennova::np::NapiNPConnection::kTypeClientSide) {
			continue;
		}
		if (!opennova::np::Server_SetPlayerSpectator(
					ctx_, connection, kernel_->world, p_spectator)) {
			return false;
		}
		kernel_->reset_local_player_input_to_player_facing();
		set_local_player_weapon_input(false, false, false);
		if (!p_spectator) respawn_local_player_loadout();
		return true;
	}
	return false;
}

bool Simulation::load_charattr_challenge(
		const Ref<ResourceRoot> &p_resource_root) {
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

void Simulation::set_join_world_ready(bool p_ready) {
	if (joiner_ && runtime_) runtime_->set_world_ready(p_ready);
}

void Simulation::finalize_loaded_model_challenge_snapshot() {
	if (!joiner_ || !runtime_) return;
	const int64_t count = ObjectData::network_challenge_model_count();
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

bool Simulation::poll_join_preload() {
	if (!joiner_ || !runtime_) return false;

	// This is the pre-mission subset of joiner_pump: the same UDP socket and
	// ClientRuntime advance the retail connect exchange, but no World exists yet
	// to tick and the runtime's world-ready gate suppresses the load/spawn drive.
	// The hello latch and the per-frame clock are the bridge's, shared with the
	// in-mission pump.
	joiner_bridge_.send_hello_once(*runtime_,
			[this](const std::vector<uint8_t> &dg) { ship_to_host(dg); });
	joiner_deposit_inbound();
	for (const std::vector<uint8_t> &dg :
			runtime_->Client_ProcessNetworkFrame(joiner_bridge_.now_tick())) {
		ship_to_host(dg);
	}
	joiner_bridge_.advance_tick();
	return true;
}

bool Simulation::is_join_preload_ready() const {
	return joiner_ && runtime_ && runtime_->preload_ready();
}

String Simulation::get_join_admission_stage() const {
	if (!joiner_ || !runtime_) {
		return String();
	}
	return String(runtime_->admission_stage_name());
}

bool Simulation::has_join_mission() const {
	return joiner_ && runtime_ && runtime_->mission_known();
}

// String::utf8, not the Latin-1 const char* constructor: the LAN browser rows
// decode the same wire fields as UTF-8, and both presentations of one host's
// metadata must agree byte-for-byte.
String Simulation::get_join_server_name() const {
	return (joiner_ && runtime_) ? String::utf8(runtime_->server_name().c_str()) : String();
}

String Simulation::get_join_mission_name() const {
	return (joiner_ && runtime_) ? String::utf8(runtime_->mission_name().c_str()) : String();
}

String Simulation::get_join_mission_file() const {
	return (joiner_ && runtime_) ? String::utf8(runtime_->map_file().c_str()) : String();
}

PackedByteArray Simulation::get_join_mission_header() const {
	PackedByteArray out;
	if (!joiner_ || !runtime_ || !runtime_->has_mission_header()) return out;
	const std::vector<uint8_t> &bytes = runtime_->mission_header_bytes();
	out.resize(static_cast<int64_t>(bytes.size()));
	if (!bytes.empty()) std::memcpy(out.ptrw(), bytes.data(), bytes.size());
	return out;
}

int64_t Simulation::get_join_terrain_til_state() const {
	if (!joiner_ || !runtime_) return JOIN_TERRAIN_TIL_ABSENT;
	switch (runtime_->terrain_til_state()) {
	case opennova::np::TerrainTilState::Absent:
		return JOIN_TERRAIN_TIL_ABSENT;
	case opennova::np::TerrainTilState::Receiving:
		return JOIN_TERRAIN_TIL_RECEIVING;
	case opennova::np::TerrainTilState::Complete:
		return JOIN_TERRAIN_TIL_COMPLETE;
	case opennova::np::TerrainTilState::Invalid:
		return JOIN_TERRAIN_TIL_INVALID;
	}
	return JOIN_TERRAIN_TIL_INVALID;
}

PackedByteArray Simulation::get_join_terrain_til() const {
	PackedByteArray out;
	if (!joiner_ || !runtime_ || !runtime_->has_terrain_til()) return out;
	const std::vector<uint8_t> &bytes = runtime_->terrain_til_bytes();
	out.resize(static_cast<int64_t>(bytes.size()));
	if (!bytes.empty()) std::memcpy(out.ptrw(), bytes.data(), bytes.size());
	return out;
}

String Simulation::get_join_expansion() const {
	return (joiner_ && runtime_) ? String::utf8(runtime_->expansion().c_str()) : String();
}

int64_t Simulation::get_join_game_type() const {
	return (joiner_ && runtime_) ? static_cast<int64_t>(runtime_->game_type()) : -1;
}

String Simulation::get_join_error() const {
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
String Simulation::get_session_loss_reason() const {
	if (!joiner_ || !runtime_) return String();
	return String::utf8(runtime_->session_loss_reason().c_str());
}

bool Simulation::is_session_lost() const {
	return joiner_ && runtime_ && runtime_->session_lost();
}

bool Simulation::is_joined_in_match() const {
	return joiner_ && runtime_ && runtime_->in_match();
}

bool Simulation::is_join_initial_admission_complete() const {
	return joiner_ && runtime_ && runtime_->initial_admission_complete();
}

bool Simulation::is_joiner_network_diagnostics_enabled() const {
	return joiner_net_diagnostics_;
}

void Simulation::set_joiner_network_diagnostics_enabled(bool p_enabled) {
	joiner_net_diagnostics_ = p_enabled;
}

void Simulation::set_capture_pcap_path(const String &p_path) {
	capture_pcap_path_ = p_path;
}

Dictionary Simulation::get_joiner_network_diagnostics() const {
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
	out["flat_seconds"] = joiner_bridge_.flat_seconds();
	out["freeze_suspected"] = joiner_bridge_.freeze_suspected();
	out["in_match"] = runtime_ && runtime_->in_match();
	out["deployed"] = runtime_ && runtime_->is_deployed();
	if (runtime_) {
		out["stage"] = String(runtime_->admission_stage_name());
		const opennova::np::JoinerConnection::ChallengeDiagnostics challenges =
				runtime_->challenge_diagnostics();
		Dictionary crc;
		crc["entity_checksum_seen"] = static_cast<int64_t>(challenges.entity_checksum_seen);
		crc["entity_checksum_answered"] =
				static_cast<int64_t>(challenges.entity_checksum_answered);
		crc["loadout_crc_seen"] = static_cast<int64_t>(challenges.loadout_crc_seen);
		crc["loadout_crc_answered"] = static_cast<int64_t>(challenges.loadout_crc_answered);
		crc["charattr_seen"] = static_cast<int64_t>(challenges.charattr_seen);
		crc["charattr_row_missing"] = static_cast<int64_t>(challenges.charattr_row_missing);
		crc["property_clears"] = static_cast<int64_t>(challenges.property_clears);
		out["challenges"] = crc;
		const opennova::np::JoinerConnection::JoinRejectRecord reject =
				runtime_->last_join_reject();
		if (reject.set) {
			Dictionary r;
			r["jfc"] = static_cast<int64_t>(reject.jfc);
			r["jfp"] = static_cast<int64_t>(reject.jfp);
			r["jfs"] = String::utf8(reject.jfs.c_str());
			out["last_reject"] = r;
		}
		if (runtime_->has_disconnect_event()) {
			const opennova::DisconnectEvent event = runtime_->last_disconnect_event();
			Dictionary d;
			d["dc"] = static_cast<int64_t>(event.dc);
			d["dpc"] = static_cast<int64_t>(event.dpc);
			d["ddstr"] = String::utf8(event.ddstr.c_str());
			d["dstr"] = String::utf8(event.dstr.c_str());
			out["last_disconnect"] = d;
		}
	}
	return out;
}

bool Simulation::is_join_deploy_pick_pending() const {
	return joiner_ && runtime_ && runtime_->deployment_pick_pending();
}

bool Simulation::is_join_deploy_overlay_active() const {
	// The deploy-map overlay (retail g_deploy_screen_active): armed by the S2C
	// 0x0F game_flags bit0, then host-maintained per frame from the 0x0A flags1
	// bit1. A UI signal only — it never gates the spawn. [orig: the folds
	// @0x42e2f8/@0x42ff82]
	return joiner_ && runtime_ && runtime_->state().deploy_overlay_active;
}

bool Simulation::take_join_deploy_overlay_open() {
	// The open latch and its clear are ClientState's (client_state.h
	// deploy_overlay_open_latch); this is the typed seam for the shell's frame.
	return joiner_ && runtime_ && runtime_->state().take_deploy_overlay_open();
}

int Simulation::get_join_assigned_team() const {
	return joiner_ && runtime_ ? runtime_->assigned_team() : 0;
}

int Simulation::get_class_allow_mask() const {
	if (joiner_ && runtime_) return runtime_->class_allow_mask();
	if (ctx_.is_authority != 0) return ctx_.config.class_allow_mask;
	return host_session_config_.class_allow_mask;
}

const opennova::world::SpawnZoneRegistry &Simulation::deploy_zone_registry() {
	// Built once per load; the zone set is authored (the world stream upserts state,
	// not membership). The joiner role hook resets the flag. [orig: Entity_BuildSpawnZoneList
	// @0x43EAE0 — rebuilt at mission start]
	if (!deploy_zone_registry_built_ && kernel_) {
		deploy_zone_registry_ = opennova::world::build_spawn_zone_list(kernel_->world);
		deploy_zone_registry_built_ = true;
	}
	return deploy_zone_registry_;
}

TypedArray<Dictionary> Simulation::get_deploy_spawn_zones() {
	// The DEATH screen's zone rows [orig: UI_UpdateDeathScreenContent @0x5536a0 —
	// def present, team match, SECURED (a numbered zone lists only at full control:
	// the zone-timer EntryById[9] >= [10] gate), attrib 0x40000; letter = 'A' +
	// registry index, name = WPNames/STRWPNAME%03d(index+1)]. The local BMS owns
	// membership/letter identity; live S2C 0x6F/0x53 owns team + control. Every
	// TEAM zone is emitted with its `secured` verdict: the second (occupant)
	// loop of the populate has no secured gate, so the engine builder decides
	// which rows list and where the occupants land.
	TypedArray<Dictionary> rows;
	if (!kernel_ || !joiner_ || !runtime_) return rows;
	const opennova::world::SpawnZoneRegistry &reg = deploy_zone_registry();
	const uint8_t team = runtime_->assigned_team();
	const opennova::netsim::ClientState &cs = runtime_->state();
	const uint16_t self_handle = runtime_->has_self_handle() ? runtime_->self_handle() : 0xFFFFu;
	for (size_t i = 0; i < reg.entries.size(); ++i) {
		const opennova::world::Entity *e = kernel_->world.registry.get(reg.entries[i]);
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
		Dictionary row;
		row["param"] = static_cast<int>(i) + 1;
		row["letter"] = String::chr('A' + static_cast<int>(i));
		row["name_key"] = vformat("STRWPNAME%03d", static_cast<int>(i) + 1);
		row["secured"] = !(e->zone_number != 0 && effective_control < effective_limit);
		// The 0x6E wave group on this zone: its countdown (entity+548) and the
		// queued members, named through the roster the way retail reads the
		// member entity's Name (the player entity's name IS the roster name)
		// [orig: dword_A85BC4[idx] / unk_A85CC4 @0x553cd0..0x553d8b, see world/deploy_screen_feed.h].
		int wave_countdown = 0;
		Array occupants;
		if (cs.spawn_waves.known) {
			for (const opennova::SpawnWaveGroup &g : cs.spawn_waves.value.groups) {
				if (g.zone_handle != e->handle.packed) continue;
				wave_countdown = g.wave_countdown;
				for (uint16_t member : g.members) {
					Dictionary o;
					o["handle"] = static_cast<int>(member);
					std::string name;
					const opennova::world::EntityHandle mh{member};
					for (const opennova::netsim::ClientRosterSlot &slot : cs.roster) {
						if (slot.bound && slot.entity_slot == mh.slot() && mh.pool() == 0) {
							name = slot.name;
							break;
						}
					}
					if (name.empty()) {
						if (const opennova::netsim::ClientEntityState *row_state = cs.find(member))
							name = row_state->name;
					}
					o["name"] = String::utf8(name.c_str());
					o["self"] = member == self_handle;
					occupants.push_back(o);
				}
			}
		}
		row["wave_countdown"] = wave_countdown;
		row["occupants"] = occupants;
		rows.push_back(row);
	}
	return rows;
}

bool Simulation::send_deployment_pick(int p_param) {
	// [orig: Input_HandleActionBinding case 12 @0x49b0c5-0x49b17b — param 0 -> 0xFFFF,
	// 65534 -> 0xFFFE, else SpawnZoneList_GetByIndex(param-1) -> the entity handle;
	// an index that resolves no entity falls through to 0xFFFF @0x49b17b LABEL_70]
	if (!joiner_ || !runtime_) return false;
	uint16_t wire = opennova::world::kDeployPickNone;
	if (p_param == 65534) {
		wire = opennova::world::kDeployPickAutoTeam;
	} else if (p_param > 0) {
		const opennova::world::SpawnZoneRegistry &reg = deploy_zone_registry();
		const size_t idx = static_cast<size_t>(p_param - 1);
		if (idx < reg.entries.size()) wire = reg.entries[idx].packed;
	}
	return runtime_->queue_deployment_pick(wire);
}

int Simulation::get_joiner_phase() const {
	return (joiner_ && runtime_) ? static_cast<int>(runtime_->phase()) : -1;
}

int Simulation::get_joiner_self_handle() const {
	return joiner_ ? static_cast<int>(joiner_bridge_.self_wire_handle()) : 0;
}

// [orig: CNapiNPConnection_TeardownActiveConnection @0x6253c0 — the leave sends a burst of
// 0x46 disconnect packets (SendDisconnectPacket @0x61f2a0) before the key material clears; the
// host's non-timeout teardown fires only on that opcode (Nwu_HandleClientGoodbye @0x624250)]
void Simulation::leave_net_session() {
	if (!joiner_ || runtime_ == nullptr) return;
	for (const std::vector<uint8_t> &dg : runtime_->disconnect()) ship_to_host(dg);
}


void Simulation::ship_to_host(const std::vector<uint8_t> &dg) {
	if (pump_.is_null() || dg.empty()) return;
	PackedByteArray bytes;
	bytes.resize(static_cast<int64_t>(dg.size()));
	std::memcpy(bytes.ptrw(), dg.data(), dg.size());
	pump_->send_to_host(bytes);
}

// (P7 A4: joiner_net_poll / joiner_net_flush deleted — the joiner now runs through joiner_pump
//  over an np::ClientRuntime; the legacy JoinerSession path is retired here.)

bool Simulation::admit_test_remote_peer(Vector3 p_position, float p_yaw_deg, int p_team) {
	if (!host_listen_ || !kernel_->world.ai) return false;
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
					ctx_, kernel_->world, peer, spawn, link.transport.get());
	kernel_->resolve_new_infantry_adm_ids();
	return h.valid();
}


// Drain this frame's folded S2C 0x1E game events into feed rows. Actor names
// resolve here against the decoded roster (a pool-0 INDEX on the wire becomes
// the handle (0<<12)|index); the canned-message key, the camp key's team
// suffix, and the line color all come from the witnessed policy in
// engine/runtime/hud/feed_format.h. Suppressed types never surface
// [orig: the LFP result set formats and returns @0x42702E-@0x42716D;
// 58 posts to the tip system only @0x427202].
Array Simulation::drain_feed_events() {
	Array out;
	if (!runtime_) return out;
	opennova::netsim::ClientState &cs = runtime_->state();
	const uint16_t self_handle =
			runtime_->has_self_handle() ? runtime_->self_handle() : 0xFFFF;
	auto name_of = [&cs](uint8_t index) -> String {
		if (index == 0xFF) return String();
		const opennova::netsim::ClientEntityState *e =
				cs.find(static_cast<uint16_t>(index));
		return e != nullptr ? String::utf8(e->name.c_str()) : String();
	};
	// A line the local player took no part in posts only while the MP verbose
	// toggle is on [orig: g_MpVerbose2 @0x24D2154, seeded verbose-on from
	// the session settings @0x551D0F]. The keybind that flips it (@0x49B78F,
	// STRMISC_VERBOSE_ON/OFF) is unported, so the seed default stands.
	constexpr bool mp_verbose = true;
	for (const opennova::netsim::ClientGameEvent &ev : runtime_->drain_game_events()) {
		if (opennova::hud::feed_event_suppressed(ev.event_type)) continue;
		// Camp events reuse the slots: attacker is the LEVEL index and victim
		// is the TEAM byte, and their key gets a client-side team suffix
		// [orig: case 59 @0x4272D7 / case 60 @0x4273DC].
		const bool camp = ev.event_type == 59 || ev.event_type == 60;
		const bool own =
				!camp && self_handle != 0xFFFF &&
				(static_cast<uint16_t>(ev.attacker_index) == self_handle ||
				 static_cast<uint16_t>(ev.victim_index) == self_handle);
		if (!own && !mp_verbose &&
				opennova::hud::feed_event_verbose_only(ev.event_type)) {
			continue;
		}
		const std::string camp_key =
				camp ? opennova::hud::feed_camp_key(ev.event_type, ev.victim_index)
				     : std::string();
		if (camp && camp_key.empty()) continue;   // team outside 1/2 draws nothing
		const char *key = camp ? camp_key.c_str()
		                       : opennova::game_event_strcnd_key(ev.event_type);
		if (key == nullptr) continue;   // team/gametype-keyed at runtime — not ported
		// The aux slot carries the bonus-credited player; only when that is
		// the LOCAL player does retail re-compose the line through STRCND48
		// "%s - Bonus for %s" with their name [orig: the 4th
		// HUD_FormatKillEventMessage arg @0x422F5F -> the sprintf @0x422CA2].
		String extra;
		if (!camp && self_handle != 0xFFFF && ev.aux_index != 0xFF &&
				static_cast<uint16_t>(ev.aux_index) == self_handle) {
			extra = name_of(ev.aux_index);
		}
		Dictionary d;
		d["event_type"] = ev.event_type;
		d["kind"] = static_cast<int>(ev.kind);
		// The engine decides the line's compose form; the presenter branches
		// on this, never on which keys happen to be present.
		d["camp"] = camp;
		d["key"] = String::utf8(key);
		d["attacker"] = camp ? String() : name_of(ev.attacker_index);
		d["victim"] = camp ? String() : name_of(ev.victim_index);
		d["extra"] = extra;
		if (camp) {
			// The camp template's %s takes the WPNames string of the level
			// slot — index PLUS ONE [orig: sprintf @0x4272EC/@0x4273F1].
			d["wpname_key"] = String::utf8(
					opennova::hud::feed_camp_wpname_key(ev.attacker_index).c_str());
		}
		d["color"] = static_cast<int64_t>(opennova::hud::feed_event_color(
				ev.event_type, own, camp ? ev.victim_index : 0));
		d["own"] = own;
		out.push_back(d);
	}
	return out;
}

String Simulation::format_feed_line(const String &p_template, const String &p_attacker,
		const String &p_victim, const String &p_extra,
		const String &p_bonus_template) const {
	return String::utf8(opennova::hud::feed_format_line(
			p_template.utf8().get_data(), p_attacker.utf8().get_data(),
			p_victim.utf8().get_data(), p_extra.utf8().get_data(),
			p_bonus_template.utf8().get_data())
			                    .c_str());
}

String Simulation::format_feed_camp_line(const String &p_template,
		const String &p_wpname) const {
	return String::utf8(opennova::hud::feed_format_camp_line(
			p_template.utf8().get_data(), p_wpname.utf8().get_data())
			                    .c_str());
}


// One wire row's REMOTE-body sounds for this frame: resolve the row's clip,
// scan the trigger words its wire-driven playhead crossed, and hand the
// witnessed consume to the portable engine leg
// (world::wire_body_slot_sounds; the witness map lives in
// world/wire_body_sound.h and docs/audio/lwf-dbf-sound-re.md).
//
// Exactly one sound source per drawn body: this consume runs only for rows
// the wire pass renders (a joiner's remote rows; a listen host's admitted
// players — authored rows defer to the authority presenter and never enter
// the wire plan), and the authority tick's sound pass never reaches
// net-snapped peers (tick_infantry returns at the net-snap gate before the
// consume, the gate AiSystem::tick_infantry cites; each machine instead
// consumes from the body updater of every body it draws; see
// docs/audio/lwf-dbf-sound-re.md).
void Simulation::present_wire_body_sounds(int p_type_id, int p_character_id,
		int p_wire_handle, int p_carrier_handle, int p_anim_state,
		int p_from_phase, int p_to_phase, const Vector3 &p_pos) {
	if (!world_installed_ || !kernel_ || p_anim_state < 0) return;
	if (p_to_phase <= p_from_phase) return;
	const auto adm = client_row_adm_by_type_.find(static_cast<uint16_t>(p_type_id));
	if (adm == client_row_adm_by_type_.end() || adm->second < 0) return;

	uint32_t words[16] = {};
	const int n = kernel_->root_motion.scan_triggers(adm->second, p_anim_state,
			p_from_phase, p_to_phase, words, 16, /*variant=*/0);
	if (n <= 0) return;
	// The dip belongs to the frame the playhead ended on (a multi-frame
	// catch-up dips all its words by the final frame's bottom — the scan
	// carries no per-word phases).
	const int32_t capsule_bottom =
			kernel_->root_motion.capsule_bottom_at(adm->second, p_anim_state, p_to_phase,
					/*variant=*/0);
	// Godot (x, y, z) -> mission (x, -z, y) 16.16, the drain's own convention.
	const int32_t body[3] = { static_cast<int32_t>(p_pos.x * 65536.0f),
		                      static_cast<int32_t>(-p_pos.z * 65536.0f),
		                      static_cast<int32_t>(p_pos.y * 65536.0f) };
	opennova::world::wire_body_slot_sounds(kernel_->world, words, n, capsule_bottom,
			p_type_id, static_cast<uint16_t>(p_character_id),
			static_cast<uint16_t>(p_wire_handle), p_carrier_handle >= 0, body);
}

// The Tab board's header as the shell needs it. Row data no longer rides a
// script Dictionary: HudOverlay pulls the drawn rows natively through
// fill_scoreboard_rows, and the counts here come from the same netsim
// projection (netsim::scoreboard_header — the accepted-rows-minus-spectators
// players count is the witnessed header arithmetic, retail @0x4231dd).
Dictionary Simulation::get_scoreboard() const {
	Dictionary out;
	if (!runtime_) return out;
	const opennova::netsim::ClientScoreboardHeader header =
			opennova::netsim::scoreboard_header(runtime_->state());
	out["known"] = header.known;
	out["team_mode"] = header.team_mode;
	out["timed"] = header.timed;
	out["players"] = header.players;
	out["in_game"] = header.in_game;
	out["spectators"] = header.spectators;
	// The drawer branches on the session game type (retail reads g_GameType
	// @0x423acb); the header's session strings ride along — joiner-decoded,
	// empty on a host until the host sessionvars are plumbed (D-HUD-24).
	out["game_type"] = static_cast<int64_t>(runtime_->game_type());
	out["server"] = String::utf8(runtime_->server_name().c_str());
	out["mission"] = String::utf8(runtime_->mission_name().c_str());
	return out;
}

bool Simulation::fill_scoreboard_rows(
		std::vector<opennova::hud::ScoreboardEntry> &r_rows) const {
	if (!runtime_) {
		r_rows.clear();
		return false;
	}
	opennova::netsim::project_scoreboard(runtime_->state(), r_rows);
	return true;
}

int Simulation::scoreboard_team_count() const {
	if (!runtime_) return 0;
	return static_cast<int>(runtime_->state().scoreboard.team_count);
}
