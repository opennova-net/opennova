#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

#include <mission/bms.h>
#include <mission/mission.h>
#include <mission/mission_schema.h>
#include <oned_edit/edit_history.h>

namespace godot {

class NovaResourceRoot;

// Thin GDExtension wrapper over opennova::mission::MissionDocument (libs/mission).
// Parses a NovaLogic .bms/.mis mission file and exposes its header (terrain/env refs,
// metadata) and its placed entities as Godot dictionaries. The read surface is
// loading + getters; the mutate surface (Phase 1 authoring) is set_entity_transform
// + save, calling the already byte-faithful writer in libs/mission.
class NovaMissionData : public RefCounted {
	GDCLASS(NovaMissionData, RefCounted)

private:
	opennova::mission::MissionDocument document;
	String source_path;
	String last_error;
	// True once an in-memory mutation lands and before the next successful save/load. Used only
	// as the dirty fallback before a clean baseline exists; the exact dirty flag is the
	// clean_baseline compare below.
	bool modified = false;
	// Staged terrain base heights for the next .mis save (see set_mis_base_heights). Cleared by
	// every save_as() (whichever format ran) and by open_file()/create_default(), so stale
	// heights can never leak onto a different document or a later save.
	PackedInt32Array mis_base_heights;

	// Whole-document undo / redo history + exact dirty, on the shared editor core
	// (libs/oned_edit). The snapshot is the parsed bms::File (never serialized bytes);
	// the no-op equal-gate and the dirty compare both use the byte-faithful bms::equal
	// via BmsFileEqual (a free function, not operator==). begin_edit() captures the
	// pre-edit document, commit_edit() records one step iff it changed, undo()/redo()
	// swap the live file with a stack top in O(1), and is_dirty() compares against the
	// baseline set by mark_clean() at open / save / new. See nova_mission_data.cpp.
	struct BmsFileEqual {
		bool operator()(const opennova::bms::File &a, const opennova::bms::File &b) const {
			return opennova::bms::equal(a, b);
		}
	};
	opennova::edit::EditHistory<opennova::bms::File, BmsFileEqual> history{100};

