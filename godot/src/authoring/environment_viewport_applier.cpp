#include "authoring/environment_viewport_applier.h"

#include "authoring/mission_placed_tiles.h"

#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <algorithm>
#include <cmath>

#include <base/io/fixed.h>
#include <editor/assets/project_asset_source.h>
#include <editor/preview/environment_viewport.h>
#include <editor/preview/mission_camera.h>
#include <editor/preview/mission_scene.h>
#include <editor/preview/preview_clock.h>
#include <editor/preview/viewport_device.h>
#include <editor/session/view/session_view.h>
#include <formats/env/env.h>
#include <runtime/environment/environment_state.h>
#include <runtime/environment/weather_runtime.h>
#include <runtime/renderer/render_order.h>
#include <runtime/renderer/scene_overlay.h>

#include "authoring/preview_effects.h"
#include "env/celestial_overlay.h"
#include "env/mission_environment_overrides.h"
#include "mission/mission_object_placer.h"
#include "particle/particle_renderer.h"
#include "authoring/preview_frame_effects.h"
#include "render/frame_fx.h"
#include "terrain/terrain_tile_info.h"
#include "util/string_convert.h"

namespace godot {

namespace {

using opennova::editor::EnvironmentViewport;

Vector3 to_godot(const opennova::editor::PreviewVec3 &v) {
	return Vector3(v.x, v.y, v.z);
}

const EnvironmentViewport &environment_of(const opennova::editor::ViewportModel &model) {
	return static_cast<const EnvironmentViewport &>(model);
}

// Each device's scene state is its own (the mission device's rule): a counter no two share, never 0.
uint64_t next_scene_state() {
	static uint64_t next = 1u << 20;
	return ++next;
}

} // namespace

bool EnvironmentViewportApplier::EnvironmentKey::operator==(const EnvironmentKey &o) const {
	return file == o.file && terrain == o.terrain && attrib_flags == o.attrib_flags && water_override == o.water_override &&
			fog_override == o.fog_override && water_murk == o.water_murk &&
			std::equal(std::begin(fog_color), std::end(fog_color), std::begin(o.fog_color)) &&
			std::equal(std::begin(water_color), std::end(water_color), std::begin(o.water_color));
}

EnvironmentViewportApplier::TerrainKey EnvironmentViewportApplier::terrain_key_of_(const EnvironmentViewport &model) const {
	TerrainKey key;
	if (!model.options().terrain) return key;
	key.terrain = model.header().terrain;
	key.tile_set = model.header().tile_set;
	key.mission = model.mission_name();
	// The file this viewport draws is the .env the drawn mission's terrain reads after its .trn (D-TERRAIN-18).
	key.environment = model.file_name();
	if (!key.terrain.empty() && stamped_)
		key.later = opennova::editor::mission_terrain_later_lines(*stamped_, key.environment);
	return key;
}

EnvironmentViewportApplier::EnvironmentKey EnvironmentViewportApplier::environment_key_of_(const EnvironmentViewport &model) {
	const opennova::editor::MissionSceneHeader &header = model.header();
	EnvironmentKey key;
	key.file = model.file_name();
	key.terrain = header.terrain;
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

int EnvironmentViewportApplier::layer_of_(Unit::Kind kind) {
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

EnvironmentViewportApplier::EnvironmentViewportApplier(SubViewport &viewport) : scene_state_(next_scene_state()) {
	// Single-sampled, as the game's own view draws: the particle renderer's overlay passes bind the view's depth
	// (a multisampled view's resolved depth is no attachment they can bind: DI-14's rule).
	viewport.set_msaa_3d(Viewport::MSAA_DISABLED);
	// Its camera is the 3D listener of its own world (S23 C: the Listen's rain loops pan about it, as the game's camera).
	viewport.set_as_audio_listener_3d(true);
	root_ = memnew(Node3D);
	root_->set_name("EnvironmentPreview");
	viewport.add_child(root_);
	listen_ = std::make_unique<PreviewSoundLoops>(root_);
	// The clear colour is the environment's frame clear (the game's ClearColor node, its clear colour leg);
	// the terminal display decode installs on it (the one decode a 3D view of retail's gamma-domain shaders
	// takes), and the particle renderer's overlay passes inherit it, placing theirs before the decode.
	clear_ = memnew(WorldEnvironment);
	clear_->set_name("ClearColor");
	Ref<Environment> environment;
	environment.instantiate();
	environment->set_background(Environment::BG_COLOR);
	environment->set_ambient_source(Environment::AMBIENT_SOURCE_DISABLED);
	clear_->set_environment(environment);
	root_->add_child(clear_);
	// The environment and the water hold their process-wide globals (E13): only this device's publication
	// and its presented frames write them.
	environment_ = memnew(MissionEnvironment);
	environment_->set_name("Environment");
	environment_->set_globals_held(true);
	root_->add_child(environment_);
	weather_ = memnew(Weather);
	weather_->set_name("Weather");
	weather_->set_environment_path(NodePath("../Environment"));
	root_->add_child(weather_);
	sky_ = memnew(SkyDome);
	sky_->set_name("Sky");
	sky_->set_environment_path(NodePath("../Environment"));
	sky_->set_weather_path(NodePath("../Weather"));
	root_->add_child(sky_);
	celestial_ = memnew(Celestial);
	celestial_->set_name("Celestial");
	celestial_->set_environment_path(NodePath("../Environment"));
	root_->add_child(celestial_);
	water_ = memnew(Water);
	water_->set_name("Water");
	water_->set_environment_path(NodePath("../Environment"));
	water_->set_globals_held(true);
	root_->add_child(water_);
	terrain_ = memnew(Terrain);
	terrain_->set_name("Terrain");
	terrain_->set_environment_path(NodePath("../Environment"));
	terrain_->set_weather_path(NodePath("../Weather"));
	terrain_->set_water_path(NodePath("../Water"));
	root_->add_child(terrain_);
	// The terrain's foliage beside it, never under it (GameWorld's): the detail cells come off the terrain's
	// frame, the sway off the weather's oscillator.
	foliage_ = memnew(FoliageDispatcher);
	foliage_->set_name("Foliage");
	root_->add_child(foliage_);
	foliage_->set_terrain(terrain_);
	foliage_->set_weather(weather_);
	precipitation_ = memnew(Precipitation);
	precipitation_->set_name("Precipitation");
	precipitation_->set_weather_path(NodePath("../Weather"));
	root_->add_child(precipitation_);
	camera_ = memnew(Camera3D);
	camera_->set_name("Camera");
	camera_->set_keep_aspect_mode(Camera3D::KEEP_WIDTH);
	camera_->set_fov(opennova::editor::OrbitCamera::fov_horizontal_degrees());
	camera_->set_current(true);
	root_->add_child(camera_);
	// The overlay tail's passes (the drops, the murk, the glint and the glare; the mirror's closing draws):
	// the game's particle renderer's, composed on this device's cameras, no effect of its own drawn.
	effects_ = std::make_unique<PreviewEffects>(*root_);
	effects_->set_environment_source(environment_);
	// The game's frame effects (S23 C): FrameFx as the terminal compositor (the bloom of the sun and the glare, and the
	// display decode), the sun-glare veil over the picture.
	frame_effects_ = std::make_unique<PreviewFrameEffects>(*root_, viewport);
	root_files_.instantiate();
}

EnvironmentViewportApplier::~EnvironmentViewportApplier() = default;

void EnvironmentViewportApplier::touch_scene_state_() {
	scene_state_ = next_scene_state();
}

// --- the files -----------------------------------------------------------------------------------

bool EnvironmentViewportApplier::mount_(const opennova::editor::SessionView &view, const std::string &environment,
		const std::string &terrain) {
	const std::shared_ptr<const opennova::editor::ProjectAssetSource> source = view.findings.assets;
	const bool another = source != mounted_ || !stamped_;
	bool stale[kLayers] = { another, another };
	if (!another) {
		if (!stamped_->stamps().moved(*source)) return false;
		// What moved: the layers that read it; a moved file neither layer's units read (a model the sun
		// read as it was first drawn, the drops' texture) is the environment's.
		// The .env and overcast.def the terrain's load read are the terrain's by the lines of them its parser
		// takes, which its key holds (D-TERRAIN-18): their other edits leave the ground standing.
		stale[kTerrain] = layer_files_[kTerrain].moved_but(*source, { environment, opennova::env::kOvercastFile });
		stale[kEnvironment] = layer_files_[kEnvironment].moved(*source) || !stale[kTerrain];
		// A file the bodies read moved (a model, its textures; not the environment's own text, whose edits
		// rebuild the environment as they are made): the bodies loaded again with the layer.
		bodies_moved_ = bodies_moved_ ||
				layer_files_[kEnvironment].moved_but(*source, { environment, terrain, opennova::env::kOvercastFile });
	} else {
		bodies_moved_ = true;
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
	return stale[kTerrain];
}

size_t EnvironmentViewportApplier::reads_() const {
	return stamped_ ? stamped_->stamps().files().size() : 0;
}

void EnvironmentViewportApplier::note_reads_(int layer, size_t from) {
	if (!stamped_ || layer >= kLayers) return;
	const std::vector<opennova::FileStamp> &files = stamped_->stamps().files();
	for (size_t i = from; i < files.size(); ++i) layer_files_[layer].note(files[i].name, files[i].stamp);
}

void EnvironmentViewportApplier::note_missing_(int layer, const String &name) {
	const std::string text = opennova::to_std(name);
	std::vector<std::string> &missing = layer_missing_[layer];
	if (std::find(missing.begin(), missing.end(), text) == missing.end()) missing.push_back(text);
}

// --- the build -----------------------------------------------------------------------------------

void EnvironmentViewportApplier::plan_(Build &build, const EnvironmentViewport &model) {
	build.environment = !environment_built_ || !(environment_key_of_(model) == environment_key_);
	const TerrainKey terrain_key = terrain_key_of_(model);
	build.terrain = !terrain_built_ || !(terrain_key == terrain_key_);
	build.terrain_key = terrain_key;
	if (build.environment) {
		layer_missing_[kEnvironment].clear();
		build.units.push_back(Unit{ Unit::Kind::Environment });
	}
	if (build.terrain) {
		// The .trn parsed as the load begins (its units planned then); a terrain the project lacks is a
		// note, the sky standing over no ground.
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

void EnvironmentViewportApplier::rebuild(const opennova::editor::ViewportModel &viewport,
		const opennova::editor::SessionView &view, const opennova::editor::PreviewClock &) {
	build_.reset();
	project_root_ = view.project.root;
	const EnvironmentViewport &model = environment_of(viewport);
	if (model.view_status() != opennova::editor::EnvironmentViewStatus::Ready || !view.findings.assets) {
		clear();
		return;
	}
	mount_(view, model.file_name(), model.header().terrain.empty() ? std::string() : model.header().terrain + ".trn");
	auto build = std::make_unique<Build>();
	plan_(*build, model);
	// The pose alone: made whole as it is taken.
	if (build->units.size() == 1) {
		run_(*build, build->units[0], model);
		return;
	}
	build_ = std::move(build);
}

void EnvironmentViewportApplier::run_(Build &build, const Unit &unit, const EnvironmentViewport &model) {
	const size_t from = reads_();
	switch (unit.kind) {
	case Unit::Kind::Environment: run_environment_(model); break;
	case Unit::Kind::TerrainFile: run_terrain_file_(build); break;
	case Unit::Kind::TerrainBuild: run_terrain_build_(build); break;
	case Unit::Kind::NoTerrain: terrain_empty_(build.terrain_key); break;
	case Unit::Kind::Sky:
		sky_->build();
		// The sun, moon, glare and glint the environment names, read through the project's files: loaded again
		// where a file they read moved (another name the environment gives them reloads them anyway).
		if (bodies_moved_) celestial_->set_resource_root(root_files_);
		bodies_moved_ = false;
		celestial_->set_terrain_data(terrain_data_);
		break;
	case Unit::Kind::Water:
		water_->build();
		touch_scene_state_();
		break;
	case Unit::Kind::Pose: apply_state_(model); break;
	}
	note_reads_(layer_of_(unit.kind), from);
}

ApplierStep EnvironmentViewportApplier::step(const opennova::editor::ViewportModel &viewport,
		const opennova::editor::PreviewClock &, std::string &) {
	Build &build = *build_;
	const Unit unit = build.units[build.next++];
	run_(build, unit, environment_of(viewport));
	if (build.next < build.units.size()) return ApplierStep::More;
	build_.reset();
	return ApplierStep::Built;
}

void EnvironmentViewportApplier::run_environment_(const EnvironmentViewport &model) {
	const opennova::editor::MissionSceneHeader &header = model.header();
	const String name = opennova::to_gd(model.file_name());
	const String trn = header.terrain.empty() ? String() : opennova::to_gd(header.terrain) + ".trn";
	// As the drawn mission's load reads it (GameWorld::load_environment): its terrain's .trn, overcast.def (the
	// overcast table, by the runtime's own name), then the file over them (env-tod-re.md #43).
	Ref<EnvFile> env;
	env.instantiate();
	Ref<EnvFile> overcast;
	overcast.instantiate();
	if (!env->load_mission_environment(root_files_, trn, name, overcast)) {
		if (!name.is_empty()) note_missing_(kEnvironment, name);
		environment_->set_environment_data(Ref<EnvFile>());
		water_->set_mission_water_height_override(NAN);
	} else {
		// The mission header's overrides over the file, where the viewport draws them (the game's
		// apply_mission_environment_overrides; none otherwise).
		Ref<MissionEnvironmentOverrides> overrides;
		overrides.instantiate();
		overrides->assign(opennova::env::bms_env_overrides_from_header(header.attrib_flags, header.water_override,
				header.fog_override, header.fog_color, header.water_color, header.water_murk));
		env->apply_mission_overrides_or_clear(overrides);
		environment_->set_environment_data(env);
		water_->set_mission_water_height_override(overrides->get_water_height_world_or_nan());
	}
	environment_->set_overcast_data(overcast);
	// The texts the load read are the environment's whatever unit read one first (the .trn its terrain's units
	// stamped already): an edit of one builds the environment again.
	for (const String &text : { trn, name, opennova::to_gd(opennova::env::kOvercastFile) }) {
		if (!text.is_empty()) layer_files_[kEnvironment].note(opennova::to_std(text), stamped_->stamp(opennova::to_std(text)));
	}
	environment_->configure_mission_clock(header.start_time, header.minutes_per_day);
	// The weather started as a mission's start starts it (its home seeded, the drop pool reset from the
	// mission start's stream, the settle), at the next frame's tick.
	weather_start_ = true;
	precipitation_->set_resource_root(root_files_);
	environment_key_ = environment_key_of_(model);
	environment_built_ = true;
	++environment_builds_;
	touch_scene_state_();
}

void EnvironmentViewportApplier::terrain_empty_(const TerrainKey &key) {
	loading_.unref();
	foliage_->reset();
	foliage_->set_terrain_data(Ref<TerrainData>());
	terrain_->set_terrain_data(Ref<TerrainData>());
	terrain_->clear_built();
	water_->set_terrain_data(Ref<TerrainData>());
	celestial_->set_terrain_data(Ref<TerrainData>());
	terrain_data_.unref();
	terrain_built_ = true;
	terrain_key_ = key;
	touch_scene_state_();
}

void EnvironmentViewportApplier::run_terrain_file_(Build &build) {
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
	// <mission>.til, else the terrain's own polytrn_tileinfo, each where the game's loader takes it).
	terrain_->set_tile_info_override(read_mission_placed_tiles(root_files_, build.terrain_key.mission,
			build.terrain_key.terrain, build.terrain_key.environment));
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

void EnvironmentViewportApplier::run_terrain_build_(Build &build) {
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
		// Its foliage as the game's load configures it after the build (GameWorld::configure_foliage: the
		// terrain data, the mission's tiles, each definition's model and :fd texture through the root); a
		// definition whose model the project lacks a note, its slot drawing nothing.
		foliage_->reset();
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
}

opennova::editor::OperationProgress EnvironmentViewportApplier::progress() const {
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

void EnvironmentViewportApplier::update(const opennova::editor::ViewportModel &viewport, const opennova::editor::PreviewClock &) {
	apply_state_(environment_of(viewport));
}

void EnvironmentViewportApplier::apply_state_(const EnvironmentViewport &model) {
	const opennova::editor::EnvironmentViewportOptions &options = model.options();
	terrain_->set_visible(options.terrain);
	foliage_->set_visible(options.terrain);
	if (options.water != shown_water_) touch_scene_state_();
	water_->set_visible(options.water);
	shown_water_ = options.water;
	place_camera_(model);
}

void EnvironmentViewportApplier::place_camera_(const EnvironmentViewport &model) {
	const opennova::editor::OrbitCamera &camera = model.camera();
	opennova::editor::PreviewVec3 right, up, back;
	camera.axes(right, up, back);
	camera_->set_transform(Transform3D(Basis(to_godot(right), to_godot(up), to_godot(back)), to_godot(camera.eye())));
}

void EnvironmentViewportApplier::apply_listen_(const EnvironmentViewport &model) {
	// The Listen's rain loops (S23 C) as the viewport's mix binds them now, heard while the picture is drawn (held a
	// moment past its last drawn frame, as the mission device's Listen is).
	const opennova::editor::EnvironmentListen &listen = model.listen();
	if (!model.options().listen.on || !listen.open() || project_root_.empty()) {
		listen_->stop();
		listen_idle_frames_ = kListenHeldFrames;
		return;
	}
	listen_idle_frames_ = presented_ != listen_presented_ ? 0 : std::min(listen_idle_frames_ + 1, kListenHeldFrames);
	listen_presented_ = presented_;
	std::vector<PreviewSoundLoops::Channel> channels;
	channels.reserve(listen.channels().size());
	for (const opennova::editor::MissionSoundChannel &each : listen.channels()) {
		PreviewSoundLoops::Channel channel;
		channel.started = each.started;
		channel.path = each.candidate >= 0 ? each.path : std::string();
		channel.volume = each.volume;
		channel.pitch_q16 = each.pitch_q16;
		channel.at[0] = each.at.x;
		channel.at[1] = each.at.y;
		channel.at[2] = each.at.z;
		channels.push_back(std::move(channel));
	}
	listen_->follow(project_root_, channels, model.options().listen.volume, listen_idle_frames_ < kListenHeldFrames);
}

void EnvironmentViewportApplier::clear() {
	build_.reset();
	listen_->stop();
	loading_.unref();
	terrain_->set_terrain_data(Ref<TerrainData>());
	terrain_->clear_built();
	water_->set_terrain_data(Ref<TerrainData>());
	celestial_->set_terrain_data(Ref<TerrainData>());
	terrain_data_.unref();
	terrain_built_ = false;
	environment_->set_environment_data(Ref<EnvFile>());
	environment_built_ = false;
	precipitation_->hide_frame();
	for (std::vector<std::string> &missing : layer_missing_) missing.clear();
	wanted_ = Wanted();
	touch_scene_state_();
}

void EnvironmentViewportApplier::apply(const opennova::editor::ViewportModel &viewport, const opennova::editor::PreviewClock &,
		opennova::editor::ViewportDeviceReport &report) {
	report.missing.clear();
	for (const std::vector<std::string> &missing : layer_missing_)
		report.missing.insert(report.missing.end(), missing.begin(), missing.end());
	report.surface = terrain_built_ && terrain_data_.is_valid();
	if (stamped_) report.files = stamped_->stamps();
	const EnvironmentViewport &model = environment_of(viewport);
	// The viewport's clock and weather, run at the next present.
	const opennova::world::WeatherState &home = model.weather();
	wanted_.known = model.view_status() == opennova::editor::EnvironmentViewStatus::Ready;
	wanted_.ticks = model.weather_ticks();
	wanted_.clock_sets = model.clock_sets();
	wanted_.tod_fixed24 = home.tod_fixed24;
	wanted_.advance = home.tod_advance_per_tick;
	wanted_.precipitation_kind = home.precipitation_kind;
	wanted_.overcast_for_tod_q16 = home.overcast_for_tod_q16;
	wanted_.channels = home.core.scalar_channels;
	if (build_) return;
	apply_state_(model);
}

void EnvironmentViewportApplier::tick(const opennova::editor::ViewportModel &viewport, const opennova::editor::PreviewClock &clock) {
	clock_ms_ = int64_t(clock.ms());
	apply_listen_(environment_of(viewport));
	// No mirror pass unless this frame presents the picture.
	water_->set_mirror_enabled(false);
	// The weather's state follows the viewport's every frame, drawn or not; what it publishes is the
	// presented frame's.
	if (environment_built_) run_weather_();
}

// --- the frame -----------------------------------------------------------------------------------

void EnvironmentViewportApplier::publish_scene_state() {
	environment_->republish_shader_globals();
	water_->set_globals_held(false);
	water_->set_world_rendering_enabled(true);
	water_->set_globals_held(true);
}

void EnvironmentViewportApplier::run_weather_() {
	// The engine's state alone (the runtime, the environment's state): nothing here reaches the process-wide
	// globals, which the presented frame publishes.
	opennova::env::WeatherRuntime &runtime = weather_->runtime();
	opennova::env::EnvironmentState &state = environment_->state();
	if (weather_start_) {
		// As a mission's start: the home seeded from the loaded environment, then the 255-tick settle.
		weather_start_ = false;
		runtime.prepare_autonomous(&state);
		runtime.prewarm_mission_start(&state);
		ticks_seen_ = wanted_.ticks;
		resync_ = true;
	}
	opennova::world::WeatherState &home = runtime.state();
	if (wanted_.known) {
		// The ticks the viewport's home ran since: the drops' fall (the entity update's, before the weather
		// tick in the game's frame), then the game's whole weather tick, colour legs and all.
		const uint64_t behind = wanted_.ticks > ticks_seen_ ? wanted_.ticks - ticks_seen_ : 0;
		const int run = int(std::min<uint64_t>(behind, uint64_t(opennova::env::WeatherRuntime::kMaxCatchupTicks)));
		ticks_seen_ = std::max(ticks_seen_, wanted_.ticks);
		for (int i = 0; i < run; ++i) {
			home.precipitation.fall_tick(home.core.scalar_channels.rain_pct_fp, home.precipitation_kind);
			runtime.tick_fixed(&state);
			++ticks_run_;
		}
		// The viewport's clock and channels: its time, its rate, the rain, the overcast, the fog.
		home.tod_fixed24 = wanted_.tod_fixed24;
		home.tod_advance_per_tick = wanted_.advance;
		home.core.scalar_channels = wanted_.channels;
		home.precipitation_kind = wanted_.precipitation_kind;
		home.overcast_for_tod_q16 = wanted_.overcast_for_tod_q16;
		home.compute_night_phase();
		state.sync_clock_from_weather();
	}
	// A clock set anew takes its colours at once; otherwise a zero-tick refresh of the clock's targets.
	if (resync_ || wanted_.clock_sets != clock_sets_seen_) {
		resync_ = false;
		clock_sets_seen_ = wanted_.clock_sets;
		runtime.resync_colors_now(&state);
	} else {
		runtime.tick_weather(&state, 0);
	}
}

int32_t EnvironmentViewportApplier::floor_height_(void *ctx, int32_t x, int32_t y) {
	const auto *self = static_cast<const EnvironmentViewportApplier *>(ctx);
	if (!self->terrain_built_ || self->terrain_data_.is_null()) return 0;
	const Vector3 at = MissionObjectPlacer::bms_to_godot_position(Vector3(float(x) / 65536.0f, float(y) / 65536.0f, 0.0f));
	const float height = self->terrain_data_->get_height_world_bilinear(at);
	return std::isfinite(height) ? opennova::io::float_to_fp16_16_round_sat(height) : 0;
}

void EnvironmentViewportApplier::overlay_frame_() {
	ParticleRenderer *renderer = effects_->renderer();
	if (renderer == nullptr) return;
	auto submission = std::make_shared<SceneOverlaySubmission>();
	submission->frame_id = ++overlay_frame_id_;
	precipitation_->append_overlay(*submission);
	if (water_->is_water_render_active() && shown_water_) append_underwater_murk_overlay(*environment_, *water_, *submission);
	append_celestial_overlays(overlay_bodies_, *celestial_, *environment_, water_, *camera_, *submission);
	if (water_->is_water_render_active() && shown_water_)
		append_water_mirror_overlays(overlay_bodies_, celestial_, environment_, *water_, *submission);
	renderer->publish_scene_overlay(submission);
}

void EnvironmentViewportApplier::present(double dt) {
	// Nothing to draw while the first build runs.
	if (!environment_built_) return;
	++presented_;
	environment_->set_globals_held(false);
	water_->set_globals_held(false);
	water_->set_mirror_enabled(shown_water_);
	// The scene environment leg: the world pass's planes and the render eye's side of the water.
	environment_->apply_scene_pass_planes(*camera_);
	const float eye_y = camera_->get_global_transform().origin.y;
	const bool water_active = water_->is_water_active() && shown_water_;
	environment_->apply_render_eye(eye_y, water_active ? water_->get_water_height() : 0.0f, water_active);
	// The environment nodes: the weather's publication (its state the frame's tick followed), the sky, the
	// bodies.
	weather_->advance_frame(0.0);
	sky_->advance_frame(dt);
	celestial_->advance_frame(dt);
	// The terrain, then the water over its visible bounds.
	if (terrain_built_ && terrain_data_.is_valid() && terrain_->is_visible()) {
		terrain_->render_frame();
		water_->set_visible_terrain_bounds(terrain_->has_visible_terrain_bounds(), terrain_->get_visible_terrain_min_height(),
				terrain_->get_visible_terrain_max_height());
	}
	water_->advance_frame(dt);
	// The foliage leg (GameWorld::render_foliage_frame), with the terrain: its detail cells about the eye, the
	// water's height splitting the passes.
	if (terrain_built_ && terrain_data_.is_valid() && terrain_->is_visible()) {
		foliage_->set_water_height(water_active ? water_->get_water_height() : 0.0f);
		foliage_->render_frame(camera_->get_global_transform(), clock_ms_);
	}
	// The sun veil's exposure feed for the next weather ticks.
	weather_->set_sun_veil_stopdown(celestial_->get_sun_veil_stopdown());
	// The drops over the home's pool, floored on the terrain and the water.
	ParticleRenderer *renderer = effects_->renderer();
	if (renderer != nullptr) renderer->set_water_plane(water_active ? water_->get_water_height() : 0.0f, water_->get_reflection_camera());
	const opennova::world::WeatherState &home = weather_->runtime().state();
	if (home.raining()) {
		opennova::env::PrecipitationFloorSampler sampler;
		sampler.terrain_height = &EnvironmentViewportApplier::floor_height_;
		sampler.ctx = this;
		const int32_t water_z = water_active ? opennova::io::float_to_fp16_16_round_sat(water_->get_water_height()) : 0;
		precipitation_->render_pool_frame(weather_->runtime().state().precipitation, home.core.scalar_channels.rain_pct_fp,
				home.precipitation_kind, water_z, sampler, camera_, 0, uint32_t(weather_->get_terrain_light_combined_rgb()));
	} else {
		precipitation_->hide_frame();
	}
	// The overlay tail, then the particle renderer's passes composed for this frame.
	overlay_frame_();
	effects_->render(clock_ms_);
	// The frame effects: the Q3 glow frame at the camera, the screen effects' plan (the veil reads the Celestial's
	// global as it pushed it above).
	frame_effects_->present();
	// The clear colour: Godot decodes BG_COLOR from sRGB, so the gamma-domain value goes pre-encoded.
	const bool above = !water_active || eye_y > water_->get_water_height();
	clear_->get_environment()->set_bg_color(environment_->frame_clear_color_for(above).linear_to_srgb());
	touch_scene_state_();
	environment_->set_globals_held(true);
	water_->set_globals_held(true);
}

// --- the ground ----------------------------------------------------------------------------------

bool EnvironmentViewportApplier::ground_at(double x, double y, double &height) const {
	if (!terrain_built_ || terrain_data_.is_null()) return false;
	const Vector3 at = MissionObjectPlacer::bms_to_godot_position(Vector3(float(x), float(y), 0.0f));
	const float found = terrain_data_->get_height_world_bilinear(at);
	if (!std::isfinite(found)) return false;
	height = double(MissionObjectPlacer::godot_to_bms_position(Vector3(at.x, found, at.z)).z);
	return true;
}

} // namespace godot
