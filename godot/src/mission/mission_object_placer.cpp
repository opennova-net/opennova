#include <runtime/renderer/render_order.h>
#include "mission/mission_object_placer.h"
#include "mission/static_source_convert.h"
#include "util/string_convert.h"

#include <formats/mission/mission.h> // the entity read the native place() walks
#include "util/axes.h"
#include "mission/static_population_instance.h"
#include "render/frame_fx.h"
#include "render/object_lod_frame.h"
#include "render/visual_layers.h"

#include <algorithm>
#include <cmath>

#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/object.hpp>

#include <runtime/anim/adm_fallback.h> // the default.adm substitution every spawn applies
#include <runtime/renderer/object_lod.h>
#include <base/io/fixed.h>
#include <runtime/world/model_geometry.h>
#include <runtime/world/entity.h>
#include <runtime/world/person_overlays.h> // kParachuteItemTypeId

#include "env/water.h"
#include "mission/mission_data.h"
#include "mission/mission_object_placer_keys.h"
#include "mission/mission_placement_run.h"

using namespace opennova::threedi;

namespace godot {

namespace {

constexpr const char *kContainerName = "MissionObjects";

} // namespace

Ref<MissionObjectPlacer> MissionObjectPlacer::create(
		const Ref<ResourceRoot> &p_root, const Ref<ItemDatabase> &p_db) {
	Ref<MissionObjectPlacer> placer;
	placer.instantiate();
	placer->set_resource_root(p_root);
	placer->set_item_db(p_db);
	return placer;
}

void MissionObjectPlacer::_bind_methods() {
	ClassDB::bind_static_method("MissionObjectPlacer",
			D_METHOD("create", "resource_root", "item_db"),
			&MissionObjectPlacer::create);
	ClassDB::bind_method(D_METHOD("set_resource_root", "root"),
			&MissionObjectPlacer::set_resource_root);
	ClassDB::bind_method(D_METHOD("get_resource_root"),
			&MissionObjectPlacer::get_resource_root);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "resource_root",
						 PROPERTY_HINT_RESOURCE_TYPE, "ResourceRoot"),
			"set_resource_root", "get_resource_root");
	ClassDB::bind_method(D_METHOD("set_item_db", "db"),
			&MissionObjectPlacer::set_item_db);
	ClassDB::bind_method(D_METHOD("get_item_db_property"),
			&MissionObjectPlacer::get_item_db_property);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "item_db",
						 PROPERTY_HINT_RESOURCE_TYPE, "ItemDatabase"),
			"set_item_db", "get_item_db_property");
	ClassDB::bind_method(D_METHOD("set_avatar_db", "db"),
			&MissionObjectPlacer::set_avatar_db);
	ClassDB::bind_method(D_METHOD("set_panm_clock", "clock"),
			&MissionObjectPlacer::set_panm_clock);
	ClassDB::bind_method(D_METHOD("get_item_db"),
			&MissionObjectPlacer::get_item_db);

	ClassDB::bind_static_method("MissionObjectPlacer",
			D_METHOD("bms_to_godot_position", "p"),
			&MissionObjectPlacer::bms_to_godot_position);
	ClassDB::bind_static_method("MissionObjectPlacer",
			D_METHOD("godot_to_bms_position", "p"),
			&MissionObjectPlacer::godot_to_bms_position);
	ClassDB::bind_static_method("MissionObjectPlacer",
			D_METHOD("bms_to_godot_basis", "rot_deg"),
			&MissionObjectPlacer::bms_to_godot_basis);
	ClassDB::bind_static_method("MissionObjectPlacer",
			D_METHOD("entity_transform", "position", "rotation_deg"),
			&MissionObjectPlacer::entity_transform);
	ClassDB::bind_static_method("MissionObjectPlacer",
			D_METHOD("item_casts_dynamic_shadow", "item_type", "attrib2"),
			&MissionObjectPlacer::item_casts_dynamic_shadow);
	ClassDB::bind_static_method("MissionObjectPlacer",
			D_METHOD("item_casts_static_terrain_shadow", "kind",
					"entity_attrib", "item_attrib", "item_attrib2"),
			&MissionObjectPlacer::item_casts_static_terrain_shadow);

	ClassDB::bind_method(D_METHOD("place", "mission", "parent", "options"),
			&MissionObjectPlacer::place, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("begin_place", "mission", "parent", "options"),
			&MissionObjectPlacer::begin_place, DEFVAL(Dictionary()));
	ClassDB::bind_method(
			D_METHOD("update_static_lods", "camera_transform",
					"vertical_fov_degrees", "viewport_width", "viewport_height"),
			&MissionObjectPlacer::update_static_lods);
	ClassDB::bind_method(D_METHOD("set_static_instance_occlusion_hidden", "bms_id", "hidden"),
			&MissionObjectPlacer::set_static_instance_occlusion_hidden);
	ClassDB::bind_method(D_METHOD("clear_static_instance_occlusion"),
			&MissionObjectPlacer::clear_static_instance_occlusion);
	ClassDB::bind_method(D_METHOD("get_static_instance_lod", "bms_id"),
			&MissionObjectPlacer::get_static_instance_lod);
	ClassDB::bind_method(D_METHOD("update_static_lods_for_views", "main_camera", "main_width",
								 "inset_camera", "inset_width"),
			&MissionObjectPlacer::update_static_lods_for_views);
	ClassDB::bind_method(D_METHOD("get_static_instance_inset_lod", "bms_id"),
			&MissionObjectPlacer::get_static_instance_inset_lod);
	ClassDB::bind_method(
			D_METHOD("get_static_instance_live_populations", "bms_id"),
			&MissionObjectPlacer::get_static_instance_live_populations);
	ClassDB::bind_method(
			D_METHOD("get_static_population_live_bms_ids", "population"),
			&MissionObjectPlacer::get_static_population_live_bms_ids);
	ClassDB::bind_method(D_METHOD("get_static_live_population_count"),
			&MissionObjectPlacer::get_static_live_population_count);
	ClassDB::bind_method(
			D_METHOD("resolve_player_visual_item_id", "runtime_type_id"),
			&MissionObjectPlacer::resolve_player_visual_item_id);
	ClassDB::bind_method(
			D_METHOD("resolve_player_visual_spec", "runtime_type_id",
					"character_id"),
			&MissionObjectPlacer::resolve_player_visual_spec);
	ClassDB::bind_method(
			D_METHOD("build_player_animated_model", "runtime_type_id", "parent",
					"character_id"),
			&MissionObjectPlacer::build_player_animated_model, DEFVAL(0));
	ClassDB::bind_method(
			D_METHOD("build_model_from_graphic", "graphic", "adm_name",
					"parent", "clip_key", "rig_graphic", "retain_authored_lods"),
			&MissionObjectPlacer::build_model_from_graphic, DEFVAL(String()),
			DEFVAL(String()), DEFVAL(false));

	ClassDB::bind_method(D_METHOD("get_placed_models"),
			&MissionObjectPlacer::get_placed_models);
	ClassDB::bind_method(D_METHOD("set_placed_models", "models"),
			&MissionObjectPlacer::set_placed_models);
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "placed_models", PROPERTY_HINT_ARRAY_TYPE,
						 "ObjectModel"),
			"set_placed_models", "get_placed_models");
	ClassDB::bind_method(D_METHOD("get_static_instance_binding_count", "bms_id"),
			&MissionObjectPlacer::get_static_instance_binding_count);
	ClassDB::bind_method(
			D_METHOD("get_static_terrain_shadow_source_revision"),
			&MissionObjectPlacer::get_static_terrain_shadow_source_revision);
	ClassDB::bind_method(D_METHOD("object_data_for", "graphic"),
			&MissionObjectPlacer::object_data_for);

	ClassDB::bind_method(D_METHOD("get_static_instance_batch_key", "bms_id"),
			&MissionObjectPlacer::get_static_instance_batch_key);
	ClassDB::bind_method(
			D_METHOD("static_instance_is_mirror_reflected", "bms_id"),
			&MissionObjectPlacer::static_instance_is_mirror_reflected);
	ClassDB::bind_method(
			D_METHOD("register_static_instance", "bms_id", "graphic", "index",
					"xform", "casts_static_shadow", "mirror_reflected"),
			&MissionObjectPlacer::register_static_instance, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("is_static_instance_hidden", "bms_id"),
			&MissionObjectPlacer::is_static_instance_hidden);
	ClassDB::bind_method(D_METHOD("hide_static_instance", "bms_id"),
			&MissionObjectPlacer::hide_static_instance);
	ClassDB::bind_method(D_METHOD("show_static_instance", "bms_id"),
			&MissionObjectPlacer::show_static_instance);
	ClassDB::bind_method(D_METHOD("move_static_instance", "bms_id", "xform"),
			&MissionObjectPlacer::move_static_instance);
	ClassDB::bind_method(D_METHOD("get_static_instance_transform", "bms_id"),
			&MissionObjectPlacer::get_static_instance_transform);
	ClassDB::bind_method(D_METHOD("item_entity_transform", "position", "rotation_deg", "item_id"),
			&MissionObjectPlacer::item_entity_transform);
	ClassDB::bind_method(D_METHOD("warm_static_graphic", "graphic", "tree_parent"),
			&MissionObjectPlacer::warm_static_graphic);
	ClassDB::bind_method(
			D_METHOD("register_resolved_static_graphic", "graphic", "data",
					"batches", "lod_profile"),
			&MissionObjectPlacer::register_resolved_static_graphic,
			DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("register_object_data", "graphic", "data"),
			&MissionObjectPlacer::register_object_data);
	ClassDB::bind_method(
			D_METHOD("register_occlusion_verdict", "item_id", "has_occlusion"),
			&MissionObjectPlacer::register_occlusion_verdict);

	ClassDB::bind_integer_constant(get_class_static(), "", "RENDER_LOD",
			RENDER_LOD);
	ClassDB::bind_integer_constant(get_class_static(), "",
			"PLAYER_RUNTIME_TYPE_ID", PLAYER_RUNTIME_TYPE_ID);
	ClassDB::bind_integer_constant(get_class_static(), "",
			"PLAYER_VISUAL_ITEM_ID", PLAYER_VISUAL_ITEM_ID);
}

