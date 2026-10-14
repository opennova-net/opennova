#include "authoring/preview_scene_layers.h"

#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/world_environment.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>

#include <base/io/fixed.h>
#include <editor/assets/project_asset_source.h>
#include <editor/preview/model_preview_camera.h>
#include <editor/preview/viewport_device.h>
#include <editor/session/view/session_view.h>
#include <formats/env/env.h>
#include <runtime/environment/environment_state.h>
#include <runtime/environment/precipitation.h>
#include <runtime/environment/weather_runtime.h>

#include "env/celestial.h"
#include "env/celestial_overlay.h"
#include "env/env_file.h"
#include "env/environment_cube_capture.h"
#include "env/mission_environment.h"
#include "env/mission_environment_overrides.h"
#include "env/precipitation.h"
#include "env/sky_dome.h"
#include "env/water.h"
#include "env/weather.h"
#include "lights/effect_light_director.h"
#include "mission/mission_object_placer.h"
#include "particle/particle_renderer.h"
#include "render/frame_fx.h"
#include "terrain/foliage_dispatcher.h"
#include "terrain/foliage_frame_stats.h"
#include "terrain/terrain.h"
#include "terrain/terrain_tile_info.h"
#include "util/string_convert.h"

namespace godot {

namespace {

using opennova::io::json_number;
using opennova::io::JsonValue;

// Each device's scene state is its own: a counter no two share, never 0 (0 is the shipped
// defaults').
uint64_t next_scene_state() {
	static uint64_t next = 0;
	return ++next;
}

Vector3 to_godot(const opennova::editor::PreviewVec3 &v) {
	return Vector3(v.x, v.y, v.z);
}

// The header's .env name with its extension cut: the name the terrain's load and the placed tiles'
// read take.
std::string environment_name(const std::string &file) {
	if (file.size() >= 4) {
		std::string tail = file.substr(file.size() - 4);
		std::transform(tail.begin(), tail.end(), tail.begin(),
				[](unsigned char c) { return char(std::tolower(c)); });
		if (tail == ".env")
			return file.substr(0, file.size() - 4);
	}
	return file;
}

JsonValue json_vector(const Vector3 &v) {
	JsonValue out = JsonValue::make_array();
	out.push(json_number(v.x));
	out.push(json_number(v.y));
	out.push(json_number(v.z));
	return out;
}

} // namespace

bool PreviewSceneLayers::TerrainKey::operator==(const TerrainKey &o) const {
	return terrain == o.terrain && tile_set == o.tile_set && mission == o.mission &&
			environment == o.environment && later == o.later;
}

bool PreviewSceneLayers::EnvironmentKey::operator==(const EnvironmentKey &o) const {
	return environment == o.environment && terrain == o.terrain && clock == o.clock &&
			start_time == o.start_time && minutes_per_day == o.minutes_per_day &&
			attrib_flags == o.attrib_flags && water_override == o.water_override &&
			fog_override == o.fog_override && water_murk == o.water_murk &&
			std::equal(std::begin(fog_color), std::end(fog_color), std::begin(o.fog_color)) &&
			std::equal(std::begin(water_color), std::end(water_color), std::begin(o.water_color));
}

PreviewSceneLayers::PreviewSceneLayers(SubViewport &viewport, const char *name, bool slot_shadows) :
		scene_state_(next_scene_state()) {
	// Single-sampled, as the game's own view draws: the particle renderer's passes bind the view's
	// depth (DI-14's rule: a multisampled view's resolved depth is no attachment they can bind).
	viewport.set_msaa_3d(Viewport::MSAA_DISABLED);
	root_ = memnew(Node3D);
	root_->set_name(name);
	viewport.add_child(root_);
	// The game's scene stack from its one table, every process-wide global held from before the
	// rows enter the tree (E13): only this device's publication and its presented frames write
	// them.
	WorldSceneRows rows = WorldScene::all_rows();
	rows.reset(size_t(WorldSceneRow::SunShadow));
	if (!slot_shadows)
		rows.reset(size_t(WorldSceneRow::SlotShadow));
	scene_ = WorldScene::create(*root_, rows, true);
	camera_ = memnew(Camera3D);
	camera_->set_name("Camera"); // the device's own: the water's mirror has a camera of its own
	camera_->set_keep_aspect_mode(Camera3D::KEEP_WIDTH);
	camera_->set_fov(opennova::editor::OrbitCamera::fov_horizontal_degrees());
	camera_->set_current(true);
	root_->add_child(camera_);
	// The game's particle renderer over the scene, under the environment's fog and tints: the
	// overlay tail's passes and the device's own effects.
	effects_ = std::make_unique<PreviewEffects>(*root_);
	effects_->set_environment_source(nodes().environment);
	// The scene's FrameFx as the terminal compositor (the bloom, the display decode), the sun veil
	// over the picture.
	frame_effects_ = std::make_unique<PreviewFrameEffects>(*nodes().framefx, viewport);
	root_files_.instantiate();
}

PreviewSceneLayers::~PreviewSceneLayers() = default;

// --- the files -----------------------------------------------------------------------------------

PreviewSceneLayers::Moved PreviewSceneLayers::mount(
		const opennova::editor::SessionView &view, Layer unread) {
	Moved moved;
	const std::shared_ptr<const opennova::editor::ProjectAssetSource> source = view.findings.assets;
	if (source != mounted_ || !stamped_) {
		for (bool &layer : moved.layer)
			layer = true;
	} else {
		if (!stamped_->stamps().moved(*source)) {
			terrain_moved_ = false;
			return moved;
		}
		moved.layer[kEnvironment] = layer_files_[kEnvironment].moved(*source);
		// The .env the terrain's load reads is the planned one's (the key of the terrain built or
		// building; the built one's stays empty until a build ends).
		moved.layer[kTerrain] = layer_files_[kTerrain].moved_but(
				*source, { planned_terrain_.environment, opennova::env::kOvercastFile });
		moved.layer[kEntities] = layer_files_[kEntities].moved(*source);
		if (!moved.layer[kEnvironment] && !moved.layer[kTerrain] && !moved.layer[kEntities])
			moved.layer[unread] = true;
	}
	// A fresh record of what is asked for, the root mounted over it (its own caches dropped: a file
	// moved); the layers that read none of what moved keep their files noted. Nothing drawn changes
	// here: the layers that read what moved are built again by the plan's units.
	mounted_ = source;
	stamped_ = std::make_shared<opennova::StampedFiles>(source);
	root_files_->mount_files(stamped_);
	for (int layer = 0; layer < kLayers; ++layer) {
		if (moved.layer[layer]) {
			layer_files_[layer].clear();
			layer_missing_[layer].clear();
			continue;
		}
		for (const opennova::FileStamp &file : layer_files_[layer].files())
			stamped_->stamp(file.name);
	}
	if (moved.layer[kEnvironment])
		environment_built_ = false;
	if (moved.layer[kTerrain])
		terrain_built_ = false;
	terrain_moved_ = moved.layer[kTerrain];
	return moved;
}

size_t PreviewSceneLayers::reads() const {
	return stamped_ ? stamped_->stamps().files().size() : 0;
}

void PreviewSceneLayers::note_reads(Layer layer, size_t from) {
	if (!stamped_ || layer >= kLayers)
		return;
	const std::vector<opennova::FileStamp> &files = stamped_->stamps().files();
	for (size_t i = from; i < files.size(); ++i)
		layer_files_[layer].note(files[i].name, files[i].stamp);
}

void PreviewSceneLayers::note_missing(Layer layer, const std::string &name) {
	std::vector<std::string> &missing = layer_missing_[layer];
	if (std::find(missing.begin(), missing.end(), name) == missing.end())
		missing.push_back(name);
}

void PreviewSceneLayers::clear_missing(Layer layer) {
	layer_missing_[layer].clear();
}

void PreviewSceneLayers::report(opennova::editor::ViewportDeviceReport &report) const {
	report.missing.clear();
	for (const std::vector<std::string> &missing : layer_missing_)
		report.missing.insert(report.missing.end(), missing.begin(), missing.end());
	report.surface = terrain_built_ && terrain_data_.is_valid();
	if (stamped_)
		report.files = stamped_->stamps();
}

void PreviewSceneLayers::restamp(const std::string &name, uint64_t stamp) {
	if (stamped_)
		stamped_->restamp(name, stamp);
	for (opennova::FileStamps &files : layer_files_)
		files.restamp(name, stamp);
}

// --- the build -----------------------------------------------------------------------------------

PreviewSceneLayers::TerrainKey PreviewSceneLayers::terrain_key_of_(const Source &source) const {
	TerrainKey key;
	if (!source.terrain)
		return key;
	key.terrain = source.header.terrain;
	key.tile_set = source.header.tile_set;
	key.mission = source.mission;
	// The .env the terrain's load reads after its .trn and overcast.def, with the lines of the two
	// its parser can take (D-TERRAIN-18, editor::mission_terrain_later_lines): an edit of either
	// that sets nothing of the terrain loads it not again.
	key.environment = source.environment;
	if (!key.terrain.empty() && stamped_)
		key.later = opennova::editor::mission_terrain_later_lines(*stamped_, key.environment);
	return key;
}

PreviewSceneLayers::EnvironmentKey PreviewSceneLayers::environment_key_of_(const Source &source) {
	const opennova::editor::MissionSceneHeader &header = source.header;
	EnvironmentKey key;
	key.environment = source.environment;
	key.terrain = header.terrain;
	key.clock = source.clock;
	key.start_time = header.start_time;
	key.minutes_per_day = header.minutes_per_day;
	key.attrib_flags = header.attrib_flags;
	key.water_override = header.water_override;
	key.fog_override = header.fog_override;
	key.water_murk = header.water_murk;
	for (int i = 0; i < 3; ++i) {
		key.fog_color[i] = header.fog_color[i];
		key.water_color[i] = header.water_color[i];
	}
	return key;
}

PreviewSceneLayers::Layer PreviewSceneLayers::layer_of_(Unit::Kind kind) {
	switch (kind) {
		case Unit::Kind::Environment:
		case Unit::Kind::Sky:
			return kEnvironment;
		case Unit::Kind::TerrainFile:
		case Unit::Kind::TerrainBuild:
		case Unit::Kind::NoTerrain:
		case Unit::Kind::Water:
			break;
	}
	return kTerrain;
}

PreviewSceneLayers::Plan PreviewSceneLayers::plan(const Source &source) {
	Plan out;
	const TerrainKey terrain_key = terrain_key_of_(source);
	// A terrain load or build in flight under this key, its files standing: its units left go on
	// where it was (an edit while a large terrain builds does not begin it again).
	std::vector<Unit> carried;
	if (building() && !terrain_moved_ && terrain_key == planned_terrain_ &&
			(loading_.is_valid() || nodes().terrain->is_building())) {
		for (size_t i = next_; i < units_.size(); ++i)
			if (units_[i].kind == Unit::Kind::TerrainFile ||
					units_[i].kind == Unit::Kind::TerrainBuild)
				carried.push_back(units_[i]);
	}
	if (carried.empty())
		loading_.unref();
	units_.clear();
	next_ = 0;
	source_ = source;
	out.environment = !environment_built_ || !(environment_key_of_(source) == environment_key_);
	out.terrain = !terrain_built_ || !(terrain_key == terrain_key_) || !carried.empty();
	planned_terrain_ = terrain_key;
	if (out.environment) {
		clear_missing(kEnvironment);
		units_.push_back(Unit{ Unit::Kind::Environment });
	}
	if (!carried.empty()) {
		units_.insert(units_.end(), carried.begin(), carried.end());
	} else if (out.terrain) {
		// The .trn parsed as the load begins (its units planned then): one unit per file, as many
		// as the load will have; a terrain the project lacks is a note, the picture standing
		// without one.
		out.loads = true;
		clear_missing(kTerrain);
		loading_.instantiate();
		prepare_terrain_load(**loading_, opennova::to_gd(terrain_key.tile_set),
				opennova::to_gd(environment_name(terrain_key.environment)));
		const String trn = opennova::to_gd(terrain_key.terrain) + ".trn";
		const size_t from = reads();
		const bool begun = !terrain_key.terrain.empty() && root_files_->has_file(trn) &&
				loading_->begin_load_from_resource_root(root_files_, trn) == OK;
		note_reads(kTerrain, from);
		if (begun) {
			for (int i = 0; i < loading_->get_load_step_count(); ++i)
				units_.push_back(Unit{ Unit::Kind::TerrainFile });
			// The build's units are planned as it begins (a tile each): counted then.
			units_.push_back(Unit{ Unit::Kind::TerrainBuild });
		} else {
			if (!terrain_key.terrain.empty())
				note_missing(kTerrain, opennova::to_std(trn));
			loading_.unref();
			// No terrain: the one held dropped by a unit (not as the plan is made).
			units_.push_back(Unit{ Unit::Kind::NoTerrain });
		}
	}
	// The sky and the water bound to what was built, again where a plan cut short left them
	// unbound.
	if (out.environment || out.terrain || binding_) {
		binding_ = true;
		units_.push_back(Unit{ Unit::Kind::Sky });
		units_.push_back(Unit{ Unit::Kind::Water });
	}
	return out;
}

bool PreviewSceneLayers::step() {
	if (!building())
		return false;
	const Unit::Kind kind = units_[next_++].kind;
	const size_t from = reads();
	run_(kind);
	note_reads(layer_of_(kind), from);
	return building();
}

const char *PreviewSceneLayers::step_label() const {
	if (!building())
		return "";
	switch (units_[next_].kind) {
		case Unit::Kind::Environment:
			return "environment";
		case Unit::Kind::TerrainFile:
			return "terrain files";
		case Unit::Kind::TerrainBuild:
		case Unit::Kind::NoTerrain:
			return "terrain";
		case Unit::Kind::Sky:
			return "sky";
		case Unit::Kind::Water:
			return "water";
	}
	return "";
}

void PreviewSceneLayers::run_(Unit::Kind kind) {
	switch (kind) {
		case Unit::Kind::Environment:
			run_environment_();
			break;
		case Unit::Kind::TerrainFile:
			run_terrain_file_();
			break;
		case Unit::Kind::TerrainBuild:
			run_terrain_build_();
			break;
		case Unit::Kind::NoTerrain:
			terrain_empty_();
			break;
		case Unit::Kind::Sky:
			nodes().sky->build();
			break;
		case Unit::Kind::Water:
			nodes().water->build();
			binding_ = false;
			touch_scene_state();
			break;
	}
}

void PreviewSceneLayers::run_environment_() {
	const opennova::editor::MissionSceneHeader &header = source_.header;
	const String trn = header.terrain.empty() ? String() : opennova::to_gd(header.terrain) + ".trn";
	const String environment = opennova::to_gd(source_.environment);
	// As the game's load reads it (WorldScene::load_environment): the terrain's .trn, overcast.def
	// (the overcast table), then the .env over them, a .env that is not there skipped with the
	// earlier passes standing over the engine's own environment (env-tod-re.md #38, #43); the
	// weather's colours snapped, the bodies and the drops read through the project's files,
	// FrameFX's mission texture, the cube captured again. What the load lacks is a note, as the
	// game's load reports it.
	const WorldEnvironmentLoad load = scene_.load_environment(root_files_, trn, environment);
	if (load.environment_missing)
		note_missing(kEnvironment, source_.environment);
	if (load.overcast_missing)
		note_missing(kEnvironment, opennova::env::kOvercastFile);
	// The header's overrides over the file (the game's apply_mission_environment_overrides), the
	// water's rung with them.
	Ref<MissionEnvironmentOverrides> overrides;
	overrides.instantiate();
	overrides->assign(
			opennova::env::bms_env_overrides_from_header(header.attrib_flags, header.water_override,
					header.fog_override, header.fog_color, header.water_color, header.water_murk));
	scene_.apply_mission_overrides(overrides);
	// The texts the load read are the environment's whatever unit read one first (the .trn the
	// terrain's units stamped already): an edit of one builds the environment again.
	for (const String &text : { trn, environment, opennova::to_gd(opennova::env::kOvercastFile) })
		if (!text.is_empty())
			layer_files_[kEnvironment].note(
					opennova::to_std(text), stamped_->stamp(opennova::to_std(text)));
	// The clock the mission's start sets; with no mission the engine's own
	// (Environment_InitDefaults' curtime).
	if (source_.clock)
		scene_.configure_mission_clock(header.start_time, header.minutes_per_day);
	// The weather started as a mission's start starts it, at the next tick (start_weather).
	weather_start_ = true;
	environment_key_ = environment_key_of_(source_);
	environment_built_ = true;
	++environment_builds_;
	touch_scene_state();
}

void PreviewSceneLayers::terrain_empty_() {
	loading_.unref();
	// No terrain, no foliage: every row lets go of it, the ground drawn before with it (never the
	// last terrain under a note that the new one is missing).
	scene_.clear_terrain();
	foliage_data_.unref();
	terrain_data_.unref();
	tiles_read_ = MissionTilesRead();
	terrain_built_ = true;
	terrain_key_ = planned_terrain_;
	touch_scene_state();
}

void PreviewSceneLayers::drop_terrain_units_(bool files) {
	units_.erase(std::remove_if(units_.begin() + std::ptrdiff_t(next_), units_.end(),
						 [files](const Unit &u) {
							 return u.kind == Unit::Kind::TerrainBuild ||
									 (files && u.kind == Unit::Kind::TerrainFile);
						 }),
			units_.end());
}

void PreviewSceneLayers::run_terrain_file_() {
	if (loading_.is_null())
		return;
	const TerrainData::LoadStep result = loading_->load_step();
	if (result == TerrainData::LOAD_STEP_MORE)
		return;
	// What the load named and did not find (a texture, a map, the height data): notes.
	for (const std::string &name : loading_->get_load_missing())
		note_missing(kTerrain, name);
	// The rest of the terrain's units go with a load that failed or a build that cannot begin: the
	// layer is empty (no surface, no ground), the picture stands without it.
	if (result == TerrainData::LOAD_STEP_FAILED) {
		const String why = loading_->get_load_failure();
		if (!why.is_empty())
			note_missing(kTerrain, opennova::to_std(why));
		drop_terrain_units_(true);
		terrain_empty_();
		return;
	}
	terrain_data_ = loading_;
	loading_.unref();
	// The tiles the mission places, read as the game's load reads them before the build
	// (read_mission_tiles: <mission>.til, else the terrain's own polytrn_tileinfo, each where the
	// game's loader takes it), then the terrain bound to every row that reads it.
	const Ref<TerrainTileInfo> tiles = read_mission_tiles(root_files_, planned_terrain_.mission,
			planned_terrain_.terrain, environment_name(planned_terrain_.environment), &tiles_read_);
	scene_.bind_terrain(terrain_data_, tiles);
	terrain_built_ = false;
	touch_scene_state();
	Terrain *terrain = nodes().terrain;
	if (!terrain->build_begin()) {
		drop_terrain_units_(true);
		terrain_empty_();
		return;
	}
	// Its units, a tile each, counted now that they are planned (the one planned stands for the
	// first).
	const int steps = terrain->get_build_step_count();
	for (int i = 1; i < steps; ++i)
		units_.insert(units_.begin() + std::ptrdiff_t(next_), Unit{ Unit::Kind::TerrainBuild });
}

void PreviewSceneLayers::run_terrain_build_() {
	const Terrain::BuildStep result = nodes().terrain->build_step();
	if (result == Terrain::BUILD_STEP_FAILED) {
		// A terrain with no tile (no height data): the layer empty.
		drop_terrain_units_(false);
		terrain_empty_();
		return;
	}
	if (result == Terrain::BUILD_STEP_DONE) {
		terrain_built_ = true;
		terrain_key_ = planned_terrain_;
		++terrain_builds_;
		// Its foliage as the game's load configures it after the build (the reads its slots make
		// are the terrain layer's: a foliage model or texture moved builds the terrain again).
		configure_foliage_();
	}
}

void PreviewSceneLayers::configure_foliage_() {
	foliage_data_ = terrain_data_;
	// WorldScene::configure_foliage (GameWorld's): the terrain's detail cells and foliage map, its
	// placed tiles as the candidate blocker, each definition's model and :fd texture through the
	// root. A definition whose model the project lacks: a note, its slot drawing nothing.
	nodes().foliage->reset();
	const Array diagnostics = scene_.configure_foliage(root_files_, terrain_data_);
	for (int64_t i = 0; i < diagnostics.size(); ++i) {
		const Dictionary diagnostic = diagnostics[i];
		const String status = diagnostic.get("status", "");
		const String graphic = diagnostic.get("graphic", "");
		if ((status == "missing_mesh" || status == "invalid_mesh") && !graphic.is_empty())
			note_missing(kTerrain,
					opennova::to_std(graphic.get_extension().is_empty() ? graphic + String(".3di")
																		: graphic));
	}
}

void PreviewSceneLayers::clear() {
	units_.clear();
	next_ = 0;
	loading_.unref();
	scene_.clear_terrain();
	foliage_data_.unref();
	terrain_data_.unref();
	tiles_read_ = MissionTilesRead();
	terrain_built_ = false;
	terrain_key_ = TerrainKey();
	binding_ = false;
	nodes().environment->set_environment_data(Ref<EnvFile>());
	nodes().precipitation->hide_frame();
	environment_built_ = false;
	weather_start_ = false;
	clock_tick_ = -1;
	for (std::vector<std::string> &missing : layer_missing_)
		missing.clear();
	touch_scene_state();
}

// --- the state -----------------------------------------------------------------------------------

void PreviewSceneLayers::set_shown(const Shown &shown) {
	const WorldSceneNodes &n = nodes();
	n.terrain->set_visible(shown.terrain);
	// The sun, moon, glare and glint are the sky's.
	n.sky->set_visible(shown.sky);
	n.celestial->set_visible(shown.sky);
	if (shown.water != shown_.water)
		touch_scene_state(); // the water's globals with it
	n.water->set_visible(shown.water);
	n.foliage->set_visible(shown.foliage);
	shown_ = shown;
}

void PreviewSceneLayers::place_camera(const opennova::editor::OrbitCamera &camera) {
	opennova::editor::PreviewVec3 right, up, back;
	camera.axes(right, up, back);
	camera_->set_transform(Transform3D(
			Basis(to_godot(right), to_godot(up), to_godot(back)), to_godot(camera.eye())));
	// The world pass's planes: the game's near, its far from the environment's fog.
	nodes().environment->apply_scene_pass_planes(*camera_);
}

void PreviewSceneLayers::stand_clock_() {
	// The preview's clock stands where it is shown: the weather's ticks advance it by nothing, as a
	// day length of 0 stands the game's (env::tod_advance_per_tick).
	nodes().weather->runtime().state().tod_advance_per_tick = 0;
}

void PreviewSceneLayers::show_time(double minute_of_day) {
	shown_time_ = minute_of_day;
	if (!environment_built_)
		return;
	MissionEnvironment *environment = nodes().environment;
	if (minute_of_day >= 0.0) {
		environment->debug_set_mission_minute_of_day(minute_of_day);
	} else if (source_.clock) {
		scene_.configure_mission_clock(source_.header.start_time, source_.header.minutes_per_day);
	}
	stand_clock_();
	// A clock set anew takes its colours at once (the zero-tick refresh a scrub takes).
	nodes().weather->resync_colors_now();
	touch_scene_state();
}

bool PreviewSceneLayers::start_weather() {
	if (!weather_start_ || !environment_built_)
		return false;
	weather_start_ = false;
	// As a mission's start with no simulation: the environment's own home seeded from the loaded
	// environment and the clock, then the start boundary (the initializer, the 255-tick settle: the
	// fog, the rain and the overcast as the mission starts them).
	scene_.prepare_autonomous_weather();
	scene_.run_mission_start_boundary();
	show_time(shown_time_);
	return true;
}

void PreviewSceneLayers::run_weather(int ticks) {
	opennova::env::WeatherRuntime &runtime = nodes().weather->runtime();
	opennova::env::EnvironmentState &state = nodes().environment->state();
	opennova::world::WeatherState &home = runtime.state();
	for (int i = 0; i < ticks; ++i) {
		home.precipitation.fall_tick(
				home.core.scalar_channels.rain_pct_fp, home.precipitation_kind);
		runtime.tick_fixed(&state);
		++ticks_run_;
	}
}

void PreviewSceneLayers::follow_clock(int32_t ticks) {
	if (!environment_built_)
		return;
	if (start_weather() || clock_tick_ < 0) {
		clock_tick_ = ticks;
		return;
	}
	if (ticks > clock_tick_)
		run_weather(std::min<int32_t>(
				ticks - clock_tick_, opennova::env::WeatherRuntime::kMaxCatchupTicks));
	clock_tick_ = ticks;
}

// --- the frame -----------------------------------------------------------------------------------

void PreviewSceneLayers::touch_scene_state() {
	scene_state_ = next_scene_state();
}

void PreviewSceneLayers::publish_scene_state() const {
	// Every global the picture renders with written again, whatever another picture published
	// since: the environment's, the water's, the weather's (its rings with them), the cube's, the
	// veil, the foliage masks.
	scene_.publish_globals();
}

void PreviewSceneLayers::begin_frame() {
	scene_.hold_globals(false);
	nodes().water->set_mirror_enabled(shown_.water);
}

void PreviewSceneLayers::end_frame() {
	scene_.hold_globals(true);
}

bool PreviewSceneLayers::water_drawn() const {
	return nodes().water->is_water_active() && shown_.water;
}

float PreviewSceneLayers::water_height() const {
	return water_drawn() ? nodes().water->get_water_height() : 0.0f;
}

void PreviewSceneLayers::present_environment(double dt) {
	const WorldSceneNodes &n = nodes();
	// The scene environment leg: the world pass's planes, the render eye's side of the water.
	n.environment->apply_scene_pass_planes(*camera_);
	const bool water = water_drawn();
	n.environment->apply_render_eye(
			camera_->get_global_transform().origin.y, water_height(), water);
	// The environment nodes: the weather's publication (its state the ticks left), the sky, the
	// bodies.
	n.weather->advance_frame(0.0);
	n.sky->advance_frame(dt);
	n.celestial->advance_frame(dt);
}

void PreviewSceneLayers::present_ground(double dt, int64_t clock_ms) {
	const WorldSceneNodes &n = nodes();
	// The terrain, then the water over its visible bounds.
	const bool ground = terrain_built_ && terrain_data_.is_valid() && n.terrain->is_visible();
	if (ground) {
		n.terrain->render_frame();
		n.water->set_visible_terrain_bounds(n.terrain->has_visible_terrain_bounds(),
				n.terrain->get_visible_terrain_min_height(),
				n.terrain->get_visible_terrain_max_height());
	}
	n.water->advance_frame(dt);
	// The foliage leg (GameWorld::render_foliage_frame): the terrain's detail cells about the eye
	// grown from the foliage map, the water's height splitting the passes. No crouched or prone
	// person stands in a picture with no simulation, so the silhouette tier has no anchor.
	if (ground && shown_.foliage && foliage_data_.is_valid()) {
		n.foliage->set_water_height(water_height());
		n.foliage->render_frame(camera_->get_global_transform(), clock_ms);
	}
}

void PreviewSceneLayers::present_sun_veil() {
	// The sun veil's exposure feed for the next weather ticks (the veil alpha rides the Celestial's
	// push).
	nodes().weather->set_sun_veil_stopdown(nodes().celestial->get_sun_veil_stopdown());
}

int32_t PreviewSceneLayers::floor_height_(void *ctx, int32_t x, int32_t y) {
	const auto *self = static_cast<const PreviewSceneLayers *>(ctx);
	if (!self->terrain_built_ || self->terrain_data_.is_null())
		return 0;
	const Vector3 at = MissionObjectPlacer::bms_to_godot_position(
			Vector3(float(x) / opennova::io::kFp16One, float(y) / opennova::io::kFp16One, 0.0f));
	const float height = self->terrain_data_->get_height_world_bilinear(at);
	return std::isfinite(height) ? opennova::io::float_to_fp16_16_round_sat(height) : 0;
}

void PreviewSceneLayers::present_particles(int64_t clock_ms) {
	if (ParticleRenderer *renderer = effects_->renderer())
		renderer->set_water_plane(
				water_height(), water_drawn() ? nodes().water->get_reflection_camera() : nullptr);
	effects_->render(clock_ms);
}

void PreviewSceneLayers::present_drops() {
	const WorldSceneNodes &n = nodes();
	// The drops over the home's pool, floored on the terrain and the water; below the rain gate the
	// drawer never runs (retail returns before touching the device, Precipitation's cite).
	opennova::world::WeatherState &home = n.weather->runtime().state();
	if (home.raining()) {
		opennova::env::PrecipitationFloorSampler sampler;
		sampler.terrain_height = &PreviewSceneLayers::floor_height_;
		sampler.ctx = this;
		const int32_t water_z =
				water_drawn() ? opennova::io::float_to_fp16_16_round_sat(water_height()) : 0;
		n.precipitation->render_pool_frame(home.precipitation,
				home.core.scalar_channels.rain_pct_fp, home.precipitation_kind, water_z, sampler,
				camera_, 0, uint32_t(n.weather->get_terrain_light_combined_rgb()));
	} else {
		n.precipitation->hide_frame();
	}
}

void PreviewSceneLayers::present_overlay(EffectLightDirector *lights) {
	ParticleRenderer *renderer = effects_->renderer();
	if (renderer == nullptr)
		return;
	const WorldSceneNodes &n = nodes();
	// GameWorld::render_scene_overlay_frame's tail: the drops, the coronas, the murk, the glint and
	// the glare, the mirror's closing draws (env/celestial_overlay, renderer/scene_overlay.h).
	auto submission = std::make_shared<SceneOverlaySubmission>();
	submission->frame_id = ++overlay_frame_id_;
	n.precipitation->append_overlay(*submission);
	if (lights != nullptr)
		lights->append_overlay(*submission);
	const bool water_renders = n.water->is_water_render_active() && shown_.water;
	if (water_renders)
		append_underwater_murk_overlay(*n.environment, *n.water, *submission);
	if (shown_.sky) {
		append_celestial_overlays(
				overlay_bodies_, *n.celestial, *n.environment, n.water, *camera_, *submission);
		if (water_renders)
			append_water_mirror_overlays(
					overlay_bodies_, n.celestial, n.environment, *n.water, *submission);
	}
	renderer->publish_scene_overlay(submission);
}

void PreviewSceneLayers::present_frame_effects() {
	// The Q3 frame at the camera, the distortion row drawn by the effects' class-7 particles, the
	// screen effects' plan (the veil reads the Celestial's global as it pushed it this frame).
	frame_effects_->present(effects_->renderer());
}

void PreviewSceneLayers::present_clear_and_cube() {
	const WorldSceneNodes &n = nodes();
	// The clear colour: Godot decodes BG_COLOR from sRGB, so the gamma-domain value goes
	// pre-encoded.
	const Vector3 eye = camera_->get_global_transform().origin;
	const bool above = !water_drawn() || eye.y > n.water->get_water_height();
	n.clear_color->get_environment()->set_bg_color(
			n.environment->frame_clear_color_for(above).linear_to_srgb());
	// The environment cube about the eye (no player stands here): captured at first and every 128
	// frames.
	n.environment_cube->advance_frame(eye);
}

// --- the ground ----------------------------------------------------------------------------------

bool PreviewSceneLayers::ground_at(double x, double y, double &height) const {
	if (!terrain_built_ || terrain_data_.is_null())
		return false;
	const Vector3 at =
			MissionObjectPlacer::bms_to_godot_position(Vector3(float(x), float(y), 0.0f));
	const float found = terrain_data_->get_height_world_bilinear(at);
	if (!std::isfinite(found))
		return false;
	height = double(MissionObjectPlacer::godot_to_bms_position(Vector3(at.x, found, at.z)).z);
	return true;
}

bool PreviewSceneLayers::surface_between(
		const double from[3], const double to[3], double point[3]) const {
	if (!terrain_built_ || terrain_data_.is_null())
		return false;
	const Vector3 start = MissionObjectPlacer::bms_to_godot_position(
			Vector3(float(from[0]), float(from[1]), float(from[2])));
	const Vector3 end = MissionObjectPlacer::bms_to_godot_position(
			Vector3(float(to[0]), float(to[1]), float(to[2])));
	const Vector3 hit = terrain_data_->raycast_terrain(start, end);
	if (!hit.is_finite())
		return false;
	const Vector3 mission = MissionObjectPlacer::godot_to_bms_position(hit);
	point[0] = mission.x;
	point[1] = mission.y;
	point[2] = mission.z;
	return true;
}

// --- the read-backs ------------------------------------------------------------------------------

JsonValue PreviewSceneLayers::environment_json() const {
	const WorldSceneNodes &n = nodes();
	const MissionEnvironment *environment = n.environment;
	JsonValue out = JsonValue::make_object();
	out.set("loaded", JsonValue::make_bool(environment->is_loaded()));
	out.set("time_hhmm", json_number(environment->get_time_of_day()));
	out.set("mission_minute_of_day", json_number(environment->get_mission_minute_of_day()));
	out.set("sun_light", json_vector(environment->get_sun_light()));
	out.set("sky_ambient", json_vector(environment->get_sky_ambient()));
	out.set("fog_color", json_vector(environment->get_fog_color()));
	out.set("fog_start", json_number(environment->get_fog_start()));
	out.set("fog_end", json_number(environment->get_fog_end_distance()));
	out.set("fog_type", json_number(environment->get_fog_type()));
	out.set("fog_distance", json_number(environment->get_fog_distance()));
	out.set("water_color", json_vector(environment->get_water_color()));
	JsonValue water = JsonValue::make_object();
	water.set("active", JsonValue::make_bool(n.water->is_water_active()));
	water.set("render_active", JsonValue::make_bool(n.water->is_water_render_active()));
	water.set("height", json_number(n.water->get_water_height()));
	out.set("water", std::move(water));
	JsonValue celestial = JsonValue::make_object();
	celestial.set("visible", JsonValue::make_bool(n.celestial->is_visible_in_tree()));
	out.set("celestial", std::move(celestial));
	JsonValue cube = JsonValue::make_object();
	cube.set("ready", JsonValue::make_bool(n.environment_cube->is_cube_ready()));
	out.set("cube", std::move(cube));
	JsonValue weather = JsonValue::make_object();
	weather.set("ticks", json_number(double(ticks_run_)));
	weather.set("raining", JsonValue::make_bool(n.weather->runtime().state().raining()));
	out.set("weather", std::move(weather));
	return out;
}

JsonValue PreviewSceneLayers::foliage_json() const {
	const FoliageDispatcher *foliage = nodes().foliage;
	// Its definitions' slots that draw, the detail cells about the eye, the instances and vertices,
	// the batches.
	JsonValue out = JsonValue::make_object();
	out.set("shown", JsonValue::make_bool(shown_.foliage));
	int slots = 0;
	const Array diagnostics = foliage->get_slot_diagnostics();
	for (int64_t i = 0; i < diagnostics.size(); ++i)
		slots += String(Dictionary(diagnostics[i]).get("status", "")) == "enabled" ? 1 : 0;
	out.set("slots", json_number(slots));
	const Ref<FoliageFrameStats> stats = foliage->get_frame_stats();
	out.set("cells", json_number(double(stats.is_valid() ? stats->get_detail_cells() : 0)));
	out.set("instances", json_number(double(foliage->get_total_instances())));
	out.set("vertices", json_number(double(stats.is_valid() ? stats->get_detail_vertices() : 0)));
	out.set("batches", json_number(double(stats.is_valid() ? stats->get_render_batches() : 0)));
	return out;
}

} // namespace godot
