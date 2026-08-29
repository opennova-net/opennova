#pragma once

#include <cmath>

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/node_path.hpp>

#include <runtime/environment/sky_frame.h>
#include <runtime/environment/water_frame.h>
#include <runtime/environment/water_mirror.h>

#include "env/water_core.h"
#include "terrain/terrain_data.h"

namespace godot {

class Compositor;
class EnvFile;
class FrameFxCompositorEffect;
class MissionEnvironment;
class Weather;

// The water-plane applier — the ADR 0033 device leg over the engine's
// witnessed water pieces: the height precedence ladder + per-frame inputs
// (environment/water_frame.h), the proper-mirror reflection view
// (environment/water_mirror.h), and the noise/strip math already in
// engine/formats/env behind the WaterCore binding. This node keeps only
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
	// Visual-layer allocation for the reflection contract (env #30): above
	// water the mirror renders only the flag-0x400 population (vehicles by
	// item type + authored-Reflective records); below water it is unfiltered.
	// It never renders the water surface, FP overlay, or a player/person leg.
	// The witness lives with the mirror view (environment/water_mirror.h).
	enum {
		VISUAL_LAYER_WORLD = 1 << 0,
		VISUAL_LAYER_WATER = 1 << 10,
		// The retail environment-cube callback draws sky + sun/moon only.
		// All 20 Godot visual layers are allocated, so it aliases water's bit;
		// water rejects capture-camera eyes in its shader while the admitted
		// sky/celestial meshes carry this bit in addition to WORLD.
		VISUAL_LAYER_ENVIRONMENT_CAPTURE = VISUAL_LAYER_WATER,
		VISUAL_LAYER_VIEWMODEL = 1 << 11,
		VISUAL_LAYER_FP_BODY_SHADOW_ONLY = 1 << 12,
		VISUAL_LAYER_STATIC_SHADOW_CASTER = 1 << 13,
		VISUAL_LAYER_DYNAMIC_SHADOW_CASTER = 1 << 14,
		VISUAL_LAYER_TERRAIN_SHADOW_RECEIVER = 1 << 15,
		VISUAL_LAYER_WORLD_NO_MIRROR = 1 << 16,
		VISUAL_LAYER_SHADOW_CASTER_MASK = VISUAL_LAYER_STATIC_SHADOW_CASTER |
				VISUAL_LAYER_DYNAMIC_SHADOW_CASTER,
		// Per-slot silhouette-capture channels for the render-slot entity
		// ground shadows (12 = the retail RT budget; SlotShadow assigns the
		// per-order bits — env/slot_shadow.h). Every beauty/mirror
		// camera excludes them; only the slot capture cameras cull to them.
		VISUAL_LAYER_SLOT_CAPTURE_MASK = (1 << 1) | (1 << 2) | (1 << 3) |
				(1 << 4) | (1 << 5) | (1 << 6) | (1 << 7) | (1 << 8) |
				(1 << 9) | (1 << 17) | (1 << 18) | (1 << 19),
		// The mirror camera's above-water mask; a below-water view adds
		// WORLD_NO_MIRROR back (retail collects unfiltered there).
		REFLECTION_CULL_MASK = 0xFFFFF &
				~(VISUAL_LAYER_WATER | VISUAL_LAYER_VIEWMODEL |
						VISUAL_LAYER_FP_BODY_SHADOW_ONLY |
						VISUAL_LAYER_SHADOW_CASTER_MASK |
						VISUAL_LAYER_WORLD_NO_MIRROR |
						VISUAL_LAYER_SLOT_CAPTURE_MASK),
	};

	void set_environment_path(const NodePath &p_path);
	NodePath get_environment_path() const { return environment_path_; }
	void set_weather_path(const NodePath &p_path);
	NodePath get_weather_path() const { return weather_path_; }
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
	// Irreversibly drop the retained water renderer graph during process
	// exit. Normal world unload deliberately keeps this graph warm for the
	// next mission; SceneTree teardown is too late because shell-owned
	// ImageTextures can otherwise outlive RenderingServer/GDExtension
	// deinitialization.
	void release_runtime_renderer_resources();

	// Env_WaterHeightFixed == 0 is retail's no-water sentinel. Signed
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
	bool is_water_pass_active() const;

	void build();
	bool is_built() const { return built_; }
	Ref<ShaderMaterial> get_water_material() const { return water_material_; }
	// The live per-frame-regenerated noise color texture, pulled by the terrain
	// material for the below-water modulation (D-TERRAIN-8,
	// docs/terrain/terrain-re.md underwater section — terrain pulls from the
	// water module, matching the retail data direction). Null before build().
	Ref<Texture2D> get_noise_color_texture() const { return noise_color_tex_; }
	MeshInstance3D *get_mesh_instance() const { return mesh_instance_; }
	SubViewport *get_reflection_viewport() const { return reflection_viewport_; }
	Camera3D *get_reflection_camera() const { return reflection_camera_; }

	// One render-frame advance (the _process body) — the externally-callable
	// drive the test harness uses; the engine's virtual delegates here.
	void advance_frame(double p_delta);

	void _ready() override;
	void _process(double p_delta) override;
	void _exit_tree() override;

protected:
	static void _bind_methods();
	void _notification(int p_what);

private:
	MissionEnvironment *_env_node();
	Weather *_weather_node();
	void _recompute_terrain_water_fallback();
	void _apply_environment_water_height();
	void _push_water_split_height();
	void _sync_render_activity();
	void _update_reflection_camera(Camera3D *p_cam);
	void _rebuild_strip_mesh(Camera3D *p_cam, const Vector3 &p_cam_pos,
			float p_murk, float p_fog_end, const Vector4 &p_uv_state,
			const Color &p_lit, const Ref<EnvFile> &p_env_data);
	void _clear_strip_surfaces();

	NodePath environment_path_;
	NodePath weather_path_;
	Ref<TerrainData> terrain_data_;
	float water_height_ = 0.0f;
	float water_alpha_ = 0.6f;
	float mission_water_height_override_ = NAN;
	// The world retains this node across unload/reload, while a standalone
	// water node starts enabled.
	bool world_rendering_enabled_ = true;
	float terrain_water_height_ = 0.0f;
	bool terrain_bounds_valid_ = false;
	float terrain_min_height_ = 0.0f;
	float terrain_max_height_ = 0.0f;
	bool blink_water_visible_ = false;

	MeshInstance3D *mesh_instance_ = nullptr;
	Ref<ShaderMaterial> water_material_;
	SubViewport *reflection_viewport_ = nullptr;
	Camera3D *reflection_camera_ = nullptr;
	// The reflection camera owns a decode-only FrameFx effect. Keep both
	// resources here so process-exit teardown can detach and drain the render
	// callback while RenderingServer is still alive, before deleting the view.
	Ref<FrameFxCompositorEffect> reflection_decode_effect_;
	Ref<Compositor> reflection_compositor_;
	bool built_ = false;
	bool has_drawable_surface_ = false;
	ObjectID env_node_id_;
	ObjectID weather_node_id_;
	ObjectID cached_cam_id_;
	Ref<WaterCore> water_core_;
	Ref<Image> noise_color_img_;
	Ref<Image> noise_normal_img_;
	Ref<ImageTexture> noise_color_tex_;
	Ref<ImageTexture> noise_normal_tex_;
	int frame_counter_ = 0;
	opennova::env::ScrollFallback fallback_scroll_;
};

} // namespace godot