// --- wiring -----------------------------------------------------------------

void MissionObjectPlacer::set_resource_root(const Ref<ResourceRoot> &p_root) {
	resource_root_ = p_root;
	avatar_db_.unref();
}

void MissionObjectPlacer::set_item_db(const Ref<ItemDatabase> &p_db) {
	item_db_ = p_db;
}

void MissionObjectPlacer::set_avatar_db(const Ref<AvatarDatabase> &p_db) {
	avatar_db_ = p_db;
}

void MissionObjectPlacer::set_panm_clock(const Ref<PanmClock> &p_clock) {
	panm_clock_ = p_clock;
}

Ref<ItemDatabase> MissionObjectPlacer::get_item_db() {
	_ensure_item_db();
	return item_db_;
}

Ref<AvatarDatabase> MissionObjectPlacer::get_avatar_db() {
	_ensure_avatar_db();
	return avatar_db_;
}

void MissionObjectPlacer::_ensure_item_db() {
	if (item_db_.is_valid() || resource_root_.is_null()) {
		return;
	}
	Ref<ItemDatabase> db;
	db.instantiate();
	if (db->load_from_resource_root(resource_root_, "items.def") == OK) {
		item_db_ = db;
	}
}

void MissionObjectPlacer::_ensure_avatar_db() {
	if (avatar_db_.is_valid() || resource_root_.is_null()) {
		return;
	}
	Ref<AvatarDatabase> db;
	db.instantiate();
	if (db->load_from_resource_root(resource_root_, "Avatars.def") == OK) {
		avatar_db_ = db;
	}
}

// Drop every derived cache when the resource-root epoch has moved since they
// were filled; cheap when the epoch is unchanged.
void MissionObjectPlacer::_check_epoch() {
	const uint64_t epoch = ResourceRoot::cache_epoch();
	if (epoch == built_epoch_) {
		return;
	}
	built_epoch_ = epoch;
	object_data_cache_.clear();
	skeletal_cache_.clear();
	static_batch_cache_.clear();
	static_lod_profile_cache_.clear();
	graphic_panm_cache_.clear();
	occlusion_cache_.clear();
	static_sources_.invalidate_shadow_assets();
	// Effect snapshots retain their resources across a root epoch, like the
	// old placed rows; the shadow-only references are released here.
	HashMap<uint64_t, Ref<ObjectData>> retained;
	for (const auto &source : static_sources_.effect_sources()) {
		if (const auto *data = static_source_assets_.getptr(source.asset_id))
			retained.insert(source.asset_id, *data);
	}
	static_source_assets_ = std::move(retained);
}

// --- coordinate conversion ---------------------------------------------------

// The witnessed transform math lives engine-side (placement_traits.h);
// these wrappers only convert vector types.
Vector3 MissionObjectPlacer::bms_to_godot_position(const Vector3 &p) {
	const opennova::mission::PlacementVec3 out =
			opennova::mission::bms_to_presentation_position(
					opennova::mission::PlacementVec3{ float(p.x), float(p.y),
							float(p.z) });
	return Vector3(out.x, out.y, out.z);
}

Vector3 MissionObjectPlacer::godot_to_bms_position(const Vector3 &p) {
	const opennova::mission::PlacementVec3 out =
			opennova::mission::presentation_to_bms_position(
					opennova::mission::PlacementVec3{ float(p.x), float(p.y),
							float(p.z) });
	return Vector3(out.x, out.y, out.z);
}

Basis MissionObjectPlacer::bms_to_godot_basis(const Vector3 &rot_deg) {
	return ::godot::bms_to_godot_basis(rot_deg);
}

Transform3D MissionObjectPlacer::entity_transform(const Vector3 &position,
		const Vector3 &rotation_deg) {
	return Transform3D(bms_to_godot_basis(rotation_deg),
			bms_to_godot_position(position));
}

int32_t MissionObjectPlacer::_item_model_scale_q16(int p_item_id) const {
	return item_db_.is_valid() ? item_db_->get_model_scale_q16(p_item_id) : 0;
}

bool MissionObjectPlacer::_item_projection_zero_center(int p_item_id) const {
	return item_db_.is_valid() && opennova::world::item_def_zero_bbox_center(
			item_db_->get_item_type(p_item_id), item_db_->get_attrib(p_item_id));
}

Transform3D MissionObjectPlacer::_entity_transform_for_item(
		const Vector3 &p_position, const Vector3 &p_rotation_deg,
		int p_item_id) const {
	Transform3D out = entity_transform(p_position, p_rotation_deg);
	const int32_t scale_q16 = _item_model_scale_q16(p_item_id);
	if (scale_q16 != 0) {
		const float scale = static_cast<float>(scale_q16) / 65536.0f;
		out.basis = out.basis.scaled(Vector3(scale, scale, scale));
	}
	return out;
}

void MissionObjectPlacer::_configure_item_scale(ObjectModel *p_model,
		int p_item_id) const {
	if (p_model != nullptr) {
		p_model->set_entity_uniform_scale_q16(_item_model_scale_q16(p_item_id));
	}
}

// --- witnessed eligibility ---------------------------------------------------

bool MissionObjectPlacer::item_casts_dynamic_shadow(int item_type, uint32_t attrib2) {
	return opennova::mission::item_casts_dynamic_shadow(item_type, attrib2);
}

bool MissionObjectPlacer::item_casts_static_terrain_shadow(MissionData::EntityKind kind,
		uint32_t entity_attrib, uint32_t item_attrib, uint32_t item_attrib2) {
	return opennova::mission::item_casts_static_terrain_shadow(static_cast<int>(kind),
			entity_attrib, item_attrib, item_attrib2);
}

bool MissionObjectPlacer::_item_is_mirror_reflected(int p_item_id) const {
	return item_db_.is_valid() &&
			opennova::mission::item_is_mirror_reflected(
					item_db_->get_item_type(p_item_id));
}

bool MissionObjectPlacer::_placement_is_mirror_reflected(
		uint32_t p_entity_attrib, int p_item_id) const {
	const int item_type = item_db_.is_valid()
			? item_db_->get_item_type(p_item_id)
			: -1;
	return opennova::mission::placement_is_mirror_reflected(
			p_entity_attrib, item_type);
}

// --- environment relight -----------------------------------------------------

// --- placement ---------------------------------------------------------------

Ref<MissionPlacementStats> MissionObjectPlacer::place(const Ref<MissionData> &p_mission,
		Node3D *p_parent, const Dictionary &p_options) {
	if (p_mission.is_null()) {
		return place_rows(std::vector<PlacementRow>(), nullptr, p_options);
	}
	return place_rows(placement_rows_of(p_mission), p_parent, p_options);
}

std::vector<MissionObjectPlacer::PlacementRow> MissionObjectPlacer::placement_rows_of(
		const Ref<MissionData> &p_mission) {
	std::vector<PlacementRow> rows;
	if (p_mission.is_null()) {
		return rows;
	}
	// The document's own entities, straight off the bms::File in the
	// placement order (markers, items, buildings, organics).
	const opennova::bms::File &file = p_mission->native_file();
	const opennova::mission::EntityKind kinds[] = {
		opennova::mission::EntityKind::Marker, opennova::mission::EntityKind::Item,
		opennova::mission::EntityKind::Building, opennova::mission::EntityKind::Organic
	};
	for (const opennova::mission::EntityKind kind : kinds) {
		const std::vector<opennova::bms::Entity> *list = opennova::mission::entities(file, kind);
		if (list == nullptr) {
			continue;
		}
		for (size_t i = 0; i < list->size(); ++i) {
			const opennova::bms::Entity &entity = (*list)[i];
			const opennova::mission::EntityTransform transform =
					opennova::mission::entity_transform(entity);
			PlacementRow row;
			row.kind = static_cast<int>(kind);
			row.index = static_cast<int>(i);
			// item_id is the items.def key (bms type_id + 100000).
			row.item_id = opennova::mission::entity_item_id(entity);
			row.bms_id = static_cast<int>(entity.id);
			row.group = static_cast<int>(entity.group_id);
			row.team = static_cast<int>(entity.team);
			row.ai_flags = static_cast<uint32_t>(entity.bmsi_attributes);
			row.position = Vector3(transform.x, transform.y, transform.z);
			row.rotation_deg = Vector3(transform.pitch, transform.yaw, transform.roll);
			rows.push_back(row);
		}
	}
	return rows;
}