	Dictionary entity_to_dictionary(const opennova::mission::EntityRecord &record) const;
	Dictionary waypoint_path_to_dictionary(const opennova::mission::WaypointPath &path) const;
	Dictionary area_trigger_to_dictionary(const opennova::mission::AreaTriggerRecord &record) const;
	Dictionary weapon_loadout_to_dictionary(const opennova::mission::WeaponLoadoutEntry &entry, int index) const;
	Dictionary group_to_dictionary(const opennova::mission::GroupFields &fields) const;
	Dictionary event_to_dictionary(const opennova::mission::MissionEventRecord &record) const;
	Dictionary trigger_to_dictionary(const opennova::mission::MissionTriggerRecord &record) const;
	Dictionary action_to_dictionary(const opennova::mission::MissionActionRecord &record) const;
	Dictionary logic_reference_to_dictionary(const opennova::mission::MissionLogicReference &reference) const;
	Dictionary logic_diagnostic_to_dictionary(const opennova::mission::MissionLogicDiagnostic &diagnostic) const;
	Dictionary event_chain_to_dictionary(const opennova::mission::MissionEventChain &chain) const;
	// Build a trigger / action record from an editor dictionary, starting from `seed` so omitted keys keep
	// their existing value (and the trigger's unmodeled condition_flags high bits + unknown7, and the
	// action's reserved words, survive an edit). The trigger's negated/logic_or/logic_xor booleans compose
	// condition_flags bits 0/1/2.
	opennova::mission::MissionTriggerRecord trigger_from_dictionary(const Dictionary &dict, const opennova::mission::MissionTriggerRecord &seed) const;
	opennova::mission::MissionActionRecord action_from_dictionary(const Dictionary &dict, const opennova::mission::MissionActionRecord &seed) const;

protected:
	static void _bind_methods();

public:
	// Mirrors opennova::mission::EntityKind. Bound as plain constants. Fixed uint32_t underlying type
	// so the high game-mode bits (ATTRIB_SEARCH_AND_DESTROY 0x80000000, ATTRIB_GAME_MODE_MASK 0xFF830000)
	// bind to GDScript as positive values instead of wrapping to negative signed-int.
	enum : uint32_t {
		KIND_MARKER = 0,
		KIND_ITEM = 1,
		KIND_BUILDING = 2,
		KIND_ORGANIC = 3,
		// Mirrors opennova::mission::bms::WaypointFlags (libs/mission). Bound as constants so
		// GDScript composes a path's flags without magic numbers. A path with DOES_NOT_LOOP
		// clear loops back to its first marker; BLUE_TEAM / RED_TEAM scope it to a side.
		WP_FLAG_DOES_NOT_LOOP = 1,
		WP_FLAG_BLUE_TEAM = 2,
		WP_FLAG_RED_TEAM = 4,
		// Mirrors opennova::bms::AttribFlags. The option bits (surfaced as checkboxes) plus the 11
		// game-mode bits (surfaced as the single-select Game mode dropdown via get/set_game_mode).
		// ATTRIB_GAME_MODE_MASK is the union of the 11 mode bits [orig: 0xFF830000, the complement of
		// the engine's `and 0x7CFFFF` clear in sub_402770 @0x4031cd, dfx2med.exe].
		ATTRIB_FORCE_INDOORS = 0x10, // game-side witness (forces the indoors blink bit every frame);
		                             // not a dfx2med checkbox. Pinned to bms::AttribFlags in the .cpp.
		ATTRIB_ROTATE_MAP_180 = 0x20,
		ATTRIB_ENABLE_NVG = 0x100000,
		ATTRIB_START_WITH_NVG_ON = 0x400000,
		ATTRIB_ADVANCE_AND_SECURE = 0x10000,
		ATTRIB_CONQUER_AND_CONTROL = 0x20000,
		ATTRIB_ATTACK_AND_DEFEND = 0x800000,
		ATTRIB_COOP = 0x1000000,
		ATTRIB_DEATHMATCH = 0x2000000,
		ATTRIB_KING_OF_THE_HILL = 0x4000000,
		ATTRIB_FLAGBALL = 0x8000000,
		ATTRIB_CAPTURE_THE_FLAG = 0x10000000,
		ATTRIB_TEAM_DEATHMATCH = 0x20000000,
		ATTRIB_TEAM_KING_OF_THE_HILL = 0x40000000,
		ATTRIB_SEARCH_AND_DESTROY = 0x80000000,
		ATTRIB_GAME_MODE_MASK = 0xFF830000,
		// items.def id = wire type id + this offset (libs/mission kItemIdOffset;
		// pinned by static_assert in the .cpp). Bound so GDScript never
		// re-hardcodes the 100000. [orig: the +100000 item-id bias in the BMS
		// entity records — mission/mission.h]
		ITEM_ID_OFFSET = 100000,
	};

	Error open_file(const String &path);
	// Build a fresh, empty, valid mission in memory (no file backing). Mirrors open_file's
	// post-state: loaded document, source_path cleared, modified flag cleared, history reset.
	// Always returns OK.
	Error create_default();
	// Load a .bms/.mis by flat name through the mounted resource root (VFS), so missions packed in
	// PFF archives load at runtime. p_lookup_policy uses NovaResourceRoot::LookupPolicy ordinals;
	// its default preserves the mounted session policy for every existing caller.
	Error open_from_resource_root(const Ref<NovaResourceRoot> &p_resource_root, const String &p_name,
			int p_lookup_policy = 0);
	bool is_loaded() const;
	String get_source_path() const;
	String get_last_error() const;

	String get_mission_name() const;
	String get_designer() const;
	// Header references (basenames, no extension): e.g. "dvxi5", "full_00".
	String get_terrain_ref() const;
	String get_environment_ref() const;
	Dictionary get_info() const;
	// EnvFile.apply_mission_overrides() payload from the attrib-gated header.
	Dictionary get_environment_overrides() const;

	int get_entity_count(int kind) const;
	// Array of dictionaries; see entity_to_dictionary() for the fields.
	Array get_entities(int kind) const;
	// One entity by (kind, index) as a dictionary (same fields as get_entities), or {}
	// if out of range. O(1) — prefer this over scanning get_entities for a single hit.
	Dictionary get_entity(int kind, int index) const;
	Array get_all_entities() const;

