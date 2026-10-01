#pragma once

#include <stddef.h>
#include <stdint.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <formats/mission/bms.h>

namespace opennova::mission {

constexpr int kItemIdOffset = 100000;
constexpr size_t kMaxWaypointPathMarkers = 32;

enum class EntityKind : int {
	Marker = 0,
	Item = 1,
	Building = 2,
	Organic = 3,
};

// items.def TYPE resolver for the .mis text reader: maps an items.def id
// (bms type_id + kItemIdOffset) to its items.def TYPE (the DefItemType value —
// the domain of authoring::entity_kind_for_item_type), or any negative value
// when the id is not in the loaded item table. The mission format lib stays
// def-free, so the EMBEDDER builds this over its own parsed items.def
// (engine/formats/def). An EMPTY resolver means no items.def is loaded: every
// parsed `begin item` record then lands in the generic item pool — the pool
// kind is unknowable without the item table, and each call site states that
// explicitly by passing {}.
using MisItemTypeResolver = std::function<int(int def_item_id)>;

struct MissionInfo {
	std::string mission_name;
	std::string designer;
	std::string briefing;
	std::string terrain;
	std::string environment;
	// The tile-set name as authored (header +0x118, extension kept): a
	// non-empty value overrides the .trn tilestrip atlas and .TSD name
	// [orig: g_BmsTileSetName @ 0xA762E8, read by Terrain_LoadEnvironmentConfig
	// @ 0x6109C8; formats/trn trn_mission_tilestrip].
	std::string tile_set;
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

// (The entity itself is bms::Entity — no record twin, ADR 0043 slice E11:
// bms_edit.h reads it through entity_item_id / entity_transform /
// entity_name1/2 and edits it per named property. The editable byte
// meanings: max_simultaneous = no_more_than (byte 74, paired with the
// RemoveIfMoreThan AI flag), no_less_than (byte 75, RemoveIfLessThan),
// map_symbol (byte 81, the tactical-map icon), name1 = bytes 104..111
// (iai_name, the AI class), name2 = bytes 112..119 (ai_textfile, the AI
// script).)

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
// survive every document round-trip. New rows default it to "-1". has_flags: whether the
// record writes its fourth string (bms::WeaponLoadoutRecord::has_flags; false: three strings,
// flags the "-1" the game's sanitizer inserts).
struct WeaponLoadoutEntry {
	std::string name;
	std::string ammo_primary;
	std::string ammo_secondary;
	std::string flags = "-1";
	bool has_flags = true;
};

// Typed view of a 32-byte group record. The field WIDTHS are witnessed
// ([orig: dfx2med.exe Med_WriteBmsFile @0x44f920]) but their in-engine MEANING is still
// ungrilled (the open question is docs/mission/bms-event-runtime-re.md §3a, the
// group record's runtime reads), so the names stay offset
// placeholders: field0 = 2-bit flags, field8 = the one free int,
// field12 = writer-confirmed constant 10.
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

// The document's edit operations and the readers over these views are the
// free functions of bms_edit.h (ADR 0043 slice E11); the .mis text form is
// mission_mis.h.

} // namespace opennova::mission
