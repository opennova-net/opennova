#include "mission/nova_mission_object_placer.h"

#include <godot_cpp/classes/collision_shape3d.hpp>
#include <godot_cpp/classes/convex_polygon_shape3d.hpp>
#include <godot_cpp/classes/geometry3d.hpp>
#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include "env/nova_water.h"
#include "mission/nova_mission_data.h"

namespace godot {

namespace {

constexpr const char *kContainerName = "MissionObjects";

uint64_t entity_identity_key(int p_kind, int p_index) noexcept {
	return (static_cast<uint64_t>(static_cast<uint32_t>(p_kind)) << 32) |
			static_cast<uint32_t>(p_index);
}

// Convex hull points (Godot model-local) for one parsed collision volume:
// the volume's bounding planes form a closed convex polytope, so the hull is
// exactly their half-space intersection (never clamped to the AABB — that
// would flatten carved/oriented hulls into axis-aligned boxes). A volume
// with too few or degenerate planes falls back to its AABB corners so it is
// never silently lost. (The BVOL family semantics are witnessed in
// docs/world/world-wac-ai-re.md §15.4; the display-side helpers stay in
// collision_hull.gd.)
PackedVector3Array hull_points_for_volume(const Dictionary &p_volume) {
	TypedArray<Plane> planes;
	const Array raw_planes = p_volume.get("planes", Array());
	for (int i = 0; i < raw_planes.size(); ++i) {
		if (raw_planes[i].get_type() == Variant::PLANE) {
			planes.push_back(raw_planes[i]);
		}
	}
	PackedVector3Array pts;
	if (planes.size() >= 4) {
		pts = Geometry3D::get_singleton()->compute_convex_mesh_points(planes);
	}
	if (pts.size() < 4) {
		const Vector3 vmin = p_volume.get("min", Vector3());
		const Vector3 vmax = p_volume.get("max", Vector3());
		if (vmin != vmax) {
			pts.clear();
			for (const double x : { vmin.x, vmax.x }) {
				for (const double y : { vmin.y, vmax.y }) {
					for (const double z : { vmin.z, vmax.z }) {
						pts.push_back(Vector3(x, y, z));
					}
				}
			}
		}
	}
	return pts;
}

Array collision_shapes_for_volumes(const Array &p_volumes) {
	Array shapes;
	for (int i = 0; i < p_volumes.size(); ++i) {
		if (p_volumes[i].get_type() != Variant::DICTIONARY) {
			continue;
		}
		const PackedVector3Array pts =
				hull_points_for_volume(p_volumes[i]);
		if (pts.size() < 4) {
			continue;
		}
		Ref<ConvexPolygonShape3D> shape;
		shape.instantiate();
		shape->set_points(pts);
		shapes.push_back(shape);
	}
	return shapes;
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
	ClassDB::bind_method(D_METHOD("set_edit_mode", "edit_mode"),
			&MissionObjectPlacer::set_edit_mode);
	ClassDB::bind_method(D_METHOD("get_edit_mode"),
			&MissionObjectPlacer::get_edit_mode);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "edit_mode"), "set_edit_mode",
			"get_edit_mode");
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
			D_METHOD("place_single", "mission", "container", "kind", "index"),
			&MissionObjectPlacer::place_single);
	ClassDB::bind_method(D_METHOD("build_animated_model", "item_id", "parent"),
			&MissionObjectPlacer::build_animated_model);
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
					"parent", "clip_key", "rig_graphic"),
			&MissionObjectPlacer::build_model_from_graphic, DEFVAL(String()),
			DEFVAL(String()));

	ClassDB::bind_method(D_METHOD("get_placed_entity_records"),
			&MissionObjectPlacer::get_placed_entity_records);
	ClassDB::bind_method(D_METHOD("set_placed_entity_records", "records"),
			&MissionObjectPlacer::set_placed_entity_records);
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "placed_entity_records"),
			"set_placed_entity_records", "get_placed_entity_records");
	ClassDB::bind_method(D_METHOD("get_pickable_records"),
			&MissionObjectPlacer::get_pickable_records);
	ClassDB::bind_method(D_METHOD("set_pickable_records", "records"),
			&MissionObjectPlacer::set_pickable_records);
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "pickable_records"),
			"set_pickable_records", "get_pickable_records");
	ClassDB::bind_method(D_METHOD("get_static_user_point_sources"),
			&MissionObjectPlacer::get_static_user_point_sources);
	ClassDB::bind_method(D_METHOD("get_static_item_effect_sources"),
			&MissionObjectPlacer::get_static_item_effect_sources);
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
	ClassDB::bind_method(D_METHOD("skeletal_anim_for", "item_id", "graphic"),
			&MissionObjectPlacer::skeletal_anim_for);
	ClassDB::bind_method(D_METHOD("ground_anchor_godot", "graphic"),
			&MissionObjectPlacer::ground_anchor_godot);
	ClassDB::bind_method(D_METHOD("ground_anchor_bms", "graphic"),
			&MissionObjectPlacer::ground_anchor_bms);
	ClassDB::bind_method(D_METHOD("collision_shapes_for", "graphic"),
			&MissionObjectPlacer::collision_shapes_for);
	ClassDB::bind_method(
			D_METHOD("add_pick_collider", "container", "kind", "index",
					"graphic", "entity_xform"),
			&MissionObjectPlacer::add_pick_collider);

	ClassDB::bind_method(D_METHOD("get_static_instance_transform", "bms_id"),
			&MissionObjectPlacer::get_static_instance_transform);
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
			D_METHOD("register_static_batches", "graphic", "batches"),
			&MissionObjectPlacer::register_static_batches);
	ClassDB::bind_method(
			D_METHOD("register_ground_anchor", "graphic", "anchor"),
			&MissionObjectPlacer::register_ground_anchor);
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
	anchor_cache_.clear();
	collision_shapes_cache_.clear();
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
	pickable_records_ = Array();
	destruction_batches_.clear();
	destruction_instances_.clear();
	hidden_destruction_instances_.clear();
	static_terrain_shadow_replacements_.clear();
	static_user_point_sources_ = Array();
	static_item_effect_sources_ = Array();
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
		Array edit_refs;
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
		const Transform3D xform = entity_transform(
				entity.get("position", Vector3()),
				entity.get("rotation_deg", Vector3()));
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
		source["item_id"] = item_id;
		source["world_transform"] = xform;
		group->effect_sources.push_back(source);
		if (edit_mode_) {
			Dictionary ref;
			ref["kind"] = kind;
			ref["index"] = int(entity.get("index", -1));
			group->edit_refs.push_back(ref);
		}
	}
	stats["markers"] = markers;
	spans["bucket_entities"] = clock->get_ticks_usec() - stage_begin;
	stage_begin = clock->get_ticks_usec();

	// Static: one MultiMeshInstance3D per
	// (graphic, retail reflection population, submesh).
	int placed = 0;
	int batched = 0;
	int graphics = 0;
	int batch_count = 0;
	Vector<String> resolved_graphics;
	Vector<String> resolved_group_keys;
	for (const String &group_key : static_order) {
		if (progress.is_valid()) {
			progress.call();
		}
		StaticGroup &group = static_groups[group_key];
		const String graphic = group.graphic;
		const int instance_count = group.xforms.size();
		bool has_static_shadow = false;
		bool all_static_shadow = instance_count > 0;
		for (const bool slot : group.shadow_slots) {
			has_static_shadow = has_static_shadow || slot;
			all_static_shadow = all_static_shadow && slot;
		}
		const uint32_t batch_world_layer = group.mirror_reflected
				? uint32_t(Water::VISUAL_LAYER_WORLD)
				: uint32_t(Water::VISUAL_LAYER_WORLD_NO_MIRROR);
		const bool graphic_has_split_policy = static_groups.has(
				static_group_key(graphic, !group.mirror_reflected));
		const String policy_suffix = graphic_has_split_policy
				? (group.mirror_reflected ? "_Mirror" : "_NoMirror")
				: String();
		Array shadow_bms_ids;
		Array shadow_item_ids;
		Array shadow_attrib2;
		Array shadow_slots;
		for (int i = 0; i < instance_count; ++i) {
			shadow_bms_ids.push_back(i < group.bms_ids.size()
					? group.bms_ids[i]
					: 0);
			shadow_item_ids.push_back(i < group.item_ids.size()
					? group.item_ids[i]
					: 0);
			shadow_attrib2.push_back(i < group.attrib2_values.size()
					? int64_t(group.attrib2_values[i])
					: int64_t(0));
			shadow_slots.push_back(i < group.shadow_slots.size() &&
					group.shadow_slots[i]);
		}
		const auto tag_static_shadow_source = [&](MultiMeshInstance3D *p_source) {
			p_source->set_meta("static_shadow_bms_ids", shadow_bms_ids);
			p_source->set_meta("static_shadow_item_ids", shadow_item_ids);
			p_source->set_meta("static_shadow_attrib2", shadow_attrib2);
			p_source->set_meta("static_shadow_slots", shadow_slots);
			p_source->set_meta("static_shadow_graphic", graphic);
			p_source->set_meta("static_shadow_batch_key", group_key);
		};
		const Vector<StaticBatch> batches =
				_get_static_batches(graphic, container);
		if (batches.is_empty()) {
			unresolved += instance_count;
			continue;
		}
		resolved_group_keys.push_back(group_key);
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
		for (int i = 0; i < group.effect_sources.size(); ++i) {
			Dictionary source = group.effect_sources[i];
			_append_static_item_effect_source(int(source.get("kind", -1)),
					int(source.get("item_id", 0)), graphic,
					source.get("world_transform", Transform3D()));
		}
		for (const StaticBatch &batch : batches) {
			Ref<MultiMesh> shadow_mm;
			Ref<MultiMesh> mm;
			mm.instantiate();
			mm->set_transform_format(MultiMesh::TRANSFORM_3D);
			mm->set_mesh(batch.mesh);
			mm->set_instance_count(instance_count);
			for (int i = 0; i < instance_count; ++i) {
				mm->set_instance_transform(i, group.xforms[i] * batch.offset);
			}
			MultiMeshInstance3D *mmi = memnew(MultiMeshInstance3D);
			mmi->set_multimesh(mm);
			if (all_static_shadow) {
				// The reimpl's static directional approximation reaches only
				// the terrain receiver layer, so an all-eligible visible
				// batch carries the static-caster marker without
				// self-shadowing — no duplicate MultiMesh per submesh.
				mmi->set_layer_mask(batch_world_layer |
						Water::VISUAL_LAYER_STATIC_SHADOW_CASTER);
				mmi->set_cast_shadows_setting(
						GeometryInstance3D::SHADOW_CASTING_SETTING_ON);
			} else {
				mmi->set_layer_mask(batch_world_layer);
				mmi->set_cast_shadows_setting(
						GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
			}
			if (batch.material.is_valid()) {
				mmi->set_material_override(batch.material);
			}
			mmi->set_name(vformat("Batch_%s%s_%d", graphic, policy_suffix,
					batch.submesh));
			tag_static_shadow_source(mmi);
			container->add_child(mmi);
			++batch_count;
			destruction_batches_[group_key].push_back(mm);
			if (has_static_shadow && !all_static_shadow) {
				// Mixed eligibility: a shadows-only twin whose ineligible
				// slots collapse to zero scale.
				shadow_mm.instantiate();
				shadow_mm->set_transform_format(MultiMesh::TRANSFORM_3D);
				shadow_mm->set_mesh(batch.mesh);
				shadow_mm->set_instance_count(instance_count);
				for (int i = 0; i < instance_count; ++i) {
					Transform3D shadow_xform = group.xforms[i] * batch.offset;
					if (i >= group.shadow_slots.size() ||
							!group.shadow_slots[i]) {
						shadow_xform.basis =
								shadow_xform.basis.scaled(Vector3());
					}
					shadow_mm->set_instance_transform(i, shadow_xform);
				}
				MultiMeshInstance3D *shadow_mmi = memnew(MultiMeshInstance3D);
				shadow_mmi->set_multimesh(shadow_mm);
				shadow_mmi->set_layer_mask(
						Water::VISUAL_LAYER_STATIC_SHADOW_CASTER);
				shadow_mmi->set_cast_shadows_setting(
						GeometryInstance3D::SHADOW_CASTING_SETTING_SHADOWS_ONLY);
				if (batch.material.is_valid()) {
					shadow_mmi->set_material_override(batch.material);
				}
				shadow_mmi->set_name(vformat("StaticShadow_%s%s_%d", graphic,
						policy_suffix, batch.submesh));
				tag_static_shadow_source(shadow_mmi);
				container->add_child(shadow_mmi);
				destruction_batches_[group_key].push_back(shadow_mm);
			}
			if (edit_mode_) {
				Array shadow_slot_array;
				for (const bool slot : group.shadow_slots) {
					shadow_slot_array.push_back(slot);
				}
				_record_static_batch(graphic, group.edit_refs, mm, mmi,
						batch.offset, batch.mesh, shadow_mm,
						shadow_slot_array);
			}
		}
		batched += instance_count;
		placed += instance_count;
		for (int i = 0;
				i < MIN(group.bms_ids.size(), group.xforms.size()); ++i) {
			const int bms_id = group.bms_ids[i];
			if (bms_id != 0) {
				DestructionInstance inst;
				inst.graphic = graphic;
				inst.batch_key = group_key;
				inst.index = i;
				inst.xform = group.xforms[i];
				inst.casts_static_shadow = i < group.shadow_slots.size() &&
						group.shadow_slots[i];
				inst.mirror_reflected = group.mirror_reflected;
				destruction_instances_[bms_id] = inst;
			}
		}
	}
	spans["static_batches"] = clock->get_ticks_usec() - stage_begin;
	stage_begin = clock->get_ticks_usec();

	// Align every harvested batch material with the env AS OF placement end —
	// the throwaway-template harvest sees mid-load values (e.g. the modulator
	// before its first iris tick).
	// One pick collider per static entity (not per submesh): collision is
	// whole-model; pick bodies are addressed by name ("Pick_<kind>_<index>"),
	// never by child order.
	if (edit_mode_) {
		for (const String &group_key : resolved_group_keys) {
			const StaticGroup &group = static_groups[group_key];
			const String graphic = group.graphic;
			for (int i = 0; i < group.xforms.size(); ++i) {
				Dictionary ref;
				if (i < group.edit_refs.size()) {
					ref = group.edit_refs[i];
				}
				add_pick_collider(container, int(ref.get("kind", -1)),
						int(ref.get("index", -1)), graphic, group.xforms[i]);
			}
		}
		spans["pick_colliders"] = clock->get_ticks_usec() - stage_begin;
		stage_begin = clock->get_ticks_usec();
	}

	// Animated: an individual ObjectModel per entity.
	int animated_count = 0;
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
		// Render the model origin at the entity's stored position directly:
		// the engine bakes the Ground userpoint into the stored position at
		// author-time (place / terrain-drag), not at render (witness:
		// placement_traits.h ledger, author-time Ground bake).
		model->set_transform(a.get("xform", Transform3D()));
		container->add_child(model);
		_configure_item_shadow(model, item_id);
		// Load the entity's body-animation set (.adm) BEFORE the data:
		// setting it first is a no-op rebuild, so set_object_data below does
		// the ONE skeletal-keyed mesh build.
		_apply_skeletal_anim(model, item_id, data->get_bone_origins(),
				data->get_bone_parents());
		// The def names the AI muzzle: items.def launchups_closeattack is
		// the launch userpoint on this item's graphic
		// (world-wac-ai-re §21.2).
		model->set_muzzle_point_name(
				item_db_->get_launchups_closeattack(item_id));
		// Drive the build explicitly (not via _ready) so it is independent
		// of when place() runs relative to the main loop.
		model->set_object_data(data);
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
		if (edit_mode_) {
			Dictionary pick;
			pick["kind"] = ref["kind"];
			pick["index"] = ref["index"];
			pick["graphic"] = graphic;
			pick["node"] = model;
			pick["offset"] = Transform3D();
			pick["animated"] = true;
			pickable_records_.push_back(pick);
			add_pick_collider(container, int(ref["kind"]), int(ref["index"]),
					graphic, a.get("xform", Transform3D()));
		}
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
	stats["spans"] = spans;
	return stats;
}