Ref<MissionPlacementStats> MissionObjectPlacer::place_entities(const Array &p_entities,
		Node3D *p_parent, const Dictionary &p_options) {
	std::vector<PlacementRow> rows;
	rows.reserve(static_cast<size_t>(p_entities.size()));
	for (int64_t i = 0; i < p_entities.size(); ++i) {
		const Dictionary entity = p_entities[i];
		PlacementRow row;
		row.kind = int(entity.get("kind", -1));
		row.index = int(entity.get("index", -1));
		row.item_id = int(entity.get("item_id", 0));
		row.bms_id = int(entity.get("bms_id", 0));
		row.group = int(entity.get("group", -1));
		row.team = int(entity.get("team", 0));
		row.ai_flags = uint32_t(entity.get("ai_flags", 0));
		row.position = entity.get("position", Vector3());
		row.rotation_deg = entity.get("rotation_deg", Vector3());
		rows.push_back(row);
	}
	return place_rows(rows, p_parent, p_options);
}

Ref<MissionPlacementStats> MissionObjectPlacer::place_rows(const std::vector<PlacementRow> &p_rows,
		Node3D *p_parent, const Dictionary &p_options) {
	// The game places whole: the run begun and every unit stepped here (mission_placement_run.cpp
	// holds the walk; the editor's device steps the same run over its frames).
	Ref<MissionPlacementRun> run = begin_place_rows(p_rows, p_parent, p_options);
	while (run->step() == MissionPlacementRun::STEP_MORE) {
	}
	Ref<MissionPlacementStats> stats = run->get_stats();
	if (stats.is_null()) {
		stats.instantiate();
	}
	return stats;
}

Ref<MissionPlacementRun> MissionObjectPlacer::begin_place(const Ref<MissionData> &p_mission,
		Node3D *p_parent, const Dictionary &p_options) {
	// No mission: as place(), nothing placed and no container made.
	if (p_mission.is_null()) {
		return begin_place_rows(std::vector<PlacementRow>(), nullptr, p_options);
	}
	return begin_place_rows(placement_rows_of(p_mission), p_parent, p_options);
}

// --- owner-managed model builds ---------------------------------------------

ObjectModel *MissionObjectPlacer::build_animated_model(int p_item_id,
		Node3D *p_parent) {
	const String graphic = _graphic_for(p_item_id);
	if (graphic.is_empty()) {
		return nullptr;
	}
	const Ref<ObjectData> data = _load_object_data(graphic);
	if (data.is_null()) {
		return nullptr;
	}
	ObjectModel *model = memnew(ObjectModel);
	model->set_panm_clock(panm_clock_);
	model->set_authored_lod_enabled(true);
	model->set_name(vformat("PlayerAvatar_%s", graphic));
	model->set_graphic_name(graphic);
	// Wire-streamed and avatar builds share this chain: vehicles reflect in
	// the water mirror, persons and everything else never do (env #30).
	model->set_mirror_reflected(_item_is_mirror_reflected(p_item_id));
	_configure_item_scale(model, p_item_id);
	p_parent->add_child(model);
	_configure_item_shadow(model, p_item_id);
	_apply_skeletal_anim(model, p_item_id, data->get_bone_origins(),
			data->get_bone_parents());
	model->set_muzzle_point_name(item_db_.is_valid()
					? item_db_->get_launchups_closeattack(p_item_id)
					: String());
	model->set_object_data(data);
	return model;
}

int MissionObjectPlacer::resolve_player_visual_item_id(int p_runtime_type_id) {
	_ensure_item_db();
	if (item_db_.is_null()) {
		return p_runtime_type_id;
	}
	static_assert(MissionData::ITEM_ID_OFFSET == opennova::mission::kItemIdOffset);
	static_assert(PLAYER_RUNTIME_TYPE_ID == opennova::mission::kPlayerRuntimeTypeId);
	static_assert(PLAYER_VISUAL_ITEM_ID == opennova::mission::kPlayerVisualItemId);
	const ItemDatabase *db = item_db_.ptr();
	return opennova::mission::resolve_visual_item_id(p_runtime_type_id,
			[db](int p_id) { return db->has_item(p_id); });
}

// The player's visual = the combo its packed character id resolves to: head +
// body models in the world (retail draws BOTH with the entity's skeleton, each
// after its own camo store [orig: Terrain_RenderSectorEntitiesBySide
// @0x5c7fea..0x5c8020: CharacterEntity blip +4 head then +0 body, see docs/playerinfo/avatars-re.md]) and the arms
// model in first person. `fallback` = no combo resolved: with an EMPTY registry
// retail draws the entity's own item model @0x5c8039 (blip handles 0), which is
// what this returns. The remaining delta, recorded under avatars-re
// D-PLAYERINFO-1 (FIXED; the row keeps this note): with a
// populated registry retail's client 0x0C fold re-stamps an UNKNOWN id to the
// first combo of the entity's team side (NapiNPClientMsg 0x0C @0x42eae4..
// @0x42eb03 -> EntitySlot_LookupAndPackEntry side = team != 1) — the
// reimpl has no registry validation yet (D-NET-137) and shows the item model.
Ref<PlayerVisualSpec> MissionObjectPlacer::resolve_player_visual_spec(
		int p_runtime_type_id, int p_character_id) {
	_ensure_item_db();
	_ensure_avatar_db();
	Ref<PlayerVisualSpec> out;
	out.instantiate();
	const int item_id = resolve_player_visual_item_id(p_runtime_type_id);
	out->set_item_id(item_id);
	if (avatar_db_.is_valid()) {
		const Ref<AvatarComboRow> resolved = avatar_db_->resolve_character_id(
				p_character_id);
		if (resolved.is_valid()) {
			const Ref<AvatarPartRow> head = resolved->get_head();
			const Ref<AvatarPartRow> body = resolved->get_body();
			const Ref<AvatarPartRow> arms = resolved->get_arms();
			out->set_character_id(p_character_id & 0xffff);
			out->set_head(head->get_graphic());
			out->set_head_camo(head->get_camo());
			out->set_body(body->get_graphic());
			out->set_body_camo(body->get_camo());
			if (arms.is_valid()) {
				out->set_arms(arms->get_graphic());
				out->set_arms_camo(arms->get_camo());
			}
			out->set_avatar(head->get_voice());
			out->set_sex(head->get_sex());
			out->set_nationality_index(resolved->get_nationality_index());
			out->set_division_index(resolved->get_division_index());
			out->set_combo_index(resolved->get_combo_index());
			out->set_fallback(false);
			return out;
		}
	}
	out->set_character_id(p_character_id);
	out->set_body(item_db_.is_valid() ? item_db_->get_graphic(item_id) : String());
	out->set_fallback(true);
	return out;
}

