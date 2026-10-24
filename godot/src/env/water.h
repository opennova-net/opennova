#pragma once

#include "render/visual_layers.h"

#include <cmath>
#include <memory>

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <runtime/environment/water_frame.h>
#include <runtime/environment/water_mirror.h>

#include "env/water_core.h"
#include "terrain/terrain_data.h"

namespace godot {

class Compositor;
class EnvFile;
class Environment;
class FrameFxCompositorEffect;
class MissionEnvironment;

// The water-plane applier — the ADR 0033 device leg over the engine's
// witnessed water pieces: the height precedence ladder + per-frame inputs
// (environment/water_frame.h), the proper-mirror reflection view
// (environment/water_mirror.h), and the noise/strip math already in
// engine/formats/env behind the WaterCore device helper. This node keeps only
// device work: the live ArrayMesh strip upload, the animated noise
// ImageTexture pair, the reflection SubViewport + mirror Camera3D rig on the
// SAME World3D (Godot renders SubViewports ahead of the sampling viewport,
// preserving the witnessed prerender order), shader-parameter pushes, the
// visual-layer allocation, and the ObjectShaderCache water-split
// publication. Ported from water.gd (2026-08-10 de-scripting);
// RE record: docs/env/env-tod-re.md (env #28/#29/#30).
class Water : public Node3D {
	GDCLASS(Water, Node3D)

public:
	// The visual-layer allocation is the renderer's contract
	// (render/visual_layers.h); these names are the GDScript-visible aliases
	// the reflection contract (env #30) exposes on the water node: above
	// water the mirror renders only the flag-0x400 population (vehicles by
	// item type + authored-Reflective records; no person); below water it is
	// unfiltered, persons included. It never renders the water surface or the
	// FP overlay.
	// The witness lives with the mirror view (environment/water_mirror.h).
	enum {
		VISUAL_LAYER_WORLD = visual_layers::WORLD,
		VISUAL_LAYER_WATER = visual_layers::WATER,
		VISUAL_LAYER_ENVIRONMENT_CAPTURE = visual_layers::ENVIRONMENT_CAPTURE,
		VISUAL_LAYER_VIEWMODEL = visual_layers::VIEWMODEL,
		VISUAL_LAYER_FP_BODY_SHADOW_ONLY = visual_layers::FP_BODY_SHADOW_ONLY,
		VISUAL_LAYER_STATIC_SHADOW_CASTER = visual_layers::STATIC_SHADOW_CASTER,
		VISUAL_LAYER_DYNAMIC_SHADOW_CASTER = visual_layers::DYNAMIC_SHADOW_CASTER,
		VISUAL_LAYER_TERRAIN_SHADOW_RECEIVER = visual_layers::TERRAIN_SHADOW_RECEIVER,
		VISUAL_LAYER_WORLD_NO_MIRROR = visual_layers::WORLD_NO_MIRROR,
		VISUAL_LAYER_TERRAIN_FOLIAGE = visual_layers::TERRAIN_FOLIAGE,
		VISUAL_LAYER_TERRAIN_FLAT_FALLBACK = visual_layers::TERRAIN_FLAT_FALLBACK,
		VISUAL_LAYER_SHADOW_CASTER_MASK = visual_layers::SHADOW_CASTER_MASK,
		REFLECTION_CULL_MASK = visual_layers::REFLECTION_CULL_MASK,
		VISUAL_LAYER_MIRROR_VIEW = visual_layers::MIRROR_VIEW,
		VISUAL_LAYER_INSET_MIRROR_VIEW = visual_layers::INSET_MIRROR_VIEW,
	};

	void set_environment_path(const NodePath &p_path);
	NodePath get_environment_path() const { return environment_path_; }
	void set_terrain_data(const Ref<TerrainData> &p_data);
	Ref<TerrainData> get_terrain_data() const { return terrain_data_; }
	void set_water_height(float p_value);
	float get_water_height() const { return water_height_; }
	float get_water_alpha() const { return water_alpha_; }

