#pragma once

#include <godot_cpp/classes/multi_mesh.hpp>
#include <godot_cpp/classes/multi_mesh_instance3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/transform3d.hpp>

#include <runtime/mission/placement_traits.h>

#include "object/item_database.h"
#include "object/avatar_database.h"
#include "object/object_data.h"
#include "object/object_model.h"
#include "object/skeletal_anim.h"
#include "resource_index/resource_root.h"

namespace godot {

class MissionData;

// Placement of a mission's entities into the runtime 3D scene. Given a parsed
// MissionData, a resource root, and an item database, it resolves each placed
// entity to its visual model and instances it under a "MissionObjects"
// container.
//
// Batching strategy (hybrid): opaque and alpha-tested static surfaces merge
// into 512-unit terrain-aligned MultiMesh populations; blended surfaces keep
// one global population so their retail ordering is unchanged.
// Animated/skinned models
// (persons, anim_def carriers), portal-carrying buildings (per-section
// occlusion masks), and live-PANM graphics (the original re-poses those from
// the global clock every rendered frame) each get an individual ObjectModel.
// The witnessed eligibility policy is engine-side
// (mission/placement_traits.h); static batches harvest a throwaway template
// model's rest-pose meshes and materials. Ported from
// mission_object_placer.gd (2026-08-10 de-scripting), with typed internal
// records and stage timings returned in the stats.
class MissionObjectPlacer : public RefCounted {
	GDCLASS(MissionObjectPlacer, RefCounted)

public:
	// Typed shell snapshot consumed by the terrain page-shadow adapter. It
	// preserves the BMS/item policy inputs instead of flattening eligibility
	// into a render-layer guess; the engine collector remains the one owner of
	// admission and ordering.
	// [orig: Terrain_CollectAndRenderTileModels pool scans/admission
	// @0x60D421..0x60D450; see docs/terrain/terrain-re.md]
	struct StaticTerrainShadowSource {
		int bms_id = 0;
		int item_id = 0;
		int entity_kind = -1;
		int entity_index = -1;
		int team = 0;
		uint32_t entity_attrib = 0;
		uint32_t item_attrib = 0;
		uint32_t item_attrib2 = 0;
		String graphic;
		Transform3D world_transform;
		Ref<ObjectData> object_data;
		bool active = true;
	};

	enum {
		RENDER_LOD = 0,
		PLAYER_RUNTIME_TYPE_ID = opennova::mission::kPlayerRuntimeTypeId,
		PLAYER_VISUAL_ITEM_ID = opennova::mission::kPlayerVisualItemId,
	};

	// One-expression construction for GDScript callers (native classes take
	// no .new() arguments).
	static Ref<MissionObjectPlacer> create(const Ref<ResourceRoot> &p_root,
			const Ref<ItemDatabase> &p_db);

	// --- wiring -----------------------------------------------------------
	void set_resource_root(const Ref<ResourceRoot> &p_root);
	Ref<ResourceRoot> get_resource_root() const { return resource_root_; }
	void set_item_db(const Ref<ItemDatabase> &p_db);
	Ref<ItemDatabase> get_item_db_property() const { return item_db_; }
	void set_avatar_db(const Ref<AvatarDatabase> &p_db);
	Ref<AvatarDatabase> get_avatar_db();
	void set_panm_clock(const Ref<PanmClock> &p_clock);

	// The items database (items.def), loaded on demand from the resource
	// root; null if items.def cannot be resolved.
	Ref<ItemDatabase> get_item_db();

	// --- coordinate conversion (BMS is Z-up; Godot is Y-up) ---------------
	// Thin Variant wrappers over the engine's witnessed converters — the
	// derivation and citations live in
	// engine/runtime/mission/placement_traits.h.
	static Vector3 bms_to_godot_position(const Vector3 &p);
	static Vector3 godot_to_bms_position(const Vector3 &p);
	static Basis bms_to_godot_basis(const Vector3 &rot_deg);
	static Transform3D entity_transform(const Vector3 &position,
			const Vector3 &rotation_deg);

