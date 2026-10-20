// Basic geometry + fixed-point helpers for the OpenNova runtime world.
//
// The original engine stores mission-space coordinates and many WAC/BMS scalar
// parameters as 16.16 signed fixed-point (e.g. bms::Entity::get_x() = x / 65536).
// An entity's position keeps those words (EntityPosition); the conversion helpers
// live here so front-ends (WAC operand resolution, BMS param decode) share one
// definition.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include <base/io/fixed.h>

namespace opennova::world {

struct FixedVec3 {
    int32_t x = 0, y = 0, z = 0;

    constexpr int32_t operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
};

// A 16.16 vector's length, as the projectile code takes it: the double sqrt,
// capped immediately below the signed ftol overflow boundary.
inline int32_t fixed_magnitude(const FixedVec3 &v) {
    const double magnitude = std::sqrt(static_cast<double>(v.x) * v.x +
                                       static_cast<double>(v.y) * v.y +
                                       static_cast<double>(v.z) * v.z);
    constexpr double kFtolLimit = 2147418112.0; // float bits 0x4EFFFE00
    return static_cast<int32_t>(std::min(magnitude, kFtolLimit));
}

// 16.16 fixed-point <-> float/int. Matches WacScript_ResolveParameter scaling and
// bms.h's 1/65536 convention.
inline constexpr int32_t to_fixed(double v) { return static_cast<int32_t>(v * 65536.0); }
inline constexpr double from_fixed(int32_t v) { return static_cast<double>(v) / 65536.0; }

// The mission coordinate domain in world units: positions ride signed 16.16
// int32 carriers, so every axis spans [-32768.0, 32768.0 - 1/65536]. Edit/debug
// surfaces clamp entry fields to this domain.
inline constexpr double kMissionCoordMinUnits = from_fixed(INT32_MIN);
inline constexpr double kMissionCoordMaxUnits = from_fixed(INT32_MAX);

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

// One axis of an entity's mission-space position: retail's int32 16.16 word
// (entity+4 / +8 / +0xC), the only state the axis keeps (D-NET-507). Retail
// stores and copies these words raw: the BMS spawn
// [orig: Entity_SpawnFromBMSRecord @0x40EB30..0x40EB3F], the AI slot
// [orig: Entity_InitFromModel @0x40E113..0x40E127], a placement
// [orig: Server_PositionPlayerForSpawn @0x50D3DB..0x50D403] and every load
// serializer; `q16` is that word. The float conversion is the accessor for the
// consumers that compute in float, and a float store truncates through
// to_fixed, the conversion the wire builders applied to the float position and
// retail's own float-to-int rule [orig: _ftol2_sse @0x76BC00 - cvttsd2si @0x76BC15].
struct PositionWord {
    int32_t q16 = 0;

    operator float() const { return io::fp16_16_to_float(q16); }
    PositionWord &operator=(float units) {
        q16 = to_fixed(units);
        return *this;
    }
    PositionWord &operator+=(float units) { return *this = float(*this) + units; }
    PositionWord &operator-=(float units) { return *this = float(*this) - units; }
};

// An entity's position (and its spawn backup), three PositionWords. The float
// view converts per read; a Vec3 store truncates each axis; q16()/set_q16()
// move the words exactly. Not an aggregate, so a braced store resolves to the
// Vec3 overload alone.
struct EntityPosition {
    PositionWord x, y, z;

    EntityPosition() {}
    operator Vec3() const { return {float(x), float(y), float(z)}; }
    EntityPosition &operator=(const Vec3 &v) {
        x = v.x;
        y = v.y;
        z = v.z;
        return *this;
    }
    FixedVec3 q16() const { return {x.q16, y.q16, z.q16}; }
    // Axis i's word (0 x, 1 y, 2 z), the indexed form of x.q16 / y.q16 / z.q16.
    int32_t &word(int i) { return i == 0 ? x.q16 : (i == 1 ? y.q16 : z.q16); }
    int32_t word(int i) const { return i == 0 ? x.q16 : (i == 1 ? y.q16 : z.q16); }
    // The three words as one array: the form retail hands a callee as
    // &entity->Position (a const int32_t[3] parameter).
    std::array<int32_t, 3> words() const { return {x.q16, y.q16, z.q16}; }
    void set_q16(const FixedVec3 &w) {
        x.q16 = w.x;
        y.q16 = w.y;
        z.q16 = w.z;
    }
    static EntityPosition of_q16(const FixedVec3 &w) {
        EntityPosition p;
        p.set_q16(w);
        return p;
    }
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
