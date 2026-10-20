#pragma once

#include <cstdint>
#include <cmath>

namespace opennova::world {

// Retail code uses 32-bit binary angular measure for entity yaw/pitch: one full turn is
// 2^32 units. Use the full-turn scale here so exact quadrants round-trip cleanly;
// fixed-integer paths that were IDA-pinned to 11930464 stay in their own modules.
constexpr double kBamFullTurn = 4294967296.0;

// A BMS angle in whole degrees -> the spawned entity's BAM: `deg << 16` in a 32-bit
// register, a truncating signed divide by 360, then shifted into the high half, so
// the low 16 bits are always zero (yaw 0 spawns its heading at 0x40000000, where
// 90 x 11930464 would give 0x3FFFFFC0). The heading passes 90 - yaw; pitch and roll
// pass their degrees. Every entity angle that is still its placement carries it.
// [orig: Entity_SpawnFromBMSRecord — heading @0x40EB42..0x40EB66, pitch/roll
//  @0x40EB69..0x40EBA6, the 0B60B60B7h magic divide]
inline int32_t spawn_angle_bam(int32_t deg) {
    const int32_t turn16 = static_cast<int32_t>(static_cast<uint32_t>(deg) << 16) / 360;
    return static_cast<int32_t>(static_cast<uint32_t>(turn16) << 16);
}
// A placed row's Yaw word (Entity::heading) from its whole-degree mission yaw:
// the spawn angle of 90 - yaw (D-NET-NEW-L6-2).
// [orig: Entity_SpawnFromBMSRecord @0x40EB42..0x40EB66]
inline int32_t placed_heading_bam(int32_t yaw_deg) {
    return spawn_angle_bam(90 - yaw_deg);
}
constexpr double kBamPerDegree = kBamFullTurn / 360.0;
constexpr double kDegreesPerBam = 360.0 / kBamFullTurn;

// A BAM as the whole degrees of an Entity's int16 pitch / roll mirror, rounded
// half away from zero: (bam * 360) / 2^32 in a double, both steps exact. Ours
// alone (a presentation mirror, not a retail immediate).
inline int16_t degrees_i16_from_bam(int32_t bam) {
    return static_cast<int16_t>(std::lround(double(bam) * 360.0 / kBamFullTurn));
}

inline double normalize_mission_yaw_deg(double degrees) {
    double out = std::fmod(degrees, 360.0);
    if (out < 0.0) out += 360.0;
    return out;
}

inline int32_t bam_from_degrees_wrapped(double degrees) {
    const double normalized = normalize_mission_yaw_deg(degrees);
    const uint32_t raw = static_cast<uint32_t>(static_cast<uint64_t>(normalized * kBamPerDegree));
    return static_cast<int32_t>(raw);
}

inline int32_t bam_heading_from_mission_yaw_deg(double yaw_deg) {
    return bam_from_degrees_wrapped(90.0 - yaw_deg);
}

inline double mission_yaw_deg_from_bam_heading(int32_t heading_bam) {
    return normalize_mission_yaw_deg(90.0 - static_cast<double>(heading_bam) * kDegreesPerBam);
}

// A Yaw word as whole mission degrees in [0, 360), rounded: the display value
// of the tooling cards (ours alone, not a retail store).
inline int32_t mission_yaw_whole_deg(int32_t heading_bam) {
    return static_cast<int32_t>(std::lround(mission_yaw_deg_from_bam_heading(heading_bam)) % 360);
}

} // namespace opennova::world