	// --- witnessed eligibility (engine policy re-exported for shell/tests) -
	static bool item_casts_dynamic_shadow(int item_type, uint32_t attrib,
			uint32_t attrib2);
	static bool item_casts_static_terrain_shadow(int kind,
			uint32_t entity_attrib, uint32_t item_attrib,
			uint32_t item_attrib2);

	// --- placement --------------------------------------------------------
	// Place every renderable entity under a fresh MissionObjects node
	// parented to `parent` (any previous one is cleared). `options` may
	// carry "progress" (a per-model Callable pulse, mirroring the original's
	// per-model loading-screen presents — witness: placement_traits.h
	// ledger) and "skip_kinds"
	// (the joiner places the mission minus organics). Returns a stats
	// Dictionary (placed/batched/animated/unresolved/markers/graphics/
	// batches/static_bins/static_binned_batches/static_global_batches +
	// per-stage "spans" usec timings).
	Dictionary place(const Ref<MissionData> &p_mission, Node3D *p_parent,
			const Dictionary &p_options = Dictionary());

	// Build ONE animated ObjectModel for an item type, in rest pose, for an
	// owner-managed entity with no BMS placement (the local-player avatar).
	ObjectModel *build_animated_model(int p_item_id, Node3D *p_parent);
	int resolve_player_visual_item_id(int p_runtime_type_id);
	Dictionary resolve_player_visual_spec(int p_runtime_type_id,
			int p_character_id);
	ObjectModel *build_player_animated_model(int p_runtime_type_id,
			Node3D *p_parent, int p_character_id = 0);
	// Build ONE animated ObjectModel from an EXPLICIT graphic + .adm name
	// (the first-person weapon viewmodel path; the arms + gun share the
	// equipped gun's rig table — witness: placement_traits.h ledger).
	ObjectModel *build_model_from_graphic(const String &p_graphic,
			const String &p_adm_name, Node3D *p_parent,
			const String &p_clip_key = String(),
			const String &p_rig_graphic = String(),
			bool p_retain_authored_lods = false);

	// --- read-back seams --------------------------------------------------
	Array get_placed_entity_records() const { return placed_entity_records_; }
	void set_placed_entity_records(const Array &p_records) {
		placed_entity_records_ = p_records;
	}
	Array get_static_user_point_sources();
	Array get_static_item_effect_sources();
	// One row per retained static entity/ROBJ light draw. Row order is the
	// atlas index stamped into each matching MultiMesh INSTANCE_CUSTOM.x;
	// descriptors carry the source identity, exact world AABB, and live carve
	// state. The EffectWorld device selects this row's <=4 lights into the
	// shared RGBAF atlas [orig: collect_render_objects_for_batch @0x5d8ff7,
	// see docs/render/render-lighting-re.md].
	Array get_static_light_draw_sources();
	// Advances whenever a row is appended, the table is reset, or a carve
	// changes any row's `active` state: consumers rebuild their packed row
	// arrays only on a change instead of re-reading the rows every frame.
	uint64_t get_static_light_draw_source_revision() const;
	Vector<StaticTerrainShadowSource> get_static_terrain_shadow_sources();
	uint64_t get_static_terrain_shadow_source_revision();
	// Dictionary mirror for focused shell/asset diagnostics. Production
	// consumers use the typed snapshot above.
	Array get_static_terrain_shadow_source_diagnostics();
	String graphic_for(int p_item_id);
	Ref<ObjectData> object_data_for(const String &p_graphic);

