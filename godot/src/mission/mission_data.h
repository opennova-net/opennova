#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <formats/mission/mission.h>
#include <runtime/mission/mission_load_plan.h>

namespace godot {

class MissionAreaTrigger;
class MissionEntityRecord;
class MissionEnvironmentOverrides;
class MissionEvent;
class MissionEventAction;
class MissionEventChain;
class MissionEventTrigger;
class MissionGroup;
class MissionInfo;
class MissionLogicSummary;
class MissionWaypointMarker;
class MissionWaypointPath;
class MissionWaypointSummary;
class MissionWeaponLoadoutEntry;

class ResourceRoot;

// The mission document binding: it holds the parsed bms::File (engine/formats/mission)
// and the document facts (source path, last error, loaded / wire-header-only),
// exposes the header (terrain/env refs, metadata) and the placed entities as
// typed records (mission/mission_records.h, ADR 0043 d10), and edits the file
// through the bms_edit free functions (ADR 0043 slice E11: no facade twin of
// the file). The save paths call the byte-faithful
// writers in engine/formats/mission.
class MissionData : public RefCounted {
	GDCLASS(MissionData, RefCounted)

private:
	opennova::bms::File file_;
	bool loaded_ = false;
	// A wire S2C 0x0B header view: metadata only, never a complete mission to save.
	bool header_only_ = false;
	String source_path;
	String last_error;
	// True once an in-memory mutation lands and before the next successful save/load.
	bool modified = false;
	// Staged terrain base heights for the next .mis save (see set_mis_base_heights). Cleared by
	// every save_as() (whichever format ran) and by open_file()/create_default(), so stale
	// heights can never leak onto a different document or a later save.
	PackedInt32Array mis_base_heights;

	// The edit-error bridge: the engine's error text becomes last_error.
	bool edit_failed(const std::string &error);
	Ref<MissionEventChain> event_chain_record(size_t event_index) const;

protected:
public:
	// The loading-screen percentage a mission load stage presents when it
	// starts (engine/runtime/mission/mission_load_plan.h, the witnessed
	// Game_StartMission schedule); LOAD_PROGRESS_COMPLETE is the finished value.
	static int load_progress_percent(int p_stage);

protected:
	static void _bind_methods();

public:
	// The BMS record families (opennova::mission::EntityKind), bound as an enum
	// so the entity API's `kind` parameters are typed.
	enum EntityKind : int {
		KIND_MARKER = static_cast<int>(opennova::mission::EntityKind::Marker),
		KIND_ITEM = static_cast<int>(opennova::mission::EntityKind::Item),
		KIND_BUILDING = static_cast<int>(opennova::mission::EntityKind::Building),
		KIND_ORGANIC = static_cast<int>(opennova::mission::EntityKind::Organic),
	};
	// Bound as plain constants. Fixed uint32_t underlying type so the high game-mode bits
	// (ATTRIB_SEARCH_AND_DESTROY 0x80000000, ATTRIB_GAME_MODE_MASK 0xFF830000) bind to
	// GDScript as positive values instead of wrapping to negative signed-int.
	enum : uint32_t {
		// Mirrors opennova::mission::bms::WaypointFlags (engine/runtime/mission). Bound as constants so
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
		// Mirrors opennova::mission::MissionLoadStage (engine/runtime/mission/
		// mission_load_plan.h), pinned in the .cpp: the load stages the shell walks,
		// each presenting load_progress_percent(stage) when it starts.
		LOAD_STAGE_ENVIRONMENT = 0,
		LOAD_STAGE_TERRAIN = 1,
		LOAD_STAGE_OBJECTS = 2,
		LOAD_STAGE_RUNTIME = 3,
		LOAD_STAGE_AUDIO = 4,
		LOAD_STAGE_EFFECTS = 5,
		LOAD_STAGE_FINISH = 6,
		LOAD_PROGRESS_COMPLETE = 100,
		// items.def id = wire type id + this offset (engine/runtime/mission kItemIdOffset;
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
	// Build the client-side mission metadata view from the exact 616-byte BMS
	// header carried by retail S2C 0x0B. This never opens a local .bms and the
	// resulting document deliberately has no authored body records.
	Error open_wire_header(const PackedByteArray &p_header_bytes);
	// Load a .bms/.mis by flat name through the mounted resource root (VFS), so missions packed in
	// PFF archives load at runtime. p_lookup_policy uses ResourceRoot::LookupPolicy ordinals;
	// its default preserves the mounted session policy for every existing caller.
	Error open_from_resource_root(const Ref<ResourceRoot> &p_resource_root, const String &p_name,
			int p_lookup_policy = 0);
	bool is_loaded() const;
	bool is_wire_header_only() const;
	String get_source_path() const;
	String get_last_error() const;

