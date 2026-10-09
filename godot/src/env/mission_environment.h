#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <runtime/environment/environment_state.h>

#include "env/env_file.h"
#include "object/object_model.h"

namespace godot {

class Camera3D;

// The runtime TOD environment owner — the ADR 0033 device leg over the
// engine's env::EnvironmentState (engine/runtime/environment), which owns the
// witnessed state: the mission TOD clock, the keyframe-TARGET vs smoothed-
// CURRENT split, network phase-2 overrides, the NVG rewrite, and the
// change-gated env generation. This node keeps only device work: the .env
// document property + reload signal, the opennova_* global shader parameter
// pushes (the scene-pass block terrain/foliage/water read, and the object
// family's lighting block — retail's per-pass RenderBatchCtx constants), the
// terrain ShaderMaterial uniform pushes, the sky-map texture handles, and the
// EnvLightState publication. Ported from environment.gd (2026-08-09 de-scripting); RE record:
// docs/env/env-tod-re.md.
class MissionEnvironment : public Node {
	GDCLASS(MissionEnvironment, Node)

public:
	MissionEnvironment();

	void set_environment_data(const Ref<EnvFile> &p_value);
	Ref<EnvFile> get_environment_data() const { return environment_data_; }

	void set_time_of_day(double p_value);
	double get_time_of_day() const { return state_.time_of_day(); }
	bool is_loaded() const;

	// The typed light channel for the non-shader consumers (the light
	// director, terrain light rows, diagnostics, view effects): every
	// generation bump publishes the current world values into it. Consumers
	// hold THIS record — never this node. The object shaders read the same
	// values as global shader parameters (write_lighting_block_globals).
	Ref<EnvLightState> get_light_state() const { return light_state_; }

	// --- the mission clock (owned by the weather home) ------------------------
	// The clock lives in world::WeatherState: the Simulation's on a mission
	// (the TOD WAC / the debug row move it through Simulation.
	// command_time_of_day_minutes), this state's standalone home otherwise
	// (previews, fixtures) — configure/advance act on the standalone home.
	// The mission clock's default day length in minutes (the script-visible
	// DEFAULT_MINUTES_PER_DAY constant binds from it).
	static constexpr int DEFAULT_MINUTES_PER_DAY = 1440;
	void configure_mission_clock(int p_start_time_q8_8, int p_minutes_per_day);
	void advance_mission_clock(int p_ticks);
	Error debug_set_mission_minute_of_day(double p_minute_of_day);
	double get_mission_minute_of_day() const;
	int get_mission_time_fixed24() const;

	// --- the weather-home reads --------------------------------------------
	// g_EnvQuakeTicks, g_EnvRainPctCurrent / 65536, g_EnvOvercastBlend / 65536,
	// g_EnvPrecipitationKind (0 rain, 1 snow), and the > 48 drop gate — read
	// through the bound weather (0 / clear without one).
	int get_quake_ticks() const;
	float get_rain_current() const;
	float get_overcast_blend() const;
	bool is_raining() const;
	// The overcast table (.trn + overcast.def keyframes) the overcast blend
	// cross-fades the .env colors against; null clears it.
	void set_overcast_data(const Ref<EnvFile> &p_data);

	// --- the weather-driven split -------------------------------------------
	void set_weather_driven(bool p_driven);
	bool is_weather_driven() const;

