#pragma once

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <formats/env/env.h>

#include "env/env_keyframe.h"
#include "env/env_records.h"
#include "resource_index/resource_root.h"

namespace godot {

class MissionEnvironmentOverrides;

// Resource wrapper for a stock .env environment/TOD file.
// Native data and all format behavior live in engine/formats/env; this class is the
// Godot-facing equivalent of the (engine: formats/env/env.cpp) /
// (engine: formats/env/env.cpp) state plus save support (docs/env/env-tod-re.md).
class EnvFile : public Resource {
	GDCLASS(EnvFile, Resource)

private:
	String source_path;
	String env_name = "Untitled";
	String timeofday = "Day";
	float envscale = 1.0f;
	int curtime = 1200;
	float fog_level = 1000.0f;
	int fog_type = 2;
	Color terrain_tint = Color(1, 1, 1);
	Color water_color = Color(56.0f / 255.0f, 59.0f / 255.0f, 39.0f / 255.0f);
	float water_height = 0.0f;
	bool water_height_set = false;
	Color cloud_tint = Color(128.0f / 255.0f, 128.0f / 255.0f, 128.0f / 255.0f);
	Color vertex_tint = Color(128.0f / 255.0f, 128.0f / 255.0f, 128.0f / 255.0f);
	Color lightning_color = Color(85.0f / 255.0f, 85.0f / 255.0f, 90.0f / 255.0f);
	Color ceiling_color = Color(55.0f / 255.0f, 55.0f / 255.0f, 55.0f / 255.0f);
	Color floor_color = Color(25.0f / 255.0f, 25.0f / 255.0f, 25.0f / 255.0f);
	float water_murk = 0.8f;
	float iris_percent = 15.0f;
	float iris_center = 1.0f;
	float sky_speed = 15.0f;
	float sky_height = 175.0f;
	String sky_map1 = "Cloud01.pcx";
	String sky_map2 = "Cloud01b.pcx";
	String sun_3di = "msun.3di";
	String moon_3di = "fmoon4.3di";
	String glare_3di = "mglare.3di";
	String star_3di;
	Ref<Texture2D> sky_map1_tex;
	Ref<Texture2D> sky_map2_tex;
	int advanced_clouds = 1;
	TypedArray<EnvKeyframe> tod_keyframes;

	opennova::env::Config env;
	bool loaded = false;
	Ref<ResourceRoot> resource_root;

	// Non-persistent BMS mission override layer (engine: formats/env/env.h): properties/getters show the overridden live view, while
	// save_to_path/to_bytes always write the base captured at apply time — a
	// mission-opened env can never save contaminated values. Clear overrides
	// before document editing. See docs/env/env-tod-re.md.
	opennova::env::Config env_base;
	bool mission_overrides_active = false;

	void _sync_env_from_properties();
	void _sync_properties_from_env();
	void _notify_environment_changed();
	void _on_keyframe_changed();
	void _connect_keyframes();
	void _disconnect_keyframes();
	void _load_sky_textures();
	godot::Ref<godot::Texture2D> _load_sky_map_texture(const godot::String &name);
	godot::Ref<godot::Texture2D> _load_sky_map_texture_from_dir(const godot::String &dir, const godot::String &name);

protected:
	static void _bind_methods();

public:
	EnvFile();

