// Simulation — core lifecycle: construction/reset, mission load + step,
// finish_load, restart, WAC install/state, mission variables, perf counters.
// The class spans several TUs; see nova_simulation_internal.h for the map.
#include "simulation/nova_simulation_internal.h"

#include <mission/runtime_boot.h> // the S9 boot order + file-resolution policy
#include <terrain_query/surface_tiles.h> // the D-SND-15 placed-tile resolvers

using namespace novasim;

namespace {

// Build the same synthetic patrol mission the C++ promote_test uses: 3 markers forming a
// path, one looping waypoint record (channel 1), 2 organics on that route, 1 building.
opennova::bms::File make_demo_mission() {
	opennova::bms::File m{};

	auto marker = [](int32_t x, int32_t y, int32_t z) {
		opennova::bms::Entity e{};
		e.type = opennova::bms::ItemType::Marker;
		e.x = x; e.y = y; e.z = z;
		return e;
	};
	auto organic = [](int32_t x, int32_t y, int32_t z, uint8_t team, uint8_t wp_id) {
		opennova::bms::Entity e{};
		e.type = opennova::bms::ItemType::Organic;
		e.x = x; e.y = y; e.z = z;
		e.yaw = 90;
		e.team = team;
		e.waypoint_id = wp_id;
		e.wp_number = 0;
		e.min_engagement_distance = 50 << 16;
		e.max_engagement_distance = 500 << 16;
		return e;
	};

	m.markers.push_back(marker(100 << 16, 0, 0));
	m.markers.push_back(marker(200 << 16, 0, 0));
	m.markers.push_back(marker(300 << 16, 0, 0));

	// The waypoint table is POSITIONAL — slot index == authored list id, slot 0
	// = the reserved no-route id. The looping patrol the organics author as
	// wp_id 1 sits at slot 1; the BLUE-flagged HUD route at slot 2.
	m.waypoint_records.resize(3);
	m.waypoint_records[1].flags = opennova::bms::WaypointFlags::None; // loops
	m.waypoint_records[1].marker_count = 3;
	m.waypoint_records[1].waypoint_numbers = {0, 1, 2};

	// A BLUE-flagged player route over the same markers: promotion builds the
	// HUD waypoint track from the first such record (marker 0 authors a wide
	// radius + a name id so the view surfaces meaningful fields).
	m.markers[0].wp_distance = 25;
	m.markers[0].ttool_index = 1;
	m.waypoint_records[2].flags = opennova::bms::WaypointFlags::BlueTeam;
	m.waypoint_records[2].marker_count = 3;
	m.waypoint_records[2].waypoint_numbers = {0, 1, 2};

	m.organics.push_back(organic(0, 0, 0, /*team=*/1, /*wp_id=*/1));
	m.organics.push_back(organic(50 << 16, 0, 0, /*team=*/2, /*wp_id=*/1));

	opennova::bms::Entity bldg{};
	bldg.type = opennova::bms::ItemType::Building;
	bldg.x = 999 << 16;
	m.buildings.push_back(bldg);

	// Authored SSNs (promotion copies record ids verbatim, like the original).
	m.organics[0].id = 1;
	m.organics[1].id = 2;
	m.buildings[0].id = 3;
	m.markers[0].id = 10;
	m.markers[1].id = 11;
	m.markers[2].id = 12;
	return m;
}

} // namespace

Simulation::Simulation() : mission_session_(*this) {
	reset_world();
	set_process(false);
}

Simulation::~Simulation() {
	(void)mission_session_.close();
	if (weapon_defs_loaded_) {
		def_free_weapons(&weapon_defs_);
		weapon_defs_loaded_ = false;
	}
}

void Simulation::reset_world() {
	joiner_bridge_.reset_world_stream();
	invalidate_present_effect_pose_cache();
	local_weapon_.events.clear();
	local_weapon_.anim_tick = 0;
	local_weapon_.round_sequence = 0;
	local_weapon_.active = false;
	local_weapon_.fire_held = false;
	local_weapon_.fire_pressed = false;
	local_weapon_.reload_pressed = false;
	// Mission-scoped loadout state [orig: Game_StartMission rebuilds restrictionData +
	// g_armoryWeaponAvailability per mission @ 0x5246c3/@ 0x5246e8].
	local_inventory_valid_ = false;
	local_loadout_.reset();
	mission_availability_rows_.clear();
	mission_kit_rows_.clear();
	local_weapon_.switch_in_flight = false;
	local_weapon_.switch_deferred_action = -1;
	local_weapon_.start_in_switchto = false;
	// Mission-scoped, like the event queue cleared above. weapon_profile_ is NOT reset
	// here: the profile record is player-scoped and outlives every mission load, the
	// same way g_charSelClass is loaded once at boot [orig: PlayerProfile_LoadAllFromDisk
	// @0x54f4d0 runs from the startup path, not Game_StartMission].
	local_weapon_.presentation_pending = false;
	// Mission-scoped even though weapon_profile_ is not: the RESIDENT BUFFER is rebuilt
	// per mission, so the next session must re-copy its side's page rather than assume
	// the previous mission's copy still stands.
	weapon_profile_seeded_side_ = -1;
	player_view_ = opennova::world::PlayerViewState{};
	// Round init clears the map mode and the zooms return to the spawn
	// defaults (witness at hud::HudMapControl — Game_InitNewRound /
	// Player_InitPlayer lifecycle).
	hud_map_control_.reset_spawn();
	local_weapon_.nvg_scope_restore = false;
	binocular_yaw_offset_deg_ = 0.0f;
	binocular_pitch_offset_deg_ = 0.0f;
	local_weapon_.eye_valid = false;
	local_weapon_.usegun_switch = LocalUseGunSwitch::kNone;
	local_weapon_.usegun_slot_active = false;
	local_weapon_.usegun_mount = opennova::world::EntityHandle{};
	local_weapon_.usegun_weapon_adm = 0xFF;
	local_weapon_.usegun_pending_mount = opennova::world::EntityHandle{};
	local_weapon_.usegun_pending_weapon_adm = 0xFF;
	local_weapon_.usegun_saved_adm = 0xFF;
	local_weapon_.usegun_switch_action = -1;
	local_weapon_.first_person_model_adm = 0xFF;
	world_ = std::make_unique<World>();
	world_->external_local_mounted_weapon_pump = true;
	world_->projectile_authority = !joiner_;
	world_->mp_session = host_listen_ || joiner_;
	ai_ = std::make_unique<AiSystem>();
	bms_ = std::make_unique<opennova::mission::BmsEventSystem>();
	wac_ = std::make_unique<opennova::wac::WacSystem>();
	promo_ = opennova::mission::PromoteResult{};
	world_installed_ = false;
	have_baseline_ = false;
	have_wac_baseline_ = false;
	last_sim_tick_us_ = 0;
	last_net_tick_us_ = 0;
	last_occlusion_build_us_ = 0;
	last_occlusion_probe_us_ = 0;
	last_present_snapshot_us_ = 0;
	last_present_entity_count_ = 0;
	reset_occlusion_apply_baseline();
	present_layout_.clear();
	++present_layout_revision_;
	// Collision models/instances are mission-scoped: drop them with the world (the
	// sweep re-registers on the next load) and re-point the fresh ai_ at the container.
	collision_item_db_.unref();
	item_traits_db_.unref();
	collision_resolve_.clear();
	wire_collision_shape_by_type_.clear();
	collision_pose_native_.clear();
	infantry_adm_resource_root_.unref();
	infantry_adm_item_db_.unref();
	infantry_adm_resolved_ai_count_ = 0;
	panm_time_override_ms_ = -1;
	collision_world_ = opennova::world::CollisionWorld{};
	collision_world_.set_trace_profile_enabled(
			runtime_profiling_enabled_);
	// Occlusion models too — retail reloads the model cache per mission, so the
	// weld pass's shared-record type-5 rewrites never leak across loads.
	occlusion_world_ = opennova::world::OcclusionWorld{};
	occlusion_culled_bms_.clear();
	apply_terrain_to_ai(); // re-point the fresh ai_ at the persisted terrain field (if any)
	apply_root_motion_to_ai(); // ...and at the persisted infantry clip set (if any)
	apply_collision_to_ai();
}