	String get_mission_name() const;
	// Header references (basenames, no extension): e.g. "dvxi5", "full_00".
	String get_terrain_ref() const;
	String get_environment_ref() const;
	// The header as a record (mission/mission_info.h).
	Ref<MissionInfo> get_info() const;
	// EnvFile.apply_mission_overrides() payload from the attrib-gated header
	// (env/mission_environment_overrides.h; env::bms_env_overrides_from_header).
	Ref<MissionEnvironmentOverrides> get_environment_overrides() const;

	int get_entity_count(EntityKind kind) const;
	// The entity records of one kind (mission/mission_records.h MissionEntityRecord).
	TypedArray<MissionEntityRecord> get_entities(EntityKind kind) const;
	// One entity by (kind, index), or null if out of range. O(1) — prefer this
	// over scanning get_entities for a single hit.
	Ref<MissionEntityRecord> get_entity(EntityKind kind, int index) const;
	// Every kind in the placement order (markers, items, buildings, organics).
	TypedArray<MissionEntityRecord> get_all_entities() const;

	// --- Authoring (Phase 1) --------------------------------------------------
	// Move an existing entity. `position` is mission-space (x, y, z); `rotation_deg`
	// is (pitch, yaw, roll) in degrees, rounded to the int fields the format stores.
	// Returns false if (kind, index) is out of range. Sets the dirty flag on success.
	bool set_entity_transform(EntityKind kind, int index, const Vector3 &position, const Vector3 &rotation_deg);
	// Mutate a single editable scalar property on the entity at (kind, index). The
	// property name matches the entity dictionary key it edits; the editable set is:
	// "team", "group", "waypoint_id", "wp_number", "perception", "accuracy",
	// "alert_state", "min_engagement_distance", "max_engagement_distance",
	// "max_attack_distance", "spawn_count", "max_simultaneous", "ai_flags". The
	// underlying lib setter (set_entity_properties) replaces all property fields at
	// once, so this reads the entity's current properties, overwrites only the named
	// one, then writes the lot back. Returns false if (kind, index) is out of range or
	// `property` is not a known editable name. Sets the dirty flag on success.
	bool set_entity_property_int(EntityKind kind, int index, const String &property, int value);
	// String counterpart for the fixed-string entity fields "name1" (AI class / iai_name) and
	// "name2" (AI script / ai_textfile). Same seed-then-overwrite-one model as the int setter;
	// the value is truncated to the format's 8-byte slot. Sets the dirty flag on success.
	bool set_entity_property_string(EntityKind kind, int index, const String &property, const String &value);
	// Set one mission-header field, mirroring mission::set_header_*. String fields:
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
	// defaults (see make_default_entity). Returns the new entity record (with
	// its assigned index), or null if no mission is loaded. Sets the dirty flag
	// on success.
	Ref<MissionEntityRecord> add_entity(EntityKind kind, int item_id, const Vector3 &position, const Vector3 &rotation_deg);
	// Remove the entity at (kind, index). The lib erases it from its kind's list, so
	// every later entity of that kind shifts down by one index (callers holding indices
	// must re-fetch). Markers also repair the waypoint paths that referenced them.
	// Returns false if (kind, index) is out of range. Sets the dirty flag on success.
	bool remove_entity(EntityKind kind, int index);

