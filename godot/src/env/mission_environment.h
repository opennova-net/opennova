#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <runtime/environment/environment_state.h>

#include "env/env_file.h"
#include "object/object_model.h"

namespace godot {

// The runtime TOD environment owner — the ADR 0033 device leg over the
// engine's env::EnvironmentState (engine/runtime/environment), which owns the
// witnessed state: the mission TOD clock, the keyframe-TARGET vs smoothed-
// CURRENT split, network phase-2 overrides, the NVG rewrite, and the
// change-gated env generation. This node keeps only device work: the .env
// document property + reload signal, the opennova_* global shader parameter
// pushes (the scene-pass block terrain/foliage/water read, and the object
// family's lighting block — retail's per-pass RenderBatchCtx constants), the
// terrain ShaderMaterial uniform pushes, the sky-map texture handles, and the
// EnvLightState publication + env_generation_changed signal. Ported from environment.gd (2026-08-09 de-scripting); RE record:
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

	// --- mission clock -----------------------------------------------------
	void configure_mission_clock(int p_start_time_q8_8, int p_minutes_per_day);
	static double mission_start_time_hhmm(int p_start_time_q8_8);
	void advance_mission_clock(int p_ticks);
	Error debug_set_mission_minute_of_day(double p_minute_of_day);
	double get_mission_minute_of_day() const;
	int get_mission_time_fixed24() const;

	static double minute_of_day_to_hhmm(double p_minute_of_day);
	static double hhmm_to_minute_of_day(double p_hhmm);

	// --- network phase-2 ---------------------------------------------------
	// The dictionary is deliberately the wire view: reconstructing native
	// units happens in the engine state exactly once, and a later replacement
	// .env clears every remote override.
	void apply_network_environment_sample(const Dictionary &p_sample);
	int get_network_quake_ticks() const;
	float get_network_rain_current() const;
	float get_overcast_blend() const;

	// --- the weather-driven split -------------------------------------------
	void set_weather_driven(bool p_driven);
	bool is_weather_driven() const;

	// --- NVG ----------------------------------------------------------------
	void set_nvg_view(bool p_active, int p_gain);
	// The main scene pass selection, sampled from the render eye after local
	// camera placement and before terrain/foliage submit. This does not mutate
	// authored/current weather state; it selects the derived pass fog payload.
	void set_underwater_view(bool p_underwater);
	void apply_render_eye(float p_eye_y, float p_water_height,
			bool p_water_active);
	bool is_underwater_view() const { return underwater_view_; }
	// The later full-frame murk scissor has an independently witnessed side
	// test: camera <= water, while device fog above stays strict camera < water.
	void set_underwater_overlay_view(bool p_underwater);
	bool is_underwater_overlay_view() const {
		return underwater_overlay_view_;
	}
	Vector3 get_underwater_overlay_color() const;
	int get_underwater_overlay_alpha_byte() const;
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
	// citations): black indoors, skyfog above water, lit water underwater.
	Color frame_clear_color_for(bool p_indoors, bool p_above_water) const;
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
	void set_fog_color_rt(const Vector3 &p_value);
	void set_sky_ambient_rt(const Vector3 &p_value);
	void set_static_colors_rt(const Vector3 &p_ceiling, const Vector3 &p_cloud,
			const Vector3 &p_floor_color);
	void set_sky_colors_rt(const Vector3 &p_skyfog, const Vector3 &p_sky_base,
			const Vector3 &p_sky_bright, const Vector3 &p_sky_highlight,
			const Vector3 &p_cloud_base, const Vector3 &p_cloud_highlight,
			const Vector3 &p_cloud_edge);
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
	void set_smoothed_scalars(float p_fog_distance, float p_sky_height,
			float p_sun_dim_pct = 0.0f, float p_rain_current = 0.0f,
			float p_overcast_blend = 0.0f);

	Ref<Texture2D> get_sky_map1_tex() const;
	Ref<Texture2D> get_sky_map2_tex() const;

	// C++-only seams for the sibling native appliers (the weather node, the
	// terrain uniform pusher): the engine state and the publish flush the
	// engine-side writeback path cannot perform itself.
	opennova::env::EnvironmentState &state() { return state_; }
	const opennova::env::EnvironmentState &state() const { return state_; }
	// Publish the typed light record, write the object lighting block globals,
	// and emit env_generation_changed when the engine generation moved since
	// the last publish.
	void flush_publication(bool p_pass_changed = false);
	// The standalone-owner full global refresh (the weather node owns the
	// per-frame write while present).
	void write_shader_globals();

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
	// The object family's per-pass lighting block as global shader
	// parameters: the world block the object shaders scale per entity at
	// draw time (renderer/light_runtime.h carries the RenderBatchCtx
	// witness). One write per env change; process-wide, so the last writer
	// restores the shipped noon defaults when it leaves the tree.
	void _write_lighting_block_globals(const Ref<EnvLightValues> &p_values);
	// The writer leaving the tree or dying puts the noon register back and
	// forgets its publication generation, so re-entering republishes.
	void _release_lighting_block();
	static MissionEnvironment *lighting_block_writer_;

	Ref<EnvFile> environment_data_;
	opennova::env::EnvironmentState state_;
	Ref<EnvLightState> light_state_;
	int64_t last_published_generation_ = 0;
	bool underwater_view_ = false;
	bool underwater_overlay_view_ = false;
};

} // namespace godot
