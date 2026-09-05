#pragma once

#include "render/render_view.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/viewport.hpp>

namespace godot {

// The one shared camera lookup for the runtime environment appliers (sky,
// water, celestial). Each node renders against its own scene viewport.
inline Camera3D *find_env_render_camera(Node *p_node) {
	if (p_node == nullptr) {
		return nullptr;
	}
	return RenderView::camera(p_node);
}

} // namespace godot