	// --- Waypoints ------------------------------------------------------------
	// A mission carries 128 fixed waypoint paths; a path is an ordered list of marker
	// indices (each an index into the KIND_MARKER entity list) plus flags (WP_FLAG_*).
	// Units follow a path via their per-entity "waypoint_id". The lib (engine/runtime/mission)
	// already parses and round-trips all of this byte-faithfully; this is the binding.
	//
	// Every populated path summary (index, flags, marker_count) -- the cheap read
	// for a path list (does not materialize the marker indices).
	TypedArray<MissionWaypointSummary> get_waypoint_summaries() const;
	// One path (index, flags, marker_count, marker_indices: the KIND_MARKER
	// entity indices), or null if `index` is out of range.
	Ref<MissionWaypointPath> get_waypoint_path(int index) const;
	// All 128 paths in the same shape as get_waypoint_path (for the in-world overlay).
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
	// KIND_MARKER entity and references it from the path. Returns the new marker
	// record with the path it now belongs to, or null if no mission is loaded or
	// the path / insert index is invalid. Rotation is rounded to integer degrees
	// (same contract as add_entity). Sets the dirty flag on success.
	Ref<MissionWaypointMarker> add_waypoint_marker(int path_index, int marker_item_id, const Vector3 &position, const Vector3 &rotation_deg, int insert_index);

	// --- Area triggers / restriction zones ------------------------------------
	// A mission carries N 32-byte axis-aligned box zones (out-of-bounds / objective regions). The
	// bounds-check consumers read them as interleaved per-axis fixed-point + a flags dword at off 28
	// (Entity_IsTeamInTriggerBounds @0x43c75c). The lib (engine/runtime/mission) round-trips them byte-faithfully;
	// this is the binding. A zone record (MissionAreaTrigger) carries index, id, min/max (mission-space
	// corners; the placement layer converts to Godot space, same as entities), active, constrain_z and
	// raw_flags. `id` is the off-0 dword (Phase-5 UNKNOWN; carried raw).
	int get_area_trigger_count() const;
	TypedArray<MissionAreaTrigger> get_area_triggers() const;
	// One zone by index, or null if out of range.
	Ref<MissionAreaTrigger> get_area_trigger(int index) const;
	// Append a new zone with the given mission-space corners and flags; returns its record (with
	// the assigned index), or null if no mission is loaded. min/max are normalized so min<=max per axis
	// (the engine does NOT auto-swap area triggers, so the editor does it). Dirty on success.
	Ref<MissionAreaTrigger> add_area_trigger(const Vector3 &min_bounds, const Vector3 &max_bounds, bool active, bool constrain_z, int zone_id);
	// Overwrite the zone at `index`; same normalization + return shape as add. Null if out of range.
	Ref<MissionAreaTrigger> set_area_trigger(int index, const Vector3 &min_bounds, const Vector3 &max_bounds, bool active, bool constrain_z, int zone_id);
	// Remove the zone at `index`. Later zones shift down by one (callers holding indices must re-fetch).
	// Triggers referencing a zone by *IsWithinArea param2 are NOT auto-repaired (index semantics are
	// Phase-5 UNKNOWN); the editor warns. Returns false if out of range. Dirty on success.
	bool remove_area_trigger(int index);

	// --- Weapon loadout + groups (mission-global, Phase 3) --------------------
	// The weapon loadout is stored as canonical four-field BMS records — the kit tuple
	// {name, ammoPri, ammoSec, flags} (net-re §5.63). get_weapon_loadout() returns
	// MissionWeaponLoadoutEntry records (index, name, ammo_primary, ammo_secondary, flags); flags is
	// the per-ammo damage-class input (1 = x0.9, 2 = x1.1, otherwise neutral). set_weapon_loadout()
	// takes records authored through MissionWeaponLoadoutEntry.make (omitted values default to "-1"),
	// and all four strings survive re-serialization. Dirty on success.
	TypedArray<MissionWeaponLoadoutEntry> get_weapon_loadout() const;
	bool set_weapon_loadout(const TypedArray<MissionWeaponLoadoutEntry> &entries);
	// Groups: 64 fixed records; field0 is flags, field8 is value, field12 is the canonical constant
	// (MissionGroup: index, field0, field8, field12).
	int get_group_count() const;
	TypedArray<MissionGroup> get_groups() const;
	Ref<MissionGroup> get_group(int index) const;
	// Overwrite the three editable ints of the group at `index`, preserving every other byte of the
	// 32-byte record. Returns false if out of range. Dirty on success.
	bool set_group(int index, int field0, int field8, int field12);

