#pragma once

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <base/vfs/file_stamps.h>
#include <editor/preview/mission_scene.h>
#include <editor/preview/model_preview_camera.h>

#include "authoring/preview_effects.h"
#include "authoring/preview_frame_effects.h"
#include "render/scene_overlay_compositor.h"
#include "resource_index/resource_root.h"
#include "terrain/terrain_data.h"
#include "world/world_scene.h"

namespace opennova {
class StampedFiles;
} // namespace opennova

namespace opennova::editor {
class OrbitCamera;
class ProjectAssetSource;
struct OperationProgress;
struct SessionView;
struct ViewportDeviceReport;
} // namespace opennova::editor

namespace godot {

class EffectLightDirector;

// The world a 3D preview of a mission draws (ADR 0046 S26, B46 over master's WorldScene, B14),
// shared by the mission, terrain and environment devices: the game's scene stack made from
// WorldScene's table (game_world.tscn's rows, names and paths: the frame clear, FrameFx, the
// terrain and its foliage, the environment, the sky dome, the weather, the drops, the water, the
// environment cube, the sun, moon, glare and glint, and the entities' slot shadows where the
// device's entities cast them; never the SunShadow, which no simulation drives here), every
// process-wide global its rows write held but while the device publishes its scene state and
// presents its frame (E13), the device's camera, the game's particle renderer for the overlay tail
// and the device's own effects, and the game's frame effects (FrameFx as the terminal compositor
// and display decode, the sun veil).
//
// Two of the device's layers are built here as the game's load builds them, each again only where
// its key or a file it read moved: the environment (WorldScene::load_environment: the .trn,
// overcast.def, then the .env over them with the load's post-steps; the header's overrides and
// clock; the weather's start, as a mission's start runs it with no simulation: seeded, then the
// 255-tick settle) and the terrain (TerrainData's load a file a unit, the mission's placed tiles,
// the build a tile a unit, the foliage; the sky and the water bound to both). What a load names and
// the project lacks is a note, worded as the game's load reports it (WorldEnvironmentLoad). The
// device's own layer (the mission's entities) notes its reads here too. A terrain load or build in
// flight under the same key, its files standing, carries into the next plan.
//
// The frame's legs run in game_world_frame.cpp's order, the device's own legs between them: the
// scene environment (the world pass's planes, the render eye), the environment nodes (the weather's
// publication, the sky, the bodies), the terrain, the water, the foliage, the sun veil's exposure
// feed, [the device's lights and levels], the drops, the overlay tail (the drops, the device's
// coronas, the murk, the glint and the glare, the mirror's closing draws), the frame effects, the
// clear colour and the environment cube. The weather ticks on the engine's state alone (the drops'
// fall, then the whole weather tick), outside the presented frame.
class PreviewSceneLayers {
public:
	enum Layer : uint8_t { kEnvironment, kTerrain, kEntities, kLayers };

	// What the world is made of: the mission header's names and fields, the mission whose .til the
	// terrain's load reads (its base name), the .env the world takes (the header's environment with
	// ".env", or the environment view's own file; "" none), whether the terrain is drawn at all
	// (the environment view's option) and whether the header's clock is set (a terrain view with no
	// mission runs on the engine's own).
	struct Source {
		opennova::editor::MissionSceneHeader header;
		std::string mission;
		std::string environment;
		bool terrain = true;
		bool clock = true;
	};
	// Which layers read a file that moved.
	struct Moved {
		bool layer[kLayers] = { false, false, false };
	};
	// The layers the device's options show.
	struct Shown {
		bool terrain = true, sky = true, water = true, foliage = true;
	};

	// The stack under a root named `name` in `viewport`: every row but the SunShadow, the
	// SlotShadow only where `slot_shadows`.
	PreviewSceneLayers(SubViewport &viewport, const char *name, bool slot_shadows);
	~PreviewSceneLayers();
	PreviewSceneLayers(const PreviewSceneLayers &) = delete;
	PreviewSceneLayers &operator=(const PreviewSceneLayers &) = delete;

