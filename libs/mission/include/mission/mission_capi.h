#pragma once

// The flat C ABI over MissionDocument: the surface the Python FFI (ctypes) and the
// DCC tooling call. Lifted out of mission/mission.h (quality campaign W3-1) so the
// C++ facade header is C++ only — every other consumer of mission.h wants the
// document class, not this. Implemented in libs/mission/src/mission_capi.cpp.

#include <stddef.h>
#include <stdint.h>

#include <io/export.h>
// MISSION_EXPORT marks the flat C ABI (the 8 document-lifecycle functions,
// annotated at declaration AND definition; scripts/lint/abi_exports_baseline.txt
// pins the set on every platform). The unannotated extern "C" authoring
// functions below are C++-static-link surface only (Model B) — annotating a
// declaration whose definition is plain exports it on ELF/Mach-O but not PE,
// which is exactly the drift abi_export_identity exists to catch.
#define MISSION_EXPORT OPENNOVA_API

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

// Loadout entry over FFI — the kit tuple {name, ammoPri, ammoSec, flags} (net-re §5.63). The four
// on-disk strings are copied into fixed 64-char buffers (real weapon names + numeric values are
// short); a longer field is truncated to 63 chars on read. flags (the per-ammo damage-class request
// byte) sits last so the offsets of name/ammo_primary/ammo_secondary remain stable for rebuilt callers.
typedef struct OpenNovaMissionWeaponLoadoutEntry {
	char name[64];
	char ammo_primary[64];
	char ammo_secondary[64];
	char flags[64];
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
