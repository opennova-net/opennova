#pragma once

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/world_environment.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <editor/preview/mission_scene.h>
#include <editor/preview/viewport_follow.h>

#include "authoring/viewport_applier.h"
#include "env/env_file.h"
#include "env/mission_environment.h"
#include "env/sky_dome.h"
#include "env/water.h"
#include "mission/mission_object_placer.h"
#include "mission/mission_placement_run.h"
#include "object/object_model.h"
#include "resource_index/resource_root.h"
#include "terrain/terrain.h"
#include "terrain/terrain_data.h"

namespace opennova::editor {
class ProjectAssetSource;
}

namespace godot {

// A mission viewport's device work (ADR 0046 S14): the game's own nodes under the device's
// SubViewport, drawn as the game draws a mission without a simulation: the environment
// (MissionEnvironment over the mission's .env with the header's overrides, its clock at the start
// time), the sky, the water, the terrain (Terrain over TerrainData), the entities placed by the
// game's placer (MissionObjectPlacer: the static populations, the individual models), all read
// through a ResourceRoot mounted over the project's files, and the camera where the viewport's
// orbit camera is. The frame's legs run in the game's order (godot/src/world/game_world_frame.cpp's
// frame-leg table, no simulation): the camera, the render eye, the clear colour, the sky, the
// terrain, the water, the static and the individual models' levels. The mission's marks are the
// canvas's, drawn over the picture by the viewport's portable half on the pixels this camera
// projects them to.
//
// It builds over the Shell's frames (S13 V6), a unit a step, in three layers: the environment (its
// file); the terrain (its files a unit each and its build a tile a step, TerrainData::
// begin_load_from_resource_root and Terrain::build_begin; the sky and the water bound to it); the
// entities (the item table; each distinct graphic the entities name, its model parsed and its
// batches warmed; the placement, MissionPlacementRun's units; the added entities lifted, eight a
// unit); then the pose. A Rebuild plans only the units of what differs from what it holds (each
// layer's key, the entities by row), and one whose plan is the pose alone, or the pose after one
// unit of lifts over warm graphics, is made whole as it is taken. An entity's transform is moved in
// place (an Update: move_static_instance, or the individual model's node), its terrain shadow
// source following at the gesture's end; an entity added since the placement is lifted (an
// individual ObjectModel under "Lifted", as the placer builds one for an item); one removed is
// hidden (shown again when it comes back, an undo); one of another item is hidden and lifted anew;
// the lifted set folds back into a whole placement past 256 rows. The root notes every file it is
// asked for (StampedFiles), each layer the names its units read: a file the picture read that
// moved (the viewport's Rebuild) mounts the root afresh, and only the layers that read what moved
// are built again. Its scene state is its own (E13): the environment's shader globals and the
// water's, published again whenever another device's state was published last; its frame's legs
// run only in a frame it renders (present), and the water's mirror pass is on only in such a frame
// (off from each frame's tick until its present).
class MissionViewportApplier final : public ViewportApplier {
public:
	explicit MissionViewportApplier(SubViewport &viewport);
	~MissionViewportApplier() override;

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

	// Its nodes and its placer, for the parity tests.
	Camera3D *camera() const { return camera_; }
	Terrain *terrain() const { return terrain_; }
	MissionEnvironment *environment() const { return environment_; }
	Ref<MissionObjectPlacer> placer() const { return placer_; }
	// How the entities stand: placed by the last whole placement and shown, lifted since and shown,
	// hidden; how many whole placements ran and what the last one cost (microseconds).
	int placed_count() const;
	int lifted_count() const;
	int hidden_count() const;
	int placements() const { return placements_; }
	int64_t last_place_us() const { return last_place_us_; }
	// The placer's key of the entity whose row is `row` (0: none, or lifted).
	int key_of(opennova::editor::NodeId row) const;

private:
	// One unit of a build, in the order they run.
	struct Unit {
		enum class Kind : uint8_t {
			Environment, // the .env file, the header's overrides, the clock
			TerrainFile, // one file of the terrain's load (TerrainData::load_step)
			TerrainBuild, // one step of the terrain's build (Terrain::build_step)
			Sky, // the sky dome bound to the environment
			Water, // the water bound to the terrain and the environment
			Items, // items.def, through a placer made over the root
			Model, // a graphic's model parsed and its static batches warmed
			Place, // one unit of the placement run
			Lift, // up to kLiftPerUnit added entities lifted
			Pose, // the entities moved, shown or hidden; the options, the time, the camera
		};
		Kind kind = Kind::Pose;
		String graphic; // Model
		size_t first = 0; // Lift: the first of its rows among the adds
	};
	// The layers a build makes, each with the files its units read.
	enum Layer : uint8_t { kEnvironment, kTerrain, kEntities, kLayers };
	static int layer_of_(Unit::Kind kind);
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
	// What one entity is to the device: placed (a static's key, or an individual model) or lifted,
	// shown or hidden, and the item and stamp it stands for.
	struct Placed {
		int64_t item = 0;
		uint32_t stamp = 0;
		int key = 0; // the placer's key (its ordinal at the placement, from 1); 0 lifted
		int kind = -1, index = -1; // its pool and index at the placement (its shadow source's key)
		uint64_t model = 0; // its ObjectModel's instance id: a placed individual model, or a lifted one
		bool lifted = false;
		bool hidden = false;
	};
	// A build in flight: what it brings the layers to, and its units.
	struct Build {
		bool environment = false, terrain = false, place = false;
		std::vector<Unit> units;
		size_t next = 0;
		std::vector<opennova::editor::NodeId> adds; // the rows the Lift units lift
		Ref<MissionPlacementRun> place_run; // the placement in flight (the Place units step it)
	};