	Node3D &root() const { return *root_; }
	const WorldSceneNodes &nodes() const { return scene_.nodes(); }
	Camera3D *camera() const { return camera_; }
	PreviewEffects &effects() { return *effects_; }
	const PreviewEffects &effects() const { return *effects_; }
	const Ref<ResourceRoot> &files() const { return root_files_; }
	const std::shared_ptr<opennova::StampedFiles> &stamped() const { return stamped_; }

	// --- the files
	// ---------------------------------------------------------------------------------------------
	// The root mounted over the session's project files: the layers that read what moved (none
	// where nothing did), a move no layer's units read (a texture a model read as it was first
	// drawn) `unread`'s. A layer that read what moved builds again; the others keep their notes.
	// The .env and overcast.def the terrain's load read are the terrain's by the lines of them its
	// parser takes, which its key holds (D-TERRAIN-18).
	Moved mount(const opennova::editor::SessionView &view, Layer unread);
	// The files read since `from` (reads()) noted as `layer`'s.
	size_t reads() const;
	void note_reads(Layer layer, size_t from);
	// A file `layer`'s load named that the project lacks; the notes of one layer dropped.
	void note_missing(Layer layer, const std::string &name);
	void clear_missing(Layer layer);
	// The notes, the files read and whether the terrain stands, into a device's report.
	void report(opennova::editor::ViewportDeviceReport &report) const;
	// A patched file's read taking the stamp it was patched at (S25: its move then builds nothing
	// again).
	void restamp(const std::string &name, uint64_t stamp);

	// --- the build
	// ---------------------------------------------------------------------------------------------
	// The layers whose keys moved, planned as units (none where both stand): whether the
	// environment builds, whether the terrain does, and whether its load begins anew (not carried).
	struct Plan {
		bool environment = false, terrain = false, loads = false;
	};
	Plan plan(const Source &source);
	bool building() const { return next_ < units_.size(); }
	// One unit; false once none is left.
	bool step();
	size_t units_done() const { return next_; }
	size_t units_planned() const { return units_.size(); }
	const char *step_label() const;
	// Nothing drawn: no terrain, no environment, the plan and every note dropped.
	void clear();
	bool environment_built() const { return environment_built_; }
	bool terrain_built() const { return terrain_built_; }
	const Ref<TerrainData> &terrain_data() const { return terrain_data_; }
	// The foliage stands configured over the terrain built.
	bool foliage_configured() const { return foliage_data_.is_valid(); }
	// The placed tiles the terrain's load took (empty: none) and whether they are the terrain's
	// own.
	const MissionTilesRead &tiles_read() const { return tiles_read_; }
	int environment_builds() const { return environment_builds_; }
	int terrain_builds() const { return terrain_builds_; }

	// --- the state
	// ---------------------------------------------------------------------------------------------
	void set_shown(const Shown &shown);
	const Shown &shown() const { return shown_; }
	// The camera where the orbit camera is, its planes the world pass's.
	void place_camera(const opennova::editor::OrbitCamera &camera);
	// The clock the picture shows: `minute_of_day` (0 to 1440), or the header's start (negative).
	// It stands there: no clock advance runs here (as a day length of 0 stands the game's,
	// formats/env/tod_clock.h), and its colours are taken at once.
	void show_time(double minute_of_day);
	// The weather's start, where an environment build asked for one (true: it ran now).
	bool start_weather();
	// `ticks` of the game's weather on the engine's state: the drops' fall (the entity update's,
	// before the weather tick in the game's frame), then the whole weather tick.
	void run_weather(int ticks);
	// The weather on the preview clock's game ticks: the start where one is asked, then the ticks
	// since the last (at most the runtime's catch-up; a seek back holds it), the clock standing
	// where show_time left it.
	void follow_clock(int32_t ticks);
	int64_t ticks_run() const { return ticks_run_; }

	// --- the frame
	// ---------------------------------------------------------------------------------------------
	// The scene state its picture renders with (E13): a value of its own, renewed where the
	// environment, the terrain or the water moved; and every global it renders with written again.
	uint64_t scene_state() const { return scene_state_; }
	void touch_scene_state();
	void publish_scene_state() const;
	// The frame's legs in game_world_frame.cpp's order; begin_frame lets the rows write their
	// globals for the frame, end_frame holds them again.
	void begin_frame();
	void present_environment(double dt);
	void present_ground(double dt, int64_t clock_ms);
	void present_sun_veil();
	// The particle leg: the effects' scene as its owner stepped it (none for a device with no
	// effects), partitioned by the water's plane, at `clock_ms`.
	void present_particles(int64_t clock_ms);
	void present_drops();
	void present_overlay(EffectLightDirector *lights);
	void present_frame_effects();
	void present_clear_and_cube();
	void end_frame();
	// Whether the water draws this frame (its plane active and shown), and its height.
	bool water_drawn() const;
	float water_height() const;

