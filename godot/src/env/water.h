#pragma once

#include "render/visual_layers.h"

#include <cmath>
#include <memory>

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
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
	// item type + authored-Reflective records); below water it is unfiltered.
	// It never renders the water surface, FP overlay, or a player/person leg.
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
	// Irreversibly drop the retained water renderer graph during process
	// exit. Normal world unload deliberately keeps this graph warm for the
	// next mission; SceneTree teardown is too late because shell-owned
	// ImageTextures can otherwise outlive RenderingServer/GDExtension
	// deinitialization.
	void release_runtime_renderer_resources();

	// g_EnvWaterHeightFixed == 0 is retail's no-water sentinel. Signed
	// nonzero heights remain valid for terrain below the world origin.
	bool is_water_active() const { return water_height_ != 0.0f; }
	// The authored height can remain valid while the world retains this node
	// between loads; render-frame participation must use this predicate.
	bool is_water_render_active() const {
		return world_rendering_enabled_ && is_water_active();
	}
	// The frame's tracked visible-terrain bounds and last frame's Blink water
	// verdict feed the engine's g_WaterActive predicate
	// (terrain::water_pass_active, engine/runtime/terrain/quadtree.h).
	void set_visible_terrain_bounds(bool p_valid, float p_min_height,
			float p_max_height);
	void set_blink_water_visible(bool p_visible);
	// While the weapon Inset renders, each view's blink-water gate: the
	// Inset scene core draws its water passes on its own collect's
	// g_BlinkWaterVisible (engine: world/occlusion.h OcclusionView). The one
	// strip carries both views' verdicts: both -> the water layer, the main
	// view alone -> MAIN_VIEW_WATER (the beauty camera admits it, neither the
	// Inset nor either mirror mask does), the Inset alone -> the Inset bit,
	// neither -> hidden. (true, true) restores the ordinary strip.
	void set_blink_water_views(bool p_main, bool p_inset);
	bool is_water_pass_active() const;
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
	// The FrameFX bloom pass's nightvision redraw of the strip (the typed Q3
	// WaterNightVision source): the same march with the nightvision row
	// colors, drawn by no camera (layer mask 0), only by the Q3 pass.
	MeshInstance3D *get_night_vision_mesh_instance() const {
		return night_vision_mesh_instance_;
	}
	SubViewport *get_reflection_viewport() const { return reflection_viewport_; }
	Camera3D *get_reflection_camera() const { return reflection_camera_; }

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
		Projection projection;
		Vector2i raster;
	};
	DrawingView _drawing_view(Camera3D *p_surface_cam) const;
	void _update_reflection_camera(Camera3D *p_cam, const Projection &p_projection);
	void _apply_reflection_clear();
	void _install_reflection_decode();
	void _release_reflection_decode();
	// One strip march for `p_view` into WaterCore's rows; returns the row count
	// (0 when the view cannot march). The pass fog end follows the side.
	int _march_strip(const DrawingView &p_view, bool p_underwater, bool p_nightvision,
			float p_murk, float p_fog_end,
			const opennova::env::WaterDepthCurve &p_depth_curve,
			const Color &p_lit, const Ref<EnvFile> &p_env_data);
	// The last march as ArrayMesh surface arrays.
	Array _strip_arrays() const;
	static void _upload_strip(MeshInstance3D *p_mesh_instance, const Array &p_arrays,
			const Ref<ShaderMaterial> &p_material);
	void _clear_strip_surfaces();
	void _clear_night_vision_surfaces();

	NodePath environment_path_;
	Ref<TerrainData> terrain_data_;
	float water_height_ = 0.0f;
	float water_alpha_ = 0.6f;
	float mission_water_height_override_ = NAN;
	// The world retains this node across unload/reload, while a standalone
	// water node starts enabled.
	bool world_rendering_enabled_ = true;
	bool mirror_enabled_ = true;
	float terrain_water_height_ = 0.0f;
	bool terrain_bounds_valid_ = false;
	float terrain_min_height_ = 0.0f;
	float terrain_max_height_ = 0.0f;
	bool blink_water_visible_ = false;

	MeshInstance3D *mesh_instance_ = nullptr;
	MeshInstance3D *night_vision_mesh_instance_ = nullptr;
	Ref<ShaderMaterial> water_material_;
	SubViewport *reflection_viewport_ = nullptr;
	Camera3D *reflection_camera_ = nullptr;
	// The reflection camera owns a decode-only FrameFx effect. Keep both
	// resources here so process-exit teardown can detach and drain the render
	// callback while RenderingServer is still alive, before deleting the view.
	Ref<FrameFxCompositorEffect> reflection_decode_effect_;
	Ref<Compositor> reflection_compositor_;
	// The mirror camera's own clear environment (_apply_reflection_clear).
	Ref<Environment> reflection_environment_;
	bool mirror_scene_outdoors_ = true;
	bool built_ = false;
	bool has_drawable_surface_ = false;
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
