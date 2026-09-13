#pragma once

#include <godot_cpp/classes/multi_mesh.hpp>
#include <godot_cpp/classes/multi_mesh_instance3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <runtime/mission/placement_traits.h>

#include <cstdint>
#include <vector>

#include "mission/mission_data.h" // MissionData::EntityKind
#include "mission/mission_placement_stats.h"
#include "mission/static_source_records.h"
#include "object/item_database.h"
#include "mission/player_visual_spec.h"
#include "object/avatar_database.h"
#include "object/avatar_records.h"
#include "object/object_data.h"
#include "object/object_model.h"
#include "object/skeletal_anim.h"
#include "resource_index/resource_root.h"

namespace godot {


// Placement of a mission's entities into the runtime 3D scene. Given a parsed
// MissionData, a resource root, and an item database, it resolves each placed
// entity to its visual model and instances it under a "MissionObjects"
// container.
//
// Batching strategy (hybrid): opaque and alpha-tested static surfaces merge
// into 512-unit terrain-aligned MultiMesh populations; blended surfaces keep
// one global population so their retail ordering is unchanged. Every authored
// RLOD of a static graphic is harvested and emitted as its own population
// over the same slot list, and a population carries ONLY the slots whose
// selected level it draws: its rows are packed dense [0, live) with
// visible_instance_count = live, a level switch swap-removes the slot from
// the old level's population and appends it to the new one, and a population
// with no live row is hidden (Godot's cull tree skips it). The GPU and every
// viewport's cull therefore see one row per live instance, while the level
// is still chosen per entity by the retail selector each frame
// (update_static_lods).
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
	// The retained static read-back rows behind the get_static_* seams; the
	// getters mint fresh records from these so no consumer holds the
	// placer's own row.
	struct StaticEffectSourceRow {
		int32_t entity_bound_radius_q16 = 0;
		int kind = -1;
		int entity_index = -1;
		int bms_id = 0;
		int item_id = 0;
		String graphic;
		Transform3D world_transform;
		Ref<ObjectData> object_data;
	};
	struct StaticLightDrawRow {
		int source_index = -1;
		int kind = -1;
		int entity_index = -1;
		int bms_id = 0;
		int item_id = 0;
		int robj_index = 0;
		AABB world_bounds;
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
	static bool item_casts_dynamic_shadow(int item_type, uint32_t attrib2);
	static bool item_casts_static_terrain_shadow(MissionData::EntityKind kind,
			uint32_t entity_attrib, uint32_t item_attrib,
			uint32_t item_attrib2);

	// --- placement --------------------------------------------------------
	// Place every renderable entity under a fresh MissionObjects node
	// parented to `parent` (any previous one is cleared): the individual
	// models as its children, every static population (per-bin, blended
	// global, shadow twin) under its one StaticPopulations child. `options` may
	// carry "progress" (a per-model Callable pulse, mirroring the original's
	// per-model loading-screen presents — witness: placement_traits.h
	// ledger) and "skip_kinds"
	// (the joiner places the mission minus organics). Returns the placement
	// census as a MissionPlacementStats record (mission/mission_placement_stats.h).
	// One placement row: the BMS entity record's placement facts (the mission
	// document's own entities read straight off its bms::File, or the joiner's
	// streamed statics the sim stamped at the world-stream fence).
	struct PlacementRow {
		int kind = -1;
		int index = -1;
		int item_id = 0;
		int bms_id = 0;
		int group = -1;
		int team = 0;
		uint32_t ai_flags = 0;
		Vector3 position;     // mission space
		Vector3 rotation_deg; // (pitch, yaw, roll) as authored
	};
	// Place from entity dictionaries in the streamed-record shape (kind, index,
	// bms_id, item_id, position, rotation_deg, team, group, ai_flags): the
	// joiner's streamed statics take this entry.
	Ref<MissionPlacementStats> place_entities(const Array &p_entities, Node3D *p_parent,
			const Dictionary &p_options = Dictionary());
	// Place the mission document's entities, read natively off its bms::File in
	// the placement order (markers, items, buildings, organics).
	Ref<MissionPlacementStats> place(const Ref<MissionData> &p_mission, Node3D *p_parent,
			const Dictionary &p_options = Dictionary());
	Ref<MissionPlacementStats> place_rows(const std::vector<PlacementRow> &p_rows, Node3D *p_parent,
			const Dictionary &p_options);

