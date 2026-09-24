#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
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

class EntityRef;
class MissionEnvironmentOverrides;
class MissionInfo;

class ResourceRoot;

// The mission document binding: it holds the parsed bms::File (engine/formats/mission)
// and the document facts (source path, last error, loaded / wire-header-only),
// exposes the header (terrain/env refs, metadata) and placed entity identities
// through EntityRef (the presentation carrier), and edits the file
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

protected:
public:
	// The loading-screen percentage a mission load stage presents when it
	// starts (engine/runtime/mission/mission_load_plan.h). WORLD_READY marks
	// local construction; COMPLETE belongs to the shell reveal/admission edge.
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
		LOAD_STAGE_MISSION_SETUP = 0,
		LOAD_STAGE_ENVIRONMENT = 1,
		LOAD_STAGE_TERRAIN = 2,
		LOAD_STAGE_OBJECTS = 3,
		LOAD_STAGE_RUNTIME = 4,
		LOAD_STAGE_AUDIO = 5,
		LOAD_STAGE_EFFECTS = 6,
		LOAD_STAGE_EFFECTS_WARM = 7,
		LOAD_STAGE_FINISH = 8,
		LOAD_PROGRESS_WORLD_READY = 90,
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
	// The header's tile-set name as authored (empty = the .trn tilestrip).
	String get_tile_set_ref() const;
	// The header as a record (mission/mission_info.h).
	Ref<MissionInfo> get_info() const;
	// EnvFile.apply_mission_overrides() payload from the attrib-gated header
	// (env/mission_environment_overrides.h; env::bms_env_overrides_from_header).
	Ref<MissionEnvironmentOverrides> get_environment_overrides() const;

	int get_entity_count(EntityKind kind) const;
	// Entity identities of one kind, using the same carrier as placed models.
	TypedArray<EntityRef> get_entity_refs(EntityKind kind) const;
	// One entity by (kind, index), or null if out of range. O(1) — prefer this
	// over scanning get_entity_refs for a single hit.
	Ref<EntityRef> get_entity_ref(EntityKind kind, int index) const;
	// Every kind in the placement order (markers, items, buildings, organics).
	TypedArray<EntityRef> get_all_entity_refs() const;

	// Authored eulers in degrees (pitch, yaw, roll); zero for an absent entity.
	Vector3 get_entity_rotation(EntityKind kind, int index) const;

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
	// native setter changes the named field and preserves the rest of the record.
	// Returns false if (kind, index) is out of range or
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
	// defaults (see make_default_entity). Returns the new entity identity (with
	// its assigned index), or null if no mission is loaded. Sets the dirty flag
	// on success.
	Ref<EntityRef> add_entity(EntityKind kind, int item_id, const Vector3 &position, const Vector3 &rotation_deg);
	// Remove the entity at (kind, index). The lib erases it from its kind's list, so
	// every later entity of that kind shifts down by one index (callers holding indices
	// must re-fetch). Markers also repair the waypoint paths that referenced them.
	// Returns false if (kind, index) is out of range. Sets the dirty flag on success.
	bool remove_entity(EntityKind kind, int index);

	// Native zone records feed EntityIndex directly. Authoring returns the
	// appended index (-1 without a document); corners are mission-space.
	int get_area_trigger_count() const;
	int add_area_trigger(const Vector3 &min_bounds, const Vector3 &max_bounds,
			bool active, bool constrain_z, int zone_id);

	// Each loadout input row is {name, ammo_primary, ammo_secondary, flags}.
	// The native document validates and serializes these four strings.
	bool set_weapon_loadout(const TypedArray<PackedStringArray> &entries);

	// Author mission event inputs without materializing record wrappers.
	// add_event returns its index (-1 without a document); append operations
	// validate the owning event and return whether the native edit succeeded.
	int get_event_count() const;
	int add_event(int flags, int reset_after, int delay);
	bool add_event_trigger(int event_index, int main_type, int sub_type,
			int param1 = 0, int param2 = 0, int param3 = 0, int param4 = 0,
			int condition_flags = 0);
	bool add_event_action(int event_index, int action_type, int sub_type = 0,
			int param1 = 0, int param2 = 0, int param3 = 0, int param4 = 0);

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