	// --- NVG / thermal ------------------------------------------------------
	void set_nvg_view(bool p_active, int p_gain);
	// The local player's thermal-imaging view, fed per presented tick like
	// NVG from the sim's view frame (the engine state carries the two gates
	// and their witnesses): the world gate greys the object lighting block
	// and selects the 0x808080 pass fog and frame clear; the terrain gate
	// selects the flat terrain ramps.
	void set_thermal_view(bool p_world, bool p_terrain);
	// The world gate: the frame's latched thermal byte, which the main scene
	// also hands the particle passes (the particle secondary materials).
	bool is_thermal_view() const { return state_.thermal_view(); }
	// The main scene pass selection, sampled from the render eye after local
	// camera placement and before terrain/foliage submit. This does not mutate
	// authored/current weather state; it selects the derived pass fog payload.
	void set_underwater_view(bool p_underwater);
	void apply_render_eye(float p_eye_y, float p_water_height,
			bool p_water_active);
	// The world pass's planes on `p_camera`: the far plane the fog distance
	// sets (renderer::scene_far_plane) and the scene pass's near plane, every
	// view and camera mode (renderer::kScenePassNearZ; retail
	// Render_ProcessMainSceneFrame @ 0x5ca4d7..0x5ca4e0). GameWorld's scene
	// environment leg sets them on the render camera, and the editor's
	// environment, terrain and mission previews on theirs.
	void apply_scene_pass_planes(Camera3D &p_camera) const;
	bool is_underwater_view() const { return underwater_view_; }
	// The later full-frame murk scissor has an independently witnessed side
	// test: camera <= water, while device fog above stays strict camera < water.
	void set_underwater_overlay_view(bool p_underwater);
	bool is_underwater_overlay_view() const {
		return underwater_overlay_view_;
	}
	Vector3 get_underwater_overlay_color() const;
	int get_underwater_overlay_alpha_byte() const;
	// Whether this frame's sky pass drew the dome (the sky-dome gate owner
	// reports it); with the eye strictly above water it selects the fog colour
	// the dome wrapper leaves for the viewmodel (engine build_viewmodel_fog).
	void set_sky_dome_drawn(bool p_drawn);
	// The first-person viewmodel's pass fog, published as the
	// opennova_viewmodel_fog_color / opennova_viewmodel_fog_range globals
	// (range = start, end, type): the dry pass whatever the eye's side.
	Vector3 get_viewmodel_fog_color() const;
	Vector3 get_viewmodel_fog_range() const;
	// The water mirror pass's fog block as published: color and
	// (start, end, type).
	Vector3 get_water_mirror_fog_color() const;
	Vector3 get_water_mirror_fog_range() const;
	Vector3 get_scene_fog_color() const;
	float get_scene_fog_start() const;
	float get_scene_fog_end() const;
	int get_scene_fog_type() const;

	// --- current render colors ----------------------------------------------
	Vector3 get_sun_light() const;
	Vector3 get_fill_light() const;
	Vector3 get_sky_ambient() const;
	Vector3 get_fog_color() const;
	Vector3 get_skyfog_color() const;
	Vector3 get_frame_clear_color() const;
	// The witnessed clear SELECTION (environment_state.h carries the
	// citations): thermal grey, skyfog strictly above water, lit water at or
	// below it.
	Color frame_clear_color_for(bool p_eye_above_water) const;
	// The NVG scene's clear (environment_state.h): skyfog / lit water alone.
	Color nvg_scene_clear_color(bool p_eye_above_water) const;
	Vector3 get_ceiling_color() const;
	Vector3 get_cloud_tint() const;
	Vector3 get_floor_color() const;
	Vector3 get_sky_base() const;
	Vector3 get_sky_bright() const;
	Vector3 get_sky_highlight() const;
	Vector3 get_cloud_base() const;
	Vector3 get_cloud_highlight() const;
	Vector3 get_cloud_edge() const;
	Vector3 get_color_src_gain() const;
	// The two particle tints the effect world refreshes every tick from the
	// environment, as per-channel factors where retail byte 128 = 1.0:
	// AMBIENTCOLOR (and any blend-mode-0 graphic) draws through
	// g_EnvTerrainLightCombined; everything else through the modulator block
	// doubled and saturated at 255 (retail Render_EmitterEffect @ 0x5f70c0
	//  (world+0x3E8 / +0x3F0); CParticleEmitter_AdvanceFrame @ 0x5e6600..0x5e661c).
	Vector3 get_particle_ambient_tint() const;
	Vector3 get_particle_modulator_tint() const;

	// --- keyframe targets ---------------------------------------------------
	Vector3 get_fill_light_target() const;
	Vector3 get_sun_light_target() const;
	Vector3 get_fog_color_target() const;
	Vector3 get_sky_ambient_target() const;
	Vector3 get_ceiling_color_target() const;
	Vector3 get_cloud_tint_target() const;
	Vector3 get_floor_color_target() const;
	Vector3 get_lightning_color_target() const;