void Simulation::set_network_environment(
		int64_t p_fog_target_q16,
		int64_t p_fog_current_q16,
		int64_t p_fog_accel_clamp,
		int64_t p_tod_fixed24,
		int64_t p_tod_advance_per_tick,
		int64_t p_quake_ticks,
		int64_t p_cloud_scroll_rate_target,
		int64_t p_rain_pct_current_q16,
		int64_t p_overcast_blend_q16,
		int64_t p_precipitation_kind) {
	const auto u32 = [](int64_t value) -> uint32_t {
		return value <= 0 ? 0u
		                  : (value >= 0xFFFFFFFFll ? 0xFFFFFFFFu
		                                           : static_cast<uint32_t>(value));
	};
	opennova::world::EnvNetworkSample sample;
	sample.fog_target_q16 = static_cast<int32_t>(std::min<uint32_t>(
			u32(p_fog_target_q16), static_cast<uint32_t>(INT32_MAX)));
	sample.fog_current_q16 = static_cast<int32_t>(std::min<uint32_t>(
			u32(p_fog_current_q16), static_cast<uint32_t>(INT32_MAX)));
	sample.fog_accel_clamp = u32(p_fog_accel_clamp);
	sample.tod_fixed24 = u32(p_tod_fixed24);
	sample.tod_advance_per_tick = u32(p_tod_advance_per_tick);
	sample.quake_ticks = u32(p_quake_ticks);
	sample.cloud_scroll_rate_target = u32(p_cloud_scroll_rate_target);
	sample.rain_pct_current_q16 = u32(p_rain_pct_current_q16);
	sample.overcast_blend_q16 = u32(p_overcast_blend_q16);
	sample.precipitation_kind = u32(p_precipitation_kind);
	world_->network_env.publish_complete(sample);
}

void Simulation::advance_network_environment_tick() {
	world_->network_env.advance_tick();
}

void Simulation::initialize_network_environment_mission_start() {
	if (!world_installed_ || joiner_ || world_ == nullptr) return;
	world_->network_env.initialize_mission_start();
}

// Re-point the (possibly just-rebuilt) AI system at our owned terrain field. The field's raw
// pointers reference terrain_heightmap_/terrain_sector_grid_, which persist across reset_world.
void Simulation::apply_terrain_to_ai() {
	// The round sim's ground stop shares the same field (world.terrain; §5.60).
	if (world_) world_->terrain = terrain_field_.valid() ? &terrain_field_ : nullptr;
	if (world_) {
		// The footstep surface pick reads the charmap through this view; the
		// zero-initialized map is the sampler's "no charmap -> surface 1" leg.
		world_->surface_map =
			surface_indices_.empty() ? opennova::terrain::SurfaceTypeMap{} : surface_map_;
		// The placed-tile override rides the same view (D-SND-15). With no
		// charmap the sampler's early return-1 skips the walk exactly like
		// retail, so attaching the tiles unconditionally is faithful.
		world_->surface_map.tiles =
				surface_tiles_.empty() ? nullptr : surface_tiles_.data();
		world_->surface_map.tile_count = static_cast<int32_t>(surface_tiles_.size());
		world_->surface_map.tile_surface = tile_surface_table_.data();
	}
	apply_sound_state_to_world();
	if (!ai_) return;
	ai_->terrain = terrain_field_.valid() ? &terrain_field_ : nullptr;
	ai_->ground_clearance = opennova::world::GroundClearance{};
	// The collision ground probe shares the same field.
	collision_world_.terrain = terrain_field_.valid() ? &terrain_field_ : nullptr;
}

// (Re)apply the persisted sound-profile chain state to the current world: the parsed
// SndProf.def table and the water plane. Runs from apply_terrain_to_ai
// (reset_world / load) and from the setters when live. (The mission attrib
// dword the scream's night gate reads is stamped by finish_load from the BMS
// header — not re-applied here.)
void Simulation::apply_sound_state_to_world() {
	if (!world_) return;
	world_->sound_profiles.clear();
	if (!sndprof_text_.empty())
		world_->sound_profiles.parse(reinterpret_cast<const char *>(sndprof_text_.data()),
		                             sndprof_text_.size());
	world_->env.water_z = env_water_z_q16_;
}

// Re-point the (possibly just-rebuilt) AI system at the owned infantry root-motion source.
// A source with no clips counts as none: the selector then resolves every state to "no
// clip" and soldiers stand, exactly the original's relationship between motion and clips.
void Simulation::apply_root_motion_to_ai() {
	if (!ai_) return;
	ai_->root_motion = !infantry_anim_.empty() ? &infantry_anim_ : nullptr;
}
opennova::mission::PromoteOptions Simulation::promote_options() const {
	opennova::mission::PromoteOptions opts;
	opts.item_seat_specs = item_seat_specs_;
	opts.ai_profiles = ai_profiles_;
	// Authored display names from the installed mission text's [PeopleNames]
	// STRNAME%03i entries (the boot installs the table before load_mission
	// runs). Promote applies the retail 15-char copy at its cited port site.
	opts.people_name_resolver = [this](int32_t index) {
		const auto it = mission_people_names_.find(index);
		return it != mission_people_names_.end() ? it->second : std::string();
	};
	return opts;
}

void Simulation::set_terrain_height_field(const Ref<TerrainData> &p_terrain) {
	// Clear first so a null/unloaded terrain disables grounding.
	terrain_heightmap_.clear();
	terrain_sector_grid_.clear();
	terrain_field_ = opennova::terrain::TerrainHeightField{};

	surface_indices_.clear();
	surface_map_ = opennova::terrain::SurfaceTypeMap{};

	if (p_terrain.is_valid() && p_terrain->is_loaded()) {
		const opennova::CptFile &cpt = p_terrain->get_cpt();
		const opennova::TrnConfig &trn = p_terrain->get_trn();
		if (!cpt.depth_buffer.empty()) {
			terrain_heightmap_ = cpt.depth_buffer; // own a copy (outlives the source resource)
			terrain_sector_grid_.resize(256);
			const int *grid = &trn.sector_grid[0][0];
			for (int i = 0; i < 256; ++i) terrain_sector_grid_[i] = grid[i];

			terrain_field_.heightmap = terrain_heightmap_.data();
			terrain_field_.dim = static_cast<int>(std::sqrt(static_cast<double>(terrain_heightmap_.size())));
			terrain_field_.layout.sector_grid = terrain_sector_grid_.data();
			// Sector origins + the per-quadrant neighbour-tap locks, through the same
			// helper the render/editor field uses. Without the locks every 512-unit
			// sector boundary reads the neighbouring quadrant and grounding drops into
			// a one-unit trench the terrain mesh does not draw — you fall through
			// ground that looks solid.
			height_field_apply_trn(terrain_field_, trn);
			// Water clamp deferred: water_height units (vs the 16.16 worldY @0x26C6454 the original
			// compares) are not yet verified, so leave has_water off rather than float entities onto
			// a wrong plane. The ground-following path (the Phase 1 goal) does not need it.
			terrain_field_.has_water = false;

			// The charmap surface raster for the footstep surface pick (own a
			// copy like the depth buffer; shares the sector grid + origins)
			// [orig: Terrain_GetSurfaceTypeAtPosition @ 0x606510].
			const std::vector<uint8_t> &charmap = p_terrain->get_charmap_indices();
			if (!charmap.empty() && p_terrain->get_charmap_width() > 0) {
				surface_indices_ = charmap;
				surface_map_.data = surface_indices_.data();
				surface_map_.width = p_terrain->get_charmap_width();
				surface_map_.height = p_terrain->get_charmap_height();
				surface_map_.sector_grid = terrain_sector_grid_.data();
				surface_map_.origin_x = trn.origin_x;
				surface_map_.origin_y = trn.origin_y;
			}
		}
	}
	apply_terrain_to_ai();
}

