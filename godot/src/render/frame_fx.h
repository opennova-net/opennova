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

#include <runtime/renderer/q3_frame.h>

namespace godot {

class Camera3D;
class Compositor;
class GeometryInstance3D;
class Material;
class Viewport;
class WorldEnvironment;

// The frame's terminal compositor effect: the post-transparent device leg
// that composites the Q3 glow source and performs the sole display decode
// (FrameFX_RenderBloomPass @0x582940; targets create_frame_effect_render_targets @0x583c40;
// capture FrameFX_CaptureRenderTarget @0x584020 - docs/render/render-order-re.md).
// The Q3 source is an effect-owned full-resolution color target sharing the
// resolved beauty depth. The effect consumes Q3FrameCompiler's immutable draw
// list, runs the POT capture, 256x256 two-axis weighted blur, half-strength
// additive composite, and the final gamma->linear bridge.
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

	void compile_q3_frame(Node *p_scope, Viewport *p_viewport,
			Camera3D *p_camera);
	void clear_q3_frame();
	void release_device_resources();
	Dictionary get_backend_report() const;

	void _render_callback(int32_t p_effect_callback_type,
			RenderData *p_render_data) override;
};

// World-owned coordinator. The terminal compositor owns the sole focused Q3
// renderer and its beauty-depth target. Producers register typed scene
// sources; no auxiliary view, camera mask, or source RID exists.
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
	bool shutdown_ = false;

	void build_compositor();
	void install_compositor();
	void uninstall_compositor();
	void restore_synced_camera_mask();

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	FrameFx();
	~FrameFx() override;

	// Typed producer registry. Object materials are classified once at their
	// authoritative creation seam; visible geometry then publishes only a
	// material identity. Explicit water/celestial sources never infer a Q3
	// technique from shader names or device state.
	static void register_q3_object_material(const Ref<Material> &p_material,
			const opennova::renderer::ObjectMaterialClassification &p_classification);
	static void clone_q3_object_material(const Ref<Material> &p_source,
			const Ref<Material> &p_clone);
	static void register_q3_object_source(GeometryInstance3D *p_source,
			const Ref<Material> &p_material);
	static void register_q3_source(GeometryInstance3D *p_source,
			opennova::renderer::Q3Source p_kind);
	// A registered source's instance data was rewritten in place (the
	// placer's per-instance static RLOD switches): forward the source's
	// generation bump to the Q3 adapter.
	static void invalidate_q3_source(GeometryInstance3D *p_source);

	// Ordered device leg, driven from GameFramePipeline immediately after the
	// local-view camera placement. This module must NOT self-clock: a node
	// process callback races the pipeline's camera producer, and a Q3 frame
	// compiled from last frame's pose composites stale glow over the
	// current beauty frame.
	void advance_frame();
	// Process-exit boundary: stop render callbacks and release compositor-owned
	// device resources while RenderingServer and RenderingDevice are still live.
	void shutdown();

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
