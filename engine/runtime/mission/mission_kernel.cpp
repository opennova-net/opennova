// mission::MissionKernel — the promoted retail-mission rig body (ADR 0042 d3).
// Every leg here is the one engine implementation the embedders share; the
// witness citations moved with the bodies.

#include <runtime/mission/mission_kernel.h>

#include <base/io/bam.h>
#include <base/io/log.h>
#include <base/io/strutil.h>
#include <base/vfs/vfs.h>
#include <formats/mission/bms_edit.h> // mission_info (the BMS tile set)
#include <formats/mission/mission.h>
#include <runtime/mission/item_traits.h>
#include <runtime/mission/mission_sidecars.h>
#include <runtime/mission/seat_spec_extract.h>
#include <runtime/terrain_query/height_field.h>
#include <runtime/terrain_query/terrain_field_build.h> // the terrain field's file entry (ADR 0043 E9)
#include <runtime/wac/wac_layered_load.h>
#include <runtime/world/ammo_table_build.h>
#include <runtime/world/angle.h>
#include <runtime/world/infantry.h>
#include <runtime/world/entity_spawn.h>
#include <runtime/world/mount_controls.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/radar_contacts.h>
#include <runtime/world/round_sim.h>
#include <runtime/world/spawn_select.h>
#include <runtime/world/vehicle_attach.h>
#include <runtime/world/weapon_fsm.h>
#include <runtime/world/weapon_table_build.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>
#include <vector>

using namespace opennova::def;
using namespace opennova::threedi;