void Simulation::set_sound_profiles(const PackedByteArray &p_sndprof_text) {
	sndprof_text_.assign(p_sndprof_text.ptr(), p_sndprof_text.ptr() + p_sndprof_text.size());
	apply_sound_state_to_world();
}

void Simulation::set_water_z(double p_water_y) {
	env_water_z_q16_ = static_cast<int32_t>(p_water_y * 65536.0);
	if (world_) world_->env.water_z = env_water_z_q16_;
}

Array Simulation::drain_slot_sounds() {
	Array out;
	if (!world_installed_) return out;
	for (const opennova::world::SoundSlotEvent &ev : world_->slot_sounds) {
		Dictionary d;
		d["set"] = String(ev.set_name);
		// Mission-frame 16.16 -> godot (x, z, -y), same mapping as the fire drain.
		d["pos"] = Vector3(static_cast<float>(ev.pos[0]) / 65536.0f,
		                   static_cast<float>(ev.pos[2]) / 65536.0f,
		                   static_cast<float>(-ev.pos[1]) / 65536.0f);
		d["handle"] = ev.source_handle;
		d["slot"] = ev.slot;
		out.push_back(d);
	}
	world_->slot_sounds.clear();
	return out;
}

Array Simulation::drain_sound_emitters() {
	Array out;
	if (!world_installed_) return out;
	const std::vector<opennova::world::SoundEmitterEvent> events =
			world_->sound_emitters.drain();
	for (const opennova::world::SoundEmitterEvent &ev : events) {
		Dictionary d;
		d["source_spawn_id"] = static_cast<int64_t>(ev.source_spawn_id);
		d["handle"] = ev.source_handle;
		d["source_bms_id"] = ev.source_bms_id;
		// Mission coordinates -> Godot (x, z, -y), matching every other
		// positional presentation drain.
		d["pos"] = Vector3(ev.pos.x, ev.pos.z, -ev.pos.y);
		d["lane"] = ev.lane;
		d["slot"] = ev.slot;
		d["lifetime"] = ev.lifetime_ticks;
		d["emitted_tick"] = ev.emitted_tick;
		d["pitch_q16"] = ev.pitch_q16;
		d["volume_q8_8"] = ev.volume_q8_8;
		d["source_only"] = ev.source_only;
		d["set"] = String(ev.set_name.c_str());
		out.push_back(d);
	}
	return out;
}

void Simulation::finish_load(const opennova::bms::File &file) {
	// One world, three systems, the faithful tick order. The AI-change action family reaches
	// brains through World::ai; wire it before registering so the pre-mission pass can dispatch.
	bms_->load(file.events, file.triggers, file.actions);
	// Mission attribute flags -> the world (0x40 = SinglePlayerRespawn gates the SP
	// death auto-lose in check_win_conditions). [orig: Bms_AttribFlags @0xa76258,
	// read by Server_CheckWinConditions @0x51ad6f]
	world_->mission_attrib_flags = static_cast<uint32_t>(file.header.attrib_flags);
	// The mission's authored map_zoom scales BOTH radar-zoom spawn defaults
	// (witness at hud::HudMapControl::set_mission_map_zoom — the
	// Player_InitPlayer derivation off the BMS header float).
	hud_map_control_.set_mission_map_zoom(file.header.map_zoom);
	// The mission's loadout/availability chunks wait for the weapon catalog —
	// load_weapon_table promotes them through the engine's SP-vs-net gate.
	stash_mission_loadout_rules(file);
	reset_local_player_view_effects();
	world_->ai = ai_.get();
	// P7 listen server (SP + LAN host): stand up the npruntime in-match runtime (mode-3 HostClient
	// over an in-process loopback, the faithful §5.0 path). Server_TickUpdate owns the logic tick +
	// the C2S drain + the 0x0A fan, so there is NO net ISystem here (D-NET-123/125) — the
	// present reads the host's own ClientRuntime view, and a LAN host adds the socket legs in host_pump.
	if (listen_server_) {
		bringup_host_runtime(file);
	}
	// P7 co-op LAN joiner: a pure non-authority client. Build a fresh np::ClientRuntime (Joiner role)
	// per (re)load — start() fully resets the session, so a reload reconnects cleanly. No net ISystem
	// is registered (the joiner never serializes; run_logic_tick(false) leaves World::net the default
	// LocalSink for WAC/BMS sinks). The local player L is spawned in joiner_pump after the
	// name-match and applicable deployment release.
	if (joiner_) {
		// A retail-style menu join has already authenticated and learned the map
		// from S2C 0x7B before this local load. Preserve that exact runtime/socket;
		// rebuilding it here would silently reconnect and discard the witnessed
		// pre-load session. Direct-loaded callers have not started yet and retain
		// the historical fresh-runtime reset.
		if (!joiner_bridge_.started() || !runtime_) {
			runtime_ = std::make_unique<opennova::np::ClientRuntime>(joiner_player_name_);
			joiner_environment_revision_seen_ = 0;
			joiner_bridge_.reset_for_runtime_rebuild();
			install_charattr_challenge_table();
			install_character_join_vars();
			install_join_integrity_profile();
			install_expansion_version_root();
		}
		runtime_->set_world_ready(true);
		// The shell owns the deploy-map screen: a pick-required join parks at the
		// player's pick instead of auto-answering parameter-0 (headless ClientRuntime
		// callers keep the auto default). [orig: the DEATH screen; net-re §5.61]
		runtime_->set_player_paced_deployment(true);
		joiner_bridge_.reset_for_load(runtime_->deployment_release_revision());
		joiner_applied_loadout_revision_ = 0;
		local_loadout_.pending_player_class = -1; // the shell re-applies the kit after each load
		deploy_zone_registry_built_ = false; // fresh world -> fresh zone registry
		// Re-arm the 0x2F submission seam from the carried sim state. reset_world just
		// rebuilt an EMPTY weapon catalog, so this is a deliberate no-op that leaves
		// the capture-default fallback armed; the real arm happens when the shell's
		// load_weapon_table lands (and again on any later class/loadout apply).
		push_joiner_loadout_kit();
	}
	// Re-arm the fresh runtime's decode view with the items.def class table (built by a
	// prior resolve_item_traits; the shell also re-resolves per load, which re-installs).
	install_item_class_resolver();
	opennova::mission::register_mission_systems(*world_, *wac_, *bms_, *ai_);
	// Re-install the held script program onto the fresh WacSystem (reset_world
	// recreated it). The 62-tick execution divider stays inside the system
	// [orig: dword_C6EAD4 / cmp 0x3E]; a missing program leaves the VM unloaded
	// and its tick early-outs, the BMS-only case.
	if (wac_program_.is_valid() && wac_program_->is_ok()) {
		wac_->set_program(wac_program_->native_program());
	}
	// PreMission events settle initial scripted state before the clock starts (AI is skipped on
	// the pre-mission pass). Snapshot AFTER it so Stop restores the true play-start state.
	world_->run_logic_tick(/*is_authority=*/true, /*pre_mission=*/true);
	baseline_ = world_->snapshot();
	ai_->capture_spawn_baseline();
	have_baseline_ = true;
	wac_baseline_ = wac_->capture_runtime_state();
	have_wac_baseline_ = true;
	world_installed_ = true;
	if (!defer_session_load_completion_) complete_session_load();
}

void Simulation::apply_host_session_mission_header(const opennova::bms::File &file) {
	std::vector<uint8_t> header_blob;
	std::string error;
	if (opennova::bms::encode_loaded_header_blob(file, header_blob, error)) {
		host_session_config_.mission_header_blob = std::move(header_blob);
	} else {
		host_session_config_.mission_header_blob.clear();
	}

	const std::string mission_name = file.get_mission_name();
	if (!mission_name.empty()) {
		host_session_config_.mission_name = mission_name;
		if (host_session_config_.spawn_names.empty()) {
			host_session_config_.spawn_names.push_back(mission_name);
		}
	}
	// P7: host_session_config_ is consumed at the next load by bringup_host_runtime
	// (configure_session_runtime + the §5.1 reactive-reply config); nothing to refresh live.
}

