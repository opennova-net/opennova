#pragma once

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/world_environment.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/preview/viewport_follow.h>

#include "authoring/viewport_applier.h"
#include "env/env_file.h"
#include "env/mission_environment.h"
#include "env/sky_dome.h"
#include "env/water.h"
#include "env/weather.h"
#include "resource_index/resource_root.h"
#include "terrain/foliage_dispatcher.h"
#include "terrain/terrain.h"
#include "terrain/terrain_data.h"

namespace opennova::editor {
class TerrainViewport;
class ProjectAssetSource;
} // namespace opennova::editor

namespace godot {

// A terrain viewport's device work (ADR 0046 DI-30b): the game's own nodes under the device's SubViewport, drawn as
// the game draws a mission's ground with no mission and no simulation: the terrain (Terrain over TerrainData, its
// .trn as the game would read it were it saved now, its tiles the mission's <mission>.til or, with none, the
// terrain's own polytrn_tileinfo, which TerrainData falls back on as the game does), its foliage (the game's
// FoliageDispatcher configured from the terrain's definitions once its build ends, as GameWorld::configure_foliage
// configures it, drawn from the terrain's detail cells around the camera), the water bound to it, and the sky and
// the light of the environment of the mission the viewport draws it under (MissionEnvironment over its .env with
// the header's overrides, the clock at its start time; with none, MissionEnvironment with no file: the engine's
// own, as a mission with no .env starts), all read through a ResourceRoot mounted over the project's files, and the
// camera where the viewport's camera is. The viewport's ground overlay (DI-29) is the terrain's tint
// (Terrain::set_ground_overlay), an Update.
//
// It builds over the Shell's frames in two layers, each rebuilt only where its key or a file it read moved: the
// environment (the .env, the overcast table, the overrides, the clock; the sky bound to it) and the terrain (its
// files a unit each, its .til, then its build a tile a step; the water and the foliage bound to it). An edit of the
// terrain's document moves its file, so the terrain layer is built again. Its scene state is its own (E13): the
// environment and the water hold their process-wide globals but while it publishes them and while it presents its
// frame. The foliage's sway reads the weather's oscillator, whose wave alone the device runs on the preview clock
// (the mission's start settle first), as the mission device does; no other weather runs.
class TerrainViewportApplier final : public ViewportApplier {
public:
	explicit TerrainViewportApplier(SubViewport &viewport);
	~TerrainViewportApplier() override;

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
	bool surface_between(const double from[3], const double to[3], double point[3]) const override;
	bool ground_at(double x, double y, double &height) const override;
	uint64_t scene_state() const override { return scene_state_; }
	void publish_scene_state() override;
	void present(double dt) override;

	// Its nodes, for the device tests.
	Camera3D *camera() const { return camera_; }
	MissionEnvironment *environment() const { return environment_; }
	Terrain *terrain() const { return terrain_; }
	Water *water() const { return water_; }
	FoliageDispatcher *foliage() const { return foliage_; }
	// How many builds of each layer it ran, and how many frames it presented.
	int environment_builds() const { return environment_builds_; }
	int terrain_builds() const { return terrain_builds_; }
	int64_t presented() const { return presented_; }

private:
	struct Unit {
		enum class Kind : uint8_t {
			Environment, // the .env, the overcast table, the overrides, the clock
			TerrainFile, // one file of the terrain's load; the .til with the last
			TerrainBuild, // one step of the terrain's build
			NoTerrain, // the terrain did not load: the layer empty
			Sky, // the sky dome bound to the environment
			Water, // the water bound to the terrain and the environment
			Pose, // the options, the overlay, the camera
		};
		Kind kind = Kind::Pose;
	};
	enum Layer : uint8_t { kEnvironment, kTerrain, kLayers };
	static int layer_of_(Unit::Kind kind);
	struct TerrainKey {
		std::string terrain, tile_set, mission;
		bool operator==(const TerrainKey &o) const {
			return terrain == o.terrain && tile_set == o.tile_set && mission == o.mission;
		}
	};
	struct EnvironmentKey {
		std::string environment;
		uint32_t attrib_flags = 0;
		int water_override = 0, fog_override = 0, water_murk = 0;
		int fog_color[3] = { 0, 0, 0 }, water_color[3] = { 0, 0, 0 };
		int start_time = 0, minutes_per_day = 0;
		bool operator==(const EnvironmentKey &o) const;
	};
	static TerrainKey terrain_key_of_(const opennova::editor::TerrainViewport &model);
	static EnvironmentKey environment_key_of_(const opennova::editor::TerrainViewport &model);
	struct Build {
		bool environment = false, terrain = false;
		std::vector<Unit> units;
		size_t next = 0;
		TerrainKey terrain_key;
	};

	void mount_(const opennova::editor::SessionView &view);
	void note_reads_(int layer, size_t from);
	size_t reads_() const;
	void note_missing_(int layer, const String &name);
	void plan_(Build &build, const opennova::editor::TerrainViewport &model);
	void run_(Build &build, const Unit &unit, const opennova::editor::TerrainViewport &model);
	void run_environment_(const opennova::editor::TerrainViewport &model);
	void run_terrain_file_(Build &build);
	void run_terrain_build_(Build &build);
	void terrain_empty_(const TerrainKey &key);
	void configure_foliage_();
	void touch_scene_state_();
	void apply_state_(const opennova::editor::TerrainViewport &model);
	void place_camera_(const opennova::editor::TerrainViewport &model);
	opennova::io::JsonValue drawn_json_() const;

	Node3D *root_ = nullptr;
	WorldEnvironment *clear_ = nullptr;
	MissionEnvironment *environment_ = nullptr;
	Weather *weather_ = nullptr;
	SkyDome *sky_ = nullptr;
	Water *water_ = nullptr;
	Terrain *terrain_ = nullptr;
	FoliageDispatcher *foliage_ = nullptr;
	Camera3D *camera_ = nullptr;
	Ref<ResourceRoot> root_files_;
	std::shared_ptr<const opennova::editor::ProjectAssetSource> mounted_;
	std::shared_ptr<opennova::editor::StampedFiles> stamped_;
	opennova::editor::FileStamps layer_files_[kLayers];
	std::vector<std::string> layer_missing_[kLayers];
	Ref<TerrainData> terrain_data_;
	Ref<TerrainData> loading_;
	Ref<TerrainData> foliage_data_; // the terrain its foliage is configured over
	bool environment_built_ = false, terrain_built_ = false;
	TerrainKey terrain_key_;
	EnvironmentKey environment_key_;
	std::unique_ptr<Build> build_;
	uint64_t scene_state_ = 0;
	uint64_t overlay_serial_ = UINT64_MAX; // the viewport's overlay as last given the terrain
	bool shown_water_ = true, shown_foliage_ = true;
	int32_t wave_tick_ = -1; // the preview clock's tick the oscillator stands at (-1: not started)
	int64_t clock_ms_ = 0;
	int environment_builds_ = 0, terrain_builds_ = 0;
	int64_t presented_ = 0;
};

} // namespace godot
