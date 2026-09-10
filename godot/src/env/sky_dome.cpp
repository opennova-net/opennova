#include "env/sky_dome.h"

#include "util/axes.h"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/shader.hpp>

#include "env/env_file.h"
#include "env/env_render_camera.h"
#include "env/mission_environment.h"
#include "env/weather.h"

namespace godot {

namespace {

Vector3 to_vector3(const opennova::env::Rgb &rgb) {
	return Vector3(rgb.r, rgb.g, rgb.b);
}

Vector3 to_vector3(const opennova::env::Vec3 &v) {
	return Vector3(v.x, v.y, v.z);
}

} // namespace

void SkyDome::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_environment_path", "path"),
			&SkyDome::set_environment_path);
	ClassDB::bind_method(D_METHOD("get_environment_path"),
			&SkyDome::get_environment_path);
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "environment_path"),
			"set_environment_path", "get_environment_path");
	ClassDB::bind_method(D_METHOD("set_weather_path", "path"),
			&SkyDome::set_weather_path);
	ClassDB::bind_method(D_METHOD("get_weather_path"),
			&SkyDome::get_weather_path);
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "weather_path"),
			"set_weather_path", "get_weather_path");
	ClassDB::bind_method(D_METHOD("set_environment_capture_layer_mask", "mask"),
			&SkyDome::set_environment_capture_layer_mask);
	ClassDB::bind_method(D_METHOD("get_environment_capture_layer_mask"),
			&SkyDome::get_environment_capture_layer_mask);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "environment_capture_layer_mask",
			PROPERTY_HINT_LAYERS_3D_RENDER),
			"set_environment_capture_layer_mask",
			"get_environment_capture_layer_mask");
	ClassDB::bind_method(D_METHOD("set_frame_clear_environment", "environment"),
			&SkyDome::set_frame_clear_environment);
	ClassDB::bind_method(D_METHOD("get_frame_clear_environment"),
			&SkyDome::get_frame_clear_environment);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "frame_clear_environment",
						 PROPERTY_HINT_RESOURCE_TYPE, "Environment"),
			"set_frame_clear_environment", "get_frame_clear_environment");
	ClassDB::bind_method(D_METHOD("build"), &SkyDome::build);
	ClassDB::bind_method(D_METHOD("is_built"), &SkyDome::is_built);
	ClassDB::bind_method(D_METHOD("get_sky_material"),
			&SkyDome::get_sky_material);
	ClassDB::bind_method(D_METHOD("get_mesh_instance"),
			&SkyDome::get_mesh_instance);
	// The externally-callable render-frame drive: the
	// test harness drives frames here; the engine's virtual delegates in.
	ClassDB::bind_method(D_METHOD("advance_frame", "delta"),
			&SkyDome::advance_frame);
}

void SkyDome::set_environment_path(const NodePath &p_path) {
	environment_path_ = p_path;
	env_node_id_ = ObjectID();
}

void SkyDome::set_weather_path(const NodePath &p_path) {
	weather_path_ = p_path;
	weather_node_id_ = ObjectID();
}

void SkyDome::set_environment_capture_layer_mask(uint32_t p_mask) {
	environment_capture_layer_mask_ = p_mask;
	if (mesh_instance_ != nullptr) {
		mesh_instance_->set_layer_mask(
				mesh_instance_->get_layer_mask() | p_mask);
	}
}

void SkyDome::set_frame_clear_environment(const Ref<Environment> &p_environment) {
	frame_clear_environment_ = p_environment;
}

