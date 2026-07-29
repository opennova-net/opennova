#include "env/env_file.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "util/nova_data_format.h"
#include "util/pcx_texture_bridge.h"
#include "util/texture_path_resolver.h"

#include <env/env_celestial.h>
#include <env/env_weather.h>

#include <algorithm>
#include <sstream>

using namespace godot;

namespace {

Color to_color(const opennova::env::Rgb &rgb) {
	return Color(rgb.r, rgb.g, rgb.b);
}

opennova::env::Rgb to_rgb(const Color &color) {
	return {
		static_cast<float>(color.r),
		static_cast<float>(color.g),
		static_cast<float>(color.b),
	};
}

Vector3 to_vector3(const opennova::env::Rgb &rgb) {
	return Vector3(rgb.r, rgb.g, rgb.b);
}

Vector3 to_vector3(const opennova::env::Vec3 &value) {
	return Vector3(value.x, value.y, value.z);
}

int clamp_time(int time) {
	return std::max(0, std::min(2359, time));
}

float clamp_water_murk(float value) {
	return std::max(0.0f, std::min(0.99f, value));
}

} // namespace

EnvFile::EnvFile() {
	env = opennova::env::make_default_config();
	_sync_properties_from_env();
	loaded = true;
}

void EnvFile::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_source_path", "path"), &EnvFile::set_source_path);
	ClassDB::bind_method(D_METHOD("get_source_path"), &EnvFile::get_source_path);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "source_path", PROPERTY_HINT_FILE, "*.env"), "set_source_path", "get_source_path");

	ClassDB::bind_method(D_METHOD("load"), &EnvFile::load);
	ClassDB::bind_method(D_METHOD("load_from_resource_root", "resource_root", "name"), &EnvFile::load_from_resource_root);
	ClassDB::bind_method(D_METHOD("save_to_path", "path"), &EnvFile::save_to_path);
	ClassDB::bind_method(D_METHOD("reset_to_default"), &EnvFile::reset_to_default);
	ClassDB::bind_method(D_METHOD("is_loaded"), &EnvFile::is_loaded);
	ClassDB::bind_method(D_METHOD("interpolate_time_of_day", "time"), &EnvFile::interpolate_time_of_day);
	ClassDB::bind_method(D_METHOD("compute_sun_direction", "time"), &EnvFile::compute_sun_direction);
	ClassDB::bind_method(D_METHOD("compute_moon_direction", "time"), &EnvFile::compute_moon_direction);
	ClassDB::bind_method(D_METHOD("has_water_height"), &EnvFile::has_water_height);
	ClassDB::bind_method(D_METHOD("_on_keyframe_changed"), &EnvFile::_on_keyframe_changed);

	ClassDB::bind_method(D_METHOD("get_fog_start", "overcast"), &EnvFile::get_fog_start, DEFVAL(0.0f));
	ClassDB::bind_method(D_METHOD("get_fog_density"), &EnvFile::get_fog_density);
	ClassDB::bind_method(D_METHOD("get_fog_end_distance", "overcast"), &EnvFile::get_fog_end_distance, DEFVAL(0.0f));
	ClassDB::bind_method(D_METHOD("get_fog_end_underwater"), &EnvFile::get_fog_end_underwater);
	ClassDB::bind_method(D_METHOD("get_day_phase", "time"), &EnvFile::get_day_phase);
	ClassDB::bind_static_method("EnvFile", D_METHOD("double_saturate_color", "color"), &EnvFile::double_saturate_color);
	ClassDB::bind_static_method("EnvFile", D_METHOD("combine_terrain_light", "light", "sky"), &EnvFile::combine_terrain_light);
	ClassDB::bind_static_method("EnvFile", D_METHOD("lit_water_color", "water", "light"), &EnvFile::lit_water_color);
	ClassDB::bind_static_method("EnvFile", D_METHOD("horizon_blend_skyfog", "fog", "skyfog", "fog_distance", "fog_distance_reference"), &EnvFile::horizon_blend_skyfog);
	ClassDB::bind_static_method("EnvFile", D_METHOD("compute_sun_glare", "view_dot_sun", "occlusion_brightness"), &EnvFile::compute_sun_glare);
	ClassDB::bind_static_method("EnvFile", D_METHOD("tile_overlay_tint_factor", "terrain_tint"), &EnvFile::tile_overlay_tint_factor);
	ClassDB::bind_static_method("EnvFile", D_METHOD("build_sky_dome_arrays", "sky_height"), &EnvFile::build_sky_dome_arrays);
	ClassDB::bind_static_method("EnvFile", D_METHOD("dome_reference_height"), &EnvFile::dome_reference_height);
	ClassDB::bind_static_method("EnvFile", D_METHOD("cloud_uv_rate_per_second", "sky_speed"), &EnvFile::cloud_uv_rate_per_second);
	ClassDB::bind_static_method("EnvFile", D_METHOD("fog_start_for", "fog_type", "fog_end", "overcast"), &EnvFile::fog_start_for, DEFVAL(0.0f));
	ClassDB::bind_static_method("EnvFile", D_METHOD("celestial_body_distance"), &EnvFile::celestial_body_distance);
	ClassDB::bind_static_method("EnvFile", D_METHOD("celestial_sun_alpha", "overcast_blend", "sun_dim_pct"), &EnvFile::celestial_sun_alpha);
	ClassDB::bind_static_method("EnvFile", D_METHOD("celestial_moon_alpha", "fog_distance", "overcast_blend"), &EnvFile::celestial_moon_alpha);
	ClassDB::bind_static_method("EnvFile", D_METHOD("glare_glow_alpha", "view_dot_sun", "brightness", "overcast_blend", "sun_dim_pct"), &EnvFile::glare_glow_alpha);
	ClassDB::bind_static_method("EnvFile", D_METHOD("get_field_consumption"), &EnvFile::get_field_consumption);
	ClassDB::bind_method(D_METHOD("apply_mission_overrides", "overrides"), &EnvFile::apply_mission_overrides);
	ClassDB::bind_method(D_METHOD("clear_mission_overrides"), &EnvFile::clear_mission_overrides);
	ClassDB::bind_method(D_METHOD("has_mission_overrides"), &EnvFile::has_mission_overrides);
	ClassDB::bind_method(D_METHOD("to_bytes"), &EnvFile::to_bytes);
	ClassDB::bind_method(D_METHOD("load_bytes", "bytes"), &EnvFile::load_bytes);

