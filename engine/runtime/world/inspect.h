// world::inspect — the engine's entity inspection API (ADR 0042 d5): the one
// place a tool (MCP, F3, GUT, ctest) reads entity facts. entity_directory is
// the joined discovery list the old GDScript DebugEntities.list computed (the
// registry rows x AI cards join, unpresented AI diagnostics appended);
// build_entity_card is the full per-entity debug card the deleted Dictionary
// getters (get_entity_debug / get_world_entity_debug) assembled. The Godot
// binding forwards these into typed records and converts to JSON only at the
// MCP boundary; a joiner's decoded replica section is NET level and lives in
// runtime/inmatch/client_replica_card.h.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <runtime/world/entity.h>
#include <runtime/world/mission_diagnostics.h>
#include <runtime/world/geom.h>

namespace opennova::world {

class World;

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

// One seat as a row: `index` is its slot in the owning table (-1 for a
// def-level seat spec that has no live table yet).
SeatRow seat_row(const Seat &seat, int32_t index);

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
	std::string item_name; // items.def display name (world.tables.item_names); empty without a def
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

	// --- brain combat/movement registers (the F3 AI window's deep pane; raw
	//     brain dwords named in ai.h AiBrain::Idx, angles left as BAM32) ---
	bool target_valid = false;       // brain kTargetSlot != 0 (packed+1 rebase)
	int32_t target_handle = -1;      // rebased packed wire handle (-1 = null)
	std::string target_name;         // registry name of the target, when live
	bool priority_target_valid = false; // brain kPriorityTarget != 0
	int32_t priority_target_handle = -1;
	int32_t combat_timer = 0;        // kCombatTimer (>620 re-acquire)
	int32_t fire_delay = 0;          // kFireDelay countdown
	int32_t retarget_timer = 0;      // kRetargetTimer (>248 rescans)
	int32_t cooldown_a = 0;          // kCooldownPair low u16 (weapon A)
	int32_t cooldown_b = 0;          // kCooldownPair high u16 (weapon B)
	int32_t speed_a = 0;             // kSpeedA
	int32_t speed_b = 0;             // kSpeedB (state 16 GROUND_FOLLOWWP)
	Vec3 work_pos{};                 // kWorkPos* — the mover's goal, world units
	int32_t work_heading = 0;        // kWorkHeading (BAM32)
	int32_t turret_yaw = 0;          // kActiveYaw — slewed live turret yaw (BAM32)
	int32_t turret_pitch = 0;        // kActivePitch (BAM32)

	// --- profile summary (AiProfile, the read-only .aip definition) ---
	int32_t profile_class_priority[4] = {}; // air/ground/organics/decorations
	int32_t profile_fov_primary = 0;        // arc byte (pre OR-1)
	int32_t profile_fov_secondary = 0;
	int32_t profile_range_primary = 0;      // engage-range caps (world units)
	int32_t profile_range_secondary = 0;
	int32_t profile_approach_cap = 0;       // chase range cap (16.16)

	// --- slot control word (AiSlot f[1]; the gates infantry_combat reads:
	//     0x1 blind/no-scan, 0x8 teamless default, 0x200 berserk) ---
	int32_t slot_control_bits = 0;

	// --- vehicle/motor diagnostics (emitted only for a live registry row,
	//     matching the old card's key set; profile_type is filled for EVERY
	//     brain — the HELO/GROUND/ORGANIC class is not a vehicle fact) ---
	bool has_vehicle_block = false;
	int32_t rotor_speed = 0;
	int32_t rotor_phase = 0;
	int32_t rotor_rate = 0;
	int32_t profile_type = 0;
	bool primary_occupant = false;
	int32_t veh_family = -1;
	bool player_control = false;
	int32_t vp = 0; // legacy rounded entity pitch (degrees)
	int32_t vr = 0; // legacy rounded entity roll (degrees)
	int32_t mspd = 0;
	int32_t wc[6] = {}; // four corners plus the tank's two middle channels
	int32_t pose_bam[3] = {}; // motor yaw/pitch/roll, full precision
	int32_t sink[4] = {};
	int32_t spring_amplitude[4] = {};
	int32_t spring_energy[4] = {};
	int32_t spring_impulse[4] = {};
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
	bool airborne = false;
	int32_t anim_state = -1;
	std::string anim_key;
	double sight_range_u = 0.0;
	double attack_range_u = 0.0;
	int32_t ammo_primary = 0;
	int32_t clip_size = 0;
	int32_t magazine = 0;
	bool combat_target_valid = false;
	// The infantry combat pass's live aim/reaction state (InfantryState §17;
	// infantry-only — the SM/vehicle chain's timers are the brain block above).
	int32_t aim_heading = 0;      // the aim solution (BAM32)
	int32_t aim_pitch = 0;        // (BAM32)
	bool aim_valid = false;
	int32_t damage_timer = 0;     // alert countdown (+12 on sight)
	int32_t same_target_ticks = 0;
	int32_t combat_move_timer = 0;
	bool muzzle_valid = false;
	Vec3 muzzle{}; // mission space, world units
	int32_t death_anim_state = 0;
	int32_t corpse_timer = 0;
	int32_t deathtime_ticks = 0;
	bool leave_corpse = false;
};

