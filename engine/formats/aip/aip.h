#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace opennova::aip {

// The .aip AI-profile text format — PARTIAL PORT, and honest about it: the
// GROUND-type property set below is witnessed in full plus the two shared
// speed keys; the HELO-type key set remains unported and ORGANIC profiles
// parse nothing beyond `type` in retail itself ([orig: AIProfile_ParseProperty
// @ 0x45de70 — the type gate at +16 returns for type 3 before any key
// dispatch]). Newly witnessed fields land HERE, not in a runtime resolver
// (ADR 0030). -1 / 0 sentinels mark absent fields per-member below.

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
// [orig: AIProfile_ParseProperty @ 0x45e0xx primary_flags /
//  secondary_flags loops]. The solver consumes them as:
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
    int32_t aim_skill = -1;      // +28: atol clamped 0..4 [orig: "aim_skill"]
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
    int32_t react_ticks = -1;    // +104: atof * 62.5, chop [orig: "react_time"]
    int32_t tether_dist = 0;     // +116: atol << 16 [orig: "tether_dist"]
    int32_t min_chase = 0;       // +188: atof * 65536, chop [orig: "min_chase_dist"]
    int32_t max_chase = 0;       // +184: atof * 65536, chop [orig: "max_chase_dist"]
    WeaponBlock primary;         // +120..+151 (+148 weap byte)
    WeaponBlock secondary;       // +152..+183 (+180 weap byte)
    // Raw authored speed values (the brain seed applies the x65536/225 scale,
    // matching the pre-extension ProfileSpeeds contract).
    int32_t patrol_speed = -1;   // +192 [orig: "patrol_speed"]
    int32_t combat_speed = -1;   // +196 [orig: "combat_speed"]
};

// Parse the witnessed keys from .aip text: line-oriented, whitespace
// tokenized, keys compared case-insensitively. The property dispatch is
// TYPE-GATED exactly like retail: a `type` line switches the active set,
// GROUND (2) accepts the set above, ORGANIC (3) accepts nothing, and the
// HELO (1) set is unported (its keys are ignored — a tracked gap, not a
// design choice). [orig: AIProfile_ParseProperty @ 0x45de70]
Profile parse_profile(const uint8_t *text, size_t size);

}  // namespace opennova::aip
