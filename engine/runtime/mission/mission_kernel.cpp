// mission::MissionKernel — the promoted retail-mission rig body (ADR 0042 d3).
// Every leg here is the one engine implementation the embedders share; the
// witness citations moved with the bodies.

#include <runtime/mission/mission_kernel.h>

#include <base/io/bam.h>
#include <base/io/log.h>
#include <base/io/strutil.h>
#include <base/vfs/vfs.h>
#include <formats/mission/mission.h>
#include <runtime/mission/mission_systems.h>
#include <runtime/simassets/item_traits.h>
#include <runtime/simassets/seat_spec_extract.h>
#include <runtime/terrain_query/height_field.h>
#include <runtime/wac/wac_layered_load.h>
#include <runtime/world/ammo_table_build.h>
#include <runtime/world/angle.h>
#include <runtime/world/infantry.h>
#include <runtime/world/mount_controls.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/round_sim.h>
#include <runtime/world/spawn_select.h>
#include <runtime/world/vehicle_attach.h>
#include <runtime/world/weapon_fsm.h>
#include <runtime/world/weapon_table_build.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace opennova::mission {

namespace w = opennova::world;

namespace {

std::string basename_of(const std::string &name) {
	const size_t dot = name.rfind('.');
	return dot == std::string::npos ? name : name.substr(0, dot);
}

int32_t bam_from_radians(double radians) {
	return static_cast<int32_t>(
			static_cast<int64_t>(std::llround(radians * io::kBamPerRadian)));
}

} // namespace

MissionKernel::MissionKernel() {
	// The kernel pumps the local player's slot itself (run_local_player_post_tick
	// with the live trigger/reload/scope inputs), so the world's global weapon
	// pump must skip L's borrowed UseGun parent slot or one slot advances twice
	// per frame [orig: one WeaponAction_ProcessAllEntities walk @0x542690].
	world.external_local_mounted_weapon_pump = true;
}