	void set_source_path(const String &p_path);
	String get_source_path() const;
	void set_env_name(const String &p_value);
	String get_env_name() const;
	void set_timeofday(const String &p_value);
	String get_timeofday() const;
	void set_envscale(float p_value);
	float get_envscale() const;
	void set_curtime(int p_value);
	int get_curtime() const;
	void set_fog_level(float p_value);
	float get_fog_level() const;
	void set_fog_type(int p_value);
	int get_fog_type() const;
	void set_terrain_tint(const Color &p_value);
	Color get_terrain_tint() const;
	void set_water_color(const Color &p_value);
	Color get_water_color() const;
	void set_water_height(float p_value);
	float get_water_height() const;
	bool has_water_height() const;
	void set_cloud_tint(const Color &p_value);
	Color get_cloud_tint() const;
	void set_vertex_tint(const Color &p_value);
	Color get_vertex_tint() const;
	void set_lightning_color(const Color &p_value);
	Color get_lightning_color() const;
	void set_ceiling_color(const Color &p_value);
	Color get_ceiling_color() const;
	void set_floor_color(const Color &p_value);
	Color get_floor_color() const;
	void set_water_murk(float p_value);
	float get_water_murk() const;
	void set_iris_percent(float p_value);
	float get_iris_percent() const;
	void set_iris_center(float p_value);
	float get_iris_center() const;
	void set_sky_speed(float p_value);
	float get_sky_speed() const;
	void set_sky_height(float p_value);
	float get_sky_height() const;
	void set_sky_map1(const String &p_value);
	String get_sky_map1() const;
	void set_sky_map2(const String &p_value);
	String get_sky_map2() const;
	void set_sun_3di(const String &p_value);
	String get_sun_3di() const;
	void set_moon_3di(const String &p_value);
	String get_moon_3di() const;
	void set_glare_3di(const String &p_value);
	String get_glare_3di() const;
	void set_star_3di(const String &p_value);
	String get_star_3di() const;
	Ref<Texture2D> get_sky_map1_tex() const;
	Ref<Texture2D> get_sky_map2_tex() const;
	void set_advanced_clouds(int p_value);
	int get_advanced_clouds() const;
	TypedArray<EnvKeyframe> get_tod_keyframes() const;

	// C++-only: the live (mission-override-layered) engine config this
	// resource wraps — the environment state owner holds this pointer for the
	// resource's lifetime (the member's identity is stable across reloads).
	const opennova::env::Config &native_config() const { return env; }

	Error load();
	Error load_from_resource_root(const Ref<ResourceRoot> &p_resource_root, const String &p_name);
	// The environment a mission starts under (opennova::env::read_mission_env):
	// the terrain's .trn `p_terrain` (empty: none), overcast.def and the .env
	// `p_name` through `p_resource_root`, each over the one before; a .env that is
	// not there leaves the earlier passes over the engine's pre-parse defaults,
	// with no time-of-day keyframe (never the authoring template reset_to_default
	// writes). `p_overcast`, where valid, takes the overcast table the load makes
	// (the .trn's keyframes and overcast.def's). Always leaves the document
	// loaded; false when the .env was skipped.
	bool load_mission_environment(const Ref<ResourceRoot> &p_resource_root, const String &p_terrain,
			const String &p_name, const Ref<EnvFile> &p_overcast);
	Error save_to_path(const String &p_path);
	void reset_to_default();
	bool is_loaded() const;
	Dictionary interpolate_time_of_day(float p_time) const;
	Vector3 compute_sun_direction(float p_time) const;
	Vector3 compute_moon_direction(float p_time) const;

	// Engine fog policy (engine: formats/env/env_render.cpp); overcast is the 0..1 weather
	// blend (0 while no weather system drives it).
	float get_fog_start(float p_overcast = 0.0f) const;
	float get_fog_density() const;
	float get_fog_end_distance(float p_overcast = 0.0f) const;
	// The above-water overcast attenuation on a caller-supplied LIVE distance
	// (env fog_end_above_water (engine: formats/env/env_render.cpp)).
	static float fog_end_above_water(float p_fog_distance, float p_overcast);
	float get_fog_end_underwater() const;

	// Hardcoded sunrise/sunset windows as an EnvDayPhase record
	// (engine: formats/env/env_render.cpp).
	Ref<EnvDayPhase> get_day_phase(float p_time) const;

	// Derived render colors [orig: Environment_UpdateWeatherTick tail, see docs/env/env-tod-re.md].
	static Color double_saturate_color(const Color &p_color);
	static Color combine_terrain_light(const Color &p_light, const Color &p_sky);
	static Color lit_water_color(const Color &p_water, const Color &p_light);
	// The frame-clear horizon blend: skyfog cross-faded toward fog when the
	// smoothed fog distance drops below half the reference (distances in world
	// units; converted to 16.16 internally). Operates on UNDOUBLED colors.
	// (engine: formats/env/env_render.cpp)
	static Color horizon_blend_skyfog(const Color &p_fog, const Color &p_skyfog,
			float p_fog_distance, float p_fog_distance_reference);