	// Per-frame RLOD selection for every retained static instance, driven by
	// GameWorld beside ObjectModel.update_authored_lods. Each instance's
	// bound sphere is projected with the engine's projector and frame scale,
	// retail's sub-pixel floor drops it from every population, and only an
	// instance whose level changed has its rows moved (swap-removed from the
	// old level's populations, appended to the new level's, shadow twins
	// following) with the touched populations' visibility, shadow row map
	// and Q3 instance rows refreshed. Returns the switch count.
	int update_static_lods(const Transform3D &p_camera_transform,
			float p_vertical_fov_degrees, float p_viewport_width,
			float p_viewport_height);
	// The level currently live for a placed static entity (-1 = below the
	// sub-pixel floor or no level available, -2 = not a retained static).
	int get_static_instance_lod(int p_bms_id) const;
	bool inherit_static_entity_projection(int p_bms_id, ObjectModel *p_model) const;
	// The names of the visible populations that currently carry a live row
	// for the entity (empty when carved, culled or unknown). Headless Godot
	// stores no MultiMesh instance data, so tests pin the placer's own row
	// bookkeeping through this typed read-back.
	Array get_static_instance_live_populations(int p_bms_id) const;
	// The bms ids of one emitted population's live rows in row order (row r
	// of the MultiMesh draws the entity at index r), empty for a node that is
	// not one of the placer's populations. Pins the dense packing and the
	// swap-remove order without a device (see above).
	PackedInt32Array get_static_population_live_bms_ids(
			MultiMeshInstance3D *p_population) const;
	// Emitted populations (visible batches and shadow twins) that currently
	// carry at least one live row; the rest are hidden from the cull.
	int get_static_live_population_count() const;

	// Build ONE animated ObjectModel for an item type, in rest pose, for an
	// owner-managed entity with no BMS placement (the local-player avatar).
	ObjectModel *build_animated_model(int p_item_id, Node3D *p_parent);
	int resolve_player_visual_item_id(int p_runtime_type_id);
	Ref<PlayerVisualSpec> resolve_player_visual_spec(int p_runtime_type_id,
			int p_character_id);
	ObjectModel *build_player_animated_model(int p_runtime_type_id,
			Node3D *p_parent, int p_character_id = 0);
	// The composed avatar's head part under a body build_player_animated_model
	// returned, or null for a plain item-model avatar. Retail draws the
	// one-shot overlays (the held weapon, the mounted child) with the HEAD
	// submit and skips them on the flagged body submit, so the head is the
	// attachment RLOD owner of a composed avatar.
	static ObjectModel *avatar_head_part(ObjectModel *p_body);
	// Build ONE animated ObjectModel from an EXPLICIT graphic + .adm name
	// (the first-person weapon viewmodel path; the arms + gun share the
	// equipped gun's rig table — witness: placement_traits.h ledger).
	ObjectModel *build_model_from_graphic(const String &p_graphic,
			const String &p_adm_name, Node3D *p_parent,
			const String &p_clip_key = String(),
			const String &p_rig_graphic = String(),
			bool p_retain_authored_lods = false);

	// --- read-back seams --------------------------------------------------
	// The animated models place() registered, each carrying its EntityRef:
	// EntityIndex builds from this list (construction-time registration,
	// never a child scan). Shared by reference so a harness appends its own.
	TypedArray<ObjectModel> get_placed_models() const { return placed_models_; }
	void set_placed_models(const TypedArray<ObjectModel> &p_models) {
		placed_models_ = p_models;
	}
	TypedArray<StaticEffectSource> get_static_item_effect_sources();
	// One row per retained static entity/ROBJ light draw. Row order is the
	// atlas index stamped into each matching MultiMesh INSTANCE_CUSTOM.x;
	// descriptors carry the source identity, exact world AABB, and live carve
	// state. The EffectWorld device selects this row's <=4 lights into the
	// shared RGBAF atlas [orig: collect_render_objects_for_batch @0x5d8ff7,
	// see docs/render/render-lighting-re.md].
	TypedArray<StaticLightDrawSource> get_static_light_draw_sources();
	// Advances whenever a row is appended, the table is reset, or a carve
	// changes any row's `active` state: consumers rebuild their packed row
	// arrays only on a change instead of re-reading the rows every frame.
	uint64_t get_static_light_draw_source_revision() const;
	Vector<StaticTerrainShadowSource> get_static_terrain_shadow_sources();
	uint64_t get_static_terrain_shadow_source_revision();
	// Dictionary mirror for focused shell/asset diagnostics. Production
	// consumers use the typed snapshot above.
	TypedArray<StaticTerrainShadowSourceRow> get_static_terrain_shadow_source_diagnostics();
	String graphic_for(int p_item_id);
	Ref<ObjectData> object_data_for(const String &p_graphic);

