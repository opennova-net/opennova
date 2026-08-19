// Simulation — the net roles (ADR 0009/0011/0012): per-load listen-host
// bring-up + host pump, the LAN joiner pump family + wire proxies/events, the
// host session config FFI, and the joiner preload/session API.
#include "simulation/nova_simulation_internal.h"

#include <cmath>
#include <cstring>

#include <npruntime/session_status.h>
#include <terrain_query/surface_tiles.h> // surface_tiles_from_til_bytes (D-SND-15)
#include <threedi/threedi_panm_pose.h> // the native PANM liveness gate (S3, ADR 0028)
#include <npwire/ingame_decode.h> // kRoundEventFlag* (the fire-mode byte)
#include <npwire/ingame_message_id.h>
#include <hud/feed_format.h> // the witnessed feed line/color policy
#include <npwire/net_ports.h> // lan_host_bind_ports (the D-NET-210 bind scan)
#include <vfs/vfs.h> // vfs_expansion_version_checksum (the D-NET-166 JOIN CRC)
#include <npwire/wire_handle.h>  // pool()/kPoolItem/kInvalid (the wire handle home)
#include <world/infantry.h>      // kAnimStanceFlag* (the witnessed stance bits)
#include <world/spawn_select.h>  // kDeployPickNone/AutoTeam (C2S 0x2C sentinels)
#include <world/vehicle_motor.h> // carrier_pose_fixed (the deck-ride pose reader)
#include <rtxt/rtxt.h>
#include <world/destruction.h>  // destruction_notify_item_damage (S2C 0x13 net kill)
#include <world/entity_spawn.h> // entity_reset_to_spawn_state (redeploy release)
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace novasim;

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
// select_player_spawn start marker), and its own loopback client renders the per-frame 0x0A.
void Simulation::bringup_host_runtime(const opennova::bms::File &file) {
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
	ctx_.world = world_.get();
	ctx_.mission = &mission_file_;
	ctx_.terrain_til_data = terrain_til_data_; // S2C 0x45 terrain-tile load source (empty => skipped, §5.37)
	ctx_.mission_text_loaded = mission_text_loaded_;
	ctx_.mission_briefing3 = mission_briefing3_;
	ctx_.mission_briefing2 = mission_briefing2_;
	ctx_.mission_location_names.clear();
	int32_t location_index = 1;
	for (const opennova::bms::Entity &marker : mission_file_.markers) {
		if (marker.type_id != 2044) continue;
		// Retail assigns LOCATION001.. in type-2044 marker spawn order. The
		// BMS ttool_index is zero for both 00TRg markers and is not the text key.
		const auto found = mission_location_texts_.find(location_index);
		std::string label;
		if (found != mission_location_texts_.end()) {
			label = found->second;
		} else {
			const std::string suffix = std::to_string(location_index);
			label = "LOCATION";
			if (suffix.size() < 3) label.append(3 - suffix.size(), '0');
			label += suffix;
		}
		// Retail stores the resolved text in a 64-byte location-name slot.
		if (label.size() > 63) label.resize(63);
		ctx_.mission_location_names.push_back(std::move(label));
		++location_index;
	}
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
	// The shell's resolved PLAYER_INFO selection for the host's own player; the
	// HostConfig default is the stock fresh-profile seed until one is installed.
	if (local_character_vars_set_) {
		host_cfg.local_character_vars = local_character_vars_;
	}
	np::start_host_session(host_owner_, host_cfg);
	if (serve_and_play) {
		// The host's own replica pipeline (HostClient role: recv-fold only, 0x0C suppressed). Folds host_loop_
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
class NovaUdpPumpDatagramSocket : public opennova::netsim::IDatagramSocket {
public:
	explicit NovaUdpPumpDatagramSocket(UdpPump *pump) : pump_(pump) {}

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
	UdpPump *pump_;
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
// (engine/net/npruntime) — the SAME loop apps/nw_server runs, so the headless server and the Godot binding can no
// longer drift. Simulation supplies the socket (a UdpPump adapter; SP passes a null pump and the
// loop's socket legs go inert) and folds the host's own loopback 0x0A into ClientState for the present
// pass (serve_and_play: host_session_pump skips the loopback discard so we can read it here).
void Simulation::resolve_infantry_adm_before_server_tick(void *p_context) {
	if (p_context == nullptr) return;
	static_cast<Simulation *>(p_context)->resolve_new_infantry_adm_ids();
}

void Simulation::drain_host_client_gameplay_requests() {
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

void Simulation::host_pump() {
	namespace np = opennova::np;
	// Server_SendRandomSeedSync's non-dedicated S2C 0x68 cursor advances by 50
	// and wraps against the current renderer viewport height [orig:
	// Server_SendRandomSeedSync @ 0x511360 — CEffectWorld_GetViewportDimensions
	// @ 0x5b1560 (call @ 0x511375), wrap @ 0x511391]. Refresh the portable
	// runtime seam on every frame so resizing is observable; the retail parity
	// runbook pins this root viewport to 1920x1080. The runtime owns this node
	// without parenting it into the tree (manual pump ordering, ADR 0011), so
	// get_viewport() alone is null on every production host: resolve the render
	// window the way retail's CEffectWorld query does — the live window. A
	// headless DisplayServer has no renderer (the dedicated-host analogue), and
	// a missing/non-drawable viewport leaves the seam explicitly unset:
	// npruntime suppresses 0x68 instead of inventing a screen size
	// (docs/net/novaworld-net-re.md §5.34, D-NET-206).
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
	ctx_.loaded_model_viewport_height = viewport_height > 0.0
			? static_cast<uint32_t>(viewport_height) : 0u;
	const uint32_t now = host_owner_.now_tick;
	drain_host_client_gameplay_requests();
	apply_player_input_pre_tick(); // input -> the host player's body input, before logic (ADR 0009/0012)
	NovaUdpPumpDatagramSocket sock(host_listen_ ? pump_.ptr() : nullptr);
	np::host_session_pump(host_owner_, sock,
			&Simulation::resolve_infantry_adm_before_server_tick, this);
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

// host_pump's dispatch_event + admit_peer were promoted into engine/net/npruntime (np::dispatch_event /
// np::admit_peer over host_owner_, driven by host_session_pump) — the SAME code apps/nw_server runs, so
// the Godot binding and the headless server can no longer drift.

// Stamp each decoded Player/Infantry row's .adm registry id from its wire
// type (visual item -> anim_def -> register_adm), once per row; -1 = no adm
// (the row stays chase-only, truthful). Cached per type so late-joining
// peers and respawns cost one map lookup.
void Simulation::resolve_client_row_adm_ids() {
	if (runtime_ == nullptr || infantry_anim_.empty() ||
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
			adm_id = infantry_anim_.register_adm(
					infantry_adm_resource_root_.is_valid()
							? &infantry_adm_resource_root_->native_index()
							: nullptr,
					std::string(adm.utf8().get_data()));
		}
		if (adm_id < 0 && !infantry_anim_.empty()) adm_id = 0; // the default set
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
				collision_resolve_.resolution_attempted.find(lifetime.handle.packed);
		// A later allocation at the same packed handle may already have had
		// its caches rebuilt. The retired lifetime cannot erase those.
		if (cached != collision_resolve_.resolution_attempted.end() &&
				cached->second != lifetime.registry_spawn_id)
			continue;
		const opennova::world::EntityHandle handle = lifetime.handle;
		collision_world_.remove_entity_instance(handle);
		occlusion_world_.remove_entity_instance(handle);
		collision_pose_native_.remove_entity(handle);
		collision_resolve_.resolution_attempted.erase(handle.packed);
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
					world_->registry.get(lifetime))
				refresh_item_seat_spec(*entity);
		}
	};
	refresh_seats(p_sync.spawned);
	refresh_seats(p_sync.updated);
	// The wire materializer deliberately does not fabricate seat
	// definitions. Item/model resolution above installs them, then this
	// idempotent fold projects the retained 0x0D mountHandles image by each
	// seat's fixed retail_slot.
	(void)joiner_bridge_.materializer().sync(runtime_->state(), *world_);

	if (collision_item_db_.is_valid())
		resolve_collision_instances(collision_item_db_);
}

Dictionary Simulation::get_client_entity_debug(int p_handle) const {
	Dictionary out;
	if (runtime_ == nullptr || p_handle < 0 || p_handle > 0xFFFF) return out;
	for (const opennova::netsim::ClientEntityState &es :
			runtime_->state().entities) {
		if (es.handle != static_cast<uint16_t>(p_handle)) continue;
		out["handle"] = static_cast<int>(es.handle);
		out["type_id"] = static_cast<int>(es.type_id);
		out["cls"] = static_cast<int>(es.cls);
		out["net_id"] = static_cast<int>(es.net_id);
		out["name"] = String(es.name.c_str());
		out["carrier_handle"] = static_cast<int>(es.carrier_handle);
		out["mount_bone"] = static_cast<int>(es.mount_bone);
		out["seat_type"] = static_cast<int>(es.seat_type);
		out["net_seat_valid"] = es.net_seat_valid;
		out["heading_bam"] = es.heading_bam;
		out["heading_deg"] =
				opennova::world::mission_yaw_deg_from_bam_heading(es.heading_bam);
		out["heading_known"] = es.heading_known;
		out["pitch_bam"] = es.pitch_bam;
		out["yaw_byte"] = static_cast<int>(es.yaw_byte);
		out["mission_position"] = Vector3(
				static_cast<float>(es.x) / 65536.0f,
				static_cast<float>(es.y) / 65536.0f,
				static_cast<float>(es.z) / 65536.0f);
		out["anim_state_id"] = static_cast<int>(es.anim_state_id);
		out["state_flags"] = static_cast<int>(es.state_flags);
		out["state_flags_known"] = es.state_flags_known;
		out["team"] = static_cast<int>(es.team);
		out["compact_revision"] = static_cast<int64_t>(es.compact_revision);
		out["spawn_revision"] = static_cast<int64_t>(es.spawn_revision);
		return out;
	}
	return out;
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
			runtime_profiling_enabled_ ? perf_now_us() : 0;
	// The frame itself — provider wiring, hello, recv-fold + uplink + folds,
	// the decoded-consequence application, the local World tick, and the
	// post-tick recompose — lives in the engine bridge (S10a, ADR 0028). This
	// binding supplies the shell legs: the socket, the render-coupled asset
	// resolution, the loadout profile seams (S7b disposition), device input,
	// the view/weapon pumps shared with the host path, and the clocks.
	opennova::np::JoinerWorldBridge::PumpContext ctx{
			*world_, *runtime_, local_weapon_, local_loadout_,
			local_inventory_, local_inventory_valid_, item_seat_specs_,
			infantry_anim_.empty() ? nullptr : &infantry_anim_};
	opennova::np::JoinerWorldBridge::PumpHooks hooks;
	hooks.send = [this](const std::vector<uint8_t> &dg) { ship_to_host(dg); };
	hooks.deposit_inbound = [this] { joiner_deposit_inbound(); };
	hooks.resolve_row_adm_ids = [this] { resolve_client_row_adm_ids(); };
	if (runtime_profiling_enabled_)
		hooks.on_wire_leg_complete = [this, net_start] {
			last_net_tick_us_ = perf_now_us() - net_start;
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
		player_view_.binoculars_requested = false;
		binocular_yaw_offset_deg_ = 0.0f;
		binocular_pitch_offset_deg_ = 0.0f;
		refresh_local_player_view_effects();
		sync_local_mounted_input_heading();
	};
	hooks.wire_collision_shape = [this](uint16_t type_id, int32_t &model_id,
			float &bound_radius) {
		const WireCollisionShape shape = wire_collision_shape_for_type(type_id);
		model_id = shape.model_id;
		bound_radius = shape.bound_radius;
	};
	hooks.apply_input_pre_tick = [this] { apply_player_input_pre_tick(); };
	hooks.sync_mounted_input_heading =
			[this] { sync_local_mounted_input_heading(); };
	hooks.tick_view = [this] { tick_local_player_view(); };
	hooks.tick_weapon = [this] { tick_local_player_weapon(); };
	joiner_bridge_.pump(ctx, hooks);
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
	if (joiner_bridge_.local_spawned() && world_ && world_->ai) {
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
	resolve_new_infantry_adm_ids();
	player_input_ = opennova::world::PlayerInput{};
	player_input_.look_heading = p_look_heading_bam;
	stance_latch_ = 0;
	look_px_accum_x_ = look_px_accum_y_ = 0.0f;
}

// L revived on an ACK-qualified redeploy release: the same input-latch reset
// from the redeployed authoritative heading, plus the respawn loadout rebuild.
void Simulation::on_joiner_local_player_redeployed(int32_t p_look_heading_bam) {
	player_input_ = opennova::world::PlayerInput{};
	player_input_.look_heading = p_look_heading_bam;
	stance_latch_ = 0;
	look_px_accum_x_ = look_px_accum_y_ = 0.0f;
	respawn_local_player_loadout();
}

Simulation::WireCollisionShape Simulation::wire_collision_shape_for_type(
		uint16_t p_type_id) {
	const auto cached = wire_collision_shape_by_type_.find(p_type_id);
	if (cached != wire_collision_shape_by_type_.end()) return cached->second;
	WireCollisionShape shape;
	if (collision_item_db_.is_valid()) {
		// The same items.def graphic resolution the registry sweep runs
		// (resolve_collision_instances), keyed by the WIRE type id. Sharing the
		// by-graphic caches means a mission whose local load already registered
		// this graphic reuses the exact model id the ghost had.
		const int def_id =
				static_cast<int>(p_type_id) + opennova::mission::kItemIdOffset;
		const String graphic = collision_item_db_->get_graphic(def_id);
		if (!graphic.is_empty()) {
			const std::string key(graphic.utf8().get_data());
			// ADR 0031: the joiner's wire ghosts register through the SAME engine
			// leg as the registry sweep (simassets::collision_model_for_graphic)
			// — one implementation, one cache, identical model ids.
			const opennova::simassets::CollisionResolveDeps deps{
					collision_world_, occlusion_world_, collision_pose_native_,
					sim_models_};
			shape.model_id = opennova::simassets::collision_model_for_graphic(
					collision_resolve_, deps, key);
			shape.bound_radius = collision_resolve_.radius_by_graphic[key];
		}
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
	if (world_) {
		world_->projectile_authority = true;
		world_->mp_session = true;
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
			UtilityFunctions::push_error(
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
	if (world_ && host_listen_) {
		world_->fat_bullets = host_session_config_.fat_bullets;
		world_->one_shot_kill = host_session_config_.one_shot_kill;
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
	out["gametype"] = static_cast<int64_t>(session.game_type);
	out["mpattrib"] = static_cast<int64_t>(session.mp_attributes);
	out["class_allow_mask"] = static_cast<int64_t>(session.class_allow_mask);
	out["respawn_time"] = static_cast<int64_t>(session.respawn_time);
	out["time_limit_minutes"] = static_cast<int64_t>(session.time_limit_minutes);
	out["replay_enabled"] = static_cast<int64_t>(session.replay_enabled);
	out["max_team_lives"] = static_cast<int64_t>(session.max_team_lives);
	out["score_limit"] = static_cast<int64_t>(session.score_limit);
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

void Simulation::set_join_expansion_version_root(const String &p_game_root) {
	// D-NET-166: the JOIN VERSIONCRCSTRING checksum source. The runtime CRCs
	// the loose expansion/<SUS2>/version.txt under this root at JOIN-build
	// time (see JoinerConnection::set_expansion_version_root).
	join_expansion_version_root_ = std::string(p_game_root.utf8().get_data());
	install_expansion_version_root();
}

// ---- co-op LAN joiner (D.2) -------------------------------------------------

bool Simulation::enable_join(const String &p_host_ip, int p_port, const String &p_player_name) {
	// P7: the joiner is a non-authority np::ClientRuntime (Joiner role) built per-load in finish_load;
	// it owns the connect-leg state machine + the S2C->ClientState fold internally. Here we only dial
	// the socket + store the player name (the ClientAuth.NA the host echoes for the name-match). Leave
	// listen_server_ false (the present gate adds || joiner_); a sim is host XOR joiner.
	// Validate the lifecycle transition before opening a socket. Re-dialing a
	// live mission is rejected without partially replacing its transport.
	const opennova::np::TransitionResult role = mission_session_.configure_role(
			opennova::np::MissionSessionRole::Joiner);
	if (role.code != opennova::np::TransitionCode::Applied &&
			role.code != opennova::np::TransitionCode::NoOp) {
		return false;
	}
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
	install_join_integrity_profile();
	install_expansion_version_root();
	install_item_class_resolver();
	joiner_ = true;
	if (world_) {
		world_->projectile_authority = false;
		world_->mp_session = true;
	}
	joiner_bridge_.reset_for_join();
	joiner_environment_revision_seen_ = 0;
	joiner_applied_loadout_revision_ = 0;
	if (!mission_session_.begin_connect().applied()) {
		joiner_ = false;
		return false;
	}
	return true;
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
	return environment_flag_enabled(kJoinerNetDiagnosticsEnv);
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
	return out;
}

bool Simulation::is_join_deploy_pick_pending() const {
	return joiner_ && runtime_ && runtime_->deployment_pick_pending();
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
	// not membership). finish_load resets the flag. [orig: Entity_BuildSpawnZoneList
	// @0x43EAE0 — rebuilt at mission start]
	if (!deploy_zone_registry_built_ && world_) {
		deploy_zone_registry_ = opennova::world::build_spawn_zone_list(*world_);
		deploy_zone_registry_built_ = true;
	}
	return deploy_zone_registry_;
}

TypedArray<Dictionary> Simulation::get_deploy_spawn_zones() {
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

Dictionary Simulation::take_join_environment_update() {
	Dictionary out;
	if (!joiner_ || runtime_ == nullptr) return out;
	const opennova::netsim::ClientEnvironmentState &environment =
			runtime_->state().environment;
	if (!environment.present ||
			environment.revision == joiner_environment_revision_seen_)
		return out;
	joiner_environment_revision_seen_ = environment.revision;
	out["revision"] = static_cast<int64_t>(environment.revision);
	out["fog_dist"] = static_cast<int64_t>(environment.fog_dist);
	out["fog_accel"] = static_cast<int64_t>(environment.fog_accel);
	out["tod_fixed"] = static_cast<int64_t>(environment.tod_fixed);
	out["quake_ticks"] = static_cast<int64_t>(environment.quake_ticks);
	out["cloud_scroll"] = static_cast<int64_t>(environment.cloud_scroll);
	out["rain_pct"] = static_cast<int64_t>(environment.rain_pct);
	out["overcast"] = static_cast<int64_t>(environment.overcast);
	out["precipitation_kind"] = static_cast<int64_t>(environment.env_param);
	return out;
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


// Drain this frame's folded S2C 0x1E game events into feed rows. Actor names
// resolve here against the decoded roster (a pool-0 INDEX on the wire becomes
// the handle (0<<12)|index); the canned-message key, the camp key's team
// suffix, and the line color all come from the witnessed policy in
// engine/runtime/hud/feed_format.h. Suppressed types never surface
// (retail: the LFP result set formats and returns @0x62051-0x62084; 58 posts
// to the tip system only @0x62147).
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
	for (const opennova::netsim::ClientGameEvent &ev : runtime_->drain_game_events()) {
		if (opennova::hud::feed_event_suppressed(ev.event_type)) continue;
		const bool own =
				self_handle != 0xFFFF &&
				(static_cast<uint16_t>(ev.attacker_index) == self_handle ||
				 static_cast<uint16_t>(ev.victim_index) == self_handle);
		// Camp events reuse the slots: attacker is the LEVEL index and victim
		// is the TEAM byte, and their key gets a client-side team suffix
		// (retail: case 59 @0x62165 / case 60 @0x62190).
		const bool camp = ev.event_type == 59 || ev.event_type == 60;
		const std::string camp_key =
				camp ? opennova::hud::feed_camp_key(ev.event_type, ev.victim_index)
				     : std::string();
		if (camp && camp_key.empty()) continue;   // team outside 1/2 draws nothing
		const char *key = camp ? camp_key.c_str()
		                       : opennova::game_event_strcnd_key(ev.event_type);
		if (key == nullptr) continue;   // team/gametype-keyed at runtime — not ported
		Dictionary d;
		d["event_type"] = ev.event_type;
		d["kind"] = static_cast<int>(ev.kind);
		d["key"] = String::utf8(key);
		d["attacker"] = camp ? String() : name_of(ev.attacker_index);
		d["victim"] = camp ? String() : name_of(ev.victim_index);
		d["level"] = camp ? ev.attacker_index : 0;   // camp: the WPNames level index
		d["color"] = static_cast<int64_t>(opennova::hud::feed_event_color(
				ev.event_type, key, own, camp ? ev.victim_index : 0));
		d["own"] = own;
		out.push_back(d);
	}
	return out;
}

String Simulation::format_feed_line(const String &p_template, const String &p_attacker,
		const String &p_victim) const {
	return String::utf8(opennova::hud::feed_format_line(
			p_template.utf8().get_data(), p_attacker.utf8().get_data(),
			p_victim.utf8().get_data())
			                    .c_str());
}