	// --- terrain / water / directions ---------------------------------------
	Vector3 get_terrain_tint() const;
	Vector3 get_terrain_lighting_attenuation() const;
	Vector3 get_tile_overlay_tint() const;
	// Push the env-derived terrain lighting + fog uniforms onto a terrain
	// ShaderMaterial. The terrain shaders share these uniforms via
	// terrain_lighting.gdshaderinc.
	// Callers may override individual values afterwards (the runtime layers
	// weather-smoothed colors + the tile-overlay tint on top).
	void apply_terrain_uniforms(const Ref<ShaderMaterial> &p_material);
	Vector3 get_water_color() const;
	bool has_water_height() const;
	float get_water_height() const;
	Vector3 get_sun_direction() const;
	Vector3 get_moon_direction() const;
	Vector3 get_light_direction() const;
	// The active light as the engine serves it — the render-float tuple of
	// Environment_GetLightDirectionFloat, no axis map. For consumers that
	// port a retail packing of that tuple (the terrain page projector and
	// the tile-cache DOT3 bytes), which must never receive the Godot-axes
	// vector above: their own (g2,g1,g0) reduction would then swap twice.
	Vector3 get_light_direction_render_tuple() const;
	bool is_night_phase() const;
	float get_day_phase_blend() const;
	Vector3 get_sun_color() const;
	Vector3 get_moon_color() const;

	// --- the weather writeback seam ----------------------------------------
	void set_fill_light(const Vector3 &p_value);
	void set_sun_light(const Vector3 &p_value);
	void set_sky_ambient_rt(const Vector3 &p_value);
	void set_color_src_gain(const Vector3 &p_value);
	int64_t get_env_generation() const;
	int64_t get_scene_generation() const {
		return light_state_.is_valid() ? light_state_->get_generation() : 0;
	}

	// --- env #27 smoothed scalars -------------------------------------------
	float get_fog_distance() const;
	float get_fog_level() const;
	float get_fog_level_target() const;
	float get_fog_start() const;
	float get_fog_end_distance() const;
	int get_fog_type() const;
	float get_sky_speed() const;
	float get_sky_height() const;
	float get_sky_height_target() const;

	Ref<Texture2D> get_sky_map1_tex() const;
	Ref<Texture2D> get_sky_map2_tex() const;

	// C++-only seams for the sibling native appliers (the weather node, the
	// terrain uniform pusher): the engine state and the publish flush the
	// engine-side writeback path cannot perform itself.
	opennova::env::EnvironmentState &state() { return state_; }
	const opennova::env::EnvironmentState &state() const { return state_; }
	// Publish the typed light record and write the object lighting block
	// globals when the engine generation moved since the last publish.
	void flush_publication(bool p_pass_changed = false);
	// The standalone-owner full global refresh (the weather node owns the
	// per-frame write while present).
	void write_shader_globals();
	// The object lighting block as last written to the global shader parameters
	// (opennova_light_block_*, opennova_fog_enabled, opennova_thermal_view) and
	// how many blocks were written. Process-wide like the globals; the read seam
	// the tests assert through, since outside the editor a RenderingServer gives
	// no global back (global_shader_parameter_get is editor-only).
	static Ref<EnvLightValues> get_published_lighting_block();
	static int64_t get_lighting_block_writes();
	// Every global this environment renders with written again whether or not
	// its generation moved (ADR 0046 S14, E13: another picture published its
	// own state over the process-wide globals since; this one renders next).
	void republish_shader_globals();
	// The shipped defaults written over the process-wide globals: every global
	// an environment or a water writes, at the value project.godot ships it
	// with (no environment the lighting block's writer), as a process has them
	// before any mission publishes. What a picture of no environment (a
	// model's) renders under after a mission's picture published its own (E13).
	static void publish_shipped_defaults();
	// Whether this environment's lighting block is the one the globals hold.
	bool is_lighting_block_writer() const { return lighting_block_writer_ == this; }
	// The hold (ADR 0046 S14, E13): while held, nothing this environment
	// computes reaches the process-wide shader globals (its state, its light
	// state and its generation still move); republish_shader_globals writes
	// them whatever the hold. The editor's mission device holds its
	// environment but while it publishes its scene state and presents its own
	// frame, so another picture never renders under it; the game never holds.
	void set_globals_held(bool p_held) { globals_held_ = p_held; }
	bool is_globals_held() const { return globals_held_; }
	// How many process-wide shader globals every environment wrote since the process began: a read-back
	// for the hold's tests (a headless renderer keeps no global to read back).
	static int64_t get_global_writes() { return global_writes_; }


