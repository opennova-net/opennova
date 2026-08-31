// world::inspect — the engine's entity inspection API (ADR 0042 d5): the one
// place a tool (MCP, F3, GUT, ctest) reads entity facts. entity_directory is
// the joined discovery list the old GDScript DebugEntities.list computed (the
// registry rows x AI cards join, unpresented AI diagnostics appended);
// build_entity_card is the full per-entity debug card the deleted Dictionary
// getters (get_entity_debug / get_world_entity_debug) assembled. The Godot
// binding forwards these into typed records and converts to JSON only at the
// MCP boundary; a joiner's decoded replica section is NET level and lives in
// net/npruntime/client_replica_card.h.
#ifndef OPENNOVA_WORLD_INSPECT_H
#define OPENNOVA_WORLD_INSPECT_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <runtime/world/entity.h>
#include <runtime/world/geom.h>

namespace opennova::world {

class World;
class AiSystem;

namespace inspect {

// One seat as the debug cards carry it. The AI card's mount_target_seats and
// the world card's seats both project from this row.
struct SeatRow {
	int32_t index = -1;
	int32_t type = 0;        // SeatType
	int32_t retail_slot = 0; // fixed retail mountHandles slot
	int32_t bone_index = 0;
	int32_t pose_index = 0;
	std::string source_name;
	Vec3 local{};            // authored seat offset (mission space, Z-up)
	int32_t yaw_offset = 0;
	bool occupied = false;
};

// The registry/world half of a card — the field set the old
// get_world_entity_debug Dictionary carried, engine-space.
struct WorldDetail {
	int32_t net_id = 0;
	int32_t bms_id = 0;
	int32_t pool = -1;
	int32_t kind = -1;         // spawn-origin kind; -1 = synthetic/none
	int32_t source_index = -1; // index within its kind's list
	int32_t item_id = 0;
	std::string name;
	std::string item_name; // items.def display name (world.item_names); empty without a def
	int32_t team = -1;
	bool alive = false;
	bool hidden = false;
	int32_t health = 0;
	int32_t health_max = 0;
	bool has_item_def = false;
	int32_t handle = -1;       // packed pool/slot wire handle
	int32_t item_type = 0;
	int32_t item_unit_type = 0;
	int64_t item_attrib = 0;
	int64_t item_attrib2 = 0;
	int32_t vehicle_family = -1;
	bool has_minimap_model_marker = false;
	bool is_capture_trigger = false;
	bool is_spawn_point = false;
	int32_t zone_number = 0;
	int32_t zone_radius = 0;
	int32_t zone_control = 0;
	int32_t zone_chain_index = -1;
	Vec3 mission_position{};
	// The registry row's authored/promoted orientation, Entity::yaw/pitch/roll:
	// whole mission degrees in an int16 (every writer rounds
	// normalize_mission_yaw_deg), NOT the AI card's BAM32 heading.
	int32_t yaw = 0;
	int32_t pitch = 0;
	int32_t roll = 0;
	int32_t primary_weapon_clip = 0;
	int32_t primary_weapon_reserve = 0;
	std::vector<SeatRow> seats;
};

// The AI half of a card — the field set the old get_entity_debug Dictionary
// carried. The registry-derived block keeps TYPED DEFAULTS when the registry
// slot is despawned (a scripted VaporizeSingle/removeSSN leaves the AiEntity
// in the pool), so the card's shape is stable either way.
struct AiDetail {
	// --- registry-derived block (typed defaults when registry-despawned) ---
	int32_t kind = -1;
	int32_t source_index = -1;
	int32_t bms_id = 0;
	int32_t item_id = 0;
	std::string name;
	int32_t group_id = 0;
	int32_t pool = -1;
	int64_t engine_flags = 0;
	// The BMS/gameplay flags word is a SEPARATE store from engine_flags, and
	// the motor's suppression gates read the union (ladder zeroes the
	// horizontal root pair, drowning the vertical) — a card exposing only
	// engine_flags cannot explain a body that animates without translating.
	int64_t bms_flags = 0;
	int32_t waypoint_id = 0;
	int32_t wp_number = 0;
	int32_t health = 0;
	bool alive = false;
	bool hidden = false;
	bool held = false;
	bool disabled = false;
	int32_t vehicle_family = -1;
	int32_t body_anim_slot = -1;
	int32_t character_anim_slot = -1;
	int32_t minimap_net_id = 0;
	bool mounted = false;
	int32_t mount_target_net_id = 0;
	int32_t mount_seat = -1;
	int32_t mount_type = 0;
	bool mount_config_valid = false;
	int32_t mount_config = 0;
	int32_t mount_seat_bone = 0;
	int32_t mount_seat_pose_index = 0;
	std::string mount_seat_source_name;
	Vec3 mount_seat_local{};
	int32_t mount_seat_yaw_offset = 0;
	bool mount_target_config_valid = false;
	int32_t mount_target_config = 0;
	int32_t mount_target_seat_count = 0;
	std::vector<SeatRow> mount_target_seats;