#define BIND_PROP(type, name, setter, getter, hint, hint_string) \
	ClassDB::bind_method(D_METHOD(#setter, "value"), &EnvFile::setter); \
	ClassDB::bind_method(D_METHOD(#getter), &EnvFile::getter); \
	ADD_PROPERTY(PropertyInfo(type, #name, hint, hint_string), #setter, #getter);

	BIND_PROP(Variant::STRING, env_name, set_env_name, get_env_name, PROPERTY_HINT_NONE, "")
	BIND_PROP(Variant::STRING, timeofday, set_timeofday, get_timeofday, PROPERTY_HINT_NONE, "")

	ADD_GROUP("General", "");
	BIND_PROP(Variant::FLOAT, envscale, set_envscale, get_envscale, PROPERTY_HINT_RANGE, "0.1,4.0,0.01")
	BIND_PROP(Variant::INT, curtime, set_curtime, get_curtime, PROPERTY_HINT_RANGE, "0,2359,1")
	BIND_PROP(Variant::INT, advanced_clouds, set_advanced_clouds, get_advanced_clouds, PROPERTY_HINT_RANGE, "0,1,1")

	ADD_GROUP("Fog", "fog_");
	BIND_PROP(Variant::FLOAT, fog_level, set_fog_level, get_fog_level, PROPERTY_HINT_RANGE, "0,4096,1")
	BIND_PROP(Variant::INT, fog_type, set_fog_type, get_fog_type, PROPERTY_HINT_RANGE, "0,3,1")

#define BIND_COLOR(name, setter, getter) BIND_PROP(Variant::COLOR, name, setter, getter, PROPERTY_HINT_NONE, "")
	ADD_GROUP("Colors", "");
	BIND_COLOR(terrain_tint, set_terrain_tint, get_terrain_tint)
	BIND_COLOR(water_color, set_water_color, get_water_color)
	BIND_COLOR(cloud_tint, set_cloud_tint, get_cloud_tint)
	BIND_COLOR(vertex_tint, set_vertex_tint, get_vertex_tint)
	BIND_COLOR(lightning_color, set_lightning_color, get_lightning_color)
	BIND_COLOR(ceiling_color, set_ceiling_color, get_ceiling_color)
	BIND_COLOR(floor_color, set_floor_color, get_floor_color)
#undef BIND_COLOR

	ADD_GROUP("Water", "water_");
	BIND_PROP(Variant::FLOAT, water_height, set_water_height, get_water_height, PROPERTY_HINT_RANGE, "-1000,1000,0.1")
	BIND_PROP(Variant::FLOAT, water_murk, set_water_murk, get_water_murk, PROPERTY_HINT_RANGE, "0.0,0.99,0.01")

	ADD_GROUP("Iris", "iris_");
	BIND_PROP(Variant::FLOAT, iris_percent, set_iris_percent, get_iris_percent, PROPERTY_HINT_RANGE, "0,100,1")
	BIND_PROP(Variant::FLOAT, iris_center, set_iris_center, get_iris_center, PROPERTY_HINT_RANGE, "0,5,0.01")

	ADD_GROUP("Sky", "sky_");
	BIND_PROP(Variant::FLOAT, sky_speed, set_sky_speed, get_sky_speed, PROPERTY_HINT_RANGE, "0,100,1")
	BIND_PROP(Variant::FLOAT, sky_height, set_sky_height, get_sky_height, PROPERTY_HINT_RANGE, "10,500,1")
	BIND_PROP(Variant::STRING, sky_map1, set_sky_map1, get_sky_map1, PROPERTY_HINT_NONE, "")
	BIND_PROP(Variant::STRING, sky_map2, set_sky_map2, get_sky_map2, PROPERTY_HINT_NONE, "")

#undef BIND_PROP

	ClassDB::bind_method(D_METHOD("set_sky_map1_tex", "texture"), &EnvFile::set_sky_map1_tex);
	ClassDB::bind_method(D_METHOD("get_sky_map1_tex"), &EnvFile::get_sky_map1_tex);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "sky_map1_tex", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_sky_map1_tex", "get_sky_map1_tex");
	ClassDB::bind_method(D_METHOD("set_sky_map2_tex", "texture"), &EnvFile::set_sky_map2_tex);
	ClassDB::bind_method(D_METHOD("get_sky_map2_tex"), &EnvFile::get_sky_map2_tex);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "sky_map2_tex", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_sky_map2_tex", "get_sky_map2_tex");

	ADD_GROUP("Models", "");
	ClassDB::bind_method(D_METHOD("set_sun_3di", "value"), &EnvFile::set_sun_3di);
	ClassDB::bind_method(D_METHOD("get_sun_3di"), &EnvFile::get_sun_3di);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "sun_3di"), "set_sun_3di", "get_sun_3di");
	ClassDB::bind_method(D_METHOD("set_moon_3di", "value"), &EnvFile::set_moon_3di);
	ClassDB::bind_method(D_METHOD("get_moon_3di"), &EnvFile::get_moon_3di);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "moon_3di"), "set_moon_3di", "get_moon_3di");
	ClassDB::bind_method(D_METHOD("set_glare_3di", "value"), &EnvFile::set_glare_3di);
	ClassDB::bind_method(D_METHOD("get_glare_3di"), &EnvFile::get_glare_3di);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "glare_3di"), "set_glare_3di", "get_glare_3di");
	ClassDB::bind_method(D_METHOD("set_star_3di", "value"), &EnvFile::set_star_3di);
	ClassDB::bind_method(D_METHOD("get_star_3di"), &EnvFile::get_star_3di);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "star_3di"), "set_star_3di", "get_star_3di");

	ADD_GROUP("Time of Day", "tod_");
	ClassDB::bind_method(D_METHOD("set_tod_keyframes", "keyframes"), &EnvFile::set_tod_keyframes);
	ClassDB::bind_method(D_METHOD("get_tod_keyframes"), &EnvFile::get_tod_keyframes);
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "tod_keyframes", PROPERTY_HINT_ARRAY_TYPE, "NovaEnvKeyframe"), "set_tod_keyframes", "get_tod_keyframes");

	ADD_SIGNAL(MethodInfo("environment_changed"));
}

