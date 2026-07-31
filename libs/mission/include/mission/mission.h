#pragma once

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "mission/bms.h"

namespace opennova::mission {

constexpr int kItemIdOffset = 100000;
constexpr size_t kMaxWaypointPaths = bms::kWaypointRecordCount;
constexpr size_t kMaxWaypointPathMarkers = 32;

enum class EntityKind : int {
	Marker = 0,
	Item = 1,
	Building = 2,
	Organic = 3,
};

struct MissionInfo {
	std::string mission_name;
	std::string designer;
	std::string briefing;
	std::string terrain;
	std::string environment;
	int climate = 0;
	int weather = 0;
	int mission_type = 0;
	int attrib_flags = 0;
	int start_time = 0;        // raw packed u16 (NOT decoded HH:MM)
	int minutes_per_day = 0;
	int player_health = 0;
	int max_saves = 0;
	int music = 0;
	int reverb = 0;
	int wind_speed = 0;
	int wind_direction = 0;
	float map_zoom = 0.0f;
	// Per-mission environment overrides, gated by attrib_flags bits
	// (WaterOverrideEnable 0x1, FogDistanceOverrideEnable 0x2,
	// FogColorOverrideEnable 0x4). Applied at load via EnvFile's override layer;
	// see docs/env/env-tod-re.md [orig: Game_LoadTerrainDuringConnect @ 0x520710].
	int water_override = 0;     // s16 half-world-units; engine shifts <<15
	int fog_override = 0;       // fog distance, world units
	int fog_color[3] = {0, 0, 0};
	int water_color[3] = {0, 0, 0};
	int water_murk = 0;         // 0..255 byte; engine multiplies by 0.01
};

struct EntityTransform {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
	int pitch = 0;
	int yaw = 0;
	int roll = 0;
};

struct EntityRecord {
	EntityKind kind = EntityKind::Item;
	size_t index = 0;
	int item_id = 0;
	int bms_type_id = 0;
	int bms_id = 0;
	EntityTransform transform;
	int group_id = 0;
	int waypoint_id = 0;
	int wp_number = 0;
	int team = 0;
	int ai_flags = 0;
	int perception = 0;
	int accuracy = 0;
	int alert_state = 0;
	int min_engagement_distance = 0;
	int max_engagement_distance = 0;
	int max_attack_distance = 0;
	int spawn_count = 0;
	int max_simultaneous = 0;  // = no_more_than (byte 74); paired with the RemoveIfMoreThan AI flag
	int no_less_than = 0;      // byte 75; paired with the RemoveIfLessThan AI flag
	int map_symbol = 0;        // byte 81; tactical-map icon
	std::string name1;         // bytes 104..111 (iai_name): AI class name
	std::string name2;         // bytes 112..119 (ai_textfile): AI script file
};

struct EntityProperties {
	int group_id = 0;
	int waypoint_id = 0;
	int wp_number = 0;
	int team = 0;
	int ai_flags = 0;
	int perception = 0;
	int accuracy = 0;
	int alert_state = 0;
	int min_engagement_distance = 0;
	int max_engagement_distance = 0;
	int max_attack_distance = 0;
	int spawn_count = 0;
	int max_simultaneous = 0;  // = no_more_than (byte 74)
	int no_less_than = 0;      // byte 75
	int map_symbol = 0;        // byte 81
	std::string name1;         // iai_name (8 bytes)
	std::string name2;         // ai_textfile (8 bytes)
};

struct WaypointSummary {
	size_t index = 0;
	int flags = 0;
	int marker_count = 0;
};

struct WaypointPath {
	size_t index = 0;
	int flags = 0;
	std::vector<int> marker_indices;
};

struct AreaTriggerRecord {
	size_t index = 0;
	int wp_number = 0;        // off 0: zone id (carried here for ABI stability; not a coordinate)
	float min_x = 0.0f;
	float min_y = 0.0f;
	float min_z = 0.0f;
	float max_x = 0.0f;
	float max_y = 0.0f;
	float max_z = 0.0f;
	int reserved = 0;         // raw 32-bit flags dword at off 28
	bool active = false;      // flags & 0x01 (zone active)
	bool constrain_z = false; // flags & 0x02 (else Z unbounded ±16384.0)
};

// Public editor/runtime view of one weapon-loadout record — the engine-wide kit tuple
// {name, ammoPri, ammoSec, flags} (net-re §5.63), sanitized to four NUL-terminated fields.
// ammo_primary/ammo_secondary are requested clip counts (-1 = the weapon's default fill).
// flags is load-bearing: it becomes the per-ammo damage-class byte consumed by
// Weapon_CalcImpactDamage (1 = x0.9, 2 = x1.1, every other value neutral), so it must
// survive every editor and FFI round-trip. New rows default it to "-1".
struct WeaponLoadoutEntry {
	std::string name;
	std::string ammo_primary;
	std::string ammo_secondary;
	std::string flags = "-1";
};

// Public view of one item-availability record from the .bms secondary chunk — the
// per-map weapon rules ({name, status} pairs) the mission-list scanners compile into
// the availability template [orig: build_item_restriction_table @0x54DDB0 name-list
// mode over the chunk; status -1 maps to 3 mission-allowed at apply].
struct ItemAvailabilityEntry {
	std::string name;
	int status = 1;
};

// Typed view of a 32-byte group record. ABI field names are retained for compatibility:
// field0 = 2-bit flags, field8 = value, field12 = writer-confirmed constant 10.
struct GroupFields {
	size_t index = 0;
	int field0 = 0;
	int field8 = 0;
	int field12 = 0;
};

struct MissionEventRecord {
	size_t index = 0;
	int flags = 0;
	int trigger_index = 0;
	int action_index = 0;
	int trigger_count = 0;
	int action_count = 0;
	int reset_after = 0;
	int delay = 0;
	int unknown5 = 0;
	int unknown6 = 0;
};

struct MissionTriggerRecord {
	size_t index = 0;
	int condition_flags = 0;
	int main_type = 0;
	std::string main_type_name;
	int sub_type = 0;
	std::string sub_type_name;
	int param1 = 0;
	int param2 = 0;
	int param3 = 0;
	int param4 = 0;
	int unknown7 = 0;
	bool negated = false;
	bool logic_or = false;
	bool logic_xor = false;
	std::string logic_operator;
};

struct MissionActionRecord {
	size_t index = 0;
	int action_type = 0;
	std::string action_type_name;
	int action_sub_type = 0;
	std::string action_sub_type_name;
	int param1 = 0;
	int param2 = 0;
	int param3 = 0;
	int param4 = 0;
	int reserved0 = 0;
	int reserved1 = 0;
};

struct MissionLogicReference {
	std::string source_kind;
	int source_index = -1;
	std::string target_kind;
	int target_index = -1;
	int param_slot = 0;
	int raw_value = 0;
	std::string label;
	bool valid = false;
};

struct MissionLogicDiagnostic {
	std::string severity;
	std::string code;
	std::string message;
	std::string subject_kind;
	int subject_index = -1;
};

struct MissionEventChain {
	MissionEventRecord event;
	std::vector<MissionTriggerRecord> triggers;
	std::vector<MissionActionRecord> actions;
	std::vector<MissionLogicReference> references;
	std::vector<MissionLogicDiagnostic> diagnostics;
};

struct MissionLogicSummary {
	size_t event_count = 0;
	size_t trigger_count = 0;
	size_t action_count = 0;
	size_t area_trigger_count = 0;
	size_t diagnostic_count = 0;
};

// One row of an enum choice list (value + display name), produced by the *_types() reflectors below.
// The reflectors probe the existing name-mapping switches in mission.cpp so a new enum value added to
// bms.h shows up in the editor's dropdowns for free; entries the switch does not name are omitted.
struct MissionEnumEntry {
	int value = 0;
	std::string name;
};

class MissionDocument {
public:
	MissionDocument();
	~MissionDocument();