	// --- AI scalars ---
	int32_t net_id = 0;
	int32_t wire_handle = 0;
	int32_t team = -1;
	// The AI-side entity+286 mirror; diverges from the registry health under
	// some damage paths, so the card shows both.
	int32_t ai_health = 0;
	Vec3 mission_position{}; // AI 16.16 mirror, world units
	double yaw_deg = 0.0;
	int32_t state = 0;
	std::string state_name;
	int32_t pending_state = 0;
	int32_t alert = 0;
	int32_t alert_brain = 0;
	int32_t wp_channel = 0;
	int32_t wp_node = 0;
	int32_t wp_distance = 0;
	int32_t out_speed = 0;

	// --- vehicle/motor diagnostics (emitted only for a live registry row,
	//     matching the old card's key set) ---
	bool has_vehicle_block = false;
	int32_t rotor_speed = 0;
	int32_t rotor_phase = 0;
	int32_t rotor_rate = 0;
	int32_t profile_type = 0;
	bool primary_occupant = false;
	int32_t veh_family = -1;
	bool player_control = false;
	int32_t vp = 0; // entity pitch (BAM32)
	int32_t vr = 0; // entity roll (BAM32)
	int32_t mspd = 0;
	int32_t wc[4] = {};
	int32_t pd[4] = {};
	int32_t macc = 0;
	bool mgnd = false;
	int32_t cmd_fwd = 0;
	int32_t cmd_lat = 0;
	int32_t alt_tgt = 0;
	bool engine_on = false;
	int32_t pilot_move = -1;

	// --- infantry motor / combat diagnostics ---
	bool infantry = false;
	int32_t adm_id = -1;
	std::string adm_name;
	int32_t infantry_move_mode = 0;
	int32_t root_dx = 0;
	int32_t root_dy = 0;
	int32_t res_dx = 0;
	int32_t res_dy = 0;
	int32_t contact_item = 0;
	int32_t parent = -1; // carrier bms_id
	int32_t ground = -1; // ground-entity bms_id
	int32_t s35 = 0;
	int32_t s37 = 0;
	int32_t s38 = 0;
	int32_t fires_aimed = 0;
	int32_t fires_body = 0;
	int32_t ground_cache = 0;
	bool ground_valid = false;
	bool airborne = false;
	int32_t anim_state = -1;
	std::string anim_key;
	double sight_range_u = 0.0;
	double attack_range_u = 0.0;
	int32_t ammo_primary = 0;
	int32_t clip_size = 0;
	int32_t magazine = 0;
	bool combat_target_valid = false;
	bool muzzle_valid = false;
	Vec3 muzzle{}; // mission space, world units
	int32_t death_anim_state = 0;
	int32_t corpse_timer = 0;
	int32_t deathtime_ticks = 0;
	bool leave_corpse = false;
};

// The full per-entity debug card: the world/registry half, the AI half, or
// both (valid = at least one resolved).
struct EntityCard {
	bool valid = false;
	uint16_t handle = EntityHandle::kInvalid;
	int32_t ai_index = -1;
	bool has_world = false;
	WorldDetail world;
	bool has_ai = false;
	AiDetail ai;
};

// One discovery row of the entity directory.
struct EntityRow {
	int32_t index = -1;    // position in the directory
	int32_t ai_index = -1; // AI pool index; -1 = no brain
	bool editable = false; // has an AI brain AND a live registry slot
	bool presented = false;
	bool registry_present = false;
	int32_t kind = -1;
	int32_t source_index = -1;
	int32_t bms_id = 0;
	int32_t net_id = 0;
	int32_t item_id = 0; // the wire type id
	uint16_t wire_handle = 0;
	std::string name;
	std::string item_name; // items.def display name (world.item_names); empty without a def
	std::string state_name;
	int32_t health = 0;
	int32_t team = -1;
	bool alive = false;
	bool hidden = false;
	Vec3 mission_position{};
};

// The entity discovery join: one row per live registry slot carrying a wire
// type (the set the client present streams), each joined to its AI-pool card
// by handle; AI entities missing from that set (registry-despawned, or a row
// without a def) are appended as diagnostics. editable = has an AI brain AND
// a live registry slot — the edit seams key on the ai_index.
std::vector<EntityRow> entity_directory(const World &world, const AiSystem *ai);

// The full card for one entity by wire handle. `adm_name_resolver` maps an
// infantry adm_id to its display name (the embedder's AdmRootMotion owns the
// table; a bare World has none) — null leaves adm_name empty. Non-const World
// because the muzzle readback resolves through the live pose provider.
EntityCard build_entity_card(World &world, const AiSystem *ai, EntityHandle handle,
		const std::function<std::string(int32_t)> &adm_name_resolver = {});

} // namespace inspect
} // namespace opennova::world

#endif // OPENNOVA_WORLD_INSPECT_H
