#include "world/world_view.h"

#include <godot_cpp/core/object.hpp>

#include "world/game_world.h"

using namespace godot;

// --- WorldView ---------------------------------------------------------------

Ref<Simulation> WorldView::sim() {
	Ref<Simulation> out;
	if (GDVIRTUAL_CALL(_sim, out)) {
		return out;
	}
	return sim_impl();
}

Ref<ResourceRoot> WorldView::resource_root() {
	Ref<ResourceRoot> out;
	if (GDVIRTUAL_CALL(_resource_root, out)) {
		return out;
	}
	return resource_root_impl();
}

void WorldView::_bind_methods() {
	GDVIRTUAL_BIND(_sim);
	GDVIRTUAL_BIND(_resource_root);
	ClassDB::bind_method(D_METHOD("sim"), &WorldView::sim);
	ClassDB::bind_method(D_METHOD("resource_root"), &WorldView::resource_root);
}

// --- ArmoryWorldView ---------------------------------------------------------

Ref<WeaponDatabase> ArmoryWorldView::weapon_database() {
	Ref<WeaponDatabase> out;
	if (GDVIRTUAL_CALL(_weapon_database, out)) {
		return out;
	}
	return weapon_database_impl();
}

Ref<PlayerViewmodelDef> ArmoryWorldView::local_player_viewmodel_def() {
	Ref<PlayerViewmodelDef> out;
	if (GDVIRTUAL_CALL(_local_player_viewmodel_def, out)) {
		return out;
	}
	return local_player_viewmodel_def_impl();
}

bool ArmoryWorldView::set_local_player_weapon_by_name(const String &p_weapon_name,
		bool p_preserve_slot_state) {
	bool out = false;
	if (GDVIRTUAL_CALL(_set_local_player_weapon_by_name, p_weapon_name, p_preserve_slot_state, out)) {
		return out;
	}
	return set_local_player_weapon_by_name_impl(p_weapon_name, p_preserve_slot_state);
}

void ArmoryWorldView::clear_local_player_weapon() {
	if (GDVIRTUAL_CALL(_clear_local_player_weapon)) {
		return;
	}
	clear_local_player_weapon_impl();
}

void ArmoryWorldView::_bind_methods() {
	GDVIRTUAL_BIND(_weapon_database);
	GDVIRTUAL_BIND(_local_player_viewmodel_def);
	GDVIRTUAL_BIND(_set_local_player_weapon_by_name, "weapon_name", "preserve_slot_state");
	GDVIRTUAL_BIND(_clear_local_player_weapon);
	ClassDB::bind_method(D_METHOD("weapon_database"), &ArmoryWorldView::weapon_database);
	ClassDB::bind_method(D_METHOD("local_player_viewmodel_def"),
			&ArmoryWorldView::local_player_viewmodel_def);
	ClassDB::bind_method(D_METHOD("set_local_player_weapon_by_name", "weapon_name",
			"preserve_slot_state"), &ArmoryWorldView::set_local_player_weapon_by_name,
			DEFVAL(false));
	ClassDB::bind_method(D_METHOD("clear_local_player_weapon"),
			&ArmoryWorldView::clear_local_player_weapon);
}

// --- The live views ----------------------------------------------------------

void LiveWorldView::bind(GameWorld *p_world) {
	world_id_ = p_world != nullptr ? ObjectID(p_world->get_instance_id()) : ObjectID();
}

GameWorld *LiveWorldView::world() const {
	return Object::cast_to<GameWorld>(ObjectDB::get_instance(world_id_));
}

Ref<Simulation> LiveWorldView::sim_impl() const {
	GameWorld *w = world();
	return w != nullptr ? w->get_sim() : Ref<Simulation>();
}

Ref<ResourceRoot> LiveWorldView::resource_root_impl() const {
	GameWorld *w = world();
	return w != nullptr ? w->get_resource_root() : Ref<ResourceRoot>();
}

void LiveArmoryWorldView::bind(GameWorld *p_world) {
	world_id_ = p_world != nullptr ? ObjectID(p_world->get_instance_id()) : ObjectID();
}

GameWorld *LiveArmoryWorldView::world() const {
	return Object::cast_to<GameWorld>(ObjectDB::get_instance(world_id_));
}

Ref<Simulation> LiveArmoryWorldView::sim_impl() const {
	GameWorld *w = world();
	return w != nullptr ? w->get_sim() : Ref<Simulation>();
}

Ref<ResourceRoot> LiveArmoryWorldView::resource_root_impl() const {
	GameWorld *w = world();
	return w != nullptr ? w->get_resource_root() : Ref<ResourceRoot>();
}

Ref<WeaponDatabase> LiveArmoryWorldView::weapon_database_impl() const {
	GameWorld *w = world();
	return w != nullptr ? w->get_weapon_database() : Ref<WeaponDatabase>();
}

Ref<PlayerViewmodelDef> LiveArmoryWorldView::local_player_viewmodel_def_impl() const {
	GameWorld *w = world();
	return w != nullptr ? w->local_player_viewmodel_def() : Ref<PlayerViewmodelDef>();
}

bool LiveArmoryWorldView::set_local_player_weapon_by_name_impl(const String &p_weapon_name,
		bool p_preserve_slot_state) {
	GameWorld *w = world();
	return w != nullptr && w->set_local_player_weapon_by_name(p_weapon_name, p_preserve_slot_state);
}

void LiveArmoryWorldView::clear_local_player_weapon_impl() {
	GameWorld *w = world();
	if (w != nullptr) {
		w->clear_local_player_weapon();
	}
}
