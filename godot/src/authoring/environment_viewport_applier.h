#pragma once

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/world_environment.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <editor/preview/viewport_follow.h>
#include <formats/env/env_weather.h>

#include "authoring/viewport_applier.h"
#include "env/celestial.h"
#include "env/env_file.h"
#include "env/mission_environment.h"
#include "env/precipitation.h"
#include "env/sky_dome.h"
#include "env/water.h"
#include "env/weather.h"
#include "render/scene_overlay_compositor.h"
#include "resource_index/resource_root.h"
#include "terrain/terrain.h"
#include "terrain/terrain_data.h"

namespace opennova::editor {
class EnvironmentViewport;
class ProjectAssetSource;
} // namespace opennova::editor

namespace godot {

class PreviewEffects;

// An environment viewport's device work (ADR 0046 DI-19b): the game's own environment nodes under the
// device's SubViewport, drawn as the game draws a mission's sky with no simulation: the environment
// (MissionEnvironment over the .env as the game would read it were it saved now, the mission header's
// overrides over it where the viewport says so, the overcast table beside it), its weather (Weather on the
// environment's own home, started as a mission's start starts it: seeded, then the 255-tick settle), the
// sky dome and its clouds, the sun, moon, glare and glint (Celestial), the water, the terrain of the mission
// the viewport draws it over (Terrain over TerrainData, with the mission's tiles), and the rain's drops
// (Precipitation over the home's drop pool), all read through a ResourceRoot mounted over the project's
// files, and the camera where the viewport's camera is.
//
// Its clock and its weather are the viewport's (the portable half steps the game's weather home on the
// preview clock's game ticks, and takes the commands a script gives): every frame, drawn or not, the device
// runs on the engine's state alone the game's ticks that home ran since (the drops' fall, the entity
// update's, then the whole weather tick, colour legs and all), then takes the home's clock, its scalar
// channels (the rain and overcast springs, the fog, the sky height) and its precipitation kind, so what its
// colour legs, sky and drops draw is the viewport's time and weather; a frame it presents publishes them. A
// clock set anew (a scrub, a seek) takes the colours at once (the zero-tick refresh a paused transport's
// scrub takes). Single-sampled, as the game's own view draws: the overlay passes bind the view's depth.
//
// The frame's legs run in the game's order (godot/src/world/game_world_frame.cpp's frame-leg table, no
// simulation): the render eye, the environment nodes (the weather, the sky, the celestial bodies), the
// terrain, the water, the sun veil's exposure feed, the drops, the overlay tail (the drops, the murk, the
// glint and the glare, the mirror's closing draws: env/celestial_overlay, renderer/scene_overlay.h) through
// a post-particle overlay pass of its own placed before the terminal display decode, then the clear colour.
// It builds over the Shell's frames in two layers, each rebuilt only where its key or a file it read moved:
// the environment (the file, the overcast table, the weather's start, the sky, the celestial bodies) and the
// terrain (its files a unit each, then its build a tile a step, the water bound to it). Its scene state is
// its own, as the mission device's (E13): the environment and the water hold their process-wide globals but
// while it publishes them and while it presents its frame, and a new scene state each time its time,
// weather, environment or water moves.
class EnvironmentViewportApplier final : public ViewportApplier {
public:
	explicit EnvironmentViewportApplier(SubViewport &viewport);
	~EnvironmentViewportApplier() override;

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
	bool ground_at(double x, double y, double &height) const override;
	uint64_t scene_state() const override { return scene_state_; }
	void publish_scene_state() override;
	void present(double dt) override;

