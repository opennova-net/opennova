#pragma once

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/viewport.hpp>

namespace godot {

// Runtime nodes use their scene viewport. During a synchronous editor preview
// refresh these same appliers use the editor's camera in another SubViewport.
// Only the preview subtree sees the override; it cannot outlive the call.
// Godot device work is main-thread.
class RenderView {
	inline static thread_local const RenderView *active_ = nullptr;
	const RenderView *previous_;
	Node *root_;
	Camera3D *camera_;
public:
	RenderView(Node *p_root, Camera3D *p_camera) : previous_(active_), root_(p_root), camera_(p_camera) {
		active_ = this;
	}
	~RenderView() { active_ = previous_; }
	RenderView(const RenderView &) = delete;
	RenderView &operator=(const RenderView &) = delete;
	static bool is_preview(Node *p_node) {
		return active_ != nullptr && p_node != nullptr &&
				(active_->root_ == p_node || active_->root_->is_ancestor_of(p_node));
	}
	static Camera3D *camera(Node *p_node) {
		if (is_preview(p_node)) return active_->camera_;
		Viewport *viewport = p_node != nullptr && p_node->is_inside_tree() ? p_node->get_viewport() : nullptr;
		return viewport != nullptr ? viewport->get_camera_3d() : nullptr;
	}
};

} // namespace godot