	// --- destruction support (world-wac-ai-re §24.6) ----------------------
	// Diagnostic/read-back identity for the entity's graphic/reflection group.
	// Distinct policies may share the same authored graphic.
	String get_static_instance_batch_key(int p_bms_id) const;
	// Number of exact MultiMesh slots the carve owns across spatial, global,
	// per-RLOD and shadow populations. Public read-back for renderer
	// diagnostics/tests.
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
	bool update_static_terrain_shadow_source_transform(MissionData::EntityKind p_kind,
			int p_index, const Transform3D &p_xform);
	bool set_static_terrain_shadow_replacement(int p_bms_id,
			const String &p_graphic, const Transform3D &p_xform,
			bool p_active);
	bool clear_static_terrain_shadow_replacement(int p_bms_id);

	// Register an already-resolved object plus its static render batches —
	// the construction seam for callers that already own parsed geometry
	// (including asset-free tests). Each batch row may carry "lod_index"
	// (default 0); `lod_profile` may carry "thresholds_q16"
	// (PackedInt32Array, fine to coarse, one row per level) and
	// "sphere_radius" with optional "sphere_center" (Godot model-local);
	// absent entries derive from CMDL, else from the level-0 geometry.
	bool register_resolved_static_graphic(const String &p_graphic,
			const Ref<ObjectData> &p_data, const Array &p_batches,
			const Dictionary &p_lod_profile = Dictionary());
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
		int lod_index = 0;
		bool auxiliary_draw = false;
		bool blended_draw = false;
	};
	// A graphic's authored RLOD profile, harvested beside its batches: the
	// selector's threshold table (fine to coarse), which levels carry
	// harvested geometry, and the bound sphere the projector consumes.
	struct StaticLodProfile {
		std::vector<int32_t> thresholds_q16;
		std::vector<bool> available;
		opennova::renderer::ObjectProjectionSphere projection_sphere;
		// The same CMDL bounds in the eweap-powerup form (zero center, halves
		// = the maxima); an instance whose item def takes that leg selects it.
		// Invalid when the profile came without a document.
		opennova::renderer::ObjectProjectionSphere zero_center_projection_sphere;
	};
	// One emitted MultiMesh population: capacity = the slot list it was
	// emitted over, live rows packed [0, live) (visible_instance_count) in
	// swap-remove order. The row tables name the retained instance/binding
	// and the population-local slot each live row draws.
	struct StaticPopulation {
		Ref<MultiMesh> multimesh;
		uint64_t instance_node = 0; // MultiMeshInstance3D ObjectID
		int lod_index = 0;
		bool shadow_only = false; // the filtered shadow twin
		bool custom_data = false; // rows carry the light-atlas custom data
		bool shadow_tagged = false; // carries the static_shadow_* metas
		int live = 0;
		Vector<int> row_instance;
		Vector<int> row_binding;
		Vector<int> row_slot;
	};
	// One slot of a retained static instance in one population: the row it
	// occupies while its level is live (-1 otherwise) and what that row is
	// written with.
	struct StaticLodBinding {
		int population = -1;
		int slot = -1; // the population-local slot (the shadow meta index)
		int row = -1;
		int lod_index = 0;
		Transform3D live_xform; // the row's transform while the level is live
		Color custom_data; // the light-atlas row (visible populations)
		bool shadow_only = false; // the filtered shadow twin
		bool casts = true; // whether the slot is ever live in a shadow twin
	};
	// One retained static entity: its world bound sphere, the level live for
	// it, and every population slot it occupies across levels/populations.
	struct StaticLodInstance {
		int profile = -1;
		int bms_id = 0;
		Vector3 origin;
		int32_t radius_q16 = 0;
		opennova::renderer::ObjectProjectionSphere local_projection_sphere;
		int32_t entity_scale_q16 = 0;
		int active_lod = 0; // -1 = below the sub-pixel floor / none available
		bool carved = false;
		Vector<StaticLodBinding> bindings;
	};
	void _check_epoch();
	void _ensure_item_db();
	void _ensure_avatar_db();
	String _graphic_for(int p_item_id) const;
	Ref<ObjectData> _load_object_data(const String &p_graphic);
	String _model_name_for(const String &p_graphic) const;
	bool _needs_individual_node(int p_item_id);
	bool _graphic_needs_live_panm(const String &p_graphic);
	bool _has_occlusion_records(int p_item_id);
	bool _item_is_mirror_reflected(int p_item_id) const;
	bool _placement_is_mirror_reflected(uint32_t p_entity_attrib,
			int p_item_id) const;
	int32_t _item_model_scale_q16(int p_item_id) const;
	// Whether the item's entity init stores a zero bbox center (type-6 eweap
	// powerups), the form the collision center and projection sphere share.
	bool _item_projection_zero_center(int p_item_id) const;
	int32_t _item_entity_bound_radius_q16(int p_item_id,
			const Ref<ObjectData> &p_data);
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
	// The profile _get_static_batches / register_resolved_static_graphic
	// filled for the graphic (a single-level profile when neither did).
	StaticLodProfile _static_lod_profile_for(const String &p_graphic) const;
	static void _complete_static_lod_profile(StaticLodProfile &r_profile,
			const Vector<StaticBatch> &p_batches);
	// Whether one emitted slot is live when `p_live_lod` is the instance's
	// level: its population's level matches, and a shadow twin only carries
	// slots that cast.
	static bool _static_slot_live(const StaticLodBinding &p_binding,
			int p_live_lod);
	// Move one retained instance's rows so it is live exactly in the
	// populations of `p_live_lod`: removed (swap-remove, the last live row
	// fills the hole) where it no longer belongs, appended where it now
	// does. Every population touched is collected for the flush below.
	void _write_static_instance_slots(int p_instance_row, int p_live_lod,
			HashSet<int> &r_touched);
	void _static_population_append(int p_population, int p_instance_row,
			int p_binding);
	void _static_population_remove(int p_population, int p_instance_row,
			int p_binding);
	// After a batch of row moves: hide/show each touched population by its
	// live count, republish its shadow row map, and invalidate its Q3
	// instance rows (the populations' meshes never change, so only their
	// rows are re-read; the packed Q3 surfaces stay cached).
	void _flush_static_population_changes(const HashSet<int> &p_touched);
	static PackedInt32Array _static_population_row_slots(
			const StaticPopulation &p_population);
	void _add_individual_static_shadow_siblings(ObjectModel *p_model,
			const String &p_graphic, const Transform3D &p_local_xform,
			const String &p_suffix);
	Node3D *_ensure_container(Node3D *p_parent);
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