// S9 (ADR 0028): the mission boot policy. The ORDER and the gates live in
// engine/runtime/mission runtime_boot (run_mission_boot), the file-resolution
// rules (mission-text fallback, .aip profile speeds, the adm default) resolve
// there over the native ResourceIndex, and this entry supplies the step bodies
// from the existing feeds. Role bring-up (join/host/SP) runs before this in
// the shell; presentation composition and the shell's PANM clock re-stamp run
// after it.
int64_t Simulation::boot_mission(const Ref<MissionData> &p_mission,
		const Ref<ResourceRoot> &p_resource_root,
		const Ref<ItemDatabase> &p_item_db,
		const Ref<TerrainData> &p_terrain,
		const PackedByteArray &p_terrain_til, const String &p_wac_basename,
		const String &p_infantry_adm, const String &p_mission_file_basename,
		bool p_playable) {
	namespace ms = opennova::mission;
	ms::BootFileSource files;
	if (p_resource_root.is_valid()) {
		const opennova::ResourceIndex *index = &p_resource_root->native_index();
		files.has_file = [index](const std::string &name) {
			return index->has_file(name);
		};
		files.read_file = [index](const std::string &name,
				std::vector<uint8_t> &out) {
			return index->read_file(name, out);
		};
	}
	// The infantry clip-set default is boot policy (runtime_boot.h).
	const String infantry_adm = p_infantry_adm.is_empty()
			? String(ms::kDefaultInfantryAdm)
			: p_infantry_adm;
	boot_debug_ = MissionBootDebug{};
	boot_debug_.infantry_adm = std::string(infantry_adm.utf8().get_data());

	// S16 (ADR 0028): the native seat extraction resolves model userpoints at
	// install time, and the witnessed boot order runs the seat step FIRST.
	// Wire the sim's own model source up front — pure source wiring (no world
	// mutation); the ordered install_asset_root step later re-affirms the same
	// root (same index -> the parse cache is untouched).
	if (p_resource_root.is_valid())
		set_asset_root(p_resource_root);

	ms::BootParams params;
	params.is_joiner = joiner_;
	params.playable = p_playable;
	params.has_resource_root = p_resource_root.is_valid();
	params.has_item_db = p_item_db.is_valid();
	params.has_terrain = p_terrain.is_valid();
	params.has_terrain_til = !p_terrain_til.is_empty();
	params.has_wac = !p_wac_basename.is_empty();

	ms::BootSteps steps;
	steps.install_seat_specs = [&] {
		// S16: seeds = every mission entity's full items.def id; the native
		// extractor walks authored addeweap children transitively and drops
		// rows without runtime metadata, exactly like the shell extractor
		// this replaces.
		std::vector<int> seeds;
		if (p_mission.is_valid()) {
			using BmsEntity = opennova::bms::Entity;
			const opennova::bms::File &mission_doc =
					p_mission->native_document().bms_file();
			const auto seed_group = [&seeds](const std::vector<BmsEntity> &v) {
				for (const BmsEntity &e : v) {
					if (e.type_id > 0)
						seeds.push_back(static_cast<int>(e.type_id) +
								static_cast<int>(
										opennova::mission::kItemIdOffset));
				}
			};
			seed_group(mission_doc.items);
			seed_group(mission_doc.buildings);
			seed_group(mission_doc.markers);
			seed_group(mission_doc.organics);
		}
		install_native_seat_specs(p_item_db, seeds);
	};
	steps.install_ai_profiles = [&] {
		// The native .aip resolve (runtime_boot). Assigning an empty row set
		// clears the retained table, exactly like the shell resolver's empty
		// dictionary did through set_ai_profile_speeds.
		ai_profiles_ = p_mission.is_valid()
				? ms::resolve_ai_profiles(
						  files, p_mission->native_document().bms_file())
				: std::vector<ms::PromoteOptions::AiProfileRow>{};
		boot_debug_.aip_rows = ai_profiles_;
	};
	steps.install_terrain_til = [&] { set_terrain_til_data(p_terrain_til); };
	steps.install_mission_text = [&] {
		std::vector<uint8_t> text;
		boot_debug_.text_source = static_cast<int32_t>(ms::resolve_mission_text(
				files,
				std::string(p_mission_file_basename.utf8().get_data()), text));
		boot_debug_.text_size = static_cast<int64_t>(text.size());
		PackedByteArray bytes;
		bytes.resize(static_cast<int64_t>(text.size()));
		if (!text.empty()) std::memcpy(bytes.ptrw(), text.data(), text.size());
		set_mission_text_data(bytes);
	};
	steps.load_mission = [&] {
		return p_mission.is_valid() && load_from_mission_data(p_mission);
	};
	steps.install_terrain_field = [&] {
		set_terrain_height_field(p_terrain);
		// The D-SND-15 placed-tile surface table: the .TSD probe/parse policy
		// is engine-side (terrain_query resolve_tileset_surface_table).
		tile_surface_table_.fill(0);
		if (p_terrain.is_valid() && p_terrain->is_loaded()) {
			opennova::terrain::SurfaceTileFileSource tile_files;
			tile_files.has_file = files.has_file;
			tile_files.read_file = files.read_file;
			opennova::terrain::resolve_tileset_surface_table(tile_files,
					p_terrain->get_trn().tilestrip, tile_surface_table_.data());
		}
		apply_terrain_to_ai();
	};
	steps.install_sound_profiles = [&] {
		if (!files.valid() || !files.has_file("SndProf.def")) return;
		std::vector<uint8_t> text;
		if (!files.read_file("SndProf.def", text)) return;
		PackedByteArray bytes;
		bytes.resize(static_cast<int64_t>(text.size()));
		if (!text.empty()) std::memcpy(bytes.ptrw(), text.data(), text.size());
		set_sound_profiles(bytes);
	};
	steps.install_infantry_anim = [&] {
		if (set_infantry_anim_map(p_resource_root, infantry_adm) <= 0)
			UtilityFunctions::push_warning(vformat(
					"MissionPresentation: no infantry clips from '%s' — AI soldiers will stand still.",
					infantry_adm));
	};
	steps.install_wac = [&] {
		Ref<WacProgram> wac;
		wac.instantiate();
		const Error wac_err = wac->compile_from_resource_root(
				p_resource_root, p_wac_basename);
		if (wac_err == OK) {
			set_wac_program(wac);
		} else if (wac_err != ERR_DOES_NOT_EXIST) {
			UtilityFunctions::push_warning(vformat(
					"MissionPresentation: WAC for '%s' failed to compile (%d error(s)) — scripts disabled.",
					p_wac_basename, wac->get_error_count()));
		}
	};
	steps.spawn_local_player = [&] {
		const int spawn_status = spawn_local_player_at_start();
		if (spawn_status < 0)
			UtilityFunctions::push_warning(
					"MissionPresentation: spawn_local_player failed (pool 0 full / no AI?)");
		else if (spawn_status == 0)
			UtilityFunctions::push_warning(
					"MissionPresentation: no player-start marker (60xx start family) in this mission — spawned at fallback origin.");
	};
	steps.resolve_infantry_adm = [&] {
		resolve_infantry_adm_ids(p_resource_root, p_item_db);
	};
	steps.resolve_item_traits = [&] { resolve_item_traits(p_item_db); };
	steps.install_asset_root = [&] { set_asset_root(p_resource_root); };
	steps.resolve_collision = [&] { resolve_collision_instances(p_item_db); };
	steps.occlusion_init = [&] { occlusion_init_mission(); };
	steps.load_weapon_table = [&] {
		if (load_weapon_table(p_resource_root, "weapon.def") != OK)
			UtilityFunctions::push_warning(
					"MissionPresentation: weapon.def not loaded — 0x5A ammo resolve degraded to echo");
	};
	steps.load_ammo_table = [&] {
		if (load_ammo_table(p_resource_root, "ammo.def") == OK) return true;
		UtilityFunctions::push_warning(
				"MissionPresentation: ammo.def not loaded — client fire echoes without authoritative rounds");
		return false;
	};
	steps.resolve_ai_weapons = [&] { resolve_ai_weapons(p_item_db); };

	defer_session_load_completion_ = true;
	const ms::BootAbort abort = ms::run_mission_boot(params, steps);
	defer_session_load_completion_ = false;
	if (abort == ms::BootAbort::kNone) {
		complete_session_load();
		return OK;
	}
	fail_session_load("mission boot failed");
	return ERR_CANT_OPEN;
}