	// --- Authoring (Phase 1) --------------------------------------------------
	// Move an existing entity. `position` is mission-space (x, y, z); `rotation_deg`
	// is (pitch, yaw, roll) in degrees, rounded to the int fields the format stores.
	// Returns false if (kind, index) is out of range. Sets the dirty flag on success.
	bool set_entity_transform(int kind, int index, const Vector3 &position, const Vector3 &rotation_deg);
	// Mutate a single editable scalar property on the entity at (kind, index). The
	// property name matches the entity dictionary key it edits; the editable set is:
	// "team", "group", "waypoint_id", "wp_number", "perception", "accuracy",
	// "alert_state", "min_engagement_distance", "max_engagement_distance",
	// "max_attack_distance", "spawn_count", "max_simultaneous", "ai_flags". The
	// underlying lib setter (set_entity_properties) replaces all property fields at
	// once, so this reads the entity's current properties, overwrites only the named
	// one, then writes the lot back. Returns false if (kind, index) is out of range or
	// `property` is not a known editable name. Sets the dirty flag on success.
	bool set_entity_property_int(int kind, int index, const String &property, int value);
	// String counterpart for the fixed-string entity fields "name1" (AI class / iai_name) and
	// "name2" (AI script / ai_textfile). Same seed-then-overwrite-one model as the int setter;
	// the value is truncated to the format's 8-byte slot. Sets the dirty flag on success.
	bool set_entity_property_string(int kind, int index, const String &property, const String &value);
	// Set one mission-header field, mirroring MissionDocument::set_header_*. String fields:
	// mission_name|designer|briefing|terrain|environment. Int fields: climate|weather|mission_type|
	// attrib_flags|start_time|minutes_per_day|player_health|max_saves|music|reverb|wind_speed|wind_direction.
	// set_header_flag toggles one ATTRIB_* bit; set_header_float takes "map_zoom". Dirty on success.
	bool set_header_string(const String &field, const String &value);
	bool set_header_int(const String &field, int value);
	bool set_header_flag(int bit, bool on);
	bool set_header_float(const String &field, float value);
	// Game mode is a single-select among the 11 ATTRIB_* mode bits (or none = Single Player). The
	// engine stores it as exactly one bit of attrib_flags and selects by priority [orig: sub_402770
	// decode @0x4050c7, encode @0x4031cd `and 0x7CFFFF`/`or <bit>`, dfx2med.exe]. get_game_mode returns
	// the active mode bit (0 = Single Player); set_game_mode clears all 11 mode bits then sets `bit`
	// (0 clears all). Returns int64 because ATTRIB_SEARCH_AND_DESTROY (0x80000000) overflows a signed int.
	int64_t get_game_mode() const;
	bool set_game_mode(int64_t bit);
	// Place a new entity of `kind` for `item_id` (an items.def id) at `position`
	// (mission-space x, y, z) with `rotation_deg` (pitch, yaw, roll), rounded to the
	// int fields the format stores. The lib seeds the rest of the record with sane
	// defaults (see make_default_entity). Returns the new entity as a dictionary
	// (same fields as get_entity, including its assigned "index"), or {} if no mission
	// is loaded or the kind is invalid. Sets the dirty flag on success.
	Dictionary add_entity(int kind, int item_id, const Vector3 &position, const Vector3 &rotation_deg);
	// Remove the entity at (kind, index). The lib erases it from its kind's list, so
	// every later entity of that kind shifts down by one index (callers holding indices
	// must re-fetch). Markers also repair the waypoint paths that referenced them.
	// Returns false if (kind, index) is out of range. Sets the dirty flag on success.
	bool remove_entity(int kind, int index);

