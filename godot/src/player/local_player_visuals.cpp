#include "player/local_player_visuals.h"

#include "env/mission_environment.h"
#include "mission/mission_data.h"
#include "mission/mission_root.h"
#include "object/item_database.h"
#include "object/weapon_database.h"
#include "player/local_player_presenter.h"
#include "resource_index/resource_root.h"
#include "simulation/fp_viewmodel_spec.h"
#include "simulation/player_inventory.h"
#include "simulation/simulation.h"
#include "simulation/weapon_kit_entry.h"
#include "util/string_convert.h"

#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <runtime/world/player_present.h>

using namespace godot;
using opennova::to_std;

void LocalPlayerVisuals::setup(Node *p_world) {
	world_id_ = p_world != nullptr ? ObjectID(p_world->get_instance_id()) : ObjectID();
}

void LocalPlayerVisuals::reset() {
	spawn_loadout_.unref();
	local_weapon_.unref();
	viewmodel_def_name_ = String();
	viewmodel_def_.unref();
	local_weapon_preserve_slot_state_ = false;
	viewmodel_weapon_override_ = String();
	viewmodel_weapon_cleared_ = false;
}

Node *LocalPlayerVisuals::world() const {
	return Object::cast_to<Node>(ObjectDB::get_instance(world_id_));
}

Node3D *LocalPlayerVisuals::world_node3d() const {
	return Object::cast_to<Node3D>(ObjectDB::get_instance(world_id_));
}

Ref<Simulation> LocalPlayerVisuals::sim() const {
	Node *node = world();
	if (node == nullptr) {
		return Ref<Simulation>();
	}
	return Ref<Simulation>(node->call("get_sim"));
}

MissionRoot *LocalPlayerVisuals::runtime() const {
	Node *node = world();
	if (node == nullptr) {
		return nullptr;
	}
	return Object::cast_to<MissionRoot>(static_cast<Object *>(node->call("get_runtime")));
}

// The placer of the loaded mission (the root it was built over keeps it);
// null before a mission is placed.
Ref<MissionObjectPlacer> LocalPlayerVisuals::placer() const {
	MissionRoot *root = runtime();
	return root != nullptr ? root->get_placer() : Ref<MissionObjectPlacer>();
}

Ref<ResourceRoot> LocalPlayerVisuals::resource_root() const {
	Node *node = world();
	if (node == nullptr) {
		return Ref<ResourceRoot>();
	}
	return Ref<ResourceRoot>(node->call("get_resource_root"));
}

Ref<WeaponDatabase> LocalPlayerVisuals::weapon_database() const {
	Node *node = world();
	if (node == nullptr) {
		return Ref<WeaponDatabase>();
	}
	return Ref<WeaponDatabase>(node->call("get_weapon_database"));
}

MissionEnvironment *LocalPlayerVisuals::environment() const {
	Node *node = world();
	if (node == nullptr) {
		return nullptr;
	}
	return Object::cast_to<MissionEnvironment>(static_cast<Object *>(node->call("get_environment_node")));
}

Ref<MissionData> LocalPlayerVisuals::loaded_mission() const {
	Node *node = world();
	if (node == nullptr) {
		return Ref<MissionData>();
	}
	return Ref<MissionData>(node->call("get_loaded_mission"));
}

void LocalPlayerVisuals::set_first_person_model_available(bool p_available) {
	const Ref<Simulation> model_sim = sim();
	if (model_sim.is_valid()) {
		model_sim->set_local_player_first_person_model_available(p_available);
	}
}

int LocalPlayerVisuals::local_player_character_id() const {
	const Ref<Simulation> id_sim = sim();
	return id_sim.is_valid() ? id_sim->get_local_player_character_id() : 0;
}

ObjectModel *LocalPlayerVisuals::build_local_player_held_weapon(const String &p_graphic) {
	const Ref<MissionObjectPlacer> mission_placer = placer();
	Node3D *parent = world_node3d();
	if (mission_placer.is_null() || parent == nullptr || p_graphic.is_empty()) {
		return nullptr;
	}
	ObjectModel *model = mission_placer->build_model_from_graphic(p_graphic, String(), parent, String(),
			String(), true);
	if (model != nullptr) {
		model->set_shadow_caster_enabled(true);
		// The 3P gun silhouettes inside the AVATAR's render slot, exactly like
		// retail's child walk (RenderSlot_RenderEntityAndChildren renders the
		// held weapon with the person) -- never in a slot of its own.
		LocalPlayerPresenter *presenter = Object::cast_to<LocalPlayerPresenter>(
				static_cast<Object *>(parent->call("local_view_presenter")));
		if (presenter != nullptr) {
			model->set_slot_shadow_capture_with(presenter->avatar());
			model->set_entity_light_owner(presenter->avatar());
		}
	}
	return model;
}