ObjectModel *MissionObjectPlacer::build_player_animated_model(
		int p_runtime_type_id, Node3D *p_parent, int p_character_id) {
	const int item_id = resolve_player_visual_item_id(p_runtime_type_id);
	const Ref<PlayerVisualSpec> spec = resolve_player_visual_spec(
			p_runtime_type_id, p_character_id);
	if (spec->get_fallback()) {
		return build_animated_model(item_id, p_parent);
	}
	const String body_graphic = spec->get_body();
	const String head_graphic = spec->get_head();
	// Retail composes head + body only when BOTH blip model handles are
	// nonzero; either missing falls to the entity's own item model.
	// [orig: Terrain_RenderSectorEntitiesBySide @0x5c7fdf-0x5c7fea —
	//  jz to the forced_model 0 item path on either zero handle]
	if (body_graphic.is_empty() || head_graphic.is_empty()) {
		return build_animated_model(item_id, p_parent);
	}
	const String adm_name = item_db_.is_valid()
			? item_db_->get_anim_def(item_id)
			: String();
	ObjectModel *body = build_model_from_graphic(body_graphic, adm_name,
			p_parent, String(), body_graphic, true);
	if (body == nullptr) {
		return build_animated_model(item_id, p_parent);
	}
	// Same node naming as the item-model path (PlayerAvatar_<graphic>): the
	// composed body IS the player's avatar node; the head rides under it.
	body->set_name(vformat("PlayerAvatar_%s", body_graphic.get_file().get_basename()));
	body->set_avatar_part(ObjectModel::AVATAR_PART_BODY);
	body->set_character_id(p_character_id & 0xffff);
	// [orig: Avatar_SetBodyCamoCtrl @0x57a390 immediately before the body submit
	// @0x5c800f, see docs/playerinfo/avatars-re.md]
	AvatarDatabase::apply_part_camo(body, spec->get_body_camo(),
			"player_avatar:body_camo");
	body->set_mirror_reflected(_item_is_mirror_reflected(item_id));
	_configure_item_scale(body, item_id);
	body->set_muzzle_point_name(item_db_.is_valid()
			? item_db_->get_launchups_closeattack(item_id)
			: String());
	_configure_item_shadow(body, item_id);

	ObjectModel *head = build_model_from_graphic(head_graphic, adm_name,
			body, String(), body_graphic, true);
	if (head == nullptr) {
		// A head that fails to build mirrors the zero-handle case: tear the
		// composed body down and render the plain item model instead of a
		// headless avatar. [orig: the both-or-neither gate above]
		body->queue_free();
		return build_animated_model(item_id, p_parent);
	}
	head->set_name(vformat("PlayerAvatarHead_%s",
			head_graphic.get_file().get_basename()));
	head->set_avatar_part(ObjectModel::AVATAR_PART_HEAD);
	head->set_character_id(p_character_id & 0xffff);
	// [orig: Avatar_SetHeadCamoCtrl @0x57a370 immediately before the
	// head submit @0x5c7fec, see docs/playerinfo/avatars-re.md]
	AvatarDatabase::apply_part_camo(head, spec->get_head_camo(),
			"player_avatar:head_camo");
	head->set_mirror_reflected(_item_is_mirror_reflected(item_id));
	_configure_item_shadow(head, item_id);
	head->set_authored_lod_projection_owner(body);
	// The head follows every body presentation call (one entity, one
	// skeleton, one CTRL bus) except the per-part camo triplet.
	body->add_presentation_link(head,
			AvatarDatabase::part_camo_registers());
	return body;
}

ObjectModel *MissionObjectPlacer::avatar_head_part(ObjectModel *p_body) {
	if (p_body == nullptr) {
		return nullptr;
	}
	for (int i = 0; i < p_body->get_child_count(); ++i) {
		ObjectModel *part = Object::cast_to<ObjectModel>(p_body->get_child(i));
		if (part != nullptr && part->get_avatar_part() == ObjectModel::AVATAR_PART_HEAD) {
			return part;
		}
	}
	return nullptr;
}

ObjectModel *MissionObjectPlacer::build_model_from_graphic(
		const String &p_graphic, const String &p_adm_name, Node3D *p_parent,
		const String &p_clip_key, const String &p_rig_graphic,
		bool p_retain_authored_lods) {
	if (p_graphic.is_empty() || p_parent == nullptr) {
		return nullptr;
	}
	const Ref<ObjectData> data = _load_object_data(p_graphic);
	if (data.is_null()) {
		return nullptr;
	}
	ObjectModel *model = memnew(ObjectModel);
	model->set_panm_clock(panm_clock_);
	model->set_authored_lod_enabled(p_retain_authored_lods);
	model->set_name(vformat("Viewmodel_%s", p_graphic));
	model->set_graphic_name(p_graphic);
	p_parent->add_child(model);
	if (!p_adm_name.is_empty()) {
		// The ADM names the CLIP SET; the rig table belongs to the equipped
		// FP gun — the arms and gun ride one shared table, exactly as retail
		// draws the arms with the GUN's bone matrices (witness:
		// placement_traits.h ledger, shared FP rig table). Origins +
		// parents = the MODEL bone table (SkeletalAnim reconstructs rest
		// positions from the model pivots + the reset .bad bind rotations).
		const String rig_name =
				p_rig_graphic.is_empty() ? p_graphic : p_rig_graphic;
		Ref<ObjectData> skel_model =
				rig_name.nocasecmp_to(p_graphic) == 0
				? data
				: _load_object_data(rig_name);
		const PackedVector3Array skel_origins = skel_model.is_valid()
				? skel_model->get_bone_origins()
				: PackedVector3Array();
		const PackedInt32Array skel_parents = skel_model.is_valid()
				? skel_model->get_bone_parents()
				: PackedInt32Array();
		const String adm_name = p_adm_name.to_lower().ends_with(".adm")
				? p_adm_name
				: p_adm_name + String(".adm");
		const Ref<SkeletalAnim> skeletal =
				_skeletal_from_adm(adm_name, skel_origins, skel_parents);
		if (skeletal.is_valid()) {
			model->set_skeletal_anim(skeletal);
		}
	}
	model->set_object_data(data);
	// Pose into a starting clip (e.g. the FP weapon idle) so the model holds
	// that pose rather than its bind/T-pose; the model self-ticks the clip.
	if (!p_clip_key.is_empty()) {
		model->play_body_clip(p_clip_key);
	}
	return model;
}

// --- internals ---------------------------------------------------------------

String MissionObjectPlacer::_graphic_for(int p_item_id) const {
	if (item_db_.is_null()) {
		return String();
	}
	return item_db_->get_graphic(p_item_id);
}

String MissionObjectPlacer::_model_name_for(const String &p_graphic) const {
	const String basename = p_graphic.get_file().get_basename();
	return basename.is_empty() ? String() : basename + String(".3di");
}

Ref<ObjectData> MissionObjectPlacer::_load_object_data(
		const String &p_graphic) {
	const Ref<ObjectData> *cached = object_data_cache_.getptr(p_graphic);
	if (cached != nullptr) {
		return *cached;
	}
	Ref<ObjectData> data;
	const String model_name = _model_name_for(p_graphic);
	if (!model_name.is_empty() && resource_root_.is_valid()) {
		Ref<ObjectData> d;
		d.instantiate();
		if (d->open_from_resource_root(resource_root_, model_name) == OK) {
			data = d;
		}
	}
	object_data_cache_[p_graphic] = data;
	return data;
}

// An item that cannot ride the pooled MultiMesh batch and needs its own
// ObjectModel: animated items (persons, anim-def carriers, dynamic-shadow
// casters) plus portal-carrying buildings — the render-occlusion frame
// drives per-section (Robj) visibility masks, and a pooled batch has no
// per-instance section handle (witness: placement_traits.h ledger,
// per-section building visibility; docs/render/render-occlusion-re.md).
bool MissionObjectPlacer::_needs_individual_node(int p_item_id) {
	if (item_db_.is_null()) {
		return false;
	}
	// The rule is the engine's (placement_traits.h); the occlusion-record probe
	// is an asset-cache leg, so it runs only when the cheaper tests said no.
	const int item_type = item_db_->get_item_type(p_item_id);
	if (opennova::mission::uses_sway_renderer(
				item_db_->get_render_function(p_item_id).utf8().get_data()))
		return true;
	if (opennova::mission::uses_submodel_renderer(
				item_db_->get_render_function(p_item_id).utf8().get_data()))
		return true;
    if (opennova::mission::uses_section_renderer(item_db_->get_ai_function(p_item_id).utf8().get_data()) ||
            opennova::mission::uses_section_renderer(item_db_->get_move_function(p_item_id).utf8().get_data()) ||
            opennova::mission::uses_section_renderer(item_db_->get_render_function(p_item_id).utf8().get_data()))
        return true;
	const bool has_anim_def = !item_db_->get_anim_def(p_item_id).is_empty();
	if (opennova::mission::needs_individual_node(item_type, item_db_->get_attrib2(p_item_id),
				has_anim_def, false)) {
		return true;
	}
	return _has_occlusion_records(p_item_id);
}

// A graphic whose model carries a live PANM track must not be frozen into a
// MultiMesh batch: the harvest captures the rest pose while the original
// engine rebuilds PANM node matrices from the global millisecond clock every
// rendered frame — free-running decorations animate with no mission action
// involved. The evaluator's own gates are ported in
// engine/formats/threedi (threedi_panm_matrices / threedi_panm_runtime).
bool MissionObjectPlacer::_graphic_needs_live_panm(const String &p_graphic) {
	const bool *cached = graphic_panm_cache_.getptr(p_graphic);
	if (cached != nullptr) {
		return *cached;
	}
	bool result = false;
	const Ref<ObjectData> data = _load_object_data(p_graphic);
	if (data.is_valid()) {
		result = data->has_live_panm();
	}
	graphic_panm_cache_[p_graphic] = result;
	return result;
}

bool MissionObjectPlacer::_has_occlusion_records(int p_item_id) {
	const bool *cached = occlusion_cache_.getptr(p_item_id);
	if (cached != nullptr) {
		return *cached;
	}
	bool has_occ = false;
	const String graphic = _graphic_for(p_item_id);
	if (!graphic.is_empty()) {
		const Ref<ObjectData> data = _load_object_data(graphic);
		if (data.is_valid()) {
			has_occ = data->has_occlusion();
		}
	}
	occlusion_cache_[p_item_id] = has_occ;
	return has_occ;
}