	MissionDocument(MissionDocument &&) noexcept;
	MissionDocument &operator=(MissionDocument &&) noexcept;

	MissionDocument(const MissionDocument &) = delete;
	MissionDocument &operator=(const MissionDocument &) = delete;

	bool load_bms_file(const std::string &path);
	bool load_bms_bytes(const uint8_t *data, size_t size);
	bool load_mis_file(const std::string &path);
	bool load_mis_text(const std::string &text);
	bool save_bms_file(const std::string &path);
	bool write_bms_bytes(std::vector<uint8_t> &out);
	// The .mis writer. `base_heights` (optional): editor-sampled terrain heights under each entity,
	// 16.16 fixed-point, FLAT in WRITE ORDER (items, buildings, markers, organics). When provided,
	// each in-range entry is emitted as that entity's `extra_bheight` (the baked base height the
	// original editor subtracts from the height-locked absolute z); out-of-range / absent entries
	// fall back to the entity's own mis_extra_bheight. See docs/mission/mis-format-re.md (D-MIS-4).
	bool save_mis_file(const std::string &path, const std::vector<int32_t> *base_heights = nullptr);
	bool write_mis_text(std::string &out, const std::vector<int32_t> *base_heights = nullptr);
	void clear();
	// Build a minimal, valid, empty mission in memory (no file). Resets to the loaded state
	// with a correct magic + version and the fixed waypoint/group/layer tables backfilled via
	// sync_counts(), so write_bms_bytes() produces a buffer parse() accepts. Always succeeds.
	void create_default();

