#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <godot_cpp/classes/compositor_effect.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/render_data.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <runtime/renderer/frame_fx_effects.h>
#include <runtime/renderer/nvg_scope_lens.h>
#include <runtime/renderer/q3_frame.h>

#include "render/frame_fx_distortion.h"
#include "render/nvg_view_device.h"

namespace godot {

class Camera3D;
class Compositor;
class GeometryInstance3D;
class Material;
class Skeleton3D;
class Texture2D;
class Viewport;
class WorldEnvironment;

// One frame's FrameFX screen-effect plan as the terminal effect consumes it:
// the planner's rows (runtime/renderer/frame_fx_effects.h), the effects
// device that draws the type-0 distortion sets, on the NVG lens arm the lens
// over the surface's overlay rect (runtime/renderer/nvg_scope_lens.h) with
// the surface size its pixels span, and on the NVG Sighted arm the SIGHTS
// card's rows in the NVG scene's pixels.
struct FrameFxScreenFrame {
	std::uint64_t frame_id = 0;
	opennova::renderer::FrameFxFramePlan plan;
	std::shared_ptr<FrameFxDistortionDrawer> distortion;
	std::shared_ptr<const opennova::renderer::NvgScopeLens> lens;
	Vector2i screen_size;
	std::vector<NvgViewDevice::SightsRow> nvg_sights;
};

// The frame's terminal compositor effect: the post-transparent device leg
// that runs retail's FrameFX over the finished scene and performs the sole
// display decode. It executes the planner's DrawPass rows in dispatch order
// (distortion, death or damage blur, the bloom, thermal and monitor) or, in
// the first-person NVG view, the NVG scene/glow/composite chain in their
// place (Render_ProcessMainSceneFrame @0x5ca8f6..0x5caad5 / @0x5ca6ab; the
// bloom kernel FrameFX_BloomKernel @0x5841d0 over the Q3 source FrameFX_RenderGlowSource
// @0x582940 draws and FrameFX_CaptureAltBuffer @0x584020 captures; targets
// create_frame_effect_render_targets @0x583c40).
// The Q3 source is an effect-owned full-resolution color target sharing the
// resolved beauty depth; the capture is the power-of-two floor of the frame.
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
	// Main thread: the frame's screen-effect plan, consumed by the next render
	// (a frame's one-shot parts, the NVG glow clear, run once per frame id).
	void publish_screen_effects(const std::shared_ptr<const FrameFxScreenFrame> &p_frame);
	// Main thread: the mission's "ffscan" texels (frame_fx_scanline_texels).
	void publish_scanline_texels(const std::vector<std::uint8_t> &p_texels);
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
	// The screen-effect planner's inputs and cross-frame state.
	opennova::renderer::FrameFxViewInputs view_effects_;
	opennova::renderer::FrameFxPlannerState planner_state_;
	std::shared_ptr<FrameFxDistortionDrawer> distortion_drawer_;
	std::uint64_t screen_frame_id_ = 0;
	std::vector<std::uint8_t> scanline_texels_;
	// The NVG lens over the last surface size it was built for.
	std::shared_ptr<const opennova::renderer::NvgScopeLens> nvg_lens_;
	Vector2i nvg_lens_size_;
	// The SIGHTS card rows the NVG Sighted arm draws into the scene.
	std::vector<NvgViewDevice::SightsRow> nvg_sights_;

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
	// many times). A strip whose skin binds every vertex to one bone (the
	// first-person gun's rigid parts) names that skeleton and bone, so its
	// copy draws at the bone's part matrix.
	static void register_q3_object_source(GeometryInstance3D *p_source,
			const Ref<Material> &p_material, Skeleton3D *p_rigid_skeleton = nullptr,
			int p_rigid_bone = -1);
	static void unregister_q3_source(GeometryInstance3D *p_source);
	// A celestial source's bloom-pass SelfLumColor (runtime/renderer/q3_frame.h
	// Q3CelestialMaterialParameters), written by the producer every frame.
	static void set_q3_celestial_self_lum(GeometryInstance3D *p_source,
			const Vector3 &p_self_lum);
	// The classification an object material was registered with (false and
	// untouched for a material outside the registry): the typed blend/family
	// facts another producer may need about an ObjectModel surface.
	static bool q3_object_material_classification(const Ref<Material> &p_material,
			opennova::renderer::ObjectMaterialClassification &r_classification);
	// Water/celestial sources. A celestial surface draws its authored
	// material, whose registered classification sets the Q3 blend.
	static void register_q3_source(GeometryInstance3D *p_source,
			opennova::renderer::Q3Source p_kind);
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
	// The frame's FrameFX view facts (the local player's LocalPlayerViewFrame
	// ::frame_fx; defaults with no local player). The GDScript overload is the
	// same seam with the fields spelled out.
	void set_view_effects(const opennova::renderer::FrameFxViewInputs &p_view);
	void set_view_effects_values(int p_red_word, int p_camera_mode, bool p_local_dead,
			bool p_in_session, int p_death_elapsed_ticks, bool p_thermal_view,
			bool p_monitor_view, bool p_nvg_active, bool p_death_screen_active,
			bool p_binoculars_view_active, bool p_scoped_selector, bool p_sighted_selector);
	// The equipped SIGHTS card for the NVG Sighted arm: each row's texture,
	// its rect in the 512-square NVG scene's pixels (x1, y1, x2, y2 per row;
	// HudPos.nvg_scene_sight_rect) and its DefSightBlendMode, in draw order.
	// The shell's card publishes it; the rows draw only on that arm.
	void set_nvg_sights_card(const TypedArray<Texture2D> &p_textures,
			const PackedFloat32Array &p_rects, const PackedInt32Array &p_blends);
	// The effects device that draws the type-0 distortion sets; null = none.
	void set_distortion_drawer(const std::shared_ptr<FrameFxDistortionDrawer> &p_drawer);
	// Ordered device leg after the particle and precipitation legs: plan the
	// frame's screen effects from the view facts, the distortion drawer's
	// content and the millisecond clock, drawing from the render CRT stream,
	// and publish the plan to the terminal effect (with the NVG lens over this
	// node's viewport, the surface, on the lens arm).
	void advance_screen_effects();
	// The mission-texture init: the "ffscan" scanline texture, 2048 draws from
	// the render CRT stream (retail CFrameFX_CreatePixelShaders @0x58289f, from
	// Render_InitMissionTextures @0x587261).
	void init_mission_textures();
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

// The display decode for a 3D view that has no FrameFx (the menu avatar
// preview, probes). Every engine shader
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
