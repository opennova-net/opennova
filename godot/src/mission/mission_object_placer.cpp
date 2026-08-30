#include "mission/mission_object_placer.h"
#include "render/frame_fx.h"

#include <cmath>

#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/time.hpp>

#include <runtime/simassets/model_builders.h>
#include <runtime/world/entity.h>

#include "env/water.h"
#include "mission/mission_data.h"
#include "mission/mission_object_placer_keys.h"

namespace godot {

namespace {

constexpr const char *kContainerName = "MissionObjects";
constexpr float kStaticBatchBinSize = 512.0f;

int static_batch_bin_coord(float p_world) {
	return static_cast<int>(std::floor(p_world / kStaticBatchBinSize));
}

uint64_t static_batch_bin_key(int p_x, int p_z) {
	return (static_cast<uint64_t>(static_cast<uint32_t>(p_x)) << 32) |
			static_cast<uint32_t>(p_z);
}

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
	ClassDB::bind_method(D_METHOD("get_avatar_db"),
			&MissionObjectPlacer::get_avatar_db);
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
			D_METHOD("item_casts_dynamic_shadow", "item_type", "attrib",
					"attrib2"),
			&MissionObjectPlacer::item_casts_dynamic_shadow);
	ClassDB::bind_static_method("MissionObjectPlacer",
			D_METHOD("item_casts_static_terrain_shadow", "kind",
					"entity_attrib", "item_attrib", "item_attrib2"),
			&MissionObjectPlacer::item_casts_static_terrain_shadow);

	ClassDB::bind_method(D_METHOD("place", "mission", "parent", "options"),
			&MissionObjectPlacer::place, DEFVAL(Dictionary()));
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

	ClassDB::bind_method(D_METHOD("get_placed_entity_records"),
			&MissionObjectPlacer::get_placed_entity_records);
	ClassDB::bind_method(D_METHOD("set_placed_entity_records", "records"),
			&MissionObjectPlacer::set_placed_entity_records);
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "placed_entity_records"),
			"set_placed_entity_records", "get_placed_entity_records");
	ClassDB::bind_method(D_METHOD("get_static_user_point_sources"),
			&MissionObjectPlacer::get_static_user_point_sources);
	ClassDB::bind_method(D_METHOD("get_static_item_effect_sources"),
			&MissionObjectPlacer::get_static_item_effect_sources);
	ClassDB::bind_method(D_METHOD("get_static_light_draw_sources"),
			&MissionObjectPlacer::get_static_light_draw_sources);
	ClassDB::bind_method(D_METHOD("get_static_light_draw_source_revision"),
			&MissionObjectPlacer::get_static_light_draw_source_revision);
	ClassDB::bind_method(D_METHOD("get_static_instance_binding_count", "bms_id"),
			&MissionObjectPlacer::get_static_instance_binding_count);
	ClassDB::bind_method(
			D_METHOD("get_static_terrain_shadow_source_diagnostics"),
			&MissionObjectPlacer::get_static_terrain_shadow_source_diagnostics);
	ClassDB::bind_method(
			D_METHOD("get_static_terrain_shadow_source_revision"),
			&MissionObjectPlacer::get_static_terrain_shadow_source_revision);
	ClassDB::bind_method(D_METHOD("graphic_for", "item_id"),
			&MissionObjectPlacer::graphic_for);
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
	ClassDB::bind_method(
			D_METHOD("static_instance_casts_terrain_shadow", "bms_id"),
			&MissionObjectPlacer::static_instance_casts_terrain_shadow);
	ClassDB::bind_method(D_METHOD("hide_static_instance", "bms_id"),
			&MissionObjectPlacer::hide_static_instance);
	ClassDB::bind_method(D_METHOD("show_static_instance", "bms_id"),
			&MissionObjectPlacer::show_static_instance);
	ClassDB::bind_method(
			D_METHOD("update_static_terrain_shadow_source_transform", "kind",
					"index", "xform"),
			&MissionObjectPlacer::update_static_terrain_shadow_source_transform);
	ClassDB::bind_method(
			D_METHOD("set_static_terrain_shadow_replacement", "bms_id",
					"graphic", "xform", "active"),
			&MissionObjectPlacer::set_static_terrain_shadow_replacement);
	ClassDB::bind_method(
			D_METHOD("clear_static_terrain_shadow_replacement", "bms_id"),
			&MissionObjectPlacer::clear_static_terrain_shadow_replacement);
	ClassDB::bind_method(
			D_METHOD("register_resolved_static_graphic", "graphic", "data",
					"batches"),
			&MissionObjectPlacer::register_resolved_static_graphic);
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
	graphic_panm_cache_.clear();
	graphic_multiple_lods_cache_.clear();
	occlusion_cache_.clear();
	for (int i = 0; i < static_terrain_shadow_sources_.size(); ++i) {
		static_terrain_shadow_sources_.write[i].object_data.unref();
	}
	static_terrain_shadow_replacements_.clear();
	_bump_static_terrain_shadow_source_revision();
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
	const opennova::mission::PlacementBasis b =
			opennova::mission::bms_to_presentation_basis(
					float(rot_deg.x), float(rot_deg.y), float(rot_deg.z));
	return Basis(Vector3(b.x.x, b.x.y, b.x.z), Vector3(b.y.x, b.y.y, b.y.z),
			Vector3(b.z.x, b.z.y, b.z.z));
}

Transform3D MissionObjectPlacer::entity_transform(const Vector3 &position,
		const Vector3 &rotation_deg) {
	return Transform3D(bms_to_godot_basis(rotation_deg),
			bms_to_godot_position(position));
}

