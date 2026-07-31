#pragma once

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/classes/resource_format_loader.hpp>
#include <godot_cpp/classes/resource_format_saver.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <env/env.h>

#include "env/nova_env_keyframe.h"
#include "resource_index/nova_resource_root.h"

namespace godot {

// Resource wrapper for a stock .env environment/TOD file.
// Native data and all format behavior live in libs/env; this class is the
// Godot-facing equivalent of the [orig: Environment_LoadTimeOfDayConfig @ 0x57db30] /
// [orig: TimeOfDay_ParseProperty @ 0x57c590] state plus save support (docs/env/env-tod-re.md).
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
	TypedArray<NovaEnvKeyframe> tod_keyframes;

	opennova::env::Config env;
	bool loaded = false;
	Ref<NovaResourceRoot> resource_root;

	// Non-persistent BMS mission override layer [orig: Game_LoadTerrainDuringConnect
	// @ 0x520710]: properties/getters show the overridden live view, while
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
	void set_sky_map1_tex(const Ref<Texture2D> &p_tex);
	Ref<Texture2D> get_sky_map1_tex() const;
	void set_sky_map2_tex(const Ref<Texture2D> &p_tex);
	Ref<Texture2D> get_sky_map2_tex() const;
	void set_advanced_clouds(int p_value);
	int get_advanced_clouds() const;
	void set_tod_keyframes(const TypedArray<NovaEnvKeyframe> &p_keyframes);
	TypedArray<NovaEnvKeyframe> get_tod_keyframes() const;

	Error load();
	Error load_from_resource_root(const Ref<NovaResourceRoot> &p_resource_root, const String &p_name);
	Error save_to_path(const String &p_path);
	void reset_to_default();
	bool is_loaded() const;
	Dictionary interpolate_time_of_day(float p_time) const;
	Vector3 compute_sun_direction(float p_time) const;
	Vector3 compute_moon_direction(float p_time) const;

	// Engine fog policy [orig: Render_SetFogState @ 0x58a950 ->
	// CD3DDevice_SetFogParameters @ 0x677960]; overcast is the 0..1 weather
	// blend (0 while no weather system drives it).
	float get_fog_start(float p_overcast = 0.0f) const;
	float get_fog_density() const;
	float get_fog_end_distance(float p_overcast = 0.0f) const;
	float get_fog_end_underwater() const;

	// Hardcoded sunrise/sunset windows; {"is_night": bool, "blend": float}
	// [orig: Environment_ComputeTimeOfDayColors @ 0x57de99].
	Dictionary get_day_phase(float p_time) const;

	// Derived render colors [orig: Environment_UpdateWeatherTick tail].
	static Color double_saturate_color(const Color &p_color);
	static Color combine_terrain_light(const Color &p_light, const Color &p_sky);
	static Color lit_water_color(const Color &p_water, const Color &p_light);
	// The frame-clear horizon blend: skyfog cross-faded toward fog when the
	// smoothed fog distance drops below half the reference (distances in world
	// units; converted to 16.16 internally). Operates on UNDOUBLED colors.
	// [orig: Environment_UpdateWeatherTick @ 0x57e9b0 blend @ 0x57f037..0x57f0a1]
	static Color horizon_blend_skyfog(const Color &p_fog, const Color &p_skyfog,
			float p_fog_distance, float p_fog_distance_reference);

	// Sun glare intensity from view-sun alignment and occlusion brightness
	// [orig: compute_sun_glare_and_fog_blend @ 0x5ad610]; returns
	// {"glare": 0..255, "fog_whiten": 0..40}.
	static Dictionary compute_sun_glare(float p_view_dot_sun, int p_occlusion_brightness);

	// The .til tile-overlay tint factor for a single-multiply shader:
	// 2*HALF(terrain_rgb)/255 per channel — 254/255 at the default tint (the
	// witnessed MODULATE2X-over-half combine is near-identity, not exact).
	// [orig: PolyTrn_SetTerrainTintColors @ 0x605e20; PolyTrn_RenderTile @ 0x60df0d]
	static Color tile_overlay_tint_factor(const Color &p_terrain_tint);

