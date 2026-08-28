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

MissionKernel::MissionKernel() = default;

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
	files_ = std::move(files);
	if (items_ok) {
		def_free_items(&items);
		items = DefItemsFile{};
		items_ok = false;
	}
	std::vector<uint8_t> items_bytes;
	if (files_.valid() && files_.read_file("items.def", items_bytes) &&
			def_parse_items_memory(items_bytes.data(), items_bytes.size(), &items) == 0)
		items_ok = true;
}

void MissionKernel::wire_terrain() {
	world.terrain = terrain_store.valid() ? &terrain_store.height_field() : nullptr;
	ai.terrain = terrain_store.valid() ? &terrain_store.height_field() : nullptr;
	collision.terrain = terrain_store.valid() ? &terrain_store.height_field() : nullptr;
	// The footstep surface pick reads the charmap through this view, exactly
	// as Simulation::apply_terrain_to_ai wires it (the kernel has no mission
	// .til path here, matching the sim's empty-tiles leg).
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
	if (!items_ok) return {};
	return [this](int32_t type_id) {
		PromoteOptions::AiProfileDefaults d;
		const int item_id = static_cast<int>(type_id) + static_cast<int>(kItemIdOffset);
		const DefItemDef *def = simassets::find_item_def(items, item_id);
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
	baseline = world.snapshot();
	have_baseline = true;
	ai.capture_spawn_baseline();
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
	const w::EntityHandle h = w::spawn_player(world, spawn);
	if (!h.valid()) return -1;
	resolve_new_infantry_adm_ids();
	input = w::PlayerInput{};
	input.look_heading = w::bam_heading_from_mission_yaw_deg(spawn.yaw);
	stance_latch_ = 0;
	look_accum_x_ = look_accum_y_ = 0.0f;
	w::local_player_view_reset(&world, weapon, view, view_tracker);
	return sel.found ? 1 : 0;
}

void MissionKernel::resolve_new_infantry_adm_ids() {
	if (!infantry_adm_retained_ || !items_ok || root_motion.empty()) return;
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
		const int visual = simassets::visual_item_id_for_runtime_type(ent->item_id, items);
		const DefItemDef *def = simassets::find_item_def(items, visual);
		if (def == nullptr || def->anim_def[0] == '\0') continue;
		std::string adm = def->anim_def;
		if (!strutil::ends_with_icase(adm, ".adm")) adm += ".adm";
		const int adm_id = root_motion.register_adm(&index, adm);
		if (adm_id >= 0) e->inf.adm_id = adm_id;
	}
	infantry_adm_resolved_ai_count_ = count;
}

