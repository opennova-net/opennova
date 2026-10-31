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
#include <runtime/renderer/material_eval.h>
#include <runtime/renderer/nvg_scope_lens.h>
#include <runtime/renderer/q3_frame.h>

#include "render/frame_fx_distortion.h"
#include "render/frame_fx_compositor_effect.h"
#include "render/nvg_view_device.h"

namespace godot {

class Camera3D;
class Compositor;
class Environment;
class GeometryInstance3D;
class ImageTexture;
class Material;
class Skeleton3D;
struct Q3ObjectMaterialValues;
class Texture;
class Texture2D;
class Viewport;
class WorldEnvironment;

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
	// The display decode table on the world's environment
	// (install_display_table) and the adjustments it replaced there.
	struct AdjustmentState {
		bool enabled = false;
		float brightness = 1.0f;
		float contrast = 1.0f;
		float saturation = 1.0f;
		Ref<Texture> color_correction;
	};
	Ref<ImageTexture> display_table_;
	Ref<Environment> table_environment_;
	AdjustmentState previous_adjustment_;
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
	// The world environment's tonemap takes the terminal display decode: its
	// adjustments at unity with the decode table as their colour correction.
	void install_display_table(const Ref<Environment> &p_environment);
	void uninstall_display_table();
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
	// Each material registers with its draw values (Q3ObjectMaterialValues:
	// the engine's evaluated effect parameters and its stage textures), and
	// its producer publishes every later write of them; the Q3 compile reads
	// those values, never the ShaderMaterial's parameters.
	static void register_q3_object_material(const Ref<Material> &p_material,
			const opennova::renderer::ObjectMaterialClassification &p_classification,
			const Q3ObjectMaterialValues &p_values);
	static void clone_q3_object_material(const Ref<Material> &p_source,
			const Ref<Material> &p_clone);
	// A producer wrote an object material's evaluated effect parameters (the
	// UV rows, SelfLumColor, AlphaGenValue, ReflectColor), or its Diffuse1
	// stage (an animated frame): the surfaces on it re-read their cached
	// block at their next sight. ObjectModel's runtime writes call these; a
	// stable material is never re-read per frame.
	static void update_q3_object_runtime(const Ref<Material> &p_material,
			const opennova::renderer::MaterialRuntime &p_runtime);
	static void update_q3_object_diffuse(const Ref<Material> &p_material,
			const Ref<Texture2D> &p_diffuse, float p_max_lod);
	// The update legs for a script driving a material directly (the windowed
	// device tests): the SelfLumColor alone, the AlphaGenValue alone, and the
	// Diffuse1 stage.
	static void set_q3_object_self_lum(const Ref<Material> &p_material,
			const Vector3 &p_self_lum);
	static void set_q3_object_alpha(const Ref<Material> &p_material, float p_alpha);
	static void set_q3_object_diffuse(const Ref<Material> &p_material,
			const Ref<Texture2D> &p_diffuse, float p_max_lod);
	// A water source's published block (the night-vision strip's).
	static void publish_q3_water(GeometryInstance3D *p_source,
			const opennova::renderer::Q3WaterMaterialParameters &p_water);
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
	// A celestial source's bloom-pass world transform: its model's parts
	// posed at the bloom pass's own register value, written by the producer
	// every frame (Q3SourceRecord::celestial_pose).
	static void set_q3_celestial_pose(GeometryInstance3D *p_source,
			const Transform3D &p_global_transform);
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
	// The renderer's start: the Q3 glow source's device shader, as the effect set's GLOW
	// passes load (engine renderer/object_shader_template.h carries the witness,
	// D-NET-1231).
	void create_glow_source_device();
	// The mission textures stage's view effects: the NVG view's device objects (engine
	// renderer/frame_fx_effects.h creates_view_effect_shaders carries the witness,
	// D-NET-1231).
	void init_view_effects();
	// The same stage's FrameFX pixel shaders: the terminal effect's shaders and samplers
	// (engine renderer/frame_fx_effects.h creates_frame_fx_pixel_shaders carries the witness,
	// D-NET-1231).
	void create_pixel_shaders();
	// The mission-texture init: the "ffscan" scanline texture, 2048 draws from
	// the render CRT stream (retail CFrameFX_CreatePixelShaders @0x58289f, from
	// Render_InitMissionTextures @0x587261).
	void init_mission_textures();
	// Whether the node holds a mission's "ffscan" texels (init_mission_textures ran).
	bool has_mission_textures() const { return !scanline_texels_.empty(); }
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

} // namespace godot
