#include "authoring/mission_viewport_applier.h"

#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/object.hpp>
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
#include <editor/preview/preview_clock.h>
#include <editor/preview/viewport_device.h>
#include <editor/session/view/session_view.h>
#include <formats/env/env.h>
#include <runtime/renderer/render_order.h>

#include "env/mission_environment_overrides.h"
#include "mission/mission_data.h"
#include "object/entity_ref.h"
#include "render/frame_fx.h"
#include "render/object_lod_frame.h"
#include "util/string_convert.h"

namespace godot {

namespace {

using opennova::editor::MissionEntityMark;
using opennova::editor::MissionPool;
using opennova::editor::MissionScene;
using opennova::editor::MissionSceneHeader;
using opennova::editor::MissionViewport;
using opennova::editor::NodeId;

Vector3 to_godot(const opennova::editor::PreviewVec3 &v) {
	return Vector3(v.x, v.y, v.z);
}

const MissionViewport &mission_of(const opennova::editor::ViewportModel &model) {
	return static_cast<const MissionViewport &>(model);
}

// The placer's kind of a pool's entity (MissionData::EntityKind).
int kind_of(MissionPool pool) {
	switch (pool) {
	case MissionPool::Item: return MissionData::KIND_ITEM;
	case MissionPool::Building: return MissionData::KIND_BUILDING;
	case MissionPool::Marker: return MissionData::KIND_MARKER;
	case MissionPool::Organic: return MissionData::KIND_ORGANIC;
	}
	return MissionData::KIND_ITEM;
}

int64_t now_us() {
	return int64_t(Time::get_singleton()->get_ticks_usec());
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

int MissionViewportApplier::layer_of_(Unit::Kind kind) {
	switch (kind) {
	case Unit::Kind::Environment:
	case Unit::Kind::Sky: return kEnvironment;
	case Unit::Kind::TerrainFile:
	case Unit::Kind::TerrainBuild:
	case Unit::Kind::Water: return kTerrain;
	case Unit::Kind::Items:
	case Unit::Kind::Model:
	case Unit::Kind::Place:
	case Unit::Kind::Lift: return kEntities;
	case Unit::Kind::Pose: break;
	}
	return kLayers;
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
	terrain_id_ = terrain_->get_instance_id();
	camera_ = memnew(Camera3D);
	camera_->set_name("Camera"); // the device's own: the water's mirror has a camera of its own
	camera_->set_keep_aspect_mode(Camera3D::KEEP_WIDTH);
	camera_->set_fov(opennova::editor::OrbitCamera::fov_horizontal_degrees());
	camera_->set_current(true);
	root_->add_child(camera_);
	// The placer's parent (its "MissionObjects" container) and the lifted models'.
	objects_ = memnew(Node3D);
	objects_->set_name("Objects");
	root_->add_child(objects_);
	lifted_root_ = memnew(Node3D);
	lifted_root_->set_name("Lifted");
	root_->add_child(lifted_root_);
	root_files_.instantiate();
	clock_.instantiate();
}

MissionViewportApplier::~MissionViewportApplier() {
	// The terrain lets go of the placer while both stand (the device frees its SubViewport after).
	if (Terrain *terrain = Object::cast_to<Terrain>(ObjectDB::get_instance(terrain_id_)))
		terrain->set_static_shadow_placer(Ref<MissionObjectPlacer>());
}

// --- the files -----------------------------------------------------------------------------------

void MissionViewportApplier::mount_(const opennova::editor::SessionView &view) {
	const std::shared_ptr<const opennova::editor::ProjectAssetSource> source = view.findings.assets;
	const bool another = source != mounted_ || !stamped_;
	bool stale[kLayers] = { another, another, another };
	if (!another) {
		if (!stamped_->stamps().moved(*source)) return;
		// What moved: the layers that read it; a moved file no layer's units read (a texture a model
		// read as it was first drawn) is the entities'.
		stale[kEnvironment] = layer_files_[kEnvironment].moved(*source);
		stale[kTerrain] = layer_files_[kTerrain].moved(*source);
		stale[kEntities] = layer_files_[kEntities].moved(*source) || (!stale[kEnvironment] && !stale[kTerrain]);
	}
	// A fresh record of what is asked for, the root mounted over it (its caches and the process's
	// dropped: a file moved); the layers that read none of what moved keep their files noted.
	mounted_ = source;
	stamped_ = std::make_shared<opennova::editor::StampedFiles>(source);
	root_files_->mount_files(stamped_);
	for (int layer = 0; layer < kLayers; ++layer) {
		if (stale[layer]) {
			layer_files_[layer].clear();
			layer_missing_[layer].clear();
			continue;
		}
		for (const opennova::editor::FileStamp &file : layer_files_[layer].files()) stamped_->stamp(file.name);
	}
	if (stale[kEnvironment]) environment_built_ = false;
	if (stale[kTerrain]) terrain_built_ = false;
	if (stale[kEntities]) drop_entities_();
}

size_t MissionViewportApplier::reads_() const {
	return stamped_ ? stamped_->stamps().files().size() : 0;
}

void MissionViewportApplier::note_reads_(int layer, size_t from) {
	if (!stamped_ || layer >= kLayers) return;
	const std::vector<opennova::editor::FileStamp> &files = stamped_->stamps().files();
	for (size_t i = from; i < files.size(); ++i) layer_files_[layer].note(files[i].name, files[i].stamp);
}

void MissionViewportApplier::note_missing_(int layer, const String &name) {
	const std::string text = opennova::to_std(name);
	std::vector<std::string> &missing = layer_missing_[layer];
	if (std::find(missing.begin(), missing.end(), text) == missing.end()) missing.push_back(text);
}

String MissionViewportApplier::graphic_of_(int64_t item) {
	if (placer_.is_null()) return String();
	const Ref<ItemDatabase> items = placer_->get_item_db();
	if (items.is_null() || !items->has_item(int(item))) return String();
	return placer_->graphic_for(int(item));
}

Transform3D MissionViewportApplier::transform_of_(const MissionEntityMark &entity) const {
	const Vector3 position(float(entity.x), float(entity.y), float(entity.z));
	const Vector3 rotation(float(entity.pitch), float(entity.yaw), float(entity.roll));
	return placer_.is_valid() ? placer_->item_entity_transform(position, rotation, int(entity.item))
							  : MissionObjectPlacer::entity_transform(position, rotation);
}

ObjectModel *MissionViewportApplier::model_of_(const Placed &placed) {
	return placed.model != 0 ? Object::cast_to<ObjectModel>(ObjectDB::get_instance(placed.model)) : nullptr;
}

// --- the build -----------------------------------------------------------------------------------

void MissionViewportApplier::plan_(Build &build, const MissionScene &scene) {
	const MissionSceneHeader &header = scene.header();
	// The layers whose keys moved: the environment, the terrain (the sky and the water bound to them).
	build.environment = !environment_built_ || !(environment_key_of_(header) == environment_key_);
	TerrainKey terrain_key;
	terrain_key.terrain = header.terrain;
	terrain_key.tile_set = header.tile_set;
	build.terrain = !terrain_built_ || !(terrain_key == terrain_key_);
	if (build.environment) {
		layer_missing_[kEnvironment].clear();
		build.units.push_back(Unit{ Unit::Kind::Environment });
	}
	if (build.terrain) {
		// The .trn parsed as the load begins (its units planned then): one unit per file here, as many
		// as the load will have; a terrain the project lacks is a note, the picture standing without.
		layer_missing_[kTerrain].clear();
		loading_.instantiate();
		loading_->set_mission_tile_set(opennova::to_gd(header.tile_set));
		const String trn = opennova::to_gd(header.terrain) + ".trn";
		const size_t from = reads_();
		const bool begun = !header.terrain.empty() && root_files_->has_file(trn) &&
				loading_->begin_load_from_resource_root(root_files_, trn) == OK;
		note_reads_(kTerrain, from);
		if (begun) {
			for (int i = 0; i < loading_->get_load_step_count(); ++i) build.units.push_back(Unit{ Unit::Kind::TerrainFile });
			// The build's units are planned as it begins (a tile each): counted then.
			build.units.push_back(Unit{ Unit::Kind::TerrainBuild });
		} else {
			if (!header.terrain.empty()) note_missing_(kTerrain, trn);
			loading_.unref();
			// No terrain: the one held dropped, the layer built (with nothing) from its key.
			terrain_->set_terrain_data(Ref<TerrainData>());
			water_->set_terrain_data(Ref<TerrainData>());
			terrain_data_.unref();
			terrain_built_ = true;
			terrain_key_ = terrain_key;
		}
	}
	if (build.environment || build.terrain) {
		build.units.push_back(Unit{ Unit::Kind::Sky });
		build.units.push_back(Unit{ Unit::Kind::Water });
	}
	// The entities: placed whole when none are placed; else the rows added since or of another item
	// lifted (keys are rows), the lifted set folded back into a whole placement past kLiftedMost.
	bool place = !placed_ || placer_.is_null();
	std::vector<NodeId> adds;
	if (!place) {
		for (const MissionEntityMark &entity : scene.entities()) {
			const auto found = entities_.find(entity.row);
			if (found == entities_.end() || found->second.item != entity.item) adds.push_back(entity.row);
		}
		size_t lifted = adds.size();
		for (const NodeId row : lifted_)
			if (std::find(adds.begin(), adds.end(), row) == adds.end()) ++lifted;
		place = lifted > kLiftedMost;
	}
	build.place = place;
	if (placer_.is_null()) {
		// The item table read first; the graphics it names are planned as it is read.
		layer_missing_[kEntities].clear();
		build.units.push_back(Unit{ Unit::Kind::Items });
	} else {
		// The graphics the entities name, each warmed once: every one for a placement, the adds' for a
		// lift.
		std::vector<String> graphics;
		const auto want = [&](const MissionEntityMark &entity) {
			// A marker draws nothing (the placement places none).
			if (entity.pool == MissionPool::Marker) return;
			const String graphic = graphic_of_(entity.item);
			if (graphic.is_empty() || warm_.count(opennova::to_std(graphic)) ||
					std::find(graphics.begin(), graphics.end(), graphic) != graphics.end())
				return;
			graphics.push_back(graphic);
		};
		if (place) {
			for (const MissionEntityMark &entity : scene.entities()) want(entity);
		} else {
			for (const NodeId row : adds)
				if (const MissionEntityMark *entity = scene.entity(row)) want(*entity);
		}
		for (const String &graphic : graphics) {
			Unit unit{ Unit::Kind::Model };
			unit.graphic = graphic;
			build.units.push_back(unit);
		}
	}
	if (place) {
		build.units.push_back(Unit{ Unit::Kind::Place });
	} else {
		build.adds = std::move(adds);
		for (size_t first = 0; first < build.adds.size(); first += kLiftPerUnit) {
			Unit unit{ Unit::Kind::Lift };
			unit.first = first;
			build.units.push_back(unit);
		}
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
	// The pose alone, or one unit of lifts over warm graphics and the pose: made whole as it is taken
	// (an entity removed, added or of another item, a header field no layer reads).
	const bool whole = build->units.size() == 1 || (build->units.size() == 2 && build->units[0].kind == Unit::Kind::Lift);
	if (whole) {
		std::string failure;
		for (size_t i = 0; i < build->units.size(); ++i) run_(*build, build->units[i], viewport, failure);
		return;
	}
	build_ = std::move(build);
}

bool MissionViewportApplier::run_(Build &build, const Unit &unit, const opennova::editor::ViewportModel &viewport,
		std::string &failure) {
	const MissionScene &scene = mission_of(viewport).scene();
	const size_t from = reads_();
	bool ok = true;
	switch (unit.kind) {
	case Unit::Kind::Environment: run_environment_(scene); break;
	case Unit::Kind::TerrainFile: {
		if (loading_.is_null()) break;
		const TerrainData::LoadStep result = loading_->load_step();
		if (result == TerrainData::LOAD_STEP_FAILED) {
			failure = "The terrain " + scene.header().terrain + " does not load: " + opennova::to_std(loading_->get_load_step_label());
			loading_.unref();
			ok = false;
			break;
		}
		if (result == TerrainData::LOAD_STEP_DONE) {
			terrain_data_ = loading_;
			loading_.unref();
			terrain_->set_terrain_data(terrain_data_);
			water_->set_terrain_data(terrain_data_);
			terrain_built_ = false;
			if (!terrain_->build_begin()) {
				failure = "The terrain " + scene.header().terrain + " does not build.";
				ok = false;
				break;
			}
			// Its units, a tile each, counted now that they are planned (the one planned stands for the
			// first).
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
			ok = false;
			break;
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
	case Unit::Kind::Items:
		run_items_();
		if (placer_->get_item_db().is_valid()) {
			// The item table read: the graphics the entities name, now that it names them, before the
			// placement.
			std::vector<String> graphics;
			for (const MissionEntityMark &entity : scene.entities()) {
				if (entity.pool == MissionPool::Marker) continue;
				const String graphic = graphic_of_(entity.item);
				if (!graphic.is_empty() && !warm_.count(opennova::to_std(graphic)) &&
						std::find(graphics.begin(), graphics.end(), graphic) == graphics.end())
					graphics.push_back(graphic);
			}
			for (size_t i = 0; i < graphics.size(); ++i) {
				Unit model{ Unit::Kind::Model };
				model.graphic = graphics[i];
				build.units.insert(build.units.begin() + std::ptrdiff_t(build.next + i), model);
			}
		}
		break;
	case Unit::Kind::Model: run_model_(unit.graphic); break;
	case Unit::Kind::Place:
		if (build.place_run.is_null()) run_place_begin_(scene, build);
		run_place_step_(build);
		break;
	case Unit::Kind::Lift: run_lift_(scene, build, unit.first); break;
	case Unit::Kind::Pose:
		// The state as it is now, as an Update applies it (one that came while the build ran is folded
		// into this).
		apply_state_(viewport);
		break;
	}
	note_reads_(layer_of_(unit.kind), from);
	return ok;
}

ApplierStep MissionViewportApplier::step(const opennova::editor::ViewportModel &viewport,
		const opennova::editor::PreviewClock &, std::string &failure) {
	Build &build = *build_;
	const Unit unit = build.units[build.next++];
	if (!run_(build, unit, viewport, failure)) {
		build_.reset();
		return ApplierStep::Failed;
	}
	if (build.next < build.units.size()) return ApplierStep::More;
	build_.reset();
	return ApplierStep::Built;
}

void MissionViewportApplier::run_environment_(const MissionScene &scene) {
	const MissionSceneHeader &header = scene.header();
	const String name = opennova::to_gd(header.environment) + ".env";
	Ref<EnvFile> env;
	env.instantiate();
	if (header.environment.empty() || !root_files_->has_file(name) || env->load_from_resource_root(root_files_, name) != OK) {
		if (!header.environment.empty()) note_missing_(kEnvironment, name);
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

void MissionViewportApplier::run_items_() {
	// The game's placer over the device's root: its item table read from the project's files.
	placer_ = MissionObjectPlacer::create(root_files_, Ref<ItemDatabase>());
	placer_->set_panm_clock(clock_);
	if (placer_->get_item_db().is_null()) note_missing_(kEntities, "items.def");
}

void MissionViewportApplier::run_model_(const String &graphic) {
	if (placer_.is_null()) return;
	const Ref<ObjectData> data = placer_->object_data_for(graphic);
	if (data.is_null()) {
		note_missing_(kEntities, graphic.get_file().get_basename() + ".3di");
		warm_[opennova::to_std(graphic)] = false;
		return;
	}
	// Its static batches harvested once, so the placement finds every graphic warm.
	placer_->warm_static_graphic(graphic, objects_);
	warm_[opennova::to_std(graphic)] = true;
}

// The placement begun as a run (MissionPlacementRun, D5): its units are the build's Place units, one
// stepped per unit; a newer placement on the placer cancels a run in flight by its generation.
void MissionViewportApplier::run_place_begin_(const MissionScene &scene, Build &build) {
	// What the last placement and the lifts made goes: the run's container is made afresh.
	terrain_->set_static_shadow_placer(Ref<MissionObjectPlacer>());
	for (auto &entry : entities_)
		if (entry.second.lifted)
			if (ObjectModel *model = model_of_(entry.second)) model->queue_free();
	entities_.clear();
	lifted_.clear();
	shadow_pending_.clear();
	place_started_us_ = now_us();
	place_units_planned_ = 1;
	if (placer_.is_null() || placer_->get_item_db().is_null()) {
		// No item table: nothing draws, every entity held as placed with nothing to show.
		for (const MissionEntityMark &entity : scene.entities()) {
			Placed placed;
			placed.item = entity.item;
			placed.stamp = entity.stamp;
			entities_[entity.row] = placed;
		}
		return;
	}
	std::vector<MissionObjectPlacer::PlacementRow> rows;
	rows.reserve(scene.entities().size());
	int key = 0;
	for (const MissionEntityMark &entity : scene.entities()) {
		// A key of the device's own: the placer registers no static whose key is 0, and the file's
		// SSNs repeat.
		++key;
		MissionObjectPlacer::PlacementRow row;
		row.kind = kind_of(entity.pool);
		row.index = entity.index;
		row.item_id = int(entity.item);
		row.bms_id = key;
		row.team = entity.team;
		row.position = Vector3(float(entity.x), float(entity.y), float(entity.z));
		row.rotation_deg = Vector3(float(entity.pitch), float(entity.yaw), float(entity.roll));
		rows.push_back(row);
		Placed placed;
		placed.item = entity.item;
		placed.stamp = entity.stamp;
		placed.key = key;
		placed.kind = row.kind;
		placed.index = row.index;
		entities_[entity.row] = placed;
	}
	build.place_run = placer_->begin_place_rows(rows, objects_, Dictionary());
}

// One unit of the placement run: the run's units joined to the build's as it counts them.
void MissionViewportApplier::run_place_step_(Build &build) {
	if (build.place_run.is_valid()) {
		const MissionPlacementRun::Step result = build.place_run->step();
		const int known = build.place_run->get_step_count();
		for (; place_units_planned_ < known; ++place_units_planned_)
			build.units.insert(build.units.begin() + std::ptrdiff_t(build.next), Unit{ Unit::Kind::Place });
		if (result != MissionPlacementRun::STEP_DONE) return;
		build.place_run.unref();
		// The individual models the placement made, each to its row by the key it carries.
		std::unordered_map<int, NodeId> rows;
		for (const auto &entry : entities_) rows[entry.second.key] = entry.first;
		const TypedArray<ObjectModel> models = placer_->get_placed_models();
		for (int64_t i = 0; i < models.size(); ++i) {
			ObjectModel *model = Object::cast_to<ObjectModel>(models[i]);
			const Ref<EntityRef> ref = model ? model->get_entity_ref() : Ref<EntityRef>();
			const auto found = ref.is_valid() ? rows.find(ref->get_bms_id()) : rows.end();
			if (found != rows.end()) entities_[found->second].model = model->get_instance_id();
		}
	}
	last_place_us_ = now_us() - place_started_us_;
	++placements_;
	placed_ = true;
	terrain_->set_static_shadow_placer(shown_shadows_ ? placer_ : Ref<MissionObjectPlacer>());
}

ObjectModel *MissionViewportApplier::lift_(const MissionEntityMark &entity) {
	// A marker draws nothing, as the placement places none.
	if (placer_.is_null() || placer_->get_item_db().is_null() || entity.pool == MissionPool::Marker) return nullptr;
	// One model for the item as the placer builds one (its graphic, scale, rig and shadow).
	ObjectModel *model = placer_->build_animated_model(int(entity.item), lifted_root_);
	if (model == nullptr) return nullptr;
	model->set_name(vformat("Lifted_%d", int64_t(entity.row)));
	model->set_transform(transform_of_(entity));
	return model;
}

void MissionViewportApplier::run_lift_(const MissionScene &scene, Build &build, size_t first) {
	for (size_t i = first; i < build.adds.size() && i < first + kLiftPerUnit; ++i) {
		const MissionEntityMark *entity = scene.entity(build.adds[i]);
		if (!entity) continue;
		// Of another item: what showed it goes (a lifted model freed, a placed one hidden until the next
		// placement).
		const auto found = entities_.find(entity->row);
		if (found != entities_.end()) {
			if (found->second.lifted) {
				if (ObjectModel *model = model_of_(found->second)) model->queue_free();
			} else {
				show_(found->second, false);
			}
		}
		Placed placed;
		placed.item = entity->item;
		placed.stamp = entity->stamp;
		placed.lifted = true;
		if (ObjectModel *model = lift_(*entity)) placed.model = model->get_instance_id();
		entities_[entity->row] = placed;
		if (std::find(lifted_.begin(), lifted_.end(), entity->row) == lifted_.end()) lifted_.push_back(entity->row);
	}
}

void MissionViewportApplier::show_(Placed &placed, bool shown) {
	if (ObjectModel *model = model_of_(placed)) model->set_visible(shown);
	else if (placed.key != 0 && placer_.is_valid()) {
		if (shown) placer_->show_static_instance(placed.key);
		else placer_->hide_static_instance(placed.key);
	}
	placed.hidden = !shown;
}

void MissionViewportApplier::drop_entities_() {
	terrain_->set_static_shadow_placer(Ref<MissionObjectPlacer>());
	for (auto &entry : entities_)
		if (entry.second.lifted)
			if (ObjectModel *model = model_of_(entry.second)) model->queue_free();
	entities_.clear();
	lifted_.clear();
	shadow_pending_.clear();
	// The placed populations and models go with their container at the frame's end, renamed now: a
	// placement begun before then makes a container of its own (the placer reuses one it finds by its
	// name), and nothing of the old one leaves the tree while the frame may still read it.
	for (int i = objects_->get_child_count() - 1; i >= 0; --i) {
		Node *child = objects_->get_child(i);
		child->set_name("Retired");
		child->queue_free();
	}
	placer_.unref();
	warm_.clear();
	placed_ = false;
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
		case Unit::Kind::Items: progress.label = "items"; break;
		case Unit::Kind::Model: progress.label = "models"; break;
		case Unit::Kind::Place: progress.label = "place"; break;
		case Unit::Kind::Lift: progress.label = "lift"; break;
		case Unit::Kind::Pose: progress.label = "pose"; break;
		}
	}
	return progress;
}

// --- the state -----------------------------------------------------------------------------------

void MissionViewportApplier::update(const opennova::editor::ViewportModel &viewport, const opennova::editor::PreviewClock &) {
	apply_state_(viewport);
}

void MissionViewportApplier::move_entities_(const MissionScene &scene) {
	for (const MissionEntityMark &entity : scene.entities()) {
		const auto found = entities_.find(entity.row);
		// One of another item is the next build's to lift.
		if (found == entities_.end() || found->second.item != entity.item) continue;
		Placed &placed = found->second;
		// Back in the scene (a removal undone): shown again, where it stands now.
		const bool back = placed.hidden;
		if (back) show_(placed, true);
		if (placed.stamp == entity.stamp && !back) continue;
		placed.stamp = entity.stamp;
		const Transform3D xform = transform_of_(entity);
		if (ObjectModel *model = model_of_(placed)) model->set_transform(xform);
		else if (placed.key != 0 && placer_.is_valid()) placer_->move_static_instance(placed.key, xform);
		if (!placed.lifted && placed.key != 0 &&
				std::find(shadow_pending_.begin(), shadow_pending_.end(), entity.row) == shadow_pending_.end())
			shadow_pending_.push_back(entity.row);
	}
	// What the scene no longer has: hidden until it comes back or the next placement.
	for (auto &entry : entities_)
		if (!entry.second.hidden && !scene.entity(entry.first)) show_(entry.second, false);
}

void MissionViewportApplier::flush_shadows_(const MissionScene &scene) {
	if (shadow_pending_.empty() || placer_.is_null()) return;
	for (const NodeId row : shadow_pending_) {
		const auto found = entities_.find(row);
		const MissionEntityMark *entity = scene.entity(row);
		if (found == entities_.end() || !entity || found->second.lifted) continue;
		placer_->update_static_terrain_shadow_source_transform(static_cast<MissionData::EntityKind>(found->second.kind),
				found->second.index, transform_of_(*entity));
	}
	shadow_pending_.clear();
}

void MissionViewportApplier::apply_state_(const opennova::editor::ViewportModel &viewport) {
	const MissionViewport &mission = mission_of(viewport);
	const opennova::editor::MissionViewportOptions &options = mission.options();
	move_entities_(mission.scene());
	// The layers the options switch.
	terrain_->set_visible(options.terrain);
	sky_->set_visible(options.sky);
	water_->set_visible(options.water);
	shown_water_ = options.water;
	objects_->set_visible(options.models);
	lifted_root_->set_visible(options.models);
	if (options.shadows != shown_shadows_) {
		shown_shadows_ = options.shadows;
		if (placed_) terrain_->set_static_shadow_placer(shown_shadows_ ? placer_ : Ref<MissionObjectPlacer>());
	}
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
	drop_entities_();
	terrain_->set_terrain_data(Ref<TerrainData>());
	water_->set_terrain_data(Ref<TerrainData>());
	terrain_data_.unref();
	terrain_built_ = false;
	environment_->set_environment_data(Ref<EnvFile>());
	env_file_.unref();
	environment_built_ = false;
	for (std::vector<std::string> &missing : layer_missing_) missing.clear();
	applied_time_ = -2.0;
}

void MissionViewportApplier::apply(const opennova::editor::ViewportModel &viewport, const opennova::editor::PreviewClock &,
		opennova::editor::ViewportDeviceReport &report) {
	report.missing.clear();
	for (const std::vector<std::string> &missing : layer_missing_)
		report.missing.insert(report.missing.end(), missing.begin(), missing.end());
	report.surface = terrain_built_ && terrain_data_.is_valid();
	if (stamped_) report.files = stamped_->stamps();
	if (build_) return;
	apply_state_(viewport);
	// A moved entity's terrain shadow follows once its gesture ended (not each sample: the terrain
	// casts its static shadows again when a source moves).
	const MissionViewport &mission = mission_of(viewport);
	if (!mission.gesture_open()) flush_shadows_(mission.scene());
}

void MissionViewportApplier::tick(const opennova::editor::ViewportModel &, const opennova::editor::PreviewClock &clock) {
	// The models' part animations on the preview clock.
	clock_->sample(int64_t(clock.ms()), ++frame_);
	// The frame's start (the Shell ticks before it arbitrates): no mirror pass unless this frame
	// presents the picture.
	water_->set_mirror_enabled(false);
}

// --- the frame -----------------------------------------------------------------------------------

void MissionViewportApplier::publish_scene_state() {
	// Every global the mission's picture renders with written again, whatever another picture
	// published since: the environment's (the lighting block and the rest), the water's (its active
	// flag, height and plane, pushed as its render activity syncs).
	environment_->republish_shader_globals();
	water_->set_world_rendering_enabled(true);
}

void MissionViewportApplier::present(double dt) {
	// Nothing to draw while the first build runs (the device keeps no picture yet).
	if (build_ && !environment_built_) return;
	water_->set_mirror_enabled(shown_water_);
	// In the game's leg order (game_world_frame.cpp): the camera, the render eye and the clear, the
	// environment nodes, the terrain, the water, the static and the individual models' levels.
	const float eye_y = camera_->get_global_transform().origin.y;
	const bool water_active = water_->is_water_active() && shown_water_;
	environment_->apply_render_eye(eye_y, water_active ? water_->get_water_height() : 0.0f, water_active);
	const bool above = !water_active || eye_y > water_->get_water_height();
	// Godot decodes BG_COLOR from sRGB before writing the scene target: the retail gamma-domain value
	// pre-encoded, as the game's clear colour leg does.
	clear_->get_environment()->set_bg_color(environment_->frame_clear_color_for(above).linear_to_srgb());
	sky_->advance_frame(dt);
	if (terrain_built_ && terrain_data_.is_valid()) {
		terrain_->render_frame();
		water_->set_visible_terrain_bounds(terrain_->has_visible_terrain_bounds(), terrain_->get_visible_terrain_min_height(),
				terrain_->get_visible_terrain_max_height());
	}
	water_->advance_frame(dt);
	const Viewport *viewport = camera_->get_viewport();
	const float width = viewport ? float(viewport->get_visible_rect().size.x) : 0.0f;
	if (placed_ && placer_.is_valid()) placer_->update_static_lods_for_views(camera_, width, nullptr, 0.0f);
	// The individual models' levels (the walk is the process's: the model preview's model draws its
	// level as its options say and never joins it).
	const ObjectLodFrame frames[1] = { ObjectLodFrame::from_camera(camera_, width) };
	ObjectModel::update_authored_lod_views(frames, 1);
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

// --- the read-backs ------------------------------------------------------------------------------

int MissionViewportApplier::placed_count() const {
	int count = 0;
	for (const auto &entry : entities_) count += !entry.second.lifted && !entry.second.hidden ? 1 : 0;
	return count;
}

int MissionViewportApplier::lifted_count() const {
	int count = 0;
	for (const auto &entry : entities_) count += entry.second.lifted && !entry.second.hidden ? 1 : 0;
	return count;
}

int MissionViewportApplier::hidden_count() const {
	int count = 0;
	for (const auto &entry : entities_) count += entry.second.hidden ? 1 : 0;
	return count;
}

int MissionViewportApplier::key_of(NodeId row) const {
	const auto found = entities_.find(row);
	return found == entities_.end() ? 0 : found->second.key;
}

} // namespace godot
