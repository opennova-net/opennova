#include "env/precipitation.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>

#include "env/weather.h"
#include "simulation/simulation.h"

namespace godot {

void Precipitation::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_resource_root", "root"),
			&Precipitation::set_resource_root);
	ClassDB::bind_method(D_METHOD("set_weather_path", "path"),
			&Precipitation::set_weather_path);
	ClassDB::bind_method(D_METHOD("get_weather_path"),
			&Precipitation::get_weather_path);
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "weather_path"),
			"set_weather_path", "get_weather_path");
	ClassDB::bind_method(D_METHOD("render_frame", "sim", "camera"),
			&Precipitation::render_frame);
	ClassDB::bind_method(D_METHOD("get_last_drop_count"),
			&Precipitation::get_last_drop_count);
	ClassDB::bind_method(D_METHOD("is_last_frame_snow"),
			&Precipitation::is_last_frame_snow);
}

void Precipitation::set_resource_root(const Ref<ResourceRoot> &p_root) {
	resource_root_ = p_root;
	rain_texture_.unref();
	snow_texture_.unref();
	textures_loaded_ = false;
}

void Precipitation::set_weather_path(const NodePath &p_path) {
	weather_path_ = p_path;
}

Weather *Precipitation::_weather_node() const {
	if (weather_path_.is_empty()) {
		return nullptr;
	}
	if (!is_inside_tree() && weather_path_.is_absolute()) {
		return nullptr;
	}
	return Object::cast_to<Weather>(get_node_or_null(weather_path_));
}

void Precipitation::_ready() {
	_ensure_scene();
}

void Precipitation::_ensure_scene() {
	if (mesh_instance_ != nullptr) {
		return;
	}
	Ref<Shader> shader =
			ResourceLoader::get_singleton()->load("res://shaders/precipitation.gdshader");
	material_.instantiate();
	material_->set_shader(shader);
	mesh_.instantiate();
	mesh_instance_ = memnew(MeshInstance3D);
	mesh_instance_->set_name("PrecipitationStreaks");
	mesh_instance_->set_mesh(mesh_);
	mesh_instance_->set_material_override(material_);
	// The drops draw in world space under an identity matrix, unlit, never
	// shadowed (retail the identity world matrix @ 0x5def50; pass flags
	// 0x10500000 lighting off).
	mesh_instance_->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
	mesh_instance_->set_as_top_level(true);
	mesh_instance_->set_ignore_occlusion_culling(true);
	add_child(mesh_instance_);
}

Ref<Texture2D> Precipitation::_texture_for(bool p_snow) {
	if (!textures_loaded_ && resource_root_.is_valid()) {
		// (retail WeatherParticle_LoadTextures @ 0x5de840 — eraindrp.tga and
		//  jsnwflk.tga through the archive texture loader)
		rain_texture_ = resource_root_->load_texture(kRainTexture);
		snow_texture_ = resource_root_->load_texture(kSnowTexture);
		textures_loaded_ = true;
	}
	return p_snow ? snow_texture_ : rain_texture_;
}

void Precipitation::render_frame(Object *p_sim, Camera3D *p_camera) {
	_ensure_scene();
	Simulation *sim = Object::cast_to<Simulation>(p_sim);
	Weather *weather = _weather_node();
	if (sim == nullptr || p_camera == nullptr || weather == nullptr) {
		mesh_->clear_surfaces();
		mesh_instance_->set_visible(false);
		last_drops_ = 0;
		return;
	}
	const Transform3D xform = p_camera->get_global_transform();
	const Basis basis = xform.basis;
	const Dictionary frame = sim->compile_precipitation_frame(xform.origin,
			basis.get_column(0).normalized(), basis.get_column(1).normalized(),
			weather->get_terrain_light_combined_rgb());
	const int drops = static_cast<int>(frame.get("drops", 0));
	mesh_->clear_surfaces();
	last_drops_ = drops;
	last_snow_ = static_cast<bool>(frame.get("snow", false));
	if (drops <= 0) {
		mesh_instance_->set_visible(false);
		return;
	}
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = frame.get("positions", PackedVector3Array());
	arrays[Mesh::ARRAY_TEX_UV] = frame.get("uvs", PackedVector2Array());
	mesh_->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	const int64_t argb = static_cast<int64_t>(frame.get("color", 0xFF000000));
	// Env_TerrainLightCombined | 0xFF000000: the diffuse the fixed-function
	// combine modulates (x2) the texture with.
	const Color diffuse(
			static_cast<float>((argb >> 16) & 0xFF) / 255.0f,
			static_cast<float>((argb >> 8) & 0xFF) / 255.0f,
			static_cast<float>(argb & 0xFF) / 255.0f,
			static_cast<float>((argb >> 24) & 0xFF) / 255.0f);
	material_->set_shader_parameter("diffuse", diffuse);
	Ref<Texture2D> texture = _texture_for(last_snow_);
	if (texture.is_valid()) {
		material_->set_shader_parameter("drop_texture", texture);
	}
	mesh_instance_->set_visible(true);
}

} // namespace godot