	// The mission/BMS rung in world units; NAN means absent. Zero is
	// meaningful and still beats TRN and ENV.
	void set_mission_water_height_override(float p_value);
	// Disable retained runtime rendering while no world is loaded.
	void set_world_rendering_enabled(bool p_value);
	// The mirror pass on or off, the surface drawn either way: an editor device renders its mirror
	// only in the frames it presents (ADR 0046 S14, E13), so a picture no canvas draws this frame
	// spends no mirror pass. On in the game, always.
	void set_mirror_enabled(bool p_value);
	bool is_mirror_enabled() const { return mirror_enabled_; }
	// The water's process-wide globals as a process has them with no water (the
	// active flag, the height, the shader cache's plane): what a picture with no
	// water (a model's) renders under after a mission's picture published its
	// own (ADR 0046 S14, E13).
	static void publish_absent();
	// The hold (ADR 0046 S14, E13), as MissionEnvironment's: while held, the water writes none of its
	// process-wide globals (the active flag, the height, the shader cache's plane); its node state
	// still moves. The editor's mission device releases it to publish and to present its own frame.
	void set_globals_held(bool p_held) { globals_held_ = p_held; }
	bool is_globals_held() const { return globals_held_; }
	// This water's globals written again from its state whatever the hold
	// (its render activity untouched); an unbuilt water writes the absent set.
	void republish_globals();
	// How many times a water wrote its process-wide globals since the process began, one count per
	// value written (each shader global, the shader cache's water plane): a read-back for the
	// hold's tests (a headless renderer keeps no global to read back).
	static int64_t get_global_writes() { return global_writes_; }
	// Irreversibly drop the retained water renderer graph during process
	// exit. Normal world unload deliberately keeps this graph warm for the
	// next mission; SceneTree teardown is too late because shell-owned
	// ImageTextures can otherwise outlive RenderingServer/GDExtension
	// deinitialization.
	void release_runtime_renderer_resources();
	// The reflection target's first render, in the load's one forced frame: the mirror view
	// through the view p_camera draws (the frame's own install, its projection served) and
	// the target armed for that draw alone, so its render buffers, its pass's pipelines and
	// the projection interface's first serve are made in the load, as retail makes its
	// reflection target and surfaces with the mission start's render subsystems. A mission
	// with no water, a mirror switched off or no camera arms nothing. Returns whether it armed.
	bool warm_reflection_target(Camera3D *p_camera);

