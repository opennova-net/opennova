#include "env/precipitation.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/core/object.hpp>

#include <runtime/renderer/scene_overlay.h>

#include "env/weather.h"
#include "render/scene_overlay_compositor.h"
#include "simulation/simulation.h"

namespace godot {

void Precipitation::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_weather_path", "path"),
			&Precipitation::set_weather_path);
	ClassDB::bind_method(D_METHOD("get_weather_path"),
			&Precipitation::get_weather_path);
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "weather_path"),
			"set_weather_path", "get_weather_path");
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
		//  jsnwflk.tga through the stage texture loader; the install ships no
		//  jsnwflk.tga, so snow loads no texture, as in retail)
		rain_texture_ = resource_root_->load_texture(kRainTexture, ResourceRoot::TEXTURE_LOADER_STAGE);
		snow_texture_ = resource_root_->load_texture(kSnowTexture, ResourceRoot::TEXTURE_LOADER_STAGE);
		textures_loaded_ = true;
	}
	return p_snow ? snow_texture_ : rain_texture_;
}

void Precipitation::render_frame(Object *p_sim, Camera3D *p_camera, int p_camera_mode) {
	compile_into_(p_sim, p_camera, p_camera_mode, frame_);
}

void Precipitation::render_inset_frame(Object *p_sim, Camera3D *p_camera, int p_camera_mode) {
	compile_into_(p_sim, p_camera, p_camera_mode, inset_frame_);
}

void Precipitation::release_inset_frame() {
	inset_frame_.clear();
}

void Precipitation::compile_into_(Object *p_sim, Camera3D *p_camera, int p_camera_mode,
		opennova::renderer::PrecipitationDrawFrame &r_frame) {
	Simulation *sim = Object::cast_to<Simulation>(p_sim);
	Weather *weather = _weather_node();
	if (sim == nullptr || p_camera == nullptr || weather == nullptr) {
		r_frame.clear();
		return;
	}
	const Transform3D xform = p_camera->get_global_transform();
	const Basis basis = xform.basis;
	r_frame = sim->compile_precipitation_frame(xform.origin, basis.get_column(0).normalized(),
			basis.get_column(1).normalized(), weather->get_terrain_light_combined_rgb(),
			p_camera_mode);
}

void Precipitation::hide_frame() {
	frame_.clear();
	inset_frame_.clear();
}

void Precipitation::append_overlay(SceneOverlaySubmission &r_submission) {
	const struct {
		const opennova::renderer::PrecipitationDrawFrame &frame;
		opennova::renderer::SceneOverlaySlot slot;
	} views[] = {
		{ frame_, opennova::renderer::SceneOverlaySlot::Precipitation },
		{ inset_frame_, opennova::renderer::SceneOverlaySlot::InsetPrecipitation },
	};
	for (const auto &view : views) {
		if (view.frame.drops <= 0) {
			continue;
		}
		const uint32_t texture = r_submission.texture_index(_texture_for(view.frame.snow));
		opennova::renderer::append_precipitation_overlay(view.frame, texture,
				r_submission.frame, view.slot);
	}
}

} // namespace godot