Dictionary Simulation::get_mission_boot_debug() const {
	Dictionary out;
	out["text_source"] = boot_debug_.text_source;
	out["text_size"] = boot_debug_.text_size;
	out["infantry_adm"] = String(boot_debug_.infantry_adm.c_str());
	Dictionary aip;
	for (const opennova::mission::PromoteOptions::AiProfileRow &row :
			boot_debug_.aip_rows) {
		Dictionary speeds;
		if (row.data.patrol_speed != -1) speeds["patrol"] = row.data.patrol_speed;
		if (row.data.combat_speed != -1) speeds["combat"] = row.data.combat_speed;
		aip[String(row.profile.c_str())] = speeds;
	}
	out["aip"] = aip;
	return out;
}

bool Simulation::load_from_mission_data(const Ref<MissionData> &p_mission) {
	if (p_mission.is_null()) return false;
	if (!begin_session_load()) return false;
	reset_world();
	// Do not infer this from `joiner_`: tests/tools and legacy direct joins may
	// still load a complete BMS, whose authored promotion is already canonical.
	// Only the production 616-byte S2C header needs wire-time materialization.
	joiner_bridge_.set_wire_header_world(p_mission->is_wire_header_only());
	// The editor's live, in-memory mission (unsaved edits included).
	const opennova::bms::File &file = p_mission->native_document().bms_file();
	promo_ = opennova::mission::promote_mission(file, *world_, *ai_, promote_options());
	finish_load(file);
	apply_host_session_mission_header(file);
	return true;
}

bool Simulation::load_mission_file(const String &path) {
	if (!begin_session_load()) return false;
	reset_world();
	opennova::bms::File file;
	std::string err;
	if (!opennova::bms::parse_file(std::string(path.utf8().get_data()), file, err)) {
		fail_session_load(err.c_str());
		return false;
	}
	host_session_config_.mission_file = std::string(path.get_file().utf8().get_data());
	promo_ = opennova::mission::promote_mission(file, *world_, *ai_, promote_options());
	finish_load(file);
	apply_host_session_mission_header(file);
	return true;
}

void Simulation::build_demo_mission() {
	if (!begin_session_load()) return;
	reset_world();
	opennova::bms::File file = make_demo_mission();
	host_session_config_.mission_file = "demo.bms";
	promo_ = opennova::mission::promote_mission(file, *world_, *ai_, promote_options());
	finish_load(file);
	apply_host_session_mission_header(file);
}

bool Simulation::advance_world_tick() {
	if (!world_installed_) return false;
	// ONE logic tick (the original's 62 Hz engine tick). The WAC VM self-gates to every
	// 62nd tick and the BMS evaluator quarter-passes every 16th, inside their systems —
	// exactly where the original keeps those dividers. A render frame runs 0..N of these;
	// the accumulator that decides N lives in MissionSession
	// [orig: Game_MainLoop @ 0x52b630].
	//
	// Listen-server frame order [orig: Game_ProcessMainFrame @ 0x5263f0]:
	//   input -> net(drain C2S) -> run_logic_tick(WAC/BMS/AI) -> net(emit S2C) -> present.
	// Server_TickUpdate owns the C2S drain at the top of the loop and the post-logic S2C fan;
	// host_pump drives it and the local client's decode happens via the host's ClientRuntime.
	const uint64_t sim_start =
			runtime_profiling_enabled_ ? perf_now_us() : 0;
	if (listen_server_) { // P7 listen server (SP + LAN host) -> the npruntime owner loop
		host_pump();
		resolve_new_infantry_adm_ids();
		if (runtime_profiling_enabled_)
			last_sim_tick_us_ = perf_now_us() - sim_start;
		return true;
	}
	if (joiner_) { // P7 co-op joiner -> the npruntime ClientRuntime (non-authority)
		joiner_pump();
		resolve_new_infantry_adm_ids();
		if (runtime_profiling_enabled_)
			last_sim_tick_us_ = perf_now_us() - sim_start;
		return true;
	}
	// No-net editor/unit path: one authoritative logic tick, no replication.
	apply_player_input_pre_tick();
	world_->run_logic_tick(/*is_authority=*/true);
	sync_local_mounted_input_heading();
	tick_local_player_view();   // retail promotes the per-frame view before weapon actions
	tick_local_player_weapon(); // the equipped-slot FSM pump, after the view promoter
	resolve_new_infantry_adm_ids();
	if (runtime_profiling_enabled_)
		last_sim_tick_us_ = perf_now_us() - sim_start;
	return true;
}

