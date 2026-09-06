#pragma once

#include <memory>

#include <godot_cpp/classes/compositor_effect.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/render_data.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/array.hpp>
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
	// F3-only GPU timing: RD timestamps around the pass carve its span out of
	// the root viewport's GPU row. capture_timestamp barriers the RD graph, so
	// this stays off unless the Stats capture is live.
	void set_gpu_timing_enabled(bool p_enabled);
	Dictionary get_backend_report() const;
	// Inspection seam for the GUT pins: the first view's focused Q3 colour
	// target as drawn by the last completed frame. Call only after
	// RenderingServer.force_sync(); an empty Ref means no target exists (no
	// RenderingDevice, no frame yet, or an unmapped attachment format).
	Ref<Image> capture_q3_target();

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
	// Latched so a compositor rebuild mid-capture re-applies the F3 timing flag.
	bool gpu_timing_enabled_ = false;

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
	// A producer rewrote one of an object material's Q3 parameters
	// (u_diffuse, u_detail, u_rgb_mod, u_alpha_mod, u_reflect_color, the UV
	// rows): the surfaces on it re-read their cached block at their next
	// sight. ObjectModel's runtime writes call this; a stable material is
	// never re-read per frame.
	static void invalidate_q3_object_material(const Ref<Material> &p_material);
	// An object surface instance with the material it currently draws: a
	// glow-capable material registers it (a node registered before keeps its
	// record, its swapped mesh is re-read once); any other material, or
	// unregister_q3_source, parks the record inactive without losing its
	// generations (the ObjectModel level swap re-registers the same node
	// many times).
	static void register_q3_object_source(GeometryInstance3D *p_source,
			const Ref<Material> &p_material);
	static void unregister_q3_source(GeometryInstance3D *p_source);
	// The classification an object material was registered with (false and
	// untouched for a material outside the registry): the typed blend/family
	// facts another producer may need about an ObjectModel surface.
	static bool q3_object_material_classification(const Ref<Material> &p_material,
			opennova::renderer::ObjectMaterialClassification &r_classification);
	// Water/celestial sources; `p_additive_surfaces` (bit i = surface i)
	// carries the blend the celestial installed per surface, which the Q3
	// disc draw follows.
	static void register_q3_source(GeometryInstance3D *p_source,
			opennova::renderer::Q3Source p_kind,
			uint32_t p_additive_surfaces = 0);
	// Producers that already hold a surface's CPU arrays publish them here
	// (the water strip every frame), so the Q3 geometry cache re-packs from
	// memory instead of reading the mesh back through the server. A producer
	// that rebuilt (or cleared) a registered mesh invalidates the source (its
	// surface list is re-read, its arrays re-read once at the next sight);
	// one that only rewrote a MultiMesh's instance transforms (a static RLOD
	// switch, a destruction carve) invalidates its instances, which re-reads
	// the rows and the population bounds and never the packed surfaces.
	static void publish_q3_geometry(GeometryInstance3D *p_source, int p_surface,
			const Array &p_arrays);
	static void invalidate_q3_source(GeometryInstance3D *p_source);
	static void invalidate_q3_instances(GeometryInstance3D *p_source);

	// Ordered device leg, driven from the GameWorld leg table immediately after the
	// local-view camera placement. This module must NOT self-clock: a node
	// process callback races the pipeline's camera producer, and a Q3 frame
	// compiled from last frame's pose composites stale glow over the
	// current beauty frame.
	void advance_frame();
	// Process-exit boundary: stop render callbacks and release compositor-owned
	// device resources while RenderingServer and RenderingDevice are still live.
	void shutdown();
	// F3 Stats capture toggle for the terminal pass's RD GPU span (see
	// FrameFxCompositorEffect::set_gpu_timing_enabled).
	void set_gpu_timing_enabled(bool p_enabled);

	Dictionary get_backend_report() const;
	// The terminal effect's focused Q3 target (see
	// FrameFxCompositorEffect::capture_q3_target); empty without one.
	Ref<Image> get_q3_target_image() const;
};

// The display decode for a 3D view that has no FrameFx (the editor
// world preview, the menu avatar preview, probes). Every engine shader
// writes gamma-domain numbers into the scene target and relies on exactly one
// terminal decode before Godot's sRGB output encode; without it a viewport
// shows the scene encoded twice. This node installs a decode-only
// FrameFxCompositorEffect (no Q3 source, so no FrameFX composite) on the
// nearest WorldEnvironment, or directly on the viewport's World3D when the
// view has none.
class DisplayDecode : public Node3D {
	GDCLASS(DisplayDecode, Node3D)

public:
	// Build a decoder for a caller-owned camera without changing scene resources.
	Ref<Compositor> create_view_compositor(const Ref<Compositor> &p_base = Ref<Compositor>());


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
