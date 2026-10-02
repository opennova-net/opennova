#pragma once

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/world_environment.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <editor/preview/mission_scene.h>

namespace opennova::editor {
class ProjectAssetSource;
}

#include "authoring/viewport_applier.h"
#include "env/env_file.h"
#include "env/mission_environment.h"
#include "env/sky_dome.h"
#include "env/water.h"
#include "resource_index/resource_root.h"
#include "terrain/terrain.h"
#include "terrain/terrain_data.h"

namespace godot {

// A mission viewport's device work (ADR 0046 S14): the game's own nodes under the device's
// SubViewport, drawn as the game draws a mission without a simulation: the environment
// (MissionEnvironment over the mission's .env with the header's overrides, its clock at the start
// time), the sky, the water, the terrain (Terrain over TerrainData, read through a ResourceRoot
// mounted over the project's files), and the camera where the viewport's orbit camera is. The
// frame's legs run in the game's order (godot/src/world/game_world_frame.cpp's frame-leg table, no
// simulation): the camera, the render eye, the clear colour, the sky, the terrain, the water. The
// entities the placer places are the next commit's; the mission's marks are the canvas's, drawn over
// the picture by the viewport's portable half on the pixels this camera projects them to.
//
// It builds over the Shell's frames (S13 V6), a unit a step: the environment file; the terrain's
// files a unit each and its build a tile a step (TerrainData::begin_load_from_resource_root,
// Terrain::build_begin); the sky and the water; the pose. A Rebuild plans only the units of what
// differs from what it holds (the terrain's key, the environment's), so a Rebuild that moves neither
// has one unit, the pose, and is made whole as it is taken. Its scene state is its own (E13): the
// environment's shader globals and the water plane, published again whenever another device's state
// was published last; its frame's legs run only in a frame it renders (present), and the water's
// mirror pass is on only in such a frame (off from each frame's tick until its present).
class MissionViewportApplier final : public ViewportApplier {
public:
	explicit MissionViewportApplier(SubViewport &viewport);

	void rebuild(const opennova::editor::ViewportModel &model, const opennova::editor::SessionView &view,
			const opennova::editor::PreviewClock &clock) override;
	ApplierStep step(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock,
			std::string &failure) override;
	bool building() const override { return build_ != nullptr; }
	opennova::editor::OperationProgress progress() const override;
	void update(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock) override;
	void clear() override;
	void apply(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock,
			opennova::editor::ViewportDeviceReport &report) override;
	void tick(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock) override;
	void resize(int, int) override {}
	bool surface_at(float x, float y, float point[3]) const override;
	bool surface_between(const double from[3], const double to[3], double point[3]) const override;
	bool ground_at(double x, double y, double &height) const override;
	uint64_t scene_state() const override { return scene_state_; }
	void publish_scene_state() override;
	void present(double dt) override;

	// Its nodes, for the parity tests.
	Camera3D *camera() const { return camera_; }
	Terrain *terrain() const { return terrain_; }
	MissionEnvironment *environment() const { return environment_; }

private:
	// One unit of a build, in the order they run.
	struct Unit {
		enum class Kind : uint8_t {
			Environment, // the .env file, the header's overrides, the clock
			TerrainFile, // one file of the terrain's load (TerrainData::load_step)
			TerrainBuild, // one step of the terrain's build (Terrain::build_step)
			Sky, // the sky dome bound to the environment
			Water, // the water bound to the terrain and the environment
			Pose, // the options, the time, the camera
		};
		Kind kind = Kind::Pose;
	};
	// The keys the layers were built from.
	struct TerrainKey {
		std::string terrain, tile_set;
		bool operator==(const TerrainKey &o) const { return terrain == o.terrain && tile_set == o.tile_set; }
	};
	struct EnvironmentKey {
		std::string environment;
		int start_time = 0, minutes_per_day = 0;
		uint32_t attrib_flags = 0;
		int water_override = 0, fog_override = 0, water_murk = 0;
		int fog_color[3] = { 0, 0, 0 }, water_color[3] = { 0, 0, 0 };
		bool operator==(const EnvironmentKey &o) const;
	};
	static EnvironmentKey environment_key_of_(const opennova::editor::MissionSceneHeader &header);
	// A build in flight: what it brings the layers to, and its units.
	struct Build {
		bool environment = false, terrain = false;
		std::vector<Unit> units;
		size_t next = 0;
	};

	// The project's files mounted as the device's resource root (again when the project's source
	// is another); the caches cleared when the files moved.
	void mount_(const opennova::editor::SessionView &view);
	void plan_(Build &build, const opennova::editor::MissionScene &scene);
	void run_environment_(const opennova::editor::MissionScene &scene);
	// The state over the picture that stands: the layers' visibility, the time of day, the camera.
	void apply_state_(const opennova::editor::ViewportModel &model);
	void place_camera_(const opennova::editor::ViewportModel &model);
	void note_missing_(const String &name);

	Node3D *root_ = nullptr;
	WorldEnvironment *clear_ = nullptr;
	MissionEnvironment *environment_ = nullptr;
	SkyDome *sky_ = nullptr;
	Water *water_ = nullptr;
	Terrain *terrain_ = nullptr;
	Camera3D *camera_ = nullptr;
	Ref<ResourceRoot> root_files_;
	std::shared_ptr<const opennova::editor::ProjectAssetSource> mounted_; // the source the root is mounted over
	uint64_t mounted_generation_ = 0;
	Ref<TerrainData> terrain_data_;
	Ref<TerrainData> loading_; // the terrain data a build loads
	Ref<EnvFile> env_file_;
	// What the layers hold.
	bool environment_built_ = false, terrain_built_ = false;
	TerrainKey terrain_key_;
	EnvironmentKey environment_key_;
	// What the picture asked the project for and did not find, each once.
	std::vector<std::string> missing_;
	std::unique_ptr<Build> build_;
	uint64_t scene_state_ = 0;
	// The time applied (-2: none yet; -1 the mission's own; else the option's hour).
	double applied_time_ = -2.0;
	bool shown_water_ = true;
};

} // namespace godot
