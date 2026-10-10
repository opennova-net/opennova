#include "authoring/terrain_viewport_applier.h"

#include "authoring/mission_placed_tiles.h"

#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <algorithm>
#include <cmath>

#include <editor/assets/project_asset_source.h>
#include <editor/preview/mission_scene.h>
#include <editor/preview/mission_viewport.h>
#include <editor/preview/preview_clock.h>
#include <editor/preview/terrain_viewport.h>
#include <editor/preview/viewport_device.h>
#include <editor/session/view/session_view.h>
#include <formats/env/env.h>
#include <formats/env/env_weather.h>
#include <runtime/environment/weather_runtime.h>
#include <runtime/renderer/render_order.h>

#include "env/mission_environment_overrides.h"
#include "mission/mission_object_placer.h"
#include "render/frame_fx.h"
#include "terrain/foliage_frame_stats.h"
#include "terrain/terrain_tile_info.h"
#include "util/string_convert.h"

namespace godot {

namespace {

using opennova::editor::TerrainViewport;

Vector3 to_godot(const opennova::editor::PreviewVec3 &v) {
	return Vector3(v.x, v.y, v.z);
}

const TerrainViewport &terrain_of(const opennova::editor::ViewportModel &model) {
	return static_cast<const TerrainViewport &>(model);
}

// Each device's scene state is its own (the mission device's rule): a counter no two share, never 0.
uint64_t next_scene_state() {
	static uint64_t next = 1u << 24;
	return ++next;
}

} // namespace

bool TerrainViewportApplier::EnvironmentKey::operator==(const EnvironmentKey &o) const {
	return environment == o.environment && terrain == o.terrain && attrib_flags == o.attrib_flags &&
			water_override == o.water_override &&
			fog_override == o.fog_override && water_murk == o.water_murk && start_time == o.start_time &&
			minutes_per_day == o.minutes_per_day &&
			std::equal(std::begin(fog_color), std::end(fog_color), std::begin(o.fog_color)) &&
			std::equal(std::begin(water_color), std::end(water_color), std::begin(o.water_color));
}

TerrainViewportApplier::TerrainKey TerrainViewportApplier::terrain_key_of_(const TerrainViewport &model) const {
	TerrainKey key;
	key.terrain = model.header().terrain;
	key.tile_set = model.header().tile_set;
	key.mission = model.mission_name();
	// The mission's .env, which its terrain's load reads after the .trn (D-TERRAIN-18); none with no mission.
	key.environment = model.header().environment.empty() ? std::string() : model.header().environment + ".env";
	if (!key.terrain.empty() && stamped_)
		key.later = opennova::editor::mission_terrain_later_lines(*stamped_, key.environment);
	return key;
}

TerrainViewportApplier::EnvironmentKey TerrainViewportApplier::environment_key_of_(const TerrainViewport &model) {
	const opennova::editor::MissionSceneHeader &header = model.header();
	EnvironmentKey key;
	key.environment = header.environment;
	key.terrain = header.terrain;
	key.attrib_flags = header.attrib_flags;
	key.water_override = header.water_override;
	key.fog_override = header.fog_override;
	key.water_murk = header.water_murk;
	key.start_time = header.start_time;
	key.minutes_per_day = header.minutes_per_day;
	for (int i = 0; i < 3; ++i) {
		key.fog_color[i] = header.fog_color[i];
		key.water_color[i] = header.water_color[i];
	}
	return key;
}

int TerrainViewportApplier::layer_of_(Unit::Kind kind) {
	switch (kind) {
	case Unit::Kind::Environment:
	case Unit::Kind::Sky: return kEnvironment;
	case Unit::Kind::TerrainFile:
	case Unit::Kind::TerrainBuild:
	case Unit::Kind::NoTerrain:
	case Unit::Kind::Water: return kTerrain;
	case Unit::Kind::Pose: break;
	}
	return kLayers;
}

TerrainViewportApplier::TerrainViewportApplier(SubViewport &viewport) : scene_state_(next_scene_state()) {
	// Single-sampled, as the game's own view draws (the mission device's).
	viewport.set_msaa_3d(Viewport::MSAA_DISABLED);
	root_ = memnew(Node3D);
	root_->set_name("TerrainPreview");
	viewport.add_child(root_);
	// The clear colour is the environment's frame clear (the game's ClearColor node); the terminal display decode
	// installs on it.
	root_->add_child(memnew(DisplayDecode));
	clear_ = memnew(WorldEnvironment);
	clear_->set_name("ClearColor");
	Ref<Environment> environment;
	environment.instantiate();
	environment->set_background(Environment::BG_COLOR);
	environment->set_ambient_source(Environment::AMBIENT_SOURCE_DISABLED);
	clear_->set_environment(environment);
	root_->add_child(clear_);
	// The environment and the water hold their process-wide globals (E13): only this device's publication and its
	// presented frames write them.
	environment_ = memnew(MissionEnvironment);
	environment_->set_name("Environment");
	environment_->set_globals_held(true);
	root_->add_child(environment_);
	sky_ = memnew(SkyDome);
	sky_->set_name("Sky");
	sky_->set_environment_path(NodePath("../Environment"));
	root_->add_child(sky_);
	water_ = memnew(Water);
	water_->set_name("Water");
	water_->set_environment_path(NodePath("../Environment"));
	water_->set_globals_held(true);
	root_->add_child(water_);
	// The weather whose oscillator the foliage's sway reads: no environment bound, its wave alone run (tick).
	weather_ = memnew(Weather);
	weather_->set_name("Weather");
	root_->add_child(weather_);
	terrain_ = memnew(Terrain);
	terrain_->set_name("Terrain");
	terrain_->set_environment_path(NodePath("../Environment"));
	terrain_->set_water_path(NodePath("../Water"));
	root_->add_child(terrain_);
	// The foliage beside the terrain, never under it (GameWorld's).
	foliage_ = memnew(FoliageDispatcher);
	foliage_->set_name("Foliage");
	root_->add_child(foliage_);
	foliage_->set_terrain(terrain_);
	foliage_->set_weather(weather_);
	camera_ = memnew(Camera3D);
	camera_->set_name("Camera");
	camera_->set_keep_aspect_mode(Camera3D::KEEP_WIDTH);
	camera_->set_fov(opennova::editor::OrbitCamera::fov_horizontal_degrees());
	camera_->set_current(true);
	root_->add_child(camera_);
	root_files_.instantiate();
}

TerrainViewportApplier::~TerrainViewportApplier() = default;

void TerrainViewportApplier::touch_scene_state_() {
	scene_state_ = next_scene_state();
}

// --- the files -----------------------------------------------------------------------------------

void TerrainViewportApplier::mount_(const opennova::editor::SessionView &view) {
	const std::shared_ptr<const opennova::editor::ProjectAssetSource> source = view.findings.assets;
	const bool another = source != mounted_ || !stamped_;
	bool stale[kLayers] = { another, another };
	if (!another) {
		if (!stamped_->stamps().moved(*source)) return;
		// What moved: the layers that read it (an edit of the terrain's document moves its .trn).
		stale[kEnvironment] = layer_files_[kEnvironment].moved(*source);
		// The .env and overcast.def the terrain's load read are the terrain's by the lines of them its parser
		// takes, which its key holds (D-TERRAIN-18): their other edits leave the ground standing.
		stale[kTerrain] = layer_files_[kTerrain].moved_but(*source, { terrain_key_.environment, opennova::env::kOvercastFile }) ||
				!stale[kEnvironment];
	}
	mounted_ = source;
	stamped_ = std::make_shared<opennova::StampedFiles>(source);
	root_files_->mount_files(stamped_);
	for (int layer = 0; layer < kLayers; ++layer) {
		if (stale[layer]) {
			layer_files_[layer].clear();
			layer_missing_[layer].clear();
			continue;
		}
		for (const opennova::FileStamp &file : layer_files_[layer].files()) stamped_->stamp(file.name);
	}
	if (stale[kEnvironment]) environment_built_ = false;
	if (stale[kTerrain]) terrain_built_ = false;
}

size_t TerrainViewportApplier::reads_() const {
	return stamped_ ? stamped_->stamps().files().size() : 0;
}

void TerrainViewportApplier::note_reads_(int layer, size_t from) {
	if (!stamped_ || layer >= kLayers) return;
	const std::vector<opennova::FileStamp> &files = stamped_->stamps().files();
	for (size_t i = from; i < files.size(); ++i) layer_files_[layer].note(files[i].name, files[i].stamp);
}

void TerrainViewportApplier::note_missing_(int layer, const String &name) {
	const std::string text = opennova::to_std(name);
	std::vector<std::string> &missing = layer_missing_[layer];
	if (std::find(missing.begin(), missing.end(), text) == missing.end()) missing.push_back(text);
}

// --- the build -----------------------------------------------------------------------------------

void TerrainViewportApplier::plan_(Build &build, const TerrainViewport &model) {
	build.environment = !environment_built_ || !(environment_key_of_(model) == environment_key_);
	const TerrainKey terrain_key = terrain_key_of_(model);
	build.terrain = !terrain_built_ || !(terrain_key == terrain_key_);
	build.terrain_key = terrain_key;
	if (build.environment) {
		layer_missing_[kEnvironment].clear();
		build.units.push_back(Unit{ Unit::Kind::Environment });
	}
	if (build.terrain) {
		// The .trn parsed as the load begins (its units planned then); a terrain that does not load is a note.
		layer_missing_[kTerrain].clear();
		loading_.unref();
		const String trn = opennova::to_gd(terrain_key.terrain) + ".trn";
		const size_t from = reads_();
		bool begun = false;
		if (!terrain_key.terrain.empty() && root_files_->has_file(trn)) {
			loading_.instantiate();
			loading_->set_mission_tile_set(opennova::to_gd(terrain_key.tile_set));
			loading_->set_mission_environment(opennova::to_gd(terrain_key.environment));
			begun = loading_->begin_load_from_resource_root(root_files_, trn) == OK;
		}
		note_reads_(kTerrain, from);
		if (begun) {
			for (int i = 0; i < loading_->get_load_step_count(); ++i) build.units.push_back(Unit{ Unit::Kind::TerrainFile });
			build.units.push_back(Unit{ Unit::Kind::TerrainBuild });
		} else {
			if (!terrain_key.terrain.empty()) note_missing_(kTerrain, trn);
			loading_.unref();
			build.units.push_back(Unit{ Unit::Kind::NoTerrain });
		}
	}
	if (build.environment || build.terrain) {
		build.units.push_back(Unit{ Unit::Kind::Sky });
		build.units.push_back(Unit{ Unit::Kind::Water });
	}
	build.units.push_back(Unit{ Unit::Kind::Pose });
}

void TerrainViewportApplier::rebuild(const opennova::editor::ViewportModel &viewport, const opennova::editor::SessionView &view,
		const opennova::editor::PreviewClock &) {
	build_.reset();
	const TerrainViewport &model = terrain_of(viewport);
	if (model.view_status() != opennova::editor::TerrainViewStatus::Ready || !view.findings.assets) {
		clear();
		return;
	}
	mount_(view);
	auto build = std::make_unique<Build>();
	plan_(*build, model);
	// The pose alone: made whole as it is taken.
	if (build->units.size() == 1) {
		run_(*build, build->units[0], model);
		return;
	}
	build_ = std::move(build);
}

void TerrainViewportApplier::run_(Build &build, const Unit &unit, const TerrainViewport &model) {
	const size_t from = reads_();
	switch (unit.kind) {
	case Unit::Kind::Environment: run_environment_(model); break;
	case Unit::Kind::TerrainFile: run_terrain_file_(build); break;
	case Unit::Kind::TerrainBuild: run_terrain_build_(build); break;
	case Unit::Kind::NoTerrain: terrain_empty_(build.terrain_key); break;
	case Unit::Kind::Sky: sky_->build(); break;
	case Unit::Kind::Water:
		water_->build();
		touch_scene_state_();
		break;
	case Unit::Kind::Pose: apply_state_(model); break;
	}
	note_reads_(layer_of_(unit.kind), from);
}

ApplierStep TerrainViewportApplier::step(const opennova::editor::ViewportModel &viewport, const opennova::editor::PreviewClock &,
		std::string &) {
	Build &build = *build_;
	const Unit unit = build.units[build.next++];
	run_(build, unit, terrain_of(viewport));
	if (build.next < build.units.size()) return ApplierStep::More;
	build_.reset();
	return ApplierStep::Built;
}

void TerrainViewportApplier::run_environment_(const TerrainViewport &model) {
	const opennova::editor::MissionSceneHeader &header = model.header();
	const String name = header.environment.empty() ? String() : opennova::to_gd(header.environment) + ".env";
	const String trn = header.terrain.empty() ? String() : opennova::to_gd(header.terrain) + ".trn";
	// As the game's load reads it (GameWorld::load_environment): the terrain's .trn (the document's file, its
	// water_rgb and water_murk the mission's until a .env line of them), overcast.def (the overcast table, by the
	// runtime's own name), then the .env over them; with no .env (no mission, or one the project lacks: a note) the
	// earlier passes over the engine's own environment, as a mission with no .env starts on it (env-tod-re.md #38,
	// #43).
	Ref<EnvFile> env;
	env.instantiate();
	Ref<EnvFile> overcast;
	overcast.instantiate();
	if (!env->load_mission_environment(root_files_, trn, name, overcast) && !header.environment.empty()) {
		note_missing_(kEnvironment, name);
	}
	// The header's overrides over the file (the game's apply_mission_environment_overrides).
	Ref<MissionEnvironmentOverrides> overrides;
	overrides.instantiate();
	overrides->assign(opennova::env::bms_env_overrides_from_header(header.attrib_flags, header.water_override,
			header.fog_override, header.fog_color, header.water_color, header.water_murk));
	env->apply_mission_overrides_or_clear(overrides);
	// The fog as the mission's start settles it, which no weather tick here does (the mission view's helper).
	env->set_fog_level(opennova::env::EnvScalarChannels::settled_fog_level(env->get_fog_level()));
	environment_->set_environment_data(env);
	water_->set_mission_water_height_override(overrides->get_water_height_world_or_nan());
	environment_->set_overcast_data(overcast);
	// The texts the load read are the environment's whatever unit read one first (the .trn its terrain's units
	// stamped already): an edit of one builds the environment again.
	for (const String &text : { trn, name, opennova::to_gd(opennova::env::kOvercastFile) }) {
		if (!text.is_empty()) layer_files_[kEnvironment].note(opennova::to_std(text), stamped_->stamp(opennova::to_std(text)));
	}
	// The clock the mission's start sets (the header the model reads, cited in preview/terrain_viewport); with no
	// mission the engine's own (Environment_InitDefaults' curtime).
	if (model.mission()) environment_->configure_mission_clock(header.start_time, header.minutes_per_day);
	environment_key_ = environment_key_of_(model);
	environment_built_ = true;
	++environment_builds_;
	touch_scene_state_();
}

void TerrainViewportApplier::terrain_empty_(const TerrainKey &key) {
	loading_.unref();
	foliage_->reset();
	foliage_->set_terrain_data(Ref<TerrainData>());
	foliage_->set_tile_info(Ref<TerrainTileInfo>());
	foliage_data_.unref();
	terrain_->set_terrain_data(Ref<TerrainData>());
	terrain_->clear_built();
	water_->set_terrain_data(Ref<TerrainData>());
	terrain_data_.unref();
	terrain_built_ = true;
	terrain_key_ = key;
	touch_scene_state_();
}

void TerrainViewportApplier::run_terrain_file_(Build &build) {
	if (loading_.is_null()) return;
	const TerrainData::LoadStep result = loading_->load_step();
	if (result == TerrainData::LOAD_STEP_MORE) return;
	for (const std::string &name : loading_->get_load_missing()) note_missing_(kTerrain, opennova::to_gd(name));
	const auto give_up = [&](const String &why) {
		if (!why.is_empty()) note_missing_(kTerrain, why);
		build.units.erase(std::remove_if(build.units.begin() + std::ptrdiff_t(build.next), build.units.end(),
								  [](const Unit &u) {
									  return u.kind == Unit::Kind::TerrainFile || u.kind == Unit::Kind::TerrainBuild;
								  }),
				build.units.end());
		terrain_empty_(build.terrain_key);
	};
	if (result == TerrainData::LOAD_STEP_FAILED) {
		give_up(loading_->get_load_failure());
		return;
	}
	terrain_data_ = loading_;
	loading_.unref();
	// The tiles the mission places, read as the game's load reads them before the build (read_mission_placed_tiles:
	// <mission>.til first, else the terrain's own polytrn_tileinfo, each where the game's loader takes it; the order
	// MissionGround::tiles_file reads).
	terrain_->set_tile_info_override(read_mission_placed_tiles(root_files_, build.terrain_key.mission,
			build.terrain_key.terrain, build.terrain_key.environment, &tiles_own_));
	terrain_->set_terrain_data(terrain_data_);
	water_->set_terrain_data(terrain_data_);
	terrain_built_ = false;
	touch_scene_state_();
	if (!terrain_->build_begin()) {
		give_up(String());
		return;
	}
	const int steps = terrain_->get_build_step_count();
	for (int i = 1; i < steps; ++i)
		build.units.insert(build.units.begin() + std::ptrdiff_t(build.next), Unit{ Unit::Kind::TerrainBuild });
}

void TerrainViewportApplier::run_terrain_build_(Build &build) {
	const Terrain::BuildStep result = terrain_->build_step();
	if (result == Terrain::BUILD_STEP_FAILED) {
		build.units.erase(std::remove_if(build.units.begin() + std::ptrdiff_t(build.next), build.units.end(),
								  [](const Unit &u) { return u.kind == Unit::Kind::TerrainBuild; }),
				build.units.end());
		terrain_empty_(build.terrain_key);
		return;
	}
	if (result == Terrain::BUILD_STEP_DONE) {
		terrain_built_ = true;
		terrain_key_ = build.terrain_key;
		++terrain_builds_;
		// Its foliage as the game's load configures it after the build (the reads its slots make are the terrain
		// layer's: a foliage model or texture moved builds the terrain again).
		configure_foliage_();
	}
}

void TerrainViewportApplier::configure_foliage_() {
	foliage_data_ = terrain_data_;
	foliage_->reset();
	if (terrain_data_.is_null()) {
		foliage_->set_terrain_data(Ref<TerrainData>());
		return;
	}
	// GameWorld::configure_foliage: the terrain data, the placed tiles as the candidate blocker (the tiles the load
	// took: the mission's, else the terrain's own), each definition's model and :fd texture through the root.
	foliage_->configure_for_terrain(root_files_, terrain_data_, terrain_->get_tile_info_override());
	const Array diagnostics = foliage_->get_slot_diagnostics();
	for (int64_t i = 0; i < diagnostics.size(); ++i) {
		const Dictionary diagnostic = diagnostics[i];
		const String status = diagnostic.get("status", "");
		const String graphic = diagnostic.get("graphic", "");
		if ((status == "missing_mesh" || status == "invalid_mesh") && !graphic.is_empty())
			note_missing_(kTerrain, graphic.get_extension().is_empty() ? graphic + String(".3di") : graphic);
	}
}

opennova::editor::OperationProgress TerrainViewportApplier::progress() const {
	opennova::editor::OperationProgress progress;
	progress.unit = opennova::editor::OperationUnit::Steps;
	if (!build_) return progress;
	progress.done = build_->next;
	progress.total = build_->units.size();
	if (build_->next < build_->units.size()) {
		switch (build_->units[build_->next].kind) {
		case Unit::Kind::Environment: progress.label = "environment"; break;
		case Unit::Kind::TerrainFile: progress.label = "terrain files"; break;
		case Unit::Kind::TerrainBuild:
		case Unit::Kind::NoTerrain: progress.label = "terrain"; break;
		case Unit::Kind::Sky: progress.label = "sky"; break;
		case Unit::Kind::Water: progress.label = "water"; break;
		case Unit::Kind::Pose: progress.label = "pose"; break;
		}
	}
	return progress;
}

// --- the state -----------------------------------------------------------------------------------

void TerrainViewportApplier::update(const opennova::editor::ViewportModel &viewport, const opennova::editor::PreviewClock &) {
	apply_state_(terrain_of(viewport));
}

void TerrainViewportApplier::apply_state_(const TerrainViewport &model) {
	const opennova::editor::TerrainViewportOptions &options = model.options();
	if (options.water != shown_water_) touch_scene_state_();
	water_->set_visible(options.water);
	shown_water_ = options.water;
	foliage_->set_visible(options.foliage);
	shown_foliage_ = options.foliage;
	// The ground overlay (DI-29): the picture the viewport made of what the game reads at each point, laid over the
	// terrain from above (presentation x east, z the negated mission y), past it the one value the game reads all
	// round where there is one (the mission device's leg).
	if (model.overlay_serial() != overlay_serial_) {
		overlay_serial_ = model.overlay_serial();
		const opennova::editor::MissionOverlayImage &overlay = model.overlay();
		if (overlay.kind == opennova::editor::MissionGroundOverlay::None) {
			terrain_->clear_ground_overlay();
		} else {
			Ref<Image> picture;
			if (!overlay.empty()) {
				PackedByteArray bytes;
				bytes.resize(int64_t(overlay.rgba.size()));
				std::copy(overlay.rgba.begin(), overlay.rgba.end(), bytes.ptrw());
				picture = Image::create_from_data(overlay.width, overlay.height, false, Image::FORMAT_RGBA8, bytes);
			}
			const Rect2 rect(float(overlay.west), float(-overlay.north), float(overlay.width * overlay.texel),
					float(overlay.height * overlay.texel));
			const uint8_t *outside = overlay.outside_rgba;
			terrain_->set_ground_overlay(picture, rect,
					Color(outside[0] / 255.0f, outside[1] / 255.0f, outside[2] / 255.0f, outside[3] / 255.0f),
					overlay.outside);
		}
	}
	place_camera_(model);
}

void TerrainViewportApplier::place_camera_(const TerrainViewport &model) {
	const opennova::editor::OrbitCamera &camera = model.camera();
	opennova::editor::PreviewVec3 right, up, back;
	camera.axes(right, up, back);
	camera_->set_transform(Transform3D(Basis(to_godot(right), to_godot(up), to_godot(back)), to_godot(camera.eye())));
	// The world pass's planes: the game's near, its far from the environment's fog.
	environment_->apply_scene_pass_planes(*camera_);
}

void TerrainViewportApplier::clear() {
	build_.reset();
	terrain_empty_(TerrainKey());
	terrain_built_ = false;
	environment_->set_environment_data(Ref<EnvFile>());
	environment_built_ = false;
	overlay_serial_ = UINT64_MAX;
	terrain_->clear_ground_overlay();
	for (std::vector<std::string> &missing : layer_missing_) missing.clear();
	touch_scene_state_();
}

void TerrainViewportApplier::apply(const opennova::editor::ViewportModel &viewport, const opennova::editor::PreviewClock &,
		opennova::editor::ViewportDeviceReport &report) {
	report.missing.clear();
	for (const std::vector<std::string> &missing : layer_missing_)
		report.missing.insert(report.missing.end(), missing.begin(), missing.end());
	report.surface = terrain_built_ && terrain_data_.is_valid();
	if (stamped_) report.files = stamped_->stamps();
	report.drawn = drawn_json_();
	if (build_) return;
	apply_state_(terrain_of(viewport));
}

opennova::io::JsonValue TerrainViewportApplier::drawn_json_() const {
	using opennova::io::json_number;
	using opennova::io::JsonValue;
	JsonValue out = JsonValue::make_object();
	out.set("frames", json_number(double(presented_)));
	// The foliage the last presented frame drew (the mission device's words): its definitions' slots that draw, the
	// detail cells about the eye, the instances and vertices.
	JsonValue foliage = JsonValue::make_object();
	foliage.set("shown", JsonValue::make_bool(shown_foliage_));
	int slots = 0;
	const Array diagnostics = foliage_->get_slot_diagnostics();
	for (int64_t i = 0; i < diagnostics.size(); ++i)
		slots += String(Dictionary(diagnostics[i]).get("status", "")) == "enabled" ? 1 : 0;
	foliage.set("slots", json_number(slots));
	const Ref<FoliageFrameStats> stats = foliage_->get_frame_stats();
	foliage.set("cells", json_number(double(stats.is_valid() ? stats->get_detail_cells() : 0)));
	foliage.set("instances", json_number(double(foliage_->get_total_instances())));
	foliage.set("vertices", json_number(double(stats.is_valid() ? stats->get_detail_vertices() : 0)));
	out.set("foliage", std::move(foliage));
	// The placed tiles the terrain draws: the tiles the load took (the mission's .til, else the terrain's own), and
	// with none the terrain's own its data reads.
	Ref<TerrainTileInfo> tiles = terrain_->get_tile_info_override();
	bool own = tiles_own_;
	if (tiles.is_null() && terrain_data_.is_valid()) {
		tiles = terrain_data_->get_tileinfo_resource();
		own = true;
	}
	JsonValue placed = JsonValue::make_object();
	placed.set("count", json_number(double(tiles.is_valid() ? tiles->get_entry_count() : 0)));
	placed.set("from", opennova::io::json_string(tiles.is_null() ? "none" : own ? "terrain" : "mission"));
	out.set("tiles", std::move(placed));
	return out;
}

void TerrainViewportApplier::tick(const opennova::editor::ViewportModel &, const opennova::editor::PreviewClock &clock) {
	clock_ms_ = int64_t(clock.ms());
	// No mirror pass unless this frame presents the picture.
	water_->set_mirror_enabled(false);
	// The weather's wave on the preview clock (the foliage's sway): the oscillator alone (env::WeatherOscillator,
	// the weather tick's legs), from the mission start's settle, then a tick per game tick.
	opennova::env::WeatherOscillator &wave = weather_->runtime().core().oscillator;
	if (wave_tick_ < 0) {
		for (int i = 0; i < opennova::env::WeatherRuntime::kMissionStartPrewarmTicks; ++i) wave.tick();
		wave_tick_ = clock.ticks();
	} else if (clock.ticks() > wave_tick_) {
		const int32_t run = std::min<int32_t>(clock.ticks() - wave_tick_, opennova::env::WeatherRuntime::kMaxCatchupTicks);
		for (int32_t i = 0; i < run; ++i) wave.tick();
		wave_tick_ = clock.ticks();
	} else {
		wave_tick_ = clock.ticks();
	}
}

// --- the frame -----------------------------------------------------------------------------------

void TerrainViewportApplier::publish_scene_state() {
	environment_->republish_shader_globals();
	water_->set_globals_held(false);
	water_->set_world_rendering_enabled(true);
	water_->set_globals_held(true);
}

void TerrainViewportApplier::present(double dt) {
	// Nothing to draw while the first build runs.
	if (build_ && !environment_built_) return;
	environment_->set_globals_held(false);
	water_->set_globals_held(false);
	water_->set_mirror_enabled(shown_water_);
	// In the game's leg order (game_world_frame.cpp): the render eye and the clear, the sky, the terrain, the water,
	// the foliage.
	const float eye_y = camera_->get_global_transform().origin.y;
	const bool water_active = water_->is_water_active() && shown_water_;
	environment_->apply_render_eye(eye_y, water_active ? water_->get_water_height() : 0.0f, water_active);
	const bool above = !water_active || eye_y > water_->get_water_height();
	// Godot decodes BG_COLOR from sRGB: the gamma-domain value goes pre-encoded (the game's clear colour leg).
	clear_->get_environment()->set_bg_color(environment_->frame_clear_color_for(above).linear_to_srgb());
	sky_->advance_frame(dt);
	if (terrain_built_ && terrain_data_.is_valid()) {
		terrain_->render_frame();
		water_->set_visible_terrain_bounds(terrain_->has_visible_terrain_bounds(), terrain_->get_visible_terrain_min_height(),
				terrain_->get_visible_terrain_max_height());
	}
	water_->advance_frame(dt);
	// The foliage leg (GameWorld::render_foliage_frame): the terrain's detail cells about the eye grown from the
	// foliage map, the water's height splitting the passes.
	if (shown_foliage_ && terrain_built_ && foliage_data_.is_valid()) {
		foliage_->set_water_height(water_active ? water_->get_water_height() : 0.0f);
		foliage_->render_frame(camera_->get_global_transform(), clock_ms_);
	}
	++presented_;
	environment_->set_globals_held(true);
	water_->set_globals_held(true);
}

// --- the ground ----------------------------------------------------------------------------------

bool TerrainViewportApplier::ground_at(double x, double y, double &height) const {
	if (!terrain_built_ || terrain_data_.is_null()) return false;
	const Vector3 at = MissionObjectPlacer::bms_to_godot_position(Vector3(float(x), float(y), 0.0f));
	const float found = terrain_data_->get_height_world_bilinear(at);
	if (!std::isfinite(found)) return false;
	height = double(MissionObjectPlacer::godot_to_bms_position(Vector3(at.x, found, at.z)).z);
	return true;
}

bool TerrainViewportApplier::surface_between(const double from[3], const double to[3], double point[3]) const {
	if (!terrain_built_ || terrain_data_.is_null()) return false;
	const Vector3 start = MissionObjectPlacer::bms_to_godot_position(Vector3(float(from[0]), float(from[1]), float(from[2])));
	const Vector3 end = MissionObjectPlacer::bms_to_godot_position(Vector3(float(to[0]), float(to[1]), float(to[2])));
	const Vector3 hit = terrain_data_->raycast_terrain(start, end);
	if (!hit.is_finite()) return false;
	const Vector3 mission = MissionObjectPlacer::godot_to_bms_position(hit);
	point[0] = mission.x;
	point[1] = mission.y;
	point[2] = mission.z;
	return true;
}

} // namespace godot