namespace opennova::mission {

namespace w = opennova::world;

MissionKernel::MissionKernel() : local(world) {
	world.local_player_state = &local;
	world.teammate_spawner = this;
	world.item_piece_spawner = this;
	occlusion.bind_focal_wind_random(&world.prng16_c_state);
	// The world's weapon-action walk pumps the local player's slot (its own or
	// the UseGun parent slot it borrowed) through this LocalPlayer, with the
	// live trigger/reload/scope inputs, at L's own pool-0 slot; the AI pump
	// never advances it as well [orig: one WeaponAction_ProcessAllEntities
	// walk @0x542690].
	world.profile = &profile;
}

MissionKernel::~MissionKernel() {
	// The systems and providers the world points at outlive nothing: drop the
	// non-owning links before the members tear down in reverse order.
	world.teammate_spawner = nullptr;
	world.item_piece_spawner = nullptr;
	world.local_player_state = nullptr;
	world.collision = nullptr;
	world.pose_provider = nullptr;
	world.tables.terrain = nullptr;
	world.ai.collision = nullptr;
	world.ai.terrain = nullptr;
	world.ai.root_motion = nullptr;
	collision.set_pose_provider(nullptr);
	if (weapon_defs_ok) def_free_weapons(&weapon_defs);
	if (items_ok) def_free_items(&items);
}

// --- open / boot ------------------------------------------------------------

bool MissionKernel::open(const std::string &root, const std::string &name,
		std::string &error, const std::string &expansion) {
	root_dir = root;
	// A packed install mounts its .pff set with loose overrides; a loose asset
	// tree (no archives) mounts as loose files only.
	bool mounted = index.scan(root_dir, expansion);
	if (!mounted || !index.has_file(name))
		mounted = index.scan(root_dir, expansion, VfsMountMode::LooseOnly);
	if (!mounted) {
		error = "could not mount " + root_dir + ": " + index.last_error();
		return false;
	}
	std::vector<uint8_t> bytes;
	if (!index.read_file(name, bytes)) {
		error = name + " is not under " + root_dir;
		return false;
	}
	std::string parse_error;
	bms::File parsed;
	if (!bms::parse(bytes.data(), bytes.size(), parsed, parse_error)) {
		error = name + " did not parse: " + parse_error;
		return false;
	}
	BootFileSource files = boot_files_from_index(index);
	// The by-name readers' base: the name cut at its first '.'
	// (mission_sidecars.h mission_base_name).
	open_document(std::move(parsed), mission_base_name(name), std::move(files));
	mission_name = name;
	return true;
}

void MissionKernel::open_document(bms::File mission_doc,
		std::string mission_file_basename, BootFileSource files) {
	mission = std::move(mission_doc);
	mission_name = mission_file_basename;
	mission_basename = std::move(mission_file_basename);
	opened_ = true;
	files_ = std::move(files);
	world.script.voice.set_file_reader(files_.read_file);
	world.tables.voice_macros = {};
	if (files_.read_file) {
		std::vector<uint8_t> bytes;
		std::string error;
		if (files_.read_file("vmacros.bin", bytes))
			rtxt::parse(bytes.data(), bytes.size(), world.tables.voice_macros, error);
	}
	if (items_ok) {
		def_free_items(&items);
		items = DefItemsFile{};
		items_ok = false;
	}
	// An embedder-supplied table (set_items_table) IS the item db — parsing a
	// second copy from the mount would only shadow it.
	std::vector<uint8_t> items_bytes;
	if (items_override_ == nullptr && files_.valid() &&
			files_.read_file("items.def", items_bytes) &&
			def_parse_items_memory(items_bytes.data(), items_bytes.size(), &items) == 0)
		items_ok = true;
}

void MissionKernel::set_assets(const assets::AssetStore *source) {
	external_assets_ = source;
	// A source switch invalidates every retained parse-derived pose, exactly
	// like the shell's asset-root switch did.
	mounted_rest_cache_.clear();
	mounted_live_cache_.clear();
	mounted_cache_tick_ = 0xFFFFFFFFu;
	collision_pose.set_assets(&assets());
}

void MissionKernel::resolve_item_traits(mission::ItemWireClassFn wire_class) {
	// Rebuild on an explicit definition sweep, including in-place edits.
	// Rebinding the retained source for animation/collision is not a sweep.
	world.vehicles.traits.clear();
	item_wire_class_ = std::move(wire_class);
	resweep_item_traits();
	// Restore model-derived probes/trail anchors from the retained model cache.
	refresh_collision_instances();
}

void MissionKernel::resweep_item_traits() {
	if (item_wire_class_ && items_table() != nullptr)
		mission::resolve_item_traits(world, *items_table(), item_wire_class_);
	if (items_table() != nullptr)
		world.facials.configure(world, asset_index(), *items_table());
	if (items_table() != nullptr && !world.tables.ammo.entries.empty())
		mission::resolve_minefields(world, *items_table(), assets());
}

void MissionKernel::bind_spawned_body(w::EntityHandle handle) {
	const opennova::def::DefItemsFile *items = items_table();
	if (items == nullptr || !handle.valid()) return;
	// The boot's own classifier until the embedder supplied its wire classes.
	if (item_wire_class_)
		mission::resolve_item_traits(world, *items, item_wire_class_, handle);
	else
		mission::resolve_item_traits(world, *items,
				[](int32_t) -> uint8_t { return 0; }, handle);
	mission::resolve_ai_weapons(world, *items, handle, &assets());
	ensure_collision_instance(world, handle);
}

int MissionKernel::resolve_collision_instances() {
	if (items_table() == nullptr) return 0;
	collision_items_resolved_ = true;
	// The kernel is the ONE registered section-matrix/mounted-pose provider;
	// wire_collision points the world/AI systems at its collision world.
	wire_collision();
	const mission::CollisionResolveDeps deps{collision, occlusion, collision_pose, assets()};
	collision_attached = mission::resolve_collision_instances(
			world, *items_table(), collision_state, deps);
	return collision_attached;
}

int MissionKernel::refresh_collision_instances() {
	if (!collision_items_resolved_) return 0;
	return resolve_collision_instances();
}

w::ResolvedCollisionShape MissionKernel::wire_collision_shape_for_type(uint16_t type_id) {
	const auto cached = wire_collision_shape_by_type_.find(type_id);
	if (cached != wire_collision_shape_by_type_.end()) return cached->second;
	w::ResolvedCollisionShape shape;
	if (collision_items_resolved_ && items_table() != nullptr) {
		const mission::CollisionResolveDeps deps{collision, occlusion, collision_pose, assets()};
		shape = mission::collision_shape_for_runtime_type(
				static_cast<int>(type_id), *items_table(), collision_state, deps);
	}
	wire_collision_shape_by_type_.emplace(type_id, shape);
	return shape;
}

void MissionKernel::retire_replica_entity(const w::EntityLifetime &lifetime) {
	if (!lifetime.valid()) return;
	const auto cached = collision_state.resolution_attempted.find(lifetime.handle.packed);
	if (cached != collision_state.resolution_attempted.end() &&
			cached->second != lifetime.registry_spawn_id)
		return;
	const w::EntityHandle handle = lifetime.handle;
	collision.remove_entity_instance(handle);
	occlusion.remove_entity_instance(handle);
	collision_pose.remove_entity(handle);
	collision_state.resolution_attempted.erase(handle.packed);
}

void MissionKernel::occlusion_init_mission() {
	collision.build_initial_tables(world);
	// The mission-start blink stamp runs ahead of the portal init.
	// [orig: Game_StartMission — Entity_BuildProximityListsForPools12
	//  @ 0x525898, Terrain_InitBuildingPortals @ 0x525e11]
	collision.refresh_mission_start_blink(world);
	occlusion.init_mission(world, collision);
}

int MissionKernel::adm_id_for_runtime_type(uint16_t type_id) {
	const auto cached = adm_by_runtime_type_.find(type_id);
	if (cached != adm_by_runtime_type_.end()) return cached->second;
	const DefItemsFile *item_rows = items_table();
	if (!infantry_adm_retained_ || item_rows == nullptr) return -2;
	const assets::AssetStore *adm_source = adm_assets_ != nullptr ? adm_assets_ : &assets();
	const int visual = mission::visual_item_id_for_runtime_type(type_id, *item_rows);
	const DefItemDef *def = mission::find_item_def(*item_rows, visual);
	int adm_id = -1;
	if (def != nullptr && def->anim_def[0] != '\0') {
		std::string adm = def->anim_def;
		if (!strutil::ends_with_icase(adm, ".adm")) adm += ".adm";
		adm_id = root_motion.register_adm(adm_source, adm);
	}
	if (adm_id < 0) adm_id = default_infantry_adm_id_;
	world.ai.root_motion = root_motion.empty() ? nullptr : &root_motion;
	adm_by_runtime_type_[type_id] = adm_id;
	return adm_id;
}

int MissionKernel::cached_adm_id_for_runtime_type(uint16_t type_id) const {
	const auto cached = adm_by_runtime_type_.find(type_id);
	return cached != adm_by_runtime_type_.end() ? cached->second : -1;
}

void MissionKernel::set_items_table(const DefItemsFile *items_table_ptr) {
	items_override_ = items_table_ptr;
}

void MissionKernel::sync_water_plane() {
	terrain_store.set_water_plane(world.env.water_z);
}

void MissionKernel::wire_terrain() {
	sync_water_plane();
	world.tables.terrain = terrain_store.valid() ? &terrain_store.height_field() : nullptr;
	world.ai.terrain = terrain_store.valid() ? &terrain_store.height_field() : nullptr;
	world.ai.ground_clearance = w::GroundClearance{};
	collision.terrain = terrain_store.valid() ? &terrain_store.height_field() : nullptr;
	// The footstep surface pick reads the charmap through this view, the
	// store's placed-tile overlay (D-SND-15) riding it, so a re-wire keeps the
	// tiles; the shell's apply_terrain_to_ai calls this and then re-layers its
	// sound profiles.
	world.tables.surface_map = terrain_store.surface_map();
}

bool MissionKernel::load_terrain_field() {
	if (asset_index() == nullptr) return false;
	std::string terrain_error;
	const MissionInfo info = mission_info(mission);
	if (!terrain::terrain_field_store_load(terrain_store, *asset_index(), mission.get_terrain(), info.tile_set,
				info.environment, terrain_error)) {
		io::logf(io::LogLevel::kWarn,
				"mission kernel: terrain not loaded (%s) - the ground solve will not run",
				terrain_error.c_str());
		return false;
	}
	return true;
}

void MissionKernel::set_placed_tiles(const std::vector<uint8_t> &til_bytes) {
	terrain::terrain_field_store_set_placed_tiles(terrain_store, til_bytes);
	world.tables.surface_map = terrain_store.surface_map();
}

void MissionKernel::resolve_tile_surface_table() {
	terrain::terrain_field_store_resolve_tile_surfaces(terrain_store, files_.has_file,
			files_.read_file);
	world.tables.surface_map = terrain_store.surface_map();
}

void MissionKernel::wire_collision() {
	collision.terrain = terrain_store.valid() ? &terrain_store.height_field() : nullptr;
	collision.set_pose_provider(this);
	world.collision = &collision;
	world.pose_provider = this;
	world.ai.collision = &collision;
}

std::function<PromoteOptions::AiProfileDefaults(int32_t)>
MissionKernel::ai_profile_defaults_fn() const {
	if (items_table() == nullptr) return {};
	return [this](int32_t type_id) {
		PromoteOptions::AiProfileDefaults d;
		const int item_id = static_cast<int>(type_id) + static_cast<int>(kItemIdOffset);
		const DefItemDef *def = mission::find_item_def(*items_table(), item_id);
		if (def == nullptr) return d;
		const std::string cls = strutil::to_lower(def->ai_function);
		d.helicopter_init = cls == "chel" || cls == "cpln";
		d.known = d.helicopter_init || cls == "cveh" || cls == "cbot" || cls == "ctrn";
		d.default_aip = def->default_aip;
		return d;
	};
}

bool MissionKernel::load_mission_into_world(const KernelBootOptions &options) {
	PromoteOptions opts;
	opts.player_limit = options.player_limit;
	opts.team_count = options.team_count;
	opts.game_type = options.game_type;
	if (items_table() != nullptr) {
		opts.item_attributes = [this](int32_t type_id) {
			const auto *def = mission::find_item_def(*items_table(),
					static_cast<int>(type_id) + static_cast<int>(kItemIdOffset));
			return def != nullptr ? def->attrib : 0u;
		};
	}
	opts.item_seat_specs = seat_specs;
	opts.ai_profiles = ai_profiles;
	opts.ai_profile_defaults = ai_profile_defaults_fn();
	// Authored display names from the embedder's [PeopleNames] table (D-HUD-20);
	// promote applies the retail 15-char copy at its cited port site.
	opts.people_name_resolver = people_name_resolver_;
	promo = promote_mission(mission, world, opts);
	// DEF initialization precedes PreMission actions in retail. Marker/health
	// predicates and dynamically spawned helpers must see those traits now.
	// [orig: Entity_SpawnFromBMSRecord @0x40E9F0 -> Entity_InitFromModel]
	if (items_table() != nullptr)
		mission::resolve_item_traits(world, *items_table(), item_wire_class_);
	register_mission_systems();
	return true;
}

void MissionKernel::register_mission_systems() {
	events.load(mission.events, mission.triggers, mission.actions);
	world.tables.mission_attrib_flags = static_cast<uint32_t>(mission.header.attrib_flags);
	// The net half stands its session up here — between the world wiring and
	// the system registration, exactly where the SP listen host's bring-up
	// sits inside the load (inmatch::HostRole::bring_up_singleplayer)
	// [orig: SinglePlayer_StartMission @0x561af0].
	if (bringup_net_session_) bringup_net_session_();
	// The mission's script systems register in the faithful within-tick
	// order, then load (each system's on_load, then the AI's). Order: WAC ->
	// the every-32 idle legs -> BMS; the World's entity update follows them
	// every tick, so it consumes the entity state the scripts mutate.
	// [orig: Game_ProcessMainFrame @0x5266b6 (the Server_TickUpdate call, whose
	//  Server_TickUpdate @0x51d8bf WacScript_AdvanceTick call runs the WAC
	//  executor first, @0x51d8d2/@0x51d8d7 the every-32 spawn-marker and
	//  idle-timer calls next and @0x51d8f4 the BMS event quarter pass) precedes
	//  Game_ProcessMainFrame @0x52674b (the Entity_UpdateAllEntities call).]
	// Each system carries its own cadence gate (WAC every 62nd tick, the idle
	// legs every 32nd, BMS quarters every 16th), so the registration order
	// only fixes the within-tick sequence.
	world.add_system(&wac);
	world.add_system(&world.server_idle_legs);
	world.add_system(&events);
	world.load_systems();

}

void MissionKernel::capture_baseline() {
	baseline = world.snapshot();
	world.ai.capture_spawn_baseline();
	wac_baseline = wac.capture_runtime_state();
	have_baseline = true;
	have_wac_baseline = true;
}

int MissionKernel::spawn_local_player_at_start(uint32_t game_type) {
	if (local.has_local_player()) return 1;
	const w::SpawnPointResult sel = w::resolve_player_spawn_pose(
			world, w::EntityHandle{}, w::EntityHandle{}, 0, 1, game_type);
	w::PlayerSpawn spawn;
	if (sel.found) {
		spawn.position = sel.position;
		spawn.yaw = sel.yaw;
		spawn.heading_bam = sel.heading_bam;
		spawn.pitch = sel.pitch;
		spawn.roll = sel.roll;
	}
	spawn.team = 1;
	if (!spawn_local_player(spawn)) return -1;
	// The Co-op marker arm's chute bit and queued carrier mount.
	// [orig: Server_PositionPlayerForSpawn @0x50D424..0x50D45A]
	if (w::Entity *player = world.registry.get(world.cached.local_player))
		w::apply_spawn_point_latches(*player, sel);
	return sel.found ? 1 : 0;
}

bool MissionKernel::spawn_local_player(const w::PlayerSpawn &spawn) {
	const w::EntityHandle h = w::spawn_player(world, spawn);
	if (!h.valid()) return false;
	resolve_new_infantry_adm_ids();
	// The look yaw starts at the spawn's heading word [orig:
	// Entity_FindBestSpawnPoint @0x50CF4D].
	local.reset_local_player_input(w::player_spawn_heading(spawn));
	w::local_player_view_reset(&world, local.weapon, local.view, local.view_tracker);
	return true;
}

void MissionKernel::resolve_new_infantry_adm_ids() {
	if (!infantry_adm_retained_ || items_table() == nullptr) return;
	const int count = world.ai.count();
	if (infantry_adm_resolved_ai_count_ < 0 || infantry_adm_resolved_ai_count_ > count)
		infantry_adm_resolved_ai_count_ = 0;
	for (int i = infantry_adm_resolved_ai_count_; i < count; ++i) {
		w::AiEntity *e = world.ai.at(i);
		if (e == nullptr) continue;
		e->inf.adm_id = default_infantry_adm_id_;
		if (!e->inf.active) continue;
		const w::Entity *ent = world.registry.get(e->handle);
		if (ent == nullptr) continue;
		e->inf.adm_id = adm_id_for_runtime_type(ent->item_id);
	}
	infantry_adm_resolved_ai_count_ = count;
	world.ai.root_motion = root_motion.empty() ? nullptr : &root_motion;
}

void MissionKernel::reset_infantry_adm_ids() {
	adm_by_runtime_type_.clear();
	++infantry_adm_revision_;
	infantry_adm_resolved_ai_count_ = 0;
	for (int i = 0; i < world.ai.count(); ++i)
		if (w::AiEntity *e = world.ai.at(i)) e->inf.adm_id = default_infantry_adm_id_;
	resolve_new_infantry_adm_ids();
}

void MissionKernel::rearm_infantry_adm(const assets::AssetStore *adm_assets) {
	if (adm_assets != nullptr) adm_assets_ = adm_assets;
	infantry_adm_retained_ = true;
	reset_infantry_adm_ids();
}

int MissionKernel::install_infantry_anim(const std::string &adm_name,
		const assets::AssetStore *adm_assets) {
	adm_assets_ = adm_assets != nullptr ? adm_assets : &assets();
	root_motion.clear();
	// Registration id 0 may belong to a model-specific map when the configured
	// default is absent. Only the configured map may serve as the fallback.
	default_infantry_adm_id_ = root_motion.register_adm(adm_assets_, adm_name);
	if (default_infantry_adm_id_ < 0)
		io::logf(io::LogLevel::kWarn,
				"mission kernel: no default infantry clips from '%s' - model-specific maps remain available",
				adm_name.c_str());
	// A source with no clips counts as none: the selector then resolves every
	// state to "no clip" and soldiers stand, exactly the original's
	// relationship between motion and clips.
	world.ai.root_motion = root_motion.empty() ? nullptr : &root_motion;
	reset_infantry_adm_ids();
	return root_motion.clip_count(default_infantry_adm_id_);
}

bool MissionKernel::load_weapon_table(const BootFileSource &files,
		const assets::AssetStore *table_assets, const std::string &name) {
	std::vector<uint8_t> bytes;
	if (!files.valid() || !files.read_file(name, bytes)) return false;
	DefWeaponsFile file = {};
	// A SIGHTS row whose texture the mount lacks is no row [orig: the sights
	// arm's FileSystem_FileExists @0x544AE2].
	const DefFileProbe probe = {
			[](const void *ctx, const char *name) {
				return static_cast<const BootFileSource *>(ctx)->has_file(name);
			},
			&files};
	if (def_parse_weapons_memory(bytes.data(), bytes.size(), &file, nullptr, &probe) != 0) return false;
	world.tables.weapons = w::build_weapon_table(file,
			table_assets != nullptr ? table_assets : &assets());
	if (weapon_defs_ok) def_free_weapons(&weapon_defs);
	weapon_defs = file;
	weapon_defs_ok = true;
	mission::stamp_seat_spec_turret_limits(world, seat_specs);
	// The authoritative side's own player spawned before this feed: re-stamp
	// its equipped default now that WPN_M4AUTO resolves by name
	// [orig: PlayerClass_InitEntity @0x4B1116] (D-NET-143).
	const int m4 = world.tables.weapons.index_of("WPN_M4AUTO");
	if (m4 >= 0) {
		std::vector<w::EntityHandle> handles;
		world.registry.for_each([&](const w::Entity &e) {
			if (e.item_id == w::kPlayerInfantryTypeId && e.equipped_adm_index == w::kAdmSlotNone)
				handles.push_back(e.handle);
		});
		for (const w::EntityHandle h : handles)
			if (w::Entity *e = world.registry.get(h)) e->equipped_adm_index = static_cast<uint8_t>(m4);
	}
	// The mission's stashed local.loadout/availability chunks promote NOW, through
	// the witnessed SP-vs-net gate [orig: Mission_LoadBMSFile @ 0x40F4E0 — gate
	// @ 0x40f694], then the local player's slot pool builds from the spawn kit
	// and selects the spawn default — the Player_InitPlayer local.weapon leg
	// [orig: @ 0x4e15f0; the default kit literal @ 0x5246be].
	std::vector<std::pair<std::string, int32_t>> availability_rows;
	std::vector<w::WeaponKitEntry> kit_rows;
	stash_mission_loadout_rules(mission, availability_rows, kit_rows);
	if (w::local_loadout_promote_mission_rules(world, local.loadout, availability_rows, std::move(kit_rows)))
		w::local_player_view_reset(&world, local.weapon, local.view, local.view_tracker);
	w::local_loadout_rebuild(world, local.loadout, local.weapon, local.inventory, local.inventory_valid,
			/*select_spawn_default=*/true);
	return true;
}

bool MissionKernel::load_ammo_table(const BootFileSource &files,
		const std::string &name) {
	std::vector<uint8_t> bytes;
	if (!files.valid() || !files.read_file(name, bytes)) return false;
	DefAmmoFile file = {};
	if (def_parse_ammo_memory(bytes.data(), bytes.size(), &file) != 0) return false;
	world.tables.ammo = w::build_ammo_table(file);
	def_free_ammo(&file);
	// The whiz radius rides the loaded sound sets (the boot mounts them first).
	w::resolve_ammo_whiz_radii(world.tables.ammo, world.tables.sound_sets);
	w::resolve_weapon_round_types(world.tables.weapons, world.tables.ammo);
	w::local_loadout_sync_damage_classes(world, local.loadout);
	ammo_ok = true;
	return true;
}

bool MissionKernel::load_powerup_table(const BootFileSource &files,
		const std::string &name) {
	world.tables.powerups = w::PowerupTable{};
	std::vector<uint8_t> bytes;
	if (!files.valid() || !files.read_file(name, bytes)) return false;
	def::DefPowerupFile file = {};
	if (def::def_parse_powerup_memory(bytes.data(), bytes.size(), &file) != 0) return false;
	world.tables.powerups = w::build_powerup_table(file, world.tables.weapons);
	def::def_free_powerup(&file);
	return true;
}

bool MissionKernel::boot(const KernelBootOptions &options, std::string &error) {
	mission_start_pending = false;
	have_baseline = false;
	have_wac_baseline = false;
	if (!opened_) {
		error = "open() / open_document() first";
		return false;
	}
	// [orig: SinglePlayer_StartMission @0x561af0 / host setup precede
	// Game_StartMission @0x524360 and Mission_LoadBMSFile @0x40f4e0]
	world.rules.mp_session = options.mp_session || options.joiner;
	world.rules.projectile_authority = !options.joiner;
	// The shared model source, wired before the seat step runs (S16).
	collision_pose.set_assets(&assets());
	bringup_net_session_ = options.bringup_net_session;
	people_name_resolver_ = options.people_name_resolver;
	restart_boot_ = options.restart;
	boot_trace.clear();

	// The mission's .cpt/.trn(+charmap) height field: the shell hands its
	// parsed documents over before the boot (terrain_field_store_build, the
	// parsed-document entry); an embedder that holds none (the ctests) has the
	// kernel load through its own index here (the file entry) — one builder,
	// two entries, both through height_field_apply_trn.
	if (options.terrain && !terrain_store.valid()) (void)load_terrain_field();

	// The gates: a missing file source skips every file-fed step, a missing
	// item db the trait/collision steps, a joiner never spawns its own player
	// here (L spawns on the name-match inside the joiner frame).
	const bool has_files = files_.valid();
	const bool has_item_db = items_table() != nullptr;
	const auto step = [this](const char *name) { boot_trace.emplace_back(name); };

	// Seat/mount specs install before promotion (they persist across resets).
	if (has_files && has_item_db) {
		step("seat_specs");
		seat_specs.clear();
		mounted_graphics.clear();
		if (options.seat_specs) {
			std::vector<int> seeds;
			const auto seed_group = [&seeds](const std::vector<bms::Entity> &v) {
				for (const bms::Entity &e : v)
					if (e.type_id > 0)
						seeds.push_back(static_cast<int>(e.type_id) + static_cast<int>(kItemIdOffset));
			};
			seed_group(mission.items);
			seed_group(mission.buildings);
			seed_group(mission.markers);
			seed_group(mission.organics);
			if (!seeds.empty()) {
				mission::SeatSpecExtraction native;
				mission::extract_item_seat_specs(*items_table(),
						[this](const std::string &graphic) { return assets().model(graphic).get(); },
						seeds, native);
				seat_specs = std::move(native.specs);
				mounted_graphics = std::move(native.graphic_by_type);
			}
			std::sort(seat_specs.begin(), seat_specs.end(),
					[](const ItemSeatSpec &a, const ItemSeatSpec &b) { return a.type_id < b.type_id; });
			mission::stamp_seat_spec_turret_limits(world, seat_specs);
		}
	}
	// The .aip profiles per mission ai_textfile — without them, unscripted AI
	// vehicles crawl at the promote stand-in speed and SM weapons stay unarmed.
	if (has_files) {
		step("ai_profiles");
		ai_profiles = resolve_ai_profiles(files_, mission, ai_profile_defaults_fn());
	}
	// The per-mission RTXT table retail's NetPacket_WriteBriefingText reads
	// for world-stream phase 6 (S2C 0x7E). Runs unconditionally.
	{
		step("mission_text");
		std::vector<uint8_t> text;
		text_source = resolve_mission_text(files_, mission_basename, text);
		text_size = text.size();
		// g_TextMission, the table the dialog lines' subtitles read.
		world.tables.mission_text = nullptr;
		mission_text_table = rtxt::File{};
		std::string text_error;
		if (!text.empty() && rtxt::parse(text.data(), text.size(), mission_text_table, text_error))
			world.tables.mission_text = &mission_text_table;
	}
	// Load + promote the mission; a failure aborts the boot (nothing later
	// runs). (The shell re-stamps its presentation/PANM clock right after the
	// boot — the load reset cleared it; an order-free scalar, not a boot step.)
	step("load_mission");
	if (!load_mission_into_world(options)) {
		error = "mission boot aborted (load failed)";
		return false;
	}
	// Ground the AI on the terrain field.
	if (terrain_store.valid()) {
		step("terrain");
		wire_terrain();
	}
	// The infantry clip set (.adm -> .bad root-motion tracks).
	if (has_files) {
		step("infantry_anim");
		(void)install_infantry_anim(options.infantry_adm);
	}
	// The script compiler's SOUNDSET and FX name catalogs, from the mounted
	// banks and effect documents [orig: WacScript_ResolveParameter @0x4F2920
	// binds both against the loaded tables; CEffectWorld_InternEffectHandle
	// @0x5F7310]. Independent of the wac gate: a compile that arrives after
	// the boot (Simulation::compile_and_set_wac) binds the same names.
	if (has_files) {
		step("script_catalogs");
		wac::load_script_sound_sets(files_, script_sound_catalog);
		world.tables.sound_sets = &script_sound_catalog;
		// The mission's dialog bank when it exists, <mission>.dbf or the one the
		// header's slot names, and with it the bank's sounds, <bank>.lwf else
		// <bank>.pwf. [orig: DialogSystem_Init @0x52760c..0x527659 ->
		// DialogManager_LoadFromFile @0x44e650, the sounds @0x44e7d4..0x44e807]
		world.tables.dialog_bank = nullptr;
		world.tables.dialog_sounds = nullptr;
		dialog_bank = dbf::File{};
		dialog_sounds = lwf::File{};
		const std::string bank_name = mission::dialog_bank_name(mission_basename,
				strutil::fixed_string(mission.header.terrain + 16, 16));
		std::vector<uint8_t> dbf_bytes;
		std::string dbf_error;
		if (files_.read_file(bank_name, dbf_bytes) &&
				dbf::parse_dbf_memory(dbf_bytes.data(), dbf_bytes.size(), dialog_bank, dbf_error)) {
			world.tables.dialog_bank = &dialog_bank;
			std::vector<uint8_t> lwf_bytes;
			std::string lwf_error;
			const std::string sounds = mission::dialog_sounds_name(bank_name);
			const std::string sounds_alternate = mission::dialog_sounds_name(bank_name, true);
			if ((files_.read_file(sounds, lwf_bytes) || files_.read_file(sounds_alternate, lwf_bytes)) &&
					lwf::parse_lwf_buffer(lwf_bytes.data(), lwf_bytes.size(), dialog_sounds, lwf_error))
				world.tables.dialog_sounds = &dialog_sounds;
		}
		// SndProf.def -> the footstep/foley/landing/scream slot table. The
		// parse appends, so it runs only over an EMPTY table: a table the
		// embedder filled before the boot (Simulation::set_sound_profiles,
		// the tests/tools override) wins. [orig: SoundProfile_LoadAll
		// @0x527490 from Game_InitSubsystems]
		if (world.tables.sound_profiles.empty()) {
			std::vector<uint8_t> profile_bytes;
			if (files_.read_file("SndProf.def", profile_bytes))
				world.tables.sound_profiles.parse(
						reinterpret_cast<const char *>(profile_bytes.data()), profile_bytes.size());
		}
		particle::EffectSceneConfig effects_config;
		wac::load_script_effect_catalog(files_, script_effect_catalog, &effects_config);
		auto effects = std::make_shared<particle::EffectScene>();
		effects->open(effects_config);
		world.item_emitters.bind_scene(std::move(effects), true);
	}
	// Compile the mission WAC scripts; absent files install an empty program. The
	// numbered-variable reset belongs after PreMission, immediately before
	// initial execution in complete_mission_start.
	// [orig: WacScript_InitAndLoad @0x4F91F0, reset @0x4F95EE]
	if (options.wac) {
		step("wac");
		// A non-authoritative load compiles no layer: it installs only the
		// terminator. [orig: WacScript_InitAndLoad @0x4F9437 (the authority
		// test), @0x4F944E (jz past the three compiles), @0x4F95A9 ('zzzz')]
		const mission::BootFileSource no_layers{};
		const wac::WacLayeredLoadStatus status = wac::wac_layered_load(wac,
				world.rules.projectile_authority ? files_ : no_layers,
				options.wac_basename.empty() ? mission_basename : options.wac_basename,
				&world, &script_effect_catalog, &script_sound_catalog, options.music_globals);
		wac_loaded = status == wac::WacLayeredLoadStatus::kLoaded;
	}
	// The host's own player as an authoritative pool-0 entity (ADR 0012 /
	// net-re §5.2b) — after load (the spawn needs the AI system wired). A
	// joiner's L spawns on the name-match instead.
	if (options.playable && !options.joiner) {
		step("spawn_local_player");
		const int status = spawn_local_player_at_start(options.game_type);
		if (status < 0)
			io::logf(io::LogLevel::kWarn, "mission kernel: spawn_local_player failed");
		else if (status == 0)
			io::logf(io::LogLevel::kWarn,
					"mission kernel: no player-start marker - spawned at the origin");
	}
	// Per-entity grounding: each soldier's OWN model .adm (D-INF-6). After
	// the NPC promote AND the player spawn so both are covered.
	if (has_files && has_item_db) {
		step("infantry_adm");
		rearm_infantry_adm();
	}
	// items.def wire traits onto every entity + the replica-pipeline class
	// table (D-NET-97; §5.10b). The kernel's own sweep classifies every
	// definition Unknown; the embedder's sweep re-stamps with its wire-class
	// source (resolve_item_traits).
	if (has_item_db) {
		step("item_traits");
		mission::resolve_item_traits(world, *items_table(),
				[](int32_t) -> uint8_t { return 0; });
		world.facials.configure(world, asset_index(), *items_table());
	}
	if (has_item_db && options.collision) {
		// World-object collision instances (BVOL/BPLN) [orig: the movement
		// collision resolver @0x4b2bd0 + the query set; §15] over the sim's
		// own .3di source (ADR 0028).
		step("collision");
		collision_attached = resolve_collision_instances();
		// Mission-start portal init over the occlusion models just attached.
		step("occlusion");
		occlusion_init_mission();
	}
	if (has_files) {
		// Armory table (weapon.def) — the 0x5A ammo resolve + 0x2F filter
		// source; the stashed mission loadout/availability chunks promote
		// INSIDE it through the witnessed SP-vs-net gate (S7b)
		// [orig: Mission_LoadBMSFile @0x40F4E0 — gate @0x40f694].
		step("weapon_table");
		if (!load_weapon_table(files_, nullptr))
			io::logf(io::LogLevel::kWarn, "mission kernel: local.weapon.def not loaded");
		// Ballistics table (ammo.def) + round_type resolve.
		step("ammo_table");
		const bool ammo_ok_now = load_ammo_table(files_);
		// Minefield resources require a loaded ammo table.
		if (ammo_ok_now && has_item_db) {
			mission::resolve_minefields(world, *items_table(), assets());
		}
		// The powerup rows (powerup.def), after the weapon table its names
		// resolve over [orig: Game_StartMission @0x5256CD].
		step("powerup_table");
		if (!load_powerup_table(files_))
			io::logf(io::LogLevel::kWarn,
					"mission kernel: Unable to load powerup.def - every Powerup row is destroyed at the bind");
	}
	if (has_item_db) {
		step("ai_weapons");
		mission::resolve_ai_weapons(world, *items_table(), {}, &assets());
		// The powerup bind over pools 1 and 2: each Powerup row takes its
		// powerup.def row by the item's `powerupdef` name, or is destroyed
		// [orig: Game_StartMission @0x525DE7 -> the init walk @0x4432A0].
		step("powerup_bind");
		w::powerup_bind_entities(world, *items_table());
	}
	// The vehicle spawn-marker list is built from the mission as loaded, once
	// the definitions are attached and ahead of the class inits, the
	// PreMission pass and the WAC's initial execution.
	// [orig: Game_StartMission — the per-vehicle sub_529A80 walk
	//  @0x52527A..0x5252BF and the Spawn_BuildMarkerBudgetList call
	//  @0x5252C6 precede the Entity_InitAllFromModels call @0x52567F and the
	//  EventTrigger_UpdateAllWithFlag2 call @0x525B86]
	world.vehicles.build_spawn_markers();
	// Definition callbacks finish before the pre-mission event pass. In
	// particular, NPCs need their own ADM, ammunition and collision bindings.
	// [orig: Entity_SpawnFromBMSRecord @0x40E9F0 -> Entity_InitOrganicAI @0x4BFCC0]
	step("organic_init");
	world.registry.for_each_in_pool(0, [&](const w::Entity &row) {
		w::initialize_organic_ai(world, *world.registry.get(row.handle));
	});
	// Only authority runs the PreMission whole-list pass. A joiner can
	// carry a full BMS in a tool session without replaying its actions.
	// [orig: Game_StartMission @0x525b86, g_NapiNPCtx.is_authority gate]
	if (!options.joiner) {
		step("premission");
		world.run_logic_tick(/*is_authority=*/true, w::TickPhase::PreMission);
		// Then the SP score block zeroes whole, so a PreMission SubGoalWon
		// keeps its mask bit but not its tally; the WAC init and the census
		// write after it.
		// [orig: Game_StartMission — the Server_ResetRoundCounters call
		//  @0x525B90 inside the same authority gate; its memset(0xC84688, 0,
		//  0x84) @0x516C5E]
		world.kill_stats = w::MissionKillStats{};
	}
	// Every peer then zeroes the frame tick, and each frame advances it ahead
	// of its entity update, so the first frame runs at tick 1 on the host and
	// on every client alike [orig: Game_StartMission `mov tick, ebx` (ebx = 0)
	// @0x525B9F, past the is_authority-gated pre pass @0x525B78..0x525B90;
	// Game_ProcessMainFrame `add tick, ebx` @0x5265B4 ahead of the
	// Entity_UpdateAllEntities call @0x52674B]. World::run_logic_tick advances its
	// clock after the tick, so the same first frame starts from 1 here; a
	// joiner, which runs no pre pass, otherwise ran every even/odd cadence one
	// tick out of phase.
	world.logic_tick = 1;
	// The Attack & Defend side latch (world/local_player.h), taken here where
	// the load still knows its game type; retail's call sits after the initial
	// WAC execution and the weather settle (complete_mission_start's legs)
	// [orig: Game_StartMission -> sub_524110 @0x5260C1].
	local.latch_attack_defend_role(options.game_type);
	mission_start_pending = true;
	if (!options.defer_mission_start) complete_mission_start();
	return true;
}

// --- the cross-mission carry --------------------------------------------------

void MissionKernel::carry_across_load_from(MissionKernel &previous) {
	seat_specs = std::move(previous.seat_specs);
	mounted_graphics = std::move(previous.mounted_graphics);
	local.look_settings = previous.local.look_settings;
	local.carry_process_globals_from(previous.local);
	world.script.vars.carry_declared_from(previous.world.script.vars);
	// [orig: g_EntityUpdateCounter, whose one writer is
	// Entity_UpdateAllEntities @0x4C2639]
	world.entity_update_counter = previous.world.entity_update_counter;
	// [orig: dword_26970F4, whose only live writer is the cine render pass
	//  sub_570BB0 @0x570C66]
	world.epilog.frame_drawn = previous.world.epilog.frame_drawn;
	// [orig: dword_2C05A14 (the mode) and dword_2C05A18..20 (the camera),
	// zero-initialized data whose only writer is Render_WeatherTrailParticles
	// @0x5DEEB4..0x5DEED8]
	precipitation_draw = previous.precipitation_draw;
}

// --- the tick ---------------------------------------------------------------

void MissionKernel::tick_weather() {
	w::WeatherTickEvents events;
	world.weather.tick_sim(&world, events);
	w::WeatherSoundEvent thunder[2];
	const size_t thunders = w::weather_thunder_sounds(events, thunder);
	for (size_t i = 0; i < thunders; ++i) world.out.weather_sounds.push_back(thunder[i]);
	// A host without an audio presenter (the dedicated host) never drains
	// the queue: keep it bounded, dropping the oldest.
	constexpr size_t kWeatherSoundQueueCap = 32;
	while (world.out.weather_sounds.size() > kWeatherSoundQueueCap) {
		world.out.weather_sounds.erase(world.out.weather_sounds.begin());
	}
	// The quake HARD-SETS the shake counter [orig: @ 0x57eb7d / @ 0x57ec29].
	if (events.quake_shake_local) local.view.shake.counter = w::kShakeQuakeLevel;
	if (weather_render != nullptr) weather_render->weather_render_tick(world.weather);
}

bool MissionKernel::complete_mission_start() {
	if (!mission_start_pending) return false;
	// PreMission actions share their numbered variables for the whole pass,
	// then WAC initialization clears V0..V255 before its first execution.
	// This reset also runs without script files; declared V256+ and globals
	// retain their values. Keep it behind the once-per-load boundary guard.
	// [orig: Game_StartMission @0x525B86 (the EventTrigger_UpdateAllWithFlag2
	// call, authority-gated @0x525B78) -> Game_StartMission @0x525CB3 (the
	// WacScript_InitAndLoad call); WacScript_InitAndLoad @0x4F95EE (V0..V255
	// memset), @0x4F976B (initial execute), @0x4F9770 (++g_WacVarTicks)]
	world.script.vars.clear_numbered_mission_vars();
	// The environment has been seeded before this boundary. Initial WAC can
	// change its targets and entity poses before the 255-tick settle and the
	// first vehicle callback captures the respawn pose.
	// [orig: Game_StartMission @0x525CB8..0x526095]
	if (world.rules.projectile_authority) wac.execute_initial(world);
	world.weather.settle_mission_start([this] { tick_weather(); });
	w::count_mission_units(world);
	// The mission start's cine legs, after the unit census: every node gone,
	// the end screen down, and on a first SP start the intro-cine leg
	// [orig: Game_StartMission — Score_CountMissionSubgoalsAndUnits @0x525D5D,
	//  then sub_577940 @0x525DA8 and the intro arm @0x525DAF..0x525DD6].
	world.epilog.mission_start(!restart_boot_, world.rules.mp_session, world);
	if (world.rules.projectile_authority)
		world.vehicles.initialize_mission_vehicles();
	capture_baseline();
	mission_start_pending = false;
	return true;
}

namespace {

struct PrecipitationFloorContext {
	MissionKernel *kernel = nullptr;
};

int32_t precipitation_terrain_height(void *ctx, int32_t x, int32_t y) {
	MissionKernel *kernel = static_cast<PrecipitationFloorContext *>(ctx)->kernel;
	if (!kernel->has_terrain()) return 0;
	// [orig: Terrain_SampleHeightBilinear @ 0x6067b0] over the mission frame.
	const float h = kernel->ground_height(static_cast<float>(x) / 65536.0f,
			static_cast<float>(y) / 65536.0f);
	return static_cast<int32_t>(h * 65536.0f);
}

bool precipitation_entity_hit(void *ctx, int32_t x, int32_t y, int32_t z_top,
		int32_t z_bottom, int32_t &hit_z) {
	MissionKernel *kernel = static_cast<PrecipitationFloorContext *>(ctx)->kernel;
	if (kernel->world.collision == nullptr) return false;
	// The ray from floor + 200 m down to the floor through the local player's
	// candidate slice [orig: Physics_RaycastIntContext @ 0x5385e0 +
	// Physics_RaycastProximityEntities @ 0x538350 — the end clips to the first hit].
	const int32_t start[3] = {x, y, z_top};
	int32_t end[3] = {x, y, z_bottom};
	const w::CollisionWorld::RayDebugScope ray_scope(
			kernel->world.collision,
			w::CollisionWorld::RayDebugCategory::kPrecipitation);
	const w::EntityHandle hit = kernel->world.collision->clip_segment_to_nearest_collision(
			kernel->world, kernel->world.cached.local_player, start, end);
	if (!hit.valid()) return false;
	hit_z = end[2];
	return true;
}

} // namespace

void MissionKernel::update_precipitation(int32_t cam_x, int32_t cam_y, int32_t cam_z) {
	PrecipitationFloorContext ctx;
	ctx.kernel = this;
	env::PrecipitationFloorSampler sampler;
	sampler.terrain_height = &precipitation_terrain_height;
	sampler.entity_hit = &precipitation_entity_hit;
	sampler.ctx = &ctx;
	world.weather.precipitation.update(cam_x, cam_y, cam_z,
			world.weather.core.scalar_channels.rain_pct_fp, world.env.water_z, sampler);
}

// The teardown destroys pools 0, 1 and 2 before the authority's PostMission
// sweep, so the sweep's net-id lookups and pool walks find no row there; the
// pool-3 markers stay resident. Only the teardown's sweep is live: the SP
// restart's call runs after its own mission reset freed the event list, so it
// sweeps nothing and has no port.
// [orig: Game_TeardownMission — Entity_Destroy over pools 0, 1 and 2
//  @0x522365..0x5223C8, then the is_authority-gated
//  EventTrigger_UpdateAllWithFlag4 call @0x522663..0x52266C;
//  Game_RestartRoundSP @0x5263A0..0x5263AE, whose first call
//  Game_DestroyAllEntitiesAndReset @0x523604 enters
//  Mission_ResetBmsState (the mission reset), whose
//  EventSystem_FreeAll call @0x40DBEF zeroes the count @0x453266]
void MissionKernel::run_post_mission_pass(bool is_authority) {
	for (int pool = 0; pool <= 2; ++pool) {
		std::vector<w::EntityHandle> rows;
		world.registry.for_each_in_pool(pool, [&](const w::Entity &row) {
			rows.push_back(row.handle);
		});
		for (const w::EntityHandle row : rows) world.registry.despawn(row);
	}
	if (is_authority) events.run_post_mission_pass(world);
}

bool MissionKernel::restore_baseline() {
	if (!have_baseline) return false;
	const bool usegun_was_active = local.weapon.usegun_slot_active;
	const bool usegun_was_pending = local.weapon.usegun_switch != w::LocalUseGunSwitch::kNone;
	const uint8_t saved_personal_adm = local.weapon.usegun_saved_adm;
	local.weapon.events.clear();
	local.weapon.power_throw_start_tick = 0;
	local.weapon.pending_throw_charge = 0;
	local.weapon.fire_held = false;
	local.weapon.fire_pressed = false;
	local.weapon.reload_pressed = false;
	local.weapon.usegun_switch = w::LocalUseGunSwitch::kNone;
	local.weapon.usegun_slot_active = false;
	local.weapon.usegun_mount = w::EntityHandle{};
	local.weapon.usegun_weapon_adm = 0xFF;
	local.weapon.usegun_pending_mount = w::EntityHandle{};
	local.weapon.usegun_pending_weapon_adm = 0xFF;
	local.weapon.usegun_saved_adm = 0xFF;
	local.weapon.usegun_switch_action = -1;
	local.weapon.switch_deferred_action = -1;
	world.restore(baseline); // rewinds registry/vars/env/clock + re-inits systems (incl.
	                         // AI; WacSystem::on_load also resets its 62-tick accumulator)
	if (have_wac_baseline) wac.restore_runtime_state(world, wac_baseline);
	// Re-ground every soldier from scratch: the restored registry may reuse
	// handles across epochs, so the high-water mark cannot be trusted.
	reset_infantry_adm_ids();
	if (usegun_was_active) {
		// The world snapshot restores the play-start entity set, while the
		// embedder still presents the borrowed emplacement definition.
		// Reinstall the saved personal selection as a fresh restart epoch.
		if (w::Entity *player_row = world.registry.get(world.cached.local_player))
			player_row->equipped_adm_index = saved_personal_adm;
		local.weapon.active = false;
		const w::WeaponTableEntry *saved_def = world.tables.weapons.by_index(saved_personal_adm);
		local.weapon.start_in_switchto = saved_def != nullptr;
		w::WeaponPresentationEvent event;
		event.tick = world.logic_tick;
		if (const w::Entity *local = world.registry.get(world.cached.local_player))
			event.world_position = local->position;
		event.switch_to_weapon = saved_def != nullptr ? saved_def->name : std::string();
		event.clear_weapon = saved_def == nullptr;
		local.weapon.events.push_back(std::move(event));
	} else if (usegun_was_pending) {
		// The presenter never left the personal local.weapon, but its outgoing slot
		// may already be inside SWITCHFROM/RANK. Cancel only that action state
		// while retaining the personal magazine and reserve, and the slot's scope
		// zoom: MountSlot+0xC belongs to the slot, not to its action state
		// [orig: seeded once per slot by WeaponSlot_InitFromDef @0x53EF35].
		const int32_t clip = local.weapon.slot.clip;
		const int32_t reserve = local.weapon.slot.reserve;
		const int32_t zoom = local.weapon.slot.scope_zoom;
		local.weapon.slot = w::WeaponSlotState{};
		local.weapon.slot.clip = clip;
		local.weapon.slot.reserve = reserve;
		local.weapon.slot.scope_zoom = zoom;
	}
	// Retail's SP restart re-runs Game_StartMission [orig: Game_RestartRoundSP
	// @0x5263DB -> Game_StartMission @0x524360]. There the player re-init's weapon
	// switch resets the FOV target to 80 [orig: Game_StartMission's
	// Player_InitPlayer call @0x525BBC -> Player_InitPlayer's
	// Player_SwitchToWeaponByHandle call @0x4E19A1 ->
	// Player_ResetCameraAndMovementState @0x4DE202] BEFORE the
	// Environment_SnapStateToTargets call @0x525CAE re-seeds it from
	// the .env default (@0x57D2BB) and the WacScript_InitAndLoad call @0x525CB3 re-applies
	// the script's fov (WacCmd_Fov @0x4EDEA7), so the post-restart target is the
	// authored value. The sealed baseline already holds that post-init target:
	// restoring it after the reset stands in for the re-run (the port restores
	// the sealed world instead of re-executing the script's initial pass).
	const int32_t baseline_fov = world.weather.core.scalar_channels.camera_fov_target_fp;
	// The FP channel position is a gated advance count, not a clock delta, so
	// the restored world keeps the held clip pose with no epoch re-stamp.
	w::local_player_view_reset(&world, local.weapon, local.view, local.view_tracker);
	world.weather.core.scalar_channels.camera_fov_target_fp = baseline_fov;
	// The baseline predates the embedder's items.def traits. Re-stamp those
	// authoritative callback/health traits now, before any client view is
	// rebuilt from the restored rows: the encoder and the client classifier
	// must agree on every 0x0A record width.
	resweep_item_traits();
	return true;
}

// --- the local player -------------------------------------------------------





bool MissionKernel::install_weapon(const std::string &weapon_name, bool preserve_slot_state,
		bool allow_same_weapon_rebake) {
	if (!weapon_defs_ok || weapon_name.empty()) return false;
	// The entry the weapon table holds for the name: a name's last block
	// (def_weapon_index_by_name), the one whose descriptors the table baked.
	const int index = def_weapon_index_by_name(weapon_defs.entries, weapon_defs.count, weapon_name.c_str());
	if (index < 0) return false;
	const DefWeaponDef *row = &weapon_defs.entries[index];
	// The mount runs the descriptors the weapon table baked as it loaded, and
	// its channel plays from the table's shared ANIMADM rings. A same-weapon
	// re-bake (the dev tools' live ACTION edits) bakes the retained row as
	// edited instead. [orig: Player_MountWeaponSlot @ 0x4dfa40; Anim_InitActions
	// @ 0x541fa0 at the def's END]
	w::WeaponInstallData data = w::weapon_install_data_from_def(*row);
	data.table_baked = !allow_same_weapon_rebake;
	w::local_weapon_install(world, local.weapon, data, preserve_slot_state,
			allow_same_weapon_rebake, local.inventory_valid ? &local.inventory : nullptr, local.view);
	return true;
}

void MissionKernel::keep_weapon_action_edit(int action_id) {
	if (action_id < 0 || action_id >= w::weapon_action::kCount) return;
	const int index = world.tables.weapons.index_of(local.weapon.def_name.c_str());
	if (index < 0) return;
	world.tables.weapons.entries[static_cast<size_t>(index)].action_fsm.actions[action_id] =
			local.weapon.def.actions[action_id];
}



// --- terrain ----------------------------------------------------------------

float MissionKernel::ground_height(float mission_x, float mission_y) const {
	// The field's world frame is the renderer's: x, and z = -mission y.
	return terrain::height_field_height_world_bilinear(terrain_store.height_field(),
			mission_x, -mission_y);
}

// --- observation ------------------------------------------------------------

std::vector<w::Effect> MissionKernel::drain_effects() {
	std::vector<w::Effect> out = world.out.effects.entries();
	world.out.effects.clear();
	return out;
}

std::vector<w::CollisionWorld::DebugInstance> MissionKernel::collision_instances(
		const w::Vec3 &anchor, float range_units, int32_t max_instances) {
	const int32_t a[3] = {w::to_fixed(anchor.x), w::to_fixed(anchor.y), w::to_fixed(anchor.z)};
	const int32_t range = range_units < 0.0f ? -1 : w::to_fixed(range_units);
	return collision.debug_instances(world, a, range, max_instances);
}

std::vector<w::CollisionWorld::DebugHitboxEntity> MissionKernel::hitboxes(
		const w::Vec3 &anchor, float range_units, int32_t max_entities, int32_t max_faces) {
	const int32_t a[3] = {w::to_fixed(anchor.x), w::to_fixed(anchor.y), w::to_fixed(anchor.z)};
	const int32_t range = range_units < 0.0f ? -1 : w::to_fixed(range_units);
	return collision.debug_hitboxes(world, a, range, max_entities, max_faces);
}

// --- world::IPoseProvider: seats ---------------------------------------------

bool MissionKernel::resolve_mounted_pose(w::World &p_world, const w::Entity &carrier,
		const w::Seat &seat, w::MountedPose &out) {
	// The kernel IS the world's IPoseProvider: the one mounted-pose
	// resolver every embedder's world reaches.
	if (&p_world != &world || seat.type != w::SeatType::Gunner || seat.bone_index == 0) return false;
	++mounted_queries;
	assets::Model model_asset;
	const auto graphic = mounted_graphics.find(carrier.item_id);
	if (graphic != mounted_graphics.end()) model_asset = assets().model(graphic->second);
	if (!model_asset) {
		++mounted_declines;
		return false;
	}
	const Threedi3di3 &model = *model_asset;
	if (model.ctrl.count > 0 && model.ctrl.registers == nullptr) {
		++mounted_declines;
		return false;
	}
	world::MountedPoseControlSources sources;
	if (const w::AiEntity *carrier_ai = world.ai.for_handle(carrier.handle)) {
		sources.part_anim_phase0 = carrier_ai->brain.f[w::AiBrain::kPartAnimPhase0];
		sources.part_anim_phase1 = carrier_ai->brain.f[w::AiBrain::kPartAnimPhase0 + 1];
	}
	// An addeweap attachment frame is posed through Bone_BuildAttachmentMatrix,
	// which runs the carrier's own render-class CTRL callback first (the ewep
	// writer only for an 'ewep' class; another class leaves the words its
	// UseGun rider's seat call wrote). A UseGun rider's attachment never calls
	// that callback: it calls the ewep writer directly on the carrier, whatever
	// its class. Registers neither call writes read the global CTRL bus as the
	// carrier's last class publication left it, which the vehicle projection
	// below stands for.
	// [orig: Bone_BuildAttachmentMatrix def+0x144 @0x56C6DC..0x56C6F3, reached
	//  from Entity_UpdateTransformAndTurret @0x44109D;
	//  Entity_AttachToBoneAndUpdateTransform @0x546517..0x546518]
	w::EmplacedWeaponControls emplaced;
	if (seat.attachment_frame) {
		sources.has_heat_glow = w::world_model_heat_glow_for(world, carrier, sources.heat_glow);
		w::emplaced_weapon_controls_for(world, carrier, emplaced);
	} else {
		sources.has_heat_glow = true;
		sources.heat_glow = w::emplaced_slot_heat_glow(world, carrier);
		emplaced = w::emplaced_weapon_controls_of(carrier);
	}
	if (emplaced.valid) {
		sources.has_emplaced = true;
		sources.emplaced_gun_yaw = emplaced.gun_yaw;
		sources.emplaced_gun_pitch = emplaced.gun_pitch;
		sources.emplaced_spin_phase = emplaced.spin;
	}
	int32_t ctrl_bus[THREEDI_CTRL_REGISTER_COUNT] = {};
	world::compose_mounted_pose_controls(carrier.item_attrib, sources, ctrl_bus);
	world::compose_vehicle_pose_controls(world, carrier, ctrl_bus);
	std::array<int32_t, THREEDI_CTRL_REGISTER_COUNT> controls{};
	std::copy(std::begin(ctrl_bus), std::end(ctrl_bus), controls.begin());
	const uint32_t time_ms = world::mounted_pose_time_ms(world.logic_tick,
			panm_time_override_ms);
	if (mounted_cache_tick_ != world.logic_tick) {
		mounted_live_cache_.clear();
		mounted_cache_tick_ = world.logic_tick;
	}
	auto rest = mounted_rest_cache_.find(model_asset);
	if (rest == mounted_rest_cache_.end()) {
		MountedPoseRest r;
		if (!world::evaluate_model_mounted_pose_parts(model, 0u, nullptr, r.parts)) {
			++mounted_declines;
			return false;
		}
		rest = mounted_rest_cache_.emplace(model_asset, std::move(r)).first;
	}
	std::vector<MountedPoseLive> &live_list = mounted_live_cache_[model_asset];
	auto live = std::find_if(live_list.begin(), live_list.end(), [&](const MountedPoseLive &c) {
		return c.time_ms == time_ms && c.controls == controls;
	});
	if (live == live_list.end()) {
		MountedPoseLive l;
		l.time_ms = time_ms;
		l.controls = controls;
		l.valid = world::evaluate_model_mounted_pose_parts(model, time_ms, controls.data(), l.parts);
		live_list.push_back(std::move(l));
		live = live_list.end() - 1;
		++mounted_evaluations;
	} else {
		++mounted_cache_hits;
	}
	const bool resolved = live->valid && world::resolve_model_mounted_pose_from_parts(
			model, carrier, seat, rest->second.parts, live->parts, out);
	if (!resolved) ++mounted_declines;
	return resolved;
}

// Without a mounted store there is no model data: the static seat stands.
bool MissionKernel::resolve_seat_bone(w::World &p_world, const w::Entity &carrier,
		int bone_index) {
	if (&p_world != &world || !assets().has_source()) return true;
	ensure_collision_instance(p_world, carrier.handle);
	return collision_pose.resolve_seat_bone(p_world, carrier, bone_index);
}

// --- world::IPoseProvider: muzzles / userpoints (the sim pose) ---------------

bool MissionKernel::resolve_skeletal_anchor(w::World &p_world, w::EntityHandle entity,
		w::SkeletalAnchor anchor, int32_t out[3]) {
	ensure_collision_instance(p_world, entity);
	return collision_pose.resolve_skeletal_anchor(p_world, entity, anchor, out);
}

bool MissionKernel::resolve_organic_attachment(w::World &p_world, w::EntityHandle entity,
		uint8_t userpoint, int32_t out[3]) {
	ensure_collision_instance(p_world, entity);
	return collision_pose.resolve_organic_attachment(p_world, entity, userpoint, out);
}

bool MissionKernel::resolve_muzzle_pose(w::World &p_world, w::EntityHandle entity,
		int32_t out[3]) {
	return collision_pose.resolve_muzzle_pose(p_world, entity, out);
}

bool MissionKernel::resolve_userpoint_transform(w::World &p_world, w::EntityHandle entity,
		int userpoint_index, int32_t out[6]) {
	return collision_pose.resolve_userpoint_transform(p_world, entity, userpoint_index, out);
}

bool MissionKernel::resolve_userpoint_frame(w::World &p_world, w::EntityHandle entity,
		const Threedi3di3 *model, int userpoint_index, int32_t out[6],
		int32_t out_direction[3]) {
	return collision_pose.resolve_userpoint_frame(p_world, entity, model, userpoint_index, out,
			out_direction);
}

bool MissionKernel::resolve_userpoint_rigid(w::World &p_world, w::EntityHandle entity,
		int userpoint_index, int32_t out[3]) {
	return collision_pose.resolve_userpoint_rigid(p_world, entity, userpoint_index, out);
}

bool MissionKernel::resolve_named_transform(w::World &p_world, w::EntityHandle entity,
		const char *name, int32_t out[6]) {
	return collision_pose.resolve_named_transform(p_world, entity, name, out);
}

int MissionKernel::last_named_userpoint(w::World &p_world, w::EntityHandle entity,
		const char *name) {
	return collision_pose.last_named_userpoint(p_world, entity, name);
}

bool MissionKernel::resolve_userpoint_pivot(w::World &p_world, w::EntityHandle entity,
		int userpoint_index, int32_t out[3]) {
	return collision_pose.resolve_userpoint_pivot(p_world, entity, userpoint_index, out);
}

bool MissionKernel::resolve_section_pivot(w::World &p_world, w::EntityHandle entity,
		int part, int32_t out[3]) {
	return collision_pose.resolve_section_pivot(p_world, entity, part, out);
}

// --- world::IPoseProvider: collision sections --------------------------------

bool MissionKernel::ensure_collision_instance(w::World &p_world, w::EntityHandle entity) {
	if (&p_world != &world || items_table() == nullptr) return false;
	const w::Entity *e = world.registry.get(entity);
	if (e == nullptr) {
		collision.remove_entity_instance(entity);
		collision_pose.remove_entity(entity);
		collision_state.resolution_attempted.erase(entity.packed);
		return false;
	}
	const auto attempted = collision_state.resolution_attempted.find(entity.packed);
	if (attempted != collision_state.resolution_attempted.end()) {
		if (attempted->second == e->registry_spawn_id) return collision.has_instance(world, entity);
		collision.remove_entity_instance(entity);
		collision_pose.remove_entity(entity);
		collision_state.resolution_attempted.erase(attempted);
	}
	// The idempotent attach sweep over the retained caches: every entity that
	// appeared since the previous sweep (a player deployed after load).
	wire_collision();
	const mission::CollisionResolveDeps deps{collision, occlusion, collision_pose, assets()};
	collision_attached = mission::resolve_collision_instances(world, *items_table(),
			collision_state, deps);
	return collision.has_instance(world, entity);
}

bool MissionKernel::build_section_matrices(w::World &p_world, w::EntityHandle entity,
		int32_t model_id, const w::CollisionMatrix &entity_world, const w::CollisionModel &model,
		std::vector<w::CollisionMatrix> &out) {
	collision_pose.weapon_active = local.weapon.active;
	collision_pose.panm_time_override_ms = panm_time_override_ms;
	++collision_queries;
	if (collision_pose.build_section_matrices(p_world, entity, model_id, entity_world, model, out))
		return true;
	if (collision_pose.has_skeletal_entity(entity) || collision_pose.has_generic_model(model_id))
		++collision_declines;
	return false;
}

} // namespace opennova::mission