	void _ready() override;

protected:
	static void _bind_methods();
	void _notification(int p_what);

private:
	void _ensure_loaded();
	void _attach_config();
	void _on_environment_changed();
	// The post-TOD device tail: publish if the generation moved, then the
	// standalone-owner shader-global refresh (weather-driven frames leave the
	// globals to the weather writeback).
	void _after_tod_update();
	Ref<EnvLightValues> _build_light_values() const;
	void _write_scene_fog_globals();
	opennova::env::SceneFogValues _viewmodel_fog() const;
	void _write_viewmodel_fog_globals();
	// The water mirror pass's fog block (EnvironmentState::
	// build_water_mirror_fog) as opennova_water_mirror_fog_color / _range:
	// the reflected pass selects it per camera in the object and terrain
	// shaders.
	void _write_water_mirror_fog_globals();
	// The object family's per-pass lighting block as global shader
	// parameters: the world block the object shaders scale per entity at
	// draw time (renderer/light_runtime.h carries the RenderBatchCtx
	// witness). One write per env change; process-wide, so the last writer
	// restores the shipped noon defaults when it leaves the tree.
	void _write_lighting_block_globals(const Ref<EnvLightValues> &p_values);
	// The block's globals alone, whoever writes them.
	static void _write_lighting_block(const Ref<EnvLightValues> &p_values);
	static void _write_globals(const opennova::env::EnvShaderGlobals &p_globals);
	// The writer leaving the tree or dying puts the noon register back and
	// forgets its publication generation, so re-entering republishes.
	void _release_lighting_block();
	// Every process-wide shader global an environment writes, counted.
	static void _set_global(const StringName &p_name, const Variant &p_value);
	static MissionEnvironment *lighting_block_writer_;
	// What _write_lighting_block_globals last wrote (plain values: a static
	// Variant would outlive the engine at exit).
	struct PublishedLightingBlock {
		Vector3 dir;
		Vector3 dir_color;
		Vector3 hemi_sky;
		Vector3 hemi_ground;
		Vector3 ceiling;
		Vector3 floor_color;
		Vector3 gain;
		bool fog_enabled = false;
		bool thermal_view = false;
		int64_t writes = 0;
	};
	static PublishedLightingBlock published_block_;
	static int64_t global_writes_;

	Ref<EnvFile> environment_data_;
	Ref<EnvFile> overcast_data_;
	opennova::env::EnvironmentState state_;
	Ref<EnvLightState> light_state_;
	int64_t last_published_generation_ = 0;
	bool underwater_view_ = false;
	bool underwater_overlay_view_ = false;
	bool sky_dome_drawn_ = true;
	bool globals_held_ = false;
};

// The env appliers' (sky, water, celestial) cached sibling lookup of their
// MissionEnvironment (and the sky's Weather): the cached node while it is live
// and in the tree, else `p_path` resolved from `p_self` and re-cached (a miss
// clears the cache). Lazy (re-)resolution supports owners that create the
// target after the node; relative sibling paths also resolve off-tree.
template <typename T>
T *resolve_cached_node(const Node &p_self, const NodePath &p_path, ObjectID &r_cache) {
	if (r_cache.is_valid()) {
		T *node = Object::cast_to<T>(ObjectDB::get_instance(r_cache));
		if (node != nullptr && node->is_inside_tree()) {
			return node;
		}
	}
	if (p_path.is_empty() || (!p_self.is_inside_tree() && p_path.is_absolute())) {
		return nullptr;
	}
	T *node = Object::cast_to<T>(p_self.get_node_or_null(p_path));
	r_cache = node != nullptr ? ObjectID(node->get_instance_id()) : ObjectID();
	return node;
}

} // namespace godot