	// --- destruction support (world-wac-ai-re §24.6) ----------------------
	// Diagnostic/read-back identity for the entity's graphic/reflection group.
	// Distinct policies may share the same authored graphic.
	String get_static_instance_batch_key(int p_bms_id) const;
	// Number of exact MultiMesh slots the carve owns across spatial, global,
	// and shadow populations. Public read-back for renderer diagnostics/tests.
	int get_static_instance_binding_count(int p_bms_id) const;
	// The carved instance's authored reflection policy, so the husk graft can
	// keep reflecting: retail's husk swap flips only the husk-model flag,
	// never the reflect flag the mirror collectors filter on (witnesses in
	// engine/runtime/mission/placement_traits.h).
	bool static_instance_is_mirror_reflected(int p_bms_id) const;
	// Register one carveable batched-static record directly (the
	// construction seam matching register_resolved_static_graphic: callers
	// that own their placement — including asset-free tests — feed the same
	// carve bookkeeping place() fills).
	void register_static_instance(int p_bms_id, const String &p_graphic,
			int p_index, const Transform3D &p_xform,
			bool p_casts_static_shadow, bool p_mirror_reflected = false);
	bool is_static_instance_hidden(int p_bms_id) const {
		return hidden_destruction_instances_.has(p_bms_id);
	}
	bool static_instance_casts_terrain_shadow(int p_bms_id) const;
	Variant hide_static_instance(int p_bms_id);
	bool show_static_instance(int p_bms_id);
	bool update_static_terrain_shadow_source_transform(int p_kind,
			int p_index, const Transform3D &p_xform);
	bool set_static_terrain_shadow_replacement(int p_bms_id,
			const String &p_graphic, const Transform3D &p_xform,
			bool p_active);
	bool clear_static_terrain_shadow_replacement(int p_bms_id);

	// Register an already-resolved object plus its static render batches —
	// the construction seam for callers that already own parsed geometry
	// (including asset-free tests).
	bool register_resolved_static_graphic(const String &p_graphic,
			const Ref<ObjectData> &p_data, const Array &p_batches);
	// Cache-inject a resolved object for a graphic without batches (the
	// classification seam: live-PANM routing reads the injected data).
	bool register_object_data(const String &p_graphic,
			const Ref<ObjectData> &p_data);
	// Pre-fill the per-item occlusion verdict (isolates the PANM routing
	// rule from a fixture's independent portal payload).
	void register_occlusion_verdict(int p_item_id, bool p_has_occlusion);

protected:
	static void _bind_methods();

private:
	struct StaticBatch {
		Ref<Mesh> mesh;
		Ref<Material> material;
		Transform3D offset;
		int submesh = 0;
		int robj_index = 0;
		bool auxiliary_draw = false;
		bool blended_draw = false;
	};

	void _check_epoch();
	void _ensure_item_db();
	void _ensure_avatar_db();
	String _graphic_for(int p_item_id) const;
	Ref<ObjectData> _load_object_data(const String &p_graphic);
	String _model_name_for(const String &p_graphic) const;
	bool _needs_individual_node(int p_item_id);
	bool _graphic_needs_live_panm(const String &p_graphic);
	bool _graphic_has_multiple_lods(const String &p_graphic);
	bool _has_occlusion_records(int p_item_id);
	bool _item_is_mirror_reflected(int p_item_id) const;
	bool _placement_is_mirror_reflected(uint32_t p_entity_attrib,
			int p_item_id) const;
	int32_t _item_model_scale_q16(int p_item_id) const;
	Transform3D _entity_transform_for_item(const Vector3 &p_position,
			const Vector3 &p_rotation_deg, int p_item_id) const;
	void _configure_item_scale(ObjectModel *p_model, int p_item_id) const;
	void _configure_item_shadow(ObjectModel *p_model, int p_item_id);
	void _configure_item_lighting(ObjectModel *p_model, int p_item_id);
	Ref<SkeletalAnim> _skeletal_from_adm(const String &p_adm_name,
			const PackedVector3Array &p_bone_origins,
			const PackedInt32Array &p_bone_parents);
	void _apply_skeletal_anim(ObjectModel *p_model, int p_item_id,
			const PackedVector3Array &p_bone_origins,
			const PackedInt32Array &p_bone_parents);
	Vector<StaticBatch> _get_static_batches(const String &p_graphic,
			Node *p_tree_parent);
	void _add_individual_static_shadow_siblings(ObjectModel *p_model,
			const String &p_graphic, const Transform3D &p_local_xform,
			const String &p_suffix);
	Node3D *_ensure_container(Node3D *p_parent);
	void _record_static_user_point_group(const String &p_graphic,
			const Array &p_transforms);
	int _append_static_item_effect_source(int p_kind, int p_entity_index,
			int p_bms_id, int p_item_id, const String &p_graphic,
			const Transform3D &p_xform);
	int _append_static_light_draw_source(int p_source_index, int p_kind,
			int p_entity_index, int p_bms_id, int p_item_id,
			int p_robj_index, const AABB &p_world_bounds);
	void _record_static_terrain_shadow_source(int p_kind, int p_index,
			int p_bms_id, int p_team, uint32_t p_entity_attrib, int p_item_id,
			const String &p_graphic, const Transform3D &p_xform,
			const Ref<ObjectData> &p_data);
	void _bump_static_terrain_shadow_source_revision();

