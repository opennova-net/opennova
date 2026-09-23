#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace opennova::aip {

// Vehicle AI profile properties. Parsing preserves the original type gate:
// HELO and GROUND accept their respective fields; ORGANIC accepts only type.
// [orig: AIProfile_ParseProperty @0x45DE70]

// One weapon block — profile+120 (primary) / +152 (secondary). The SM
// fire pump hands the block to the fire-transform solver, which reads
// +8 (cone), +16 (flags), +20 (facing) [orig: Entity_ComputeWeaponFireTransform_0
// @ 0x456980 reads weaponDef+8/+0x10/+0x14].
struct WeaponBlock {
    int32_t ammo = 0;        // +0: shot pool count  [orig: "primary_ammo" -> atol]
    int32_t rate_ticks = 0;  // +4: fire interval    [orig: "primary_rate" -> atof * 62.5, chop]
    int32_t cone_bam = 0;    // +8: aim cone half-angle [orig: "primary_fov" -> atof * 11930464.0 (deg->BAM32), chop]
    int32_t range = 0;       // +12: engage range 16.16 [orig: "primary_range" -> atol << 16]
    uint32_t flags = 0;      // +16: WEAPON_* mask (below) [orig: "primary_flags" token loop]
    int32_t facing_bam = 0;  // +20: yaw bias  [orig: "primary_facing" -> deg->BAM32]
    int32_t pitch_bam = 0;   // +24: pitch bias [orig: "primary_pitch" -> deg->BAM32]
    std::string weapon;      // +28 byte in retail: AmmoDef_LookupByName("primary_weap");
                             // the port keeps the authored name — the embedder resolves
                             // it against the loaded ammo table at the item-traits sweep.
};

// WEAPON_* flag names, verbatim from the parser's token compares
// [orig: AIProfile_ParseProperty primary_flags / secondary_flags loops, GROUND
//  @0x45E303 / @0x45E5B1, HELO @0x45F16D / @0x45F471]. The solver consumes them as:
// TURRET = track/stage the solution, SLOW/FAST = the two slew rates,
// PITCHLOCKED = commanded elevation (brain+0x314), PITCHLOCKED_MINUS45 =
// the fixed -45 deg elevation leg.
inline constexpr uint32_t kWeaponTurret = 0x1;
inline constexpr uint32_t kWeaponSlow = 0x2;
inline constexpr uint32_t kWeaponFast = 0x4;
inline constexpr uint32_t kWeaponPitchLocked = 0x8;
inline constexpr uint32_t kWeaponPitchLockedMinus45 = 0x10;

// COMBAT_FLAGS / EVADE_FLAGS names, verbatim [orig: the +100 / +96 token
// loops]: FOLLOW_WP 0x1, NO_ACTION 0x2, FLEE 0x4, NO_CAP 0x8, COUNTER 0x10,
// then COMBAT_FLAGS only: ATEAM 0x20, ATEAM_LOCK 0x40, RC_FIRE 0x80.
// (RC_FIRE is the stationary-fire mode the SM pump dispatches on.)

struct Profile {
    int32_t type = 0;            // +16: HELO 1 / GROUND 2 / ORGANIC 3 [orig: "type"]
	int32_t subtype = 0; // +20: STD 0 / BOAT 1 / PLANE 2 / TRAIN 3
	int32_t default_state = 0; // +24, literal AIState_LookupByName value
	int32_t drive_skill = 0; // +32, drive_skill / flight_skill, clamped 0..4
	int32_t alert = 0; // +36, GREEN/YELLOW/RED
	int32_t rank = 0; // +60
	int32_t check_six_rate = 0; // +108, atof * 655.36
	int32_t target_eval_rate = 0; // +112, atof * 655.36
	int32_t radio_distance = 0; // HELO +240 / GROUND +208
	int32_t radio_delay = 0; // HELO +244 / GROUND +212
	int32_t hunt_limit = 0; // HELO +196, seconds *62.5
	int32_t ground_patrol_speed = 0; // parsed fractional speed, 16.16 u/tick
	int32_t ground_combat_speed = 0;
	bool has_ground_patrol_speed = false;
	bool has_ground_combat_speed = false;
	int32_t aim_skill = 0; // +28: atol clamped 0..4 [orig: "aim_skill"]
	int32_t view_fov_bam = 0;    // +64: deg->BAM32 [orig: "view_fov"]
    int32_t view_dist = 0;       // +68: atol << 16 [orig: "view_dist"]
    int32_t radar_fov_bam = 0;   // +72: deg->BAM32 [orig: "radar_fov"]
    int32_t radar_dist = 0;      // +76: atol << 16 [orig: "radar_dist"]
    int32_t priority_air = 0;        // +80  [orig: "priority_air" -> atol]
    int32_t priority_ground = 0;     // +84  [orig: "priority_ground"]
    int32_t priority_organics = 0;   // +88  [orig: "priority_organics"]
    int32_t priority_decorations = 0;// +92  [orig: "priority_decorations"]
    uint32_t evade_flags = 0;    // +96  [orig: "EVADE_FLAGS" token loop]
    uint32_t combat_flags = 0;   // +100 [orig: "COMBAT_FLAGS" token loop]
    int32_t react_ticks = 0;     // +104: atof * 62.5, chop [orig: "react_time"]
    int32_t tether_dist = 0;     // +116: atol << 16 [orig: "tether_dist"]
	int32_t min_chase = 0; // +184: atof * 65536, chop [orig: "min_chase_dist"]
	int32_t max_chase = 0; // +188: atof * 65536, chop [orig: "max_chase_dist"]
	WeaponBlock primary; // +120..+151 (+148 weap byte)
	WeaponBlock secondary;       // +152..+183 (+180 weap byte)
    // The GROUND speed keys as raw authored integers, -1 when absent: the boot
    // resolver's parsed-anything probe. Retail stores only the parsed value at
    // +192/+196 (ground_patrol_speed / ground_combat_speed above).
    int32_t patrol_speed = -1;   // [orig: "patrol_speed"]
    int32_t combat_speed = -1;   // [orig: "combat_speed"]