void EnvFile::set_source_path(const String &p_path) { source_path = p_path; resource_root.unref(); }
String EnvFile::get_source_path() const { return source_path; }

#define IMPL_SET_GET(field, setter, getter, arg_type, ret_type) \
	void EnvFile::setter(arg_type p_value) { field = p_value; _notify_environment_changed(); } \
	ret_type EnvFile::getter() const { return field; }

IMPL_SET_GET(env_name, set_env_name, get_env_name, const String &, String)
IMPL_SET_GET(timeofday, set_timeofday, get_timeofday, const String &, String)
IMPL_SET_GET(envscale, set_envscale, get_envscale, float, float)
void EnvFile::set_curtime(int p_value) { curtime = clamp_time(p_value); _notify_environment_changed(); }
int EnvFile::get_curtime() const { return curtime; }
IMPL_SET_GET(fog_level, set_fog_level, get_fog_level, float, float)
IMPL_SET_GET(fog_type, set_fog_type, get_fog_type, int, int)
IMPL_SET_GET(terrain_tint, set_terrain_tint, get_terrain_tint, const Color &, Color)
IMPL_SET_GET(water_color, set_water_color, get_water_color, const Color &, Color)
void EnvFile::set_water_height(float p_value) { water_height = p_value; water_height_set = true; _notify_environment_changed(); }
float EnvFile::get_water_height() const { return water_height; }
bool EnvFile::has_water_height() const { return water_height_set; }
IMPL_SET_GET(cloud_tint, set_cloud_tint, get_cloud_tint, const Color &, Color)
IMPL_SET_GET(vertex_tint, set_vertex_tint, get_vertex_tint, const Color &, Color)
IMPL_SET_GET(lightning_color, set_lightning_color, get_lightning_color, const Color &, Color)
IMPL_SET_GET(ceiling_color, set_ceiling_color, get_ceiling_color, const Color &, Color)
IMPL_SET_GET(floor_color, set_floor_color, get_floor_color, const Color &, Color)
void EnvFile::set_water_murk(float p_value) { water_murk = clamp_water_murk(p_value); _notify_environment_changed(); }
float EnvFile::get_water_murk() const { return water_murk; }
IMPL_SET_GET(iris_percent, set_iris_percent, get_iris_percent, float, float)
IMPL_SET_GET(iris_center, set_iris_center, get_iris_center, float, float)
IMPL_SET_GET(sky_speed, set_sky_speed, get_sky_speed, float, float)
IMPL_SET_GET(sky_height, set_sky_height, get_sky_height, float, float)
// The sky-map setters re-resolve the cached textures so the live sky (and any
// editor preview) tracks the edit; load_bytes/load are otherwise the only
// resolution points. Diff-guarded: undo snapshot replays with an unchanged
// name must not hit the disk.
void EnvFile::set_sky_map1(const String &p_value) {
	if (sky_map1 == p_value) {
		return;
	}
	sky_map1 = p_value;
	_load_sky_textures();
	_notify_environment_changed();
}
String EnvFile::get_sky_map1() const { return sky_map1; }
void EnvFile::set_sky_map2(const String &p_value) {
	if (sky_map2 == p_value) {
		return;
	}
	sky_map2 = p_value;
	_load_sky_textures();
	_notify_environment_changed();
}
String EnvFile::get_sky_map2() const { return sky_map2; }
IMPL_SET_GET(sun_3di, set_sun_3di, get_sun_3di, const String &, String)
IMPL_SET_GET(moon_3di, set_moon_3di, get_moon_3di, const String &, String)
IMPL_SET_GET(glare_3di, set_glare_3di, get_glare_3di, const String &, String)
IMPL_SET_GET(star_3di, set_star_3di, get_star_3di, const String &, String)
IMPL_SET_GET(sky_map1_tex, set_sky_map1_tex, get_sky_map1_tex, const Ref<Texture2D> &, Ref<Texture2D>)
IMPL_SET_GET(sky_map2_tex, set_sky_map2_tex, get_sky_map2_tex, const Ref<Texture2D> &, Ref<Texture2D>)
IMPL_SET_GET(advanced_clouds, set_advanced_clouds, get_advanced_clouds, int, int)

#undef IMPL_SET_GET

void EnvFile::set_tod_keyframes(const TypedArray<NovaEnvKeyframe> &p_keyframes) {
	_disconnect_keyframes();
	tod_keyframes = p_keyframes;
	_connect_keyframes();
	_notify_environment_changed();
}