int32_t MissionObjectPlacer::_item_model_scale_q16(int p_item_id) const {
	return item_db_.is_valid() ? item_db_->get_model_scale_q16(p_item_id) : 0;
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

bool MissionObjectPlacer::item_casts_dynamic_shadow(int item_type,
		uint32_t p_attrib, uint32_t attrib2) {
	(void)p_attrib;
	return opennova::mission::item_casts_dynamic_shadow(item_type, attrib2);
}

bool MissionObjectPlacer::item_casts_static_terrain_shadow(int kind,
		uint32_t entity_attrib, uint32_t item_attrib, uint32_t item_attrib2) {
	return opennova::mission::item_casts_static_terrain_shadow(kind,
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

Dictionary MissionObjectPlacer::place(const Ref<MissionData> &p_mission,
		Node3D *p_parent, const Dictionary &p_options) {
	_check_epoch();
	Dictionary stats;
	stats["placed"] = 0;
	stats["batched"] = 0;
	stats["animated"] = 0;
	stats["unresolved"] = 0;
	stats["markers"] = 0;
	stats["graphics"] = 0;
	stats["batches"] = 0;
	stats["static_bins"] = 0;
	stats["static_binned_batches"] = 0;
	stats["static_global_batches"] = 0;
	stats["authored_occluder_models"] = 0;
	destruction_instances_.clear();
	hidden_destruction_instances_.clear();
	static_terrain_shadow_replacements_.clear();
	static_user_point_sources_ = Array();
	static_item_effect_sources_ = Array();
	static_light_draw_sources_ = Array();
	++static_light_draw_source_revision_;
	static_terrain_shadow_sources_.clear();
	static_terrain_shadow_source_rows_.clear();
	static_terrain_shadow_rows_by_bms_.clear();
	_bump_static_terrain_shadow_source_revision();
	placed_entity_records_ = Array();
	if (p_mission.is_null() || p_parent == nullptr || resource_root_.is_null()) {
		return stats;
	}
	_ensure_item_db();

	// Per-stage usec spans returned in the stats (the editor merges them into
	// its own load timeline; replaces the old GDScript timeline seam).
	Dictionary spans;
	Time *clock = Time::get_singleton();
	uint64_t stage_begin = clock->get_ticks_usec();

	// Optional per-model load-progress pulse (the game shell's loading
	// screen), mirroring the original's per-model loading-screen presents
	// (witness: placement_traits.h ledger, Game_StartMission's in-loop
	// presents).
	const Callable progress = p_options.get("progress", Callable());
	// Optional entity-kind exclusion: the joiner places the mission minus
	// organics — players and streamed AI render wire-direct while
	// items/buildings/markers become ordinary placed nodes.
	const Array skip_kinds = p_options.get("skip_kinds", Array());
	Node3D *container = _ensure_container(p_parent);

	// Bucket entities by graphic and retail reflection population, then split
	// static vs animated. One graphic may be authored both with and without
	// the BMS Reflective attribute (CP01 does exactly that); a single
	// MultiMesh layer cannot express those different mirror policies. The
	// parallel per-slot arrays (shadow eligibility, identity, effect sources,
	// edit refs) share one slot order per group so destruction can carve every
	// matching draw by the same BMS index.
	struct StaticGroup {
		String graphic;
		bool mirror_reflected = false;
		Vector<Transform3D> xforms;
		Vector<bool> shadow_slots;
		Vector<int> bms_ids;
		Vector<int> item_ids;
		Vector<int> kinds;
		Vector<int> entity_indices;
		Vector<int> teams;
		Vector<uint32_t> entity_attribs;
		Vector<uint32_t> attrib2_values;
		Array effect_sources;
	};
	const auto static_group_key = [](const String &p_graphic,
			bool p_mirror_reflected) -> String {
		return p_graphic +
				String(p_mirror_reflected ? "::mirror" : "::no_mirror");
	};
	HashMap<String, StaticGroup> static_groups;
	Vector<String> static_order;
	Array animated;
	int markers = 0;
	int unresolved = 0;
	const Array entities = p_mission->get_all_entities();
	for (int i = 0; i < entities.size(); ++i) {
		const Dictionary entity = entities[i];
		const int kind = int(entity.get("kind", -1));
		if (!skip_kinds.is_empty() && skip_kinds.has(kind)) {
			continue;
		}
		if (kind == MissionData::KIND_MARKER) {
			++markers;
			continue;
		}
		const int item_id = int(entity.get("item_id", 0));
		const String graphic = _graphic_for(item_id);
		if (graphic.is_empty()) {
			++unresolved;
			continue;
		}
		const Transform3D xform = _entity_transform_for_item(
				entity.get("position", Vector3()),
				entity.get("rotation_deg", Vector3()), item_id);
		if (_needs_individual_node(item_id) ||
				_graphic_needs_live_panm(graphic)) {
			Dictionary a;
			a["graphic"] = graphic;
			a["item_id"] = item_id;
			a["xform"] = xform;
			a["kind"] = kind;
			a["index"] = int(entity.get("index", -1));
			a["bms_id"] = int(entity.get("bms_id", 0));
			a["group"] = int(entity.get("group", -1));
			a["team"] = int(entity.get("team", -1));
			a["ai_flags"] = int(entity.get("ai_flags", 0));
			a["position"] = entity.get("position", Vector3());
			animated.push_back(a);
			continue;
		}
		const bool mirror_reflected = _placement_is_mirror_reflected(
				uint32_t(entity.get("ai_flags", 0)), item_id);
		const String group_key = static_group_key(graphic, mirror_reflected);
		StaticGroup *group = static_groups.getptr(group_key);
		if (group == nullptr) {
			static_groups[group_key] = StaticGroup();
			group = static_groups.getptr(group_key);
			group->graphic = graphic;
			group->mirror_reflected = mirror_reflected;
			static_order.push_back(group_key);
		}
		group->xforms.push_back(xform);
		group->shadow_slots.push_back(item_casts_static_terrain_shadow(kind,
				uint32_t(entity.get("ai_flags", 0)),
				item_db_->get_attrib(item_id), item_db_->get_attrib2(item_id)));
		group->bms_ids.push_back(int(entity.get("bms_id", 0)));
		group->item_ids.push_back(item_id);
		group->kinds.push_back(kind);
		group->entity_indices.push_back(int(entity.get("index", -1)));
		group->teams.push_back(int(entity.get("team", 0)));
		group->entity_attribs.push_back(
				uint32_t(entity.get("ai_flags", 0)));
		group->attrib2_values.push_back(item_db_->get_attrib2(item_id));
		Dictionary source;
		source["kind"] = kind;
		source["entity_index"] = int(entity.get("index", -1));
		source["bms_id"] = int(entity.get("bms_id", 0));
		source["item_id"] = item_id;
		source["world_transform"] = xform;
		group->effect_sources.push_back(source);
	}
	stats["markers"] = markers;
	spans["bucket_entities"] = clock->get_ticks_usec() - stage_begin;
	stage_begin = clock->get_ticks_usec();

	// Opaque and alpha-tested statics are divided into the same 512-world-unit
	// cells as terrain. Retail's blended strips remain global because their
	// independently chosen Q1/Q2 order must not inherit spatial batch centers.
	int placed = 0;
	int batched = 0;
	int graphics = 0;
	int batch_count = 0;
	int binned_batch_count = 0;
	int global_batch_count = 0;
	HashMap<uint64_t, bool> occupied_static_bins;
	Vector<String> resolved_graphics;
	for (const String &group_key : static_order) {
		if (progress.is_valid()) {
			progress.call();
		}
		StaticGroup &group = static_groups[group_key];
		const String graphic = group.graphic;
		const int instance_count = group.xforms.size();
		const uint32_t batch_world_layer =
				group.mirror_reflected ? uint32_t(Water::VISUAL_LAYER_WORLD)
									   : uint32_t(Water::VISUAL_LAYER_WORLD_NO_MIRROR);
		const bool graphic_has_split_policy =
				static_groups.has(static_group_key(graphic, !group.mirror_reflected));
		const String policy_suffix =
				graphic_has_split_policy
				? (group.mirror_reflected ? "_Mirror" : "_NoMirror")
				: String();
		const Vector<StaticBatch> batches = _get_static_batches(graphic, container);
		if (batches.is_empty()) {
			unresolved += instance_count;
			continue;
		}
		if (!resolved_graphics.has(graphic)) {
			resolved_graphics.push_back(graphic);
			++graphics;
		}
		Ref<ObjectData> shadow_data;
		if (const Ref<ObjectData> *resolved =
					object_data_cache_.getptr(graphic)) {
			shadow_data = *resolved;
		}
		for (int i = 0; i < group.xforms.size(); ++i) {
			_record_static_terrain_shadow_source(
					i < group.kinds.size() ? group.kinds[i] : -1,
					i < group.entity_indices.size()
							? group.entity_indices[i]
							: -1,
					i < group.bms_ids.size() ? group.bms_ids[i] : 0,
					i < group.teams.size() ? group.teams[i] : 0,
					i < group.entity_attribs.size()
							? group.entity_attribs[i]
							: 0,
					i < group.item_ids.size() ? group.item_ids[i] : 0,
					graphic, group.xforms[i], shadow_data);
		}
		Array xform_array;
		for (const Transform3D &xform : group.xforms) {
			xform_array.push_back(xform);
		}
		_record_static_user_point_group(graphic, xform_array);
		Vector<int> effect_source_rows;
		effect_source_rows.resize(instance_count);
		for (int i = 0; i < effect_source_rows.size(); ++i) {
			effect_source_rows.write[i] = -1;
		}
		for (int i = 0; i < group.effect_sources.size(); ++i) {
			Dictionary source = group.effect_sources[i];
			effect_source_rows.write[i] = _append_static_item_effect_source(
					int(source.get("kind", -1)),
					int(source.get("entity_index", -1)),
					int(source.get("bms_id", 0)),
					int(source.get("item_id", 0)), graphic,
					source.get("world_transform", Transform3D()));
		}
		// One atlas row per retained entity/ROBJ. Multiple material surfaces
		// under that ROBJ share the row, while its query AABB is the exact merge
		// of those surfaces in model-rest space transformed by the entity.
		HashMap<int, AABB> local_robj_bounds;
		Vector<int> robj_order;
		for (const StaticBatch &batch : batches) {
			if (batch.mesh.is_null()) {
				continue;
			}
			const AABB surface_bounds = batch.offset.xform(batch.mesh->get_aabb());
			AABB *merged = local_robj_bounds.getptr(batch.robj_index);
			if (merged == nullptr) {
				local_robj_bounds[batch.robj_index] = surface_bounds;
				robj_order.push_back(batch.robj_index);
			} else {
				*merged = merged->merge(surface_bounds);
			}
		}
		HashMap<uint64_t, int> light_draw_rows;
		for (int i = 0; i < instance_count; ++i) {
			for (const int robj_index : robj_order) {
				const AABB *local_bounds = local_robj_bounds.getptr(robj_index);
				if (local_bounds == nullptr) {
					continue;
				}
				const int row = _append_static_light_draw_source(
						i < effect_source_rows.size() ? effect_source_rows[i] : -1,
						i < group.kinds.size() ? group.kinds[i] : -1,
						i < group.entity_indices.size()
								? group.entity_indices[i] : -1,
						i < group.bms_ids.size() ? group.bms_ids[i] : 0,
						i < group.item_ids.size() ? group.item_ids[i] : 0,
						robj_index, group.xforms[i].xform(*local_bounds));
				light_draw_rows[static_light_draw_key(i, robj_index)] = row;
			}
		}

		struct StaticBin {
			int x = 0;
			int z = 0;
			Vector<int> slots;
		};
		HashMap<uint64_t, int> bin_rows;
		Vector<StaticBin> bins;
		for (int i = 0; i < instance_count; ++i) {
			const int bin_x = static_batch_bin_coord(group.xforms[i].origin.x);
			const int bin_z = static_batch_bin_coord(group.xforms[i].origin.z);
			const uint64_t bin_key = static_batch_bin_key(bin_x, bin_z);
			int *bin_row = bin_rows.getptr(bin_key);
			if (bin_row == nullptr) {
				StaticBin bin;
				bin.x = bin_x;
				bin.z = bin_z;
				bins.push_back(bin);
				bin_rows[bin_key] = bins.size() - 1;
				bin_row = bin_rows.getptr(bin_key);
			}
			bins.write[*bin_row].slots.push_back(i);

			const int bms_id = i < group.bms_ids.size() ? group.bms_ids[i] : 0;
			if (bms_id != 0) {
				DestructionInstance inst;
				inst.graphic = graphic;
				inst.batch_key = group_key;
				inst.index = i;
				inst.xform = group.xforms[i];
				inst.casts_static_shadow =
						i < group.shadow_slots.size() && group.shadow_slots[i];
				inst.mirror_reflected = group.mirror_reflected;
				destruction_instances_[bms_id] = inst;
			}
		}
		Vector<int> global_slots;
		global_slots.resize(instance_count);
		for (int i = 0; i < instance_count; ++i) {
			global_slots.write[i] = i;
		}

		const auto tag_static_shadow_source = [&](MultiMeshInstance3D *p_source,
				const Vector<int> &p_slots) {
			Array shadow_bms_ids;
			Array shadow_item_ids;
			Array shadow_attrib2;
			Array shadow_slots;
			for (const int slot : p_slots) {
				shadow_bms_ids.push_back(
						slot < group.bms_ids.size() ? group.bms_ids[slot] : 0);
				shadow_item_ids.push_back(
						slot < group.item_ids.size() ? group.item_ids[slot] : 0);
				shadow_attrib2.push_back(slot < group.attrib2_values.size()
								? int64_t(group.attrib2_values[slot])
								: int64_t(0));
				shadow_slots.push_back(slot < group.shadow_slots.size() &&
						group.shadow_slots[slot]);
			}
			p_source->set_meta("static_shadow_bms_ids", shadow_bms_ids);
			p_source->set_meta("static_shadow_item_ids", shadow_item_ids);
			p_source->set_meta("static_shadow_attrib2", shadow_attrib2);
			p_source->set_meta("static_shadow_slots", shadow_slots);
			p_source->set_meta("static_shadow_graphic", graphic);
			p_source->set_meta("static_shadow_batch_key", group_key);
		};
		const auto bind_destruction_slots = [&](const Ref<MultiMesh> &p_mm,
				const Vector<int> &p_slots) {
			for (int local_index = 0; local_index < p_slots.size(); ++local_index) {
				const int slot = p_slots[local_index];
				if (slot < 0 || slot >= group.bms_ids.size()) {
					continue;
				}
				DestructionInstance *instance =
						destruction_instances_.getptr(group.bms_ids[slot]);
				if (instance == nullptr) {
					continue;
				}
				DestructionBinding binding;
				binding.multimesh = p_mm;
				binding.index = local_index;
				instance->bindings.push_back(binding);
			}
		};

		const auto emit_population = [&](const StaticBatch &p_batch,
				const Vector<int> &p_slots, bool p_global, int p_bin_x,
				int p_bin_z) {
			if (p_slots.is_empty() || p_batch.mesh.is_null()) {
				return;
			}
			bool has_static_shadow = false;
			bool all_static_shadow = true;
			for (const int slot : p_slots) {
				const bool casts = slot >= 0 && slot < group.shadow_slots.size() &&
						group.shadow_slots[slot];
				has_static_shadow = has_static_shadow || casts;
				all_static_shadow = all_static_shadow && casts;
			}

			Ref<MultiMesh> mm;
			mm.instantiate();
			mm->set_transform_format(MultiMesh::TRANSFORM_3D);
			mm->set_use_custom_data(true);
			mm->set_mesh(p_batch.mesh);
			mm->set_instance_count(p_slots.size());
			AABB population_bounds;
			bool has_population_bounds = false;
			for (int local_index = 0; local_index < p_slots.size(); ++local_index) {
				const int slot = p_slots[local_index];
				const Transform3D surface_xform = group.xforms[slot] * p_batch.offset;
				mm->set_instance_transform(local_index, surface_xform);
				const int *row = light_draw_rows.getptr(
						static_light_draw_key(slot, p_batch.robj_index));
				mm->set_instance_custom_data(
						local_index,
						Color(row != nullptr ? static_cast<float>(*row + 1) : 0.0f, 0.0f,
								0.0f, 0.0f));
				const AABB surface_bounds =
						surface_xform.xform(p_batch.mesh->get_aabb());
				population_bounds = has_population_bounds
						? population_bounds.merge(surface_bounds)
						: surface_bounds;
				has_population_bounds = true;
			}

			MultiMeshInstance3D *mmi = memnew(MultiMeshInstance3D);
			mmi->set_multimesh(mm);
			if (has_population_bounds) {
				mmi->set_custom_aabb(population_bounds);
			}
			if (all_static_shadow && !p_batch.auxiliary_draw) {
				mmi->set_layer_mask(batch_world_layer |
						Water::VISUAL_LAYER_STATIC_SHADOW_CASTER);
				mmi->set_cast_shadows_setting(
						GeometryInstance3D::SHADOW_CASTING_SETTING_ON);
			} else {
				mmi->set_layer_mask(batch_world_layer);
				mmi->set_cast_shadows_setting(
						GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
			}
			if (p_batch.material.is_valid()) {
				mmi->set_material_override(p_batch.material);
			}
			mmi->set_meta("static_batch_population",
					p_global ? String("global") : String("bin"));
			if (!p_global) {
				mmi->set_meta("static_batch_bin_x", p_bin_x);
				mmi->set_meta("static_batch_bin_z", p_bin_z);
			}
			const bool legacy_name = p_global || bins.size() == 1;
			mmi->set_name(legacy_name ? vformat("Batch_%s%s_%d", graphic,
												policy_suffix, p_batch.submesh)
									  : vformat("Batch_%s%s_BinX%d_Z%d_%d", graphic,
												policy_suffix, p_bin_x, p_bin_z,
												p_batch.submesh));
			if (!p_batch.auxiliary_draw) {
				tag_static_shadow_source(mmi, p_slots);
			}
			container->add_child(mmi);
			if (!p_batch.auxiliary_draw) {
				FrameFx::register_q3_object_source(mmi, p_batch.material);
			}
			++batch_count;
			if (p_global) {
				++global_batch_count;
			} else {
				++binned_batch_count;
				occupied_static_bins[static_batch_bin_key(p_bin_x, p_bin_z)] = true;
			}
			bind_destruction_slots(mm, p_slots);

			if (!has_static_shadow || all_static_shadow || p_batch.auxiliary_draw) {
				return;
			}
			Ref<MultiMesh> shadow_mm;
			shadow_mm.instantiate();
			shadow_mm->set_transform_format(MultiMesh::TRANSFORM_3D);
			shadow_mm->set_mesh(p_batch.mesh);
			shadow_mm->set_instance_count(p_slots.size());
			AABB shadow_bounds;
			bool has_shadow_bounds = false;
			for (int local_index = 0; local_index < p_slots.size(); ++local_index) {
				const int slot = p_slots[local_index];
				Transform3D shadow_xform = group.xforms[slot] * p_batch.offset;
				const bool casts =
						slot < group.shadow_slots.size() && group.shadow_slots[slot];
				if (!casts) {
					shadow_xform.basis = shadow_xform.basis.scaled(Vector3());
				} else {
					const AABB surface_bounds =
							shadow_xform.xform(p_batch.mesh->get_aabb());
					shadow_bounds = has_shadow_bounds
							? shadow_bounds.merge(surface_bounds)
							: surface_bounds;
					has_shadow_bounds = true;
				}
				shadow_mm->set_instance_transform(local_index, shadow_xform);
			}
			MultiMeshInstance3D *shadow_mmi = memnew(MultiMeshInstance3D);
			shadow_mmi->set_multimesh(shadow_mm);
			if (has_shadow_bounds) {
				shadow_mmi->set_custom_aabb(shadow_bounds);
			}
			shadow_mmi->set_layer_mask(Water::VISUAL_LAYER_STATIC_SHADOW_CASTER);
			shadow_mmi->set_cast_shadows_setting(
					GeometryInstance3D::SHADOW_CASTING_SETTING_SHADOWS_ONLY);
			if (p_batch.material.is_valid()) {
				shadow_mmi->set_material_override(p_batch.material);
			}
			shadow_mmi->set_meta("static_batch_population",
					p_global ? String("global") : String("bin"));
			if (!p_global) {
				shadow_mmi->set_meta("static_batch_bin_x", p_bin_x);
				shadow_mmi->set_meta("static_batch_bin_z", p_bin_z);
			}
			shadow_mmi->set_name(legacy_name
							? vformat("StaticShadow_%s%s_%d", graphic,
									  policy_suffix, p_batch.submesh)
							: vformat("StaticShadow_%s%s_BinX%d_Z%d_%d",
									  graphic, policy_suffix, p_bin_x,
									  p_bin_z, p_batch.submesh));
			tag_static_shadow_source(shadow_mmi, p_slots);
			container->add_child(shadow_mmi);
			bind_destruction_slots(shadow_mm, p_slots);
		};
		for (const StaticBatch &batch : batches) {
			if (batch.blended_draw) {
				emit_population(batch, global_slots, true, 0, 0);
				continue;
			}
			for (const StaticBin &bin : bins) {
				emit_population(batch, bin.slots, false, bin.x, bin.z);
			}
		}
		batched += instance_count;
		placed += instance_count;
	}
	spans["static_batches"] = clock->get_ticks_usec() - stage_begin;
	stage_begin = clock->get_ticks_usec();

	// Animated: an individual ObjectModel per entity.
	int animated_count = 0;
	int authored_occluder_models = 0;
	for (int a_index = 0; a_index < animated.size(); ++a_index) {
		const Dictionary a = animated[a_index];
		if (progress.is_valid()) {
			progress.call();
		}
		const String graphic = a.get("graphic", String());
		const Ref<ObjectData> data = _load_object_data(graphic);
		if (data.is_null()) {
			++unresolved;
			continue;
		}
		const int item_id = int(a.get("item_id", 0));
		const int kind = int(a.get("kind", -1));
		ObjectModel *model = memnew(ObjectModel);
		model->set_panm_clock(panm_clock_);
		model->set_name(vformat("Anim_%s_%d", graphic, animated_count));
		model->set_mirror_reflected(_placement_is_mirror_reflected(
				uint32_t(a.get("ai_flags", 0)), item_id));
		_configure_item_scale(model, item_id);
		// Render the model origin at the entity's stored position directly:
		// the engine bakes the Ground userpoint into the stored position at
		// author-time (place / terrain-drag), not at render (witness:
		// placement_traits.h ledger, author-time Ground bake).
		model->set_transform(a.get("xform", Transform3D()));
		container->add_child(model);
		_configure_item_shadow(model, item_id);
		_configure_item_lighting(model, item_id);
		// Load the entity's body-animation set (.adm) BEFORE the data:
		// setting it first is a no-op rebuild, so set_object_data below does
		// the ONE skeletal-keyed mesh build.
		_apply_skeletal_anim(model, item_id, data->get_bone_origins(),
				data->get_bone_parents());
		// The def names the AI muzzle: items.def launchups_closeattack is
		// the launch userpoint on this item's graphic
		// (world-wac-ai-re §21.2).
		model->set_muzzle_point_name(item_db_->get_launchups_closeattack(item_id));
		// Mission-world models retain every authored RLOD and select by the
		// retail projected-radius rule. Closed building OOBJ records also become
		// Godot occluders; portal/window/open records stay with the section pass.
		model->set_authored_lod_enabled(true);
		model->set_authored_occluders_enabled(kind == MissionData::KIND_BUILDING &&
				_has_occlusion_records(item_id));
		// Drive the build explicitly (not via _ready) so it is independent
		// of when place() runs relative to the main loop.
		model->set_object_data(data);
		if (model->get_authored_occluder_count() > 0) {
			++authored_occluder_models;
		}
		if (item_casts_static_terrain_shadow(kind,
					uint32_t(a.get("ai_flags", 0)),
					item_db_->get_attrib(item_id),
					item_db_->get_attrib2(item_id))) {
			_add_individual_static_shadow_siblings(model, graphic,
					Transform3D(), vformat("live%d", animated_count));
		}
		// Tag identity on the node in BOTH runtime + editor so EntityIndex
		// can resolve SSN/group/zone event-action targets back to this live
		// model.
		Dictionary ref;
		ref["kind"] = kind;
		ref["index"] = int(a.get("index", -1));
		ref["bms_id"] = int(a.get("bms_id", 0));
		ref["group"] = int(a.get("group", -1));
		ref["team"] = int(a.get("team", -1));
		ref["position"] = a.get("position", Vector3());
		ref["item_id"] = item_id;
		ref["graphic"] = graphic;
		ref["attrib2"] = int64_t(item_db_->get_attrib2(item_id));
		model->set_meta("entity_ref", ref);
		Dictionary record;
		record["model"] = model;
		record["ref"] = ref;
		placed_entity_records_.push_back(record);
		_record_static_terrain_shadow_source(kind,
				int(a.get("index", -1)), int(a.get("bms_id", 0)),
				int(a.get("team", 0)),
				uint32_t(a.get("ai_flags", 0)), item_id, graphic,
				a.get("xform", Transform3D()), data);
		++animated_count;
		++placed;
	}
	spans["animated_models"] = clock->get_ticks_usec() - stage_begin;

	stats["placed"] = placed;
	stats["batched"] = batched;
	stats["animated"] = animated_count;
	stats["unresolved"] = unresolved;
	stats["graphics"] = graphics;
	stats["batches"] = batch_count;
	stats["static_bins"] = occupied_static_bins.size();
	stats["static_binned_batches"] = binned_batch_count;
	stats["static_global_batches"] = global_batch_count;
	stats["authored_occluder_models"] = authored_occluder_models;
	stats["spans"] = spans;
	return stats;
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
	if (p_runtime_type_id == PLAYER_RUNTIME_TYPE_ID && item_db_.is_valid() &&
			item_db_->has_item(PLAYER_VISUAL_ITEM_ID)) {
		return PLAYER_VISUAL_ITEM_ID;
	}
	if (item_db_.is_valid()) {
		if (item_db_->has_item(p_runtime_type_id)) {
			return p_runtime_type_id;
		}
		if (p_runtime_type_id > 0 &&
				p_runtime_type_id < MissionData::ITEM_ID_OFFSET) {
			const int authored_item_id =
					p_runtime_type_id + MissionData::ITEM_ID_OFFSET;
			if (item_db_->has_item(authored_item_id)) {
				return authored_item_id;
			}
		}
	}
	return p_runtime_type_id;
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
// @0x42eb03 -> lookup_entity_slot_and_pack_entry side = team != 1) — the
// reimpl has no registry validation yet (D-NET-137) and shows the item model.
Dictionary MissionObjectPlacer::resolve_player_visual_spec(
		int p_runtime_type_id, int p_character_id) {
	_ensure_item_db();
	_ensure_avatar_db();
	if (avatar_db_.is_valid()) {
		const Dictionary resolved = avatar_db_->resolve_character_id(
				p_character_id);
		if (!resolved.is_empty()) {
			const Dictionary head = resolved.get("head", Dictionary());
			const Dictionary body = resolved.get("body", Dictionary());
			const Dictionary arms = resolved.get("arms", Dictionary());
			Dictionary out;
			out["character_id"] = p_character_id & 0xffff;
			out["item_id"] = resolve_player_visual_item_id(
					p_runtime_type_id);
			out["head"] = head.get("graphic", String());
			out["head_camo"] = head.get("camo", Array());
			out["body"] = body.get("graphic", String());
			out["body_camo"] = body.get("camo", Array());
			out["arms"] = arms.get("graphic", String());
			out["arms_camo"] = arms.get("camo", Array());
			out["avatar"] = head.get("voice", 1);
			out["sex"] = head.get("sex", 0);
			out["nationality_index"] = resolved.get(
					"nationality_index", -1);
			out["division_index"] = resolved.get("division_index", -1);
			out["combo_index"] = resolved.get("combo_index", -1);
			out["fallback"] = false;
			return out;
		}
	}
	Dictionary out;
	const int item_id = resolve_player_visual_item_id(p_runtime_type_id);
	out["character_id"] = p_character_id;
	out["item_id"] = item_id;
	out["head"] = String();
	out["body"] = item_db_.is_valid() ? item_db_->get_graphic(item_id) : String();
	out["arms"] = String();
	out["fallback"] = true;
	return out;
}

ObjectModel *MissionObjectPlacer::build_player_animated_model(
		int p_runtime_type_id, Node3D *p_parent, int p_character_id) {
	const int item_id = resolve_player_visual_item_id(p_runtime_type_id);
	const Dictionary spec = resolve_player_visual_spec(
			p_runtime_type_id, p_character_id);
	if (bool(spec.get("fallback", true))) {
		return build_animated_model(item_id, p_parent);
	}
	const String body_graphic = spec.get("body", String());
	const String head_graphic = spec.get("head", String());
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
	body->set_meta("avatar_part", "body");
	body->set_meta("character_id", p_character_id & 0xffff);
	body->set_meta("player_visual_spec", spec);
	// [orig: Avatar_SetBodyCamoCtrl @0x57a390 immediately before the body submit
	// @0x5c800f, see docs/playerinfo/avatars-re.md]
	AvatarDatabase::apply_part_camo(body, spec.get("body_camo", Array()),
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
	head->set_meta("avatar_part", "head");
	head->set_meta("character_id", p_character_id & 0xffff);
	// [orig: Avatar_SetHeadCamoCtrl @0x57a370 immediately before the
	// head submit @0x5c7fec, see docs/playerinfo/avatars-re.md]
	AvatarDatabase::apply_part_camo(head, spec.get("head_camo", Array()),
			"player_avatar:head_camo");
	head->set_mirror_reflected(_item_is_mirror_reflected(item_id));
	_configure_item_shadow(head, item_id);
	// The head follows every body presentation call (one entity, one
	// skeleton, one CTRL bus) except the per-part camo triplet.
	body->add_presentation_link(head,
			AvatarDatabase::part_camo_registers());
	return body;
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
	const int item_type = item_db_->get_item_type(p_item_id);
	if (item_type == ItemDatabase::TYPE_PERSON) {
		return true;
	}
	if (item_casts_dynamic_shadow(item_type, item_db_->get_attrib(p_item_id),
				item_db_->get_attrib2(p_item_id))) {
		return true;
	}
	if (!item_db_->get_anim_def(p_item_id).is_empty()) {
		return true;
	}
	const String graphic = _graphic_for(p_item_id);
	return _has_occlusion_records(p_item_id) ||
			(!graphic.is_empty() && _graphic_has_multiple_lods(graphic));
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

bool MissionObjectPlacer::_graphic_has_multiple_lods(const String &p_graphic) {
	const bool *cached = graphic_multiple_lods_cache_.getptr(p_graphic);
	if (cached != nullptr) {
		return *cached;
	}
	bool result = false;
	const Ref<ObjectData> data = _load_object_data(p_graphic);
	if (data.is_valid() && data->has_document()) {
		result = data->native_model().lod_count > 1;
	}
	graphic_multiple_lods_cache_[p_graphic] = result;
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

void MissionObjectPlacer::_configure_item_shadow(ObjectModel *p_model,
		int p_item_id) {
	if (p_model == nullptr || item_db_.is_null()) {
		return;
	}
	p_model->set_shadow_caster_enabled(item_casts_dynamic_shadow(
			item_db_->get_item_type(p_item_id),
			item_db_->get_attrib(p_item_id), item_db_->get_attrib2(p_item_id)));
	// The render-slot ground-shadow profile: person-type casters steepen the
	// drape's depth-clip plane 4x (the shadowztex stage, not the silhouette
	// projection), and vehicles may author an items.def `shadow` blob decal —
	// the drape fallback for a bound slot past the silhouette-capture budget
	// [orig: itemdef type 3 gate @0x5d5d7f, the +0xA0 decal via @0x5d59d0;
	// see docs/render/render-lighting-re.md].
	p_model->set_slot_shadow_person(
			item_db_->get_item_type(p_item_id) == ItemDatabase::TYPE_PERSON);
	// The two radii the shadow slot reads. The MODEL SPHERE is the .3di
	// header's origin sphere (gpm[5]) — the silhouette capture extent and the
	// depth clip size from it. The ENTITY BOUND (entity+0) is that sphere
	// raised to the first husk stage's sphere and padded + 0x1000, written
	// only when the graphic carries a collision block — the slot lod/patch
	// and the light query size from it [orig: Entity_InitFromModel
	// @0x40dc30 — the gpm[44] collision-block gate, the scaled base-model
	// bound, the unscaled husk max, and the +0x1000 pad].
	const String graphic = _graphic_for(p_item_id);
	if (!graphic.is_empty()) {
		const Ref<ObjectData> data = _load_object_data(graphic);
		if (data.is_valid()) {
			// 0 for a document without LOD 0 (nothing loaded): stays unstamped.
			const float model_sphere =
					opennova::simassets::model_bound_radius_from_3di(
							data->native_model());
			if (model_sphere > 0.0f) {
				float entity_bound = 0.0f;
				if (data->has_collision()) {
					int32_t bound_q16 = static_cast<int32_t>(Math::round(
							static_cast<double>(model_sphere) * 65536.0));
					const int32_t scale_q16 = _item_model_scale_q16(p_item_id);
					if (scale_q16 != 0) {
						bound_q16 = opennova::world::retail_q16_mul_rhu(
								bound_q16, scale_q16);
					}
					String husk = item_db_->get_husk(p_item_id);
					if (husk.is_empty()) {
						husk = item_db_->get_huskfinal(p_item_id);
					}
					if (!husk.is_empty()) {
						const Ref<ObjectData> husk_data = _load_object_data(husk);
						if (husk_data.is_valid()) {
							const float husk_bound =
									opennova::simassets::model_bound_radius_from_3di(
											husk_data->native_model());
							const int32_t husk_bound_q16 = static_cast<int32_t>(
									Math::round(static_cast<double>(husk_bound) * 65536.0));
							bound_q16 = MAX(bound_q16, husk_bound_q16);
						}
					}
					// The +0x1000 pad on the entity bound (Entity_InitFromModel @0x40dc30 -
					// docs/render/render-lighting-re.md).
					if (bound_q16 > 0) {
						entity_bound = static_cast<float>(bound_q16 + 0x1000) /
								65536.0f;
					}
				}
				p_model->set_shadow_bound_radii(model_sphere, entity_bound);
			}
		}
	}
	String decal_texture;
	Vector4 decal_dims;
	if (item_db_->get_shadow_decal(p_item_id, decal_texture, decal_dims)) {
		p_model->set_slot_shadow_decal(decal_texture, decal_dims);
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
	// Retail's building collector marks ROBJ 1+ as interior-lighting entries
	// while ROBJ 0 remains the exterior shell. Only portal buildings take
	// this model-section path (witness: placement_traits.h ledger,
	// per-section building visibility).
	if (item_db_->get_item_type(p_item_id) != ItemDatabase::TYPE_BUILDING ||
			!_has_occlusion_records(p_item_id)) {
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
	const String cache_key = p_adm_name + String("#") +
			String::num_int64(Variant(p_bone_origins).hash()) + String("#") +
			String::num_int64(Variant(p_bone_parents).hash());
	const Ref<SkeletalAnim> *cached = skeletal_cache_.getptr(cache_key);
	if (cached != nullptr) {
		return *cached;
	}
	Ref<SkeletalAnim> skeletal;
	skeletal.instantiate();
	if (!skeletal->load_from_resource_root(resource_root_, p_adm_name,
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

// Build a template ObjectModel, let it assemble the rest-pose meshes and
// fidelity materials, then harvest one batch per submesh. The model enters
// the live tree only for the harvest (so bounds/global-transform math is
// valid and silent), then frees; the harvested Mesh/Material refs survive.
// Each batch offset is the submesh's model-local rest transform relative to
// the entity origin — NO ground-anchor offset (the engine bakes the Ground
// userpoint into the stored position at author-time; witness:
// placement_traits.h ledger).
Vector<MissionObjectPlacer::StaticBatch>
MissionObjectPlacer::_get_static_batches(const String &p_graphic,
		Node *p_tree_parent) {
	const Vector<StaticBatch> *cached = static_batch_cache_.getptr(p_graphic);
	if (cached != nullptr) {
		return *cached;
	}
	Vector<StaticBatch> batches;
	const Ref<ObjectData> data = _load_object_data(p_graphic);
	if (data.is_valid() && p_tree_parent != nullptr) {
		ObjectModel *model = memnew(ObjectModel);
		p_tree_parent->add_child(model);
		model->set_object_data(data);
		model->rebuild();
		int submesh = 0;
		const Dictionary part_nodes = model->get_render_part_nodes();
		const Array part_keys = part_nodes.keys();
		for (int k = 0; k < part_keys.size(); ++k) {
			Node3D *part_node =
					Object::cast_to<Node3D>(part_nodes[part_keys[k]]);
			if (part_node == nullptr) {
				continue;
			}
			for (int c = 0; c < part_node->get_child_count(); ++c) {
				MeshInstance3D *mi =
						Object::cast_to<MeshInstance3D>(part_node->get_child(c));
				if (mi == nullptr || mi->get_mesh().is_null()) {
					continue;
				}
				StaticBatch batch;
				batch.mesh = mi->get_mesh();
				batch.material = mi->get_material_override();
				batch.offset = part_node->get_transform() * mi->get_transform();
				batch.submesh = submesh;
				batch.robj_index = int(part_keys[k]);
				batch.auxiliary_draw =
						bool(mi->get_meta("_opennova_auxiliary_draw", false));
				batch.blended_draw =
						bool(mi->get_meta("_opennova_blended_draw", false));
				batches.push_back(batch);
				++submesh;
			}
		}
		p_tree_parent->remove_child(model);
		memdelete(model);
	}
	static_batch_cache_[p_graphic] = batches;
	return batches;
}

// Visible portal/PANM models live below camera-masked ROBJ nodes; retail's
// terrain-tile collector ignores those masks and submits every selected-LOD
// ROBJ, so harvest one independent all-section shadow-only sibling per
// submesh.
void MissionObjectPlacer::_add_individual_static_shadow_siblings(
		ObjectModel *p_model, const String &p_graphic,
		const Transform3D &p_local_xform, const String &p_suffix) {
	if (p_model == nullptr) {
		return;
	}
	Vector<MeshInstance3D *> sources;
	LocalVector<Node *> stack;
	stack.push_back(p_model);
	while (!stack.is_empty()) {
		Node *parent = stack[stack.size() - 1];
		stack.remove_at(stack.size() - 1);
		for (int i = 0; i < parent->get_child_count(); ++i) {
			Node *child = parent->get_child(i);
			stack.push_back(child);
			MeshInstance3D *source = Object::cast_to<MeshInstance3D>(child);
			if (source != nullptr && source->get_mesh().is_valid() &&
					!bool(source->get_meta("_opennova_auxiliary_draw", false))) {
				sources.push_back(source);
			}
		}
	}
	// Registered/synthetic graphics can supply harvested static batches without
	// an ObjectModel scene. Keep that owner-facing seam useful while production
	// models clone their retained per-LOD MeshInstance descendants below.
	if (sources.is_empty()) {
		const Vector<StaticBatch> batches = _get_static_batches(p_graphic, p_model);
		for (const StaticBatch &batch : batches) {
			if (batch.auxiliary_draw || batch.mesh.is_null()) {
				continue;
			}
			Ref<MultiMesh> mm;
			mm.instantiate();
			mm->set_transform_format(MultiMesh::TRANSFORM_3D);
			mm->set_mesh(batch.mesh);
			mm->set_instance_count(1);
			mm->set_instance_transform(0, p_local_xform * batch.offset);
			MultiMeshInstance3D *mmi = memnew(MultiMeshInstance3D);
			mmi->set_multimesh(mm);
			mmi->set_layer_mask(Water::VISUAL_LAYER_STATIC_SHADOW_CASTER);
			mmi->set_cast_shadows_setting(
					GeometryInstance3D::SHADOW_CASTING_SETTING_SHADOWS_ONLY);
			if (batch.material.is_valid()) {
				mmi->set_material_override(batch.material);
			}
			mmi->set_name(vformat("StaticShadow_%s_%s_%d", p_graphic,
					p_suffix, batch.submesh));
			p_model->add_child(mmi);
		}
		return;
	}
	const Transform3D model_inverse = p_model->get_global_transform().affine_inverse();
	int submesh = 0;
	for (MeshInstance3D *source : sources) {
		Ref<MultiMesh> mm;
		mm.instantiate();
		mm->set_transform_format(MultiMesh::TRANSFORM_3D);
		mm->set_mesh(source->get_mesh());
		mm->set_instance_count(1);
		mm->set_instance_transform(0, p_local_xform * model_inverse *
				source->get_global_transform());
		MultiMeshInstance3D *mmi = memnew(MultiMeshInstance3D);
		mmi->set_multimesh(mm);
		mmi->set_layer_mask(Water::VISUAL_LAYER_STATIC_SHADOW_CASTER);
		mmi->set_cast_shadows_setting(
				GeometryInstance3D::SHADOW_CASTING_SETTING_SHADOWS_ONLY);
		const Ref<Material> material = source->get_material_override();
		if (material.is_valid()) {
			mmi->set_material_override(material);
		}
		if (source->has_meta("_opennova_lod_index")) {
			mmi->set_meta("_opennova_lod_index",
					source->get_meta("_opennova_lod_index"));
			mmi->set_visible(source->is_visible());
		}
		mmi->set_name(vformat("StaticShadow_%s_%s_%d", p_graphic, p_suffix,
				submesh++));
		p_model->add_child(mmi);
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