int32_t MissionObjectPlacer::_item_entity_bound_radius_q16(int p_item_id,
		const Ref<ObjectData> &p_data) {
	if (p_data.is_null() || item_db_.is_null()) return 0;
	const auto &model = p_data->native_model();
	opennova::world::EntityBoundRadiusInputs inputs;
	inputs.model_radius_q16 = opennova::world::model_bound_radius_q16_from_3di(model);
	inputs.uniform_scale_q16 = _item_model_scale_q16(p_item_id);
	inputs.has_collision_block = model.collision != nullptr;
	const String husk = item_db_->get_husk(p_item_id);
	if (inputs.has_collision_block && !husk.is_empty()) {
		const Ref<ObjectData> husk_data = _load_object_data(husk);
		if (husk_data.is_valid()) {
			inputs.has_first_husk = true;
			inputs.first_husk_radius_q16 =
					opennova::world::model_bound_radius_q16_from_3di(
							husk_data->native_model());
		}
	}
	return opennova::world::entity_bound_radius_q16(inputs);
}

void MissionObjectPlacer::_configure_item_shadow(ObjectModel *p_model,
		int p_item_id) {
	if (p_model == nullptr || item_db_.is_null()) {
		return;
	}
    p_model->set_geometry_visible(!opennova::mission::uses_submodel_renderer(
            item_db_->get_render_function(p_item_id).utf8().get_data()));
	p_model->set_shadow_caster_enabled(item_casts_dynamic_shadow(
			item_db_->get_item_type(p_item_id),
			item_db_->get_attrib2(p_item_id)));
	// The render-slot ground-shadow profile: person-type casters steepen the
	// drape's depth-clip plane 4x (the shadowztex stage, not the silhouette
	// projection), and vehicles may author an items.def `shadow` blob decal —
	// the drape fallback for a bound slot past the silhouette-capture budget
	// [orig: itemdef type 3 gate @0x5d5d7f, the +0xA0 decal via @0x5d59d0;
	// see docs/render/render-lighting-re.md].
	p_model->set_slot_shadow_person(
			item_db_->get_item_type(p_item_id) == ItemDatabase::TYPE_PERSON);
	// The same native entity-bound producer supplies live and batched draws.
	// The model sphere remains the separate silhouette capture extent.
	const String graphic = _graphic_for(p_item_id);
	if (!graphic.is_empty()) {
		const Ref<ObjectData> data = _load_object_data(graphic);
		if (data.is_valid()) {
			const int32_t entity_radius = _item_entity_bound_radius_q16(p_item_id, data);
			p_model->set_bound_radii_q16(
					opennova::world::model_bound_radius_q16_from_3di(data->native_model()),
					entity_radius);
			const bool person = item_db_->get_item_type(p_item_id) == ItemDatabase::TYPE_PERSON;
			int32_t parachute_radius = 0;
			if (person) {
				const String chute_graphic = _graphic_for(opennova::mission::kItemIdOffset +
						opennova::world::kParachuteItemTypeId);
				if (!chute_graphic.is_empty()) {
					const Ref<ObjectData> chute = _load_object_data(chute_graphic);
					if (chute.is_valid()) parachute_radius =
							opennova::world::model_bound_radius_q16_from_3di(chute->native_model());
				}
			}
			p_model->configure_entity_projection(person, parachute_radius,
					_item_projection_zero_center(p_item_id));
		}
	}
	// The static tile pass must ignore the visible model's portal/section
	// mask; eligible mission entities get independent all-section siblings
	// after their visible model is built.
	p_model->set_static_shadow_caster_enabled(false);
}

void MissionObjectPlacer::_configure_item_lighting(ObjectModel *p_model,
		int p_item_id) {
	if (p_model == nullptr || item_db_.is_null()) {
		return;
	}
	// Every type-5 building draws only through the building batch, which
	// pushes its own ItemDef+0x218 daylight and submits with 0x40; the rigid
	// collector marks ROBJ 1+ as interior-lighting entries while ROBJ 0 stays
	// the exterior shell -- portal or not (retail Terrain_RenderSectorModels
	// @0x5c5df2..0x5c5e00, push 40h @0x5c5f1f; Render_CollectRenderObjectsForBatch
	// @0x5d9156..0x5d9162; the rule is renderer::static_row_entity_lighting).
	if (item_db_->get_item_type(p_item_id) != ItemDatabase::TYPE_BUILDING) {
		return;
	}
	p_model->set_interior_section_light_transfer(
			item_db_->get_light_transfer(p_item_id));
}

// Cached per (.adm, model bone table): two models can share one .adm (the FP
// arms + gun both use ak47_1st.adm) yet carry different .3di pivots, so they
// must not alias (witness: placement_traits.h ledger — the model is the rig
// source, never the lossy .bad records).
Ref<SkeletalAnim> MissionObjectPlacer::_skeletal_from_adm(
		const String &p_adm_name, const PackedVector3Array &p_bone_origins,
		const PackedInt32Array &p_bone_parents) {
	// A def-named .adm the mounted roots do not carry loads default.adm in its
	// place, and the cache keys the RESOLVED name (AnimMap_LoadAdmFile's
	// FileSystem_FileExists miss -> "default.adm" substitution ahead of
	// AnimMap_FindByName; engine anim/adm_fallback.h). A present-but-broken
	// file still fails below, as retail's parse error path does.
	const String adm_name = opennova::to_gd(
			opennova::anim::adm_name_or_default(opennova::to_std(p_adm_name),
					resource_root_.is_valid() && resource_root_->has_file(p_adm_name)));
	const String cache_key = adm_name + String("#") +
			String::num_int64(Variant(p_bone_origins).hash()) + String("#") +
			String::num_int64(Variant(p_bone_parents).hash());
	const Ref<SkeletalAnim> *cached = skeletal_cache_.getptr(cache_key);
	if (cached != nullptr) {
		return *cached;
	}
	Ref<SkeletalAnim> skeletal;
	skeletal.instantiate();
	if (!skeletal->load_from_resource_root(resource_root_, adm_name,
				p_bone_origins, p_bone_parents)) {
		skeletal.unref();
	}
	skeletal_cache_[cache_key] = skeletal;
	return skeletal;
}

void MissionObjectPlacer::_apply_skeletal_anim(ObjectModel *p_model,
		int p_item_id, const PackedVector3Array &p_bone_origins,
		const PackedInt32Array &p_bone_parents) {
	if (p_model == nullptr || resource_root_.is_null() || item_db_.is_null()) {
		return;
	}
	const String anim_def = item_db_->get_anim_def(p_item_id);
	if (anim_def.is_empty()) {
		return;
	}
	const String adm_name = anim_def.to_lower().ends_with(".adm")
			? anim_def
			: anim_def + String(".adm");
	const Ref<SkeletalAnim> skeletal =
			_skeletal_from_adm(adm_name, p_bone_origins, p_bone_parents);
	if (skeletal.is_valid()) {
		p_model->set_skeletal_anim(skeletal);
	}
}

// Build a template ObjectModel with every authored RLOD retained and harvest
// one batch per (level, submesh) through its typed level harvest (the model
// poses each level in turn, so every offset is the submesh's ROBJ base pose
// at that level), together with the graphic's RLOD profile (thresholds,
// model sphere, harvested levels). The model enters the live tree only for
// the harvest (so bounds/global-transform math is valid and silent), then
// frees; the harvested Mesh/Material refs survive. Each batch offset is the
// submesh's model-local rest transform at its level relative to the entity
// origin — NO ground-anchor offset (the engine bakes the Ground userpoint
// into the stored position at author-time; witness: placement_traits.h
// ledger).
Vector<MissionObjectPlacer::StaticBatch>
MissionObjectPlacer::_get_static_batches(const String &p_graphic,
		Node *p_tree_parent) {
	const Vector<StaticBatch> *cached = static_batch_cache_.getptr(p_graphic);
	if (cached != nullptr) {
		return *cached;
	}
	Vector<StaticBatch> batches;
	StaticLodProfile profile;
	const Ref<ObjectData> data = _load_object_data(p_graphic);
	if (data.is_valid() && p_tree_parent != nullptr) {
		ObjectModel *model = memnew(ObjectModel);
		model->set_authored_lod_enabled(true);
		p_tree_parent->add_child(model);
		model->set_object_data(data);
		Vector<ObjectModel::HarvestedSurface> rows;
		model->harvest_level_surfaces(rows);
		int submesh = 0;
		for (const ObjectModel::HarvestedSurface &row : rows) {
			StaticBatch batch;
			batch.mesh = row.mesh;
			batch.material = row.material;
			batch.offset = row.offset;
			batch.submesh = submesh;
			batch.robj_index = row.robj_index;
			batch.lod_index = row.lod_index;
			batch.auxiliary_draw = row.auxiliary_draw;
			batch.blended_draw = row.blended_draw;
			batches.push_back(batch);
			++submesh;
		}
		if (data->has_document()) {
			const Threedi3di3 &native_model = data->native_model();
			const int lod_count = MAX(1, static_cast<int>(native_model.lod_count));
			for (int lod = 0; lod < lod_count; ++lod) {
				profile.thresholds_q16.push_back(opennova::renderer::rlod_threshold_q16_from_rmdl(
						native_model.lods[lod].lod_threshold));
			}
			profile.projection_sphere =
					opennova::world::collision_projection_sphere_from_3di(native_model);
			profile.zero_center_projection_sphere =
					opennova::world::collision_projection_sphere_from_3di(
							native_model, 0, 0, true);
		}
		p_tree_parent->remove_child(model);
		memdelete(model);
	}
	_complete_static_lod_profile(profile, batches);
	static_batch_cache_[p_graphic] = batches;
	static_lod_profile_cache_[p_graphic] = profile;
	return batches;
}

