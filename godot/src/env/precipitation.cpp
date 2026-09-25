#include "env/precipitation.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/core/object.hpp>

#include <runtime/renderer/scene_overlay.h>

#include "env/weather.h"
#include "render/scene_overlay_compositor.h"
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
	ClassDB::bind_method(D_METHOD("get_drop_count"), &Precipitation::get_drop_count);
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
	Simulation *sim = Object::cast_to<Simulation>(p_sim);
	Weather *weather = _weather_node();
	if (sim == nullptr || p_camera == nullptr || weather == nullptr) {
		hide_frame();
		return;
	}
	const Transform3D xform = p_camera->get_global_transform();
	const Basis basis = xform.basis;
	frame_ = sim->compile_precipitation_frame(xform.origin, basis.get_column(0).normalized(),
			basis.get_column(1).normalized(), weather->get_terrain_light_combined_rgb());
}

void Precipitation::hide_frame() {
	frame_.clear();
}

void Precipitation::append_overlay(SceneOverlaySubmission &r_submission) {
	if (frame_.drops <= 0) {
		return;
	}
	const uint32_t texture = r_submission.texture_index(_texture_for(frame_.snow));
	opennova::renderer::append_precipitation_overlay(frame_, texture, r_submission.frame);
}

} // namespace godot