TypedArray<NovaEnvKeyframe> EnvFile::get_tod_keyframes() const { return tod_keyframes; }

void EnvFile::_disconnect_keyframes() {
	for (int i = 0; i < tod_keyframes.size(); ++i) {
		Ref<NovaEnvKeyframe> keyframe = tod_keyframes[i];
		if (keyframe.is_valid() && keyframe->is_connected("changed", Callable(this, "_on_keyframe_changed"))) {
			keyframe->disconnect("changed", Callable(this, "_on_keyframe_changed"));
		}
	}
}

void EnvFile::_connect_keyframes() {
	for (int i = 0; i < tod_keyframes.size(); ++i) {
		Ref<NovaEnvKeyframe> keyframe = tod_keyframes[i];
		if (keyframe.is_valid() && !keyframe->is_connected("changed", Callable(this, "_on_keyframe_changed"))) {
			keyframe->connect("changed", Callable(this, "_on_keyframe_changed"));
		}
	}
}

void EnvFile::_sync_env_from_properties() {
	env.name = env_name.utf8().get_data();
	env.timeofday = timeofday.utf8().get_data();
	env.envscale = envscale;
	env.curtime = curtime;
	env.fog_level = fog_level;
	env.fog_type = fog_type;
	env.terrain_rgb = to_rgb(terrain_tint);
	env.water_rgb = to_rgb(water_color);
	env.water_height = water_height;
	env.water_height_set = water_height_set;
	env.cloud_rgb = to_rgb(cloud_tint);
	env.vertex_rgb = to_rgb(vertex_tint);
	env.lightning_rgb = to_rgb(lightning_color);
	env.ceiling_rgb = to_rgb(ceiling_color);
	env.floor_rgb = to_rgb(floor_color);
	env.water_murk = water_murk;
	env.iris_percent = iris_percent;
	env.iris_center = iris_center;
	env.sky_speed = sky_speed;
	env.sky_height = sky_height;
	env.sky_map1 = sky_map1.utf8().get_data();
	env.sky_map2 = sky_map2.utf8().get_data();
	env.sun_3di = sun_3di.utf8().get_data();
	env.moon_3di = moon_3di.utf8().get_data();
	env.glare_3di = glare_3di.utf8().get_data();
	env.star_3di = star_3di.utf8().get_data();
	env.advanced_clouds = advanced_clouds;
	env.keyframes.clear();
	for (int i = 0; i < tod_keyframes.size(); ++i) {
		Ref<NovaEnvKeyframe> keyframe = tod_keyframes[i];
		if (keyframe.is_valid()) {
			env.keyframes.push_back(keyframe->to_native());
		}
	}
	std::stable_sort(env.keyframes.begin(), env.keyframes.end(), [](const opennova::env::Keyframe &a, const opennova::env::Keyframe &b) {
		return a.time < b.time;
	});
}

void EnvFile::_sync_properties_from_env() {
	env_name = String(env.name.c_str());
	timeofday = String(env.timeofday.c_str());
	envscale = env.envscale;
	curtime = env.curtime;
	fog_level = env.fog_level;
	fog_type = env.fog_type;
	terrain_tint = to_color(env.terrain_rgb);
	water_color = to_color(env.water_rgb);
	water_height = env.water_height;
	water_height_set = env.water_height_set;
	cloud_tint = to_color(env.cloud_rgb);
	vertex_tint = to_color(env.vertex_rgb);
	lightning_color = to_color(env.lightning_rgb);
	ceiling_color = to_color(env.ceiling_rgb);
	floor_color = to_color(env.floor_rgb);
	water_murk = env.water_murk;
	iris_percent = env.iris_percent;
	iris_center = env.iris_center;
	sky_speed = env.sky_speed;
	sky_height = env.sky_height;
	sky_map1 = String(env.sky_map1.c_str());
	sky_map2 = String(env.sky_map2.c_str());
	sun_3di = String(env.sun_3di.c_str());
	moon_3di = String(env.moon_3di.c_str());
	glare_3di = String(env.glare_3di.c_str());
	star_3di = String(env.star_3di.c_str());
	advanced_clouds = env.advanced_clouds;

	_disconnect_keyframes();
	tod_keyframes.clear();
	for (const opennova::env::Keyframe &native_keyframe : env.keyframes) {
		Ref<NovaEnvKeyframe> keyframe;
		keyframe.instantiate();
		keyframe->copy_from_native(native_keyframe);
		tod_keyframes.push_back(keyframe);
	}
	_connect_keyframes();
}

void EnvFile::_notify_environment_changed() {
	_sync_env_from_properties();
	emit_signal("environment_changed");
	emit_changed();
}

void EnvFile::_on_keyframe_changed() {
	_notify_environment_changed();
}

void EnvFile::_load_sky_textures() {
	sky_map1_tex.unref();
	sky_map2_tex.unref();
	if (resource_root.is_valid()) {
		sky_map1_tex = _load_sky_map_texture(sky_map1);
		sky_map2_tex = _load_sky_map_texture(sky_map2);
		return;
	}
	if (source_path.is_empty()) {
		return;
	}
	const String dir = source_path.get_base_dir();
	sky_map1_tex = _load_sky_map_texture_from_dir(dir, sky_map1);
	sky_map2_tex = _load_sky_map_texture_from_dir(dir, sky_map2);
}