// The post-logic half of the listen-server frame: serialize the live world into one
// S2C 0x0A frame, loop it back in-process, and let the local client decode it into the
// ClientState the present pass reads. No-op when the listen server is off.
void Simulation::restore_world_baseline() {
	if (!world_installed_ || !have_baseline_) return;
	const bool usegun_was_active = local_weapon_.usegun_slot_active;
	const bool usegun_was_pending =
			local_weapon_.usegun_switch != LocalUseGunSwitch::kNone;
	const uint8_t saved_personal_adm = local_weapon_.usegun_saved_adm;
	local_weapon_.events.clear();
	local_weapon_.power_throw_start_tick = 0;
	local_weapon_.pending_throw_charge = 0;
	local_weapon_.fire_held = false;
	local_weapon_.fire_pressed = false;
	local_weapon_.reload_pressed = false;
	local_weapon_.usegun_switch = LocalUseGunSwitch::kNone;
	local_weapon_.usegun_slot_active = false;
	local_weapon_.usegun_mount = opennova::world::EntityHandle{};
	local_weapon_.usegun_weapon_adm = 0xFF;
	local_weapon_.usegun_pending_mount = opennova::world::EntityHandle{};
	local_weapon_.usegun_pending_weapon_adm = 0xFF;
	local_weapon_.usegun_saved_adm = 0xFF;
	local_weapon_.usegun_switch_action = -1;
	local_weapon_.switch_deferred_action = -1;
	world_->restore(baseline_); // rewinds registry/vars/env/clock + re-inits systems (incl. AI;
	                            // WacSystem::on_load also resets its 62-tick accumulator)
	if (have_wac_baseline_ && wac_) {
		wac_->restore_runtime_state(wac_baseline_);
	}
	if (joiner_bridge_.wire_header_world()) {
		// ClientState survives Stop/Start, while the body-empty baseline removes
		// its registry carriers. Force one exact rematerialization fold; retain the
		// already-built portal tables because their handles remain identical and
		// the occlusion models' weld records are intentionally one-shot mutable.
		joiner_bridge_.reset_materialization();
		deploy_zone_registry_built_ = false;
	}
	// The baseline is captured during finish_load, before MissionPresentation supplies
	// items.def. Restore those authoritative callback/health traits first; the
	// encoder and the client classifier must agree on every 0x0A record width.
	if (item_traits_db_.is_valid()) resolve_item_traits(item_traits_db_);
	if (listen_server_ && !joiner_ && runtime_) {
		// Stop restores the authoritative registry, including NoNetworkCallback
		// attachment children, but those children never have a live 0x0A body that
		// could recreate a row erased from the host's decoded ClientState. Start a
		// fresh HostClient view and replay the same production load batches, in the
		// witnessed stream order, so restored runtime identities materialize now
		// instead of inheriting a prior play epoch's rows and handle caches.
		host_loop_.clear();
		runtime_ = std::make_unique<opennova::np::ClientRuntime>(host_loop_);
		install_item_class_resolver();
		opennova::netsim::ClientReplicaPipeline &view = runtime_->view();
		view.apply(0x10, opennova::encode_static_entity_batch(
				opennova::netsim::build_pool2_static_batch(*world_)));
		view.apply(0x0D, opennova::encode_pool_spawn_batch(
				opennova::netsim::build_pool1_spawn_batch(*world_)));
		view.apply(0x0C, opennova::encode_organic_spawn_batch(
				opennova::netsim::build_pool0_organic_batch(
						*world_, world_->cached.local_player)));
		view.apply(0x20, opennova::encode_pool3_sync_batch(
				opennova::netsim::build_pool3_marker_batch(*world_)));
	}
	reset_infantry_adm_ids();
	resolve_new_infantry_adm_ids();
	if (usegun_was_active) {
		// The world snapshot restores the play-start entity set, while the host
		// still presents the borrowed emplacement definition. Reinstall the saved
		// personal selection as a fresh restart epoch; MissionPresentation delivers this
		// event synchronously while stopped.
		if (opennova::world::Entity *player =
					world_->registry.get(world_->cached.local_player))
			player->equipped_adm_index = saved_personal_adm;
		local_weapon_.active = false;
		const opennova::world::WeaponTableEntry *saved_def =
				world_->weapons.by_index(saved_personal_adm);
		local_weapon_.start_in_switchto = saved_def != nullptr;
		opennova::world::WeaponPresentationEvent event;
		event.tick = world_->logic_tick;
		if (const opennova::world::Entity *local =
					world_->registry.get(world_->cached.local_player))
			event.world_position = local->position;
		event.switch_to_weapon =
				saved_def != nullptr ? saved_def->name : std::string();
		event.clear_weapon = saved_def == nullptr;
		local_weapon_.events.push_back(std::move(event));
	} else if (usegun_was_pending) {
		// The presenter never left the personal weapon, but its outgoing slot may
		// already be inside SWITCHFROM/RANK. Cancel only that action state while
		// retaining the personal magazine and reserve.
		const int32_t clip = local_weapon_.slot.clip;
		const int32_t reserve = local_weapon_.slot.reserve;
		local_weapon_.slot = opennova::world::WeaponSlotState{};
		local_weapon_.slot.clip = clip;
		local_weapon_.slot.reserve = reserve;
	}
	if (collision_item_db_.is_valid())
		resolve_collision_instances(collision_item_db_);
	local_weapon_.anim_tick = world_->logic_tick;
	reset_local_player_view_effects();
	// The restored world can share a tick number with a previously cached view.
	// Force the next FollowOwner query to rebuild against the post-restart epoch.
	invalidate_present_effect_pose_cache();
}

void Simulation::set_wac_program(const Ref<WacProgram> &p_program) {
	wac_program_ = p_program;
	if (!world_installed_ || !wac_) {
		return; // finish_load applies it on the next load
	}
	if (wac_program_.is_valid() && wac_program_->is_ok()) {
		wac_->set_program(wac_program_->native_program());
	} else {
		wac_->set_program(opennova::wac::Program());
	}
}

bool Simulation::compile_and_set_wac(const PackedStringArray &p_sources) {
	ERR_FAIL_COND_V_MSG(!world_installed_, false, "compile_and_set_wac needs a loaded world (the registry resolves symbolic names).");
	std::vector<std::string> sources;
	sources.reserve(static_cast<size_t>(p_sources.size()));
	for (int64_t i = 0; i < p_sources.size(); ++i) {
		const CharString utf8 = p_sources[i].utf8();
		sources.emplace_back(utf8.get_data(), static_cast<size_t>(utf8.length()));
	}
	opennova::wac::CompileEnv env;
	env.registry = &world_->registry;
	opennova::wac::Program program = opennova::wac::compile_program(sources, env);
	Ref<WacProgram> holder;
	holder.instantiate();
	// Adopt the registry-compiled program into the holder so get_wac_program()
	// exposes its diagnostics either way.
	holder->adopt(std::move(program));
	wac_program_ = holder;
	if (!wac_program_->is_ok()) {
		return false;
	}
	wac_->set_program(wac_program_->native_program());
	return true;
}

bool Simulation::run_mission_start_wac() {
	if (!world_installed_ || joiner_ || world_ == nullptr || wac_ == nullptr) return false;
	return wac_->execute_initial(*world_);
}

void Simulation::seal_mission_start_baseline() {
	if (!world_installed_ || joiner_ || world_ == nullptr || ai_ == nullptr || wac_ == nullptr)
		return;
	baseline_ = world_->snapshot();
	ai_->capture_spawn_baseline();
	wac_baseline_ = wac_->capture_runtime_state();
	have_baseline_ = true;
	have_wac_baseline_ = true;
}

Dictionary Simulation::get_wac_state() const {
	Dictionary out;
	out["loaded"] = wac_ != nullptr && wac_->vm().loaded();
	out["paused"] = wac_ != nullptr && wac_->paused;
	out["runs"] = wac_ != nullptr ? static_cast<int64_t>(wac_->runs()) : 0;
	out["event_count"] = wac_ != nullptr ? wac_->program().event_count : 0;
	out["code_size"] = wac_ != nullptr ? static_cast<int>(wac_->program().code.size()) : 0;
	return out;
}

void Simulation::set_runtime_profiling_enabled(bool p_enabled) {
	if (runtime_profiling_enabled_ == p_enabled) return;
	runtime_profiling_enabled_ = p_enabled;
	last_sim_tick_us_ = 0;
	last_net_tick_us_ = 0;
	last_present_snapshot_us_ = 0;
	last_occlusion_build_us_ = 0;
	last_occlusion_probe_us_ = 0;
	collision_world_.set_trace_profile_enabled(p_enabled);
}

Vector4i Simulation::get_last_projectile_trace_times_us() const {
	const opennova::world::CollisionWorld::TraceProfile &tp =
			collision_world_.trace_profile();
	return Vector4i(trace_profile_lane(tp.terrain_us),
			trace_profile_lane(tp.static_us),
			trace_profile_lane(tp.dynamic_us),
			trace_profile_lane(tp.person_us));
}

Vector4i Simulation::get_last_projectile_trace_counts() const {
	const opennova::world::CollisionWorld::TraceProfile &tp =
			collision_world_.trace_profile();
	return Vector4i(trace_profile_lane(tp.calls),
			trace_profile_lane(tp.static_survivors),
			trace_profile_lane(tp.dynamic_survivors),
			trace_profile_lane(tp.person_survivors));
}

Vector2i Simulation::get_last_projectile_trace_faces() const {
	const opennova::world::CollisionWorld::TraceProfile &tp =
			collision_world_.trace_profile();
	return Vector2i(trace_profile_lane(tp.static_faces),
			trace_profile_lane(tp.dynamic_faces));
}

Dictionary Simulation::get_runtime_perf_counters() const {
	Dictionary out;
	out["loaded"] = is_loaded();
	out["listen_server"] = listen_server_;
	out["ai_count"] = ai_ ? ai_->count() : 0;
	out["present_entity_count"] = last_present_entity_count_;
	out["sim_tick_us"] = static_cast<int64_t>(last_sim_tick_us_);
	out["net_tick_us"] = static_cast<int64_t>(last_net_tick_us_);
	out["present_snapshot_us"] = static_cast<int64_t>(last_present_snapshot_us_);
	out["occlusion_build_us"] = static_cast<int64_t>(last_occlusion_build_us_);
	out["occlusion_probe_us"] = static_cast<int64_t>(last_occlusion_probe_us_);
	out["runtime_profiling_enabled"] = runtime_profiling_enabled_;
	// The last tick's projectile-trace attribution (collision.h TraceProfile).
	const opennova::world::CollisionWorld::TraceProfile &tp =
			collision_world_.trace_profile();
	out["trace_profiling_enabled"] = collision_world_.trace_profile_enabled();
	out["trace_calls"] = tp.calls;
	out["trace_terrain_us"] = tp.terrain_us;
	out["trace_static_us"] = tp.static_us;
	out["trace_dynamic_us"] = tp.dynamic_us;
	out["trace_person_us"] = tp.person_us;
	out["trace_static_survivors"] = tp.static_survivors;
	out["trace_dynamic_survivors"] = tp.dynamic_survivors;
	out["trace_person_survivors"] = tp.person_survivors;
	out["trace_static_faces"] = tp.static_faces;
	out["trace_dynamic_faces"] = tp.dynamic_faces;
	return out;
}