	bool is_loaded() const;
	const std::string &source_path() const;
	const std::string &last_error() const;

	MissionInfo info() const;

	// Mission-header editing. Field names match the MissionInfo members above.
	bool set_header_string(const std::string &field, const std::string &value);
	bool set_header_int(const std::string &field, int value);
	bool set_header_flag(int bit, bool on);              // single attrib_flags bit
	bool set_header_float(const std::string &field, float value);

	size_t entity_count(EntityKind kind) const;
	bool get_entity(EntityKind kind, size_t index, EntityRecord &out) const;
	bool set_entity_transform(EntityKind kind, size_t index, const EntityTransform &transform);
	bool set_entity_properties(EntityKind kind, size_t index, const EntityProperties &properties, EntityRecord *out = nullptr);
	// Edit a single named property of an entity (mirrors set_header_int/set_header_string). The
	// name->member mapping lives in mission.cpp, so callers do not re-derive it. Unknown name -> false
	// with last_error set; out-of-range index -> false.
	bool set_entity_property_int(EntityKind kind, size_t index, const std::string &name, int value);
	bool set_entity_property_string(EntityKind kind, size_t index, const std::string &name, const std::string &value);
	bool add_entity(EntityKind kind, int item_id, const EntityTransform &transform, EntityRecord *out = nullptr);
	bool remove_entity(EntityKind kind, size_t index);
	std::vector<WaypointSummary> waypoint_summaries() const;
	size_t waypoint_path_count() const;
	bool get_waypoint_path(size_t index, WaypointPath &out) const;
	std::vector<WaypointPath> waypoint_paths() const;
	bool set_waypoint_path(size_t index, const std::vector<int> &marker_indices, int flags, WaypointPath *out = nullptr);
	bool clear_waypoint_path(size_t index, WaypointPath *out = nullptr);
	bool add_waypoint_marker(size_t path_index,
	                         int marker_item_id,
	                         const EntityTransform &transform,
	                         int insert_index = -1,
	                         EntityRecord *out_marker = nullptr,
	                         WaypointPath *out_path = nullptr);
	size_t area_trigger_count() const;
	bool get_area_trigger(size_t index, AreaTriggerRecord &out) const;
	std::vector<AreaTriggerRecord> area_triggers() const;
	bool add_area_trigger(const AreaTriggerRecord &record, AreaTriggerRecord *out = nullptr);
	bool set_area_trigger(size_t index, const AreaTriggerRecord &record, AreaTriggerRecord *out = nullptr);
	bool remove_area_trigger(size_t index);
	// Weapon / restriction loadout (mission-global). weapon_loadout() returns the public three-field view;
	// set_weapon_loadout() writes canonical four-field BMS records and refreshes weapon_loadout_chunk_len.
	std::vector<WeaponLoadoutEntry> weapon_loadout() const;
	bool set_weapon_loadout(const std::vector<WeaponLoadoutEntry> &entries);
	// The .bms secondary chunk's per-map weapon rules; read-only runtime view (the
	// editor round-trips the chunk bytes through the writer unchanged).
	std::vector<ItemAvailabilityEntry> item_availability() const;
	// Groups: a fixed array of 64 modeled records. field0 is the 2-bit flags value, field8 is the editable
	// value, and field12 must be the canonical constant 10.
	size_t group_count() const;
	bool get_group(size_t index, GroupFields &out) const;
	std::vector<GroupFields> groups() const;
	bool set_group(size_t index, int field0, int field8, int field12);
	size_t event_count() const;
	bool get_event(size_t index, MissionEventRecord &out) const;
	std::vector<MissionEventRecord> events() const;
	bool set_event(size_t index, const MissionEventRecord &record, MissionEventRecord *out = nullptr);
	size_t trigger_count() const;
	bool get_trigger(size_t index, MissionTriggerRecord &out) const;
	std::vector<MissionTriggerRecord> triggers() const;
	bool set_trigger(size_t index, const MissionTriggerRecord &record, MissionTriggerRecord *out = nullptr);
	size_t action_count() const;
	bool get_action(size_t index, MissionActionRecord &out) const;
	std::vector<MissionActionRecord> actions() const;
	bool set_action(size_t index, const MissionActionRecord &record, MissionActionRecord *out = nullptr);
	bool get_event_chain(size_t index, MissionEventChain &out) const;
	bool insert_event_trigger(size_t event_index, size_t local_index, const MissionTriggerRecord &record, MissionEventChain *out = nullptr);
	bool remove_event_trigger(size_t event_index, size_t local_index, MissionEventChain *out = nullptr);
	bool move_event_trigger(size_t event_index, size_t local_index, int delta, MissionEventChain *out = nullptr);
	bool insert_event_action(size_t event_index, size_t local_index, const MissionActionRecord &record, MissionEventChain *out = nullptr);
	bool remove_event_action(size_t event_index, size_t local_index, MissionEventChain *out = nullptr);
	bool move_event_action(size_t event_index, size_t local_index, int delta, MissionEventChain *out = nullptr);
	// Whole-event add / remove (the only scripting mutators the engine's loader implies but that the
	// insert/remove_event_* helpers above did not cover). add_event appends a fresh empty event (no
	// triggers/actions; the caller fills them via insert_event_trigger/action), applies author-facing flags
	// and confirmed internal bits from record.flags, and returns its index via `out`. remove_event drains
	// the event's trigger and action ranges through the single-element removers (so every other event's
	// trigger_index/action_index stays correct), repairs ResetEvent action references (param1 = event index:
	// decremented past the hole; an exact hit is set to -1 = dangling, which get_event_chain then flags),
	// erases the event, and re-syncs the header counts.
	bool add_event(const MissionEventRecord &record, MissionEventRecord *out = nullptr);
	bool remove_event(size_t index);
	MissionLogicSummary logic_summary() const;

