#include "authoring/mission_viewport_applier.h"

#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <algorithm>
#include <cmath>

#include <editor/assets/project_asset_source.h>
#include <editor/preview/mission_camera.h>
#include <editor/preview/mission_handle_edit.h>
#include <editor/preview/mission_options.h>
#include <editor/preview/mission_viewport.h>
#include <editor/preview/viewport_device.h>
#include <editor/session/view/session_view.h>
#include <formats/env/env.h>
#include <runtime/renderer/render_order.h>

#include "env/mission_environment_overrides.h"
#include "mission/mission_object_placer.h"
#include "render/frame_fx.h"
#include "util/string_convert.h"

namespace godot {

namespace {

using opennova::editor::MissionScene;
using opennova::editor::MissionSceneHeader;
using opennova::editor::MissionViewport;

Vector3 to_godot(const opennova::editor::PreviewVec3 &v) {
	return Vector3(v.x, v.y, v.z);
}

const MissionViewport &mission_of(const opennova::editor::ViewportModel &model) {
	return static_cast<const MissionViewport &>(model);
}

// Each device's scene state is its own: a counter no two share, never 0 (the shipped defaults').
uint64_t next_scene_state() {
	static uint64_t next = 0;
	return ++next;
}

} // namespace

bool MissionViewportApplier::EnvironmentKey::operator==(const EnvironmentKey &o) const {
	return environment == o.environment && start_time == o.start_time && minutes_per_day == o.minutes_per_day &&
			attrib_flags == o.attrib_flags && water_override == o.water_override && fog_override == o.fog_override &&
			water_murk == o.water_murk && std::equal(std::begin(fog_color), std::end(fog_color), std::begin(o.fog_color)) &&
			std::equal(std::begin(water_color), std::end(water_color), std::begin(o.water_color));
}

MissionViewportApplier::EnvironmentKey MissionViewportApplier::environment_key_of_(const MissionSceneHeader &header) {
	EnvironmentKey key;
	key.environment = header.environment;
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

MissionViewportApplier::MissionViewportApplier(SubViewport &viewport) : scene_state_(next_scene_state()) {
	viewport.set_msaa_3d(Viewport::MSAA_4X);
	root_ = memnew(Node3D);
	root_->set_name("Mission");
	viewport.add_child(root_);
	// Retail shaders write gamma-domain values and rely on one terminal display decode per 3D
	// view; the clear colour is the environment's frame clear (BG_COLOR, no ambient: the game's
	// ClearColor node, game_world_frame.cpp's clear colour leg).
	root_->add_child(memnew(DisplayDecode));
	clear_ = memnew(WorldEnvironment);
	clear_->set_name("ClearColor");
	Ref<Environment> environment;
	environment.instantiate();
	environment->set_background(Environment::BG_COLOR);
	environment->set_ambient_source(Environment::AMBIENT_SOURCE_DISABLED);
	clear_->set_environment(environment);
	root_->add_child(clear_);
	environment_ = memnew(MissionEnvironment);
	environment_->set_name("Environment");
	root_->add_child(environment_);
	sky_ = memnew(SkyDome);
	sky_->set_name("Sky");
	sky_->set_environment_path(NodePath("../Environment"));
	root_->add_child(sky_);
	water_ = memnew(Water);
	water_->set_name("Water");
	water_->set_environment_path(NodePath("../Environment"));
	root_->add_child(water_);
	terrain_ = memnew(Terrain);
	terrain_->set_name("Terrain");
	terrain_->set_environment_path(NodePath("../Environment"));
	terrain_->set_water_path(NodePath("../Water"));
	root_->add_child(terrain_);
	camera_ = memnew(Camera3D);
	camera_->set_name("Camera"); // the device's own: the water's mirror has a camera of its own
	camera_->set_keep_aspect_mode(Camera3D::KEEP_WIDTH);
	camera_->set_fov(opennova::editor::OrbitCamera::fov_horizontal_degrees());
	camera_->set_current(true);
	root_->add_child(camera_);
	root_files_.instantiate();
}

// --- the files -----------------------------------------------------------------------------------

void MissionViewportApplier::mount_(const opennova::editor::SessionView &view) {
	const std::shared_ptr<const opennova::editor::ProjectAssetSource> source = view.findings.assets;
	const uint64_t generation = source ? source->generation() : 0;
	if (source != mounted_) {
		// The project's files as the game would find them: the root over the session's source.
		root_files_->mount_files(source);
		mounted_ = source;
		mounted_generation_ = generation;
		return;
	}
	if (generation != mounted_generation_) {
		// A file moved: what the root cached of the old one goes.
		root_files_->files_changed();
		mounted_generation_ = generation;
	}
}

void MissionViewportApplier::note_missing_(const String &name) {
	const std::string text = opennova::to_std(name);
	if (std::find(missing_.begin(), missing_.end(), text) == missing_.end()) missing_.push_back(text);
}

// --- the build -----------------------------------------------------------------------------------

void MissionViewportApplier::plan_(Build &build, const MissionScene &scene) {
	const MissionSceneHeader &header = scene.header();
	// The layers whose keys moved: the environment, the terrain (with the sky and the water bound
	// to them).
	build.environment = !environment_built_ || !(environment_key_of_(header) == environment_key_);
	TerrainKey terrain_key;
	terrain_key.terrain = header.terrain;
	terrain_key.tile_set = header.tile_set;
	build.terrain = !terrain_built_ || !(terrain_key == terrain_key_);
	if (build.environment) build.units.push_back(Unit{ Unit::Kind::Environment });
	if (build.terrain) {
		// The .trn parsed as the load begins (its units planned then): one unit per file here, as
		// many as the load will have; a terrain the project lacks is a note, the picture standing
		// without one.
		loading_.instantiate();
		loading_->set_mission_tile_set(opennova::to_gd(header.tile_set));
		const String trn = opennova::to_gd(header.terrain) + ".trn";
		if (!header.terrain.empty() && root_files_->has_file(trn) &&
				loading_->begin_load_from_resource_root(root_files_, trn) == OK) {
			for (int i = 0; i < loading_->get_load_step_count(); ++i) build.units.push_back(Unit{ Unit::Kind::TerrainFile });
			// The build's units are planned as it begins (a tile each): counted then.
			build.units.push_back(Unit{ Unit::Kind::TerrainBuild });
		} else {
			if (!header.terrain.empty()) note_missing_(trn);
			loading_.unref();
		}
		build.units.push_back(Unit{ Unit::Kind::Sky });
		build.units.push_back(Unit{ Unit::Kind::Water });
	}
	build.units.push_back(Unit{ Unit::Kind::Pose });
}

void MissionViewportApplier::rebuild(const opennova::editor::ViewportModel &viewport,
		const opennova::editor::SessionView &view, const opennova::editor::PreviewClock &) {
	build_.reset();
	loading_.unref();
	const MissionViewport &mission = mission_of(viewport);
	if (mission.view_status() != opennova::editor::MissionViewStatus::Ready || !view.findings.assets) {
		clear();
		return;
	}
	mount_(view);
	auto build = std::make_unique<Build>();
	plan_(*build, mission.scene());
	build_ = std::move(build);
}

void MissionViewportApplier::run_environment_(const MissionScene &scene) {
	const MissionSceneHeader &header = scene.header();
	const String name = opennova::to_gd(header.environment) + ".env";
	Ref<EnvFile> env;
	env.instantiate();
	if (header.environment.empty() || !root_files_->has_file(name) || env->load_from_resource_root(root_files_, name) != OK) {
		if (!header.environment.empty()) note_missing_(name);
		// The retail noon the environment has with no file: the mission still shows.
		environment_->set_environment_data(Ref<EnvFile>());
		env_file_.unref();
		water_->set_mission_water_height_override(NAN);
	} else {
		// The header's overrides over the file (the game's apply_mission_environment_overrides).
		Ref<MissionEnvironmentOverrides> overrides;
		overrides.instantiate();
		overrides->assign(opennova::env::bms_env_overrides_from_header(header.attrib_flags, header.water_override,
				header.fog_override, header.fog_color, header.water_color, header.water_murk));
		if (overrides->is_empty()) env->clear_mission_overrides();
		else env->apply_mission_overrides(overrides);
		environment_->set_environment_data(env);
		env_file_ = env;
		water_->set_mission_water_height_override(
				overrides->get_has_water_height() ? overrides->get_water_height_world() : NAN);
	}
	// The overcast table beside it where the project has one (the game's, read the same way).
	Ref<EnvFile> overcast;
	overcast.instantiate();
	const bool has_overcast = root_files_->has_file("overcast.def") && overcast->load_from_resource_root(root_files_, "overcast.def") == OK;
	environment_->set_overcast_data(has_overcast ? overcast : Ref<EnvFile>());
	environment_->configure_mission_clock(header.start_time, header.minutes_per_day);
	applied_time_ = -2.0;
	environment_key_ = environment_key_of_(header);
	environment_built_ = true;
}

ApplierStep MissionViewportApplier::step(const opennova::editor::ViewportModel &viewport,
		const opennova::editor::PreviewClock &, std::string &failure) {
	Build &build = *build_;
	const MissionScene &scene = mission_of(viewport).scene();
	const Unit unit = build.units[build.next++];
	switch (unit.kind) {
	case Unit::Kind::Environment: run_environment_(scene); break;
	case Unit::Kind::TerrainFile: {
		if (loading_.is_null()) break;
		const TerrainData::LoadStep result = loading_->load_step();
		if (result == TerrainData::LOAD_STEP_FAILED) {
			failure = "The terrain " + scene.header().terrain + " does not load: " + opennova::to_std(loading_->get_load_step_label());
			loading_.unref();
			build_.reset();
			return ApplierStep::Failed;
		}
		if (result == TerrainData::LOAD_STEP_DONE) {
			terrain_data_ = loading_;
			loading_.unref();
			terrain_->set_terrain_data(terrain_data_);
			water_->set_terrain_data(terrain_data_);
			terrain_built_ = false;
			if (!terrain_->build_begin()) {
				failure = "The terrain " + scene.header().terrain + " does not build.";
				build_.reset();
				return ApplierStep::Failed;
			}
			// Its units, a tile each, counted now that they are planned (the one planned stands for
			// the first).
			const int steps = terrain_->get_build_step_count();
			for (int i = 1; i < steps; ++i)
				build.units.insert(build.units.begin() + std::ptrdiff_t(build.next), Unit{ Unit::Kind::TerrainBuild });
		}
		break;
	}
	case Unit::Kind::TerrainBuild: {
		const Terrain::BuildStep result = terrain_->build_step();
		if (result == Terrain::BUILD_STEP_FAILED) {
			failure = "The terrain " + scene.header().terrain + " does not build.";
			build_.reset();
			return ApplierStep::Failed;
		}
		if (result == Terrain::BUILD_STEP_DONE) {
			terrain_built_ = true;
			terrain_key_.terrain = scene.header().terrain;
			terrain_key_.tile_set = scene.header().tile_set;
		}
		break;
	}
	case Unit::Kind::Sky: sky_->build(); break;
	case Unit::Kind::Water: water_->build(); break;
	case Unit::Kind::Pose:
		// The state as it is now, as an Update applies it (one that came while the build ran is
		// folded into this).
		apply_state_(viewport);
		break;
	}
	if (build.next < build.units.size()) return ApplierStep::More;
	build_.reset();
	return ApplierStep::Built;
}

opennova::editor::OperationProgress MissionViewportApplier::progress() const {
	opennova::editor::OperationProgress progress;
	progress.unit = opennova::editor::OperationUnit::Steps;
	if (!build_) return progress;
	progress.done = build_->next;
	progress.total = build_->units.size();
	if (build_->next < build_->units.size()) {
		switch (build_->units[build_->next].kind) {
		case Unit::Kind::Environment: progress.label = "environment"; break;
		case Unit::Kind::TerrainFile: progress.label = "terrain files"; break;
		case Unit::Kind::TerrainBuild: progress.label = "terrain"; break;
		case Unit::Kind::Sky: progress.label = "sky"; break;
		case Unit::Kind::Water: progress.label = "water"; break;
		case Unit::Kind::Pose: progress.label = "pose"; break;
		}
	}
	return progress;
}

// --- the state -----------------------------------------------------------------------------------

void MissionViewportApplier::update(const opennova::editor::ViewportModel &viewport, const opennova::editor::PreviewClock &) {
	apply_state_(viewport);
}

void MissionViewportApplier::apply_state_(const opennova::editor::ViewportModel &viewport) {
	const MissionViewport &mission = mission_of(viewport);
	const opennova::editor::MissionViewportOptions &options = mission.options();
	// The layers the options switch.
	terrain_->set_visible(options.terrain);
	sky_->set_visible(options.sky);
	water_->set_visible(options.water);
	shown_water_ = options.water;
	// The time of day: the mission's start time, or the option's hour.
	if (options.time != applied_time_ && environment_built_) {
		if (options.time >= 0.0) environment_->debug_set_mission_minute_of_day(options.time * 60.0);
		else environment_->configure_mission_clock(environment_key_.start_time, environment_key_.minutes_per_day);
		applied_time_ = options.time;
	}
	place_camera_(viewport);
}

void MissionViewportApplier::place_camera_(const opennova::editor::ViewportModel &viewport) {
	const opennova::editor::OrbitCamera &camera = mission_of(viewport).camera();
	opennova::editor::PreviewVec3 right, up, back;
	camera.axes(right, up, back);
	camera_->set_transform(Transform3D(Basis(to_godot(right), to_godot(up), to_godot(back)), to_godot(camera.eye())));
	// The world pass's planes: the game's near, its far from the environment's fog.
	camera_->set_near(opennova::renderer::kScenePassNearZ);
	camera_->set_far(opennova::renderer::scene_far_plane(environment_->get_fog_distance()));
}

void MissionViewportApplier::clear() {
	build_.reset();
	loading_.unref();
	terrain_->set_terrain_data(Ref<TerrainData>());
	water_->set_terrain_data(Ref<TerrainData>());
	terrain_data_.unref();
	terrain_built_ = false;
	environment_->set_environment_data(Ref<EnvFile>());
	env_file_.unref();
	environment_built_ = false;
	missing_.clear();
	applied_time_ = -2.0;
}

void MissionViewportApplier::apply(const opennova::editor::ViewportModel &viewport, const opennova::editor::PreviewClock &,
		opennova::editor::ViewportDeviceReport &report) {
	report.missing = missing_;
	report.surface = terrain_built_ && terrain_data_.is_valid();
	if (build_) return;
	apply_state_(viewport);
}

// --- the frame -----------------------------------------------------------------------------------

void MissionViewportApplier::publish_scene_state() {
	// Every global the mission's picture renders with written again, whatever another picture
	// published since: the environment's (the lighting block and the rest), the water's (its active
	// flag, height and plane, pushed as its render activity syncs).
	environment_->republish_shader_globals();
	water_->set_world_rendering_enabled(true);
}

void MissionViewportApplier::tick(const opennova::editor::ViewportModel &, const opennova::editor::PreviewClock &) {
	// The frame's start (the Shell ticks before it arbitrates): no mirror pass unless this frame
	// presents the picture.
	water_->set_mirror_enabled(false);
}

void MissionViewportApplier::present(double dt) {
	// Nothing to draw while the first build runs (the device keeps no picture yet).
	if (build_ && !environment_built_) return;
	water_->set_mirror_enabled(shown_water_);
	// In the game's leg order (game_world_frame.cpp): the camera, the render eye and the clear, the
	// environment nodes, the terrain, the water.
	const float eye_y = camera_->get_global_transform().origin.y;
	const bool water_active = water_->is_water_active() && shown_water_;
	environment_->apply_render_eye(eye_y, water_active ? water_->get_water_height() : 0.0f, water_active);
	const bool above = !water_active || eye_y > water_->get_water_height();
	// Godot decodes BG_COLOR from sRGB before writing the scene target: the retail gamma-domain value
	// pre-encoded, as the game's clear colour leg does.
	clear_->get_environment()->set_bg_color(environment_->frame_clear_color_for(above).linear_to_srgb());
	sky_->advance_frame(dt);
	if (terrain_built_) {
		terrain_->render_frame();
		water_->set_visible_terrain_bounds(terrain_->has_visible_terrain_bounds(), terrain_->get_visible_terrain_min_height(),
				terrain_->get_visible_terrain_max_height());
	}
	water_->advance_frame(dt);
}

// --- the ground ----------------------------------------------------------------------------------

bool MissionViewportApplier::ground_at(double x, double y, double &height) const {
	if (!terrain_built_ || terrain_data_.is_null()) return false;
	const Vector3 at = MissionObjectPlacer::bms_to_godot_position(Vector3(float(x), float(y), 0.0f));
	const float found = terrain_data_->get_height_world_bilinear(at);
	if (!std::isfinite(found)) return false;
	height = double(MissionObjectPlacer::godot_to_bms_position(Vector3(at.x, found, at.z)).z);
	return true;
}

bool MissionViewportApplier::surface_between(const double from[3], const double to[3], double point[3]) const {
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

bool MissionViewportApplier::surface_at(float x, float y, float point[3]) const {
	// Along the applied camera's ray, as far as a pick reaches.
	if (!terrain_built_ || terrain_data_.is_null() || camera_->get_viewport() == nullptr) return false;
	const Vector3 from = camera_->project_ray_origin(Vector2(x, y));
	const Vector3 along = camera_->project_ray_normal(Vector2(x, y));
	const Vector3 hit = terrain_data_->raycast_terrain(from, from + along * float(opennova::editor::kMissionPickReach));
	if (!hit.is_finite()) return false;
	const Vector3 mission = MissionObjectPlacer::godot_to_bms_position(hit);
	point[0] = mission.x;
	point[1] = mission.y;
	point[2] = mission.z;
	return true;
}

} // namespace godot