MissionKernel::~MissionKernel() {
	// The systems and providers the world points at outlive nothing: drop the
	// non-owning links before the members tear down in reverse order.
	world.collision = nullptr;
	world.mounted_pose_provider = nullptr;
	world.muzzle_pose_provider = nullptr;
	world.terrain = nullptr;
	world.ai = nullptr;
	ai.collision = nullptr;
	ai.terrain = nullptr;
	ai.root_motion = nullptr;
	collision.set_section_matrix_provider(nullptr);
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
	own_mounted_ = true;
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
	BootFileSource files;
	files.has_file = [this](const std::string &file) { return index.has_file(file); };
	files.read_file = [this](const std::string &file, std::vector<uint8_t> &out) {
		return index.read_file(file, out);
	};
	open_document(std::move(parsed), basename_of(name), std::move(files));
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

void MissionKernel::set_asset_index(const ResourceIndex *asset_index_ptr) {
	external_index_ = asset_index_ptr;
	// A source switch invalidates every retained parse-derived pose, exactly
	// like the shell's asset-root switch did.
	mounted_rest_cache_.clear();
	mounted_live_cache_.clear();
	mounted_cache_tick_ = 0xFFFFFFFFu;
	models.set_index(asset_index());
	collision_pose.set_resource_index(asset_index());
}

void MissionKernel::set_items_table(const DefItemsFile *items_table_ptr) {
	items_override_ = items_table_ptr;
}

void MissionKernel::sync_water_plane() {
	terrain_store.set_water_plane(world.env.water_z);
}

void MissionKernel::wire_terrain() {
	sync_water_plane();
	world.terrain = terrain_store.valid() ? &terrain_store.height_field() : nullptr;
	ai.terrain = terrain_store.valid() ? &terrain_store.height_field() : nullptr;
	ai.ground_clearance = w::GroundClearance{};
	collision.terrain = terrain_store.valid() ? &terrain_store.height_field() : nullptr;
	// The footstep surface pick reads the charmap through this view; the
	// shell's apply_terrain_to_ai calls this and then re-layers its
	// device-fed extras (placed tiles, sound profiles).
	world.surface_map = terrain_store.surface_map();
}

void MissionKernel::wire_collision() {
	collision.terrain = terrain_store.valid() ? &terrain_store.height_field() : nullptr;
	collision.set_section_matrix_provider(this);
	world.collision = &collision;
	world.mounted_pose_provider = this;
	world.muzzle_pose_provider = &collision_pose;
	ai.collision = &collision;
}

std::function<PromoteOptions::AiProfileDefaults(int32_t)>
MissionKernel::ai_profile_defaults_fn() const {
	if (items_table() == nullptr) return {};
	return [this](int32_t type_id) {
		PromoteOptions::AiProfileDefaults d;
		const int item_id = static_cast<int>(type_id) + static_cast<int>(kItemIdOffset);
		const DefItemDef *def = simassets::find_item_def(*items_table(), item_id);
		if (def == nullptr) return d;
		const std::string cls = strutil::to_lower(def->ai_function);
		d.helicopter_init = cls == "chel" || cls == "cpln";
		d.known = d.helicopter_init || cls == "cveh" || cls == "cbot" || cls == "ctrn";
		d.default_aip = def->default_aip;
		return d;
	};
}

bool MissionKernel::load_mission_into_world() {
	PromoteOptions opts;
	opts.item_seat_specs = seat_specs;
	opts.ai_profiles = ai_profiles;
	opts.ai_profile_defaults = ai_profile_defaults_fn();
	// Authored display names from the embedder's [PeopleNames] table (D-HUD-20);
	// promote applies the retail 15-char copy at its cited port site.
	opts.people_name_resolver = people_name_resolver_;
	promo = promote_mission(mission, world, ai, opts);
	finish_load();
	return true;
}

void MissionKernel::finish_load() {
	events.load(mission.events, mission.triggers, mission.actions);
	world.mission_attrib_flags = static_cast<uint32_t>(mission.header.attrib_flags);
	world.ai = &ai;
	// The net half stands its session up here — between the world wiring and
	// register_mission_systems, exactly where the SP listen host's bring-up
	// sits inside the load (inmatch::listen_host::bringup)
	// [orig: SinglePlayer_StartMission @0x561af0].
	if (bringup_net_session_) bringup_net_session_();
	register_mission_systems(world, wac, events, ai);
	// PreMission events settle initial scripted state before the clock starts.
	world.run_logic_tick(/*is_authority=*/true, w::TickPhase::PreMission);
	w::count_mission_units(world);
	capture_baseline();
}

void MissionKernel::capture_baseline() {
	baseline = world.snapshot();
	ai.capture_spawn_baseline();
	wac_baseline = wac.capture_runtime_state();
	have_baseline = true;
	have_wac_baseline = true;
}

int MissionKernel::spawn_local_player_at_start(uint32_t game_type) {
	if (has_local_player()) return 1;
	const w::SpawnPointResult sel = w::resolve_player_spawn_pose(
			world, w::EntityHandle{}, w::EntityHandle{}, 0, 1, game_type);
	w::PlayerSpawn spawn;
	if (sel.found) {
		spawn.position = sel.position;
		spawn.yaw = sel.yaw;
		spawn.pitch = sel.pitch;
		spawn.roll = sel.roll;
	}
	spawn.team = 1;
	if (!spawn_local_player(spawn)) return -1;
	return sel.found ? 1 : 0;
}

bool MissionKernel::spawn_local_player(const w::PlayerSpawn &spawn) {
	const w::EntityHandle h = w::spawn_player(world, spawn);
	if (!h.valid()) return false;
	resolve_new_infantry_adm_ids();
	// Seed the look heading from the spawn facing so the body starts aligned.
	reset_local_player_input(w::bam_heading_from_mission_yaw_deg(spawn.yaw));
	w::local_player_view_reset(&world, weapon, view, view_tracker);
	return true;
}

void MissionKernel::resolve_new_infantry_adm_ids() {
	const DefItemsFile *item_rows = items_table();
	if (!infantry_adm_retained_ || item_rows == nullptr || root_motion.empty()) return;
	const ResourceIndex *adm_source = adm_index_ != nullptr ? adm_index_ : asset_index();
	const int count = ai.count();
	if (infantry_adm_resolved_ai_count_ < 0 || infantry_adm_resolved_ai_count_ > count)
		infantry_adm_resolved_ai_count_ = 0;
	for (int i = infantry_adm_resolved_ai_count_; i < count; ++i) {
		w::AiEntity *e = ai.at(i);
		if (e == nullptr) continue;
		e->inf.adm_id = 0;
		if (!e->inf.active) continue;
		const w::Entity *ent = world.registry.get(e->handle);
		if (ent == nullptr) continue;
		const int visual = simassets::visual_item_id_for_runtime_type(ent->item_id, *item_rows);
		const DefItemDef *def = simassets::find_item_def(*item_rows, visual);
		if (def == nullptr || def->anim_def[0] == '\0') continue;
		std::string adm = def->anim_def;
		if (!strutil::ends_with_icase(adm, ".adm")) adm += ".adm";
		const int adm_id = root_motion.register_adm(adm_source, adm);
		if (adm_id >= 0) e->inf.adm_id = adm_id;
	}
	infantry_adm_resolved_ai_count_ = count;
}

void MissionKernel::rearm_infantry_adm(const ResourceIndex *adm_index) {
	if (adm_index != nullptr) adm_index_ = adm_index;
	infantry_adm_retained_ = true;
	infantry_adm_resolved_ai_count_ = 0;
	for (int i = 0; i < ai.count(); ++i)
		if (w::AiEntity *e = ai.at(i)) e->inf.adm_id = 0;
	resolve_new_infantry_adm_ids();
}

int MissionKernel::install_infantry_anim(const std::string &adm_name,
		const ResourceIndex *adm_index) {
	adm_index_ = adm_index != nullptr ? adm_index : asset_index();
	root_motion.clear();
	infantry_adm_resolved_ai_count_ = 0;
	const int default_adm = root_motion.register_adm(adm_index_, adm_name);
	if (default_adm != 0)
		io::logf(io::LogLevel::kWarn,
				"mission kernel: no infantry clips from '%s' - AI soldiers will stand still",
				adm_name.c_str());
	// A source with no clips counts as none: the selector then resolves every
	// state to "no clip" and soldiers stand, exactly the original's
	// relationship between motion and clips.
	ai.root_motion = root_motion.empty() ? nullptr : &root_motion;
	if (default_adm == 0) resolve_new_infantry_adm_ids();
	return default_adm == 0 ? root_motion.clip_count(0) : 0;
}

bool MissionKernel::load_weapon_table(const BootFileSource &files,
		const ResourceIndex *table_index, const std::string &name) {
	std::vector<uint8_t> bytes;
	if (!files.valid() || !files.read_file(name, bytes)) return false;
	DefWeaponsFile file = {};
	if (def_parse_weapons_memory(bytes.data(), bytes.size(), &file) != 0) return false;
	world.weapons = w::build_weapon_table(file,
			table_index != nullptr ? table_index : asset_index());
	if (weapon_defs_ok) def_free_weapons(&weapon_defs);
	weapon_defs = file;
	weapon_defs_ok = true;
	simassets::stamp_seat_spec_turret_limits(world, seat_specs);
	// The authoritative side's own player spawned before this feed: re-stamp
	// its equipped default now that WPN_M4AUTO resolves by name
	// [orig: PlayerClass_InitEntity @0x4B1116] (D-NET-143).
	const int m4 = world.weapons.index_of("WPN_M4AUTO");
	if (m4 >= 0) {
		std::vector<w::EntityHandle> handles;
		world.registry.for_each([&](const w::Entity &e) {
			if (e.item_id == w::kPlayerInfantryTypeId && e.equipped_adm_index == w::kAdmSlotNone)
				handles.push_back(e.handle);
		});
		for (const w::EntityHandle h : handles)
			if (w::Entity *e = world.registry.get(h)) e->equipped_adm_index = static_cast<uint8_t>(m4);
	}
	// The mission's stashed loadout/availability chunks promote NOW, through
	// the witnessed SP-vs-net gate [orig: Mission_LoadBMSFile @ 0x40F4E0 — gate
	// @ 0x40f694], then the local player's slot pool builds from the spawn kit
	// and selects the spawn default — the Player_InitPlayer weapon leg
	// [orig: @ 0x4e15f0; the default kit literal @ 0x5246be].
	std::vector<std::pair<std::string, int32_t>> availability_rows;
	std::vector<w::WeaponKitEntry> kit_rows;
	stash_mission_loadout_rules(mission, availability_rows, kit_rows);
	if (w::local_loadout_promote_mission_rules(world, loadout, availability_rows, std::move(kit_rows)))
		w::local_player_view_reset(&world, weapon, view, view_tracker);
	w::local_loadout_rebuild(world, loadout, weapon, inventory, inventory_valid,
			/*select_spawn_default=*/true);
	return true;
}

bool MissionKernel::load_ammo_table(const BootFileSource &files,
		const std::string &name) {
	std::vector<uint8_t> bytes;
	if (!files.valid() || !files.read_file(name, bytes)) return false;
	DefAmmoFile file = {};
	if (def_parse_ammo_memory(bytes.data(), bytes.size(), &file) != 0) return false;
	world.ammo = w::build_ammo_table(file);
	def_free_ammo(&file);
	w::resolve_weapon_round_types(world.weapons, world.ammo);
	w::local_loadout_sync_damage_classes(world, loadout);
	ammo_ok = true;
	return true;
}

bool MissionKernel::boot(const KernelBootOptions &options, std::string &error) {
	if (!opened_) {
		error = "open() / open_document() first";
		return false;
	}
	// The sim's own model source, wired before the seat step runs (S16).
	models.set_index(asset_index());
	collision_pose.set_resource_index(asset_index());
	bringup_net_session_ = options.bringup_net_session;
	people_name_resolver_ = options.people_name_resolver;

	BootParams params;
	params.is_joiner = options.joiner;
	params.playable = options.playable;
	params.has_resource_root = files_.valid();
	params.has_item_db = items_table() != nullptr;
	params.has_terrain = terrain_store.valid();
	params.has_terrain_til = false;
	params.has_wac = options.wac;

	BootSteps steps;
	steps.install_seat_specs = [&] {
		seat_specs.clear();
		mounted_graphics.clear();
		if (!options.seat_specs) return;
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
		if (!seeds.empty() && items_table() != nullptr) {
			simassets::SeatSpecExtraction native;
			simassets::extract_item_seat_specs(*items_table(),
					[this](const std::string &graphic) { return models.model_for(graphic); },
					seeds, native);
			seat_specs = std::move(native.specs);
			mounted_graphics = std::move(native.graphic_by_type);
		}
		std::sort(seat_specs.begin(), seat_specs.end(),
				[](const ItemSeatSpec &a, const ItemSeatSpec &b) { return a.type_id < b.type_id; });
		simassets::stamp_seat_spec_turret_limits(world, seat_specs);
	};
	steps.install_ai_profiles = [&] {
		ai_profiles = resolve_ai_profiles(files_, mission, ai_profile_defaults_fn());
	};
	steps.install_terrain_til = [] {};
	steps.install_mission_text = [&] {
		std::vector<uint8_t> text;
		text_source = resolve_mission_text(files_, mission_basename, text);
		text_size = text.size();
	};
	steps.load_mission = [&] { return load_mission_into_world(); };
	steps.install_terrain_field = [&] { wire_terrain(); };
	steps.install_sound_profiles = [] {}; // the footstep/foley slot table is presentation-side
	steps.install_infantry_anim = [&] {
		(void)install_infantry_anim(options.infantry_adm);
	};
	std::string wac_blocked_error; // strict mode's fatal diagnostic, if any
	steps.install_wac = [&] {
		wac_loaded = false;
		std::string wac_error;
		const wac::WacLayeredLoadStatus status = wac::wac_layered_load(wac, files_,
				options.wac_basename.empty() ? mission_basename : options.wac_basename,
				&world.registry, options.wac_strict_diagnostics, wac_error);
		if (status == wac::WacLayeredLoadStatus::kBlocked) {
			if (options.wac_strict_diagnostics) {
				wac_blocked_error = std::move(wac_error);
				return;
			}
			io::logf(io::LogLevel::kWarn, "mission kernel: %s - scripts disabled",
					wac_error.c_str());
			return;
		}
		wac_loaded = status == wac::WacLayeredLoadStatus::kLoaded;
	};
	steps.spawn_local_player = [&] {
		const int status = spawn_local_player_at_start(options.game_type);
		if (status < 0)
			io::logf(io::LogLevel::kWarn, "mission kernel: spawn_local_player failed");
		else if (status == 0)
			io::logf(io::LogLevel::kWarn,
					"mission kernel: no player-start marker - spawned at the origin");
	};
	steps.resolve_infantry_adm = [&] { rearm_infantry_adm(); };
	steps.resolve_item_traits = [&] {
		simassets::resolve_item_traits(world, *items_table(),
				[](int32_t) -> uint8_t { return 0; });
	};
	steps.install_asset_root = [] {};
	steps.resolve_collision = [&] {
		if (!options.collision) return;
		wire_collision();
		const simassets::CollisionResolveDeps deps{collision, occlusion, collision_pose, models};
		collision_attached = simassets::resolve_collision_instances(world,
				*items_table(), collision_state, deps);
	};
	steps.occlusion_init = [&] {
		if (!options.collision) return;
		collision.build_initial_tables(world);
		occlusion.init_mission(world, collision);
	};
	steps.load_weapon_table = [&] {
		if (!load_weapon_table(files_, nullptr))
			io::logf(io::LogLevel::kWarn, "mission kernel: weapon.def not loaded");
	};
	steps.load_ammo_table = [&] { return load_ammo_table(files_); };
	steps.resolve_ai_weapons = [&] {
		simassets::resolve_ai_weapons(world, *items_table());
	};

	const BootAbort abort = run_mission_boot(params, steps);
	if (abort != BootAbort::kNone) {
		error = "mission boot aborted (load failed)";
		return false;
	}
	if (!wac_blocked_error.empty()) {
		error = wac_blocked_error;
		return false;
	}
	// WacScript_InitAndLoad executes the freshly loaded bytecode once before
	// the world ticks.
	if (wac_loaded) wac.execute_initial(world);
	return true;
}

// --- the tick ---------------------------------------------------------------

bool MissionKernel::local_player_can_fire(const w::AiEntity *body) const {
	// The Player_CanFireWeapon verdict the body updater and the HUD share
	// [orig: @0x5cf7c7..0x5cf886; Scoped helper @0x4dcc80; Sighted helper
	// @0x4dcd30].
	const w::Entity *local = world.registry.get(world.cached.local_player);
	if (local == nullptr || body == nullptr || !weapon.active) return false;
	bool mount_allows = true;
	if (local->mounted)
		mount_allows = local->mount_type == w::SeatType::Passenger ||
				(local->mount_type == w::SeatType::Gunner && weapon.usegun_slot_active);
	if (!mount_allows || view.third_person || view.binoculars_view_active) return false;
	const w::WeaponSlotState *slot = w::active_local_weapon_slot(world, weapon);
	if (slot == nullptr) return false;
	const uint32_t flags = static_cast<uint32_t>(weapon.def.flags);
	if (slot->current == w::weapon_action::kReload && (flags & DEF_WEAPON_FLAG_NOCARDSWITCH) == 0)
		return false;
	const bool scope_promoted = body->inf.scope_raised;
	const bool scoped = scope_promoted && (flags & DEF_WEAPON_FLAG_SCOPED) != 0;
	const bool sighted = scope_promoted && (flags & DEF_WEAPON_FLAG_SIGHTED) != 0 &&
			slot->current != w::weapon_action::kSwitchFrom;
	const uint32_t entity_flags = local->flags | local->engine_flags;
	const bool in_air = body->inf.airborne || (entity_flags & w::kEntityFlagInAir) != 0;
	const bool submerged = (entity_flags & w::kEntityFlagDrowning) != 0 ||
			w::entity_eye_below_water(world, body->pos[2], local->eye_offset_z);
	// Dead (Flags & 0x2) and airborne (0x2000) share ONE can_fire=0 group
	// that the FORCESCOPED override reverses, so a dead body holding a
	// ForceScoped weapon still reads can_fire [orig: `Flags & 0x2002` @0x5cf7fb;
	// the override @0x5cf845].
	const bool dead = !local->alive || local->health <= 0;
	const bool ordinary = !dead && !in_air && (sighted || (scoped && !body->inf.player_moving)) &&
			(sighted || !submerged);
	return (flags & DEF_WEAPON_FLAG_FORCESCOPED) != 0 || ordinary;
}

bool MissionKernel::local_player_dead() const {
	if (!world.cached.local_player.valid()) return false;
	const w::Entity *e = world.registry.get(world.cached.local_player);
	return e != nullptr && ((e->flags | e->engine_flags) & w::kEntityFlagDead) != 0;
}

void MissionKernel::stamp_medic_request() {
	medic_request_cooldown_ticks = kMedicRequestCooldownTicks;
	++medic_request_serial;
}

void MissionKernel::tick_medic_cooldown(bool local_dead) {
	if (local_dead && !medic_dead_edge_seen_) medic_request_cooldown_ticks = 0;
	medic_dead_edge_seen_ = local_dead;
	if (medic_request_cooldown_ticks > 0) --medic_request_cooldown_ticks;
}

void MissionKernel::apply_player_input_pre_tick() {
	if (!world.cached.local_player.valid()) return;
	// The per-tick shake decay, ahead of the entity update's arms and the
	// weather tick's quake hard-set: retail decays in Player_UpdatePerFrame
	// from the client network frame that precedes both, once per quantum and
	// gated on the player entity alone [orig: @ 0x4DE590; Game_ProcessMainFrame
	// @ 0x52674b / @ 0x526774].
	w::camera_shake_decay(view.shake);
	w::AiEntity *p = ai.for_handle(world.cached.local_player);
	if (p == nullptr) return;
	w::local_player_view_refresh(&world, view);
	w::apply_player_body_input(*p, w::pack_player_body_input(input));
	const bool scope_promoted = weapon.active && view.scope_engaged &&
			!w::player_view_scope_ease_active(view);
	p->inf.aimed_shot_available = false;
	if (p->inf.active) {
		if (weapon.active) w::infantry_weapon_switch_stamp(p->inf, weapon.anim_map_serial);
		p->inf.scope_raised = scope_promoted;
		p->inf.binoculars_raised = view.binoculars_raised;
		p->inf.wpn_run_anim = weapon.active ? weapon.run_anim : 0;
		p->inf.wpn_force_crouch = weapon.active && weapon.force_crouch;
		p->inf.aimed_shot_available = local_player_can_fire(p);
	}
	if (w::Entity *entity = world.registry.get(world.cached.local_player)) {
		// The per-frame view-flag restamp onto the body's Flags word
		// [orig: the g_NVGActive / g_binocularsRaised / g_weaponScopeActive
		// refresh in Player_PackInputStateToEntity @0x4df450].
		uint32_t view_flags = 0;
		if (view.nvg_active) view_flags |= w::kEntityFlagNVGWorn;
		if (view.binoculars_raised) view_flags |= w::kEntityFlagBinoculars;
		if (scope_promoted) view_flags |= w::kEntityFlagScopeRaised;
		constexpr uint32_t kViewFlagMask =
				w::kEntityFlagNVGWorn | w::kEntityFlagBinoculars | w::kEntityFlagScopeRaised;
		entity->flags = (entity->flags & ~kViewFlagMask) | view_flags;
	}
}

void MissionKernel::sync_local_mounted_input_heading() {
	if (!world.cached.local_player.valid()) return;
	const w::Entity *player_entity = world.registry.get(world.cached.local_player);
	const w::AiEntity *body = ai.for_handle(world.cached.local_player);
	if (player_entity == nullptr || body == nullptr || !body->inf.is_local_player) return;
	// A post-tick difference from the pre-tick input copy is "the sim wrote
	// the view this tick" (the mount-attach yaw snap, the ladder legs).
	if (body->inf.target_heading != input.look_heading) input.look_heading = body->inf.target_heading;
	if (body->inf.look_pitch != input.look_pitch) input.look_pitch = body->inf.look_pitch;
}

void MissionKernel::run_local_player_post_tick() {
	// Retail promotes the per-frame view before weapon actions; the sim-wrote-
	// the-view fold runs first so the pumps read the settled look.
	sync_local_mounted_input_heading();
	w::local_player_view_tick(&world, weapon, view, view_tracker, view_session_inputs);
	w::LocalWeaponPumpIO io;
	io.view = &view;
	io.inventory = inventory_valid ? &inventory : nullptr;
	io.is_authority = true;
	w::local_weapon_pump_tick(world, weapon, io);
	// The wire-facing outcomes for the embedder's relay legs (the local reload
	// producer the listen drain consumes; a joiner's fired-round uplink).
	last_fired = io.fired;
	last_reload = io.reload;
}

void MissionKernel::set_movement_keys(bool forward, bool back, bool left,
		bool right, bool lean_left, bool lean_right, bool jump) {
	input.forward = forward;
	input.back = back;
	input.left = left;
	input.right = right;
	// Lean keys -> MoveOrder bits 6/7 [orig: g_inputFlags 0x2000/0x4000 packed
	// @0x4df708-0x4df741]; jump is a per-frame edge the motor consumes once
	// grounded.
	input.lean_left = lean_left;
	input.lean_right = lean_right;
	input.jump = jump;
	// Stance comes from the sim-owned SELECT latches (request_stance — the
	// C2S 0x1D apply semantics [orig: @0x501c60]).
	input.crouch = stance_latch_ == 1;
	input.prone = stance_latch_ == 2;
	// The movement-held latch and the unscope-on-move [orig:
	// Player_PackInputStateToEntity @0x4df450 — any of the four direction keys
	// sets g_movementKeyHeld (blocks scope-UP on Scoped weapons @0x4df29c)
	// and, while SETTLED at scope on a Scoped (flags 1) weapon, routes through
	// Player_ToggleWeaponScope @0x4df4c9..0x4df4ec = the full unscope. The
	// toggle's ForceScoped pin (@0x4df12d) keeps pinned sights raised].
	const bool move_held = forward || back || left || right;
	if (w::player_view_move_input(view, move_held,
				weapon.active ? weapon.def.flags : 0) &&
			(weapon.def.flags & DEF_WEAPON_FLAG_FORCESCOPED) == 0) {
		if (w::player_view_set_engaged(view, false,
					(weapon.def.flags2 & DEF_WEAPON_FLAG2_INSET) != 0))
			w::weapon_fsm_queue_scope_down(*w::active_local_weapon_slot(world, weapon));
	}
	w::local_player_view_refresh(&world, view);
}

bool MissionKernel::request_stance(int stance) {
	if (stance < 0 || stance > 2) return false;
	// ForceCrouch weapons refuse stance changes [orig: the case-169/170/172
	// gate Entity_CheckWeaponSeatFlags(equipped, 0x40000) @0x4e0d8a].
	if (weapon.active && weapon.force_crouch) return false;
	// So does the UseGun seat: a mounted gunner never sends the C2S 0x1D
	// [orig: Input_HandleActionBinding_0 cases 169/170/172 `parentEntity &&
	// parentSlot == 3` @0x4e0da0..0x4e0db5].
	if (const w::Entity *p = player();
			p != nullptr && p->mounted && p->mount_type == w::SeatType::Gunner)
		return false;
	if (stance_latch_ == stance) return false;
	// SELECT with mutual exclusion — the 0x1D apply writes one stance bit and
	// clears the other [orig: NapiNPServerMsg_HandleStanceChange @0x501c60:
	// 169 -> crouch, 170 -> prone, 172 -> clear both].
	stance_latch_ = stance;
	input.crouch = stance_latch_ == 1;
	input.prone = stance_latch_ == 2;
	return true;
}

void MissionKernel::tick_no_net(w::LogicTickPerf *perf) {
	apply_player_input_pre_tick();
	world.run_logic_tick(/*is_authority=*/true, w::TickPhase::Gameplay, perf);
	// The weather tick follows the entity update [orig: Game_ProcessMainFrame
	// @ 0x52674b -> @ 0x526774].
	tick_weather();
	run_local_player_post_tick();
	resolve_new_infantry_adm_ids();
}

void MissionKernel::tick_weather() {
	w::WeatherTickEvents events;
	world.weather.tick_sim(&world, events);
	if (events.thunder_a) world.weather_sounds.push_back(w::WeatherSoundEvent{0x10000, 0});
	if (events.thunder_b) world.weather_sounds.push_back(w::WeatherSoundEvent{0xA0000, 128});
	// A host without an audio presenter (the dedicated host) never drains
	// the queue: keep it bounded, dropping the oldest.
	constexpr size_t kWeatherSoundQueueCap = 32;
	while (world.weather_sounds.size() > kWeatherSoundQueueCap) {
		world.weather_sounds.erase(world.weather_sounds.begin());
	}
	// The quake HARD-SETS the shake counter [orig: @ 0x57eb7d / @ 0x57ec29].
	if (events.quake_shake_local) view.shake.counter = w::kShakeQuakeLevel;
	if (weather_render != nullptr) weather_render->weather_render_tick(world.weather);
}

void MissionKernel::settle_weather_mission_start() {
	world.weather.mission_start_init();
	for (int i = 0; i < 255; ++i) tick_weather();
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
	// raycast_proximity_entities @ 0x538350 — the end clips to the first hit].
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

void MissionKernel::reset_local_player_input(int32_t look_heading_bam) {
	input = w::PlayerInput{};
	stance_latch_ = 0;
	look_accum_x_ = look_accum_y_ = 0.0f;
	input.look_heading = look_heading_bam;
}

void MissionKernel::reset_local_player_input_to_player_facing() {
	int32_t heading = 0;
	if (world.cached.local_player.valid())
		if (const w::AiEntity *pe = ai.for_handle(world.cached.local_player))
			heading = pe->heading;
	reset_local_player_input(heading);
}

bool MissionKernel::restore_baseline() {
	if (!have_baseline) return false;
	const bool usegun_was_active = weapon.usegun_slot_active;
	const bool usegun_was_pending = weapon.usegun_switch != w::LocalUseGunSwitch::kNone;
	const uint8_t saved_personal_adm = weapon.usegun_saved_adm;
	weapon.events.clear();
	weapon.power_throw_start_tick = 0;
	weapon.pending_throw_charge = 0;
	weapon.fire_held = false;
	weapon.fire_pressed = false;
	weapon.reload_pressed = false;
	weapon.usegun_switch = w::LocalUseGunSwitch::kNone;
	weapon.usegun_slot_active = false;
	weapon.usegun_mount = w::EntityHandle{};
	weapon.usegun_weapon_adm = 0xFF;
	weapon.usegun_pending_mount = w::EntityHandle{};
	weapon.usegun_pending_weapon_adm = 0xFF;
	weapon.usegun_saved_adm = 0xFF;
	weapon.usegun_switch_action = -1;
	weapon.switch_deferred_action = -1;
	world.restore(baseline); // rewinds registry/vars/env/clock + re-inits systems (incl.
	                         // AI; WacSystem::on_load also resets its 62-tick accumulator)
	if (have_wac_baseline) wac.restore_runtime_state(wac_baseline);
	// Re-ground every soldier from scratch: the restored registry may reuse
	// handles across epochs, so the high-water mark cannot be trusted.
	infantry_adm_resolved_ai_count_ = 0;
	for (int i = 0; i < ai.count(); ++i)
		if (w::AiEntity *e = ai.at(i)) e->inf.adm_id = 0;
	resolve_new_infantry_adm_ids();
	if (usegun_was_active) {
		// The world snapshot restores the play-start entity set, while the
		// embedder still presents the borrowed emplacement definition.
		// Reinstall the saved personal selection as a fresh restart epoch.
		if (w::Entity *player_row = world.registry.get(world.cached.local_player))
			player_row->equipped_adm_index = saved_personal_adm;
		weapon.active = false;
		const w::WeaponTableEntry *saved_def = world.weapons.by_index(saved_personal_adm);
		weapon.start_in_switchto = saved_def != nullptr;
		w::WeaponPresentationEvent event;
		event.tick = world.logic_tick;
		if (const w::Entity *local = world.registry.get(world.cached.local_player))
			event.world_position = local->position;
		event.switch_to_weapon = saved_def != nullptr ? saved_def->name : std::string();
		event.clear_weapon = saved_def == nullptr;
		weapon.events.push_back(std::move(event));
	} else if (usegun_was_pending) {
		// The presenter never left the personal weapon, but its outgoing slot
		// may already be inside SWITCHFROM/RANK. Cancel only that action state
		// while retaining the personal magazine and reserve.
		const int32_t clip = weapon.slot.clip;
		const int32_t reserve = weapon.slot.reserve;
		weapon.slot = w::WeaponSlotState{};
		weapon.slot.clip = clip;
		weapon.slot.reserve = reserve;
	}
	// The FP channel position is a gated advance count, not a clock delta, so
	// the restored world keeps the held clip pose with no epoch re-stamp.
	w::local_player_view_reset(&world, weapon, view, view_tracker);
	return true;
}

// --- the local player -------------------------------------------------------

bool MissionKernel::has_local_player() const {
	return world.cached.local_player.valid() && world.registry.get(world.cached.local_player) != nullptr;
}

w::Entity *MissionKernel::player() {
	return world.cached.local_player.valid() ? world.registry.get(world.cached.local_player) : nullptr;
}

const w::Entity *MissionKernel::player() const {
	return world.cached.local_player.valid() ? world.registry.get(world.cached.local_player) : nullptr;
}

w::AiEntity *MissionKernel::player_ai() {
	return world.cached.local_player.valid() ? ai.for_handle(world.cached.local_player) : nullptr;
}

w::Vec3 MissionKernel::player_position() const {
	const w::Entity *e = player();
	return e != nullptr ? e->position : w::Vec3{};
}

int32_t MissionKernel::player_health() const {
	const w::Entity *e = player();
	return e != nullptr ? e->health : 0;
}

std::string MissionKernel::player_anim_key() const {
	const w::AiEntity *e =
			world.cached.local_player.valid() ? ai.for_handle(world.cached.local_player) : nullptr;
	if (e == nullptr || !e->inf.active) return std::string();
	return w::infantry_anim_key(e->inf.anim_state);
}

// Mouse pixels onto the look angles through the witnessed integer pipeline
// [orig: Input_ProcessMouseAxisBindings @0x499680]. The float accumulator is
// the device-input fold over retail's integer remainder pump [orig:
// Game_ProcessMainFrame @0x526481..0x5264a9, dword_24E0E78].
void MissionKernel::look(float dx_px, float dy_px) {
	int32_t scoped_zoom = 0;
	if (!view.binoculars_view_active && weapon.active && view.scope_engaged && weapon.scope_max_mag > 1.0f)
		scoped_zoom = static_cast<int32_t>(weapon.scope_max_mag);
	const bool prone = stance_latch_ == 2;
	look_accum_x_ += dx_px;
	look_accum_y_ += dy_px;
	const int32_t dx = static_cast<int32_t>(look_accum_x_);
	const int32_t dy = static_cast<int32_t>(look_accum_y_);
	look_accum_x_ -= static_cast<float>(dx);
	look_accum_y_ -= static_cast<float>(dy);
	if (dx == 0 && dy == 0) return;
	w::player_look_apply(input.look_heading, input.look_pitch, look_settings, dx, dy, scoped_zoom, prone);
}

void MissionKernel::aim_at(const w::Vec3 &eye, const w::Vec3 &target) {
	const double dx = target.x - eye.x, dy = target.y - eye.y, dz = target.z - eye.z;
	const double horizontal = std::sqrt(dx * dx + dy * dy);
	input.look_heading = bam_from_radians(std::atan2(dy, dx));
	input.look_pitch = bam_from_radians(std::atan2(dz, horizontal));
	if (w::AiEntity *p = player_ai()) {
		p->inf.target_heading = input.look_heading;
		p->inf.look_pitch = input.look_pitch;
	}
}

void MissionKernel::teleport_local_player(const w::Vec3 &mission_pos, double yaw_deg, double pitch_deg) {
	w::Entity *e = player();
	w::AiEntity *p = player_ai();
	if (e == nullptr || p == nullptr) return;
	e->position = mission_pos;
	p->pos[0] = w::to_fixed(mission_pos.x);
	p->pos[1] = w::to_fixed(mission_pos.y);
	p->pos[2] = w::to_fixed(mission_pos.z);
	p->heading = w::bam_heading_from_mission_yaw_deg(yaw_deg);
	p->pitch = static_cast<int32_t>(pitch_deg / w::kDegreesPerBam);
	// The input-owned view mirrors, or the next pre-tick snaps the view back.
	p->inf.target_heading = p->heading;
	p->inf.look_pitch = p->pitch;
	input.look_heading = p->heading;
	input.look_pitch = p->pitch;
	e->flags &= ~w::kEntityFlagLadderContact;
	e->engine_flags &= ~w::kEntityFlagLadderContact;
	p->inf.pitch_restore_active = false;
	p->inf.pitch_restore_target = 0;
	p->inf.pitch_restore_prev = 0;
	p->collide_state = {};
}

void MissionKernel::set_weapon_input(bool fire_held, bool fire_pressed, bool reload_pressed) {
	w::local_weapon_set_input(weapon, view, fire_held, fire_pressed, reload_pressed);
}

bool MissionKernel::install_weapon(const std::string &weapon_name, bool preserve_slot_state) {
	if (!weapon_defs_ok || weapon_name.empty()) return false;
	const DefWeaponDef *row = nullptr;
	for (size_t i = 0; i < weapon_defs.count; ++i) {
		if (strutil::iequals(weapon_defs.entries[i].weapon_name, weapon_name)) {
			row = &weapon_defs.entries[i];
			break;
		}
	}
	if (row == nullptr) return false;
	clip_index.load(asset_index(), row->animadm);
	w::WeaponInstallData data;
	data.name = row->weapon_name;
	data.animadm = row->animadm;
	data.flags = row->flags;
	data.flags2 = row->flags2;
	data.heat_per_shot = row->heat_per_shot;
	data.heat_decay_per_tick = row->heat_decay_per_tick;
	data.heat_glow_threshold = row->heat_glow_threshold;
	data.scope_max_mag = row->scope_max_mag;
	data.attack_anim = row->attack_anim;
	data.run_anim = row->run_anim;
	data.clipsize = row->clipsize;
	data.startrounds = row->startrounds;
	data.rows.reserve(row->actions_count);
	for (size_t a = 0; a < row->actions_count; ++a) {
		const DefWeaponAction &act = row->actions[a];
		w::WeaponFsmActionRow r;
		std::snprintf(r.name, sizeof(r.name), "%s", act.name);
		std::snprintf(r.anim, sizeof(r.anim), "%s", act.anim);
		std::snprintf(r.function, sizeof(r.function), "%s", act.function);
		r.delaystart = act.delaystart;
		r.delayend = act.delayend;
		std::snprintf(r.soundset, sizeof(r.soundset), "%s", act.soundset);
		std::snprintf(r.soundsetend, sizeof(r.soundsetend), "%s", act.soundsetend);
		std::snprintf(r.particle, sizeof(r.particle), "%s", act.particle);
		std::snprintf(r.particleuserpoint, sizeof(r.particleuserpoint), "%s", act.particleuserpoint);
		data.rows.push_back(r);
	}
	const auto add_key = [&](const char *key) {
		if (key == nullptr || key[0] == '\0') return;
		const std::string lowered = strutil::to_lower(key);
		for (const auto &kv : data.clip_rings)
			if (kv.first == lowered) return;
		if (const std::vector<float> *lengths = clip_index.lengths_for(key))
			data.clip_rings.emplace_back(lowered, *lengths);
	};
	add_key("anim_wpn_idle");
	add_key("anim_wpn_empty_idle");
	for (size_t a = 0; a < row->actions_count; ++a) add_key(row->actions[a].anim);
	w::local_weapon_install(world, weapon, data, preserve_slot_state,
			/*allow_same_weapon_rebake=*/false, inventory_valid ? &inventory : nullptr, view);
	return true;
}

bool MissionKernel::toggle_mount() {
	const w::Entity *toggle_player = player();
	if (toggle_player == nullptr || !toggle_player->alive || toggle_player->health <= 0) return false;
	w::sync_local_usegun_weapon_transition(world, weapon);
	const w::WeaponSlotState *active_slot = w::active_local_weapon_slot(world, weapon);
	if (active_slot != nullptr &&
			!w::weapon_state_allows_mount_toggle(active_slot->current, active_slot->next))
		return false;
	// The null-EquippedSlot rejection belongs to UseGun itself, not the
	// top-level USE action: an unarmed local player still enters an ordinary
	// passenger/control seat, and it is an out-of-session-only player gate
	// (force/script and NAPI authority paths bypass it) [orig:
	// Entity_AttachToUseGunSlot @0x546b80, reject `!is_in_session &&
	// Flags&0x100 && !EquippedSlot` @0x546c07].
	if (!session_open && !weapon.active) {
		w::VehicleSeatSelection hit;
		if (w::find_mount_toggle_candidate(world, *toggle_player, hit) && hit.type == w::SeatType::Gunner)
			return false;
	}
	const bool changed = w::player_toggle_vehicle_mount(world, world.cached.local_player);
	if (changed) {
		view.binoculars_requested = false;
		view_tracker.binocular_yaw_offset_deg = 0.0f;
		view_tracker.binocular_pitch_offset_deg = 0.0f;
		w::local_player_view_refresh(&world, view);
		sync_local_mounted_input_heading();
		w::sync_local_usegun_weapon_transition(world, weapon);
	}
	return changed;
}

w::LocalPlayerViewFrame MissionKernel::view_frame() {
	w::LocalPlayerViewFrame f;
	w::local_player_view_frame(&world, weapon, view, view_tracker, f);
	return f;
}

// --- entities ---------------------------------------------------------------

w::Entity *MissionKernel::by_net_id(uint16_t ssn) {
	const w::EntityHandle h = world.registry.find_by_net_id(ssn);
	return h.valid() ? world.registry.get(h) : nullptr;
}

w::Entity *MissionKernel::by_bms_id(int32_t bms_id) {
	w::EntityHandle found;
	world.registry.for_each([&](const w::Entity &e) {
		if (!found.valid() && e.bms_id == bms_id) found = e.handle;
	});
	return found.valid() ? world.registry.get(found) : nullptr;
}

w::AiEntity *MissionKernel::ai_for(w::EntityHandle h) {
	return h.valid() ? ai.for_handle(h) : nullptr;
}

void MissionKernel::set_entity_position(w::EntityHandle h, const w::Vec3 &mission_pos) {
	world.commands.set_entity_position(h, mission_pos);
}

void MissionKernel::set_entity_health(w::EntityHandle h, int32_t hp) {
	world.commands.set_entity_health(h, hp);
}

// --- terrain ----------------------------------------------------------------

float MissionKernel::ground_height(float mission_x, float mission_y) const {
	// The field's world frame is the renderer's: x, and z = -mission y.
	return terrain::height_field_height_world_bilinear(terrain_store.height_field(),
			mission_x, -mission_y);
}

// --- observation ------------------------------------------------------------

std::vector<w::Effect> MissionKernel::drain_effects() {
	std::vector<w::Effect> out = world.effects.entries();
	world.effects.clear();
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

// --- world::IMountedPoseProvider --------------------------------------------

bool MissionKernel::resolve_mounted_pose(w::World &p_world, const w::Entity &carrier,
		const w::Seat &seat, w::MountedPose &out) {
	// The kernel IS the world's IMountedPoseProvider: the one mounted-pose
	// resolver every embedder's world reaches.
	if (&p_world != &world || seat.type != w::SeatType::Gunner || seat.bone_index == 0) return false;
	++mounted_queries;
	const Threedi3di3 *model_ptr = nullptr;
	const auto graphic = mounted_graphics.find(carrier.item_id);
	if (graphic != mounted_graphics.end() && models.has_index()) model_ptr = models.model_for(graphic->second);
	if (model_ptr == nullptr) {
		++mounted_declines;
		return false;
	}
	const Threedi3di3 &model = *model_ptr;
	if (model.ctrl.count > 0 && model.ctrl.registers == nullptr) {
		++mounted_declines;
		return false;
	}
	simassets::MountedPoseControlSources sources;
	if (const w::AiEntity *carrier_ai = ai.for_handle(carrier.handle)) {
		sources.part_anim_phase0 = carrier_ai->brain.f[w::AiBrain::kPartAnimPhase0];
		sources.part_anim_phase1 = carrier_ai->brain.f[w::AiBrain::kPartAnimPhase0 + 1];
	}
	sources.has_heat_glow = w::world_model_heat_glow_for(world, carrier, sources.heat_glow);
	w::EmplacedWeaponControls emplaced;
	if (w::emplaced_weapon_controls_for(world, &ai, carrier, emplaced)) {
		sources.has_emplaced = true;
		sources.emplaced_gun_yaw = emplaced.gun_yaw;
		sources.emplaced_gun_pitch = emplaced.gun_pitch;
	}
	int32_t ctrl_bus[THREEDI_CTRL_REGISTER_COUNT] = {};
	simassets::compose_mounted_pose_controls(carrier.item_attrib, sources, ctrl_bus);
	std::array<int32_t, THREEDI_CTRL_REGISTER_COUNT> controls{};
	std::copy(std::begin(ctrl_bus), std::end(ctrl_bus), controls.begin());
	const uint32_t time_ms = simassets::mounted_pose_time_ms(world.logic_tick,
			panm_time_override_ms);
	if (mounted_cache_tick_ != world.logic_tick) {
		mounted_live_cache_.clear();
		mounted_cache_tick_ = world.logic_tick;
	}
	auto rest = mounted_rest_cache_.find(model_ptr);
	if (rest == mounted_rest_cache_.end()) {
		MountedPoseRest r;
		if (!simassets::evaluate_model_mounted_pose_parts(model, 0u, nullptr, r.parts)) {
			++mounted_declines;
			return false;
		}
		rest = mounted_rest_cache_.emplace(model_ptr, std::move(r)).first;
	}
	std::vector<MountedPoseLive> &live_list = mounted_live_cache_[model_ptr];
	auto live = std::find_if(live_list.begin(), live_list.end(), [&](const MountedPoseLive &c) {
		return c.time_ms == time_ms && c.controls == controls;
	});
	if (live == live_list.end()) {
		MountedPoseLive l;
		l.time_ms = time_ms;
		l.controls = controls;
		l.valid = simassets::evaluate_model_mounted_pose_parts(model, time_ms, controls.data(), l.parts);
		live_list.push_back(std::move(l));
		live = live_list.end() - 1;
		++mounted_evaluations;
	} else {
		++mounted_cache_hits;
	}
	const bool resolved = live->valid && simassets::resolve_model_mounted_pose_from_parts(
			model, carrier, seat, rest->second.parts, live->parts, out);
	if (!resolved) ++mounted_declines;
	return resolved;
}

// --- world::ICollisionSectionMatrixProvider ---------------------------------

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
	const simassets::CollisionResolveDeps deps{collision, occlusion, collision_pose, models};
	collision_attached = simassets::resolve_collision_instances(world, *items_table(),
			collision_state, deps);
	return collision.has_instance(world, entity);
}

bool MissionKernel::build_section_matrices(w::World &p_world, w::EntityHandle entity,
		int32_t model_id, const w::CollisionMatrix &entity_world, const w::CollisionModel &model,
		std::vector<w::CollisionMatrix> &out) {
	collision_pose.weapon_active = weapon.active;
	collision_pose.panm_time_override_ms = panm_time_override_ms;
	++collision_queries;
	if (collision_pose.build_section_matrices(p_world, entity, model_id, entity_world, model, out))
		return true;
	if (collision_pose.has_skeletal_entity(entity) || collision_pose.has_generic_model(model_id))
		++collision_declines;
	return false;
}

} // namespace opennova::mission
