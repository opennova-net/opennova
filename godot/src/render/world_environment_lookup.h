#pragma once

// The WorldEnvironment lookups the render legs share: the depth-first search
// (FrameFx, SlotShadow, the particle renderer, DisplayDecode) for the first
// WorldEnvironment at or under `p_root`, else null; and one held by identity
// (FrameFx, DisplayDecode), null once freed.

#include <cstdint>

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/world_environment.hpp>
#include <godot_cpp/core/object.hpp>

namespace godot {

inline WorldEnvironment *find_world_environment(Node *p_root) {
	if (p_root == nullptr) {
		return nullptr;
	}
	if (WorldEnvironment *environment = Object::cast_to<WorldEnvironment>(p_root)) {
		return environment;
	}
	for (int i = 0; i < p_root->get_child_count(); ++i) {
		if (WorldEnvironment *environment = find_world_environment(p_root->get_child(i))) {
			return environment;
		}
	}
	return nullptr;
}

inline WorldEnvironment *world_environment_from_id(const ObjectID &id) {
	if (!id.is_valid())
		return nullptr;
	return Object::cast_to<WorldEnvironment>(
			ObjectDB::get_instance(static_cast<std::uint64_t>(id)));
}

} // namespace godot
