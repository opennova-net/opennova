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
// Render_WeatherTrailParticles @ 0x5dee10 refills one vertex stream per
// call under a fixed layout, 768-vertex batches, from
// Terrain_RenderWorldScene @ 0x5c96a6 — its only caller, the scene passes
// (the main scene's and the weapon Inset pass's), so the water mirror never
// draws it; WeatherParticle_LoadTextures @ 0x5de840 from
// Render_InitMissionTextures @ 0x587120). Drives nothing itself: GameWorld's
// render ladder calls render_frame once per display frame after the particle
// leg, then render_inset_frame while the Inset renders, and the overlay leg
// appends both frames.
class Precipitation : public Node3D {
	GDCLASS(Precipitation, Node3D)

public:
	static constexpr const char *kRainTexture = opennova::renderer::kRainTexture;
	static constexpr const char *kSnowTexture = opennova::renderer::kSnowTexture;

	void set_resource_root(const Ref<ResourceRoot> &p_root);
	void set_weather_path(const NodePath &p_path);
	NodePath get_weather_path() const { return weather_path_; }

	// One display frame: update + compile the drops for `camera` (its
	// global transform supplies position / right / up) under the frame's
	// camera mode (the drawer's mode test, renderer/precipitation_frame.h).
	void render_frame(Object *p_sim, Camera3D *p_camera, int p_camera_mode);
	// The weapon Inset pass's own call, after render_frame in the same frame:
	// the same update + compile at the Inset camera, over the one drop pool
	// and draw state (renderer/precipitation_frame.h PrecipitationDrawState;
	// renderer/scene_overlay.h kInsetOverlayOrder carries the witness).
	// release_inset_frame drops it while that pass does not render.
	void render_inset_frame(Object *p_sim, Camera3D *p_camera, int p_camera_mode);
	void release_inset_frame();
	// Below the rain gate the drawer never touches the device (retail
	// returns @ 0x5dee48): neither view keeps streaks.
	void hide_frame();
	// This frame's streaks into the post-particle overlay tail. Not bound to
	// Godot.
	void append_overlay(SceneOverlaySubmission &r_submission);

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
	// The weapon Inset pass's call of the same frame.
	opennova::renderer::PrecipitationDrawFrame inset_frame_;
	void compile_into_(Object *p_sim, Camera3D *p_camera, int p_camera_mode,
			opennova::renderer::PrecipitationDrawFrame &r_frame);
};

} // namespace godot