	TypedArray<ObjectModel> placed_models_;
	Vector<StaticEffectSourceRow> static_item_effect_sources_;
	Vector<StaticLightDrawRow> static_light_draw_sources_;
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
	HashMap<String, StaticLodProfile> static_lod_profile_cache_;
	HashMap<String, bool> graphic_panm_cache_;
	HashMap<int64_t, bool> occlusion_cache_;
	uint64_t built_epoch_ = 0;

	// The retained static instances of the current placement (one per
	// batched entity), the per-graphic profiles they select from, and the
	// populations their rows live in (looked up by MultiMeshInstance3D id
	// for the read-backs).
	Vector<StaticLodProfile> static_lod_profiles_;
	Vector<StaticLodInstance> static_lod_instances_;
	Vector<StaticPopulation> static_populations_;
	HashMap<uint64_t, int> static_population_by_node_;
	int static_lod_switches_ = 0;

	// Destruction carve state: batched statics have no per-entity node; a
	// destroyed one has its row removed from every population it is live in
	// (every level's population and shadow twin) and the caller grafts the
	// husk model at the returned transform.
	struct DestructionInstance {
		String graphic;
		// Stable diagnostic identity for the graphic/reflection population.
		// Legacy/manual registrations use `graphic`.
		String batch_key;
		int index = -1;
		Transform3D xform;
		bool casts_static_shadow = false;
		bool mirror_reflected = false;
		// The retained instance whose slots the carve rewrites (-1 for a
		// manual registration without emitted populations).
		int lod_instance = -1;
	};
	HashMap<int64_t, DestructionInstance> destruction_instances_;
	HashSet<int64_t> hidden_destruction_instances_;
	HashMap<int64_t, StaticTerrainShadowSource>
			static_terrain_shadow_replacements_;
};

} // namespace godot
