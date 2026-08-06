#ifndef OPENNOVA_WORLD_ANGLE_H
#define OPENNOVA_WORLD_ANGLE_H

#include <cstdint>
#include <cmath>

namespace opennova::world {

// Retail code uses 32-bit binary angular measure for entity yaw/pitch: one full turn is
// 2^32 units. Use the full-turn scale here so exact quadrants round-trip cleanly;
// fixed-integer spawn/promotion paths that were IDA-pinned to 11930464 stay in their own modules.
constexpr double kBamFullTurn = 4294967296.0;
constexpr double kBamPerDegree = kBamFullTurn / 360.0;
constexpr double kDegreesPerBam = 360.0 / kBamFullTurn;

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

} // namespace opennova::world

#endif // OPENNOVA_WORLD_ANGLE_H
