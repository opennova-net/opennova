#pragma once

// The depth-first WorldEnvironment search the render legs share (FrameFx,
// SlotShadow, the particle renderer): the first WorldEnvironment at or under
// `p_root`, else null.

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

} // namespace godot