MissionObjectPlacer::StaticLodProfile
MissionObjectPlacer::_static_lod_profile_for(const String &p_graphic) const {
	const StaticLodProfile *cached = static_lod_profile_cache_.getptr(p_graphic);
	if (cached != nullptr) {
		return *cached;
	}
	StaticLodProfile profile;
	_complete_static_lod_profile(profile, Vector<StaticBatch>());
	return profile;
}

// Fill what the harvest or the registration seam left implicit: one
// threshold row per harvested level, which levels carry geometry, and the
// level-0 geometry bounds as the sphere fallback when no document was
// supplied (a loaded document without a CMDL block already carries its
// valid zero-radius sphere and never reaches this fallback).
void MissionObjectPlacer::_complete_static_lod_profile(
		StaticLodProfile &r_profile, const Vector<StaticBatch> &p_batches) {
	std::size_t level_count = 1;
	for (const StaticBatch &batch : p_batches) {
		level_count = MAX(level_count,
				static_cast<std::size_t>(MAX(batch.lod_index, 0)) + 1);
	}
	if (r_profile.thresholds_q16.size() < level_count) {
		r_profile.thresholds_q16.resize(level_count, 0);
	}
	r_profile.available.assign(r_profile.thresholds_q16.size(), false);
	for (const StaticBatch &batch : p_batches) {
		if (batch.lod_index >= 0 &&
				static_cast<std::size_t>(batch.lod_index) <
						r_profile.available.size()) {
			r_profile.available[static_cast<std::size_t>(batch.lod_index)] = true;
		}
	}
	if (r_profile.projection_sphere.valid) return;
	AABB bounds;
	bool has_bounds = false;
	for (const StaticBatch &batch : p_batches) {
		if (batch.lod_index != 0 || batch.mesh.is_null()) {
			continue;
		}
		const AABB surface_bounds = batch.offset.xform(batch.mesh->get_aabb());
		bounds = has_bounds ? bounds.merge(surface_bounds) : surface_bounds;
		has_bounds = true;
	}
	if (has_bounds) {
		const Vector3 end = bounds.get_end();
		const auto q16 = opennova::io::float_to_fp16_16_round_sat;
		r_profile.projection_sphere = opennova::renderer::object_projection_sphere_from_bounds_q16(
				{q16(bounds.position.z), q16(bounds.position.x), q16(bounds.position.y)},
				{q16(end.z), q16(end.x), q16(end.y)});
	}
}

// [engine: renderer::project_bound_sphere_radius_q16, the sub-pixel floor
//  kObjectLodSubPixelCullQ16, renderer::select_object_lod and
//  object_lod_frame_scale own the witnessed rule; this walk only feeds them
//  each retained instance and rewrites the slots of the ones that crossed]
int MissionObjectPlacer::update_static_lods(
		const Transform3D &p_camera_transform, float p_vertical_fov_degrees,
		float p_viewport_width, float p_viewport_height) {
	const ObjectLodFrame frame = ObjectLodFrame::make(p_camera_transform,
			p_vertical_fov_degrees, p_viewport_width, p_viewport_height);
	return update_static_lod_views(&frame, 1);
}

void MissionObjectPlacer::set_static_instance_occlusion_hidden(int p_bms_id, bool p_hidden) {
	const auto *rec = static_sources_.instance(p_bms_id);
	if (rec == nullptr || rec->lod_instance < 0 ||
			rec->lod_instance >= static_lod_instances_.size()) {
		return;
	}
	static_lod_instances_.ptrw()[rec->lod_instance].occlusion_hidden = p_hidden;
}

void MissionObjectPlacer::clear_static_instance_occlusion() {
	StaticLodInstance *instances = static_lod_instances_.ptrw();
	for (int row = 0; row < static_lod_instances_.size(); ++row) {
		instances[row].occlusion_hidden = false;
	}
}

int MissionObjectPlacer::update_static_lod_views(const ObjectLodFrame *p_frames,
		int p_frame_count) {
	static_lod_switches_ = 0;
	if (static_lod_instances_.is_empty() || p_frames == nullptr) {
		return 0;
	}
	const int view_count = std::clamp(p_frame_count, 0, 2);
	const bool inset = view_count > 1 && p_frames[1].valid;
	int valid_frames = 0;
	for (int f = 0; f < view_count; ++f) {
		valid_frames += p_frames[f].valid ? 1 : 0;
	}
	if (valid_frames == 0) {
		return 0;
	}
	// The touched set allocates only when a slot actually moves; a frame
	// without a crossing walks the instances and allocates nothing. A flat
	// walk: project() rejects an instance outside the frustum (retail never
	// reaches the selector for an entity its collector rejected) before any
	// arithmetic, and a coarser per-cell rejection measured no gain at the
	// mission spawn poses (render-order-re.md, the RLOD row).
	HashSet<int> touched;
	StaticLodInstance *instances = static_lod_instances_.ptrw();
	const int instance_count = static_lod_instances_.size();
	for (int row = 0; row < instance_count; ++row) {
		StaticLodInstance &instance = instances[row];
		if (instance.carved || instance.profile < 0 ||
				instance.profile >= static_lod_profiles_.size()) {
			continue;
		}
		const StaticLodProfile &profile = static_lod_profiles_[instance.profile];
		// One view's level: none for an instance its collector culled or
		// that projects below the sub-pixel floor, and the view's current one
		// for an instance outside its frustum (retail never reaches the
		// selector for an entity its collector rejected).
		const auto view_level = [&](const ObjectLodFrame &p_frame, bool p_hidden,
										int p_current) {
			if (p_hidden) {
				return -1;
			}
			int32_t radius_q16 = 0;
			if (!p_frame.valid ||
					!p_frame.project_q16(instance.origin, instance.radius_q16, radius_q16)) {
				return p_current;
			}
			if (opennova::renderer::object_subpixel_culled(radius_q16)) {
				return -1;
			}
			return opennova::renderer::select_object_lod(profile.thresholds_q16,
					radius_q16, p_frame.projection_scale, profile.available)
					.lod_index;
		};
		const int main_lod = view_level(p_frames[0], instance.occlusion_hidden,
				instance.active_lod);
		const int inset_lod = inset
				? view_level(p_frames[1],
						  instance.inset_occlusion_own ? instance.inset_occlusion_hidden
													   : instance.occlusion_hidden,
						  instance.inset_lod_own ? instance.inset_lod : main_lod)
				: main_lod;
		const bool split = inset && inset_lod != main_lod;
		const bool unchanged = main_lod == instance.active_lod &&
				split == instance.view_split && (!split || inset_lod == instance.inset_lod);
		instance.inset_lod = inset_lod;
		instance.inset_lod_own = inset;
		if (unchanged) {
			continue;
		}
		if (split) {
			_ensure_static_view_twins(row);
			// Minting a twin grew the instances' binding lists only; the
			// instance array itself stays where it is.
			instances = static_lod_instances_.ptrw();
		}
		instances[row].view_split = split;
		_write_static_instance_slots(row, main_lod, touched);
		if (main_lod != instances[row].active_lod) {
			++static_lod_switches_;
		}
		instances[row].active_lod = main_lod;
	}
	_flush_static_population_changes(touched);
	return static_lod_switches_;
}

int MissionObjectPlacer::update_static_lods_for_views(Camera3D *p_main, float p_main_width,
		Camera3D *p_inset, float p_inset_width) {
	ObjectLodFrame frames[2];
	frames[0] = ObjectLodFrame::from_camera(p_main, p_main_width);
	int count = 1;
	if (p_inset != nullptr) {
		frames[1] = ObjectLodFrame::from_camera(p_inset, p_inset_width);
		count = 2;
	}
	return update_static_lod_views(frames, count);
}

void MissionObjectPlacer::set_static_instance_inset_occlusion_hidden(int p_bms_id,
		bool p_hidden) {
	const auto *rec = static_sources_.instance(p_bms_id);
	if (rec == nullptr || rec->lod_instance < 0 ||
			rec->lod_instance >= static_lod_instances_.size()) {
		return;
	}
	StaticLodInstance &instance = static_lod_instances_.ptrw()[rec->lod_instance];
	instance.inset_occlusion_hidden = p_hidden;
	instance.inset_occlusion_own = true;
}