Dictionary MissionObjectPlacer::place_single(const Ref<MissionData> &p_mission,
		Node3D *p_container, int p_kind, int p_index) {
	_check_epoch();
	Dictionary delta;
	delta["placed"] = 0;
	delta["batched"] = 0;
	delta["animated"] = 0;
	delta["batches"] = 0;
	delta["unresolved"] = 0;
	if (p_mission.is_null() || p_container == nullptr ||
			resource_root_.is_null()) {
		return delta;
	}
	_ensure_item_db();
	const Dictionary entity = p_mission->get_entity(p_kind, p_index);
	if (entity.is_empty()) {
		return delta;
	}
	const int item_id = int(entity.get("item_id", 0));
	const String graphic = _graphic_for(item_id);
	if (graphic.is_empty()) {
		delta["unresolved"] = 1;
		return delta;
	}
	const Transform3D xform = entity_transform(
			entity.get("position", Vector3()),
			entity.get("rotation_deg", Vector3()));

	if (_needs_individual_node(item_id) || _graphic_needs_live_panm(graphic)) {
		const Ref<ObjectData> data = _load_object_data(graphic);
		if (data.is_null()) {
			delta["unresolved"] = 1;
			return delta;
		}
		ObjectModel *model = memnew(ObjectModel);
		model->set_panm_clock(panm_clock_);
		model->set_name(vformat("Anim_%s_k%d_i%d", graphic, p_kind, p_index));
		model->set_mirror_reflected(_placement_is_mirror_reflected(
				uint32_t(entity.get("ai_flags", 0)), item_id));
		model->set_transform(xform);
		p_container->add_child(model);
		_configure_item_shadow(model, item_id);
		_apply_skeletal_anim(model, item_id, data->get_bone_origins(),
				data->get_bone_parents());
		model->set_object_data(data);
		if (item_casts_static_terrain_shadow(p_kind,
					uint32_t(entity.get("ai_flags", 0)),
					item_db_->get_attrib(item_id),
					item_db_->get_attrib2(item_id))) {
			_add_individual_static_shadow_siblings(model, graphic,
					Transform3D(), vformat("k%d_i%d", p_kind, p_index));
		}
		Dictionary ref;
		ref["kind"] = p_kind;
		ref["index"] = p_index;
		ref["bms_id"] = int(entity.get("bms_id", 0));
		ref["group"] = int(entity.get("group", -1));
		ref["team"] = int(entity.get("team", -1));
		ref["position"] = entity.get("position", Vector3());
		ref["item_id"] = item_id;
		ref["graphic"] = graphic;
		ref["attrib2"] = int64_t(item_db_->get_attrib2(item_id));
		model->set_meta("entity_ref", ref);
		Dictionary record;
		record["model"] = model;
		record["ref"] = ref;
		placed_entity_records_.push_back(record);
		_record_static_terrain_shadow_source(p_kind, p_index,
				int(entity.get("bms_id", 0)),
				int(entity.get("team", 0)),
				uint32_t(entity.get("ai_flags", 0)), item_id, graphic,
				xform, data);
		Dictionary pick;
		pick["kind"] = p_kind;
		pick["index"] = p_index;
		pick["graphic"] = graphic;
		pick["node"] = model;
		pick["offset"] = Transform3D();
		pick["animated"] = true;
		pickable_records_.push_back(pick);
		add_pick_collider(p_container, p_kind, p_index, graphic, xform);
		delta["placed"] = 1;
		delta["animated"] = 1;
		return delta;
	}

	const Vector<StaticBatch> batches =
			_get_static_batches(graphic, p_container);
	if (batches.is_empty()) {
		delta["unresolved"] = 1;
		return delta;
	}
	// A first-seen graphic just harvested fresh materials mid-load; align
	// them with the live env like place() does.
	Array refs;
	{
		Dictionary ref;
		ref["kind"] = p_kind;
		ref["index"] = p_index;
		refs.push_back(ref);
	}
	const bool casts_static_shadow = item_casts_static_terrain_shadow(p_kind,
			uint32_t(entity.get("ai_flags", 0)), item_db_->get_attrib(item_id),
			item_db_->get_attrib2(item_id));
	const bool single_mirror_reflected = _placement_is_mirror_reflected(
			uint32_t(entity.get("ai_flags", 0)), item_id);
	const uint32_t single_world_layer = single_mirror_reflected
			? uint32_t(Water::VISUAL_LAYER_WORLD)
			: uint32_t(Water::VISUAL_LAYER_WORLD_NO_MIRROR);
	int batch_count = 0;
	for (const StaticBatch &batch : batches) {
		Ref<MultiMesh> mm;
		mm.instantiate();
		mm->set_transform_format(MultiMesh::TRANSFORM_3D);
		mm->set_mesh(batch.mesh);
		mm->set_instance_count(1);
		mm->set_instance_transform(0, xform * batch.offset);
		MultiMeshInstance3D *mmi = memnew(MultiMeshInstance3D);
		mmi->set_multimesh(mm);
		if (casts_static_shadow) {
			mmi->set_layer_mask(single_world_layer |
					Water::VISUAL_LAYER_STATIC_SHADOW_CASTER);
			mmi->set_cast_shadows_setting(
					GeometryInstance3D::SHADOW_CASTING_SETTING_ON);
		} else {
			mmi->set_layer_mask(single_world_layer);
			mmi->set_cast_shadows_setting(
					GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
		}
		if (batch.material.is_valid()) {
			mmi->set_material_override(batch.material);
		}
		mmi->set_name(vformat("Place_%s_k%d_i%d_s%d", graphic, p_kind,
				p_index, batch.submesh));
		mmi->set_meta("static_shadow_bms_ids",
				Array::make(int(entity.get("bms_id", 0))));
		mmi->set_meta("static_shadow_item_ids", Array::make(item_id));
		mmi->set_meta("static_shadow_attrib2",
				Array::make(int64_t(item_db_->get_attrib2(item_id))));
		mmi->set_meta("static_shadow_slots", Array::make(casts_static_shadow));
		mmi->set_meta("static_shadow_graphic", graphic);
		p_container->add_child(mmi);
		++batch_count;
		_record_static_batch(graphic, refs, mm, mmi, batch.offset, batch.mesh);
	}
	add_pick_collider(p_container, p_kind, p_index, graphic, xform);
	_append_static_user_point_source(graphic, xform);
	_append_static_item_effect_source(p_kind, item_id, graphic, xform);
	Ref<ObjectData> shadow_data;
	if (const Ref<ObjectData> *resolved = object_data_cache_.getptr(graphic)) {
		shadow_data = *resolved;
	}
	_record_static_terrain_shadow_source(p_kind, p_index,
			int(entity.get("bms_id", 0)),
			int(entity.get("team", 0)),
			uint32_t(entity.get("ai_flags", 0)), item_id, graphic, xform,
			shadow_data);
	delta["placed"] = 1;
	delta["batched"] = 1;
	delta["batches"] = batch_count;
	return delta;
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
	model->set_name(vformat("PlayerAvatar_%s", graphic));
	// Wire-streamed and avatar builds share this chain: vehicles reflect in
	// the water mirror, persons and everything else never do (env #30).
	model->set_mirror_reflected(_item_is_mirror_reflected(p_item_id));
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
// after its own camo store (retail: Terrain_RenderSectorEntitiesBySide
// @0x5c7fea..0x5c8020: CharacterEntity blip +4 head then +0 body, see docs/playerinfo/avatars-re.md)) and the arms
// model in first person. `fallback` = no combo resolved: with an EMPTY registry
// retail draws the entity's own item model @0x5c8039 (blip handles 0), which is
// what this returns. Divergence, tracked in avatars-re D-PLAYERINFO-1: with a
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
	// (retail: Terrain_RenderSectorEntitiesBySide @0x5c7fdf-0x5c7fea —
	//  jz to the forced_model 0 item path on either zero handle)
	if (body_graphic.is_empty() || head_graphic.is_empty()) {
		return build_animated_model(item_id, p_parent);
	}
	const String adm_name = item_db_.is_valid()
			? item_db_->get_anim_def(item_id)
			: String();
	ObjectModel *body = build_model_from_graphic(body_graphic, adm_name,
			p_parent, String(), body_graphic);
	if (body == nullptr) {
		return build_animated_model(item_id, p_parent);
	}
	// Same node naming as the item-model path (PlayerAvatar_<graphic>): the
	// composed body IS the player's avatar node; the head rides under it.
	body->set_name(vformat("PlayerAvatar_%s", body_graphic.get_file().get_basename()));
	body->set_meta("avatar_part", "body");
	body->set_meta("character_id", p_character_id & 0xffff);
	body->set_meta("player_visual_spec", spec);
	// (retail: Avatar_SetBodyCamoCtrl @0x57a390 immediately before the body submit
	// @0x5c800f, see docs/playerinfo/avatars-re.md)
	AvatarDatabase::apply_part_camo(body, spec.get("body_camo", Array()),
			"player_avatar:body_camo");
	body->set_mirror_reflected(_item_is_mirror_reflected(item_id));
	body->set_muzzle_point_name(item_db_.is_valid()
			? item_db_->get_launchups_closeattack(item_id)
			: String());
	_configure_item_shadow(body, item_id);

	ObjectModel *head = build_model_from_graphic(head_graphic, adm_name,
			body, String(), body_graphic);
	if (head == nullptr) {
		// A head that fails to build mirrors the zero-handle case: tear the
		// composed body down and render the plain item model instead of a
		// headless avatar. (retail: the both-or-neither gate above)
		body->queue_free();
		return build_animated_model(item_id, p_parent);
	}
	head->set_name(vformat("PlayerAvatarHead_%s",
			head_graphic.get_file().get_basename()));
	head->set_meta("avatar_part", "head");
	head->set_meta("character_id", p_character_id & 0xffff);
	// (retail: Avatar_SetHeadCamoCtrl @0x57a370 immediately before the
	// head submit @0x5c7fec, see docs/playerinfo/avatars-re.md)
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
		const String &p_clip_key, const String &p_rig_graphic) {
	if (p_graphic.is_empty() || p_parent == nullptr) {
		return nullptr;
	}
	const Ref<ObjectData> data = _load_object_data(p_graphic);
	if (data.is_null()) {
		return nullptr;
	}
	ObjectModel *model = memnew(ObjectModel);
	model->set_panm_clock(panm_clock_);
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

void MissionObjectPlacer::_configure_item_shadow(ObjectModel *p_model,
		int p_item_id) {
	if (p_model == nullptr || item_db_.is_null()) {
		return;
	}
	p_model->set_shadow_caster_enabled(item_casts_dynamic_shadow(
			item_db_->get_item_type(p_item_id),
			item_db_->get_attrib(p_item_id), item_db_->get_attrib2(p_item_id)));
	// The static tile pass must ignore the visible model's portal/section
	// mask; eligible mission entities get independent all-section siblings
	// after their visible model is built.
	p_model->set_static_shadow_caster_enabled(false);
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
	const Vector<StaticBatch> batches = _get_static_batches(p_graphic, p_model);
	for (const StaticBatch &batch : batches) {
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
		mmi->set_name(vformat("StaticShadow_%s_%s_%d", p_graphic, p_suffix,
				batch.submesh));
		p_model->add_child(mmi);
	}
}

// One pickable record per entity slot in a freshly-built static batch: slot
// `i` is consistent across every submesh batch of the same graphic, so
// moving entity i rewrites instance i in every batch sharing its graphic;
// mesh_aabb (under the instance transform) gives the editor a tight pick
// volume without per-instance physics bodies.
void MissionObjectPlacer::_record_static_batch(const String &p_graphic,
		const Array &p_refs, const Ref<MultiMesh> &p_mm,
		MultiMeshInstance3D *p_mmi, const Transform3D &p_offset,
		const Ref<Mesh> &p_mesh, const Ref<MultiMesh> &p_shadow_mm,
		const Array &p_shadow_slots) {
	const AABB mesh_aabb = p_mesh.is_valid() ? p_mesh->get_aabb() : AABB();
	for (int i = 0; i < p_mm->get_instance_count(); ++i) {
		Dictionary ref;
		if (i < p_refs.size()) {
			ref = p_refs[i];
		}
		Dictionary pick;
		pick["kind"] = int(ref.get("kind", -1));
		pick["index"] = int(ref.get("index", -1));
		pick["graphic"] = p_graphic;
		pick["slot"] = i;
		pick["mm"] = p_mm;
		pick["mmi"] = p_mmi;
		pick["shadow_mm"] = p_shadow_mm;
		pick["casts_static_shadow"] = i < p_shadow_slots.size() &&
				bool(p_shadow_slots[i]);
		pick["offset"] = p_offset;
		pick["mesh_aabb"] = mesh_aabb;
		pick["animated"] = false;
		pickable_records_.push_back(pick);
	}
}

// --- read-back seams ---------------------------------------------------------

// Snapshot of successfully rendered static user-point sources for a world
// debug view; transform arrays are duplicated so a consumer cannot mutate
// the placer's placement record.
Array MissionObjectPlacer::get_static_user_point_sources() {
	_check_epoch();
	Array out;
	for (int i = 0; i < static_user_point_sources_.size(); ++i) {
		const Dictionary row = static_user_point_sources_[i];
		Dictionary copy;
		copy["graphic"] = row.get("graphic", String());
		copy["object_data"] = row.get("object_data", Variant());
		copy["transforms"] = Array(row.get("transforms", Array())).duplicate();
		out.push_back(copy);
	}
	return out;
}

// Snapshot of successfully rendered static entities for mission-start item
// effects. Every row is a value descriptor; no placed/render Node is
// exposed; row order is placement order and stable for the mission.
Array MissionObjectPlacer::get_static_item_effect_sources() {
	_check_epoch();
	return static_item_effect_sources_.duplicate(true);
}

void MissionObjectPlacer::_record_static_terrain_shadow_source(int p_kind,
		int p_index, int p_bms_id, int p_team, uint32_t p_entity_attrib,
		int p_item_id, const String &p_graphic,
		const Transform3D &p_xform, const Ref<ObjectData> &p_data) {
	// The retail collector walks only pool 2 then pool 1. Keeping rejected
	// records from those pools preserves the exact policy inputs for the
	// portable admission predicate and its diagnostics.
	if (p_kind != opennova::mission::kEntityKindBuilding &&
			p_kind != opennova::mission::kEntityKindItem) {
		return;
	}
	StaticTerrainShadowSource source;
	source.bms_id = p_bms_id;
	source.item_id = p_item_id;
	source.entity_kind = p_kind;
	source.entity_index = p_index;
	source.team = p_team;
	source.entity_attrib = p_entity_attrib;
	if (item_db_.is_valid()) {
		source.item_attrib = item_db_->get_attrib(p_item_id);
		source.item_attrib2 = item_db_->get_attrib2(p_item_id);
	}
	source.graphic = p_graphic;
	source.world_transform = p_xform;
	source.object_data = p_data;
	const int row = static_terrain_shadow_sources_.size();
	static_terrain_shadow_sources_.push_back(source);
	static_terrain_shadow_source_rows_[entity_identity_key(p_kind, p_index)]
			.push_back(row);
	if (source.bms_id != 0) {
		static_terrain_shadow_rows_by_bms_[source.bms_id].push_back(row);
	}
	_bump_static_terrain_shadow_source_revision();
}

void MissionObjectPlacer::_bump_static_terrain_shadow_source_revision() {
	++static_terrain_shadow_source_revision_;
	if (static_terrain_shadow_source_revision_ == 0) {
		++static_terrain_shadow_source_revision_;
	}
}

uint64_t MissionObjectPlacer::get_static_terrain_shadow_source_revision() {
	_check_epoch();
	return static_terrain_shadow_source_revision_;
}

Vector<MissionObjectPlacer::StaticTerrainShadowSource>
MissionObjectPlacer::get_static_terrain_shadow_sources() {
	_check_epoch();
	Vector<StaticTerrainShadowSource> out = static_terrain_shadow_sources_;
	for (int i = 0; i < out.size(); ++i) {
		StaticTerrainShadowSource &source = out.write[i];
		if (source.object_data.is_null() && !source.graphic.is_empty()) {
			source.object_data = _load_object_data(source.graphic);
		}
		if (source.bms_id == 0) continue;
		if (const DestructionInstance *instance =
					destruction_instances_.getptr(source.bms_id)) {
			source.world_transform = instance->xform;
		}
		source.active = !hidden_destruction_instances_.has(source.bms_id);
		if (const StaticTerrainShadowSource *replacement =
					static_terrain_shadow_replacements_.getptr(source.bms_id)) {
			source.graphic = replacement->graphic;
			source.world_transform = replacement->world_transform;
			source.object_data = replacement->object_data.is_valid()
					? replacement->object_data
					: _load_object_data(replacement->graphic);
			source.active = replacement->active;
		}
	}

	// `register_static_instance` is the deterministic construction seam for
	// already-resolved renderers and asset-free tests. Merge any record not
	// already published by place()/place_single() as a building-policy source;
	// casts=false maps to the same authored NoShadow veto the collector owns.
	for (const KeyValue<int64_t, DestructionInstance> &kv :
			destruction_instances_) {
		bool represented = false;
		for (const StaticTerrainShadowSource &source : out) {
			if (source.bms_id == static_cast<int>(kv.key)) {
				represented = true;
				break;
			}
		}
		if (represented) continue;
		StaticTerrainShadowSource source;
		source.bms_id = static_cast<int>(kv.key);
		source.entity_kind = opennova::mission::kEntityKindBuilding;
		source.entity_index = kv.value.index;
		source.entity_attrib = kv.value.casts_static_shadow
				? 0u
				: opennova::mission::kEntityAttribNoShadow;
		source.graphic = kv.value.graphic;
		source.world_transform = kv.value.xform;
		source.object_data = _load_object_data(source.graphic);
		source.active = !hidden_destruction_instances_.has(kv.key);
		if (const StaticTerrainShadowSource *replacement =
					static_terrain_shadow_replacements_.getptr(kv.key)) {
			source.graphic = replacement->graphic;
			source.world_transform = replacement->world_transform;
			source.object_data = replacement->object_data.is_valid()
					? replacement->object_data
					: _load_object_data(replacement->graphic);
			source.active = replacement->active;
		}
		out.push_back(source);
	}
	return out;
}

Array MissionObjectPlacer::get_static_terrain_shadow_source_diagnostics() {
	Array out;
	const Vector<StaticTerrainShadowSource> sources =
			get_static_terrain_shadow_sources();
	for (const StaticTerrainShadowSource &source : sources) {
		Dictionary row;
		row["bms_id"] = source.bms_id;
		row["item_id"] = source.item_id;
		row["entity_kind"] = source.entity_kind;
		row["entity_index"] = source.entity_index;
		row["team"] = source.team;
		row["entity_attrib"] = static_cast<int64_t>(source.entity_attrib);
		row["item_attrib"] = static_cast<int64_t>(source.item_attrib);
		row["item_attrib2"] = static_cast<int64_t>(source.item_attrib2);
		row["graphic"] = source.graphic;
		row["world_transform"] = source.world_transform;
		row["object_data"] = source.object_data;
		row["active"] = source.active;
		out.push_back(row);
	}
	return out;
}

void MissionObjectPlacer::_record_static_user_point_group(
		const String &p_graphic, const Array &p_transforms) {
	const Ref<ObjectData> data = _load_object_data(p_graphic);
	if (data.is_null() || data->get_user_point_count() <= 0) {
		return;
	}
	Dictionary row;
	row["graphic"] = p_graphic;
	row["object_data"] = data;
	row["transforms"] = p_transforms.duplicate();
	static_user_point_sources_.push_back(row);
}

void MissionObjectPlacer::_append_static_user_point_source(
		const String &p_graphic, const Transform3D &p_xform) {
	const Ref<ObjectData> data = _load_object_data(p_graphic);
	if (data.is_null() || data->get_user_point_count() <= 0) {
		return;
	}
	for (int i = 0; i < static_user_point_sources_.size(); ++i) {
		Dictionary row = static_user_point_sources_[i];
		if (String(row.get("graphic", String())) != p_graphic) {
			continue;
		}
		Array transforms = Array(row.get("transforms", Array())).duplicate();
		transforms.push_back(p_xform);
		row["transforms"] = transforms;
		static_user_point_sources_[i] = row;
		return;
	}
	Dictionary row;
	row["graphic"] = p_graphic;
	row["object_data"] = data;
	Array transforms;
	transforms.push_back(p_xform);
	row["transforms"] = transforms;
	static_user_point_sources_.push_back(row);
}

void MissionObjectPlacer::_append_static_item_effect_source(int p_kind,
		int p_item_id, const String &p_graphic, const Transform3D &p_xform) {
	const Ref<ObjectData> data = _load_object_data(p_graphic);
	if (data.is_null()) {
		return;
	}
	Dictionary row;
	row["kind"] = p_kind;
	row["item_id"] = p_item_id;
	row["graphic"] = p_graphic;
	row["world_transform"] = p_xform;
	row["object_data"] = data;
	static_item_effect_sources_.push_back(row);
}

String MissionObjectPlacer::graphic_for(int p_item_id) {
	_ensure_item_db();
	return _graphic_for(p_item_id);
}

Ref<ObjectData> MissionObjectPlacer::object_data_for(const String &p_graphic) {
	_check_epoch();
	return _load_object_data(p_graphic);
}

// Authoritative read-only skeletal set for simulation collision: the same
// ADM + canonical model bone-table cache as the rendered ObjectModel, so
// headless per-bone collision cannot drift onto lossy BAD parents/pivots.
Ref<SkeletalAnim> MissionObjectPlacer::skeletal_anim_for(int p_item_id,
		const String &p_graphic) {
	_check_epoch();
	if (resource_root_.is_null() || item_db_.is_null()) {
		return Ref<SkeletalAnim>();
	}
	const Ref<ObjectData> data = _load_object_data(p_graphic);
	if (data.is_null()) {
		return Ref<SkeletalAnim>();
	}
	const String anim_def = item_db_->get_anim_def(p_item_id);
	if (anim_def.is_empty()) {
		return Ref<SkeletalAnim>();
	}
	const String adm_name = anim_def.to_lower().ends_with(".adm")
			? anim_def
			: anim_def + String(".adm");
	return _skeletal_from_adm(adm_name, data->get_bone_origins(),
			data->get_bone_parents());
}

// The Godot model-local ground reference point for `graphic`: the "ground"
// userpoint if present, else part-0 center. Not applied at render — the
// editor subtracts it (in BMS axes) from a terrain-drop position, mirroring
// the engine's author-time bake (witness: placement_traits.h ledger).
Vector3 MissionObjectPlacer::_ground_anchor_for(const String &p_graphic,
		const Ref<ObjectData> &p_data) {
	const Vector3 *cached = anchor_cache_.getptr(p_graphic);
	if (cached != nullptr) {
		return *cached;
	}
	Vector3 anchor;
	if (p_data.is_valid()) {
		anchor = p_data->get_ground_anchor(RENDER_LOD);
	}
	anchor_cache_[p_graphic] = anchor;
	return anchor;
}

Vector3 MissionObjectPlacer::ground_anchor_godot(const String &p_graphic) {
	_check_epoch();
	return _ground_anchor_for(p_graphic, _load_object_data(p_graphic));
}

// The (x, y, z) -> (x, -z, y) axis map is linear, so it is valid on offset
// vectors like the anchor, not just points.
Vector3 MissionObjectPlacer::ground_anchor_bms(const String &p_graphic) {
	return godot_to_bms_position(ground_anchor_godot(p_graphic));
}

// Convex collision hulls in model-local space for the editor's pickable
// physics bodies — the SAME path the Object Editor overlay validates. Models
// with no collision volumes fall back to a single box hull from the visual
// model AABB so every placed entity stays pickable.
Array MissionObjectPlacer::collision_shapes_for(const String &p_graphic) {
	_check_epoch();
	const Array *cached = collision_shapes_cache_.getptr(p_graphic);
	if (cached != nullptr) {
		return *cached;
	}
	Array shapes;
	const Ref<ObjectData> data = _load_object_data(p_graphic);
	if (data.is_valid()) {
		shapes = collision_shapes_for_volumes(data->get_collision_volumes());
	}
	if (shapes.is_empty()) {
		const AABB aabb = _visual_model_aabb(data);
		if (aabb.size != Vector3()) {
			PackedVector3Array pts;
			for (const double x : { aabb.position.x, aabb.get_end().x }) {
				for (const double y : { aabb.position.y, aabb.get_end().y }) {
					for (const double z :
							{ aabb.position.z, aabb.get_end().z }) {
						pts.push_back(Vector3(x, y, z));
					}
				}
			}
			Ref<ConvexPolygonShape3D> box;
			box.instantiate();
			box->set_points(pts);
			shapes.push_back(box);
		}
	}
	collision_shapes_cache_[p_graphic] = shapes;
	return shapes;
}

// One StaticBody3D pick collider for entity (kind,index) with an
// "entity_ref" meta the editor reads back from intersect_ray; freed
// automatically when the container is cleared/re-baked.
StaticBody3D *MissionObjectPlacer::add_pick_collider(Node3D *p_container,
		int p_kind, int p_index, const String &p_graphic,
		const Transform3D &p_entity_xform) {
	const Array shapes = collision_shapes_for(p_graphic);
	if (shapes.is_empty()) {
		return nullptr;
	}
	StaticBody3D *body = memnew(StaticBody3D);
	body->set_name(vformat("Pick_%d_%d", p_kind, p_index));
	Dictionary ref;
	ref["kind"] = p_kind;
	ref["index"] = p_index;
	body->set_meta("entity_ref", ref);
	body->set_transform(p_entity_xform);
	for (int i = 0; i < shapes.size(); ++i) {
		CollisionShape3D *cs = memnew(CollisionShape3D);
		cs->set_shape(shapes[i]);
		body->add_child(cs);
	}
	p_container->add_child(body);
	return body;
}

// Merged AABB of the render submeshes (model-local) — the collision fallback
// for models with no collision volumes.
AABB MissionObjectPlacer::_visual_model_aabb(const Ref<ObjectData> &p_data) {
	if (p_data.is_null()) {
		return AABB();
	}
	AABB aabb;
	bool first = true;
	const Array submeshes = p_data->build_lod_submeshes(RENDER_LOD);
	for (int i = 0; i < submeshes.size(); ++i) {
		const Dictionary entry = submeshes[i];
		const Ref<Mesh> mesh = entry.get("mesh", Variant());
		if (mesh.is_null()) {
			continue;
		}
		AABB m = mesh->get_aabb();
		m.position += Vector3(entry.get("abs", Vector3()));
		if (first) {
			aabb = m;
			first = false;
		} else {
			aabb = aabb.merge(m);
		}
	}
	return aabb;
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

// --- destruction support (world-wac-ai-re §24.6) -----------------------------

void MissionObjectPlacer::register_static_instance(int p_bms_id,
		const String &p_graphic, int p_index, const Transform3D &p_xform,
		bool p_casts_static_shadow, bool p_mirror_reflected) {
	DestructionInstance inst;
	inst.graphic = p_graphic;
	inst.batch_key = p_graphic;
	inst.index = p_index;
	inst.xform = p_xform;
	inst.casts_static_shadow = p_casts_static_shadow;
	inst.mirror_reflected = p_mirror_reflected;
	destruction_instances_[p_bms_id] = inst;
	_bump_static_terrain_shadow_source_revision();
}

Variant MissionObjectPlacer::get_static_instance_transform(
		int p_bms_id) const {
	const DestructionInstance *rec = destruction_instances_.getptr(p_bms_id);
	if (rec == nullptr) {
		return Variant();
	}
	return rec->xform;
}

String MissionObjectPlacer::get_static_instance_batch_key(int p_bms_id) const {
	const DestructionInstance *rec = destruction_instances_.getptr(p_bms_id);
	if (rec == nullptr) {
		return String();
	}
	return rec->batch_key.is_empty() ? rec->graphic : rec->batch_key;
}

bool MissionObjectPlacer::static_instance_is_mirror_reflected(
		int p_bms_id) const {
	const DestructionInstance *rec = destruction_instances_.getptr(p_bms_id);
	return rec != nullptr && rec->mirror_reflected;
}

bool MissionObjectPlacer::static_instance_casts_terrain_shadow(
		int p_bms_id) const {
	const DestructionInstance *rec = destruction_instances_.getptr(p_bms_id);
	return rec != nullptr && rec->casts_static_shadow;
}

// Hide a destroyed batched static in every batch of its graphic/reflection
// population (zero-scale at its own origin — the batch keeps its instance
// count); returns the instance's placed transform for the husk graft.
Variant MissionObjectPlacer::hide_static_instance(int p_bms_id) {
	const DestructionInstance *rec = destruction_instances_.getptr(p_bms_id);
	if (rec == nullptr) {
		return Variant();
	}
	if (hidden_destruction_instances_.has(p_bms_id)) {
		return rec->xform;
	}
	const Transform3D carved(Basis().scaled(Vector3()), rec->xform.origin);
	Array originals;
	const String batch_key = rec->batch_key.is_empty()
			? rec->graphic
			: rec->batch_key;
	const Vector<Ref<MultiMesh>> *batches =
			destruction_batches_.getptr(batch_key);
	if (batches != nullptr) {
		for (const Ref<MultiMesh> &mm : *batches) {
			if (mm.is_valid() && rec->index >= 0 &&
					rec->index < mm->get_instance_count()) {
				Dictionary saved;
				saved["multimesh"] = mm;
				saved["transform"] = mm->get_instance_transform(rec->index);
				saved["index"] = rec->index;
				originals.push_back(saved);
				mm->set_instance_transform(rec->index, carved);
			}
		}
	}
	hidden_destruction_instances_[p_bms_id] = originals;
	_bump_static_terrain_shadow_source_revision();
	return rec->xform;
}

// Restore a carved static; repeated reset calls are safe (false when the
// instance was not hidden).
bool MissionObjectPlacer::show_static_instance(int p_bms_id) {
	const Array *originals = hidden_destruction_instances_.getptr(p_bms_id);
	if (originals == nullptr) {
		return false;
	}
	for (int i = 0; i < originals->size(); ++i) {
		const Dictionary saved = (*originals)[i];
		const Ref<MultiMesh> mm = saved.get("multimesh", Variant());
		const int index = int(saved.get("index", -1));
		if (mm.is_valid() && index >= 0 && index < mm->get_instance_count()) {
			mm->set_instance_transform(index,
					saved.get("transform", Transform3D()));
		}
	}
	hidden_destruction_instances_.erase(p_bms_id);
	_bump_static_terrain_shadow_source_revision();
	return true;
}

bool MissionObjectPlacer::update_static_terrain_shadow_source_transform(
		int p_kind, int p_index, const Transform3D &p_xform) {
	_check_epoch();
	bool matched = false;
	bool saw_source_match = false;
	bool observable_changed = false;
	const Vector<int> *source_rows = static_terrain_shadow_source_rows_.getptr(
			entity_identity_key(p_kind, p_index));
	if (source_rows != nullptr) {
		for (const int i : *source_rows) {
			if (i < 0 || i >= static_terrain_shadow_sources_.size()) continue;
			StaticTerrainShadowSource &source =
					static_terrain_shadow_sources_.write[i];
			if (source.entity_kind != p_kind || source.entity_index != p_index) {
				continue;
			}
			saw_source_match = true;
			matched = true;
			const bool policy_admitted =
					opennova::mission::item_casts_static_terrain_shadow(
					source.entity_kind, source.entity_attrib,
					source.item_attrib, source.item_attrib2);
			bool effective_active = source.active &&
					(source.bms_id == 0 ||
							!hidden_destruction_instances_.has(source.bms_id));
			StaticTerrainShadowSource *replacement = source.bms_id != 0
					? static_terrain_shadow_replacements_.getptr(source.bms_id)
					: nullptr;
			if (replacement != nullptr) effective_active = replacement->active;
			bool value_changed = false;
			if (source.world_transform != p_xform) {
				source.world_transform = p_xform;
				value_changed = true;
			}
			if (source.bms_id != 0) {
				if (DestructionInstance *instance =
							destruction_instances_.getptr(source.bms_id)) {
					if (instance->xform != p_xform) {
						instance->xform = p_xform;
						value_changed = true;
					}
				}
				if (replacement != nullptr) {
					if (replacement->world_transform != p_xform) {
						replacement->world_transform = p_xform;
						value_changed = true;
					}
				}
			}
			observable_changed |=
					policy_admitted && effective_active && value_changed;
		}
	}
	if (!saw_source_match &&
			p_kind == opennova::mission::kEntityKindBuilding) {
		for (KeyValue<int64_t, DestructionInstance> &kv :
				destruction_instances_) {
			if (kv.value.index != p_index) continue;
			matched = true;
			StaticTerrainShadowSource *replacement =
					static_terrain_shadow_replacements_.getptr(kv.key);
			const bool effective_active = kv.value.casts_static_shadow &&
					(replacement != nullptr
							? replacement->active
							: !hidden_destruction_instances_.has(kv.key));
			bool value_changed = false;
			if (kv.value.xform != p_xform) {
				kv.value.xform = p_xform;
				value_changed = true;
			}
			if (replacement != nullptr) {
				if (replacement->world_transform != p_xform) {
					replacement->world_transform = p_xform;
					value_changed = true;
				}
			}
			observable_changed |= effective_active && value_changed;
		}
	}
	if (observable_changed) _bump_static_terrain_shadow_source_revision();
	return matched;
}

void MissionObjectPlacer::_static_shadow_bms_policy(int p_bms_id,
		bool &r_represented, bool &r_policy_admitted,
		bool &r_base_active) const {
	r_represented = false;
	r_policy_admitted = false;
	r_base_active = false;
	const Vector<int> *rows =
			static_terrain_shadow_rows_by_bms_.getptr(p_bms_id);
	if (rows == nullptr) return;
	for (const int i : *rows) {
		if (i < 0 || i >= static_terrain_shadow_sources_.size()) continue;
		const StaticTerrainShadowSource &source =
				static_terrain_shadow_sources_[i];
		if (source.bms_id != p_bms_id) continue;
		r_represented = true;
		const bool admitted =
				opennova::mission::item_casts_static_terrain_shadow(
					source.entity_kind, source.entity_attrib,
					source.item_attrib, source.item_attrib2);
		r_policy_admitted |= admitted;
		r_base_active |= admitted && source.active &&
				!hidden_destruction_instances_.has(p_bms_id);
	}
}

bool MissionObjectPlacer::set_static_terrain_shadow_replacement(
		int p_bms_id, const String &p_graphic, const Transform3D &p_xform,
		bool p_active) {
	_check_epoch();
	bool policy_admitted = false;
	bool base_active = false;
	bool represented = false;
	_static_shadow_bms_policy(p_bms_id, represented, policy_admitted,
			base_active);
	bool source_exists = represented;
	if (!represented) {
		if (const DestructionInstance *instance =
					destruction_instances_.getptr(p_bms_id)) {
			source_exists = true;
			policy_admitted = instance->casts_static_shadow;
			base_active = policy_admitted &&
					!hidden_destruction_instances_.has(p_bms_id);
		}
	}
	if (p_bms_id == 0 || p_graphic.is_empty() || !source_exists) {
		return false;
	}
	StaticTerrainShadowSource replacement;
	replacement.bms_id = p_bms_id;
	replacement.graphic = p_graphic;
	replacement.world_transform = p_xform;
	replacement.object_data = _load_object_data(p_graphic);
	replacement.active = p_active && replacement.object_data.is_valid();
	const StaticTerrainShadowSource *current =
			static_terrain_shadow_replacements_.getptr(p_bms_id);
	if (current != nullptr) {
		if (current->graphic == replacement.graphic &&
				current->world_transform == replacement.world_transform &&
				current->object_data.ptr() == replacement.object_data.ptr() &&
				current->active == replacement.active) {
			return replacement.object_data.is_valid();
		}
	}
	const bool was_effective = policy_admitted &&
			(current != nullptr ? current->active : base_active);
	const bool becomes_effective = policy_admitted && replacement.active;
	static_terrain_shadow_replacements_[p_bms_id] = replacement;
	if (was_effective || becomes_effective) {
		_bump_static_terrain_shadow_source_revision();
	}
	return replacement.object_data.is_valid();
}

bool MissionObjectPlacer::clear_static_terrain_shadow_replacement(
		int p_bms_id) {
	_check_epoch();
	const StaticTerrainShadowSource *replacement =
			static_terrain_shadow_replacements_.getptr(p_bms_id);
	if (replacement == nullptr) return false;
	bool policy_admitted = false;
	bool base_active = false;
	bool represented = false;
	_static_shadow_bms_policy(p_bms_id, represented, policy_admitted,
			base_active);
	if (!represented) {
		if (const DestructionInstance *instance =
					destruction_instances_.getptr(p_bms_id)) {
			policy_admitted = instance->casts_static_shadow;
			base_active = policy_admitted &&
					!hidden_destruction_instances_.has(p_bms_id);
		}
	}
	const bool was_effective = policy_admitted && replacement->active;
	const bool becomes_effective = policy_admitted && base_active;
	static_terrain_shadow_replacements_.erase(p_bms_id);
	if (was_effective || becomes_effective) {
		_bump_static_terrain_shadow_source_revision();
	}
	return true;
}

bool MissionObjectPlacer::register_object_data(const String &p_graphic,
		const Ref<ObjectData> &p_data) {
	_check_epoch();
	if (p_graphic.is_empty() || p_data.is_null()) {
		return false;
	}
	object_data_cache_[p_graphic] = p_data;
	_bump_static_terrain_shadow_source_revision();
	return true;
}

bool MissionObjectPlacer::register_static_batches(const String &p_graphic,
		const Array &p_batches) {
	_check_epoch();
	if (p_graphic.is_empty() || p_batches.is_empty()) {
		return false;
	}
	Vector<StaticBatch> retained;
	for (int i = 0; i < p_batches.size(); ++i) {
		if (p_batches[i].get_type() != Variant::DICTIONARY) {
			return false;
		}
		const Dictionary batch = p_batches[i];
		const Ref<Mesh> mesh = batch.get("mesh", Variant());
		if (mesh.is_null()) {
			return false;
		}
		StaticBatch retained_batch;
		retained_batch.mesh = mesh;
		retained_batch.material = batch.get("material", Variant());
		retained_batch.offset = batch.get("offset", Transform3D());
		retained_batch.submesh = int(batch.get("submesh", 0));
		retained.push_back(retained_batch);
	}
	static_batch_cache_[p_graphic] = retained;
	return true;
}

void MissionObjectPlacer::register_ground_anchor(const String &p_graphic,
		const Vector3 &p_anchor) {
	_check_epoch();
	anchor_cache_[p_graphic] = p_anchor;
}

void MissionObjectPlacer::register_occlusion_verdict(int p_item_id,
		bool p_has_occlusion) {
	_check_epoch();
	occlusion_cache_[p_item_id] = p_has_occlusion;
}

bool MissionObjectPlacer::register_resolved_static_graphic(
		const String &p_graphic, const Ref<ObjectData> &p_data,
		const Array &p_batches) {
	_check_epoch();
	if (p_graphic.is_empty() || p_data.is_null() || p_batches.is_empty()) {
		return false;
	}
	Vector<StaticBatch> retained;
	for (int i = 0; i < p_batches.size(); ++i) {
		if (p_batches[i].get_type() != Variant::DICTIONARY) {
			return false;
		}
		const Dictionary batch = p_batches[i];
		const Ref<Mesh> mesh = batch.get("mesh", Variant());
		if (mesh.is_null()) {
			return false;
		}
		StaticBatch retained_batch;
		retained_batch.mesh = mesh;
		retained_batch.material = batch.get("material", Variant());
		retained_batch.offset = batch.get("offset", Transform3D());
		retained_batch.submesh = int(batch.get("submesh", 0));
		retained.push_back(retained_batch);
	}
	object_data_cache_[p_graphic] = p_data;
	static_batch_cache_[p_graphic] = retained;
	_bump_static_terrain_shadow_source_revision();
	return true;
}

} // namespace godot
