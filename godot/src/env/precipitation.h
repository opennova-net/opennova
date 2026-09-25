#pragma once

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/node_path.hpp>

#include "resource_index/resource_root.h"

#include <runtime/environment/precipitation.h>
#include <runtime/renderer/precipitation_frame.h>

namespace godot {

class Simulation;
class Weather;
struct SceneOverlaySubmission;

// The precipitation presenter — the device leg of the retail rain/snow
// drawer: this frame's compiled drop streaks (the engine's
// renderer::compile_precipitation_frame through
// Simulation.compile_precipitation_frame, which also runs the kernel's
// per-render pool update) and the eraindrp / jsnwflk textures from the
// resource root, handed to the post-particle overlay stage
// (renderer/scene_overlay.h), which draws them under the drawer's pass state
// after particle pass B and before the coronas (retail
// render_weather_trail_particles @ 0x5dee10 refills one vertex stream per
// frame under a fixed layout, 768-vertex batches, from
// Terrain_RenderWorldScene @ 0x5c96a6 — its only caller, so the
// water mirror never draws it; WeatherParticle_LoadTextures @ 0x5de840 from
// Render_InitMissionTextures @ 0x587120). Drives nothing itself: GameWorld's
// render ladder calls render_frame once per display frame after the particle
// leg, and the overlay leg appends the frame.
class Precipitation : public Node3D {
	GDCLASS(Precipitation, Node3D)

public:
	static constexpr const char *kRainTexture = "eraindrp.tga";
	static constexpr const char *kSnowTexture = "jsnwflk.tga";

	void set_resource_root(const Ref<ResourceRoot> &p_root);
	void set_weather_path(const NodePath &p_path);
	NodePath get_weather_path() const { return weather_path_; }

	// One display frame: update + compile the drops for `camera` (its
	// global transform supplies position / right / up).
	void render_frame(Object *p_sim, Camera3D *p_camera);
	// Below the rain gate the drawer never touches the device (retail
	// returns @ 0x5dee48): the frame keeps no streaks.
	void hide_frame();
	// This frame's streaks into the post-particle overlay tail. Not bound to
	// Godot.
	void append_overlay(SceneOverlaySubmission &r_submission);
	// The streak count of the last compiled frame (0 below the rain gate).
	int get_drop_count() const { return frame_.drops; }

protected:
	static void _bind_methods();

private:
	Weather *_weather_node() const;
	Ref<Texture2D> _texture_for(bool p_snow);

	NodePath weather_path_;
	Ref<ResourceRoot> resource_root_;
	Ref<Texture2D> rain_texture_;
	Ref<Texture2D> snow_texture_;
	bool textures_loaded_ = false;
	// The last compiled frame (its vertex storage is reused frame to frame).
	opennova::renderer::PrecipitationDrawFrame frame_;
};

} // namespace godot