	// Sun glare intensity from view-sun alignment and occlusion brightness
	// (engine: formats/env/env_celestial.h) as an EnvSunGlare record
	// (glare 0..255, fog_whiten 0..40).
	static Ref<EnvSunGlare> compute_sun_glare(float p_view_dot_sun, int p_occlusion_brightness);

	// The mission time-of-day clock (env/tod_clock.h): the BMS Q8.8 start hour
	// widened into the day-wrapped 8.24 accumulator (engine: formats/env/env.h)
	// and the tick advance (engine: formats/env/tod_clock.h).
	static int tod_start_fixed24(int p_start_time_q8_8);
	static int tod_advance(int p_time_fixed24, int p_ticks, int p_advance_per_tick);

	// The .til tile-overlay tint factor for a single-multiply shader:
	// 2*HALF(terrain_rgb)/255 per channel — 254/255 at the default tint (the
	// witnessed MODULATE2X-over-half combine is near-identity, not exact).
	// (engine: formats/env/env_render.cpp)
	static Color tile_overlay_tint_factor(const Color &p_terrain_tint);

	// The witnessed 21x21 sky dome mesh in Mesh.ARRAY_* layout (VERTEX /
	// NORMAL / TEX_UV / TEX_UV2 / INDEX populated), built at p_sky_height
	// (engine: formats/env/env_celestial.h) in the render basis and placed
	// in the Godot world through the util/axes.h x/z swap (the UVs stay the
	// builder's render-basis ones). The reimpl builds ONCE at
	// dome_reference_height() and folds the Y-only height scale into the
	// vertex shader (env #20's ratified structure; retail re-bakes on
	// smoothed-height change via SkyDome_SetHeightAndRebuild @ 0x579070).
	static Array build_sky_dome_arrays(float p_sky_height);

	// 3072 - sqrt(2^23) ~= 175.6906 — the exact apex reference height behind
	// the shaders' rounded "175.69" divisor (engine: formats/env/env_celestial.h).
	static float dome_reference_height();

	// Celestial bodies place at camera + direction * this distance (world
	// units, full camera height, identity rotation)
	// (engine: formats/env/env_celestial.h).
	static float celestial_body_distance();

	// Witnessed body alphas (0..1 out): sun = (1 - overcast) x (100 - dim)/100;
	// moon = clamp01((fogDistInt - 400)/600) x (1 - overcast)
	// (engine: formats/env/env_celestial.h). overcast/dim in 0..1 / 0..100.
	static float celestial_sun_alpha(float p_overcast_blend, float p_sun_dim_pct);
	static float celestial_moon_alpha(float p_fog_distance, float p_overcast_blend);

	// The glare glow alpha (0..1): dot_view^4/2 x brightness(0..256)/256 x the
	// overcast and SunDim folds (engine: formats/env/env_celestial.h).
	static float glare_glow_alpha(float p_view_dot_sun, int p_brightness,
			float p_overcast_blend, float p_sun_dim_pct);

	// The steady-state layer-1 cloud UV drift per second for a parsed
	// sky_speed (rate = sky_speed << 10 through 62 Hz x 2^-28) — the single
	// home of the old "sky_speed * 1024 * 62 / 2^28" magic; owners with a live
	// weather node read the RAMPING rate off it instead
	// (engine: formats/env/env_render.cpp).
	static float cloud_uv_rate_per_second(float p_sky_speed);

	// Field name -> renderer-consumption status for editor badging:
	// {"status": "honored"|"partial"|"unconsumed", "faithful": bool,
	//  "anchor": String, "note": String}. Statuses mirror
	// docs/env/env-honored-matrix.md and flip only with grill citations.
	static Dictionary get_field_consumption();

	// BMS mission override layer (env/mission_environment_overrides.h): the
	// armed fields land on a copy of the base config.
	void apply_mission_overrides(const Ref<MissionEnvironmentOverrides> &p_overrides);
	void clear_mission_overrides();
	bool has_mission_overrides() const;

	// Byte snapshots of the BASE config (CRLF .env text) for editor undo.
	PackedByteArray to_bytes() const;
	bool load_bytes(const PackedByteArray &p_bytes);
};

} // namespace godot