	// g_EnvWaterHeightFixed == 0 is retail's no-water sentinel for the water's
	// own draws alone: the surface, the mirror (its clip arm with it) and the
	// glint (engine environment/water_frame.h water_draws_at_height carries the
	// witness). Signed nonzero heights remain valid for terrain below the world
	// origin.
	bool is_water_active() const {
		return opennova::env::water_draws_at_height(water_height_);
	}
	// The authored height can remain valid while the world retains this node
	// between loads; render-frame participation must use this predicate.
	bool is_water_render_active() const {
		return world_rendering_enabled_ && is_water_active();
	}
	// The waterline the scene compares against stands at every height, zero
	// included: the transparent split (renderer/render_order.h) and the eye's
	// side of it (the pass fog, the clear, the sky bracket, the murk;
	// EnvironmentState::classify_render_eye) read no sentinel. Render-frame
	// participation still follows the owner's lifecycle.
	bool is_waterline_active() const { return world_rendering_enabled_; }
	// The frame's tracked visible-terrain bounds and the water pass latch
	// feed the engine's g_WaterActive predicate
	// (terrain::water_pass_active, engine/runtime/terrain/quadtree.h).
	void set_visible_terrain_bounds(bool p_valid, float p_min_height,
			float p_max_height);
	// The water pass latch every scene's g_WaterActive reads, one word for the
	// main scene and the weapon Inset's (engine environment/water_frame.h
	// water_pass_latch carries the witness): set outright here, or stored by a
	// collect's water pass below.
	void set_blink_water_visible(bool p_visible);
	bool get_blink_water_latch() const { return blink_water_visible_; }
	// A scene's water pass: past its water letter (p_water_pass_runs) it stores
	// its collect's verdict, the main scene's and then the Inset's (for the next
	// frame's main scene).
	void latch_blink_water(bool p_water_pass_runs, bool p_visible);
	// The Inset scene's water-active test reads the latch as the main scene's
	// water pass left it this frame: open it ahead of that test (and the Inset
	// mirror's collect), before the Inset's own pass stores through
	// latch_blink_water.
	void open_inset_water_latch();
	// Each view's water letter label (renderer::scene_water_visible), over the
	// g_BlinkWaterVisible word as its scene core reads it ahead of its own
	// collect: its mirror's collect's, else the last scene's (OcclusionFrame;
	// engine: world/occlusion.h OcclusionWorld::mirror_water_visible). A label
	// gates its view's strip alone: the mirrors render on their g_WaterActive
	// and the bloom's redraw draws on its side whatever the labels say. While
	// the Inset pass draws (advance_inset_frame) each view has its own strip:
	// the main view's on MAIN_VIEW_WATER (the beauty camera admits it, neither
	// the Inset nor either mirror mask does), the Inset's on its own bit; with
	// the Inset down the main label gates the one strip on the water layer. A
	// view whose label is down draws no strip. (p, p) with the Inset down sets
	// the main view's label.
	void set_blink_water_views(bool p_main, bool p_inset);
	bool is_main_view_water() const { return main_view_water_; }
	bool is_water_pass_active() const;
	// The weapon Inset pass's own water (engine environment/water_mirror.h
	// inset_mirror_source carries the witness): its g_WaterActive over its own
	// view's visible terrain bounds and the latch as the main collect left it,
	// then its mirror of the view block p_view_block (the Inset's compose
	// without its slot offsets) at its camera's fov and aspect 1.0, and its own
	// strip marched through p_camera (the Inset camera, offsets in) sampling
	// that mirror. After the main scene's water (advance_frame); null retires
	// the Inset's water.
	void set_inset_visible_terrain_bounds(bool p_valid, float p_min_height,
			float p_max_height);
	bool is_inset_water_pass_active() const;
	void advance_inset_frame(Camera3D *p_camera, const Transform3D &p_view_block);
	bool is_inset_mirror_rendered() const { return inset_mirror_rendered_; }
	// The world's entity-update counter, the noise pair's frame counter (the
	// one retail's noise generator reads; witness in advance_frame): a frame
	// whose world held its entity update regenerates the same pair. A Water
	// nobody feeds counts its own render frames.
	void set_noise_frame_counter(uint32_t p_counter);
	int get_noise_frame_counter() const { return frame_counter_; }
	// The mirror's outdoors flag (renderer::ScenePassGates::mirror_sky, off
	// under the indoors blink letter), fed from the scene-pass gates; it
	// selects the mirror target's own clear (EnvironmentState::
	// water_mirror_clear_color), which the mirror camera's BG_COLOR
	// environment carries while a loaded MissionEnvironment exists.
	void set_mirror_scene_outdoors(bool p_outdoors);
	bool is_mirror_scene_outdoors() const { return mirror_scene_outdoors_; }

	void build();
	bool is_built() const { return built_; }
	Ref<ShaderMaterial> get_water_material() const { return water_material_; }
	// The live per-frame-regenerated noise color texture, pulled by the terrain
	// material for the below-water modulation (D-TERRAIN-8,
	// docs/terrain/terrain-re.md underwater section — terrain pulls from the
	// water module, matching the retail data direction). Null before build().
	Ref<Texture2D> get_noise_color_texture() const { return noise_color_tex_; }
	MeshInstance3D *get_mesh_instance() const { return mesh_instance_; }
	// How many frames rewrote the beauty strip's surface in place (_rewrite_strip_in_place): the
	// device pin's read seam.
	int64_t get_strip_surface_rewrites() const { return strip_surface_rewrites_; }
	// The FrameFX bloom pass's nightvision redraw of the strip (the typed Q3
	// WaterNightVision source): the same march with the nightvision row
	// colors, drawn by no camera (layer mask 0), only by the Q3 pass.
	MeshInstance3D *get_night_vision_mesh_instance() const {
		return night_vision_mesh_instance_;
	}
	// Its material: the water program sampling the mirror the frame rendered
	// last (environment/water_mirror.h bloom_water_mirror_image).
	Ref<ShaderMaterial> get_night_vision_material() const { return night_vision_material_; }
	SubViewport *get_reflection_viewport() const { return mirror_.viewport; }
	Camera3D *get_reflection_camera() const { return mirror_.camera; }
	// The weapon Inset pass's own mirror and strip (advance_inset_frame).
	SubViewport *get_inset_reflection_viewport() const { return inset_mirror_.viewport; }
	Camera3D *get_inset_reflection_camera() const { return inset_mirror_.camera; }
	// The pose a water mirror draws its reflected subscene from for the view
	// pose it mirrors (engine environment/water_mirror.h build_water_mirror_view):
	// the pose _install_mirror_view gives that mirror's camera, read without
	// placing it (the Inset's mirror is placed after the frame's level walk).
	Transform3D mirror_view_transform(const Transform3D &p_source) const;
	MeshInstance3D *get_inset_mesh_instance() const { return inset_mesh_instance_; }
	Ref<ShaderMaterial> get_inset_water_material() const { return inset_material_; }