bool MissionKernel::load_weapon_table() {
	std::vector<uint8_t> bytes;
	if (!files_.read_file("weapon.def", bytes)) return false;
	DefWeaponsFile file = {};
	if (def_parse_weapons_memory(bytes.data(), bytes.size(), &file) != 0) return false;
	world.weapons = w::build_weapon_table(file, &index);
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

bool MissionKernel::load_ammo_table() {
	std::vector<uint8_t> bytes;
	if (!files_.read_file("ammo.def", bytes)) return false;
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
	if (!files_.valid()) {
		error = "open() first";
		return false;
	}
	// The sim's own model source, wired before the seat step runs (S16).
	models.set_index(&index);
	collision_pose.set_resource_index(&index);
	bringup_net_session_ = options.bringup_net_session;

	BootParams params;
	params.is_joiner = false;
	params.playable = options.playable;
	params.has_resource_root = true;
	params.has_item_db = items_ok;
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
		if (!seeds.empty()) {
			simassets::SeatSpecExtraction native;
			simassets::extract_item_seat_specs(items,
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
		text_source = static_cast<int>(resolve_mission_text(files_, mission_basename, text));
		text_size = text.size();
	};
	steps.load_mission = [&] { return load_mission_into_world(); };
	steps.install_terrain_field = [&] { wire_terrain(); };
	steps.install_sound_profiles = [] {}; // the footstep/foley slot table is presentation-side
	steps.install_infantry_anim = [&] {
		root_motion.clear();
		infantry_adm_resolved_ai_count_ = 0;
		const int default_adm = root_motion.register_adm(&index, options.infantry_adm);
		if (default_adm != 0)
			io::logf(io::LogLevel::kWarn,
					"mission kernel: no infantry clips from '%s' - AI soldiers will stand still",
					options.infantry_adm.c_str());
		ai.root_motion = &root_motion;
		if (default_adm == 0) resolve_new_infantry_adm_ids();
	};
	steps.install_wac = [&] {
		wac_loaded = false;
		std::string wac_error;
		const wac::WacLayeredLoadStatus status = wac::wac_layered_load(wac, files_,
				mission_basename, &world.registry, /*strict_diagnostics=*/false, wac_error);
		if (status == wac::WacLayeredLoadStatus::kBlocked) {
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
	steps.resolve_infantry_adm = [&] {
		infantry_adm_retained_ = true;
		infantry_adm_resolved_ai_count_ = 0;
		for (int i = 0; i < ai.count(); ++i)
			if (w::AiEntity *e = ai.at(i)) e->inf.adm_id = 0;
		resolve_new_infantry_adm_ids();
	};
	steps.resolve_item_traits = [&] {
		simassets::resolve_item_traits(world, items, [](int32_t) -> uint8_t { return 0; });
	};
	steps.install_asset_root = [] {};
	steps.resolve_collision = [&] {
		if (!options.collision) return;
		wire_collision();
		const simassets::CollisionResolveDeps deps{collision, occlusion, collision_pose, models};
		collision_attached = simassets::resolve_collision_instances(world, items, collision_state, deps);
	};
	steps.occlusion_init = [&] {
		if (!options.collision) return;
		collision.build_initial_tables(world);
		occlusion.init_mission(world, collision);
	};
	steps.load_weapon_table = [&] {
		if (!load_weapon_table())
			io::logf(io::LogLevel::kWarn, "mission kernel: weapon.def not loaded");
	};
	steps.load_ammo_table = [&] { return load_ammo_table(); };
	steps.resolve_ai_weapons = [&] { simassets::resolve_ai_weapons(world, items); };

	const BootAbort abort = run_mission_boot(params, steps);
	if (abort != BootAbort::kNone) {
		error = "mission boot aborted (load failed)";
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
	if (local == nullptr || body == nullptr || !local->alive || local->health <= 0 || !weapon.active)
		return false;
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
	const bool ordinary = !in_air && (sighted || (scoped && !body->inf.player_moving)) &&
			(sighted || !submerged);
	return (flags & DEF_WEAPON_FLAG_FORCESCOPED) != 0 || ordinary;
}

void MissionKernel::apply_player_input_pre_tick() {
	if (!world.cached.local_player.valid()) return;
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
		uint32_t view_flags = 0;
		if (view.nvg_active) view_flags |= 0x4u;
		if (view.binoculars_raised) view_flags |= 0x8u;
		if (scope_promoted) view_flags |= 0x10u;
		entity->flags = (entity->flags & ~0x1cu) | view_flags;
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
	w::local_player_view_tick(&world, weapon, view, view_tracker, w::LocalViewSessionInputs{});
	w::LocalWeaponPumpIO io;
	io.view = &view;
	io.inventory = inventory_valid ? &inventory : nullptr;
	io.is_authority = true;
	w::local_weapon_pump_tick(world, weapon, io);
}

void MissionKernel::tick_no_net() {
	apply_player_input_pre_tick();
	world.run_logic_tick(/*is_authority=*/true, w::TickPhase::Gameplay);
	run_local_player_post_tick();
	resolve_new_infantry_adm_ids();
}

void MissionKernel::reset_local_player_input_to_player_facing() {
	input = w::PlayerInput{};
	stance_latch_ = 0;
	look_accum_x_ = look_accum_y_ = 0.0f;
	if (world.cached.local_player.valid())
		if (const w::AiEntity *pe = ai.for_handle(world.cached.local_player))
			input.look_heading = pe->heading;
}

bool MissionKernel::restore_baseline() {
	if (!have_baseline) return false;
	world.restore(baseline);
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
	const w::AiEntity *e = world.cached.local_player.valid()
			? const_cast<w::AiSystem &>(ai).for_handle(world.cached.local_player)
			: nullptr;
	if (e == nullptr || !e->inf.active) return std::string();
	const int state = e->inf.anim_state;
	if (state < 0 || state >= w::kInfantryAnimStateCount) return std::string();
	return std::string("anim_") + w::kInfantryAnimNames[state];
}

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
	p->pos[0] = static_cast<int32_t>(mission_pos.x * 65536.0f);
	p->pos[1] = static_cast<int32_t>(mission_pos.y * 65536.0f);
	p->pos[2] = static_cast<int32_t>(mission_pos.z * 65536.0f);
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
	clip_index.load(&index, row->animadm);
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
	// The out-of-session UseGun rejection: an unarmed local player still
	// enters an ordinary seat [orig: Entity_AttachToUseGunSlot @0x546b80].
	if (!weapon.active) {
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
	if (w::Entity *e = world.registry.get(h)) e->position = mission_pos;
	if (w::AiEntity *a = ai.for_handle(h)) {
		a->pos[0] = static_cast<int32_t>(mission_pos.x * 65536.0f);
		a->pos[1] = static_cast<int32_t>(mission_pos.y * 65536.0f);
		a->pos[2] = static_cast<int32_t>(mission_pos.z * 65536.0f);
	}
}

void MissionKernel::set_entity_health(w::EntityHandle h, int32_t hp) {
	if (w::AiEntity *a = ai.for_handle(h)) a->health = static_cast<int16_t>(hp);
	if (w::Entity *e = world.registry.get(h)) {
		e->health = hp;
		e->alive = hp > 0;
	}
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
	const int32_t a[3] = {static_cast<int32_t>(anchor.x * 65536.0f),
			static_cast<int32_t>(anchor.y * 65536.0f), static_cast<int32_t>(anchor.z * 65536.0f)};
	return collision.debug_instances(world, a, static_cast<int32_t>(range_units * 65536.0f), max_instances);
}

std::vector<w::CollisionWorld::DebugHitboxEntity> MissionKernel::hitboxes(
		const w::Vec3 &anchor, float range_units, int32_t max_entities, int32_t max_faces) {
	const int32_t a[3] = {static_cast<int32_t>(anchor.x * 65536.0f),
			static_cast<int32_t>(anchor.y * 65536.0f), static_cast<int32_t>(anchor.z * 65536.0f)};
	return collision.debug_hitboxes(world, a, static_cast<int32_t>(range_units * 65536.0f),
			max_entities, max_faces);
}

// --- world::IMountedPoseProvider --------------------------------------------

bool MissionKernel::resolve_mounted_pose(w::World &p_world, const w::Entity &carrier,
		const w::Seat &seat, w::MountedPose &out) {
	// The native-only mounted-pose resolver (Simulation::resolve_mounted_pose_native).
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
	const uint32_t time_ms = simassets::mounted_pose_time_ms(world.logic_tick, -1);
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
	if (&p_world != &world || !items_ok) return false;
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
	collision_attached = simassets::resolve_collision_instances(world, items, collision_state, deps);
	return collision.has_instance(world, entity);
}

bool MissionKernel::build_section_matrices(w::World &p_world, w::EntityHandle entity,
		int32_t model_id, const w::CollisionMatrix &entity_world, const w::CollisionModel &model,
		std::vector<w::CollisionMatrix> &out) {
	collision_pose.weapon_active = weapon.active;
	collision_pose.panm_time_override_ms = -1;
	++collision_queries;
	if (collision_pose.build_section_matrices(p_world, entity, model_id, entity_world, model, out))
		return true;
	if (collision_pose.has_skeletal_entity(entity) || collision_pose.has_generic_model(model_id))
		++collision_declines;
	return false;
}

} // namespace opennova::mission