	// The witnessed 21x21 sky dome mesh in Mesh.ARRAY_* layout (VERTEX /
	// NORMAL / TEX_UV / TEX_UV2 / INDEX populated), built at p_sky_height
	// [orig: build_sky_dome_mesh @ 0x578db0]. The reimpl builds ONCE at
	// dome_reference_height() and folds the Y-only height scale into the
	// vertex shader (env #20's ratified structure; retail re-bakes on
	// smoothed-height change via SkyDome_SetHeightAndRebuild @ 0x579070).
	static Array build_sky_dome_arrays(float p_sky_height);

	// 3072 - sqrt(2^23) ~= 175.6906 — the exact apex reference height behind
	// the shaders' rounded "175.69" divisor [orig: @ 0x578ed4].
	static float dome_reference_height();

	// Celestial bodies place at camera + direction * this distance (world
	// units, full camera height, identity rotation)
	// [orig: render_celestial_bodies @ 0x5acaa0, constant 64.0].
	static float celestial_body_distance();

	// Witnessed body alphas (0..1 out): sun = (1 - overcast) x (100 - dim)/100;
	// moon = clamp01((fogDistInt - 400)/600) x (1 - overcast)
	// [orig: @ 0x5acbc1..0x5acccd]. overcast/dim in 0..1 / 0..100.
	static float celestial_sun_alpha(float p_overcast_blend, float p_sun_dim_pct);
	static float celestial_moon_alpha(float p_fog_distance, float p_overcast_blend);

	// The glare glow alpha (0..1): dot_view^4/2 x brightness(0..256)/256 x the
	// overcast and SunDim folds [orig: render_skybox_sun_glow @ 0x5acfb8..0x5ad0a9].
	static float glare_glow_alpha(float p_view_dot_sun, int p_brightness,
			float p_overcast_blend, float p_sun_dim_pct);

	// Fog-start policy for a bare (no EnvFile) owner: the same
	// compute_fog_params table the instance getters use — fog-start policy
	// has ONE home [orig: Render_SetFogState @ 0x58a950].
	static float fog_start_for(int p_fog_type, float p_fog_end, float p_overcast = 0.0f);

	// The steady-state layer-1 cloud UV drift per second for a parsed
	// sky_speed (rate = sky_speed << 10 through 62 Hz x 2^-28) — the single
	// home of the old "sky_speed * 1024 * 62 / 2^28" magic; owners with a live
	// weather node read the RAMPING rate off it instead
	// [orig: rate ramp @ 0x57eecc; accumulators @ 0x57f1a5].
	static float cloud_uv_rate_per_second(float p_sky_speed);

	// Field name -> renderer-consumption status for editor badging:
	// {"status": "honored"|"partial"|"unconsumed", "faithful": bool,
	//  "anchor": String, "note": String}. Statuses mirror
	// docs/env/env-honored-matrix.md and flip only with grill citations.
	static Dictionary get_field_consumption();

	// BMS mission override layer. Recognized keys: water_height (float),
	// fog_level (float), fog_color (Color), water_color (Color),
	// water_murk (float), start_time (int HHMM).
	void apply_mission_overrides(const Dictionary &p_overrides);
	void clear_mission_overrides();
	bool has_mission_overrides() const;

	// Byte snapshots of the BASE config (CRLF .env text) for editor undo.
	PackedByteArray to_bytes() const;
	bool load_bytes(const PackedByteArray &p_bytes);
};

class EnvFileLoader : public ResourceFormatLoader {
	GDCLASS(EnvFileLoader, ResourceFormatLoader)

protected:
	static void _bind_methods() {}

public:
	PackedStringArray _get_recognized_extensions() const override;
	bool _handles_type(const StringName &p_type) const override;
	String _get_resource_type(const String &p_path) const override;
	PackedStringArray _get_dependencies(const String &p_path, bool p_add_types) const override;
	Variant _load(const String &p_path, const String &p_original_path, bool p_use_sub_threads, int32_t p_cache_mode) const override;
};

class EnvFileSaver : public ResourceFormatSaver {
	GDCLASS(EnvFileSaver, ResourceFormatSaver)

protected:
	static void _bind_methods() {}

public:
	Error _save(const Ref<Resource> &p_resource, const String &p_path, uint32_t p_flags) override;
	bool _recognize(const Ref<Resource> &p_resource) const override;
	PackedStringArray _get_recognized_extensions(const Ref<Resource> &p_resource) const override;
};

} // namespace godot
