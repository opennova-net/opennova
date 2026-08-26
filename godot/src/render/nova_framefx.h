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
// that composites the Q3 glow source and performs the sole display decode
// (FrameFX_RenderBloomPass @0x582940; targets create_frame_effect_render_targets @0x583c40;
// capture FrameFX_CaptureRenderTarget @0x584020 - docs/render/render-order-re.md).
// The source RID is the isolated Q3 view owned by FrameFx; the effect runs
// the POT capture, 256x256 two-axis weighted blur, half-strength additive
// composite, and the final gamma->linear bridge.
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

	void set_q3_texture_rid(const RID &p_texture);
	void clear_q3_texture_rid();
	Dictionary get_backend_report() const;

	void _render_callback(int32_t p_effect_callback_type,
			RenderData *p_render_data) override;
};

// World-owned coordinator. One private shared-world view owns the isolated
// Q3 (glow/envmap duplicate) pass at FrameFX's working size; the terminal
// compositor effect installed on the shared WorldEnvironment consumes it.
// Callers publish no pass plumbing: this module installs the terminal
// FrameFX compositor effect and reports through get_backend_report().
class FrameFx : public Node3D {
	GDCLASS(FrameFx, Node3D)

private:
	SubViewport *q3_viewport_ = nullptr;
	Camera3D *q3_camera_ = nullptr;
	// The owning WorldEnvironment by identity: an embedder may free it before
	// this node leaves the tree (preview teardown), so never a raw pointer.
	ObjectID world_environment_id_;
	Ref<FrameFxCompositorEffect> terminal_effect_;
	Ref<Compositor> previous_compositor_;
	Ref<Compositor> installed_compositor_;
	ObjectID synced_camera_id_;
	uint32_t synced_camera_original_mask_ = 0;
	bool has_synced_camera_mask_ = false;

	void build_auxiliary_views();
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
	// process callback races the pipeline's camera producer, and a Q3 view
	// rendered from last frame's pose composites a stale glow over the
	// current beauty frame.
	void advance_frame();

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
};

} // namespace godot
