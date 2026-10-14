#include "authoring/mission_viewport_applier.h"

#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/image.hpp>
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
#include <editor/preview/mission_scene.h>
#include <editor/preview/mission_viewport.h>
#include <editor/preview/preview_clock.h>
#include <editor/preview/preview_pick.h>
#include <editor/preview/viewport_device.h>
#include <editor/session/view/session_view.h>
#include <formats/env/env.h>
#include <formats/env/env_weather.h>
#include <runtime/world/infantry.h>
#include <runtime/environment/environment_state.h>
#include <runtime/environment/weather_runtime.h>
#include <runtime/renderer/render_order.h>

#include <runtime/mission/mission_sidecars.h>
#include <runtime/mission/placement_traits.h>
#include <godot_cpp/variant/packed_vector4_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "env/mission_environment_overrides.h"
#include "lights/effect_light_report.h"
#include "mission/mission_data.h"
#include "mission/static_source_convert.h"
#include "object/entity_ref.h"
#include "object/item_database.h"
#include "particle/particle_renderer.h"
#include "render/frame_fx.h"
#include "render/object_lod_frame.h"
#include "render/scene_overlay_compositor.h"
#include "terrain/foliage_frame_stats.h"
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

} // namespace

MissionViewportApplier::MissionViewportApplier(SubViewport &viewport) :
		layers_(std::make_unique<PreviewSceneLayers>(viewport, "Mission", true)) {
	// Its camera is the 3D listener of its own world (DI-36: the Listen's channels pan about it, as the game's camera).
	viewport.set_as_audio_listener_3d(true);
	Node3D &root = layers_->root();
	listen_ = std::make_unique<PreviewSoundLoops>(&root);
	const WorldSceneNodes &nodes = layers_->nodes();
	environment_ = nodes.environment;
	water_ = nodes.water;
	weather_ = nodes.weather;
	terrain_ = nodes.terrain;
	terrain_id_ = terrain_->get_instance_id();
	foliage_ = nodes.foliage;
	slot_shadow_ = nodes.slot_shadow;
	camera_ = layers_->camera();
	effects_ = &layers_->effects();
	// The picture shows the mission as a load composes it: an edit that changes a caster's static
	// shadow (a NoShadow set or cleared, an entity moved, removed or brought back) composes again the
	// pages that shadow touched and touches, which the game, keeping a composed page, never asks.
	terrain_->set_static_shadow_follows_casters(true);
	// The placer's parent (its "MissionObjects" container) and the lifted models'.
	objects_ = memnew(Node3D);
	objects_->set_name("Objects");
	root.add_child(objects_);
	lifted_root_ = memnew(Node3D);
	lifted_root_->set_name("Lifted");
	root.add_child(lifted_root_);
	// The game's light director over the picture's statics and its models (no world behind it).
	lights_.instantiate();
	lights_->setup_with_provider(nullptr, &sources_);
	lights_->set_scene(TypedArray<ObjectModel>(), environment_, weather_);
	clock_.instantiate();
	// The Shoot tool's scars (DI-23), drawn by the game's ScarPresenter (its effects are the items' scene's).
	shot_scars_ = memnew(ScarPresenter);
	shot_scars_->set_name("ShotScars");
	root.add_child(shot_scars_);
}

MissionViewportApplier::~MissionViewportApplier() {
	// The lights let go of the models they followed; the terrain of the placer and the light pool while both
	// stand (the device frees its SubViewport after).
	lights_->reset();
	if (Terrain *terrain = Object::cast_to<Terrain>(ObjectDB::get_instance(terrain_id_))) {
		terrain->set_static_shadow_placer(Ref<MissionObjectPlacer>());
		terrain->set_light_context(Ref<LightScene>(), 0);
	}
}

// --- the picture's statics (DI-31) ---------------------------------------------------------------

std::vector<opennova::mission::StaticEffectSource> MissionViewportApplier::PictureSources::static_item_effect_sources() {
	if (placer_.is_null()) return {};
	std::vector<opennova::mission::StaticEffectSource> sources = placer_->static_item_effect_sources();
	for (opennova::mission::StaticEffectSource &source : sources) {
		if (source.bms_id == 0) continue;
		// A removed entity's static: no model, so nothing of it spawns.
		if (placer_->is_static_instance_hidden(source.bms_id)) {
			source.asset_id = 0;
			continue;
		}
		// Where its rows draw it now (a move rewrites the rows alone).
		const Variant at = placer_->get_static_instance_transform(source.bms_id);
		if (at.get_type() == Variant::TRANSFORM3D) source.world_transform = to_static_source_transform(at);
	}
	return sources;
}

std::vector<opennova::mission::StaticLightDrawSource> MissionViewportApplier::PictureSources::static_light_draw_sources() {
	return placer_.is_valid() ? placer_->static_light_draw_sources() : std::vector<opennova::mission::StaticLightDrawSource>();
}

uint64_t MissionViewportApplier::PictureSources::static_light_draw_source_revision() {
	return placer_.is_valid() ? placer_->static_light_draw_source_revision() : 0;
}

Ref<ItemDatabase> MissionViewportApplier::PictureSources::static_source_item_db() {
	return placer_.is_valid() ? placer_->get_item_db() : Ref<ItemDatabase>();
}

