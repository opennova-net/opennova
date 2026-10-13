#include "world/world_scene.h"

#include <cmath>
#include <cstring>
#include <vector>

#include <formats/env/env.h>
#include <runtime/mission/mission_sidecars.h>
#include <runtime/mission/runtime_boot.h>

#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/world_environment.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/node_path.hpp>

#include "env/celestial.h"
#include "env/env_file.h"
#include "env/environment_cube_capture.h"
#include "env/mission_environment.h"
#include "env/mission_environment_overrides.h"
#include "env/precipitation.h"
#include "env/sky_dome.h"
#include "env/slot_shadow.h"
#include "env/sun_shadow.h"
#include "env/water.h"
#include "env/weather.h"
#include "render/frame_fx.h"
#include "resource_index/resource_root.h"
#include "terrain/foliage_dispatcher.h"
#include "terrain/terrain.h"
#include "terrain/terrain_data.h"
#include "terrain/terrain_tile_info.h"

namespace godot {

namespace {

// The capture layer the cube capture's cameras draw (the sky and the bodies
// join it): EnvironmentCubeCapture's capture mask.
constexpr uint32_t kEnvironmentCaptureLayerMask = 1024;

// A NodePath property a row takes and the row it names.
struct RowPath {
	const char *property;
	WorldSceneRow target;
};

// One row of game_world.tscn: its name, its class and the paths it takes, the
// capture layer where it draws into the cube.
struct RowSpec {
	WorldSceneRow row;
	const char *name;
	const char *class_name;
	RowPath paths[3];
	int path_count;
	uint32_t capture_layer_mask;
};

const RowSpec kRows[kWorldSceneRowCount] = {
	{ WorldSceneRow::ClearColor, "ClearColor", "WorldEnvironment", {}, 0, 0 },
	{ WorldSceneRow::FrameFx, "FrameFx", "FrameFx", {}, 0, 0 },
	{ WorldSceneRow::Terrain, "Terrain", "Terrain",
			{ { "environment_path", WorldSceneRow::Environment },
					{ "weather_path", WorldSceneRow::Weather },
					{ "water_path", WorldSceneRow::Water } },
			3, 0 },
	{ WorldSceneRow::Foliage, "FoliageDispatcher", "FoliageDispatcher", {}, 0, 0 },
	{ WorldSceneRow::Environment, "MissionEnvironment", "MissionEnvironment", {}, 0, 0 },
	{ WorldSceneRow::Sky, "SkyDome", "SkyDome",
			{ { "environment_path", WorldSceneRow::Environment },
					{ "weather_path", WorldSceneRow::Weather } },
			2, kEnvironmentCaptureLayerMask },
	{ WorldSceneRow::Weather, "Weather", "Weather",
			{ { "environment_path", WorldSceneRow::Environment } }, 1, 0 },
	{ WorldSceneRow::Precipitation, "Precipitation", "Precipitation",
			{ { "weather_path", WorldSceneRow::Weather } }, 1, 0 },
	{ WorldSceneRow::Water, "Water", "Water",
			{ { "environment_path", WorldSceneRow::Environment } }, 1, 0 },
	{ WorldSceneRow::EnvironmentCube, "EnvironmentCubeCapture", "EnvironmentCubeCapture", {}, 0,
			0 },
	{ WorldSceneRow::Celestial, "Celestial", "Celestial",
			{ { "environment_path", WorldSceneRow::Environment } }, 1,
			kEnvironmentCaptureLayerMask },
	{ WorldSceneRow::SunShadow, "SunShadow", "SunShadow", {}, 0, 0 },
	{ WorldSceneRow::SlotShadow, "SlotShadow", "SlotShadow", {}, 0, 0 },
};

const RowSpec &spec_of(WorldSceneRow p_row) {
	return kRows[static_cast<size_t>(p_row)];
}

NodePath sibling_path(WorldSceneRow p_target) {
	return NodePath(String("../") + spec_of(p_target).name);
}

// The frame clear GameWorld's ClearColor holds: a colour background (BG_SKY
// with no sky renders black and swallows the clear-colour writes) and no
// ambient light (the fixed-function shaders own every colour).
WorldEnvironment *make_clear_color() {
	WorldEnvironment *clear = memnew(WorldEnvironment);
	Ref<Environment> environment;
	environment.instantiate();
	environment->set_background(Environment::BG_COLOR);
	environment->set_ambient_source(Environment::AMBIENT_SOURCE_DISABLED);
	clear->set_environment(environment);
	return clear;
}

Node *make_row(WorldSceneRow p_row) {
	switch (p_row) {
		case WorldSceneRow::ClearColor:
			return make_clear_color();
		case WorldSceneRow::FrameFx:
			return memnew(FrameFx);
		case WorldSceneRow::Terrain:
			return memnew(Terrain);
		case WorldSceneRow::Foliage:
			return memnew(FoliageDispatcher);
		case WorldSceneRow::Environment:
			return memnew(MissionEnvironment);
		case WorldSceneRow::Sky:
			return memnew(SkyDome);
		case WorldSceneRow::Weather:
			return memnew(Weather);
		case WorldSceneRow::Precipitation:
			return memnew(Precipitation);
		case WorldSceneRow::Water:
			return memnew(Water);
		case WorldSceneRow::EnvironmentCube:
			return memnew(EnvironmentCubeCapture);
		case WorldSceneRow::Celestial:
			return memnew(Celestial);
		case WorldSceneRow::SunShadow:
			return memnew(SunShadow);
		case WorldSceneRow::SlotShadow:
			return memnew(SlotShadow);
		case WorldSceneRow::Count:
			break;
	}
	return nullptr;
}

void assign_row(WorldSceneNodes &r_nodes, WorldSceneRow p_row, Node *p_node) {
	switch (p_row) {
		case WorldSceneRow::ClearColor:
			r_nodes.clear_color = Object::cast_to<WorldEnvironment>(p_node);
			break;
		case WorldSceneRow::FrameFx:
			r_nodes.framefx = Object::cast_to<FrameFx>(p_node);
			break;
		case WorldSceneRow::Terrain:
			r_nodes.terrain = Object::cast_to<Terrain>(p_node);
			break;
		case WorldSceneRow::Foliage:
			r_nodes.foliage = Object::cast_to<FoliageDispatcher>(p_node);
			break;
		case WorldSceneRow::Environment:
			r_nodes.environment = Object::cast_to<MissionEnvironment>(p_node);
			break;
		case WorldSceneRow::Sky:
			r_nodes.sky = Object::cast_to<SkyDome>(p_node);
			break;
		case WorldSceneRow::Weather:
			r_nodes.weather = Object::cast_to<Weather>(p_node);
			break;
		case WorldSceneRow::Precipitation:
			r_nodes.precipitation = Object::cast_to<Precipitation>(p_node);
			break;
		case WorldSceneRow::Water:
			r_nodes.water = Object::cast_to<Water>(p_node);
			break;
		case WorldSceneRow::EnvironmentCube:
			r_nodes.environment_cube = Object::cast_to<EnvironmentCubeCapture>(p_node);
			break;
		case WorldSceneRow::Celestial:
			r_nodes.celestial = Object::cast_to<Celestial>(p_node);
			break;
		case WorldSceneRow::SunShadow:
			r_nodes.sun_shadow = Object::cast_to<SunShadow>(p_node);
			break;
		case WorldSceneRow::SlotShadow:
			r_nodes.slot_shadow = Object::cast_to<SlotShadow>(p_node);
			break;
		case WorldSceneRow::Count:
			break;
	}
}

} // namespace

WorldSceneRows WorldScene::all_rows() {
	WorldSceneRows rows;
	rows.set();
	return rows;
}

Array WorldScene::describe_rows() {
	Array out;
	for (const RowSpec &spec : kRows) {
		Dictionary row;
		row["name"] = spec.name;
		row["class"] = spec.class_name;
		Dictionary paths;
		for (int i = 0; i < spec.path_count; ++i) {
			paths[spec.paths[i].property] = sibling_path(spec.paths[i].target);
		}
		row["paths"] = paths;
		row["capture_layer_mask"] = static_cast<int64_t>(spec.capture_layer_mask);
		out.push_back(row);
	}
	return out;
}

WorldScene WorldScene::bind(Node &p_world) {
	WorldScene scene;
	for (const RowSpec &spec : kRows) {
		assign_row(scene.nodes_, spec.row, p_world.get_node_or_null(NodePath(spec.name)));
	}
	scene.wire_();
	return scene;
}

WorldScene WorldScene::create(Node &p_parent, WorldSceneRows p_rows, bool p_held_globals) {
	WorldScene scene;
	// In the table's order, each with its paths set before it enters the tree,
	// as the packaged scene's are: a row resolves its paths at the latest when
	// it draws, and the weather (which resolves its environment in _ready)
	// comes after the environment.
	for (const RowSpec &spec : kRows) {
		if (!p_rows.test(static_cast<size_t>(spec.row))) {
			continue;
		}
		Node *node = make_row(spec.row);
		node->set_name(spec.name);
		for (int i = 0; i < spec.path_count; ++i) {
			if (p_rows.test(static_cast<size_t>(spec.paths[i].target))) {
				node->set(spec.paths[i].property, sibling_path(spec.paths[i].target));
			}
		}
		if (spec.capture_layer_mask != 0) {
			node->set("environment_capture_layer_mask",
					static_cast<int64_t>(spec.capture_layer_mask));
		}
		assign_row(scene.nodes_, spec.row, node);
		// The hold before the tree: some rows write their globals as they enter it.
		if (p_held_globals) {
			scene.hold_globals(true);
		}
		p_parent.add_child(node);
	}
	scene.wire_();
	return scene;
}

void WorldScene::wire_() const {
	// The foliage dispatcher sits beside the terrain, never under it: the
	// indoors letter hides the terrain but closes only the dispatcher's detail
	// passes, so the MODEL masks draw on. It reads the native detail-cell
	// handoff and the composed surface through the terrain, and the detail
	// sway's phase off the weather oscillator's ring slot 0 (retail
	// g_EnvWaveOscRing[0] in Foliage_SetupVertexShaderConstants).
	if (nodes_.foliage != nullptr) {
		nodes_.foliage->set_terrain(nodes_.terrain);
		nodes_.foliage->set_weather(nodes_.weather);
	}
	if (nodes_.sun_shadow != nullptr) {
		nodes_.sun_shadow->set_environment_node(nodes_.environment);
	}
	// The render-slot entity ground shadows (the per-slot silhouette capture
	// and the terrain drape; engine/runtime/renderer/render_slot_shadow.h
	// carries the witness map) at the highest selectable retail profile,
	// SHADOWQUALITY=3: 512px captures and retail's half-rate stagger for
	// non-player slots. Detail 4 is an internal oversample tier (1024px slot 0
	// and all slots every frame), not the shipped maximum.
	if (nodes_.slot_shadow != nullptr) {
		nodes_.slot_shadow->set_shadow_detail(3);
		nodes_.slot_shadow->set_environment_node(nodes_.environment);
	}
}

// --- the load --------------------------------------------------------------------

WorldEnvironmentLoad WorldScene::load_environment(
		const Ref<ResourceRoot> &p_root, const String &p_trn_path, const String &p_env_path) const {
	WorldEnvironmentLoad report;
	if (nodes_.environment == nullptr) {
		return report;
	}
	Ref<EnvFile> env;
	env.instantiate();
	// The overcast table the overcast blend cross-fades against: the .trn's
	// keyframes and overcast.def's after them, as the same load takes them (an
	// empty one fades toward black, env #41).
	Ref<EnvFile> overcast;
	overcast.instantiate();
	// The terrain's .trn, overcast.def, then the .env over them; a .env that is
	// not there is skipped and the world runs on the earlier passes over the
	// engine's defaults (opennova::env::load_mission_env); the load goes on
	// either way.
	report.loaded = env->load_mission_environment(p_root, p_trn_path, p_env_path, overcast);
	if (!report.loaded && !p_env_path.is_empty() && p_root.is_valid()) {
		report.environment_missing = !p_root->has_file(p_env_path.get_file());
	}
	report.overcast_missing = p_root.is_valid() && !p_root->has_file(opennova::env::kOvercastFile);
	// MissionEnvironment's setter reloads and pushes its shader globals (unless held).
	nodes_.environment->set_environment_data(env);
	nodes_.environment->set_overcast_data(overcast);
	// The weather node is retained across loads. A replacement environment is a
	// discrete state change: retail snaps every colour block to the new
	// mission's targets instead of easing over from the previous currents.
	if (nodes_.weather != nullptr) {
		nodes_.weather->resync_colors();
	}
	if (nodes_.celestial != nullptr) {
		nodes_.celestial->set_resource_root(p_root);
	}
	if (nodes_.precipitation != nullptr) {
		nodes_.precipitation->set_resource_root(p_root);
	}
	// FrameFX's mission texture (the "ffscan" scanlines) draws from the render
	// CRT stream here, as Render_InitMissionTextures does.
	if (nodes_.framefx != nullptr) {
		nodes_.framefx->init_mission_textures();
	}
	if (nodes_.environment_cube != nullptr) {
		nodes_.environment_cube->force_capture();
	}
	return report;
}

void WorldScene::apply_mission_overrides(
		const Ref<MissionEnvironmentOverrides> &p_overrides) const {
	// EnvFile owns the other live-view overrides (a non-persistent layer: the
	// base .env is never mutated), while the water keeps the header's rung
	// distinct so a flagged zero still beats a nonzero terrain height.
	if (nodes_.environment != nullptr) {
		const Ref<EnvFile> env_data = nodes_.environment->get_environment_data();
		if (env_data.is_valid()) {
			env_data->apply_mission_overrides_or_clear(p_overrides);
		}
	}
	set_mission_water_height_override(
			p_overrides.is_valid() ? p_overrides->get_water_height_world_or_nan() : NAN);
}

void WorldScene::set_mission_water_height_override(float p_world_height) const {
	if (nodes_.water != nullptr) {
		nodes_.water->set_mission_water_height_override(p_world_height);
	}
}

void WorldScene::configure_mission_clock(int p_start_q8_8, int p_minutes_per_day) const {
	if (nodes_.environment != nullptr) {
		nodes_.environment->configure_mission_clock(p_start_q8_8, p_minutes_per_day);
	}
}

void WorldScene::prepare_world_driven_weather() const {
	if (nodes_.weather != nullptr) {
		nodes_.weather->prepare_world_driven();
	}
}

void WorldScene::prepare_autonomous_weather() const {
	if (nodes_.weather != nullptr) {
		// A world without a mission runs the environment's standalone weather
		// home: drop any bound Simulation first.
		nodes_.weather->bind_simulation(nullptr);
		nodes_.weather->prepare_autonomous();
	}
}

void WorldScene::run_mission_start_boundary() const {
	if (nodes_.weather != nullptr) {
		nodes_.weather->run_mission_start_boundary(nullptr);
	}
}

void WorldScene::bind_terrain(
		const Ref<TerrainData> &p_data, const Ref<TerrainTileInfo> &p_tiles) const {
	if (nodes_.terrain != nullptr) {
		nodes_.terrain->set_tile_info_override(p_tiles);
		nodes_.terrain->set_terrain_data(p_data);
	}
	if (nodes_.slot_shadow != nullptr) {
		// The shadow anchor march probes this terrain through the engine's
		// Terrain_GetHeightAtPosition port (the cite lives with the native
		// SlotShadow planner, godot/src/env/slot_shadow.cpp).
		nodes_.slot_shadow->set_terrain_data(p_data);
	}
	if (nodes_.water != nullptr) {
		nodes_.water->set_terrain_data(p_data);
	}
	if (nodes_.environment_cube != nullptr) {
		nodes_.environment_cube->set_terrain_data(p_data);
	}
	if (nodes_.celestial != nullptr) {
		// The glare occlusion rays march this terrain (env #14).
		nodes_.celestial->set_terrain_data(p_data);
	}
}

void WorldScene::clear_terrain() const {
	if (nodes_.foliage != nullptr) {
		nodes_.foliage->reset();
		nodes_.foliage->set_terrain_data(Ref<TerrainData>());
		nodes_.foliage->set_tile_info(Ref<TerrainTileInfo>());
	}
	if (nodes_.terrain != nullptr) {
		nodes_.terrain->set_terrain_data(Ref<TerrainData>());
		nodes_.terrain->clear_built();
	}
	bind_terrain(Ref<TerrainData>(), Ref<TerrainTileInfo>());
}

Array WorldScene::configure_foliage(
		const Ref<ResourceRoot> &p_root, const Ref<TerrainData> &p_data) const {
	if (nodes_.foliage == nullptr || p_data.is_null()) {
		return Array();
	}
	const Ref<TerrainTileInfo> tiles = nodes_.terrain != nullptr
			? nodes_.terrain->get_tile_info_override()
			: Ref<TerrainTileInfo>();
	nodes_.foliage->configure_for_terrain(p_root, p_data, tiles);
	return nodes_.foliage->get_slot_diagnostics();
}

// --- the hold (ADR 0046 S14, E13) ----------------------------------------------

void WorldScene::hold_globals(bool p_held) const {
	if (nodes_.environment != nullptr) {
		nodes_.environment->set_globals_held(p_held);
	}
	if (nodes_.water != nullptr) {
		nodes_.water->set_globals_held(p_held);
	}
	if (nodes_.weather != nullptr) {
		nodes_.weather->set_globals_held(p_held);
	}
	if (nodes_.environment_cube != nullptr) {
		nodes_.environment_cube->set_globals_held(p_held);
	}
}

void WorldScene::publish_globals() const {
	if (nodes_.environment != nullptr) {
		nodes_.environment->republish_shader_globals();
	}
	if (nodes_.water != nullptr) {
		const bool held = nodes_.water->is_globals_held();
		nodes_.water->set_globals_held(false);
		nodes_.water->set_world_rendering_enabled(true);
		nodes_.water->set_globals_held(held);
	}
	if (nodes_.weather != nullptr) {
		nodes_.weather->republish_globals();
	}
	if (nodes_.environment_cube != nullptr) {
		nodes_.environment_cube->republish_globals();
	}
}

// --- the terrain's load ------------------------------------------------------------

void prepare_terrain_load(
		TerrainData &p_data, const String &p_tile_set, const String &p_environment_file) {
	p_data.set_mission_tile_set(p_tile_set);
	p_data.set_mission_environment(p_environment_file);
}

Ref<TerrainTileInfo> read_mission_tiles(const Ref<ResourceRoot> &p_root,
		const std::string &p_mission_file, const std::string &p_terrain,
		const std::string &p_environment, MissionTilesRead *r_read) {
	if (r_read != nullptr) {
		*r_read = MissionTilesRead();
	}
	if (p_root.is_null()) {
		return Ref<TerrainTileInfo>();
	}
	std::vector<uint8_t> bytes;
	const std::string took = opennova::mission::read_placed_tiles(
			opennova::mission::boot_files_from_index(p_root->native_index()), p_mission_file,
			p_terrain, p_environment, bytes);
	if (took.empty()) {
		return Ref<TerrainTileInfo>();
	}
	PackedByteArray til;
	til.resize(static_cast<int64_t>(bytes.size()));
	if (!bytes.empty()) {
		std::memcpy(til.ptrw(), bytes.data(), bytes.size());
	}
	if (r_read != nullptr) {
		r_read->file = took;
		// The tiles taken are the terrain's own unless they are the mission's
		// (its tiles row's name).
		r_read->terrain_own = p_mission_file.empty() ||
				took !=
						opennova::mission::sidecar_name(
								p_mission_file, *opennova::mission::sidecar_for_role("tiles"));
		r_read->bytes = til;
	}
	Ref<TerrainTileInfo> tiles;
	tiles.instantiate();
	return tiles->load_from_bytes(til) == OK ? tiles : Ref<TerrainTileInfo>();
}

} // namespace godot