	// --- Authoring facade (libs/mission authoring.h) ---------------------------
	// The editing policies the editor used to hand-roll, as engine capabilities:
	// the items.def-type -> entity-list table, the author-time Ground-userpoint
	// bake [orig: sub_401A90, dfx2med.exe], and the path-consistent marker
	// item-id policy. All geometry is mission (BMS) space; the Godot shells convert
	// with MissionObjectPlacer's axis maps (godot_to_bms_position /
	// ground_anchor_bms) before calling in.
	//
	// The KIND_* value a new placement of an items.def `type` lands in.
	static int kind_for_item_type(int def_item_type);
	// Place a new rotation-zero entity with its ground point at `ground_hit_bms`
	// (unrotated anchor subtraction; markers ignore the anchor). Returns the new
	// entity dictionary like add_entity, or {} when rejected. Dirty on success.
	Dictionary place_entity_grounded(int item_id, int def_item_type, const Vector3 &ground_hit_bms, const Vector3 &ground_anchor_bms);
	// Re-ground an existing entity at `ground_hit_bms`, keeping its rotation
	// (full rotated bake). Returns false when (kind, index) is out of range.
	bool move_entity_grounded(int kind, int index, const Vector3 &ground_hit_bms, const Vector3 &ground_anchor_bms);
	// Bulk re-ground after a terrain height change. Each request Dictionary
	// carries { kind, index, ground_hit_bms: Vector3, ground_anchor_bms: Vector3 };
	// rows whose baked origin is within `epsilon` of the stored one are skipped
	// (markers store the hit directly, like move_entity_grounded). apply = false
	// counts the would-move rows without writing, so the editor's prompt count and
	// the apply share one policy. Returns the moved (or would-move) count; dirty
	// only when something actually moved.
	int reground_entities(const Array &requests, float epsilon = 0.01f, bool apply = true);
	// Apply-mode bulk re-ground that also reports WHICH rows moved, so the editor
	// can update its placed world in place instead of re-baking it. Returns
	// { "moved": int, "rows": PackedInt32Array, "positions": PackedVector3Array }
	// where rows are indices into the CALLER'S `requests` Array (the parser skips
	// index < 0 rows, so engine row i is not requests[i] in general) and
	// positions are the post-bake BMS origins parallel to rows, read back from
	// the document after the write — exactly what it now stores.
	Dictionary reground_entities_apply(const Array &requests, float epsilon = 0.01f);
	// The item id a NEW marker on `path_index` should use: the path's own first
	// marker's id, else the canonical waypoint id (106005).
	int marker_item_id_for_path(int path_index) const;
	// add_waypoint_marker with the item-id policy applied and the hit stored
	// directly (markers have no anchor). Same return shape as add_waypoint_marker.
	Dictionary add_path_marker_grounded(int path_index, const Vector3 &ground_hit_bms, int insert_index = -1);

	// --- Waypoints ------------------------------------------------------------
	// A mission carries 128 fixed waypoint paths; a path is an ordered list of marker
	// indices (each an index into the KIND_MARKER entity list) plus flags (WP_FLAG_*).
	// Units follow a path via their per-entity "waypoint_id". The lib (libs/mission)
	// already parses and round-trips all of this byte-faithfully; this is the binding.
	//
	// Every populated path summary as { index, flags, marker_count } -- the cheap read
	// for a path list (does not materialize the marker indices).
	Array get_waypoint_summaries() const;
	// One path as { index, flags, marker_count, marker_indices: PackedInt32Array } (the
	// indices are KIND_MARKER entity indices), or {} if `index` is out of range.
	Dictionary get_waypoint_path(int index) const;
	// All 128 paths in the same shape as get_waypoint_path (for the in-world overlay).
	Array get_waypoint_paths() const;
	// Replace a path's ordered marker list and flags. `marker_indices` are KIND_MARKER
	// entity indices; an empty list clears the path. Does NOT create or delete marker
	// entities -- it only rewrites which markers (and in what order) the path references,
	// so it is the call for reorder / flag-only edits. Returns false if `index` is out of
	// range or any marker index is invalid. Sets the dirty flag on success.
	bool set_waypoint_path(int index, const PackedInt32Array &marker_indices, int flags);
	// Empty a path (drop all its marker references; the marker entities are left in place).
	// Returns false if `index` is out of range. Sets the dirty flag on success.
	bool clear_waypoint_path(int index);
	// Create a new marker entity for `marker_item_id` at the given transform AND link it
	// into path `path_index` at `insert_index` (-1 = append). One call both adds the
	// KIND_MARKER entity and references it from the path. Returns { "marker": <entity dict>,
	// "path": <path dict> } or {} if no mission is loaded or the path / insert index is
	// invalid. Rotation is rounded to integer degrees (same contract as add_entity). Sets
	// the dirty flag on success.
	Dictionary add_waypoint_marker(int path_index, int marker_item_id, const Vector3 &position, const Vector3 &rotation_deg, int insert_index);