MissionEnvironment *SkyDome::_env_node() {
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

Weather *SkyDome::_weather_node() {
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

void SkyDome::_ready() {
	build();
}

void SkyDome::clear() {
	if (mesh_instance_ != nullptr) {
		remove_child(mesh_instance_);
		mesh_instance_->queue_free();
		mesh_instance_ = nullptr;
	}
	built_ = false;
	bound_cloud_tex1_.unref();
	bound_cloud_tex2_.unref();
	sky_material_.unref();
	cached_cam_id_ = ObjectID();
}

void SkyDome::build() {
	clear();

	Ref<Shader> sky_shader =
			ResourceLoader::get_singleton()->load("res://shaders/sky.gdshader");
	sky_material_.instantiate();
	sky_material_->set_shader(sky_shader);

	// The witnessed 21x21 dome (441 verts / 800 tris), built ONCE at the
	// reference height (env #20's ratified fold; env_celestial.h carries the
	// mesh-builder cites).
	const Array arrays =
			EnvFile::build_sky_dome_arrays(EnvFile::dome_reference_height());
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	mesh->surface_set_material(0, sky_material_);

	mesh_instance_ = memnew(MeshInstance3D);
	// Retail submits the sky in each render pass; the shader reanchors to
	// that pass camera. Keep CPU culling from rejecting reflection-pass
	// relocation before the vertex stage (sky_frame.h carries the cites).
	mesh_instance_->set_extra_cull_margin(1.0e6);
	mesh_instance_->set_ignore_occlusion_culling(true);
	mesh_instance_->set_cast_shadows_setting(
			GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
	mesh_instance_->set_gi_mode(GeometryInstance3D::GI_MODE_DISABLED);
	mesh_instance_->set_layer_mask(mesh_instance_->get_layer_mask() |
			environment_capture_layer_mask_);
	mesh_instance_->set_mesh(mesh);
	add_child(mesh_instance_);
	built_ = true;
}

void SkyDome::advance_frame(double p_delta) {
	if (!built_ || sky_material_.is_null()) {
		return;
	}
	// Camera3D.current can change while the old camera remains alive. Re-resolve
	// that transition instead of
	// continuing to follow a stale, still-in-tree camera.
	Camera3D *cam = Object::cast_to<Camera3D>(
			ObjectDB::get_instance(cached_cam_id_));
	if (cam == nullptr || !cam->is_inside_tree() || !cam->is_current()) {
		cam = find_env_render_camera(this);
		cached_cam_id_ = cam != nullptr ? ObjectID(cam->get_instance_id())
										: ObjectID();
	}
	if (cam != nullptr && mesh_instance_ != nullptr) {
		const Vector3 cam_pos = cam->get_global_position();
		const opennova::env::Vec3 anchor = opennova::env::sky_dome_anchor(
				opennova::env::Vec3{static_cast<float>(cam_pos.x),
						static_cast<float>(cam_pos.y),
						static_cast<float>(cam_pos.z)});
		mesh_instance_->set_global_position(to_vector3(anchor));
	}

	sync_frame_clear_color();
	MissionEnvironment *env = _env_node();
	const opennova::env::SkyFrameState frame = env != nullptr
			? opennova::env::build_sky_frame(env->state())
			: opennova::env::SkyFrameState{};
	if (frame.loaded) {
		if (frame.flat_pass) {
			sky_material_->set_shader_parameter("u_flat_pass", true);
			sky_material_->set_shader_parameter("u_flat_color",
					to_vector3(frame.flat_color));
		} else {
			sky_material_->set_shader_parameter("u_flat_pass", false);
			sky_material_->set_shader_parameter("u_sky_base",
					to_vector3(frame.sky_base));
			sky_material_->set_shader_parameter("u_sky_bright",
					to_vector3(frame.sky_bright));
			sky_material_->set_shader_parameter("u_sky_highlight",
					to_vector3(frame.sky_highlight));
			sky_material_->set_shader_parameter("u_cloud_base",
					to_vector3(frame.cloud_base));
			sky_material_->set_shader_parameter("u_cloud_highlight",
					to_vector3(frame.cloud_highlight));
			sky_material_->set_shader_parameter("u_cloud_edge",
					to_vector3(frame.cloud_edge));
		}
		// The dome mesh is the engine layout drawn identity into the Godot
		// world, so its shader dots GODOT-world sun/light vectors: route the
		// render-float tuples through the util/axes.h swap (2026-08-20 — the
		// identity mapping put the sun-proximity highlight 90 degrees off in
		// yaw, the 03tr-sun-sky dome half).
		sky_material_->set_shader_parameter("u_sun_dir",
				render_float_to_godot(frame.sun_dir));
		sky_material_->set_shader_parameter("u_light_dir",
				render_float_to_godot(frame.light_dir));
		sky_material_->set_shader_parameter("u_fog_color",
				to_vector3(frame.skyfog_color));
		// The sky wrapper keeps its dedicated skyfog color and its own fog end
		// on both sides of the water plane: the engine frame's fog_end is the
		// raw smoothed distance the dome VS constant c9.x carries unconditionally
		// (sky_frame.cpp cites render_skybox @ 0x5792c2); the murk-derived
		// world end is the object/terrain passes' alone.
		sky_material_->set_shader_parameter("u_fog_end", frame.fog_end);
		sky_material_->set_shader_parameter("u_sky_height", frame.sky_height);
	}

	_update_cloud_textures(env);

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
		sky_material_->set_shader_parameter("u_scroll_offset1",
				weather->get_cloud_uv_offset1(cam_x, cam_z));
		sky_material_->set_shader_parameter("u_scroll_offset2",
				weather->get_cloud_uv_offset2(cam_x, cam_z));
	} else {
		fallback_scroll_.advance(p_delta, frame.sky_speed);
		const opennova::env::CloudUvOffsets offsets =
				opennova::env::cloud_scroll_uv_offsets(
						fallback_scroll_.core.cloud_scroll,
						static_cast<float>(cam_x), static_cast<float>(cam_z));
		sky_material_->set_shader_parameter("u_scroll_offset1",
				Vector2(offsets.u1, offsets.v1));
		sky_material_->set_shader_parameter("u_scroll_offset2",
				Vector2(offsets.u2, offsets.v2));
	}
}

void SkyDome::_update_cloud_textures(MissionEnvironment *p_env) {
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
		sky_material_->set_shader_parameter("u_cloud_tex1",
				tex1.is_valid() ? tex1 : tex2);
		sky_material_->set_shader_parameter("u_cloud_tex2",
				tex2.is_valid() ? tex2 : tex1);
	}
	sky_material_->set_shader_parameter("u_has_clouds", has_clouds);
}

// The faithful sky dome is open below its rim. Retail clears that region to
// the horizon-blended skyfog block; the shell provides the BG_COLOR resource.
void SkyDome::sync_frame_clear_color() {
	if (frame_clear_environment_.is_null()) {
		return;
	}
	MissionEnvironment *env = _env_node();
	if (env == nullptr || !env->is_loaded()) {
		return;
	}
	const opennova::env::Rgb rgb = env->state().frame_clear_color();
	frame_clear_environment_->set_bg_color(Color(rgb.r, rgb.g, rgb.b));
}

} // namespace godot
