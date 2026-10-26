#pragma once

// The game world's accessors as the player presenters reach them, re-resolved per use: a mission
// reload frees the runtime and its sim, so nothing caches them. The GameWorld's own C++ accessors
// first (no method-name call, no Variant return); a stand-in world (a test's node) answers by its
// methods' names. Include from .cpp files only (game_world.h includes the presenters' headers).

#include "world/game_world.h"

namespace godot::world_access {

inline GameWorld *game_world(Object *p_world) {
	return Object::cast_to<GameWorld>(p_world);
}

inline Ref<Simulation> sim(Object *p_world) {
	if (GameWorld *world = game_world(p_world)) return world->get_sim();
	return p_world != nullptr ? Ref<Simulation>(p_world->call("get_sim")) : Ref<Simulation>();
}

inline MissionRoot *runtime(Object *p_world) {
	if (GameWorld *world = game_world(p_world)) return world->get_runtime();
	return p_world != nullptr
			? Object::cast_to<MissionRoot>(static_cast<Object *>(p_world->call("get_runtime")))
			: nullptr;
}

inline bool loaded(Object *p_world) {
	if (GameWorld *world = game_world(p_world)) return world->is_loaded();
	return p_world != nullptr && static_cast<bool>(p_world->call("is_loaded"));
}

inline Ref<ItemDatabase> item_db(Object *p_world) {
	if (GameWorld *world = game_world(p_world)) return world->get_item_db();
	return p_world != nullptr ? Ref<ItemDatabase>(p_world->call("get_item_db")) : Ref<ItemDatabase>();
}

inline Ref<ResourceRoot> resource_root(Object *p_world) {
	if (GameWorld *world = game_world(p_world)) return world->get_resource_root();
	return p_world != nullptr ? Ref<ResourceRoot>(p_world->call("get_resource_root"))
							  : Ref<ResourceRoot>();
}

inline Ref<WeaponDatabase> weapon_database(Object *p_world) {
	if (GameWorld *world = game_world(p_world)) return world->get_weapon_database();
	return p_world != nullptr ? Ref<WeaponDatabase>(p_world->call("get_weapon_database"))
							  : Ref<WeaponDatabase>();
}

inline MissionEnvironment *environment(Object *p_world) {
	if (GameWorld *world = game_world(p_world)) return world->get_environment_node();
	return p_world != nullptr ? Object::cast_to<MissionEnvironment>(
										static_cast<Object *>(p_world->call("get_environment_node")))
							  : nullptr;
}

inline Ref<MissionData> loaded_mission(Object *p_world) {
	if (GameWorld *world = game_world(p_world)) return world->get_loaded_mission();
	return p_world != nullptr ? Ref<MissionData>(p_world->call("get_loaded_mission"))
							  : Ref<MissionData>();
}

inline EffectWorld *effect_world(Object *p_world) {
	if (GameWorld *world = game_world(p_world)) return world->get_effect_world();
	return p_world != nullptr
			? Object::cast_to<EffectWorld>(static_cast<Object *>(p_world->call("get_effect_world")))
			: nullptr;
}

inline MissionAudio *mission_audio(Object *p_world) {
	if (GameWorld *world = game_world(p_world)) return world->get_mission_audio();
	return p_world != nullptr
			? Object::cast_to<MissionAudio>(static_cast<Object *>(p_world->call("get_mission_audio")))
			: nullptr;
}

inline Ref<ItemEffectDirector> item_effect_director(Object *p_world) {
	if (GameWorld *world = game_world(p_world)) return world->get_item_effect_director();
	return p_world != nullptr ? Ref<ItemEffectDirector>(p_world->call("get_item_effect_director"))
							  : Ref<ItemEffectDirector>();
}

} // namespace godot::world_access