	// Its nodes, for the device tests.
	Camera3D *camera() const { return camera_; }
	MissionEnvironment *environment() const { return environment_; }
	Weather *weather() const { return weather_; }
	Celestial *celestial() const { return celestial_; }
	Terrain *terrain() const { return terrain_; }
	Water *water() const { return water_; }
	Precipitation *precipitation() const { return precipitation_; }
	// How many game ticks it ran, and how many builds of each layer.
	int64_t ticks_run() const { return ticks_run_; }
	int environment_builds() const { return environment_builds_; }
	int terrain_builds() const { return terrain_builds_; }

private:
	struct Unit {
		enum class Kind : uint8_t {
			Environment, // the .env, the overcast table, the overrides, the clock, the weather's start, the pool
			TerrainFile, // one file of the terrain's load; the mission's .til with the last
			TerrainBuild, // one step of the terrain's build
			NoTerrain, // no terrain: the layer empty
			Sky, // the sky dome and the celestial bodies bound to the environment
			Water, // the water bound to the terrain and the environment
			Pose, // the options, the camera
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
		std::string file, terrain; // the .env, and the drawn mission's .trn the load reads ahead of it
		uint32_t attrib_flags = 0;
		int water_override = 0, fog_override = 0, water_murk = 0;
		int fog_color[3] = { 0, 0, 0 }, water_color[3] = { 0, 0, 0 };
		bool operator==(const EnvironmentKey &o) const;
	};
	static TerrainKey terrain_key_of_(const opennova::editor::EnvironmentViewport &model);
	static EnvironmentKey environment_key_of_(const opennova::editor::EnvironmentViewport &model);
	struct Build {
		bool environment = false, terrain = false;
		std::vector<Unit> units;
		size_t next = 0;
		TerrainKey terrain_key;
	};
	// What the viewport says its weather and clock are (taken at each apply, run at the next present).
	struct Wanted {
		bool known = false;
		uint64_t ticks = 0; // the home's game ticks in all
		uint64_t clock_sets = 0;
		uint32_t tod_fixed24 = 0;
		uint32_t advance = 0;
		uint32_t precipitation_kind = 0;
		int32_t overcast_for_tod_q16 = 0;
		opennova::env::EnvScalarChannels channels;
		bool terrain = true, water = true;
	};

	// `environment`, `terrain`: the texts the environment reads (the .env, the drawn mission's .trn).
	bool mount_(const opennova::editor::SessionView &view, const std::string &environment, const std::string &terrain);
	void note_reads_(int layer, size_t from);
	size_t reads_() const;
	void note_missing_(int layer, const String &name);
	void plan_(Build &build, const opennova::editor::EnvironmentViewport &model);
	void run_(Build &build, const Unit &unit, const opennova::editor::EnvironmentViewport &model);
	void run_environment_(const opennova::editor::EnvironmentViewport &model);
	void run_terrain_file_(Build &build);
	void run_terrain_build_(Build &build);
	void terrain_empty_(const TerrainKey &key);
	void touch_scene_state_();
	void apply_state_(const opennova::editor::EnvironmentViewport &model);
	void place_camera_(const opennova::editor::EnvironmentViewport &model);
	// The viewport's clock and weather into the home, the ticks it ran first.
	void run_weather_();
	// The drops' floor: the terrain under a mission point, 16.16 (the terrain's bilinear height, the floor
	// query env::PrecipitationFloorSampler names); the device's ground has no entity to hit.
	static int32_t floor_height_(void *ctx, int32_t x, int32_t y);
	void overlay_frame_();

	Node3D *root_ = nullptr;
	WorldEnvironment *clear_ = nullptr;
	MissionEnvironment *environment_ = nullptr;
	Weather *weather_ = nullptr;
	SkyDome *sky_ = nullptr;
	Celestial *celestial_ = nullptr;
	Water *water_ = nullptr;
	Terrain *terrain_ = nullptr;
	Precipitation *precipitation_ = nullptr;
	Camera3D *camera_ = nullptr;
	// The game's particle renderer, no effect shown: its overlay passes composed on this device's cameras.
	std::unique_ptr<PreviewEffects> effects_;
	SceneOverlayModelSurfaces overlay_bodies_;
	uint64_t overlay_frame_id_ = 0;
	Ref<ResourceRoot> root_files_;
	std::shared_ptr<const opennova::editor::ProjectAssetSource> mounted_;
	std::shared_ptr<opennova::editor::StampedFiles> stamped_;
	opennova::editor::FileStamps layer_files_[kLayers];
	std::vector<std::string> layer_missing_[kLayers];
	Ref<TerrainData> terrain_data_;
	Ref<TerrainData> loading_;
	bool environment_built_ = false, terrain_built_ = false;
	TerrainKey terrain_key_;
	EnvironmentKey environment_key_;
	std::unique_ptr<Build> build_;
	uint64_t scene_state_ = 0;
	Wanted wanted_;
	uint64_t ticks_seen_ = 0; // the home's ticks this device has run
	uint64_t clock_sets_seen_ = 0;
	bool resync_ = true; // the colours taken at once at the next present
	bool weather_start_ = false; // the weather started as a mission's start starts it, at the next present
	int64_t clock_ms_ = 0; // the preview clock's milliseconds, as the frame's tick read them
	int64_t ticks_run_ = 0;
	int environment_builds_ = 0, terrain_builds_ = 0;
	bool shown_water_ = true;
	bool bodies_moved_ = true; // the celestial bodies to load again at the next Sky unit
};

} // namespace godot