	// --- Area triggers / restriction zones ------------------------------------
	// A mission carries N 32-byte axis-aligned box zones (out-of-bounds / objective regions). The
	// bounds-check consumers read them as interleaved per-axis fixed-point + a flags dword at off 28
	// (Entity_IsTeamInTriggerBounds @0x43c75c). The lib (libs/mission) round-trips them byte-faithfully;
	// this is the binding. A zone dictionary is { index, id, min: Vector3, max: Vector3, active: bool,
	// constrain_z: bool, raw_flags: int }; min/max are mission-space corners (the placement layer
	// converts to Godot space, same as entities). `id` is the off-0 dword (Phase-5 UNKNOWN; carried raw).
	int get_area_trigger_count() const;
	Array get_area_triggers() const;
	// One zone by index, or {} if out of range.
	Dictionary get_area_trigger(int index) const;
	// Append a new zone with the given mission-space corners and flags; returns its dictionary (with
	// the assigned "index"), or {} if no mission is loaded. min/max are normalized so min<=max per axis
	// (the engine does NOT auto-swap area triggers, so the editor does it). Dirty on success.
	Dictionary add_area_trigger(const Vector3 &min_bounds, const Vector3 &max_bounds, bool active, bool constrain_z, int zone_id);
	// Overwrite the zone at `index`; same normalization + return shape as add. {} if out of range.
	Dictionary set_area_trigger(int index, const Vector3 &min_bounds, const Vector3 &max_bounds, bool active, bool constrain_z, int zone_id);
	// Remove the zone at `index`. Later zones shift down by one (callers holding indices must re-fetch).
	// Triggers referencing a zone by *IsWithinArea param2 are NOT auto-repaired (index semantics are
	// Phase-5 UNKNOWN); the editor warns. Returns false if out of range. Dirty on success.
	bool remove_area_trigger(int index);

	// --- Weapon loadout + groups (mission-global, Phase 3) --------------------
	// The weapon loadout is stored as canonical four-field BMS records — the kit tuple
	// {name, ammoPri, ammoSec, flags} (net-re §5.63). get_weapon_loadout() returns dictionaries
	// {index, name, ammo_primary, ammo_secondary, flags}; flags is the per-ammo damage-class input
	// (1 = x0.9, 2 = x1.1, otherwise neutral). set_weapon_loadout() requires only a name; omitted
	// values default to "-1", and all four strings survive re-serialization.
	// Dirty on success.
	Array get_weapon_loadout() const;
	bool set_weapon_loadout(const Array &entries);
	// The .bms secondary chunk's per-map weapon rules: [{name, value}] pairs for
	// NovaSimulation.set_weapon_availability [orig: the item_availability chunk ->
	// build_item_restriction_table @0x54DDB0 name-list mode].
	Array get_item_availability() const;
	// Groups: 64 fixed records; field0 is flags, field8 is value, field12 is the canonical constant.
	// A group dictionary is { index, field0, field8, field12 }.
	int get_group_count() const;
	Array get_groups() const;
	Dictionary get_group(int index) const;
	// Overwrite the three editable ints of the group at `index`, preserving every other byte of the
	// 32-byte record. Returns false if out of range. Dirty on success.
	bool set_group(int index, int field0, int field8, int field12);