ObjectModel *LocalPlayerVisuals::build_local_player_avatar() {
	const Ref<MissionObjectPlacer> mission_placer = placer();
	Node3D *parent = world_node3d();
	if (mission_placer.is_null() || parent == nullptr) {
		return nullptr;
	}
	// The environment wires the TOD-reactive lighting/fog stamp -- without it
	// the avatar freezes at the noon preview defaults (retail relights every
	// entity per frame; witness: placement_traits.h ledger).
	return mission_placer->build_player_animated_model(MissionObjectPlacer::PLAYER_RUNTIME_TYPE_ID, parent,
			local_player_character_id());
}

Ref<PlayerVisualSpec> LocalPlayerVisuals::local_player_visual_spec() {
	const Ref<MissionObjectPlacer> mission_placer = placer();
	if (mission_placer.is_null()) {
		return Ref<PlayerVisualSpec>();
	}
	return mission_placer->resolve_player_visual_spec(MissionObjectPlacer::PLAYER_RUNTIME_TYPE_ID,
			local_player_character_id());
}

PackedStringArray LocalPlayerVisuals::prewarm_loaded_model_challenge_definitions() {
	PackedStringArray resolved; // the graphics warmed, in order
	const Ref<MissionObjectPlacer> mission_placer = placer();
	if (mission_placer.is_null()) {
		return resolved;
	}
	// By the deployment/admission boundary the complete initial world stream
	// has populated the joiner's replica snapshot. (S2C 0x11 itself comes
	// earlier and releases the client's C2S 0x0A world request.) Resolve each
	// unique wire type now so the C2S 0x3D loaded-model page freezes before
	// the first visible frame.
	MissionRoot *root = runtime();
	const Ref<Simulation> challenge_sim = root != nullptr ? root->get_sim() : Ref<Simulation>();
	if (challenge_sim.is_valid()) {
		const int stride = challenge_sim->get_present_stride();
		const PackedFloat32Array snapshot = challenge_sim->get_present_snapshot();
		HashSet<int> warmed_types;
		PackedInt32Array warmed_order;
		if (stride >= Simulation::PF_STRIDE) {
			const int64_t rows = snapshot.size() / stride;
			for (int64_t row = 0; row < rows; ++row) {
				const int runtime_type_id = static_cast<int>(snapshot[row * stride + Simulation::PF_TYPE_ID]);
				if (runtime_type_id == 0 || warmed_types.has(runtime_type_id)) {
					continue;
				}
				warmed_types.insert(runtime_type_id);
				warmed_order.push_back(runtime_type_id);
				const int visual_item_id = mission_placer->resolve_player_visual_item_id(runtime_type_id);
				const String wire_graphic = mission_placer->graphic_for(visual_item_id);
				if (!wire_graphic.is_empty()) {
					mission_placer->object_data_for(wire_graphic);
					resolved.push_back(wire_graphic);
				}
			}
		}
		// The header-only join learned its entity types from the stream after
		// MissionRoot's ordinary mission-body setup. Resolve the model-derived
		// seat/emplacement table and world collision/trait consumers now,
		// before admission and before the loaded-model challenge page freezes.
		const Ref<MissionData> mission = loaded_mission();
		if (mission.is_valid() && mission->is_wire_header_only()) {
			const Ref<ItemDatabase> item_db = mission_placer->get_item_db();
			if (item_db.is_valid()) {
				// S16: the native extractor reads model userpoints through the
				// sim's own parse cache, so the asset root wires FIRST (the
				// shell extractor that read models render-side is gone).
				const Ref<ResourceRoot> root_dir = resource_root();
				if (root_dir.is_valid()) {
					challenge_sim->set_asset_root(root_dir);
				}
				challenge_sim->install_seat_specs_for_type_ids(item_db, warmed_order);
				challenge_sim->resolve_item_traits(item_db);
				challenge_sim->resolve_collision_instances(item_db);
				challenge_sim->occlusion_init_mission();
			}
		}
	}
	const int player_visual_item_id = mission_placer->resolve_player_visual_item_id(
			MissionObjectPlacer::PLAYER_RUNTIME_TYPE_ID);
	const String avatar_graphic = mission_placer->graphic_for(player_visual_item_id);
	if (!avatar_graphic.is_empty()) {
		mission_placer->object_data_for(avatar_graphic);
		resolved.push_back(avatar_graphic);
	}

	if (viewmodel_weapon_cleared_) {
		return resolved;
	}
	const Ref<PlayerViewmodelDef> def = local_player_viewmodel_def();
	const Ref<PlayerVisualSpec> character_spec = local_player_visual_spec();
	const Ref<FpViewmodelSpec> spec = Simulation::fp_viewmodel_spec(def.is_valid(),
			def.is_valid() ? def->get_gfx1() : String(),
			character_spec.is_valid() ? character_spec->get_arms() : String(),
			def.is_valid() ? def->get_animadm() : String(), def.is_valid() ? def->get_flags() : 0);
	if (!spec->get_gun().is_empty()) {
		mission_placer->object_data_for(spec->get_gun());
		resolved.push_back(spec->get_gun());
	}
	if (spec->get_show_arms() && !spec->get_arms().is_empty()) {
		mission_placer->object_data_for(spec->get_arms());
		resolved.push_back(spec->get_arms());
	}
	return resolved;
}