void Simulation::set_wac_paused(bool p_paused) {
	if (wac_) {
		wac_->paused = p_paused; // [orig: dword_C6EB28]
	}
}

bool Simulation::is_wac_paused() const {
	return wac_ != nullptr && wac_->paused;
}

void Simulation::set_mission_variable(int index, int value) {
	if (world_) world_->vars.set_mission(index, value);
}

// Probe/diagnostic seam beside get_entity_debug: write an AI entity's health through
// the same stores the scripted SETHP path touches (registry + the motor copy)
// [orig: the WAC SETHP op writes entity+286]. Lets in-game probes shorten a fight
// without bypassing the damage/death chain under test.
Error Simulation::debug_set_entity_health(int p_index, int p_hp) {
	if (!ai_ || !world_) return ERR_UNAVAILABLE;
	AiEntity *e = ai_->at(p_index);
	if (!e) return ERR_INVALID_PARAMETER;
	opennova::world::Entity *ent = world_->registry.get(e->handle);
	if (ent == nullptr) return ERR_UNAVAILABLE;
	e->health = static_cast<int16_t>(p_hp);
	ent->health = p_hp;
	ent->alive = p_hp > 0;
	return OK;
}

// The D-AI-6 muzzle seam: the present layer pushes each posed model's gun-flash
// userpoint world position back to the sim once per frame; the AI fire pass spawns
// rounds from it while fresh. Godot (x, up, z) -> mission (x, -gz, gy) in 16.16
// fixed — the inverse of the present mapping godot = (mx, mz, -my).
// [orig: Entity_GetAttachmentWorldPosition @0x4b2670 from the anim-event fire block
// @0x4bf326 — computed inline against the engine-side skeleton; ours is shell-fed.]
void Simulation::set_ai_muzzle_world(int p_net_id, const Vector3 &p_godot_pos) {
	if (!ai_ || !world_) return;
	// Keyed by the row's PF_NET_ID (the authored SSN) — the wire handle is
	// 0-ambiguous for pool-0 slot 0, and the row order is not the AI index.
	// for_handle inside set_entity_muzzle drops non-AI entities.
	if (p_net_id <= 0 || p_net_id > 0xFFFF) return;
	const opennova::world::EntityHandle h =
			world_->registry.find_by_net_id(static_cast<uint16_t>(p_net_id));
	if (!h.valid()) return;
	const int32_t pos[3] = {
		static_cast<int32_t>(p_godot_pos.x * 65536.0f),
		static_cast<int32_t>(-p_godot_pos.z * 65536.0f),
		static_cast<int32_t>(p_godot_pos.y * 65536.0f),
	};
	if (opennova::world::Entity *entity = world_->registry.get(h)) {
		entity->posed_muzzle_world[0] = pos[0];
		entity->posed_muzzle_world[1] = pos[1];
		entity->posed_muzzle_world[2] = pos[2];
		entity->posed_muzzle_tick = world_->logic_tick;
		entity->posed_muzzle_valid = true;
	}
	ai_->set_entity_muzzle(h, pos, world_->logic_tick);
}

// Probe seam beside debug_set_entity_health: teleport an AI entity through both
// position stores (registry + motor copy) — mission-space coordinates. Lets
// in-game probes bring a reachable victim to the player when the mission
// geography (interiors, fences) defeats straight-line navigation.
Error Simulation::debug_set_entity_position(int p_index, const Vector3 &p_mission_pos) {
	if (!ai_ || !world_) return ERR_UNAVAILABLE;
	AiEntity *e = ai_->at(p_index);
	if (!e) return ERR_INVALID_PARAMETER;
	opennova::world::Entity *ent = world_->registry.get(e->handle);
	if (ent == nullptr) return ERR_UNAVAILABLE;
	e->pos[0] = static_cast<int32_t>(p_mission_pos.x * 65536.0f);
	e->pos[1] = static_cast<int32_t>(p_mission_pos.y * 65536.0f);
	e->pos[2] = static_cast<int32_t>(p_mission_pos.z * 65536.0f);
	ent->position.x = p_mission_pos.x;
	ent->position.y = p_mission_pos.y;
	ent->position.z = p_mission_pos.z;
	return OK;
}

// World-registry probe seams keyed by SSN — pool-1 vehicles (and anything else
// without an AI brain) are invisible to the AI-index seams above; vehicle probes
// need to find and place them. Mission-space coordinates, same convention as
// debug_set_entity_position.
Dictionary Simulation::get_world_entity_debug(int p_net_id) const {
	Dictionary out;
	if (!world_ || p_net_id <= 0 || p_net_id > 0xFFFF) return out;
	const opennova::world::EntityHandle h =
			world_->registry.find_by_net_id(static_cast<uint16_t>(p_net_id));
	const opennova::world::Entity *ent = world_->registry.get(h);
	if (!ent) return out;
	out["net_id"] = static_cast<int>(ent->net_id);
	out["bms_id"] = ent->bms_id;
	out["pool"] = h.pool();
	out["kind"] = ent->spawn_origin == opennova::world::kSpawnOriginNone
			? -1
			: opennova::world::spawn_origin_kind(ent->spawn_origin);
	out["index"] = ent->spawn_origin == opennova::world::kSpawnOriginNone
			? -1
			: static_cast<int>(
					  opennova::world::spawn_origin_index(ent->spawn_origin));
	out["item_id"] = ent->item_id;
	out["name"] = String(ent->name.c_str());
	out["team"] = static_cast<int>(ent->team);
	out["alive"] = ent->alive;
	out["hidden"] = ent->hidden;
	out["health"] = ent->health;
	out["has_item_def"] = ent->has_item_def;
	out["handle"] = static_cast<int>(h.packed);
	out["item_type"] = static_cast<int>(ent->item_type);
	out["item_unit_type"] = ent->item_unit_type;
	out["item_attrib"] = static_cast<int64_t>(ent->item_attrib);
	out["item_attrib2"] = static_cast<int64_t>(ent->item_attrib2);
	out["vehicle_family"] = -1;
	if (const opennova::world::VehicleTraits *traits =
			world_->vehicle_traits.get(ent->item_id)) {
		out["vehicle_family"] = static_cast<int>(traits->family);
	}
	out["has_minimap_model_marker"] = ent->has_minimap_model_marker;
	out["is_capture_trigger"] = ent->is_capture_trigger;
	out["is_spawn_point"] = ent->is_spawn_point;
	out["zone_number"] = static_cast<int>(ent->zone_number);
	out["zone_radius"] = static_cast<int>(ent->zone_radius);
	out["zone_control"] = ent->zone_control;
	int zone_chain_index = -1;
	for (size_t i = 0; i < world_->zone_chain.zones.size(); ++i) {
		if (world_->zone_chain.zones[i] == h) {
			zone_chain_index = static_cast<int>(i);
			break;
		}
	}
	out["zone_chain_index"] = zone_chain_index;
	out["mission_position"] = Vector3(ent->position.x, ent->position.y, ent->position.z);
	out["position"] = Vector3(ent->position.x, ent->position.z, -ent->position.y);
	out["yaw"] = static_cast<int>(ent->yaw);
	out["primary_weapon_clip"] = ent->primary_weapon_slot.clip;
	out["primary_weapon_reserve"] = ent->primary_weapon_slot.reserve;
	out["seat_count"] = static_cast<int>(ent->seats.size());
	Array seats;
	for (const opennova::world::Seat &s : ent->seats) {
		Dictionary sd;
		sd["type"] = static_cast<int>(s.type);
		sd["occupied"] = s.occupant.valid();
		sd["local"] = Vector3(s.seat_local.x, s.seat_local.y, s.seat_local.z);
		sd["name"] = String(s.source_name.c_str());
		seats.push_back(sd);
	}
	out["seats"] = seats;
	return out;
}