	// Enum choice lists for the editor's type dropdowns, reflected from the name-mapping switches so they
	// track bms.h. trigger_sub_types / action_sub_types are composite (the sub-type set depends on the
	// main / action type), matching the engine's nested switch. event_flag_bits lists the EventFlags bits.
	std::vector<MissionEnumEntry> trigger_main_types() const;
	std::vector<MissionEnumEntry> trigger_sub_types(int main_type) const;
	std::vector<MissionEnumEntry> action_types() const;
	std::vector<MissionEnumEntry> action_sub_types(int action_type) const;
	std::vector<MissionEnumEntry> event_flag_bits() const;
	// Bitmask of every editor-exposed event flag (OR of the event_flag_bits values). The editor rebuilds
	// an event's flags from these checkboxes only, so set_event must preserve the complementary (unmodeled
	// / engine-internal) bits rather than clobber them. See NovaMissionData::set_event.
	int event_flag_mask() const;
	// Per-entity AI attribute flags (bmsi_attributes), surfaced as inspector checkboxes. Lists only labeled
	// author-facing bits; confirmed-but-unlabeled bits such as Attribute17 remain valid through the raw flag
	// value but are not exposed as checkboxes.
	std::vector<MissionEnumEntry> ai_attribute_flag_bits() const;

	const bms::File &bms_file() const;
	bms::File &bms_file();
	void sync_counts();

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace opennova::mission