bool LocalPlayerVisuals::set_local_player_weapon_by_name(const String &p_weapon_name,
		bool p_preserve_slot_state) {
	if (p_weapon_name.is_empty()) {
		return false;
	}
	const Ref<WeaponDatabase> weapon_db = weapon_database();
	const int index = weapon_db.is_valid() ? weapon_db->find_weapon(p_weapon_name) : -1;
	if (index < 0) {
		UtilityFunctions::push_warning(
				vformat("GameWorld: armory weapon '%s' not in weapon.def -- keeping current", p_weapon_name));
		return false;
	}
	viewmodel_weapon_override_ = p_weapon_name;
	viewmodel_weapon_cleared_ = false;
	local_weapon_preserve_slot_state_ = p_preserve_slot_state;
	// The render-side def record (viewmodel gfx/adm/fov reads + the name guard).
	local_weapon_ = weapon_db->get_weapon(index);
	const Ref<Simulation> mount_sim = sim();
	if (mount_sim.is_valid()) {
		set_first_person_model_available(false);
		// One-step native mount (S6b, ADR 0028): the sim bakes the FSM from
		// its RETAINED weapon.def row and seeds the clip rings from the rig's
		// own .adm -- the mount is not hostage to the FP model load, matching
		// the retail ACCEPT chain exactly [orig: WeaponSlotTable_LoadAllFromDefs
		// @0x5414e0 + Player_MountWeaponSlot @0x4dfa40; the FP model resolve is
		// a separate per-frame render consumer @0x4ded60].
		if (!mount_sim->install_local_player_weapon_by_name(p_weapon_name, local_weapon_preserve_slot_state_)) {
			UtilityFunctions::push_warning(
					vformat("GameWorld: sim has no retained weapon.def row for '%s'", p_weapon_name));
			return false;
		}
	}
	return true;
}

void LocalPlayerVisuals::apply_local_player_spawn_loadout() {
	const Ref<PlayerSpawnLoadout> loadout = spawn_loadout_;
	spawn_loadout_.unref();
	const Ref<Simulation> spawn_sim = sim();
	if (spawn_sim.is_null()) {
		return;
	}
	const opennova::world::SpawnLoadoutInput input = loadout.is_valid()
			? loadout->engine_input()
			: opennova::world::SpawnLoadoutInput();
	const opennova::world::SpawnLoadoutPlan plan =
			opennova::world::spawn_loadout_plan(input, spawn_sim->has_explicit_spawn_loadout());
	if (plan.set_player_class) {
		spawn_sim->set_local_player_class(plan.player_class);
	}
	switch (plan.action) {
		case opennova::world::SpawnLoadoutAction::kNone:
			return;
		case opennova::world::SpawnLoadoutAction::kSyncInventory:
			sync_local_player_weapon_from_inventory(spawn_sim);
			return;
		case opennova::world::SpawnLoadoutAction::kApplyKit:
			break;
	}
	TypedArray<WeaponKitEntry> kit;
	for (const opennova::world::SpawnLoadoutKitRow &row : plan.kit) {
		kit.push_back(WeaponKitEntry::make(opennova::to_gd(row.name), row.clips));
	}
	if (!spawn_sim->apply_local_player_loadout(kit, plan.player_class)) {
		return;
	}
	if (plan.after_apply == opennova::world::SpawnLoadoutAfterApply::kClearWeapon) {
		clear_local_player_weapon();
		return;
	}
	sync_local_player_weapon_from_inventory(spawn_sim);
}