// The full per-entity debug card: the world/registry half, the AI half, or
// both (valid = at least one resolved).
// Facial state is independent of whether the entity has an AI brain.
struct FacialDetail {
    bool available = false;
    int32_t current = 0, next = 0, expression_override = -1, automatic = -1;
    int32_t override_timer = 0, texture_priority = -1;
    float blend = 0.0f;
    uint32_t display_frame = 0;
};

struct EntityCard {
	bool valid = false;
	uint16_t handle = EntityHandle::kInvalid;
	int32_t ai_index = -1;
	bool has_world = false;
	WorldDetail world;
	bool has_ai = false;
	AiDetail ai;
    FacialDetail facial;
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
	std::string item_name; // items.def display name (world.tables.item_names); empty without a def
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
// a live registry slot — the edit seams key on the ai_index. with_brains =
// false leaves the AI pool out (a joiner's non-authoritative tooling pool
// never joins the decoded view).
std::vector<EntityRow> entity_directory(const World &world, bool with_brains);

// The full card for one entity by wire handle. `adm_name_resolver` maps an
// infantry adm_id to its display name (the embedder's AdmRootMotion owns the
// table; a bare World has none) — null leaves adm_name empty. Non-const World
// because the muzzle readback resolves through the live pose provider.
EntityCard build_entity_card(World &world, bool with_brains, EntityHandle handle,
		const std::function<std::string(int32_t)> &adm_name_resolver = {});

// --- The AI debug join (the F3 AI window's pushed record + the Godot AI
// overlay's per-frame payload). One walk over the AI pool; positions stay
// 16.16 mission fixed so the binding converts once with its own axis map. ---

// One brain's overlay facts: enough to draw a state label, its route join,
// the target/aim lines, and the perception rings — not the deep card.
struct AiOverlayRow {
	int32_t ai_index = -1;
	uint16_t handle = EntityHandle::kInvalid;
	std::string name;
	int32_t group_id = 0;
	bool alive = false;
	bool infantry = false; // inf.active — move_mode/aim/damage_timer apply
	int32_t pos[3] = {};   // 16.16 mission fixed
	int32_t state = 0;
	std::string state_name;
	int32_t alert = 0; // AiSlot byte 136 (authoritative; see fill_ai_detail)
	int32_t move_mode = 0;
	int32_t out_speed = 0;
	int32_t wp_channel = 0;
	int32_t wp_node = 0;
	int32_t wp_distance = 0;
	bool target_valid = false;
	uint16_t target_handle = EntityHandle::kInvalid;
	int32_t target_pos[3] = {}; // 16.16; valid only with target_valid
	std::string target_name;
	bool aim_valid = false;
	int32_t aim_heading = 0; // BAM32
	int32_t aim_pitch = 0;   // BAM32
	bool muzzle_valid = false;   // resolved only for engaged brains (cost)
	int32_t muzzle[3] = {};      // 16.16
	int32_t sight_range_q16 = 0;  // AiSlot kSightRange
	int32_t attack_range_q16 = 0; // AiSlot kAttackRange
	int32_t combat_timer = 0;
	int32_t fire_delay = 0;
	int32_t damage_timer = 0;      // infantry only
	int32_t combat_move_timer = 0; // infantry only
};

// One resolved nav node of a channel (NavEntry through the pool-3 indices).
struct AiNavNodeRow {
	int32_t pos[3] = {};    // 16.16
	int32_t radius_q16 = 0; // arrival radius
	int32_t wait_ticks = 0;
};

// One nav channel with its resolved node run and the count of brains whose
// waypoint sub-struct currently walks it (kWpChannel join).
struct AiNavChannelRow {
	int32_t index = 0;
	int32_t loopflag = 0; // bit0 = one-shot (terminate at path end)
	std::vector<AiNavNodeRow> nodes;
	int32_t followers = 0;
};

// One TriggerRelations group record (only groups with members at mission
// start are emitted).
struct AiGroupRow {
	int32_t id = 0;
	int32_t alert = 0; // TriggerRelations::Alert
	int32_t initial_count = 0;
	int32_t live_count = 0;
};

// System-level AI counters worth one line in the window.
struct AiSystemCounters {
	int32_t brain_count = 0;
	int32_t event_count = 0;
    uint64_t runtime_gap_calls = 0;
    uint32_t runtime_gap_sites = 0;
	int32_t rel_ops = 0;
	int32_t find_target_calls = 0;
};

struct AiDebugReport {
	// Row cap: a debug overlay never needs more than the AI pool holds, and a
	// runaway pool must not turn the per-frame payload into a hitch.
	static constexpr int kMaxRows = 256;
	std::vector<AiOverlayRow> rows;
	std::vector<AiNavChannelRow> channels;
	std::vector<AiGroupRow> groups;
	AiSystemCounters counters;
};

// The AI-pool walk behind both debug surfaces. Non-const World: the muzzle
// readback resolves through the live pose provider (build_entity_card's
// precedent).
AiDebugReport ai_debug_report(World &world);

} // namespace inspect
} // namespace opennova::world