	Ref<ResourceRoot> resource_root_;
	Ref<ItemDatabase> item_db_;
	Ref<AvatarDatabase> avatar_db_;
	Ref<PanmClock> panm_clock_;

	Array placed_entity_records_;
	Array static_user_point_sources_;
	Array static_item_effect_sources_;
	Array static_light_draw_sources_;
	uint64_t static_light_draw_source_revision_ = 1;
	Vector<StaticTerrainShadowSource> static_terrain_shadow_sources_;
	HashMap<uint64_t, Vector<int>> static_terrain_shadow_source_rows_;
	HashMap<int, Vector<int>> static_terrain_shadow_rows_by_bms_;
	uint64_t static_terrain_shadow_source_revision_ = 0;

	// One indexed pass over this bms id's source rows: whether any row
	// represents it, whether policy admits any row, and whether an admitted
	// row is base-active (unhidden). Replaces the former full-vector scans in
	// the replacement set/clear paths.
	void _static_shadow_bms_policy(int p_bms_id, bool &r_represented,
			bool &r_policy_admitted, bool &r_base_active) const;

	HashMap<String, Ref<ObjectData>> object_data_cache_;
	HashMap<String, Ref<SkeletalAnim>> skeletal_cache_;
	HashMap<String, Vector<StaticBatch>> static_batch_cache_;
	HashMap<String, bool> graphic_panm_cache_;
	HashMap<String, bool> graphic_multiple_lods_cache_;
	HashMap<int64_t, bool> occlusion_cache_;
	uint64_t built_epoch_ = 0;

	// Destruction carve state: batched statics have no per-entity node; a
	// destroyed one is zero-scaled out of every exact emitted MultiMesh slot
	// and the caller grafts the husk model at the returned
	// transform.
	struct DestructionBinding {
		Ref<MultiMesh> multimesh;
		int index = -1;
		// The MultiMeshInstance3D drawing `multimesh`, by identity: a carve
		// rewrites its instance rows and must invalidate its Q3 source.
		ObjectID instance_id;
	};
	struct DestructionInstance {
		String graphic;
		// Stable diagnostic identity for the graphic/reflection population.
		// Legacy/manual registrations use `graphic`.
		String batch_key;
		int index = -1;
		Transform3D xform;
		bool casts_static_shadow = false;
		bool mirror_reflected = false;
		// One entity may now occupy a spatial opaque population, a global
		// blended population, and a filtered shadow twin. Carving follows the
		// exact emitted slots instead of assuming one shared group-local index.
		Vector<DestructionBinding> bindings;
	};
	HashMap<int64_t, DestructionInstance> destruction_instances_;
	HashMap<int64_t, Array> hidden_destruction_instances_;
	HashMap<int64_t, StaticTerrainShadowSource>
			static_terrain_shadow_replacements_;
};

} // namespace godot