void LocalPlayerVisuals::sync_local_player_weapon_from_inventory(const Ref<Simulation> &p_sim) {
	const Ref<PlayerInventory> inventory = p_sim->get_local_player_inventory();
	if (inventory.is_null() || !inventory->get_valid()) {
		return;
	}
	const String equipped = inventory->get_equipped_name();
	// A syntactically nonempty kit can still be rejected by mission/class
	// rules. Keep the presentation aligned with the resulting authoritative
	// inventory.
	if (equipped.is_empty()) {
		clear_local_player_weapon();
		return;
	}
	set_local_player_weapon_by_name(equipped);
}

void LocalPlayerVisuals::clear_local_player_weapon() {
	viewmodel_weapon_override_ = String();
	viewmodel_weapon_cleared_ = true;
	local_weapon_.unref();
	viewmodel_def_name_ = String();
	viewmodel_def_.unref();
	local_weapon_preserve_slot_state_ = false;
	const Ref<Simulation> clear_sim = sim();
	if (clear_sim.is_valid()) {
		set_first_person_model_available(false);
		clear_sim->clear_local_player_weapon();
	}
}

Node3D *LocalPlayerVisuals::build_local_player_viewmodel() {
	const Ref<MissionObjectPlacer> mission_placer = placer();
	Node3D *parent = world_node3d();
	if (mission_placer.is_null() || parent == nullptr) {
		set_first_person_model_available(false);
		return nullptr;
	}
	if (viewmodel_weapon_cleared_) {
		set_first_person_model_available(false);
		return nullptr;
	}
	Node3D *container = memnew(Node3D);
	container->set_name("PlayerViewmodel");
	parent->add_child(container);
	// anim_wpn_idle = the FP holding pose; without it the arms sit in their
	// bind/T-pose. The environment: the viewmodel lights/fogs with the live
	// TOD like every entity (retail draws the FP model through the same
	// lighting constants [orig: Player_RenderFirstPersonViewModel @ 0x4ded60
	// -> the ctx block]).
	const Ref<PlayerViewmodelDef> def = local_player_viewmodel_def();
	// The submit spec (gun/arms/clip-adm + the emplaced arms omission)
	// resolves natively (renderer::fp_viewmodel_spec); the AK set is only the no-definition
	// bring-up fallback and a resolved def with no fpModel intentionally
	// submits no gun. [orig: Player_RenderFirstPersonViewModel @0x4ded60; @0x4dedc7]
	const Ref<PlayerVisualSpec> character_spec = local_player_visual_spec();
	const Ref<FpViewmodelSpec> spec = Simulation::fp_viewmodel_spec(def.is_valid(),
			def.is_valid() ? def->get_gfx1() : String(),
			character_spec.is_valid() ? character_spec->get_arms() : String(),
			def.is_valid() ? def->get_animadm() : String(), def.is_valid() ? def->get_flags() : 0);
	const String gun_name = spec->get_gun();
	const String arms_name = spec->get_arms();
	const String adm_name = spec->get_adm();
	const bool show_arms = spec->get_show_arms();
	// Both submits reuse the equipped GUN's model table, while `adm_name`
	// supplies the clips. Some valid retail sets differ (M21B_1st: 42 parts,
	// M21_1st: 40); sizing from the ADM basename truncates late animated parts
	// such as the M14 magazine. [orig: @0x4ded60]
	ObjectModel *arms = show_arms
			? mission_placer->build_model_from_graphic(arms_name, adm_name, container, "anim_wpn_idle", gun_name)
			: nullptr;
	ObjectModel *gun = !gun_name.is_empty()
			? mission_placer->build_model_from_graphic(gun_name, adm_name, container, "anim_wpn_idle", gun_name)
			: nullptr;
	local_viewmodel_parts_.clear();
	if (arms != nullptr) {
		// The arms' own raw camo triplet, stored by the rig's per-submit FP
		// writer alongside TEX_TEAM/HEAT_GLOW [orig: Avatar_SetArmsCamoCtrl
		// @0x57a3b0 immediately before each FP arms submit @0x4df008/@0x4df070].
		arms->set_avatar_part(ObjectModel::AVATAR_PART_ARMS);
		arms->set_graphic_name(arms_name);
		arms->set_avatar_camo(character_spec.is_valid() ? character_spec->get_arms_camo() : Vector3i());
		local_viewmodel_parts_.push_back(ObjectID(arms->get_instance_id()));
	}
	if (gun != nullptr) {
		local_viewmodel_parts_.push_back(ObjectID(gun->get_instance_id()));
	}
	set_first_person_model_available(gun != nullptr);
	if (show_arms && arms == nullptr) {
		UtilityFunctions::push_warning(
				vformat("GameWorld: FP arms model '%s' failed to load from the resource root", arms_name));
	}
	if (gun == nullptr && !gun_name.is_empty()) {
		UtilityFunctions::push_warning(
				vformat("GameWorld: FP gun model '%s' failed to load from the resource root", gun_name));
	}
	if (arms == nullptr && gun == nullptr) {
		// A valid definition with no resolved fpModel is a stable,
		// intentionally empty presentation epoch. Returning its container
		// prevents the caller from retrying every frame or substituting a
		// different weapon.
		if (def.is_null()) {
			container->queue_free();
			return nullptr;
		}
		return container;
	}
	// The FSM and its clip rings installed natively at ACCEPT time (S6b) --
	// the model resolve is purely presentational now, as in retail [orig: the
	// FP model resolve @0x4ded60 is a render consumer, not a mount].
	return container;
}