// The sky maps load through the dedicated render-texture loader, which
// synthesizes the PCX alpha channel from the image's own palette luminance —
// the cloud combine's alpha (a1 = t0.a*t1.a*2) rides it, so a plain opaque
// decode makes the cloud pass cover the whole dome
// [orig: terrain_init_rendering_resources @ 0x578a97/0x578aa5 ->
// load_texture_from_archive @ 0x58b980, PCX alpha loop @ 0x58bc35..0x58bcee].
// Non-PCX names (DDS/TGA) keep the generic decode like retail's DDS-first path.
Ref<Texture2D> EnvFile::_load_sky_map_texture(const String &name) {
	if (name.get_extension().to_lower() == "pcx") {
		const PackedByteArray bytes = resource_root->read_file(name);
		if (!bytes.is_empty()) {
			Ref<Texture2D> tex = opennova::build_pcx_luminance_alpha_texture(bytes);
			if (tex.is_valid()) {
				return tex;
			}
		}
	}
	return resource_root->load_texture(name);
}

Ref<Texture2D> EnvFile::_load_sky_map_texture_from_dir(const String &dir, const String &name) {
	if (name.get_extension().to_lower() == "pcx") {
		PackedByteArray bytes;
		if (read_nova_payload_file(dir.path_join(name), bytes) && !bytes.is_empty()) {
			Ref<Texture2D> tex = opennova::build_pcx_luminance_alpha_texture(bytes);
			if (tex.is_valid()) {
				return tex;
			}
		}
	}
	return opennova::load_texture_from_dir(dir, name);
}

Error EnvFile::load() {
	loaded = false;
	mission_overrides_active = false;
	resource_root.unref();
	if (source_path.is_empty()) {
		return ERR_INVALID_PARAMETER;
	}

	PackedByteArray bytes;
	if (!read_nova_payload_file(source_path, bytes)) {
		UtilityFunctions::push_warning("[EnvFile] Cannot open: ", source_path);
		return ERR_FILE_CANT_READ;
	}

	std::string text(reinterpret_cast<const char *>(bytes.ptr()), static_cast<size_t>(bytes.size()));
	std::istringstream input(text);
	std::string error;
	env = opennova::env::Config();
	if (!opennova::env::load_env(input, env, error)) {
		UtilityFunctions::push_warning("[EnvFile] ", error.c_str());
		return ERR_FILE_CANT_READ;
	}

	_sync_properties_from_env();
	_load_sky_textures();
	loaded = true;
	_notify_environment_changed();
	return OK;
}

Error EnvFile::load_from_resource_root(const Ref<NovaResourceRoot> &p_resource_root, const String &p_name) {
	loaded = false;
	mission_overrides_active = false;
	resource_root.unref();
	if (p_resource_root.is_null() || p_resource_root->get_root_dir().is_empty()) {
		return ERR_INVALID_PARAMETER;
	}
	const String file = p_name.get_file();
	if (file.is_empty()) {
		return ERR_INVALID_PARAMETER;
	}
	const PackedByteArray bytes = p_resource_root->read_file(file);
	if (bytes.is_empty()) {
		UtilityFunctions::push_warning("[EnvFile] Cannot open mounted resource: ", file);
		return ERR_FILE_CANT_READ;
	}

	std::string text(reinterpret_cast<const char *>(bytes.ptr()), static_cast<size_t>(bytes.size()));
	std::istringstream input(text);
	std::string error;
	env = opennova::env::Config();
	if (!opennova::env::load_env(input, env, error)) {
		UtilityFunctions::push_warning("[EnvFile] ", error.c_str());
		return ERR_FILE_CANT_READ;
	}

	source_path = file;
	resource_root = p_resource_root;
	_sync_properties_from_env();
	_load_sky_textures();
	loaded = true;
	_notify_environment_changed();
	return OK;
}

Error EnvFile::save_to_path(const String &p_path) {
	if (!mission_overrides_active) {
		_sync_env_from_properties();
	}
	std::ostringstream output;
	std::string error;
	// Mission overrides are a live-view layer; the file always gets the base.
	const opennova::env::Config &to_save = mission_overrides_active ? env_base : env;
	if (!opennova::env::save_env(output, to_save, error)) {
		UtilityFunctions::push_warning("[EnvFile] Save failed: ", error.c_str());
		return ERR_FILE_CANT_WRITE;
	}

	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
	if (file.is_null()) {
		return ERR_FILE_CANT_WRITE;
	}
	const std::string content = output.str();
	file->store_string(String::utf8(content.c_str(), content.size()));
	file->close();
	return OK;
}

void EnvFile::reset_to_default() {
	env = opennova::env::make_default_config();
	mission_overrides_active = false;
	source_path = String();
	_sync_properties_from_env();
	loaded = true;
	_load_sky_textures();
	_notify_environment_changed();
}

bool EnvFile::is_loaded() const { return loaded; }

Dictionary EnvFile::interpolate_time_of_day(float p_time) const {
	Dictionary result;
	if (env.keyframes.empty()) {
		return result;
	}
	const opennova::env::TodState tod = opennova::env::interpolate_tod(env.keyframes, p_time, env.envscale);
	result["sun"] = to_vector3(tod.sun);
	result["ground"] = to_vector3(tod.ground);
	result["fog"] = to_vector3(tod.fog);
	result["sky"] = to_vector3(tod.sky);
	result["moon"] = to_vector3(tod.moon);
	result["skyfog"] = to_vector3(tod.skyfog);
	result["skybase"] = to_vector3(tod.skybase);
	result["skybright"] = to_vector3(tod.skybright);
	result["skyhighlight"] = to_vector3(tod.skyhighlight);
	result["cloudbase"] = to_vector3(tod.cloudbase);
	result["cloudhighlight"] = to_vector3(tod.cloudhighlight);
	result["cloudedge"] = to_vector3(tod.cloudedge);
	return result;
}

Vector3 EnvFile::compute_sun_direction(float p_time) const {
	return to_vector3(opennova::env::compute_sun_direction(p_time));
}

Vector3 EnvFile::compute_moon_direction(float p_time) const {
	return to_vector3(opennova::env::compute_moon_direction(p_time));
}