// Probe/diagnostic seam beside debug_set_world_entity_position: land the
// LOCAL player at an exact dumped pose (mission position + mission yaw/pitch
// as the F3 Player-tab dump records them) — entity + AI-motor stores written
// together so the next motor tick continues from the pose instead of
// snapping back.
Error Simulation::debug_teleport_local_player(const Vector3 &p_mission_pos,
                                                  float p_yaw_deg, float p_pitch_deg) {
	if (!world_ || !world_->ai || !world_->cached.local_player.valid()) {
		return ERR_UNAVAILABLE;
	}
	const opennova::world::EntityHandle h = world_->cached.local_player;
	opennova::world::Entity *e = world_->registry.get(h);
	AiEntity *p = world_->ai->for_handle(h);
	if (e == nullptr || p == nullptr) return ERR_UNAVAILABLE;
	e->position.x = p_mission_pos.x;
	e->position.y = p_mission_pos.y;
	e->position.z = p_mission_pos.z;
	p->pos[0] = static_cast<int32_t>(p_mission_pos.x * 65536.0f);
	p->pos[1] = static_cast<int32_t>(p_mission_pos.y * 65536.0f);
	p->pos[2] = static_cast<int32_t>(p_mission_pos.z * 65536.0f);
	p->heading = opennova::world::bam_heading_from_mission_yaw_deg(p_yaw_deg);
	p->pitch = static_cast<int32_t>(
			static_cast<double>(p_pitch_deg) / opennova::world::kDegreesPerBam);
	return OK;
}

void Simulation::debug_set_world_entity_position(int p_net_id,
                                                     const Vector3 &p_mission_pos) {
	if (!world_ || p_net_id <= 0 || p_net_id > 0xFFFF) return;
	const opennova::world::EntityHandle h =
			world_->registry.find_by_net_id(static_cast<uint16_t>(p_net_id));
	opennova::world::Entity *ent = world_->registry.get(h);
	if (!ent) return;
	ent->position.x = p_mission_pos.x;
	ent->position.y = p_mission_pos.y;
	ent->position.z = p_mission_pos.z;
	// Keep the AI mirror in step when the entity carries a brain (harmless otherwise).
	if (ai_) {
		if (AiEntity *ae = ai_->for_handle(h)) {
			ae->pos[0] = static_cast<int32_t>(p_mission_pos.x * 65536.0f);
			ae->pos[1] = static_cast<int32_t>(p_mission_pos.y * 65536.0f);
			ae->pos[2] = static_cast<int32_t>(p_mission_pos.z * 65536.0f);
		}
	}
}

Error Simulation::debug_set_world_entity_weapon_ammo(
		int p_net_id, int p_clip, int p_reserve) {
	if (!world_ || p_net_id <= 0 || p_net_id > 0xFFFF)
		return ERR_INVALID_PARAMETER;
	const opennova::world::EntityHandle h =
			world_->registry.find_by_net_id(static_cast<uint16_t>(p_net_id));
	opennova::world::Entity *ent = world_->registry.get(h);
	if (ent == nullptr) return ERR_DOES_NOT_EXIST;
	ent->primary_weapon_slot.clip =
			opennova::world::retail_signed_i16(p_clip);
	ent->primary_weapon_slot.reserve =
			opennova::world::retail_signed_i16(p_reserve);
	return OK;
}

int Simulation::get_mission_variable(int index) const {
	return world_ ? world_->vars.get_mission(index) : 0;
}

Dictionary Simulation::get_round_outcome_debug() const {
	Dictionary out;
	if (!world_) return out;
	out["ended"] = world_->round_end.ended;
	out["winner_team"] = world_->round_end.winner_team;
	out["bluekills"] = world_->kill_stats.bluekills_by_player;
	out["greenkills"] = world_->kill_stats.greenkills_by_player;
	out["enemy_kills"] = world_->kill_stats.enemy_kills_by_player;
	out["team_kills_by_others"] = world_->kill_stats.team_kills_by_others;
	out["friendly_kills_by_others"] = world_->kill_stats.friendly_kills_by_others;
	out["enemy_kills_by_others"] = world_->kill_stats.enemy_kills_by_others;
	out["humans"] = world_->cached.humans;
	out["mp_session"] = world_->mp_session;
	return out;
}

bool Simulation::has_event_fired(int index) const {
	if (!bms_ || index < 0) return false;
	// The active latch + delay-elapsed window — the same read the original's Event
	// trigger category makes [orig: EventTrigger_EvaluateCondition @0x453620 case 3].
	return bms_->event_fired(static_cast<size_t>(index));
}

int Simulation::get_event_count() const {
	return bms_ ? static_cast<int>(bms_->events().size()) : 0;
}

int64_t Simulation::get_logic_tick() const {
	// uint32 -> int64 keeps long sessions sign-safe on the GDScript side.
	return world_ ? static_cast<int64_t>(world_->logic_tick) : 0;
}

void Simulation::set_panm_time_ms(int64_t p_time_ms) {
	panm_time_override_ms_ = p_time_ms < 0
			? -1
			: static_cast<int64_t>(static_cast<uint32_t>(p_time_ms));
}

int64_t Simulation::get_panm_time_ms() const {
	return panm_time_override_ms_;
}

void Simulation::debug_set_panm_time_ms(int64_t p_time_ms) {
	set_panm_time_ms(p_time_ms);
}

namespace {
PackedInt32Array snapshot_bank(const opennova::world::World *world, int count,
                               int32_t (opennova::world::ScriptVarStore::*getter)(int) const) {
	PackedInt32Array out;
	out.resize(count);
	int32_t *w = out.ptrw();
	for (int i = 0; i < count; ++i) {
		w[i] = world ? (world->vars.*getter)(i) : 0;
	}
	return out;
}
} // namespace

PackedInt32Array Simulation::get_mission_variables_snapshot() const {
	return snapshot_bank(world_.get(), opennova::world::ScriptVarStore::kMissionVars,
	                     &opennova::world::ScriptVarStore::get_mission);
}

PackedInt32Array Simulation::get_global_variables_snapshot() const {
	return snapshot_bank(world_.get(), opennova::world::ScriptVarStore::kGlobalVars,
	                     &opennova::world::ScriptVarStore::get_global);
}

PackedInt32Array Simulation::get_music_variables_snapshot() const {
	return snapshot_bank(world_.get(), opennova::world::ScriptVarStore::kMusicVars,
	                     &opennova::world::ScriptVarStore::get_music);
}

void Simulation::set_global_variable(int index, int value) {
	if (world_) world_->vars.set_global(index, value);
}

int Simulation::get_global_variable(int index) const {
	return world_ ? world_->vars.get_global(index) : 0;
}

PackedByteArray Simulation::get_fired_events_snapshot() const {
	PackedByteArray out;
	if (!bms_) return out;
	const size_t count = bms_->events().size();
	out.resize(static_cast<int64_t>(count));
	uint8_t *w = out.ptrw();
	for (size_t i = 0; i < count; ++i) {
		w[i] = bms_->event_fired(i) ? 1 : 0;
	}
	return out;
}

void Simulation::set_loco_scale(int p_scale) {
	if (ai_) ai_->loco_scale = p_scale;
}

int Simulation::get_loco_scale() const {
	return ai_ ? ai_->loco_scale : 0;
}