	static constexpr size_t kLiftPerUnit = 8;
	static constexpr size_t kLiftedMost = 256;

	// The root mounted over the session's files afresh when the source is another or a file the
	// picture read moved: the layers that read what moved left to build again, the others keeping
	// their files noted.
	void mount_(const opennova::editor::SessionView &view);
	// The files noted since `from` given to `layer`.
	void note_reads_(int layer, size_t from);
	size_t reads_() const;
	void note_missing_(int layer, const String &name);
	void plan_(Build &build, const opennova::editor::MissionScene &scene);
	// One unit run, its reads given to its layer: false when the build failed (`failure` says why).
	bool run_(Build &build, const Unit &unit, const opennova::editor::ViewportModel &model, std::string &failure);
	void run_environment_(const opennova::editor::MissionScene &scene);
	void run_items_();
	void run_model_(const String &graphic);
	// The placement as a run (MissionPlacementRun): begun at the first Place unit, one unit of it
	// stepped per Place unit, the run's units joined to the build's as the run counts them.
	void run_place_begin_(const opennova::editor::MissionScene &scene, Build &build);
	void run_place_step_(Build &build);
	void run_lift_(const opennova::editor::MissionScene &scene, Build &build, size_t first);
	// One entity lifted as an individual model of its own (null: its item draws nothing).
	ObjectModel *lift_(const opennova::editor::MissionEntityMark &entity);
	// What the device shows of an entity, retired (hidden: a static's rows, a model's node) or shown.
	void show_(Placed &placed, bool shown);
	// The placement and the lifted models dropped, the placer with them.
	void drop_entities_();
	// The state over the picture that stands: the entities moved, shown or hidden; the layers'
	// visibility, the time of day, the camera.
	void apply_state_(const opennova::editor::ViewportModel &model);
	void move_entities_(const opennova::editor::MissionScene &scene);
	// The moved entities' terrain shadow sources moved too, once no gesture is open.
	void flush_shadows_(const opennova::editor::MissionScene &scene);
	void place_camera_(const opennova::editor::ViewportModel &model);
	String graphic_of_(int64_t item);
	Transform3D transform_of_(const opennova::editor::MissionEntityMark &entity) const;
	static ObjectModel *model_of_(const Placed &placed);

	Node3D *root_ = nullptr;
	WorldEnvironment *clear_ = nullptr;
	MissionEnvironment *environment_ = nullptr;
	SkyDome *sky_ = nullptr;
	Water *water_ = nullptr;
	Terrain *terrain_ = nullptr;
	uint64_t terrain_id_ = 0;
	Camera3D *camera_ = nullptr;
	Node3D *objects_ = nullptr;
	Node3D *lifted_root_ = nullptr;
	Ref<ResourceRoot> root_files_;
	std::shared_ptr<const opennova::editor::ProjectAssetSource> mounted_; // the source the root is mounted over
	std::shared_ptr<opennova::editor::StampedFiles> stamped_; // what the root was asked for
	opennova::editor::FileStamps layer_files_[kLayers];
	std::vector<std::string> layer_missing_[kLayers];
	Ref<TerrainData> terrain_data_;
	Ref<TerrainData> loading_; // the terrain data a build loads
	Ref<EnvFile> env_file_;
	Ref<MissionObjectPlacer> placer_;
	Ref<PanmClock> clock_;
	int64_t frame_ = 0;
	// What the layers hold.
	bool environment_built_ = false, terrain_built_ = false, placed_ = false;
	TerrainKey terrain_key_;
	EnvironmentKey environment_key_;
	std::unordered_map<opennova::editor::NodeId, Placed> entities_;
	std::vector<opennova::editor::NodeId> lifted_; // the rows lifted since the placement
	std::map<std::string, bool> warm_; // the graphics warmed (false: its model did not load)
	std::vector<opennova::editor::NodeId> shadow_pending_; // moved, their shadow sources not yet
	int placements_ = 0;
	int64_t last_place_us_ = 0;
	int64_t place_started_us_ = 0;
	int place_units_planned_ = 0;
	std::unique_ptr<Build> build_;
	uint64_t scene_state_ = 0;
	// The time applied (-2: none yet; -1 the mission's own; else the option's hour); the layers shown.
	double applied_time_ = -2.0;
	bool shown_water_ = true, shown_shadows_ = true;
};

} // namespace godot