float EnvFile::get_fog_start(float p_overcast) const {
	const float end = get_fog_end_distance(p_overcast);
	return opennova::env::compute_fog_params(fog_type, end, p_overcast).start;
}

float EnvFile::get_fog_density() const {
	return opennova::env::compute_fog_params(fog_type, fog_level, 0.0f).exp_density;
}

float EnvFile::get_fog_end_distance(float p_overcast) const {
	return opennova::env::fog_end_above_water(fog_level, p_overcast);
}

float EnvFile::get_fog_end_underwater() const {
	return opennova::env::fog_end_underwater(water_murk);
}

Dictionary EnvFile::get_day_phase(float p_time) const {
	const opennova::env::DayPhase phase = opennova::env::compute_day_phase(p_time);
	Dictionary result;
	result["is_night"] = phase.is_night;
	result["blend"] = phase.blend;
	return result;
}

Color EnvFile::double_saturate_color(const Color &p_color) {
	return to_color(opennova::env::double_saturate(to_rgb(p_color)));
}

Color EnvFile::combine_terrain_light(const Color &p_light, const Color &p_sky) {
	return to_color(opennova::env::combine_terrain_light(to_rgb(p_light), to_rgb(p_sky)));
}

Color EnvFile::lit_water_color(const Color &p_water, const Color &p_light) {
	return to_color(opennova::env::lit_water_color(to_rgb(p_water), to_rgb(p_light)));
}

Color EnvFile::horizon_blend_skyfog(const Color &p_fog, const Color &p_skyfog,
		float p_fog_distance, float p_fog_distance_reference) {
	const auto to_fixed = [](float units) {
		if (units <= 0.0f) return 0u;
		return static_cast<uint32_t>(units * 65536.0f);
	};
	return to_color(opennova::env::horizon_blend_skyfog(
			to_rgb(p_fog), to_rgb(p_skyfog),
			to_fixed(p_fog_distance), to_fixed(p_fog_distance_reference)));
}

Color EnvFile::tile_overlay_tint_factor(const Color &p_terrain_tint) {
	// [orig: PolyTrn_RenderTile @ 0x60df0d] — DIFFUSE(HALF) x TEXTURE under
	// MODULATE2X, folded to one multiply for the shader.
	const opennova::env::TerrainTint tint =
			opennova::env::terrain_tint_from_rgb(to_rgb(p_terrain_tint));
	return to_color(opennova::env::tile_overlay_tint_factor(tint));
}

Array EnvFile::build_sky_dome_arrays(float p_sky_height) {
	// [orig: build_sky_dome_mesh @ 0x578db0] — libs/env owns the math; this
	// repacks the plain vectors into Mesh.ARRAY_* surface arrays.
	const opennova::env::SkyDomeMesh mesh = opennova::env::build_sky_dome_mesh(p_sky_height);
	const int vertex_count = static_cast<int>(mesh.positions.size() / 3);

	PackedVector3Array vertices;
	PackedVector3Array normals;
	PackedVector2Array uv1;
	PackedVector2Array uv2;
	vertices.resize(vertex_count);
	normals.resize(vertex_count);
	uv1.resize(vertex_count);
	uv2.resize(vertex_count);
	for (int i = 0; i < vertex_count; ++i) {
		vertices[i] = Vector3(mesh.positions[i * 3], mesh.positions[i * 3 + 1], mesh.positions[i * 3 + 2]);
		normals[i] = Vector3(mesh.normals[i * 3], mesh.normals[i * 3 + 1], mesh.normals[i * 3 + 2]);
		uv1[i] = Vector2(mesh.uv1[i * 2], mesh.uv1[i * 2 + 1]);
		uv2[i] = Vector2(mesh.uv2[i * 2], mesh.uv2[i * 2 + 1]);
	}
	PackedInt32Array indices;
	indices.resize(static_cast<int>(mesh.indices.size()));
	for (int i = 0; i < static_cast<int>(mesh.indices.size()); ++i) {
		indices[i] = mesh.indices[i];
	}

	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = vertices;
	arrays[Mesh::ARRAY_NORMAL] = normals;
	arrays[Mesh::ARRAY_TEX_UV] = uv1;
	arrays[Mesh::ARRAY_TEX_UV2] = uv2;
	arrays[Mesh::ARRAY_INDEX] = indices;
	return arrays;
}

float EnvFile::dome_reference_height() {
	return static_cast<float>(opennova::env::kSkyDomeReferenceHeight);
}

float EnvFile::fog_start_for(int p_fog_type, float p_fog_end, float p_overcast) {
	const opennova::env::FogParams params =
			opennova::env::compute_fog_params(p_fog_type, p_fog_end, p_overcast);
	return params.start;
}

float EnvFile::celestial_body_distance() {
	return opennova::env::kCelestialBodyDistance;
}

namespace {

int to_fixed_16_16(float value) {
	return static_cast<int>(value * 65536.0f);
}

} // namespace

float EnvFile::celestial_sun_alpha(float p_overcast_blend, float p_sun_dim_pct) {
	return static_cast<float>(opennova::env::celestial_sun_alpha_fixed(
			to_fixed_16_16(p_overcast_blend), to_fixed_16_16(p_sun_dim_pct))) / 65536.0f;
}

float EnvFile::celestial_moon_alpha(float p_fog_distance, float p_overcast_blend) {
	return static_cast<float>(opennova::env::celestial_moon_alpha_fixed(
			p_fog_distance, to_fixed_16_16(p_overcast_blend), false)) / 65536.0f;
}