	// --- Mission scripting (events / triggers / actions, Phase 4) -------------
	// A mission's logic is a list of events; each event chains a contiguous run of triggers (conditions)
	// and a run of actions (effects). The typed model + index bookkeeping live in engine/runtime/mission; this is the
	// binding over the mission_records.h records: MissionEvent (index, flags, trigger_index, action_index,
	// trigger_count, action_count, reset_after, delay, unknown5, unknown6), MissionEventTrigger (index,
	// condition_flags, main_type(+name), sub_type(+name), param1..4, unknown7, negated, logic_or, logic_xor,
	// logic_operator) and MissionEventAction (index, action_type(+name), action_sub_type(+name), param1..4,
	// reserved0, reserved1). get_event_chain returns a MissionEventChain (event, triggers, actions,
	// references, diagnostics — references/diagnostics resolve cross-links like a trigger pointing at an
	// area zone, or a ResetEvent action pointing at an event). Every mutator dirties on success.
	int get_event_count() const;
	TypedArray<MissionEvent> get_events() const;
	Ref<MissionEvent> get_event(int index) const;
	Ref<MissionEventChain> get_event_chain(int index) const;
	Ref<MissionLogicSummary> get_logic_summary() const;
	// Whole-event CRUD. add_event appends an empty event (fill it via add_event_trigger/action) and returns
	// its record; remove_event drops it and repairs ResetEvent references; set_event edits the event's
	// own attributes (the EventFlags bitfield + the 10-bit reset_after / delay counters).
	Ref<MissionEvent> add_event(int flags, int reset_after, int delay);
	bool remove_event(int index);
	bool set_event(int index, int flags, int reset_after, int delay);
	// Event-local trigger ops. `local_index` is the trigger's position within the event's chain (0-based).
	// `trigger` is a MissionEventTrigger authored through make (main/sub type, params, the three logic
	// booleans); the seed's unmodeled condition_flags high bits and unknown7 survive an edit. add appends;
	// set overwrites; remove / move (delta +/-1) reorder. add/set return the refreshed event chain, or
	// null on failure.
	Ref<MissionEventChain> add_event_trigger(int event_index, const Ref<MissionEventTrigger> &trigger);
	Ref<MissionEventChain> set_event_trigger(int event_index, int local_index, const Ref<MissionEventTrigger> &trigger);
	bool remove_event_trigger(int event_index, int local_index);
	bool move_event_trigger(int event_index, int local_index, int delta);
	// Event-local action ops, mirroring the trigger ops. `action` is a MissionEventAction authored through
	// make (action_type, action_sub_type, param1..4); the reserved words ride the seed.
	Ref<MissionEventChain> add_event_action(int event_index, const Ref<MissionEventAction> &action);
	bool remove_event_action(int event_index, int local_index);
	bool move_event_action(int event_index, int local_index, int delta);

	const opennova::bms::File &native_file() const { return file_; }

	// Write the document back to disk. save_file() targets the path it was opened
	// from; save_as() targets a new path and adopts it. Both clear the dirty flag and
	// go through the byte-faithful writer in engine/runtime/mission. save_file() returns
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
	// The same staging fed world-unit floats: each height is encoded to the
	// 16.16 raw here (round-to-nearest, NaN -> 0) so callers never restate the
	// fixed-point convention. Same clearing/apply contract as the raw variant.
	bool is_modified() const;

	// A 64-bit content revision of the placed-object records (items / buildings /
	// markers / organics): equal documents share it, any record byte or count change
	// moves it. The editor uses it to decide, after an undo / redo, whether to
	// re-place the baked world or only refresh overlays.
	int64_t object_records_revision() const;
};

} // namespace godot

VARIANT_ENUM_CAST(godot::MissionData::EntityKind);
