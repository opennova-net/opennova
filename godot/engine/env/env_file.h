#pragma once

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/classes/resource_format_loader.hpp>
#include <godot_cpp/classes/resource_format_saver.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/class_db.hpp>
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

	void _sync_env_from_properties();
	void _sync_properties_from_env();
	void _notify_environment_changed();
	void _on_keyframe_changed();
	void _connect_keyframes();
	void _disconnect_keyframes();
	void _load_sky_textures();

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