float EnvFile::glare_glow_alpha(float p_view_dot_sun, int p_brightness,
		float p_overcast_blend, float p_sun_dim_pct) {
	return static_cast<float>(opennova::env::glare_glow_alpha_fixed(
			to_fixed_16_16(p_view_dot_sun), p_brightness,
			to_fixed_16_16(p_overcast_blend), to_fixed_16_16(p_sun_dim_pct))) / 65536.0f;
}

float EnvFile::cloud_uv_rate_per_second(float p_sky_speed) {
	// Steady state: the ramp's target rate (sky_speed << 10) through the
	// per-second layer-1 UV scale [orig: @ 0x57eecc; @ 0x57f1a5].
	opennova::env::CloudScrollState steady;
	steady.rate = static_cast<int>(p_sky_speed) << 10;
	return opennova::env::cloud_uv_rate_per_second(steady);
}

Dictionary EnvFile::compute_sun_glare(float p_view_dot_sun, int p_occlusion_brightness) {
	const opennova::env::GlareResult glare = opennova::env::compute_sun_glare(p_view_dot_sun, p_occlusion_brightness);
	Dictionary result;
	result["glare"] = glare.glare;
	result["fog_whiten"] = glare.fog_whiten;
	return result;
}

Dictionary EnvFile::get_field_consumption() {
	Dictionary table;
	const auto add = [&table](const char *p_field, const char *p_status, bool p_faithful, const char *p_anchor, const char *p_note) {
		Dictionary row;
		row["status"] = String(p_status);
		row["faithful"] = p_faithful;
		row["anchor"] = String(p_anchor);
		row["note"] = String(p_note);
		table[String(p_field)] = row;
	};

	add("env_name", "unconsumed", true, "authoring extension; no retail keyword",
			"A label for this file. The game never reads it.");
	add("timeofday", "unconsumed", true, "classification string only",
			"A label for this file. The game never reads it.");
	add("curtime", "honored", false, "Environment_SetCurrentTime @ 0x57c4b0", "");
	add("envscale", "honored", false, "Env_ParseEnvScale @ 0x840950", "");
	add("fog_level", "honored", false, "Render_SetFogState @ 0x58a950", "");
	add("fog_type", "honored", false, "Render_SetFogState @ 0x58a950", "");
	add("terrain_tint", "partial", false,
			"PolyTrn_InitTextures @ 0x60b8cb; PolyTrn_RenderTile @ 0x60df0d; sample_terrain_colormap_tinted @ 0x606030",
			"In the game this tints the ground, shoreline water, and plants. That part of the picture isn't built yet, so edits won't show in the preview.");
	add("vertex_tint", "unconsumed", true, "vestigial; retail modulator identity",
			"The game itself never uses this value; it's kept so files save back unchanged.");
	add("water_color", "honored", false, "TimeOfDay_ParseProperty @ 0x57c590", "");
	add("water_height", "honored", false, "water_height << 15 parse", "");
	add("water_murk", "honored", false, "Environment_SetWaterMurk @ 0x57d4f0", "");
	add("iris_percent", "honored", false,
			"terrain_sector_compute_lighting @ 0x5c7550; modulator chain @ 0x57e512/0x57ef97",
			"The game's automatic exposure - how the view brightens in dark scenes. The outdoor exposure runs; looking into buildings does not dim yet (interiors aren't built).");
	add("iris_center", "honored", false,
			"terrain_sector_compute_lighting @ 0x5c7550; modulator chain @ 0x57e512/0x57ef97",
			"The game's automatic exposure - how the view brightens in dark scenes. The outdoor exposure runs; looking into buildings does not dim yet (interiors aren't built).");
	add("ceiling_color", "partial", false, "indoor exposure inputs @ 0x5c7646; Env_CeilingFloorBlend @ 0x5f7163",
			"Indoor light color. It takes effect when indoor lighting is built.");
	add("floor_color", "partial", false, "indoor exposure inputs @ 0x5c7646; Env_CeilingFloorBlend @ 0x5f7163",
			"Indoor light color. It takes effect when indoor lighting is built.");
	add("lightning_color", "partial", false, "Environment_SetLightningFlash @ 0x57d320",
			"Lightning flash color. Storms aren't triggered yet, so flashes don't fire in the preview.");
	add("cloud_tint", "honored", false, "render_skybox @ 0x579b42",
			"Colors the whole sky only when advanced clouds are off. With advanced clouds on, the sky uses the cloud keyframe colors instead.");
	add("sky_speed", "honored", false, "render_skybox @ 0x5791de", "");
	add("sky_height", "honored", false, "build_sky_dome_mesh @ 0x578db0", "");
	add("sky_map1", "honored", false, "Path_ReplaceOrAppendExtension @ 0x57cc4b", "");
	add("sky_map2", "honored", false, "Path_ReplaceOrAppendExtension @ 0x57cc4b", "");
	add("advanced_clouds", "honored", false, "render_skybox fixed-function pass @ 0x579b42", "");
	add("sun_3di", "honored", false, "EffectWorld_LoadCelestialModels @ 0x5adc50", "");
	add("moon_3di", "honored", false, "EffectWorld_LoadCelestialModels @ 0x5adc50", "");
	add("star_3di", "honored", false, "EffectWorld_LoadCelestialModels @ 0x5adc50", "");
	add("glare_3di", "partial", false, "render_skybox_sun_glow @ 0x5acd00",
			"The sun glare always shows at full strength; hills don't block it yet.");
	return table;
}

