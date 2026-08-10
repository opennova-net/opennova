#pragma once

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/viewport.hpp>

namespace godot {

// The one shared camera lookup for the environment appliers (sky, water,
// celestial) — the C++ port of env_render_camera.gd. In editor context the
// sanctioned editor-aware nodes render live in ONED's workspaces, so the
// editor's 3D viewport camera wins; otherwise the node's own scene viewport
// camera. The dynamic-singleton guard keeps template (non-editor) builds on
// the runtime path.
inline Camera3D *find_env_render_camera(Node *p_node) {
	if (p_node == nullptr) {
		return nullptr;
	}
	Engine *engine = Engine::get_singleton();
	if (engine->is_editor_hint() && engine->has_singleton("EditorInterface")) {
		EditorInterface *editor_interface = Object::cast_to<EditorInterface>(
				engine->get_singleton("EditorInterface"));
		if (editor_interface != nullptr) {
			SubViewport *viewport = editor_interface->get_editor_viewport_3d(0);
			if (viewport != nullptr) {
				return viewport->get_camera_3d();
			}
		}
		return nullptr;
	}
	Viewport *viewport = p_node->get_viewport();
	return viewport != nullptr ? viewport->get_camera_3d() : nullptr;
}

} // namespace godot