	// One render-frame advance — the externally-callable
	// drive the test harness uses; the engine's virtual delegates here.
	void advance_frame(double p_delta);

	void _ready() override;
	void _exit_tree() override;

protected:
	static void _bind_methods();
	void _notification(int p_what);

private:
	MissionEnvironment *_env_node();
	void _recompute_terrain_water_fallback();
	void _apply_environment_water_height();
	void _push_water_split_height();
	void _sync_render_activity();
	// The view drawing the world this frame: the world's local view
	// presenter's target while that target is live (its camera; the frame's
	// projection, LocalPlayerPresenter::view_projection, the NVG raster's
	// served matrix included; the target's raster -- the blit stretches it
	// over the surface), else the surface camera `p_surface_cam` over its own
	// viewport.
	struct DrawingView {
		Camera3D *camera = nullptr;
		// The projection the original projects the view through: the local
		// view presenter's screen projection (its D3D9 raster shift taken out)
		// for the frame's cameras, else the camera's own.
		Projection projection;
		Vector2i raster;
		// True for the frame's cameras: their screen projection is the plain
		// perspective their fov and keep mode describe (the camera node draws
		// it through its raster's half pixel, render/d3d9_raster_device).
		bool frame_perspective = false;
	};
	DrawingView _drawing_view(Camera3D *p_surface_cam) const;
	// One reflection target: the RTT SubViewport on the shared World3D, its
	// mirror camera, and the camera's decode-only compositor.
	struct MirrorRig {
		SubViewport *viewport = nullptr;
		Camera3D *camera = nullptr;
		// The camera's decode-only FrameFx effect: kept here so process-exit
		// teardown can detach and drain the render callback while
		// RenderingServer is still alive, before deleting the view.
		Ref<FrameFxCompositorEffect> decode_effect;
		Ref<Compositor> compositor;
	};
	void _build_mirror_rig(MirrorRig &r_rig, const String &p_viewport_name,
			const String &p_camera_name);
	void _release_mirror_rig(MirrorRig &r_rig);
	void _update_reflection_camera(Camera3D *p_cam, const Projection &p_projection,
			bool p_frame_perspective);
	// The mirror's own bit: MIRROR_VIEW for the main scene's, INSET_MIRROR_VIEW for the
	// Inset's (render/visual_layers.h).
	uint32_t _mirror_own_layer(const MirrorRig &p_rig) const;
	// Installs p_source's mirror into r_rig: the camera, its cull mask, its
	// node-side frame p_source_frame and the served raster.
	void _install_mirror_view(MirrorRig &r_rig, const opennova::env::MirrorSourceView &p_source,
			double p_near, double p_far, real_t p_h_offset, const Vector2i &p_source_frame);
	void _apply_reflection_clear();
	void _apply_reflection_clear(MirrorRig &r_rig);
	void _install_reflection_decode(MirrorRig &r_rig);
	void _release_reflection_decode(MirrorRig &r_rig);
	// The march inputs a frame's water passes share, with the lit colours
	// pushed to every water material.
	struct MarchInputs {
		float murk = 0.0f;
		float fog_end = 0.0f;
		opennova::env::WaterDepthCurve depth_curve;
		Color lit;
		Ref<EnvFile> env_data;
	};
	MarchInputs _march_inputs(MissionEnvironment *p_env);
	// The animated noise pair at this frame's counter, once per counter value.
	void _regenerate_noise();
	// One strip march for p_view into WaterCore's rows; returns the row count
	// (0 when the view cannot march). The pass fog end follows the side; the
	// scene depth range lands on p_material.
	int _march_strip(const DrawingView &p_view, bool p_underwater, bool p_nightvision,
			const MarchInputs &p_inputs, const Ref<ShaderMaterial> &p_material);
	// The strips' layers from the views' gates and the Inset pass's state.
	void _apply_strip_layers();
	// The bloom strip's mirror image (environment/water_mirror.h
	// bloom_water_mirror_image).
	void _apply_night_vision_reflection();
	void _retire_inset();
	// The last march as ArrayMesh surface arrays.
	Array _strip_arrays() const;
	static void _upload_strip(MeshInstance3D *p_mesh_instance, const Array &p_arrays,
			const Ref<ShaderMaterial> &p_material);
	bool _rewrite_strip_in_place(int p_rows);
	void _strip_surface_rebuilt(int p_rows);
	void _clear_strip_surfaces();
	void _clear_night_vision_surfaces();
	void _clear_inset_strip_surfaces();