TypedArray<ObjectModel> LocalPlayerVisuals::local_player_viewmodel_parts() const {
	TypedArray<ObjectModel> out;
	for (const ObjectID &id : local_viewmodel_parts_) {
		if (ObjectModel *part = Object::cast_to<ObjectModel>(ObjectDB::get_instance(id))) {
			out.push_back(part);
		}
	}
	return out;
}

Ref<FirstPersonArmsWitness> LocalPlayerVisuals::local_player_first_person_arms_witness() {
	Ref<FirstPersonArmsWitness> witness;
	witness.instantiate();
	const Ref<PlayerVisualSpec> character_spec = local_player_visual_spec();
	const String expected_graphic = character_spec.is_valid() ? character_spec->get_arms() : String();
	const Vector3i expected_camo = character_spec.is_valid() ? character_spec->get_arms_camo() : Vector3i();
	for (const ObjectID &id : local_viewmodel_parts_) {
		ObjectModel *part = Object::cast_to<ObjectModel>(ObjectDB::get_instance(id));
		if (part == nullptr || part->get_avatar_part() != ObjectModel::AVATAR_PART_ARMS) {
			continue;
		}
		if (!part->is_visible_in_tree()) {
			witness->set_error("submitted first-person arms are not visible in tree");
			return witness;
		}
		const String actual_graphic = part->get_graphic_name();
		const Vector3i actual_camo = part->get_avatar_camo();
		if (actual_graphic != expected_graphic || actual_camo != expected_camo) {
			witness->set_error("submitted first-person arms do not match the resolved character");
			return witness;
		}
		witness->set_character_id(local_player_character_id());
		witness->set_arms_graphic(actual_graphic);
		PackedInt32Array camo;
		camo.push_back(actual_camo.x);
		camo.push_back(actual_camo.y);
		camo.push_back(actual_camo.z);
		witness->set_arms_camo(camo);
		return witness;
	}
	witness->set_error("no submitted first-person arms are available");
	return witness;
}

String LocalPlayerVisuals::local_player_weapon_name() const {
	return local_weapon_.is_valid() ? local_weapon_->get_name() : String();
}

void LocalPlayerVisuals::set_local_player_nvg_view(bool p_active, int p_gain) {
	if (MissionEnvironment *env = environment()) {
		env->set_nvg_view(p_active, p_gain);
	}
}

Ref<PlayerLocalView> LocalPlayerVisuals::local_player_view() const {
	const Ref<Simulation> view_sim = sim();
	if (view_sim.is_null()) {
		return Ref<PlayerLocalView>();
	}
	return view_sim->get_local_player_view();
}