	// --- the HELO (type 1) flight set [orig: the type-1 branch of
	// AIProfile_ParseProperty @0x45f684..0x45f9eb — profile offsets +200..+236
	// (+56 for use_waypoint_z)]. Unlike the GROUND speed pair above, these
	// hold retail's PARSED values: a speed is km/h -> 16.16 units per tick
	// (atof * 1000.0 * 4.444444444444444e-06 * 65536.0, i.e. x65536/225), a
	// climb is atof * 0.016 * 65536.0, an altitude is atol << 16, min_agl is
	// atof * 65536, and turn_rate/accel_time carry their integer formulas.
	// These parsed fields seed the aircraft navigation and combat brain. Every
	// unauthored field is the zeroed record's 0 (the loader memsets the record).
	int32_t helo_patrol_speed = 0; // +200 [orig: "patrol_speed" (HELO branch) km/h -> 16.16 u/tick]
	int32_t helo_patrol_altitude = 0;  // +204 [orig: "patrol_altitude" atol << 16]
    int32_t helo_patrol_climb = 0;     // +208 [orig: "patrol_climb" atof * 0.016 * 65536]
    int32_t helo_combat_speed = 0;     // +212 [orig: "combat_speed" (HELO branch) km/h -> 16.16 u/tick]
    int32_t helo_combat_altitude = 0;  // +216 [orig: "combat_altitude" atol << 16]
    int32_t helo_combat_climb = 0;     // +220 [orig: "combat_climb" atof * 0.016 * 65536]
    int32_t turn_rate_bam_tick = 0;    // +224 [orig: "turn_rate" = 11930464 * deg / 62]
    int32_t accel_ticks = 0;           // +228 [orig: "accel_time" = 62 * seconds]
	uint32_t hunt_flags = 0; // +192, MAINTAIN_SPEED bit1 [orig: @0x45DE70]
	int32_t use_waypoint_z = 0; // +56  [orig: "use_waypoint_z" atol]
	int32_t min_agl = 0;               // +232 [orig: "min_agl" atof -> 16.16]
    int32_t min_speed = 0;             // +236 [orig: "min_speed" km/h -> 16.16 u/tick]
};

// Line-oriented, whitespace-tokenized, case-insensitive property parser.
Profile parse_profile(const uint8_t *text, size_t size);

// The two words a class init copies into brain[49]/brain[50], read at the
// profile offsets that init hard-codes, whatever key the profile's own type
// stored there. The helicopter init reads +0xD4/+0xC8 (a HELO profile's
// combat/patrol speed, a GROUND profile's radio_delay/turn_rate); the vehicle
// init reads +0xC4/+0xC0 (a GROUND profile's combat/patrol speed, a HELO
// profile's hunt_limit/hunt_flags). Any other type stores nothing there: the
// zeroed record's 0.
// [orig: Entity_InitHelicopterAIFromDef @0x468597..0x4685A9;
//  Entity_InitVehicleAIFromDef @0x4688C1..0x4688D3; the AIProfile_ParseProperty
//  stores GROUND +0xC0 @0x45E70B, +0xC4 @0x45F677, +0xC8 @0x45E7C4, +0xD4
//  @0x45E871 and HELO +0xC0 @0x45F5F6, +0xC4 @0x45F677, +0xC8 @0x45F70D,
//  +0xD4 @0x45F7DD]
struct ClassSpeeds {
    int32_t speed_a = 0; // brain[49]
    int32_t speed_b = 0; // brain[50]
};
ClassSpeeds class_speed_words(const Profile &profile, bool helicopter_init);

}  // namespace opennova::aip