	NodePath environment_path_;
	Ref<TerrainData> terrain_data_;
	float water_height_ = 0.0f;
	float water_alpha_ = 0.6f;
	float mission_water_height_override_ = NAN;
	// The world retains this node across unload/reload, while a standalone
	// water node starts enabled.
	bool world_rendering_enabled_ = true;
	bool mirror_enabled_ = true;
	bool globals_held_ = false;
	static int64_t global_writes_;
	static void _write_global(
			RenderingServer *p_rs, const StringName &p_name, const Variant &p_value);
	static void _clear_water_plane();
	float terrain_water_height_ = 0.0f;
	bool terrain_bounds_valid_ = false;
	float terrain_min_height_ = 0.0f;
	float terrain_max_height_ = 0.0f;
	bool blink_water_visible_ = false;

	MeshInstance3D *mesh_instance_ = nullptr;
	MeshInstance3D *night_vision_mesh_instance_ = nullptr;
	MeshInstance3D *inset_mesh_instance_ = nullptr;
	Ref<ShaderMaterial> water_material_;
	Ref<ShaderMaterial> night_vision_material_;
	Ref<ShaderMaterial> inset_material_;
	// The main frame's mirror and the weapon Inset pass's own.
	MirrorRig mirror_;
	MirrorRig inset_mirror_;
	// The mirror cameras' one clear environment (_apply_reflection_clear).
	Ref<Environment> reflection_environment_;
	// The views' water letter labels (set_blink_water_views).
	bool main_view_water_ = true;
	bool inset_view_water_ = true;
	// The Inset pass's water this frame (advance_inset_frame).
	bool inset_open_ = false;
	bool inset_mirror_rendered_ = false;
	bool main_reflection_active_ = false;
	bool inset_bounds_valid_ = false;
	float inset_min_height_ = 0.0f;
	float inset_max_height_ = 0.0f;
	// The latch as the main collect left it, the Inset's g_WaterActive input.
	bool inset_blink_latch_ = false;
	// The counter the noise pair was last generated at.
	int noise_counter_ = 0;
	bool noise_counter_valid_ = false;
	bool mirror_scene_outdoors_ = true;
	bool built_ = false;
	bool has_drawable_surface_ = false;
	// The row count the beauty strip's surface was made with (0: none), the rewrite's byte
	// streams, kept, and its count (an instrument).
	int strip_surface_rows_ = 0;
	PackedByteArray strip_vertex_bytes_;
	PackedByteArray strip_attribute_bytes_;
	int64_t strip_surface_rewrites_ = 0;
	ObjectID env_node_id_;
	ObjectID cached_cam_id_;
	std::unique_ptr<WaterCore> water_core_;
	Ref<Image> noise_color_img_;
	Ref<Image> noise_normal_img_;
	Ref<ImageTexture> noise_color_tex_;
	Ref<ImageTexture> noise_normal_tex_;
	int frame_counter_ = 0;
	bool frame_counter_fed_ = false;
};

} // namespace godot
