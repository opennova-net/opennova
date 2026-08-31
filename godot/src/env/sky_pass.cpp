#include "env/sky_pass.h"

#include "env/env_axes.h"

#include "env/env_render_camera.h"
#include "env/mission_environment.h"
#include "env/weather.h"

namespace godot {

namespace {

// source_color uniforms take Colors: the witnessed palette bytes decode
// canonically to the linear scene (ADR 0043 linear-scene amendment).
Color to_color(const opennova::env::Rgb &rgb) {
	return Color(rgb.r, rgb.g, rgb.b);
}

} // namespace

void SkyPass::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_environment_path", "path"),
			&SkyPass::set_environment_path);
	ClassDB::bind_method(D_METHOD("get_environment_path"),
			&SkyPass::get_environment_path);
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "environment_path"),
			"set_environment_path", "get_environment_path");
	ClassDB::bind_method(D_METHOD("set_weather_path", "path"),
			&SkyPass::set_weather_path);
	ClassDB::bind_method(D_METHOD("get_weather_path"),
			&SkyPass::get_weather_path);
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "weather_path"),
			"set_weather_path", "get_weather_path");
	ClassDB::bind_method(D_METHOD("get_sky_material"),
			&SkyPass::get_sky_material);
	// The externally-callable render-frame drive (the _process body): the
	// test harness drives frames here; the engine's virtual delegates in.
	ClassDB::bind_method(D_METHOD("advance_frame", "delta"),
			&SkyPass::advance_frame);
}

void SkyPass::set_environment_path(const NodePath &p_path) {
	environment_path_ = p_path;
	env_node_id_ = ObjectID();
}

void SkyPass::set_weather_path(const NodePath &p_path) {
	weather_path_ = p_path;
	weather_node_id_ = ObjectID();
}

Ref<ShaderMaterial> SkyPass::get_sky_material() {
	MissionEnvironment *env = _env_node();
	return env != nullptr ? env->get_sky_material() : Ref<ShaderMaterial>();
}

MissionEnvironment *SkyPass::_env_node() {
	if (env_node_id_.is_valid()) {
		MissionEnvironment *env = Object::cast_to<MissionEnvironment>(
				ObjectDB::get_instance(env_node_id_));
		if (env != nullptr && env->is_inside_tree()) {
			return env;
		}
	}
	// Lazy (re-)resolution supports owners that create the environment after
	// this node; relative sibling paths also resolve off-tree.
	if (environment_path_.is_empty() ||
			(!is_inside_tree() && environment_path_.is_absolute())) {
		return nullptr;
	}
	MissionEnvironment *env = Object::cast_to<MissionEnvironment>(
			get_node_or_null(environment_path_));
	env_node_id_ = env != nullptr ? ObjectID(env->get_instance_id())
								  : ObjectID();
	return env;
}

Weather *SkyPass::_weather_node() {
	if (weather_node_id_.is_valid()) {
		Weather *weather = Object::cast_to<Weather>(
				ObjectDB::get_instance(weather_node_id_));
		if (weather != nullptr && weather->is_inside_tree()) {
			return weather;
		}
	}
	if (weather_path_.is_empty() ||
			(!is_inside_tree() && weather_path_.is_absolute())) {
		return nullptr;
	}
	Weather *weather =
			Object::cast_to<Weather>(get_node_or_null(weather_path_));
	weather_node_id_ = weather != nullptr ? ObjectID(weather->get_instance_id())
										  : ObjectID();
	return weather;
}

void SkyPass::_ready() {
	set_process(true);
}

void SkyPass::_process(double p_delta) {
	advance_frame(p_delta);
}