	// --- the ground
	// --------------------------------------------------------------------------------------------
	bool ground_at(double x, double y, double &height) const;
	bool surface_between(const double from[3], const double to[3], double point[3]) const;

	// The drawn world's environment as the game's render diagnostics word it (godot/game/world/
	// game_render_diagnostics.gd's environment block, its water's and its bodies' facts).
	opennova::io::JsonValue environment_json() const;
	// The foliage the last presented frame drew.
	opennova::io::JsonValue foliage_json() const;

private:
	struct Unit {
		enum class Kind : uint8_t {
			Environment, // the .env over the .trn and overcast.def, the overrides, the clock, the
						 // weather's start
			TerrainFile, // one file of the terrain's load; the placed tiles with the last
			TerrainBuild, // one step of the terrain's build
			NoTerrain, // the terrain did not load: the layer empty
			Sky, // the sky dome bound to the environment
			Water, // the water bound to the terrain and the environment
		};
		Kind kind = Kind::Environment;
	};
	struct TerrainKey {
		std::string terrain, tile_set, mission, environment, later;
		bool operator==(const TerrainKey &o) const;
	};
	struct EnvironmentKey {
		std::string environment, terrain;
		bool clock = true;
		int start_time = 0, minutes_per_day = 0;
		uint32_t attrib_flags = 0;
		int water_override = 0, fog_override = 0, water_murk = 0;
		int fog_color[3] = { 0, 0, 0 }, water_color[3] = { 0, 0, 0 };
		bool operator==(const EnvironmentKey &o) const;
	};
	TerrainKey terrain_key_of_(const Source &source) const;
	static EnvironmentKey environment_key_of_(const Source &source);
	static Layer layer_of_(Unit::Kind kind);
	void run_(Unit::Kind kind);
	void run_environment_();
	void run_terrain_file_();
	void run_terrain_build_();
	void terrain_empty_();
	void drop_terrain_units_(bool files);
	void configure_foliage_();
	void stand_clock_();
	static int32_t floor_height_(void *ctx, int32_t x, int32_t y);

	Node3D *root_ = nullptr;
	WorldScene scene_;
	Camera3D *camera_ = nullptr;
	std::unique_ptr<PreviewEffects> effects_;
	std::unique_ptr<PreviewFrameEffects> frame_effects_;
	Ref<ResourceRoot> root_files_;
	std::shared_ptr<const opennova::editor::ProjectAssetSource> mounted_;
	std::shared_ptr<opennova::StampedFiles> stamped_;
	opennova::FileStamps layer_files_[kLayers];
	std::vector<std::string> layer_missing_[kLayers];
	bool terrain_moved_ = false; // the last mount moved a file the terrain read

	Source source_;
	std::vector<Unit> units_;
	size_t next_ = 0;
	TerrainKey planned_terrain_; // the terrain the plan's units load or build
	Ref<TerrainData> terrain_data_;
	Ref<TerrainData> loading_;
	Ref<TerrainData> foliage_data_; // the terrain its foliage is configured over
	MissionTilesRead tiles_read_;
	bool environment_built_ = false, terrain_built_ = false;
	bool binding_ = false; // the sky and the water planned and not yet bound to what was built
	TerrainKey terrain_key_;
	EnvironmentKey environment_key_;
	int environment_builds_ = 0, terrain_builds_ = 0;

	Shown shown_;
	double shown_time_ = -1.0;
	bool weather_start_ = false;
	int32_t clock_tick_ = -1; // the preview clock's tick the weather stands at (-1: not started)
	int64_t ticks_run_ = 0;
	uint64_t scene_state_ = 0;
	uint64_t overlay_frame_id_ = 0;
	SceneOverlayModelSurfaces overlay_bodies_;
};

} // namespace godot