void EnvFile::apply_mission_overrides(const Dictionary &p_overrides) {
	if (!mission_overrides_active) {
		_sync_env_from_properties();
		env_base = env;
	}
	opennova::env::BmsEnvOverrides overrides;
	if (p_overrides.has("water_height")) {
		overrides.has_water_height = true;
		overrides.water_height = static_cast<float>(p_overrides["water_height"]);
	}
	if (p_overrides.has("fog_level")) {
		overrides.has_fog_level = true;
		overrides.fog_level = static_cast<float>(p_overrides["fog_level"]);
	}
	if (p_overrides.has("fog_color")) {
		overrides.has_fog_color = true;
		overrides.fog_color = to_rgb(p_overrides["fog_color"]);
	}
	if (p_overrides.has("water_color")) {
		overrides.has_water_color = true;
		overrides.water_color = to_rgb(p_overrides["water_color"]);
	}
	if (p_overrides.has("water_murk")) {
		overrides.has_water_murk = true;
		overrides.water_murk = static_cast<float>(p_overrides["water_murk"]);
	}
	if (p_overrides.has("start_time")) {
		overrides.has_start_time = true;
		overrides.start_time = static_cast<int>(p_overrides["start_time"]);
	}
	env = env_base;
	opennova::env::apply_bms_overrides(env, overrides);
	mission_overrides_active = true;
	_sync_properties_from_env();
	emit_signal("environment_changed");
	emit_changed();
}

void EnvFile::clear_mission_overrides() {
	if (!mission_overrides_active) {
		return;
	}
	env = env_base;
	mission_overrides_active = false;
	_sync_properties_from_env();
	emit_signal("environment_changed");
	emit_changed();
}

bool EnvFile::has_mission_overrides() const {
	return mission_overrides_active;
}

PackedByteArray EnvFile::to_bytes() const {
	std::ostringstream output;
	std::string error;
	const opennova::env::Config &to_save = mission_overrides_active ? env_base : env;
	PackedByteArray bytes;
	if (!opennova::env::save_env(output, to_save, error)) {
		return bytes;
	}
	const std::string content = output.str();
	bytes.resize(static_cast<int64_t>(content.size()));
	memcpy(bytes.ptrw(), content.data(), content.size());
	return bytes;
}

bool EnvFile::load_bytes(const PackedByteArray &p_bytes) {
	std::string text(reinterpret_cast<const char *>(p_bytes.ptr()), static_cast<size_t>(p_bytes.size()));
	std::istringstream input(text);
	std::string error;
	opennova::env::Config parsed;
	if (!opennova::env::load_env(input, parsed, error)) {
		UtilityFunctions::push_warning("[EnvFile] load_bytes: ", error.c_str());
		return false;
	}
	// Re-resolve sky textures only when the names changed — undo snapshots must
	// not hit the disk per step.
	const bool maps_changed = parsed.sky_map1 != env.sky_map1 || parsed.sky_map2 != env.sky_map2;
	env = parsed;
	mission_overrides_active = false;
	_sync_properties_from_env();
	if (maps_changed) {
		_load_sky_textures();
	}
	loaded = true;
	emit_signal("environment_changed");
	emit_changed();
	return true;
}

PackedStringArray EnvFileLoader::_get_recognized_extensions() const {
	PackedStringArray extensions;
	extensions.push_back("env");
	extensions.push_back("ENV");
	return extensions;
}

bool EnvFileLoader::_handles_type(const StringName &p_type) const {
	return p_type == StringName("EnvFile") || p_type == StringName("Resource");
}

String EnvFileLoader::_get_resource_type(const String &p_path) const {
	if (p_path.get_extension().to_lower() == "env") {
		return "EnvFile";
	}
	return String();
}

PackedStringArray EnvFileLoader::_get_dependencies(const String &p_path, bool p_add_types) const {
	(void)p_add_types;
	PackedStringArray dependencies;

	PackedByteArray bytes;
	if (!read_nova_payload_file(p_path, bytes)) {
		return dependencies;
	}

	opennova::env::Config cfg;
	std::string error;
	std::string text(reinterpret_cast<const char *>(bytes.ptr()), static_cast<size_t>(bytes.size()));
	std::istringstream input(text);
	if (!opennova::env::load_env(input, cfg, error)) {
		return dependencies;
	}

	const String dir = p_path.get_base_dir();
	if (!cfg.sky_map1.empty()) {
		const String resolved = opennova::resolve_texture_path(dir, String(cfg.sky_map1.c_str()));
		dependencies.push_back(resolved.is_empty() ? dir.path_join(String(cfg.sky_map1.c_str())) : resolved);
	}
	if (!cfg.sky_map2.empty()) {
		const String resolved = opennova::resolve_texture_path(dir, String(cfg.sky_map2.c_str()));
		dependencies.push_back(resolved.is_empty() ? dir.path_join(String(cfg.sky_map2.c_str())) : resolved);
	}
	return dependencies;
}

Variant EnvFileLoader::_load(const String &p_path, const String &p_original_path, bool p_use_sub_threads, int32_t p_cache_mode) const {
	(void)p_original_path;
	(void)p_use_sub_threads;
	(void)p_cache_mode;
	Ref<EnvFile> data;
	data.instantiate();
	data->set_source_path(p_path);
	const Error err = data->load();
	if (err != OK) {
		return Variant();
	}
	return data;
}

Error EnvFileSaver::_save(const Ref<Resource> &p_resource, const String &p_path, uint32_t p_flags) {
	(void)p_flags;
	Ref<EnvFile> data = p_resource;
	if (data.is_null()) {
		return ERR_INVALID_PARAMETER;
	}
	return data->save_to_path(p_path);
}

bool EnvFileSaver::_recognize(const Ref<Resource> &p_resource) const {
	return Object::cast_to<EnvFile>(p_resource.ptr()) != nullptr;
}

PackedStringArray EnvFileSaver::_get_recognized_extensions(const Ref<Resource> &p_resource) const {
	PackedStringArray extensions;
	if (_recognize(p_resource)) {
		extensions.push_back("env");
	}
	return extensions;
}