void SkyPass::advance_frame(double p_delta) {
	MissionEnvironment *env = _env_node();
	Ref<ShaderMaterial> material =
			env != nullptr ? env->get_sky_material() : Ref<ShaderMaterial>();
	if (material.is_null()) {
		return;
	}
	// Camera3D.current can change while the old camera remains alive.
	// Re-resolve that transition instead of continuing to follow a stale,
	// still-in-tree camera. The camera feeds only the cloud scroll's world
	// x/z terms — the sky shader anchors itself per pass through POSITION.
	Camera3D *cam = Object::cast_to<Camera3D>(
			ObjectDB::get_instance(cached_cam_id_));
	if (cam == nullptr || !cam->is_inside_tree() || !cam->is_current()) {
		cam = find_env_render_camera(this);
		cached_cam_id_ = cam != nullptr ? ObjectID(cam->get_instance_id())
										: ObjectID();
	}

	const opennova::env::SkyFrameState frame =
			opennova::env::build_sky_frame(env->state());
	if (frame.loaded) {
		if (frame.flat_pass) {
			material->set_shader_parameter("u_flat_pass", true);
			material->set_shader_parameter("u_flat_color",
					to_color(frame.flat_color));
		} else {
			material->set_shader_parameter("u_flat_pass", false);
			material->set_shader_parameter("u_sky_base",
					to_color(frame.sky_base));
			material->set_shader_parameter("u_sky_bright",
					to_color(frame.sky_bright));
			material->set_shader_parameter("u_sky_highlight",
					to_color(frame.sky_highlight));
			material->set_shader_parameter("u_cloud_base",
					to_color(frame.cloud_base));
			material->set_shader_parameter("u_cloud_highlight",
					to_color(frame.cloud_highlight));
			material->set_shader_parameter("u_cloud_edge",
					to_color(frame.cloud_edge));
		}
		// The sky shader evaluates the engine dome surface in GODOT-world
		// space, so it dots GODOT-world sun/light vectors: route the
		// render-float tuples through the env_axes.h swap (2026-08-20 — the
		// identity mapping put the sun-proximity highlight 90 degrees off in
		// yaw, the 03tr-sun-sky dome half).
		material->set_shader_parameter("u_sun_dir",
				render_float_to_godot(frame.sun_dir));
		material->set_shader_parameter("u_light_dir",
				render_float_to_godot(frame.light_dir));
		material->set_shader_parameter("u_fog_color",
				to_color(frame.skyfog_color));
		// The sky wrapper keeps its dedicated skyfog color and its own fog
		// end on both sides of the water plane: the engine frame's fog_end is
		// the raw smoothed distance the dome VS constant c9.x carries
		// unconditionally (sky_frame.cpp cites render_skybox @ 0x5792c2); the
		// murk-derived world end is the object/terrain passes' alone.
		material->set_shader_parameter("u_fog_end", frame.fog_end);
		material->set_shader_parameter("u_sky_height", frame.sky_height);
	}

	_update_cloud_textures(env, material);

	// Cloud scroll: the weather runtime owns the ramping rate and the four
	// integer accumulators (weather_runtime.h carries the cites); standalone
	// owners tick the engine fallback core at the same 62 Hz cadence.
	double cam_x = 0.0;
	double cam_z = 0.0;
	if (cam != nullptr) {
		const Vector3 cam_pos = cam->get_global_position();
		cam_x = cam_pos.x;
		cam_z = cam_pos.z;
	}
	Weather *weather = _weather_node();
	if (weather != nullptr) {
		material->set_shader_parameter("u_scroll_offset1",
				weather->get_cloud_uv_offset1(cam_x, cam_z));
		material->set_shader_parameter("u_scroll_offset2",
				weather->get_cloud_uv_offset2(cam_x, cam_z));
	} else {
		fallback_scroll_.advance(p_delta, frame.sky_speed);
		const opennova::env::CloudUvOffsets offsets =
				opennova::env::cloud_scroll_uv_offsets(
						fallback_scroll_.core.cloud_scroll,
						static_cast<float>(cam_x), static_cast<float>(cam_z));
		material->set_shader_parameter("u_scroll_offset1",
				Vector2(offsets.u1, offsets.v1));
		material->set_shader_parameter("u_scroll_offset2",
				Vector2(offsets.u2, offsets.v2));
	}
}

void SkyPass::_update_cloud_textures(MissionEnvironment *p_env,
		const Ref<ShaderMaterial> &p_material) {
	Ref<Texture2D> tex1;
	Ref<Texture2D> tex2;
	if (p_env != nullptr && p_env->is_loaded()) {
		tex1 = p_env->get_sky_map1_tex();
		tex2 = p_env->get_sky_map2_tex();
	}
	if (tex1 == bound_cloud_tex1_ && tex2 == bound_cloud_tex2_) {
		return;
	}
	bound_cloud_tex1_ = tex1;
	bound_cloud_tex2_ = tex2;
	const bool has_clouds = tex1.is_valid() || tex2.is_valid();
	if (has_clouds) {
		p_material->set_shader_parameter("u_cloud_tex1",
				tex1.is_valid() ? tex1 : tex2);
		p_material->set_shader_parameter("u_cloud_tex2",
				tex2.is_valid() ? tex2 : tex1);
	}
	p_material->set_shader_parameter("u_has_clouds", has_clouds);
}

} // namespace godot