Ref<PlayerWeaponView> LocalPlayerVisuals::local_player_weapon_view() const {
	const Ref<Simulation> view_sim = sim();
	if (view_sim.is_null()) {
		return Ref<PlayerWeaponView>();
	}
	const Ref<PlayerWeaponView> view = view_sim->get_local_player_weapon_state();
	return view.is_valid() && view->get_active() ? view : Ref<PlayerWeaponView>();
}

TypedArray<PlayerWeaponEvent> LocalPlayerVisuals::drain_local_player_weapon_events() {
	const Ref<Simulation> drain_sim = sim();
	if (drain_sim.is_null()) {
		return TypedArray<PlayerWeaponEvent>();
	}
	return drain_sim->drain_local_player_weapon_events();
}

Ref<PlayerViewmodelDef> LocalPlayerVisuals::local_player_viewmodel_def() {
	// Precedence: the armory-equipped (or debug-selected) weapon, else the
	// fixed default until first equip; NONE resolves nothing.
	const String fallback = Simulation::viewmodel_bringup_fallback_weapon();
	const opennova::world::ViewmodelDefPick pick = opennova::world::viewmodel_def_pick(
			viewmodel_weapon_cleared_, opennova::to_std(viewmodel_weapon_override_), opennova::to_std(fallback).c_str());
	if (!pick.resolves) {
		return Ref<PlayerViewmodelDef>();
	}
	const Ref<WeaponDatabase> weapon_db = weapon_database();
	if (weapon_db.is_null()) {
		return Ref<PlayerViewmodelDef>();
	}
	const String weapon_name = opennova::to_gd(pick.name);
	if (opennova::world::viewmodel_def_memo_hit(pick.name, opennova::to_std(viewmodel_def_name_), viewmodel_def_.is_valid())) {
		return viewmodel_def_;
	}
	const int index = weapon_db->find_weapon(weapon_name);
	if (index < 0) {
		UtilityFunctions::push_warning(vformat(
				"GameWorld: weapon '%s' not in weapon.def -- FP viewmodel keeps built-in defaults", weapon_name));
		return Ref<PlayerViewmodelDef>();
	}
	local_weapon_ = weapon_db->get_weapon(index);
	viewmodel_def_name_ = weapon_name;
	viewmodel_def_ = PlayerViewmodelDef::from_weapon_def(local_weapon_);
	return viewmodel_def_;
}

void LocalPlayerVisuals::_bind_methods() {
	ClassDB::bind_method(D_METHOD("setup", "world"), &LocalPlayerVisuals::setup);
	ClassDB::bind_method(D_METHOD("reset"), &LocalPlayerVisuals::reset);
	ClassDB::bind_method(D_METHOD("set_spawn_loadout", "loadout"), &LocalPlayerVisuals::set_spawn_loadout);
	ClassDB::bind_method(D_METHOD("apply_local_player_spawn_loadout"),
			&LocalPlayerVisuals::apply_local_player_spawn_loadout);
	ClassDB::bind_method(D_METHOD("set_local_player_weapon_by_name", "weapon_name", "preserve_slot_state"),
			&LocalPlayerVisuals::set_local_player_weapon_by_name, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("clear_local_player_weapon"), &LocalPlayerVisuals::clear_local_player_weapon);
	ClassDB::bind_method(D_METHOD("build_local_player_viewmodel"), &LocalPlayerVisuals::build_local_player_viewmodel);
	ClassDB::bind_method(D_METHOD("local_player_viewmodel_parts"),
			&LocalPlayerVisuals::local_player_viewmodel_parts);
	ClassDB::bind_method(D_METHOD("local_player_first_person_arms_witness"),
			&LocalPlayerVisuals::local_player_first_person_arms_witness);
	ClassDB::bind_method(D_METHOD("local_player_weapon_name"), &LocalPlayerVisuals::local_player_weapon_name);
	ClassDB::bind_method(D_METHOD("local_player_view"), &LocalPlayerVisuals::local_player_view);
	ClassDB::bind_method(D_METHOD("local_player_weapon_view"), &LocalPlayerVisuals::local_player_weapon_view);
	ClassDB::bind_method(D_METHOD("drain_local_player_weapon_events"),
			&LocalPlayerVisuals::drain_local_player_weapon_events);
	ClassDB::bind_method(D_METHOD("local_player_viewmodel_def"), &LocalPlayerVisuals::local_player_viewmodel_def);
	ClassDB::bind_method(D_METHOD("local_player_character_id"), &LocalPlayerVisuals::local_player_character_id);
}