void MissionObjectPlacer::clear_static_instance_inset_occlusion() {
	StaticLodInstance *instances = static_lod_instances_.ptrw();
	for (int row = 0; row < static_lod_instances_.size(); ++row) {
		instances[row].inset_occlusion_hidden = false;
		instances[row].inset_occlusion_own = false;
	}
}

int MissionObjectPlacer::get_static_instance_inset_lod(int p_bms_id) const {
	const auto *rec = static_sources_.instance(p_bms_id);
	if (rec == nullptr || rec->lod_instance < 0 ||
			rec->lod_instance >= static_lod_instances_.size()) {
		return -2;
	}
	const StaticLodInstance &instance = static_lod_instances_[rec->lod_instance];
	return instance.view_split ? instance.inset_lod : instance.active_lod;
}

void MissionObjectPlacer::_ensure_static_view_twins(int p_instance_row) {
	if (p_instance_row < 0 || p_instance_row >= static_lod_instances_.size()) {
		return;
	}
	// Every shared visible population of the instance (all its levels), so a
	// later level change finds its twins in place.
	const int binding_count = static_lod_instances_[p_instance_row].bindings.size();
	for (int binding_index = 0; binding_index < binding_count; ++binding_index) {
		const StaticLodBinding binding =
				static_lod_instances_[p_instance_row].bindings[binding_index];
		if (binding.view != kStaticViewShared || binding.shadow_only) {
			continue;
		}
		_static_view_twin(binding.population, kStaticViewMain);
		_static_view_twin(binding.population, kStaticViewInset);
	}
}