Ref<ObjectData> MissionViewportApplier::PictureSources::static_source_object_data(uint64_t asset_id) const {
	return placer_.is_valid() && asset_id != 0 ? placer_->static_source_object_data(asset_id) : Ref<ObjectData>();
}

// --- the files -----------------------------------------------------------------------------------

void MissionViewportApplier::mount_(const opennova::editor::SessionView &view) {
	// A moved file no layer's units read (a texture a model read as it was first drawn) is the
	// entities'.
	const PreviewSceneLayers::Moved moved = layers_->mount(view, PreviewSceneLayers::kEntities);
	// A wave the Listen decoded may be one that moved: decoded again as it is next played.
	if (moved.layer[PreviewSceneLayers::kEnvironment] ||
			moved.layer[PreviewSceneLayers::kTerrain] || moved.layer[PreviewSceneLayers::kEntities])
		listen_->forget();
	if (moved.layer[PreviewSceneLayers::kEntities]) {
		// The placement and the lifted models go in the build's first unit, the placer with them.
		if (placer_.is_valid() || !entities_.empty()) drop_pending_ = true;
		placed_ = false;
	}
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

std::vector<MissionViewportApplier::Unit> MissionViewportApplier::model_units_(
		const std::vector<const MissionEntityMark *> &entities) {
	// Each graphic the entities name, once, with the items that draw it
	// (editor::mission_models_to_warm: a marker none), a graphic warmed already left out.
	std::vector<Unit> units;
	for (opennova::editor::MissionWarmModel &model : opennova::editor::mission_models_to_warm(
				 entities, [this](int64_t item) { return opennova::to_std(graphic_of_(item)); },
				 [this](const std::string &graphic) { return warm_.count(graphic) != 0; })) {
		Unit unit{ Unit::Kind::Model };
		unit.graphic = opennova::to_gd(model.graphic);
		unit.items = std::move(model.items);
		units.push_back(std::move(unit));
	}
	return units;
}

void MissionViewportApplier::plan_(Build &build, const MissionViewport &mission) {
	const MissionScene &scene = mission.scene();
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
	PreviewSceneLayers::Source source;
	source.header = scene.header();
	// The mission's own name: the game reads <mission>.til beside its terrain, the name cut at its
	// first dot (mission::mission_base_name); its .env, which the terrain's load reads after the
	// .trn (D-TERRAIN-18).
	source.mission = opennova::mission::mission_base_name(mission.path());
	source.environment =
			source.header.environment.empty() ? std::string() : source.header.environment + ".env";
	const PreviewSceneLayers::Plan layers = layers_->plan(source);
	// The heights a new load reads hold every sculpt patch the viewport made so far (S25), and its
	// maps and tiles every paint's and tile edit's.
	if (layers.loads) {
		terrain_patches_.built(mission.heights_built_serial());
		terrain_patches_.maps_built(mission.maps_built_serial());
	}
	if (layers_->building())
		build.units.push_back(Unit{ Unit::Kind::Layers });
	// A new environment shows the time the options show again.
	if (layers.environment)
		applied_time_ = -2.0;
	// The entities: placed whole when none are placed (or a placement was cut short); else the rows
	// added since, or given another item, group or attributes, lifted (keys are rows), the lifted
	// set folded back into a whole placement past kLiftedMost (editor::mission_place_or_lift).
	const opennova::editor::MissionPlacePlan plan = opennova::editor::mission_place_or_lift(
			scene.entities(), need_items || !placed_,
			[this](const MissionEntityMark &entity) {
				const auto found = entities_.find(entity.row);
				return found != entities_.end() && places_alike_(found->second, entity);
			},
			lifted_, kLiftedMost);
	const bool place = plan.place;
	build.place = place;
	if (need_items) {
		// The item table read first; the graphics it names are planned as it is read.
		layers_->clear_missing(PreviewSceneLayers::kEntities);
		build.units.push_back(Unit{ Unit::Kind::Items });
	} else {
		// The graphics the entities name, each warmed once: every one for a placement, the adds' for a
		// lift.
		std::vector<const MissionEntityMark *> named;
		if (place) {
			for (const MissionEntityMark &entity : scene.entities())
				named.push_back(&entity);
		} else {
			for (const NodeId row : plan.adds)
				named.push_back(scene.entity(row));
		}
		const std::vector<Unit> models = model_units_(named);
		build.units.insert(build.units.end(), models.begin(), models.end());
	}
	if (place) {
		build.units.push_back(Unit{ Unit::Kind::Place });
		build.units.push_back(Unit{ Unit::Kind::Shadows });
	} else {
		build.adds = plan.adds;
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
	project_root_ = view.project.root;
	const MissionViewport &mission = mission_of(viewport);
	if (mission.view_status() != opennova::editor::MissionViewStatus::Ready ||
			!view.findings.assets) {
		clear();
		return;
	}
	// The effects' graphics through the project's files (mounted again where one they read moved, DI-31).
	effects_->mount(view.findings.assets);
	effects_mounted_ = true;
	mount_(view);
	auto build = std::make_unique<Build>();
	plan_(*build, mission);
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
	const size_t from = layers_->reads();
	switch (unit.kind) {
		case Unit::Kind::DropEntities:
			drop_entities_();
			break;
		case Unit::Kind::Layers:
			(void)layers_->step();
			return true; // its reads noted to its own layers
		case Unit::Kind::Items:
			run_items_();
			if (placer_->get_item_db().is_valid()) {
				// The item table read: the graphics the entities name, now that it names them,
				// before the placement.
				std::vector<const MissionEntityMark *> named;
				for (const MissionEntityMark &entity : scene.entities())
					named.push_back(&entity);
				const std::vector<Unit> models = model_units_(named);
				build.units.insert(build.units.begin() + std::ptrdiff_t(build.next), models.begin(),
						models.end());
			}
			break;
		case Unit::Kind::Model:
			run_model_(unit);
			break;
		case Unit::Kind::Place:
			if (build.place_run.is_null())
				run_place_begin_(scene, build);
			run_place_step_(build);
			break;
		case Unit::Kind::Shadows:
			// The placement's static shadows bound to the terrain: the casters' geometry resolved
			// in the Model units, so the snapshot is the sources' walk.
			terrain_->set_static_shadow_placer(
					shown_shadows_ && placed_ ? placer_ : Ref<MissionObjectPlacer>());
			break;
		case Unit::Kind::Lift:
			run_lift_(scene, build, unit.first);
			break;
		case Unit::Kind::Pose:
			// The state as it is now, as an Update applies it (one that came while the build ran is
			// folded into this).
			apply_state_(viewport);
			break;
	}
	layers_->note_reads(PreviewSceneLayers::kEntities, from);
	return true;
}

ApplierStep MissionViewportApplier::step(const opennova::editor::ViewportModel &viewport,
		const opennova::editor::PreviewClock &, std::string &failure) {
	picture_moved_();
	Build &build = *build_;
	// The layers' units, one a step, before the entities' that follow them.
	if (build.units[build.next].kind == Unit::Kind::Layers && layers_->building()) {
		if (layers_->step())
			return ApplierStep::More;
		++build.next;
		if (build.next < build.units.size())
			return ApplierStep::More;
		build_.reset();
		return ApplierStep::Built;
	}
	const Unit unit = build.units[build.next++];
	if (!run_(build, unit, viewport, failure)) {
		build_.reset();
		return ApplierStep::Failed;
	}
	if (build.next < build.units.size()) return ApplierStep::More;
	build_.reset();
	return ApplierStep::Built;
}

void MissionViewportApplier::run_items_() {
	// The game's placer over the device's root: its item table read from the project's files.
	placer_ = MissionObjectPlacer::create(layers_->files(), Ref<ItemDatabase>());
	placer_->set_panm_clock(clock_);
	if (placer_->get_item_db().is_null())
		layers_->note_missing(PreviewSceneLayers::kEntities, "items.def");
}

void MissionViewportApplier::run_model_(const Unit &unit) {
	if (placer_.is_null()) return;
	const Ref<ObjectData> data = placer_->object_data_for(unit.graphic);
	if (data.is_null()) {
		layers_->note_missing(PreviewSceneLayers::kEntities,
				opennova::to_std(unit.graphic.get_file().get_basename() + ".3di"));
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
	// The mission's start spawns the placed entities' lights (DI-31).
	relight_pending_ = true;
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
	// What stands moved: the lights spawned again over it.
	relight_pending_ = true;
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
	relight_pending_ = true;
}

void MissionViewportApplier::drop_entities_() {
	terrain_->set_static_shadow_placer(Ref<MissionObjectPlacer>());
	people_serial_ = UINT64_MAX;
	// The husks go with what they stood in for (an individual one's with its intact model, a graft here).
	for (auto &entry : husks_)
		if (entry.second.intact == 0)
			if (ObjectModel *model = Object::cast_to<ObjectModel>(
						ObjectDB::get_instance(entry.second.graft.husk)))
				model->queue_free();
	husks_.clear();
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
	// The lights let go of the models and statics that went.
	relight_pending_ = true;
}

opennova::editor::OperationProgress MissionViewportApplier::progress() const {
	opennova::editor::OperationProgress progress;
	progress.unit = opennova::editor::OperationUnit::Steps;
	if (!build_) return progress;
	// The layers' units count among the build's, in the place of the one that steps them.
	for (size_t i = 0; i < build_->units.size(); ++i) {
		const bool layers = build_->units[i].kind == Unit::Kind::Layers;
		const size_t weight = layers ? layers_->units_planned() : 1;
		progress.total += weight;
		if (i < build_->next)
			progress.done += weight;
		else if (i == build_->next && layers)
			progress.done += layers_->units_done();
	}
	if (build_->next < build_->units.size()) {
		switch (build_->units[build_->next].kind) {
			case Unit::Kind::DropEntities:
				progress.label = "drop";
				break;
			case Unit::Kind::Layers:
				progress.label = layers_->step_label();
				break;
			case Unit::Kind::Items:
				progress.label = "items";
				break;
			case Unit::Kind::Model:
				progress.label = "models";
				break;
			case Unit::Kind::Place:
				progress.label = "place";
				break;
			case Unit::Kind::Shadows:
				progress.label = "shadows";
				break;
			case Unit::Kind::Lift:
				progress.label = "lift";
				break;
			case Unit::Kind::Pose:
				progress.label = "pose";
				break;
		}
	}
	return progress;
}

// --- the state -----------------------------------------------------------------------------------

void MissionViewportApplier::update(const opennova::editor::ViewportModel &viewport, const opennova::editor::PreviewClock &) {
	picture_moved_();
	// S25 (the heights): a stroke's regions and a landing's tiles into the terrain built, never building it again;
	// taken once the terrain is built where a build runs.
	const Ref<TerrainData> &data = layers_->terrain_data();
	const bool standing =
			layers_->terrain_built() && !build_ && data.is_valid() && !terrain_->is_building();
	if (standing &&
			!terrain_patches_.apply(mission_of(viewport).heights_patches(), **data, *terrain_))
		UtilityFunctions::push_warning("The mission view's terrain did not take a sculpt's patch: it builds again.");
	// S25 (the maps and the tiles): a paint's regions and the placed tiles into the terrain built, in place, each
	// file's read taking the stamp it was patched at (so its move does not build the terrain again); one not taken is
	// reported and the picture made again.
	if (standing) {
		terrain_patches_.apply_maps(mission_of(viewport).maps_patches(), *terrain_,
				layers_->foliage_configured() ? foliage_ : nullptr,
				[this](const std::string &name, uint64_t stamp) { layers_->restamp(name, stamp); });
		// The foliage keeps candidates off the tiles the terrain draws (a new override where it held none).
		if (layers_->foliage_configured() &&
				foliage_->get_tile_info() != terrain_->get_tile_info_override())
			foliage_->set_tile_info(terrain_->get_tile_info_override());
	}
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

void MissionViewportApplier::pose_people_(const MissionScene &scene, const opennova::editor::MissionPoses &poses,
		const opennova::editor::MissionPeople &people) {
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
		// Its body as it plays now (S23 C), its spawn's before it plays.
		const opennova::world::InfantryBodyPose *body = people.pose(entry.first);
		pose_body_(*model, body ? *body : pose->pose);
		placed.posed_model = placed.model;
		placed.pose_stamp = pose->stamp;
	}
}

void MissionViewportApplier::pose_body_(ObjectModel &model, const opennova::world::InfantryBodyPose &body) {
	// The body channel as the game's presenter dispatches a person's row: the playing clip at its
	// playhead, or the outgoing clip blended under it at the target's weight while the blend runs
	// (EntityPresenter's body leg over the PF_ANIM_* fields the present rows carry from the same
	// world::InfantryBodyPose).
	const String key = opennova::to_gd(opennova::world::infantry_anim_key(body.state));
	if (body.blending && body.source_state >= 0 && body.weight < 1.0f)
		model.play_body_blend_at(opennova::to_gd(opennova::world::infantry_anim_key(body.source_state)),
				body.source_phase, key, body.phase, body.weight, body.source_variant, body.variant);
	else
		model.play_body_clip_at(key, body.phase, body.variant, body.parked);
}

void MissionViewportApplier::play_people_(const opennova::editor::MissionViewport &mission) {
	// The people as they play their clips on the clock (S23 C): each posed person's body as it stands now, again
	// whenever they played on.
	const opennova::editor::MissionPeople &people = mission.people();
	if (people.serial() == people_serial_) return;
	people_serial_ = people.serial();
	for (auto &entry : entities_) {
		Placed &placed = entry.second;
		const opennova::world::InfantryBodyPose *body = people.pose(entry.first);
		if (body == nullptr || placed.model == 0 || placed.posed_model != placed.model) continue;
		if (ObjectModel *model = model_of_(placed)) pose_body_(*model, *body);
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
	// The moved entities' lights where they stand now (the game's are spawn-fixed: spawned again).
	relight_pending_ = true;
}

void MissionViewportApplier::relight_() {
	relight_pending_ = false;
	sources_.bind(placed_ ? placer_ : Ref<MissionObjectPlacer>());
	// The individual models shown: the placed ones and the lifted ones.
	TypedArray<ObjectModel> models;
	for (const auto &entry : entities_) {
		if (entry.second.hidden) continue;
		if (ObjectModel *model = model_of_(entry.second))
			if (!model->is_queued_for_deletion() && model->is_inside_tree()) models.push_back(model);
	}
	lights_->set_scene(models, environment_, weather_);
	lights_dirty_ = true;
	// Each LGHT record spawned as the mission's start spawns it (the original's Game_StartMission @ 0x525d19 ->
	// Game_SpawnAllEntityGlowEffects @ 0x5227b0 -> Entity_SpawnGlowEffects @ 0x56c7c0, cited in engine/); none with the layer
	// off (the statics' atlas lanes are published either way).
	if (shown_lights_ && placed_) lights_->reattach();
	else lights_->reset();
}

void MissionViewportApplier::apply_state_(const opennova::editor::ViewportModel &viewport) {
	const MissionViewport &mission = mission_of(viewport);
	const opennova::editor::MissionViewportOptions &options = mission.options();
	move_entities_(mission.scene(), mission.poses());
	pose_people_(mission.scene(), mission.poses(), mission.people());
	// The layers the options switch.
	PreviewSceneLayers::Shown shown;
	shown.terrain = options.terrain;
	shown.sky = options.sky;
	shown.water = options.water;
	shown.foliage = options.foliage;
	layers_->set_shown(shown);
	shown_water_ = options.water;
	objects_->set_visible(options.models);
	lifted_root_->set_visible(options.models);
	// DI-31's layers: the foliage hidden with its pools, the effects drawn or not (the viewport
	// closes their scene), the lights spawned again or let go.
	shown_foliage_ = options.foliage;
	shown_effects_ = options.effects;
	if (options.lights != shown_lights_) {
		shown_lights_ = options.lights;
		relight_pending_ = true;
	}
	if (options.shadows != shown_shadows_) {
		shown_shadows_ = options.shadows;
		// The casters' geometry is resolved (the Model units): the snapshot walks the sources.
		if (placed_) terrain_->set_static_shadow_placer(shown_shadows_ ? placer_ : Ref<MissionObjectPlacer>());
	}
	// The ground overlay (DI-29): the picture the viewport made of what the game reads at each point, laid
	// over the terrain from above (presentation x east, z the negated mission y), past it the one value the
	// game reads all round where there is one.
	// S25: a stroke's patch of it the rows it changed alone, the terrain's texture taken in place (GroundOverlayUpload).
	if (mission.overlay_serial() != overlay_serial_) {
		overlay_serial_ = mission.overlay_serial();
		overlay_upload_.apply(*terrain_, mission.overlay());
	}
	// The time of day: the mission's start time, or the option's hour (the clock standing there).
	if (options.time != applied_time_ && layers_->environment_built()) {
		layers_->show_time(options.time >= 0.0 ? options.time * 60.0 : -1.0);
		applied_time_ = options.time;
	}
	place_camera_(viewport);
}

void MissionViewportApplier::place_camera_(const opennova::editor::ViewportModel &viewport) {
	layers_->place_camera(mission_of(viewport).camera());
}

void MissionViewportApplier::clear() {
	build_.reset();
	listen_->stop();
	drop_entities_();
	relight_();
	effects_->clear();
	effects_->set_environment_source(environment_);
	effects_mounted_ = false;
	shot_scars_->clear();
	shot_scars_shown_ = UINT64_MAX;
	layers_->clear();
	applied_time_ = -2.0;
}

void MissionViewportApplier::apply(const opennova::editor::ViewportModel &viewport, const opennova::editor::PreviewClock &,
		opennova::editor::ViewportDeviceReport &report) {
	picture_moved_();
	layers_->report(report);
	// The effects' graphics read (one that moves builds the picture again) and those the project lacks.
	if (effects_mounted_) {
		report.files.add(effects_->stamps());
		for (const std::string &name : effects_->missing())
			if (std::find(report.missing.begin(), report.missing.end(), name) == report.missing.end())
				report.missing.push_back(name);
	}
	report.drawn = drawn_json_();
	report.heights_taken = terrain_patches_.taken();
	report.maps_taken = terrain_patches_.maps_taken();
	report.maps_refused = terrain_patches_.take_maps_refused();
	report.heights_failed = terrain_patches_.take_failure();
	if (build_) return;
	apply_state_(viewport);
	// A moved entity's terrain shadow follows once its gesture ended (not each sample: the terrain
	// casts its static shadows again when a source moves).
	const MissionViewport &mission = mission_of(viewport);
	if (!mission.gesture_open()) flush_shadows_(mission.scene());
	// The lights spawned again where what stands moved (the state's, not a frame's: drawn or not).
	if (relight_pending_) relight_();
}

opennova::io::JsonValue MissionViewportApplier::drawn_json_() const {
	using opennova::io::json_number;
	using opennova::io::JsonValue;
	JsonValue out = JsonValue::make_object();
	// How many frames it presented (a client's measure of the picture's rate) and what the last one's three
	// legs cost on the main thread, microseconds.
	out.set("frames", json_number(double(presented_)));
	JsonValue legs = JsonValue::make_object();
	legs.set("foliage", json_number(double(leg_us_[0])));
	legs.set("lights", json_number(double(leg_us_[1])));
	legs.set("effects", json_number(double(leg_us_[2])));
	out.set("leg_us", std::move(legs));
	// S25: the ground overlay's last upload (its rows and its cost: the painting's frame cost).
	out.set("overlay", overlay_upload_.to_json());
	// The foliage the last presented frame drew: its definitions' slots that draw, the detail cells about the
	// eye, the instances and vertices, the batches.
	out.set("foliage", layers_->foliage_json());
	// The lights: the pool's live lights, the models and the statics' rows the last frame lit, its coronas.
	JsonValue lights = JsonValue::make_object();
	lights.set("shown", JsonValue::make_bool(shown_lights_));
	const Ref<EffectLightReport> report = lights_->scene()->get_report();
	lights.set("pool", json_number(double(report.is_valid() ? report->get_live() : 0)));
	lights.set("models", json_number(double(report.is_valid() ? report->get_models() : 0)));
	lights.set("lit_models", json_number(double(report.is_valid() ? report->get_lit_models() : 0)));
	lights.set("static_draws", json_number(double(report.is_valid() ? report->get_static_draws() : 0)));
	lights.set("lit_static_draws", json_number(double(report.is_valid() ? report->get_lit_static_draws() : 0)));
	lights.set("coronas", json_number(double(lights_->scene()->last_corona_quads().size())));
	out.set("lights", std::move(lights));
	// The entities' moving ground shadows (S23 C): the casters the slot device registered, the slots it bound a
	// drape to and those it captured, and the captures it armed in the last presented frame.
	JsonValue shadows = JsonValue::make_object();
	shadows.set("shown", JsonValue::make_bool(shown_shadows_));
	const Dictionary slot_report = slot_shadow_->get_report();
	shadows.set("registered", json_number(double(int64_t(slot_report.get("registered", 0)))));
	shadows.set("bound", json_number(double(int64_t(slot_report.get("bound", 0)))));
	shadows.set("captures", json_number(double(int64_t(slot_report.get("captures", 0)))));
	shadows.set("armed", json_number(double(int64_t(slot_report.get("armed", 0)))));
	out.set("shadows", std::move(shadows));
	// The effects' quads the renderer drew.
	JsonValue effects = JsonValue::make_object();
	effects.set("shown", JsonValue::make_bool(shown_effects_));
	const ParticleRenderer *renderer = effects_->renderer();
	effects.set("quads", json_number(double(renderer ? renderer->get_rendered_quad_count() : 0)));
	out.set("effects", std::move(effects));
	// The Listen's voices (DI-36): those it holds, those sounding, those whose wave still decodes or did not decode,
	// how many it started in all, and whether it is heard (the picture drawn).
	JsonValue listen = JsonValue::make_object();
	listen.set("voices", json_number(listen_->voices()));
	listen.set("playing", json_number(listen_->playing()));
	listen.set("decoding", json_number(listen_->decoding()));
	listen.set("failed", json_number(listen_->failed()));
	listen.set("started", json_number(double(listen_->started())));
	listen.set("audible", JsonValue::make_bool(listen_->audible()));
	out.set("listen", std::move(listen));
	// The environment the picture draws under, as the game's render diagnostics word it (S26 B12):
	// its time, its light, its fog, its water, the bodies, the cube, the weather's ticks.
	out.set("environment", layers_->environment_json());
	return out;
}

void MissionViewportApplier::tick(const opennova::editor::ViewportModel &viewport, const opennova::editor::PreviewClock &clock) {
	// The models' part animations on the preview clock.
	clock_->sample(int64_t(clock.ms()), ++frame_);
	// A paint's colour map, once its strokes pause, into the texture the foliage's tint reads (S25).
	if (layers_->terrain_built())
		terrain_patches_.settle_colour(*terrain_, Time::get_singleton()->get_ticks_msec());
	clock_ms_ = int64_t(clock.ms());
	// The frame's start (the Shell ticks before it arbitrates): no mirror pass unless this frame
	// presents the picture.
	water_->set_mirror_enabled(false);
	// The weather on the preview clock's game ticks (DI-31: the lights' FLICKER and the foliage's
	// sway read its oscillator): the mission's start settle first, then a tick per game tick (a
	// seek back holds it), its clock standing at the hour the options show.
	layers_->follow_clock(clock.ticks());
	// The effects' scene the viewport stepped, shown where it opened another.
	const std::shared_ptr<opennova::particle::EffectScene> &scene = mission_of(viewport).effects().scene();
	if (effects_mounted_ && scene != effects_->scene()) effects_->show(scene);
	apply_shots_(mission_of(viewport));
	if (!build_) {
		apply_husks_(mission_of(viewport), clock.ticks());
		play_people_(mission_of(viewport));
	}
	apply_listen_(mission_of(viewport));
}

void MissionViewportApplier::unhusk_(Husk &husk) {
	// The intact entity as it stood (simulation/husk_swap: its parts shown, a carved static's rows
	// back, its terrain shadow its own again), the husk model freed.
	husk_graft_restore(placer_.ptr(), husk.graft);
	if (ObjectModel *model =
					Object::cast_to<ObjectModel>(ObjectDB::get_instance(husk.graft.husk))) {
		model->set_visible(false);
		model->queue_free();
	}
	husk = Husk();
	relight_pending_ = true;
	picture_moved_();
}

void MissionViewportApplier::apply_husks_(const MissionViewport &mission, int32_t tick) {
	// What the clock's tick husks: each destroyed item past its swap (none with no placement standing).
	std::unordered_map<NodeId, opennova::editor::MissionHuskFrame> due;
	if (placed_ && placer_.is_valid() && !mission.shots().shots().empty())
		for (const opennova::editor::MissionShotDeath &death : mission.shots().deaths()) {
			opennova::editor::MissionHuskFrame frame = opennova::editor::mission_husk_frame(death, tick);
			if (frame.husked) due[death.row] = std::move(frame);
		}
	// A husk no longer due, or whose entity stands otherwise now (lifted, hidden, placed again): let go.
	for (auto it = husks_.begin(); it != husks_.end();) {
		const auto now = due.find(it->first);
		const auto placed = entities_.find(it->first);
		const Husk &held = it->second;
		const bool stands = now != due.end() && now->second.husk == held.husk &&
				placed != entities_.end() && !placed->second.hidden &&
				placed->second.model == held.intact &&
				(held.intact != 0 || placed->second.key == held.graft.bms_id) &&
				ObjectDB::get_instance(held.graft.husk) != nullptr;
		if (stands) {
			++it;
			continue;
		}
		unhusk_(it->second);
		it = husks_.erase(it);
	}
	for (const auto &entry : due) {
		const NodeId row = entry.first;
		const opennova::editor::MissionHuskFrame &frame = entry.second;
		const auto found = entities_.find(row);
		if (found == entities_.end() || found->second.hidden) continue;
		const Placed &placed = found->second;
		Husk &husk = husks_[row];
		ObjectModel *model = Object::cast_to<ObjectModel>(ObjectDB::get_instance(husk.graft.husk));
		if (model == nullptr) {
			// Swapped in through the game's own swap (simulation/husk_swap, the destruction
			// presenter's): an individual model's husk under it, its parts hidden; a retained
			// static carved out of its batches and the husk grafted at its placed transform; either
			// way the husk keeps the entity's terrain shadow, mirror flag, projection and occlusion
			// masks.
			const String graphic = opennova::to_gd(frame.husk);
			husk = Husk();
			husk.husk = frame.husk;
			HuskGraftResult result = HuskGraftResult::NotCarved;
			if (ObjectModel *intact = model_of_(placed)) {
				result = husk_graft_individual(**placer_, *intact, graphic, placed.key, husk.graft);
				husk.intact = intact->get_instance_id();
			} else if (placed.key != 0) {
				result = husk_graft_static(
						**placer_, *lifted_root_, placed.key, graphic, husk.graft);
			}
			model = result == HuskGraftResult::Grafted
					? Object::cast_to<ObjectModel>(ObjectDB::get_instance(husk.graft.husk))
					: nullptr;
			if (model == nullptr) {
				husks_.erase(row);
				continue;
			}
			relight_pending_ = true;
			picture_moved_();
		}
		// The destroy fade's registers and the sections the pieces left, as the game writes them each draw.
		if (frame.ctrl != husk.ctrl) {
			model->begin_ctrl_update();
			for (const auto &held : husk.ctrl)
				if (frame.ctrl.find(held.first) == frame.ctrl.end()) model->clear_ctrl_value(opennova::to_gd(held.first));
			for (const auto &now : frame.ctrl) model->set_ctrl_value(opennova::to_gd(now.first), now.second);
			model->end_ctrl_update();
			husk.ctrl = frame.ctrl;
		}
		if (frame.hidden_sections != husk.sections) {
			model->set_destroyed_section_mask(int64_t(frame.hidden_sections));
			husk.sections = frame.hidden_sections;
		}
	}
}

void MissionViewportApplier::apply_shots_(const opennova::editor::MissionViewport &mission) {
	if (mission.shots().shots().empty() || !effects_mounted_) {
		if (shot_scars_shown_ != UINT64_MAX) shot_scars_->clear();
		shot_scars_shown_ = UINT64_MAX;
		return;
	}
	// The scars where the run moved, their textures through the project's files as the effects read theirs.
	if (mission.shots().serial() == shot_scars_shown_) return;
	shot_scars_shown_ = mission.shots().serial();
	shot_scars_->set_resource_root(effects_->root());
	// Every ring the shots wrote, made world-space by the run: the device's space, no entity rings to resolve.
	ScarDrawList::CompiledFrame scar_frame;
	scar_frame.mission_space = false;
	if (mission.shots().scar_count() > 0) shot_scars_->present(ScarDrawList::from_compiled(mission.shot_scars(), scar_frame), Dictionary());
	else shot_scars_->clear();
}

void MissionViewportApplier::apply_listen_(const opennova::editor::MissionViewport &mission) {
	// The Listen's channels (DI-36) as the viewport's mix binds them now, heard while the picture is drawn.
	const opennova::editor::MissionListen &listen = mission.listen();
	listen_->listen(project_root_, listen.channels(), mission.options().listen.on && listen.open() && !project_root_.empty(),
			presented_);
}

// --- the frame -----------------------------------------------------------------------------------

void MissionViewportApplier::publish_scene_state() {
	// Every global the mission's picture renders with written again, whatever another picture
	// published since (the layers': the environment's, the water's, the weather's, the cube's, the
	// veil, the foliage masks). The only writes outside a frame this device presents.
	layers_->publish_scene_state();
	// The retained statics' light atlas (its rows' lights and lighting lanes), when one is built.
	lights_->scene()->publish_static_rows();
}

void MissionViewportApplier::present(double dt) {
	// Nothing to draw while the first build runs (the device keeps no picture yet).
	if (build_ && !layers_->environment_built())
		return;
	// Its own frame: what its legs write of the globals is its state's (the arbitration published
	// it). In the game's leg order (game_world_frame.cpp): the scene environment and the
	// environment nodes, the terrain, the water, the foliage, the sun veil's feed, then the
	// device's lights and levels, the slot shadows, the particles, the drops, the overlay tail, the
	// frame effects, the clear and the cube.
	layers_->begin_frame();
	layers_->present_environment(dt);
	++presented_;
	const int64_t foliage_start = now_us();
	layers_->present_ground(dt, clock_ms_);
	leg_us_[0] = now_us() - foliage_start;
	layers_->present_sun_veil();
	// The light leg (GameWorld::render_light_frame): the pool spawned again where what stands moved, each
	// model's and each static row's nearest lights selected (the atlas's lanes with them), the terrain's
	// next frame drawn with the lights its patches overlap, the coronas walked.
	const int64_t lights_start = now_us();
	if (relight_pending_ && !build_) relight_();
	// With the layer off the pool is empty: one pass after a relight or a change of the statics' rows writes
	// every draw's lights away and the rows' lanes, and nothing moves until the next.
	const uint64_t rows_revision = sources_.static_light_draw_source_revision();
	if (shown_lights_ || lights_dirty_ || rows_revision != lit_rows_revision_) {
		lights_->render_frame(camera_, clock_ms_, TypedArray<ObjectModel>(), false);
		lights_dirty_ = false;
		lit_rows_revision_ = rows_revision;
	}
	terrain_->set_light_context(shown_lights_ ? lights_->scene() : Ref<LightScene>(), int(clock_ms_));
	leg_us_[1] = now_us() - lights_start;
	// The levels at the object detail a fresh game profile starts at (the game reads its options'
	// setting; renderer::kObjectLodDetailFreshProfile), as the model viewport's auto level does.
	const Viewport *viewport = camera_->get_viewport();
	const float width = viewport ? float(viewport->get_visible_rect().size.x) : 0.0f;
	constexpr int object_detail = opennova::renderer::kObjectLodDetailFreshProfile;
	if (placed_ && placer_.is_valid()) {
		placer_->update_static_lods_for_views(camera_, width, nullptr, 0.0f, object_detail);
	}
	// The individual models' levels (the walk is the process's: the model preview's model draws its
	// level as its options say and never joins it).
	const ObjectLodFrame frames[1] = { ObjectLodFrame::from_camera(camera_, width, object_detail) };
	ObjectModel::update_authored_lod_views(frames, 1);
	// The slot-shadow leg (GameWorld::render_slot_shadow_frame, after the light leg has handed it the pool, as
	// game_world_frame.cpp's light leg does): the entities' moving ground shadows planned and their captures
	// published, the drape drawn with the terrain's pass. With the shadows off, or no terrain drawn, the drape
	// hides and nothing plans.
	const bool slot_shadows = shown_shadows_ && placed_ && layers_->terrain_built() &&
			layers_->terrain_data().is_valid() && terrain_->is_visible();
	slot_shadow_->set_terrain_pass_drawn(slot_shadows);
	if (slot_shadows) {
		slot_shadow_->set_light_director(lights_);
		slot_shadow_->set_light_context(lights_->light_gain(), int(clock_ms_), weather_);
		slot_shadow_->advance_frame();
	}
	// The particle leg: the effects' scene as the viewport stepped it, partitioned by the water's
	// plane; the drops; the post-particle overlay tail (the drops, the coronas, the murk, the glint
	// and the glare, the mirror's).
	const int64_t effects_start = now_us();
	layers_->present_particles(clock_ms_);
	layers_->present_drops();
	layers_->present_overlay(shown_lights_ ? lights_.ptr() : nullptr);
	leg_us_[2] = now_us() - effects_start;
	layers_->present_frame_effects();
	layers_->present_clear_and_cube();
	layers_->end_frame();
}

// --- the ground ----------------------------------------------------------------------------------

bool MissionViewportApplier::ground_at(double x, double y, double &height) const {
	return layers_->ground_at(x, y, height);
}

bool MissionViewportApplier::surface_between(const double from[3], const double to[3], double point[3]) const {
	return layers_->surface_between(from, to, point);
}

bool MissionViewportApplier::surface_at(float x, float y, float point[3]) const {
	// Along the applied camera's ray, as far as a pick reaches.
	const Ref<TerrainData> &data = layers_->terrain_data();
	if (!layers_->terrain_built() || data.is_null() || camera_->get_viewport() == nullptr)
		return false;
	const Vector3 from = camera_->project_ray_origin(Vector2(x, y));
	const Vector3 along = camera_->project_ray_normal(Vector2(x, y));
	const Vector3 hit =
			data->raycast_terrain(from, from + along * float(opennova::editor::kMissionPickReach));
	if (!hit.is_finite()) return false;
	const Vector3 mission = MissionObjectPlacer::godot_to_bms_position(hit);
	point[0] = mission.x;
	point[1] = mission.y;
	point[2] = mission.z;
	return true;
}

// --- the pick ------------------------------------------------------------------------------------

namespace {

opennova::editor::PreviewVec3 to_preview(const Vector3 &v) {
	return opennova::editor::PreviewVec3{ v.x, v.y, v.z };
}

void add_faces(MissionViewportApplier::PickShape &shape, const PackedVector3Array &faces, const Transform3D &through) {
	for (int64_t i = 0; i < faces.size(); ++i) {
		const Vector3 at = through.xform(faces[i]);
		const opennova::editor::PreviewVec3 point = to_preview(at);
		if (!shape.any) {
			shape.low = shape.high = point;
			shape.any = true;
		} else {
			shape.low = opennova::editor::PreviewVec3{ std::min(shape.low.x, point.x),
				std::min(shape.low.y, point.y), std::min(shape.low.z, point.z) };
			shape.high = opennova::editor::PreviewVec3{ std::max(shape.high.x, point.x),
				std::max(shape.high.y, point.y), std::max(shape.high.z, point.z) };
		}
		shape.faces.push_back(point);
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
		for (Node *up = model->get_parent(); up != nullptr && up != &layers_->root();
				up = up->get_parent())
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
	float best = 1.0f;
	bool ground = false;
	if (layers_->terrain_built() && layers_->terrain_data().is_valid() && length > 0.0f) {
		const Vector3 hit = layers_->terrain_data()->raycast_terrain(start, end);
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
		// The segment in the shape's own space, through the editor's pick (preview/preview_pick).
		const Transform3D inverse = xform.affine_inverse();
		if (opennova::editor::segment_nearest_face(shape->faces.data(), shape->faces.size(),
					shape->low, shape->high, to_preview(inverse.xform(start)),
					to_preview(inverse.xform(end)), best))
			row = entry.first;
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
