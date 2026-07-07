#pragma once

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "mission/bms.h"

#include <io/export.h>
// MISSION_EXPORT marks the flat C ABI (the 8 document-lifecycle functions,
// annotated at declaration AND definition; scripts/lint/abi_exports_baseline.txt
// pins the set on every platform). The unannotated extern "C" authoring
// functions below are C++-static-link surface only (Model B) — annotating a
// declaration whose definition is plain exports it on ELF/Mach-O but not PE,
// which is exactly the drift abi_export_identity exists to catch.
#define MISSION_EXPORT OPENNOVA_API

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

// Public editor view of one weapon / restriction-loadout record. The BMS chunk is sanitized to four
// NUL-terminated fields (name + three values); this API surfaces the name and first two values because
// value3 is consistently a restriction/default slot in shipped missions and is written as "-1" for edits.
struct WeaponLoadoutEntry {
	std::string name;
	std::string value1;
	std::string value2;
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
	// The .mis writer. `base_heights` (optional): host-sampled terrain heights under each entity,
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

extern "C" {

typedef struct OpenNovaMissionDocument OpenNovaMissionDocument;

typedef struct OpenNovaMissionInfo {
	char mission_name[64];
	char designer[64];
	char briefing[512];
	char terrain[64];
	char environment[64];
	int climate;
	int weather;
	int attrib_flags;
	int start_time;
	int minutes_per_day;
	int player_health;
	int max_saves;
	int music;
	int reverb;
} OpenNovaMissionInfo;

typedef struct OpenNovaMissionEntityTransform {
	float x;
	float y;
	float z;
	int pitch;
	int yaw;
	int roll;
} OpenNovaMissionEntityTransform;

typedef struct OpenNovaMissionEntityRecord {
	int kind;
	size_t index;
	int item_id;
	int bms_type_id;
	int bms_id;
	OpenNovaMissionEntityTransform transform;
	int group_id;
	int waypoint_id;
	int wp_number;
	int team;
	int ai_flags;
	int perception;
	int accuracy;
	int alert_state;
	int min_engagement_distance;
	int max_engagement_distance;
	int max_attack_distance;
	int spawn_count;
	int max_simultaneous;
} OpenNovaMissionEntityRecord;

typedef struct OpenNovaMissionEntityProperties {
	int group_id;
	int waypoint_id;
	int wp_number;
	int team;
	int ai_flags;
	int perception;
	int accuracy;
	int alert_state;
	int min_engagement_distance;
	int max_engagement_distance;
	int max_attack_distance;
	int spawn_count;
	int max_simultaneous;
} OpenNovaMissionEntityProperties;

typedef struct OpenNovaMissionWaypointSummary {
	size_t index;
	int flags;
	int marker_count;
} OpenNovaMissionWaypointSummary;

typedef struct OpenNovaMissionWaypointPath {
	size_t index;
	int flags;
	size_t marker_count;
	uint32_t marker_indices[32];
} OpenNovaMissionWaypointPath;

typedef struct OpenNovaMissionAreaTriggerRecord {
	size_t index;
	int wp_number;
	float min_x;
	float min_y;
	float min_z;
	float max_x;
	float max_y;
	float max_z;
	int reserved;
	int active;
	int constrain_z;
} OpenNovaMissionAreaTriggerRecord;

// Loadout entry over FFI. The three on-disk strings are copied into fixed 64-char buffers (real weapon
// names + "-1" values are short); a longer field would be truncated to 63 chars on read.
typedef struct OpenNovaMissionWeaponLoadoutEntry {
	char name[64];
	char value1[64];
	char value2[64];
} OpenNovaMissionWeaponLoadoutEntry;

typedef struct OpenNovaMissionGroupRecord {
	size_t index;
	int field0;
	int field8;
	int field12;
} OpenNovaMissionGroupRecord;

typedef struct OpenNovaMissionEventRecord {
	size_t index;
	int flags;
	int trigger_index;
	int action_index;
	int trigger_count;
	int action_count;
	int reset_after;
	int delay;
	int unknown5;
	int unknown6;
} OpenNovaMissionEventRecord;

typedef struct OpenNovaMissionTriggerRecord {
	size_t index;
	int condition_flags;
	int main_type;
	char main_type_name[32];
	int sub_type;
	char sub_type_name[64];
	int param1;
	int param2;
	int param3;
	int param4;
	int unknown7;
	int negated;
	int logic_or;
	int logic_xor;
	char logic_operator[8];
} OpenNovaMissionTriggerRecord;

typedef struct OpenNovaMissionActionRecord {
	size_t index;
	int action_type;
	char action_type_name[64];
	int action_sub_type;
	char action_sub_type_name[64];
	int param1;
	int param2;
	int param3;
	int param4;
	int reserved0;
	int reserved1;
} OpenNovaMissionActionRecord;

typedef struct OpenNovaMissionLogicSummary {
	size_t event_count;
	size_t trigger_count;
	size_t action_count;
	size_t area_trigger_count;
	size_t diagnostic_count;
} OpenNovaMissionLogicSummary;

typedef struct OpenNovaMissionBytes {
	uint8_t *data;
	size_t size;
} OpenNovaMissionBytes;

MISSION_EXPORT OpenNovaMissionDocument *opennova_mission_create(void);
MISSION_EXPORT void opennova_mission_destroy(OpenNovaMissionDocument *document);
void opennova_mission_clear(OpenNovaMissionDocument *document);
MISSION_EXPORT void opennova_mission_create_default(OpenNovaMissionDocument *document);
MISSION_EXPORT int opennova_mission_load_path(OpenNovaMissionDocument *document, const char *path);
int opennova_mission_load_bytes(OpenNovaMissionDocument *document, const uint8_t *data, size_t size);
int opennova_mission_save_path(OpenNovaMissionDocument *document, const char *path);
int opennova_mission_write_bytes(OpenNovaMissionDocument *document, OpenNovaMissionBytes *out_bytes);
MISSION_EXPORT int opennova_mission_save_mis_path(OpenNovaMissionDocument *document, const char *path);
MISSION_EXPORT int opennova_mission_write_mis_text(OpenNovaMissionDocument *document, OpenNovaMissionBytes *out_bytes);
MISSION_EXPORT void opennova_mission_free_bytes(OpenNovaMissionBytes *bytes);
int opennova_mission_is_loaded(const OpenNovaMissionDocument *document);
const char *opennova_mission_source_path(const OpenNovaMissionDocument *document);
MISSION_EXPORT const char *opennova_mission_last_error(const OpenNovaMissionDocument *document);
int opennova_mission_get_info(const OpenNovaMissionDocument *document, OpenNovaMissionInfo *out_info);
size_t opennova_mission_entity_count(const OpenNovaMissionDocument *document, int kind);
int opennova_mission_get_entity(const OpenNovaMissionDocument *document,
                                               int kind,
                                               size_t index,
                                               OpenNovaMissionEntityRecord *out_record);
int opennova_mission_set_entity_transform(OpenNovaMissionDocument *document,
                                                         int kind,
                                                         size_t index,
                                                         const OpenNovaMissionEntityTransform *transform);
int opennova_mission_set_entity_properties(OpenNovaMissionDocument *document,
                                                          int kind,
                                                          size_t index,
                                                          const OpenNovaMissionEntityProperties *properties,
                                                          OpenNovaMissionEntityRecord *out_record);
int opennova_mission_add_entity(OpenNovaMissionDocument *document,
                                               int kind,
                                               int item_id,
                                               const OpenNovaMissionEntityTransform *transform,
                                               OpenNovaMissionEntityRecord *out_record);
int opennova_mission_remove_entity(OpenNovaMissionDocument *document, int kind, size_t index);
size_t opennova_mission_waypoint_summary_count(const OpenNovaMissionDocument *document);
int opennova_mission_get_waypoint_summary(const OpenNovaMissionDocument *document,
                                                         size_t index,
                                                         OpenNovaMissionWaypointSummary *out_summary);
size_t opennova_mission_waypoint_path_count(const OpenNovaMissionDocument *document);
int opennova_mission_get_waypoint_path(const OpenNovaMissionDocument *document,
                                                      size_t index,
                                                      OpenNovaMissionWaypointPath *out_path);
int opennova_mission_set_waypoint_path(OpenNovaMissionDocument *document,
                                                      size_t index,
                                                      const uint32_t *marker_indices,
                                                      size_t marker_count,
                                                      int flags,
                                                      OpenNovaMissionWaypointPath *out_path);
int opennova_mission_clear_waypoint_path(OpenNovaMissionDocument *document,
                                                        size_t index,
                                                        OpenNovaMissionWaypointPath *out_path);
int opennova_mission_add_waypoint_marker(OpenNovaMissionDocument *document,
                                                        size_t path_index,
                                                        int marker_item_id,
                                                        const OpenNovaMissionEntityTransform *transform,
                                                        int insert_index,
                                                        OpenNovaMissionEntityRecord *out_marker,
                                                        OpenNovaMissionWaypointPath *out_path);
size_t opennova_mission_area_trigger_count(const OpenNovaMissionDocument *document);
int opennova_mission_get_area_trigger(const OpenNovaMissionDocument *document,
                                                     size_t index,
                                                     OpenNovaMissionAreaTriggerRecord *out_record);
int opennova_mission_add_area_trigger(OpenNovaMissionDocument *document,
                                                     const OpenNovaMissionAreaTriggerRecord *record,
                                                     OpenNovaMissionAreaTriggerRecord *out_record);
int opennova_mission_set_area_trigger(OpenNovaMissionDocument *document,
                                                     size_t index,
                                                     const OpenNovaMissionAreaTriggerRecord *record,
                                                     OpenNovaMissionAreaTriggerRecord *out_record);
int opennova_mission_remove_area_trigger(OpenNovaMissionDocument *document, size_t index);
size_t opennova_mission_weapon_loadout_count(const OpenNovaMissionDocument *document);
int opennova_mission_get_weapon_loadout_entry(const OpenNovaMissionDocument *document,
                                                             size_t index,
                                                             OpenNovaMissionWeaponLoadoutEntry *out_entry);
int opennova_mission_set_weapon_loadout(OpenNovaMissionDocument *document,
                                                       const OpenNovaMissionWeaponLoadoutEntry *entries,
                                                       size_t count);
size_t opennova_mission_group_count(const OpenNovaMissionDocument *document);
int opennova_mission_get_group(const OpenNovaMissionDocument *document,
                                              size_t index,
                                              OpenNovaMissionGroupRecord *out_record);
int opennova_mission_set_group(OpenNovaMissionDocument *document,
                                              size_t index,
                                              int field0, int field8, int field12);
size_t opennova_mission_event_count(const OpenNovaMissionDocument *document);
int opennova_mission_get_event(const OpenNovaMissionDocument *document,
                                              size_t index,
                                              OpenNovaMissionEventRecord *out_record);
size_t opennova_mission_trigger_count(const OpenNovaMissionDocument *document);
int opennova_mission_get_trigger(const OpenNovaMissionDocument *document,
                                                size_t index,
                                                OpenNovaMissionTriggerRecord *out_record);
size_t opennova_mission_action_count(const OpenNovaMissionDocument *document);
int opennova_mission_get_action(const OpenNovaMissionDocument *document,
                                               size_t index,
                                               OpenNovaMissionActionRecord *out_record);
int opennova_mission_get_logic_summary(const OpenNovaMissionDocument *document,
                                                      OpenNovaMissionLogicSummary *out_summary);

} // extern "C"