	// --- Mission scripting (events / triggers / actions, Phase 4) -------------
	// A mission's logic is a list of events; each event chains a contiguous run of triggers (conditions)
	// and a run of actions (effects). The typed model + index bookkeeping live in libs/mission; this is the
	// binding. Dictionary shapes: an event is { index, flags, trigger_index, action_index, trigger_count,
	// action_count, reset_after, delay, unknown5, unknown6 }; a trigger is { index, condition_flags,
	// main_type, main_type_name, sub_type, sub_type_name, param1..4, unknown7, negated, logic_or, logic_xor,
	// logic_operator }; an action is { index, action_type, action_type_name, action_sub_type,
	// action_sub_type_name, param1..4, reserved0, reserved1 }. get_event_chain returns { event, triggers:[],
	// actions:[], references:[], diagnostics:[] } (references/diagnostics resolve cross-links like a trigger
	// pointing at an area zone, or a ResetEvent action pointing at an event). Every mutator dirties on success.
	int get_event_count() const;
	Array get_events() const;
	Dictionary get_event(int index) const;
	Dictionary get_event_chain(int index) const;
	Dictionary get_logic_summary() const;
	// Whole-event CRUD. add_event appends an empty event (fill it via add_event_trigger/action) and returns
	// its dictionary; remove_event drops it and repairs ResetEvent references; set_event edits the event's
	// own attributes (the EventFlags bitfield + the 10-bit reset_after / delay counters).
	Dictionary add_event(int flags, int reset_after, int delay);
	bool remove_event(int index);
	bool set_event(int index, int flags, int reset_after, int delay);
	// Event-local trigger ops. `local_index` is the trigger's position within the event's chain (0-based).
	// `trigger` is an editor dictionary (see trigger_from_dictionary). add appends; set overwrites; remove /
	// move (delta +/-1) reorder. add/set return the refreshed event-chain dictionary, or {} on failure.
	Dictionary add_event_trigger(int event_index, const Dictionary &trigger);
	Dictionary set_event_trigger(int event_index, int local_index, const Dictionary &trigger);
	bool remove_event_trigger(int event_index, int local_index);
	bool move_event_trigger(int event_index, int local_index, int delta);
	// Event-local action ops, mirroring the trigger ops. `action` is an editor dictionary (action_type,
	// action_sub_type, param1..4).
	Dictionary add_event_action(int event_index, const Dictionary &action);
	Dictionary set_event_action(int event_index, int local_index, const Dictionary &action);
	bool remove_event_action(int event_index, int local_index);
	bool move_event_action(int event_index, int local_index, int delta);
	// Enum choice lists for the editor's type dropdowns; each an Array of { value: int, name: String },
	// reflected from libs/mission's name switches so new enum values appear without UI changes. The sub-type
	// lists are composite (depend on the chosen main / action type).
	Array get_trigger_main_types() const;
	Array get_trigger_sub_types(int main_type) const;
	Array get_action_types() const;
	Array get_action_sub_types(int action_type) const;
	Array get_event_flag_bits() const;
	Array get_ai_flag_bits() const;
	Dictionary get_trigger_param_schema(int main_type, int sub_type) const;
	Dictionary get_action_param_schema(int action_type, int action_sub_type) const;

	const opennova::mission::MissionDocument &native_document() const { return document; }

	// Write the document back to disk. save_file() targets the path it was opened
	// from; save_as() targets a new path and adopts it. Both clear the dirty flag and
	// go through the byte-faithful writer in libs/mission. save_file() returns
	// ERR_INVALID_PARAMETER when there is no current path (shell then offers Save As).
	Error save_file();
	Error save_as(const String &path);
	// Stage editor-sampled terrain base heights for the NEXT save that routes through the
	// .mis writer: one 16.16 fixed-point height per entity, FLAT in WRITE ORDER (items,
	// buildings, markers, organics). The .mis writer emits each as the entity's
	// `extra_bheight` — the baked base height the original editor subtracts from the
	// height-locked absolute z [orig: MisLdr_WriteNileProjectXml @ 0x10004930, misldr.dll].
	// Consumed and cleared by the next save_as()/save_file() (any format); when absent,
	// extra_bheight falls back to the entity's parsed value (0 for .bms-sourced documents —
	// positions stay absolute-declared, offsets just lose the baked base).
	void set_mis_base_heights(const PackedInt32Array &flat_write_order);
	bool is_modified() const;

	// --- Undo / redo + dirty (in-memory document snapshots) -------------------
	// The history holds whole-document bms::File copies, never serialized bytes. A continuous
	// gesture (a drag, a run of inspector edits) is bracketed by begin_edit()/commit_edit() and
	// becomes one step; commit pushes a step only if the document actually changed (bms::equal),
	// so a no-op edit adds nothing. One-shot mutations bracket the same way. undo()/redo() swap
	// the live document's file with a stack top in O(1) and cannot fail (no parse). is_dirty() is
	// exact: true iff the document differs from the clean baseline (set by mark_clean() at open /
	// save / new); it falls back to the coarse modified flag before any baseline exists.
	void begin_edit();
	void commit_edit();
	bool can_undo() const;
	bool can_redo() const;
	bool undo();
	bool redo();
	int undo_depth() const;
	void clear_history();
	bool is_dirty() const;
	void mark_clean();

	// A 64-bit content revision of the placed-object records (items / buildings /
	// markers / organics): equal documents share it, any record byte or count change
	// moves it. The editor uses it to decide, after an undo / redo, whether to
	// re-place the baked world or only refresh overlays.
	int64_t object_records_revision() const;
	// { events, zones, object_rev } -- the structural fingerprint the editor captures
	// before a restore and compares after (an event / zone count change means a
	// reindex that invalidates a kept selection; an object-revision change means the
	// baked world must be re-placed). One call replaces three separate probes.
	Dictionary structure_fingerprint() const;
};

} // namespace godot
