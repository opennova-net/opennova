// Basic geometry + fixed-point helpers for the OpenNova runtime world.
//
// The original engine stores mission-space coordinates and many WAC/BMS scalar
// parameters as 16.16 signed fixed-point (e.g. bms::Entity::get_x() = x / 65536).
// The portable world keeps a clean float model; conversion helpers live here so
// front-ends (WAC operand resolution, BMS param decode) share one definition.
#ifndef OPENNOVA_WORLD_GEOM_H
#define OPENNOVA_WORLD_GEOM_H

#include <cstdint>

namespace opennova::world {

// 16.16 fixed-point <-> float/int. Matches WacScript_ResolveParameter scaling and
// bms.h's 1/65536 convention.
inline constexpr int32_t to_fixed(double v) { return static_cast<int32_t>(v * 65536.0); }
inline constexpr double from_fixed(int32_t v) { return static_cast<double>(v) / 65536.0; }

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

// Axis-aligned bounding box in mission space (Z-up, the original's frame).
// Models bms::AreaTrigger; used for area/zone membership predicates.
struct Aabb {
    Vec3 min;
    Vec3 max;

    bool contains(const Vec3 &p) const {
        return p.x >= min.x && p.x <= max.x &&
               p.y >= min.y && p.y <= max.y &&
               p.z >= min.z && p.z <= max.z;
    }
};

} // namespace opennova::world

#endif // OPENNOVA_WORLD_GEOM_H
