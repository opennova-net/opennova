#pragma once

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/node_path.hpp>

#include "resource_index/resource_root.h"

namespace godot {

class Simulation;
class Weather;

// The precipitation presenter — the device leg of the retail rain/snow
// drawer: one ArrayMesh of the frame's compiled drop streaks (the engine's
// renderer::compile_precipitation_frame through Simulation.
// compile_precipitation_frame, which also runs the kernel's per-render pool
// update) under the witnessed pass state (shaders/precipitation.gdshader)
// with the eraindrp / jsnwflk textures from the resource root
// (retail render_weather_trail_particles @ 0x5dee10 from
//  Terrain_RenderSceneWithReflection @ 0x5c96a6 — after the camera-side
//  particle pass and projectile trails, before the foliage billboards;
//  WeatherParticle_LoadTextures @ 0x5de840 from Render_InitMissionTextures
//  @ 0x587120). Drives nothing itself: GameWorld's render ladder calls
// render_frame once per display frame after the particle leg.
class Precipitation : public Node3D {
	GDCLASS(Precipitation, Node3D)

public:
	static constexpr const char *kRainTexture = "eraindrp.tga";
	static constexpr const char *kSnowTexture = "jsnwflk.tga";

	void set_resource_root(const Ref<ResourceRoot> &p_root);
	void set_weather_path(const NodePath &p_path);
	NodePath get_weather_path() const { return weather_path_; }

	// One display frame: update + compile the drops for `camera` (its
	// global transform supplies position / right / up) and upload them.
	void render_frame(Object *p_sim, Camera3D *p_camera);
	// The last frame's drop count (probes/tests) and whether the snow
	// texture was bound.
	int get_last_drop_count() const { return last_drops_; }
	bool is_last_frame_snow() const { return last_snow_; }

	void _ready() override;

protected:
	static void _bind_methods();

private:
	Weather *_weather_node() const;
	void _ensure_scene();
	Ref<Texture2D> _texture_for(bool p_snow);

	NodePath weather_path_;
	Ref<ResourceRoot> resource_root_;
	Ref<ArrayMesh> mesh_;
	Ref<ShaderMaterial> material_;
	MeshInstance3D *mesh_instance_ = nullptr;
	Ref<Texture2D> rain_texture_;
	Ref<Texture2D> snow_texture_;
	bool textures_loaded_ = false;
	int last_drops_ = 0;
	bool last_snow_ = false;
};

} // namespace godot
