#include "authoring/mission_viewport_applier.h"

#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
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
#include <runtime/world/infantry.h>
#include <runtime/environment/environment_state.h>
#include <runtime/renderer/render_order.h>

#include "env/mission_environment_overrides.h"
#include "mission/mission_data.h"
#include "object/entity_ref.h"
#include "render/frame_fx.h"
#include "render/object_lod_frame.h"
#include "terrain/terrain_tile_info.h"
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

// Each device's scene state is its own: a counter no two share, never 0 (the shipped defaults'), and
// a new one each time what its picture renders with moves.
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

MissionViewportApplier::TerrainKey MissionViewportApplier::terrain_key_of_(const MissionViewport &mission) {
	TerrainKey key;
	key.terrain = mission.scene().header().terrain;
	key.tile_set = mission.scene().header().tile_set;
	// The mission's own name: the game reads <mission>.til beside its terrain.
	key.mission = opennova::to_std(opennova::to_gd(mission.path()).get_file().get_basename());
	return key;
}

int MissionViewportApplier::layer_of_(Unit::Kind kind) {
	switch (kind) {
	case Unit::Kind::Environment:
	case Unit::Kind::Sky: return kEnvironment;
	case Unit::Kind::TerrainFile:
	case Unit::Kind::TerrainBuild:
	case Unit::Kind::NoTerrain:
	case Unit::Kind::Water: return kTerrain;
	case Unit::Kind::DropEntities:
	case Unit::Kind::Items:
	case Unit::Kind::Model:
	case Unit::Kind::Place:
	case Unit::Kind::Shadows:
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
	// The environment and the water hold their process-wide globals from before they enter the tree
	// (their _ready writes some): only this device's publication and its presented frames write
	// them (E13).
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

void MissionViewportApplier::touch_scene_state_() {
	scene_state_ = next_scene_state();
}

// --- the files -----------------------------------------------------------------------------------

bool MissionViewportApplier::mount_(const opennova::editor::SessionView &view) {
	const std::shared_ptr<const opennova::editor::ProjectAssetSource> source = view.findings.assets;
	const bool another = source != mounted_ || !stamped_;
	bool stale[kLayers] = { another, another, another };
	if (!another) {
		if (!stamped_->stamps().moved(*source)) return false;
		// What moved: the layers that read it; a moved file no layer's units read (a texture a model
		// read as it was first drawn) is the entities'.
		stale[kEnvironment] = layer_files_[kEnvironment].moved(*source);
		stale[kTerrain] = layer_files_[kTerrain].moved(*source);
		stale[kEntities] = layer_files_[kEntities].moved(*source) || (!stale[kEnvironment] && !stale[kTerrain]);
	}
	// A fresh record of what is asked for, the root mounted over it (its own caches dropped: a file
	// moved); the layers that read none of what moved keep their files noted. Nothing the picture
	// draws changes here: the layers that read what moved are built again by the build's units.
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
	if (stale[kEntities]) {
		// The placement and the lifted models go in the build's first unit, the placer with them.
		if (placer_.is_valid() || !entities_.empty()) drop_pending_ = true;
		placed_ = false;
	}
	return stale[kTerrain];
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

void MissionViewportApplier::plan_(Build &build, const MissionViewport &mission, std::vector<Unit> carried) {
	const MissionScene &scene = mission.scene();
	const MissionSceneHeader &header = scene.header();
	// The entities of a moved file go first (the frame that took the Rebuild renders the last whole
	// scene; the units after it render nothing until the build ends), the item table read again.
	const bool need_items = drop_pending_ || placer_.is_null();
	if (drop_pending_) build.units.push_back(Unit{ Unit::Kind::DropEntities });
	// The graphics warmed under another cache epoch are cold again.
	if (warm_epoch_ != ResourceRoot::cache_epoch()) {
		warm_.clear();
		warm_epoch_ = ResourceRoot::cache_epoch();
	}
	// The layers whose keys moved: the environment, the terrain (the sky and the water bound to them).
	build.environment = !environment_built_ || !(environment_key_of_(header) == environment_key_);
	const TerrainKey terrain_key = terrain_key_of_(mission);
	build.terrain = !terrain_built_ || !(terrain_key == terrain_key_) || !carried.empty();
	build.terrain_key = terrain_key;
	if (build.environment) {
		layer_missing_[kEnvironment].clear();
		build.units.push_back(Unit{ Unit::Kind::Environment });
	}
	if (!carried.empty()) {
		// The load or the build in flight under this key goes on where it was.
		build.units.insert(build.units.end(), carried.begin(), carried.end());
	} else if (build.terrain) {
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
			// No terrain: the one held dropped by a unit (not as the Rebuild is taken).
			build.units.push_back(Unit{ Unit::Kind::NoTerrain });
		}
	}
	if (build.environment || build.terrain) {
		build.units.push_back(Unit{ Unit::Kind::Sky });
		build.units.push_back(Unit{ Unit::Kind::Water });
	}
	// The entities: placed whole when none are placed (or a placement was cut short); else the rows
	// added since, or given another item, group or attributes, lifted (keys are rows), the lifted set
	// folded back into a whole placement past kLiftedMost.
	bool place = need_items || !placed_;
	std::vector<NodeId> adds;
	if (!place) {
		for (const MissionEntityMark &entity : scene.entities()) {
			const auto found = entities_.find(entity.row);
			if (found == entities_.end() || !places_alike_(found->second, entity)) adds.push_back(entity.row);
		}
		size_t lifted = adds.size();
		for (const NodeId row : lifted_)
			if (std::find(adds.begin(), adds.end(), row) == adds.end()) ++lifted;
		place = lifted > kLiftedMost;
	}
	build.place = place;
	if (need_items) {
		// The item table read first; the graphics it names are planned as it is read.
		layer_missing_[kEntities].clear();
		build.units.push_back(Unit{ Unit::Kind::Items });
	} else {
		// The graphics the entities name, each warmed once: every one for a placement, the adds' for a
		// lift.
		std::vector<Unit> models;
		const auto want = [&](const MissionEntityMark &entity) {
			// A marker draws nothing (the placement places none).
			if (entity.pool == MissionPool::Marker) return;
			const String graphic = graphic_of_(entity.item);
			if (graphic.is_empty() || warm_.count(opennova::to_std(graphic))) return;
			auto unit = std::find_if(models.begin(), models.end(), [&](const Unit &u) { return u.graphic == graphic; });
			if (unit == models.end()) {
				Unit model{ Unit::Kind::Model };
				model.graphic = graphic;
				models.push_back(model);
				unit = models.end() - 1;
			}
			if (std::find(unit->items.begin(), unit->items.end(), entity.item) == unit->items.end())
				unit->items.push_back(entity.item);
		};
		if (place) {
			for (const MissionEntityMark &entity : scene.entities()) want(entity);
		} else {
			for (const NodeId row : adds)
				if (const MissionEntityMark *entity = scene.entity(row)) want(*entity);
		}
		build.units.insert(build.units.end(), models.begin(), models.end());
	}
	if (place) {
		build.units.push_back(Unit{ Unit::Kind::Place });
		build.units.push_back(Unit{ Unit::Kind::Shadows });
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
	// A terrain load or build in flight: its units left, carried into the new plan when its key and
	// its files stand (m6: an edit while a large terrain builds does not begin it again).
	std::vector<Unit> carried;
	TerrainKey carried_key;
	if (build_ && build_->terrain) {
		carried_key = build_->terrain_key;
		for (size_t i = build_->next; i < build_->units.size(); ++i)
			if (build_->units[i].kind == Unit::Kind::TerrainFile || build_->units[i].kind == Unit::Kind::TerrainBuild)
				carried.push_back(build_->units[i]);
		// Only a load begun or a build begun goes on (a plan whose load never began has nothing to carry).
		if (loading_.is_null() && !terrain_->is_building()) carried.clear();
	}
	build_.reset();
	const MissionViewport &mission = mission_of(viewport);
	if (mission.view_status() != opennova::editor::MissionViewStatus::Ready || !view.findings.assets) {
		loading_.unref();
		clear();
		return;
	}
	const bool terrain_moved = mount_(view);
	if (terrain_moved || !(terrain_key_of_(mission) == carried_key)) {
		carried.clear();
		loading_.unref();
	}
	if (carried.empty()) loading_.unref();
	auto build = std::make_unique<Build>();
	plan_(*build, mission, std::move(carried));
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
	(void)failure;
	const MissionViewport &mission = mission_of(viewport);
	const MissionScene &scene = mission.scene();
	const size_t from = reads_();
	switch (unit.kind) {
	case Unit::Kind::DropEntities: drop_entities_(); break;
	case Unit::Kind::Environment: run_environment_(scene); break;
	case Unit::Kind::TerrainFile: run_terrain_file_(build, mission); break;
	case Unit::Kind::TerrainBuild: run_terrain_build_(build); break;
	case Unit::Kind::NoTerrain: terrain_empty_(build.terrain_key); break;
	case Unit::Kind::Sky: sky_->build(); break;
	case Unit::Kind::Water:
		water_->build();
		touch_scene_state_();
		break;
	case Unit::Kind::Items:
		run_items_();
		if (placer_->get_item_db().is_valid()) {
			// The item table read: the graphics the entities name, now that it names them, before the
			// placement.
			std::vector<Unit> models;
			for (const MissionEntityMark &entity : scene.entities()) {
				if (entity.pool == MissionPool::Marker) continue;
				const String graphic = graphic_of_(entity.item);
				if (graphic.is_empty() || warm_.count(opennova::to_std(graphic))) continue;
				auto found = std::find_if(models.begin(), models.end(), [&](const Unit &u) { return u.graphic == graphic; });
				if (found == models.end()) {
					Unit model{ Unit::Kind::Model };
					model.graphic = graphic;
					models.push_back(model);
					found = models.end() - 1;
				}
				if (std::find(found->items.begin(), found->items.end(), entity.item) == found->items.end())
					found->items.push_back(entity.item);
			}
			build.units.insert(build.units.begin() + std::ptrdiff_t(build.next), models.begin(), models.end());
		}
		break;
	case Unit::Kind::Model: run_model_(unit); break;
	case Unit::Kind::Place:
		if (build.place_run.is_null()) run_place_begin_(scene, build);
		run_place_step_(build);
		break;
	case Unit::Kind::Shadows:
		// The placement's static shadows bound to the terrain: the casters' geometry resolved in the
		// Model units, so the snapshot is the sources' walk.
		terrain_->set_static_shadow_placer(shown_shadows_ && placed_ ? placer_ : Ref<MissionObjectPlacer>());
		break;
	case Unit::Kind::Lift: run_lift_(scene, build, unit.first); break;
	case Unit::Kind::Pose:
		// The state as it is now, as an Update applies it (one that came while the build ran is folded
		// into this).
		apply_state_(viewport);
		break;
	}
	note_reads_(layer_of_(unit.kind), from);
	return true;
}

ApplierStep MissionViewportApplier::step(const opennova::editor::ViewportModel &viewport,
		const opennova::editor::PreviewClock &, std::string &failure) {
	picture_moved_();
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
		// The fog as the mission's start settles it, which no weather tick here does (the editor's helper).
		env->set_fog_level(opennova::editor::mission_settled_fog_level(env->get_fog_level()));
		environment_->set_environment_data(env);
		env_file_ = env;
		water_->set_mission_water_height_override(
				overrides->get_has_water_height() ? overrides->get_water_height_world() : NAN);
	}
	// The overcast table beside it where the project has one (the game's, read the same way: by the
	// runtime's own name, env::kOvercastFile, which the import's fixed names list too).
	Ref<EnvFile> overcast;
	overcast.instantiate();
	const String overcast_name = opennova::to_gd(opennova::env::kOvercastFile);
	const bool has_overcast = root_files_->has_file(overcast_name) &&
			overcast->load_from_resource_root(root_files_, overcast_name) == OK;
	environment_->set_overcast_data(has_overcast ? overcast : Ref<EnvFile>());
	environment_->configure_mission_clock(header.start_time, header.minutes_per_day);
	applied_time_ = -2.0;
	environment_key_ = environment_key_of_(header);
	environment_built_ = true;
	touch_scene_state_();
}

void MissionViewportApplier::terrain_empty_(const TerrainKey &key) {
	loading_.unref();
	terrain_->set_terrain_data(Ref<TerrainData>());
	// The ground drawn before goes with it: the picture then has no ground (docs/mcp.md's mission view),
	// never the last terrain under a note that the new one is missing.
	terrain_->clear_built();
	water_->set_terrain_data(Ref<TerrainData>());
	terrain_data_.unref();
	terrain_built_ = true;
	terrain_key_ = key;
	touch_scene_state_();
}

void MissionViewportApplier::run_terrain_file_(Build &build, const MissionViewport &mission) {
	if (loading_.is_null()) return;
	const TerrainData::LoadStep result = loading_->load_step();
	if (result == TerrainData::LOAD_STEP_MORE) return;
	// What the load named and did not find (a texture, a map, the height data): notes.
	for (const std::string &name : loading_->get_load_missing()) note_missing_(kTerrain, opennova::to_gd(name));
	// The rest of the terrain's units go with a load that failed or a build that cannot begin: the
	// layer is empty (no surface, no ground), the picture stands without it.
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
	// The mission's .til, as the game reads it before the build (forced loose first, [orig:
	// Terrain_LoadTileInfoFile @ 0x60a740]; GameWorld::load_mission_tile_info): the tiles it places.
	Ref<TerrainTileInfo> tile_info;
	const String til = opennova::to_gd(build.terrain_key.mission) + ".til";
	if (!build.terrain_key.mission.empty() && root_files_->has_file(til, ResourceRoot::LOOKUP_FORCE_LOOSE_FIRST)) {
		const PackedByteArray bytes = root_files_->read_file(til, ResourceRoot::LOOKUP_FORCE_LOOSE_FIRST);
		Ref<TerrainTileInfo> read;
		read.instantiate();
		if (!bytes.is_empty() && read->load_from_bytes(bytes) == OK) tile_info = read;
	}
	(void)mission;
	terrain_->set_tile_info_override(tile_info);
	terrain_->set_terrain_data(terrain_data_);
	water_->set_terrain_data(terrain_data_);
	terrain_built_ = false;
	touch_scene_state_();
	if (!terrain_->build_begin()) {
		give_up(String());
		return;
	}
	// Its units, a tile each, counted now that they are planned (the one planned stands for the
	// first).
	const int steps = terrain_->get_build_step_count();
	for (int i = 1; i < steps; ++i)
		build.units.insert(build.units.begin() + std::ptrdiff_t(build.next), Unit{ Unit::Kind::TerrainBuild });
}

void MissionViewportApplier::run_terrain_build_(Build &build) {
	const Terrain::BuildStep result = terrain_->build_step();
	if (result == Terrain::BUILD_STEP_FAILED) {
		// A terrain with no tile (no height data): the layer empty, its height data noted missing where
		// the load said so.
		build.units.erase(std::remove_if(build.units.begin() + std::ptrdiff_t(build.next), build.units.end(),
								  [](const Unit &u) { return u.kind == Unit::Kind::TerrainBuild; }),
				build.units.end());
		terrain_empty_(build.terrain_key);
		return;
	}
	if (result == Terrain::BUILD_STEP_DONE) {
		terrain_built_ = true;
		terrain_key_ = build.terrain_key;
	}
}

void MissionViewportApplier::run_items_() {
	// The game's placer over the device's root: its item table read from the project's files.
	placer_ = MissionObjectPlacer::create(root_files_, Ref<ItemDatabase>());
	placer_->set_panm_clock(clock_);
	if (placer_->get_item_db().is_null()) note_missing_(kEntities, "items.def");
}

void MissionViewportApplier::run_model_(const Unit &unit) {
	if (placer_.is_null()) return;
	const Ref<ObjectData> data = placer_->object_data_for(unit.graphic);
	if (data.is_null()) {
		note_missing_(kEntities, unit.graphic.get_file().get_basename() + ".3di");
		warm_[opennova::to_std(unit.graphic)] = false;
		return;
	}
	// Its static batches harvested once where a placement batches an entity of it (an item that
	// needs a node of its own, or a graphic with a live PANM, is an individual model: no batches),
	// so the placement finds every graphic warm; its terrain shadow geometry resolved, so binding the
	// shadows walks the sources and resolves none.
	bool statics = false;
	for (const int64_t item : unit.items) statics = statics || placer_->item_places_static(int(item));
	if (statics) placer_->warm_static_graphic(unit.graphic, objects_);
	terrain_->prepare_static_shadow_caster(unit.graphic, data);
	warm_[opennova::to_std(unit.graphic)] = true;
}

// The placement begun as a run (MissionPlacementRun, D5): its units are the build's Place units, one
// stepped per unit; a newer placement on the placer cancels a run in flight by its generation.
void MissionViewportApplier::run_place_begin_(const MissionScene &scene, Build &build) {
	// What the last placement and the lifts made goes: the run's container is made afresh. Nothing
	// stands placed until the run's last unit (a Rebuild that drops the run places whole again).
	terrain_->set_static_shadow_placer(Ref<MissionObjectPlacer>());
	for (auto &entry : entities_)
		if (entry.second.lifted)
			if (ObjectModel *model = model_of_(entry.second)) model->queue_free();
	entities_.clear();
	origins_.clear();
	lifted_.clear();
	shadow_pending_.clear();
	drop_shapes_();
	picture_moved_();
	placed_ = false;
	place_started_us_ = now_us();
	place_units_planned_ = 1;
	if (placer_.is_null() || placer_->get_item_db().is_null()) {
		// No item table: nothing draws, every entity held as placed with nothing to show.
		for (const MissionEntityMark &entity : scene.entities()) {
			Placed placed;
			placed.item = entity.item;
			placed.group = entity.group;
			placed.attributes = entity.attributes;
			placed.stamp = entity.stamp;
			entities_[entity.row] = placed;
		}
		next_key_ = 0;
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
		// What the game's placement reads beside the transform (placement_rows_of): the group and the
		// attributes, which gate the water mirror and the static terrain shadow.
		row.group = entity.group;
		row.team = entity.team;
		row.ai_flags = entity.attributes;
		row.position = Vector3(float(entity.x), float(entity.y), float(entity.z));
		row.rotation_deg = Vector3(float(entity.pitch), float(entity.yaw), float(entity.roll));
		rows.push_back(row);
		Placed placed;
		placed.item = entity.item;
		placed.group = entity.group;
		placed.attributes = entity.attributes;
		placed.stamp = entity.stamp;
		placed.key = key;
		placed.kind = row.kind;
		placed.index = row.index;
		entities_[entity.row] = placed;
	}
	next_key_ = key;
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
}

ObjectModel *MissionViewportApplier::lift_(const MissionEntityMark &entity, int key) {
	// A marker draws nothing, as the placement places none.
	if (placer_.is_null() || placer_->get_item_db().is_null() || entity.pool == MissionPool::Marker) return nullptr;
	// Built as the placement builds an entity's model (its lighting, its mirror flag and wave from its
	// attributes and its item, its occluders, its static shadow siblings and its terrain shadow
	// source), keyed past every placed key so its shadow source is its own.
	MissionObjectPlacer::PlacementRow row;
	row.kind = kind_of(entity.pool);
	row.index = key;
	row.item_id = int(entity.item);
	row.bms_id = key;
	row.group = entity.group;
	row.team = entity.team;
	row.ai_flags = entity.attributes;
	row.position = Vector3(float(entity.x), float(entity.y), float(entity.z));
	row.rotation_deg = Vector3(float(entity.pitch), float(entity.yaw), float(entity.roll));
	return placer_->build_entity_model(row, lifted_root_, vformat("Lifted_%d", int64_t(entity.row)));
}

void MissionViewportApplier::free_lifted_(const Placed &placed) {
	model_shapes_.erase(placed.model);
	row_shapes_.clear();
	picture_moved_();
	ObjectModel *model = model_of_(placed);
	if (model == nullptr) return;
	if (placer_.is_valid() && placed.key != 0)
		placer_->set_static_terrain_shadow_replacement(placed.key, model->get_graphic_name(), model->get_transform(), false);
	model->queue_free();
}

void MissionViewportApplier::run_lift_(const MissionScene &scene, Build &build, size_t first) {
	for (size_t i = first; i < build.adds.size() && i < first + kLiftPerUnit; ++i) {
		const MissionEntityMark *entity = scene.entity(build.adds[i]);
		if (!entity) continue;
		// What showed it goes: a lifted model freed, a placed one hidden (its record kept: given back
		// what was placed, it shows again).
		const auto found = entities_.find(entity->row);
		if (found != entities_.end()) {
			if (found->second.lifted) {
				free_lifted_(found->second);
			} else {
				show_(found->second, false);
				origins_[entity->row] = found->second;
			}
		}
		// Given back what was placed (an undo): the placed one shown again, moved to where it stands.
		const auto origin = origins_.find(entity->row);
		if (origin != origins_.end() && places_alike_(origin->second, *entity)) {
			Placed placed = origin->second;
			origins_.erase(origin);
			placed.stamp = 0; // moved to where it stands by the pose
			entities_[entity->row] = placed;
			show_(entities_[entity->row], true);
			lifted_.erase(std::remove(lifted_.begin(), lifted_.end(), entity->row), lifted_.end());
			continue;
		}
		Placed placed;
		placed.item = entity->item;
		placed.group = entity->group;
		placed.attributes = entity->attributes;
		placed.stamp = entity->stamp;
		placed.lifted = true;
		placed.key = ++next_key_;
		placed.kind = kind_of(entity->pool);
		placed.index = placed.key;
		if (ObjectModel *model = lift_(*entity, placed.key)) placed.model = model->get_instance_id();
		entities_[entity->row] = placed;
		if (std::find(lifted_.begin(), lifted_.end(), entity->row) == lifted_.end()) lifted_.push_back(entity->row);
	}
}

void MissionViewportApplier::show_(Placed &placed, bool shown) {
	if (ObjectModel *model = model_of_(placed)) {
		model->set_visible(shown);
		// Its terrain shadow with it (an individual model's source answers to its key).
		if (placer_.is_valid() && placed.key != 0) {
			if (shown) placer_->clear_static_terrain_shadow_replacement(placed.key);
			else placer_->set_static_terrain_shadow_replacement(placed.key, model->get_graphic_name(), model->get_transform(), false);
		}
	} else if (placed.key != 0 && placer_.is_valid()) {
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
	origins_.clear();
	lifted_.clear();
	shadow_pending_.clear();
	drop_shapes_();
	picture_moved_();
	// The placed populations and models go with their container at the frame's end, renamed now: a
	// placement begun before then makes a container of its own (the placer reuses one it finds by its
	// name). This runs in a build's unit, whose frame renders nothing (the last picture stands).
	for (int i = objects_->get_child_count() - 1; i >= 0; --i) {
		Node *child = objects_->get_child(i);
		child->set_name("Retired");
		child->queue_free();
	}
	placer_.unref();
	warm_.clear();
	placed_ = false;
	drop_pending_ = false;
	next_key_ = 0;
}

opennova::editor::OperationProgress MissionViewportApplier::progress() const {
	opennova::editor::OperationProgress progress;
	progress.unit = opennova::editor::OperationUnit::Steps;
	if (!build_) return progress;
	progress.done = build_->next;
	progress.total = build_->units.size();
	if (build_->next < build_->units.size()) {
		switch (build_->units[build_->next].kind) {
		case Unit::Kind::DropEntities: progress.label = "drop"; break;
		case Unit::Kind::Environment: progress.label = "environment"; break;
		case Unit::Kind::TerrainFile: progress.label = "terrain files"; break;
		case Unit::Kind::TerrainBuild:
		case Unit::Kind::NoTerrain: progress.label = "terrain"; break;
		case Unit::Kind::Sky: progress.label = "sky"; break;
		case Unit::Kind::Water: progress.label = "water"; break;
		case Unit::Kind::Items: progress.label = "items"; break;
		case Unit::Kind::Model: progress.label = "models"; break;
		case Unit::Kind::Place: progress.label = "place"; break;
		case Unit::Kind::Shadows: progress.label = "shadows"; break;
		case Unit::Kind::Lift: progress.label = "lift"; break;
		case Unit::Kind::Pose: progress.label = "pose"; break;
		}
	}
	return progress;
}

// --- the state -----------------------------------------------------------------------------------

void MissionViewportApplier::update(const opennova::editor::ViewportModel &viewport, const opennova::editor::PreviewClock &) {
	picture_moved_();
	apply_state_(viewport);
}

namespace {

// How far a posed person stands over its record (where its spawn stands it, metres; 0 for anyone else).
double lift_of(const opennova::editor::MissionPoses &poses, NodeId row) {
	const opennova::editor::MissionPose *pose = poses.pose(row);
	return pose != nullptr && pose->status == "posed" ? pose->lift : 0.0;
}

} // namespace

void MissionViewportApplier::move_entities_(const MissionScene &scene, const opennova::editor::MissionPoses &poses) {
	for (const MissionEntityMark &entity : scene.entities()) {
		const auto found = entities_.find(entity.row);
		// One of another item, group or attributes is the next build's to lift.
		if (found == entities_.end() || !places_alike_(found->second, entity)) continue;
		Placed &placed = found->second;
		// Back in the scene (a removal undone): shown again, where it stands now.
		const bool back = placed.hidden;
		if (back) show_(placed, true);
		if (placed.stamp == entity.stamp && !back) continue;
		placed.stamp = entity.stamp;
		const Transform3D xform = transform_of_(entity);
		if (ObjectModel *model = model_of_(placed)) {
			// A person stands where its spawn stands it (its pose's lift; the mission's z up is the picture's y).
			model->set_transform(xform.translated(Vector3(0.0f, float(lift_of(poses, entity.row)), 0.0f)));
		} else if (placed.key != 0 && placer_.is_valid()) {
			placer_->move_static_instance(placed.key, xform);
		}
		if (placed.key != 0 && std::find(shadow_pending_.begin(), shadow_pending_.end(), entity.row) == shadow_pending_.end())
			shadow_pending_.push_back(entity.row);
	}
	// What the scene no longer has: hidden until it comes back or the next placement.
	for (auto &entry : entities_)
		if (!entry.second.hidden && !scene.entity(entry.first)) show_(entry.second, false);
}

void MissionViewportApplier::pose_people_(const MissionScene &scene, const opennova::editor::MissionPoses &poses) {
	for (auto &entry : entities_) {
		Placed &placed = entry.second;
		const opennova::editor::MissionPose *pose = poses.pose(entry.first);
		const MissionEntityMark *entity = scene.entity(entry.first);
		if (pose == nullptr || entity == nullptr || pose->status != "posed" || placed.model == 0) continue;
		if (placed.posed_model == placed.model && placed.pose_stamp == pose->stamp) continue;
		ObjectModel *model = model_of_(placed);
		if (model == nullptr) continue;
		// Where the spawn stands it: its record lifted by the warmup and its ground solve.
		model->set_transform(transform_of_(*entity).translated(Vector3(0.0f, float(pose->lift), 0.0f)));
		// The body channel as the game's presenter dispatches a person's row: the playing clip at its
		// playhead, or the outgoing clip blended under it at the target's weight while the blend runs
		// (EntityPresenter's body leg over the PF_ANIM_* fields the present rows carry from the same
		// world::InfantryBodyPose).
		const opennova::world::InfantryBodyPose &body = pose->pose;
		const String key = opennova::to_gd(opennova::world::infantry_anim_key(body.state));
		if (body.blending && body.source_state >= 0 && body.weight < 1.0f)
			model->play_body_blend_at(opennova::to_gd(opennova::world::infantry_anim_key(body.source_state)),
					body.source_phase, key, body.phase, body.weight, body.source_variant, body.variant);
		else
			model->play_body_clip_at(key, body.phase, body.variant, body.parked);
		placed.posed_model = placed.model;
		placed.pose_stamp = pose->stamp;
	}
}

void MissionViewportApplier::flush_shadows_(const MissionScene &scene) {
	if (shadow_pending_.empty() || placer_.is_null()) return;
	for (const NodeId row : shadow_pending_) {
		const auto found = entities_.find(row);
		const MissionEntityMark *entity = scene.entity(row);
		if (found == entities_.end() || !entity) continue;
		// A placed entity's source by its pool and index at the placement, a lifted one's by its key.
		placer_->update_static_terrain_shadow_source_transform(static_cast<MissionData::EntityKind>(found->second.kind),
				found->second.index, transform_of_(*entity));
	}
	shadow_pending_.clear();
}

void MissionViewportApplier::apply_state_(const opennova::editor::ViewportModel &viewport) {
	const MissionViewport &mission = mission_of(viewport);
	const opennova::editor::MissionViewportOptions &options = mission.options();
	move_entities_(mission.scene(), mission.poses());
	pose_people_(mission.scene(), mission.poses());
	// The layers the options switch.
	terrain_->set_visible(options.terrain);
	sky_->set_visible(options.sky);
	if (options.water != shown_water_) touch_scene_state_(); // the water's globals with it
	water_->set_visible(options.water);
	shown_water_ = options.water;
	objects_->set_visible(options.models);
	lifted_root_->set_visible(options.models);
	if (options.shadows != shown_shadows_) {
		shown_shadows_ = options.shadows;
		// The casters' geometry is resolved (the Model units): the snapshot walks the sources.
		if (placed_) terrain_->set_static_shadow_placer(shown_shadows_ ? placer_ : Ref<MissionObjectPlacer>());
	}
	// The time of day: the mission's start time, or the option's hour.
	if (options.time != applied_time_ && environment_built_) {
		if (options.time >= 0.0) environment_->debug_set_mission_minute_of_day(options.time * 60.0);
		else environment_->configure_mission_clock(environment_key_.start_time, environment_key_.minutes_per_day);
		applied_time_ = options.time;
		touch_scene_state_();
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
	terrain_->clear_built(); // nothing it drew stands
	water_->set_terrain_data(Ref<TerrainData>());
	terrain_data_.unref();
	terrain_built_ = false;
	environment_->set_environment_data(Ref<EnvFile>());
	env_file_.unref();
	environment_built_ = false;
	for (std::vector<std::string> &missing : layer_missing_) missing.clear();
	applied_time_ = -2.0;
	touch_scene_state_();
}

void MissionViewportApplier::apply(const opennova::editor::ViewportModel &viewport, const opennova::editor::PreviewClock &,
		opennova::editor::ViewportDeviceReport &report) {
	picture_moved_();
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
	// flag, height and plane, pushed as its render activity syncs). The only writes outside a frame
	// this device presents.
	environment_->republish_shader_globals();
	water_->set_globals_held(false);
	water_->set_world_rendering_enabled(true);
	water_->set_globals_held(true);
}

void MissionViewportApplier::present(double dt) {
	// Nothing to draw while the first build runs (the device keeps no picture yet).
	if (build_ && !environment_built_) return;
	// Its own frame: what its legs write of the globals is its state's (the arbitration published it).
	environment_->set_globals_held(false);
	water_->set_globals_held(false);
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
	environment_->set_globals_held(true);
	water_->set_globals_held(true);
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

// --- the pick ------------------------------------------------------------------------------------

namespace {

// Whether the segment from `a` to `b` meets `box` at a parameter below `limit` (the slabs).
bool segment_meets_box(const AABB &box, const Vector3 &a, const Vector3 &b, real_t limit) {
	real_t low = 0.0f, high = limit;
	const Vector3 along = b - a;
	const Vector3 end = box.position + box.size;
	for (int axis = 0; axis < 3; ++axis) {
		if (Math::abs(along[axis]) < CMP_EPSILON) {
			if (a[axis] < box.position[axis] || a[axis] > end[axis]) return false;
			continue;
		}
		real_t t0 = (box.position[axis] - a[axis]) / along[axis], t1 = (end[axis] - a[axis]) / along[axis];
		if (t0 > t1) std::swap(t0, t1);
		low = MAX(low, t0);
		high = MIN(high, t1);
		if (low > high) return false;
	}
	return true;
}

// The parameter of the segment from `a` to `b` where it meets the triangle (either face), in [0, 1].
bool segment_meets_triangle(const Vector3 &a, const Vector3 &b, const Vector3 &v0, const Vector3 &v1, const Vector3 &v2,
		real_t &at) {
	const Vector3 along = b - a;
	const Vector3 e1 = v1 - v0, e2 = v2 - v0;
	const Vector3 p = along.cross(e2);
	const real_t det = e1.dot(p);
	if (Math::abs(det) < 1e-12f) return false;
	const real_t inverse = 1.0f / det;
	const Vector3 t = a - v0;
	const real_t u = t.dot(p) * inverse;
	if (u < 0.0f || u > 1.0f) return false;
	const Vector3 q = t.cross(e1);
	const real_t v = along.dot(q) * inverse;
	if (v < 0.0f || u + v > 1.0f) return false;
	at = e2.dot(q) * inverse;
	return at >= 0.0f && at <= 1.0f;
}

void add_faces(MissionViewportApplier::PickShape &shape, const PackedVector3Array &faces, const Transform3D &through) {
	for (int64_t i = 0; i < faces.size(); ++i) {
		const Vector3 at = through.xform(faces[i]);
		if (!shape.any) {
			shape.box = AABB(at, Vector3());
			shape.any = true;
		} else {
			shape.box.expand_to(at);
		}
		shape.faces.push_back(at);
	}
}

// Every mesh a node draws under `node` (its levels and parts), through its transform from `node`.
void model_faces(Node *node, const Transform3D &into, MissionViewportApplier::PickShape &shape) {
	for (int i = 0; i < node->get_child_count(); ++i) {
		Node *child = node->get_child(i);
		Node3D *spatial = Object::cast_to<Node3D>(child);
		const Transform3D through = spatial ? into * spatial->get_transform() : into;
		if (MeshInstance3D *mesh = Object::cast_to<MeshInstance3D>(child))
			if (mesh->get_mesh().is_valid()) add_faces(shape, mesh->get_mesh()->get_faces(), through);
		model_faces(child, through, shape);
	}
}

} // namespace

const MissionViewportApplier::PickShape *MissionViewportApplier::pick_shape_(opennova::editor::NodeId row, const Placed &placed,
		Transform3D &xform) const {
	if (placed.hidden) return nullptr;
	RowShape &kept = row_shapes_[row];
	const bool alike = kept.shape != nullptr && kept.item == placed.item && kept.key == placed.key && kept.model == placed.model;
	if (placed.model != 0) {
		// An individual model: its meshes through its nodes, in its own space; it where its node stands.
		ObjectModel *model = model_of_(placed);
		if (model == nullptr || model->is_queued_for_deletion() || !model->is_visible()) return nullptr;
		xform = model->get_transform();
		for (Node *up = model->get_parent(); up != nullptr && up != root_; up = up->get_parent())
			if (Node3D *spatial = Object::cast_to<Node3D>(up)) xform = spatial->get_transform() * xform;
		if (alike) return kept.shape;
		auto found = model_shapes_.find(placed.model);
		if (found == model_shapes_.end()) {
			PickShape shape;
			model_faces(model, Transform3D(), shape);
			found = model_shapes_.emplace(placed.model, std::move(shape)).first;
		}
		if (!found->second.any) return nullptr;
		kept = RowShape{ placed.item, placed.key, placed.model, &found->second };
		return kept.shape;
	}
	if (placed.key == 0 || placer_.is_null()) return nullptr;
	// A static: its graphic's finest level, where its rows draw it now.
	const Variant at = placer_->get_static_instance_transform(placed.key);
	if (at.get_type() != Variant::TRANSFORM3D) return nullptr;
	xform = at;
	if (alike) return kept.shape;
	const String graphic = placer_->graphic_for(int(placed.item));
	if (graphic.is_empty()) return nullptr;
	const std::string key = graphic.utf8().get_data();
	auto found = static_shapes_.find(key);
	if (found == static_shapes_.end()) {
		PickShape shape;
		add_faces(shape, placer_->get_static_graphic_faces(graphic), Transform3D());
		// Not warm yet: nothing kept, asked again.
		if (!shape.any) return nullptr;
		found = static_shapes_.emplace(key, std::move(shape)).first;
	}
	kept = RowShape{ placed.item, placed.key, placed.model, &found->second };
	return kept.shape;
}

opennova::editor::ViewportRayHit MissionViewportApplier::ray_between(const double from[3], const double to[3]) const {
	opennova::editor::ViewportRayHit out;
	if (!placed_ || placer_.is_null()) return out; // Unknown: the placement does not stand yet
	if (ray_kept_ && std::equal(from, from + 3, ray_from_) && std::equal(to, to + 3, ray_to_)) return ray_hit_;
	const Vector3 start = MissionObjectPlacer::bms_to_godot_position(Vector3(float(from[0]), float(from[1]), float(from[2])));
	const Vector3 end = MissionObjectPlacer::bms_to_godot_position(Vector3(float(to[0]), float(to[1]), float(to[2])));
	const real_t length = (end - start).length();
	// The terrain's point along the segment, then the nearest face before it.
	real_t best = 1.0f;
	bool ground = false;
	if (terrain_built_ && terrain_data_.is_valid() && length > 0.0f) {
		const Vector3 hit = terrain_data_->raycast_terrain(start, end);
		if (hit.is_finite()) {
			best = (hit - start).length() / length;
			ground = true;
		}
	}
	opennova::editor::NodeId row = 0;
	for (const auto &entry : entities_) {
		Transform3D xform;
		const PickShape *shape = pick_shape_(entry.first, entry.second, xform);
		if (shape == nullptr) continue;
		const Transform3D inverse = xform.affine_inverse();
		const Vector3 a = inverse.xform(start), b = inverse.xform(end);
		if (!segment_meets_box(shape->box, a, b, best)) continue;
		const Vector3 *faces = shape->faces.ptr();
		for (int64_t i = 0; i + 2 < shape->faces.size(); i += 3) {
			real_t at = 0.0f;
			if (segment_meets_triangle(a, b, faces[i], faces[i + 1], faces[i + 2], at) && at < best) {
				best = at;
				row = entry.first;
			}
		}
	}
	if (row != 0 || ground) {
		out.met = row != 0 ? opennova::editor::ViewportRayHit::Met::Record : opennova::editor::ViewportRayHit::Met::Surface;
		out.row = row;
		const Vector3 mission = MissionObjectPlacer::godot_to_bms_position(start + (end - start) * best);
		out.point[0] = mission.x;
		out.point[1] = mission.y;
		out.point[2] = mission.z;
	} else {
		out.met = opennova::editor::ViewportRayHit::Met::Nothing;
	}
	ray_kept_ = true;
	std::copy(from, from + 3, ray_from_);
	std::copy(to, to + 3, ray_to_);
	ray_hit_ = out;
	return out;
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