// The main-view twin keeps the source's layer with the world bits swapped for
// the main-view bits the Inset camera lacks (its casting, Q3 source and
// shadow metas kept); the Inset twin carries the Inset bit alone and casts
// nothing (render/visual_layers.h carries the per-view design).
int MissionObjectPlacer::_static_view_twin(int p_population, uint8_t p_view) {
	if (p_population < 0 || p_population >= static_populations_.size()) {
		return -1;
	}
	{
		const StaticPopulation &source = static_populations_[p_population];
		if (source.view != kStaticViewShared || source.shadow_only) {
			return -1;
		}
		const int existing = p_view == kStaticViewMain ? source.main_twin : source.inset_twin;
		if (existing >= 0) {
			return existing;
		}
	}
	const StaticPopulation source = static_populations_[p_population];
	StaticPopulationInstance *source_node = Object::cast_to<StaticPopulationInstance>(
			ObjectDB::get_instance(source.instance_node));
	if (source_node == nullptr || source.multimesh.is_null() ||
			source_node->get_parent() == nullptr) {
		return -1;
	}
	const int capacity = source.row_instance.size();
	Ref<MultiMesh> mm;
	mm.instantiate();
	mm->set_transform_format(MultiMesh::TRANSFORM_3D);
	mm->set_use_custom_data(source.custom_data);
	mm->set_mesh(source.multimesh->get_mesh());
	mm->set_instance_count(capacity);
	mm->set_visible_instance_count(0);
	StaticPopulation twin;
	twin.multimesh = mm;
	twin.lod_index = source.lod_index;
	twin.custom_data = source.custom_data;
	twin.shadow_tagged = source.shadow_tagged;
	twin.view = p_view;
	twin.row_instance.resize(capacity);
	twin.row_binding.resize(capacity);
	twin.row_slot.resize(capacity);

	StaticPopulationInstance *mmi = memnew(StaticPopulationInstance);
	mmi->set_multimesh(mm);
	mmi->set_custom_aabb(source_node->get_custom_aabb());
	uint32_t layer = visual_layers::INSET_VIEW;
	if (p_view == kStaticViewMain) {
		layer = source_node->get_layer_mask();
		if ((layer & visual_layers::WORLD) != 0) {
			layer = (layer & ~uint32_t(visual_layers::WORLD)) | visual_layers::MAIN_VIEW;
		}
		if ((layer & visual_layers::WORLD_NO_MIRROR) != 0) {
			layer = (layer & ~uint32_t(visual_layers::WORLD_NO_MIRROR)) |
					visual_layers::MAIN_VIEW_NO_MIRROR;
		}
	}
	mmi->set_layer_mask(layer);
	mmi->set_cast_shadows_setting(p_view == kStaticViewMain
					? source_node->get_cast_shadows_setting()
					: GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
	mmi->set_material_override(source_node->get_material_override());
	mmi->set_population_kind(source_node->get_population_kind());
	mmi->set_lod_index(source_node->get_lod_index());
	mmi->set_bin_x(source_node->get_bin_x());
	mmi->set_bin_z(source_node->get_bin_z());
	mmi->set_name(String(source_node->get_name()) +
			(p_view == kStaticViewMain ? "_MainView" : "_InsetView"));
	if (source.shadow_tagged) {
		mmi->set_shadow_tagged(true);
		mmi->set_slot_bms_ids(source_node->get_slot_bms_ids());
		mmi->set_slot_item_ids(source_node->get_slot_item_ids());
		mmi->set_slot_attrib2(source_node->get_slot_attrib2());
		mmi->set_slot_casts_shadow(source_node->get_slot_casts_shadow());
		mmi->set_graphic(source_node->get_graphic());
	}
	twin.instance_node = mmi->get_instance_id();
	static_populations_.push_back(twin);
	const int index = static_populations_.size() - 1;
	StaticPopulation &source_row = static_populations_.write[p_population];
	(p_view == kStaticViewMain ? source_row.main_twin : source_row.inset_twin) = index;
	static_population_by_node_[twin.instance_node] = index;
	mmi->set_visible(false);
	if (source.shadow_tagged) {
		mmi->set_row_slots(PackedInt32Array());
	}
	source_node->get_parent()->add_child(mmi);
	if (p_view == kStaticViewMain && source.shadow_tagged) {
		FrameFx::register_q3_object_source(mmi, source_node->get_material_override());
	}
	// Every slot of the source joins the twin, no row live yet.
	for (int s = 0; s < source.slot_instance.size(); ++s) {
		const int instance_row = source.slot_instance[s];
		const int binding_index = source.slot_binding[s];
		if (instance_row < 0 || instance_row >= static_lod_instances_.size() ||
				binding_index < 0 ||
				binding_index >= static_lod_instances_[instance_row].bindings.size()) {
			continue;
		}
		StaticLodBinding binding = static_lod_instances_[instance_row].bindings[binding_index];
		binding.population = index;
		binding.row = -1;
		binding.view = p_view;
		StaticLodInstance &instance = static_lod_instances_.write[instance_row];
		instance.bindings.push_back(binding);
		StaticPopulation &twin_row = static_populations_.write[index];
		twin_row.slot_instance.push_back(instance_row);
		twin_row.slot_binding.push_back(instance.bindings.size() - 1);
	}
	return index;
}

int MissionObjectPlacer::get_static_instance_lod(int p_bms_id) const {
	const auto *rec = static_sources_.instance(p_bms_id);
	if (rec == nullptr || rec->lod_instance < 0 ||
			rec->lod_instance >= static_lod_instances_.size()) {
		return -2;
	}
	return static_lod_instances_[rec->lod_instance].active_lod;
}

Array MissionObjectPlacer::get_static_instance_live_populations(
		int p_bms_id) const {
	Array out;
	const auto *rec = static_sources_.instance(p_bms_id);
	if (rec == nullptr || rec->lod_instance < 0 ||
			rec->lod_instance >= static_lod_instances_.size()) {
		return out;
	}
	const StaticLodInstance &instance = static_lod_instances_[rec->lod_instance];
	for (const StaticLodBinding &binding : instance.bindings) {
		if (binding.shadow_only || binding.row < 0 || binding.population < 0 ||
				binding.population >= static_populations_.size()) {
			continue;
		}
		Node *node = Object::cast_to<Node>(ObjectDB::get_instance(
				static_populations_[binding.population].instance_node));
		out.push_back(node != nullptr ? String(node->get_name()) : String());
	}
	return out;
}

PackedInt32Array MissionObjectPlacer::get_static_population_live_bms_ids(
		MultiMeshInstance3D *p_population) const {
	PackedInt32Array out;
	if (p_population == nullptr) {
		return out;
	}
	const int *index =
			static_population_by_node_.getptr(p_population->get_instance_id());
	if (index == nullptr || *index < 0 || *index >= static_populations_.size()) {
		return out;
	}
	const StaticPopulation &population = static_populations_[*index];
	for (int row = 0; row < population.live; ++row) {
		const int instance_row = population.row_instance[row];
		out.push_back(instance_row >= 0 && instance_row < static_lod_instances_.size()
						? static_lod_instances_[instance_row].bms_id
						: 0);
	}
	return out;
}

int MissionObjectPlacer::get_static_live_population_count() const {
	int live = 0;
	for (const StaticPopulation &population : static_populations_) {
		if (population.live > 0) {
			++live;
		}
	}
	return live;
}

bool MissionObjectPlacer::_static_slot_live(const StaticLodBinding &p_binding,
		int p_main_lod, int p_inset_lod, bool p_split) {
	switch (p_binding.view) {
		case kStaticViewMain:
			return p_split && p_binding.lod_index == p_main_lod;
		case kStaticViewInset:
			return p_split && p_binding.lod_index == p_inset_lod;
		default:
			break;
	}
	// The shared rows: the shadow twin follows the main view (it casts for
	// the frame's shadows), the visible rows only while the views agree.
	if (p_binding.lod_index != p_main_lod) {
		return false;
	}
	return p_binding.shadow_only ? p_binding.casts : !p_split;
}

void MissionObjectPlacer::_write_static_instance_slots(int p_instance_row,
		int p_live_lod, HashSet<int> &r_touched) {
	if (p_instance_row < 0 || p_instance_row >= static_lod_instances_.size()) {
		return;
	}
	// A carved instance draws in no view; otherwise its split decision and
	// Inset level ride the instance record.
	const bool carved = static_lod_instances_[p_instance_row].carved;
	const bool split = !carved && static_lod_instances_[p_instance_row].view_split;
	const int inset_lod = split ? static_lod_instances_[p_instance_row].inset_lod : -1;
	const int binding_count =
			static_lod_instances_[p_instance_row].bindings.size();
	for (int binding_index = 0; binding_index < binding_count; ++binding_index) {
		const StaticLodBinding &binding =
				static_lod_instances_[p_instance_row].bindings[binding_index];
		if (binding.population < 0 ||
				binding.population >= static_populations_.size()) {
			continue;
		}
		const bool live = _static_slot_live(binding, p_live_lod, inset_lod, split);
		if (live == (binding.row >= 0)) {
			continue;
		}
		r_touched.insert(binding.population);
		if (live) {
			_static_population_append(binding.population, p_instance_row,
					binding_index);
		} else {
			_static_population_remove(binding.population, p_instance_row,
					binding_index);
		}
	}
}

// Append one slot as the population's next live row: the row takes the
// binding's transform and light-atlas custom data, and visible_instance_count
// grows to cover it.
void MissionObjectPlacer::_static_population_append(int p_population,
		int p_instance_row, int p_binding) {
	StaticPopulation &population = static_populations_.write[p_population];
	StaticLodBinding &binding =
			static_lod_instances_.write[p_instance_row].bindings.write[p_binding];
	if (population.multimesh.is_null() || binding.row >= 0 ||
			population.live >= population.row_instance.size()) {
		return;
	}
	const int row = population.live;
	population.multimesh->set_instance_transform(row, binding.live_xform);
	if (population.custom_data) {
		population.multimesh->set_instance_custom_data(row, binding.custom_data);
	}
	population.row_instance.write[row] = p_instance_row;
	population.row_binding.write[row] = p_binding;
	population.row_slot.write[row] = binding.slot;
	binding.row = row;
	population.live = row + 1;
	population.multimesh->set_visible_instance_count(population.live);
}

// Swap-remove one live row: the last live row moves into the hole (its
// binding follows), and visible_instance_count shrinks past it.
void MissionObjectPlacer::_static_population_remove(int p_population,
		int p_instance_row, int p_binding) {
	StaticPopulation &population = static_populations_.write[p_population];
	StaticLodBinding &binding =
			static_lod_instances_.write[p_instance_row].bindings.write[p_binding];
	if (population.multimesh.is_null() || binding.row < 0 ||
			binding.row >= population.live) {
		return;
	}
	const int row = binding.row;
	const int last = population.live - 1;
	binding.row = -1;
	if (row != last) {
		const int moved_instance = population.row_instance[last];
		const int moved_binding_index = population.row_binding[last];
		StaticLodBinding &moved = static_lod_instances_.write[moved_instance]
										  .bindings.write[moved_binding_index];
		population.multimesh->set_instance_transform(row, moved.live_xform);
		if (population.custom_data) {
			population.multimesh->set_instance_custom_data(row, moved.custom_data);
		}
		population.row_instance.write[row] = moved_instance;
		population.row_binding.write[row] = moved_binding_index;
		population.row_slot.write[row] = moved.slot;
		moved.row = row;
	}
	population.live = last;
	population.multimesh->set_visible_instance_count(population.live);
}

PackedInt32Array MissionObjectPlacer::_static_population_row_slots(
		const StaticPopulation &p_population) {
	PackedInt32Array rows;
	rows.resize(p_population.live);
	for (int row = 0; row < p_population.live; ++row) {
		rows[row] = p_population.row_slot[row];
	}
	return rows;
}

void MissionObjectPlacer::_flush_static_population_changes(
		const HashSet<int> &p_touched) {
	for (const int population_index : p_touched) {
		if (population_index < 0 ||
				population_index >= static_populations_.size()) {
			continue;
		}
		const StaticPopulation &population = static_populations_[population_index];
		StaticPopulationInstance *node = Object::cast_to<StaticPopulationInstance>(
				ObjectDB::get_instance(population.instance_node));
		if (node == nullptr) {
			continue;
		}
		node->set_visible(population.live > 0);
		if (population.shadow_tagged) {
			node->set_row_slots(_static_population_row_slots(population));
		}
		if (!population.shadow_only) {
			FrameFx::invalidate_q3_instances(node);
		}
	}
}

// Visible portal/PANM models live below camera-masked ROBJ nodes; retail's
// terrain-tile collector ignores those masks and submits every selected-LOD
// ROBJ, so harvest one independent all-section shadow-only sibling per
// submesh of every retained level, bound to its level on the owner (shown
// exactly while the owner draws that level).
void MissionObjectPlacer::_add_individual_static_shadow_siblings(
		ObjectModel *p_model, const String &p_graphic,
		const Transform3D &p_local_xform, const String &p_suffix) {
	if (p_model == nullptr) {
		return;
	}
	Vector<ObjectModel::HarvestedSurface> rows;
	p_model->harvest_level_surfaces(rows);
	// Registered/synthetic graphics can supply harvested static batches without
	// an ObjectModel scene. Keep that owner-facing seam useful while production
	// models harvest their own retained levels.
	if (rows.is_empty()) {
		const Vector<StaticBatch> batches = _get_static_batches(p_graphic, p_model);
		for (const StaticBatch &batch : batches) {
			ObjectModel::HarvestedSurface row;
			row.mesh = batch.mesh;
			row.material = batch.material;
			row.offset = batch.offset;
			row.robj_index = batch.robj_index;
			row.lod_index = batch.lod_index;
			row.auxiliary_draw = batch.auxiliary_draw;
			row.blended_draw = batch.blended_draw;
			rows.push_back(row);
		}
	}
	int submesh = 0;
	for (const ObjectModel::HarvestedSurface &row : rows) {
		if (row.auxiliary_draw || row.mesh.is_null()) {
			continue;
		}
		Ref<MultiMesh> mm;
		mm.instantiate();
		mm->set_transform_format(MultiMesh::TRANSFORM_3D);
		mm->set_mesh(row.mesh);
		mm->set_instance_count(1);
		mm->set_instance_transform(0, p_local_xform * row.offset);
		MultiMeshInstance3D *mmi = memnew(MultiMeshInstance3D);
		mmi->set_multimesh(mm);
		mmi->set_layer_mask(Water::VISUAL_LAYER_STATIC_SHADOW_CASTER);
		mmi->set_cast_shadows_setting(
				GeometryInstance3D::SHADOW_CASTING_SETTING_SHADOWS_ONLY);
		if (row.material.is_valid()) {
			mmi->set_material_override(row.material);
		}
		mmi->set_name(vformat("StaticShadow_%s_%s_%d", p_graphic, p_suffix,
				submesh++));
		p_model->add_child(mmi);
		// The owner's level swap shows and hides these beside its own slots.
		p_model->add_level_bound_visual(row.lod_index, mmi);
	}
}

String MissionObjectPlacer::graphic_for(int p_item_id) {
	_ensure_item_db();
	return _graphic_for(p_item_id);
}

Ref<ObjectData> MissionObjectPlacer::object_data_for(const String &p_graphic) {
	_check_epoch();
	return _load_object_data(p_graphic);
}

Node3D *MissionObjectPlacer::_ensure_container(Node3D *p_parent) {
	Node *existing = p_parent->get_node_or_null(NodePath(kContainerName));
	if (existing != nullptr) {
		for (int i = existing->get_child_count() - 1; i >= 0; --i) {
			Node *child = existing->get_child(i);
			existing->remove_child(child);
			child->queue_free();
		}
		return Object::cast_to<Node3D>(existing);
	}
	Node3D *container = memnew(Node3D);
	container->set_name(kContainerName);
	p_parent->add_child(container);
	return container;
}

} // namespace godot
