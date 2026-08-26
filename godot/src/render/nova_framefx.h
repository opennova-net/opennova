#pragma once

#include <memory>

#include <godot_cpp/classes/compositor_effect.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/render_data.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/rid.hpp>

namespace godot {

class Camera3D;
class Compositor;
class SubViewport;
class WorldEnvironment;

// The frame's terminal compositor effect: the post-transparent device leg
// that extracts bright/saturated beauty highlights, adds a compact bloom,
// and performs the sole display decode. The retained Q3 source methods keep
// decode-only/diagnostic callers compatible, but gameplay no longer opens a
// second scene camera for FrameFX.
class FrameFxCompositorEffect : public CompositorEffect {
	GDCLASS(FrameFxCompositorEffect, CompositorEffect)

private:
	class Impl;
	std::unique_ptr<Impl> impl_;

protected:
	static void _bind_methods();

public:
	FrameFxCompositorEffect();
	~FrameFxCompositorEffect() override;

	void set_scene_bloom_enabled(bool p_enabled);
	void set_q3_texture_rid(const RID &p_texture);
	void clear_q3_texture_rid();
	Dictionary get_backend_report() const;

	void _render_callback(int32_t p_effect_callback_type,
			RenderData *p_render_data) override;
};

// World-owned coordinator. Production bloom is extracted from the resolved
// beauty frame and blurred by the terminal compositor, avoiding a second
// shared-world scene render.
class FrameFx : public Node3D {
	GDCLASS(FrameFx, Node3D)

private:
	// The owning WorldEnvironment by identity: an embedder may free it before
	// this node leaves the tree (preview teardown), so never a raw pointer.
	ObjectID world_environment_id_;
	Ref<FrameFxCompositorEffect> terminal_effect_;
	Ref<Compositor> previous_compositor_;
	Ref<Compositor> installed_compositor_;
	ObjectID synced_camera_id_;
	uint32_t synced_camera_original_mask_ = 0;
	bool has_synced_camera_mask_ = false;

	void initialize_effect();
	void install_compositor();
	void uninstall_compositor();
	void restore_synced_camera_mask();

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	FrameFx();
	~FrameFx() override;

	// Ordered device leg, driven from GameFramePipeline immediately after the
	// local-view camera placement. This module must NOT self-clock: a node
	// process callback races the pipeline's camera producer.
	void advance_frame();

	SubViewport *get_q3_viewport() const { return nullptr; }
	Dictionary get_backend_report() const;
};

// The display decode for a 3D view that has no FrameFx (ONED
// workspace previews, the menu avatar preview, probes). Every engine shader
// writes gamma-domain numbers into the scene target and relies on exactly one
// terminal decode before Godot's sRGB output encode; without it a viewport
// shows the scene encoded twice. This node installs a decode-only
// FrameFxCompositorEffect (no Q3 source, so no FrameFX composite) on the
// nearest WorldEnvironment, or directly on the viewport's World3D when the
// view has none.
class DisplayDecode : public Node3D {
	GDCLASS(DisplayDecode, Node3D)

private:
	Ref<FrameFxCompositorEffect> effect_;
	// The owning WorldEnvironment by identity: an embedder may free it before
	// this node leaves the tree (preview teardown), so never a raw pointer.
	ObjectID world_environment_id_;
	Ref<World3D> world_;
	Ref<Compositor> previous_compositor_;
	Ref<Compositor> installed_compositor_;

	void install();
	void uninstall();

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	Dictionary get_backend_report() const;
};

} // namespace godot
