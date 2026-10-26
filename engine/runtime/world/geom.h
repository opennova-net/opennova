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

#include <base/io/bam.h>
#include <base/io/fixed.h>

namespace opennova::world {

struct FixedVec3 {
    int32_t x = 0, y = 0, z = 0;

    constexpr int32_t operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
};

// The 16.16 vector products the solves share.
// The cross product as the game's routine forms it: each term a round-half-up product BEFORE the
// subtraction (shifting the difference instead is off by one LSB on about half the inputs). The
// wide form keeps each difference's 64 bits for a caller that normalises the raw vector; the
// narrow form stores each difference's low dword, as the routine does.
// [orig: Math_FixedPointCrossProduct @0x6134A0 - `imul; add 8000h; adc; shrd 10h` per term
//  @0x6134f7 / @0x61353f / @0x613588]
inline void fixed_cross_wide(const int32_t a[3], const int32_t b[3], int64_t r[3]) {
    r[0] = int64_t(io::mul_q16_rhu(a[1], b[2])) - io::mul_q16_rhu(a[2], b[1]);
    r[1] = int64_t(io::mul_q16_rhu(a[2], b[0])) - io::mul_q16_rhu(a[0], b[2]);
    r[2] = int64_t(io::mul_q16_rhu(a[0], b[1])) - io::mul_q16_rhu(a[1], b[0]);
}
inline void fixed_cross(const int32_t a[3], const int32_t b[3], int32_t out[3]) {
    int64_t raw[3];
    fixed_cross_wide(a, b, raw);
    for (int i = 0; i < 3; ++i) out[i] = int32_t(uint32_t(uint64_t(raw[i])));
}
// The dot product as the motors sum it: each term a round-half-up product, the sum wrapping.
inline int32_t fixed_dot(const int32_t a[3], const int32_t b[3]) {
    return io::bam_add(io::bam_add(io::mul_q16_rhu(a[0], b[0]), io::mul_q16_rhu(a[1], b[1])),
                       io::mul_q16_rhu(a[2], b[2]));
}
// The witnessed solver normalize, over an int32 or a wide 64-bit vector: the length of the three
// squares' sum, compared with 0 (a zero length gives (0,0,0)); else k = 65536.0f over the length
// ONCE, then each component times k, ftol'd (truncated) back to a 16.16 int. The divide comes first: two roundings, so an axis vector
// whose length k does not invert exactly reads 65535 (a length of 425 or 131019 does at 24, 53
// and 64 bits alike), where `v * 65536 / length` would read 65536.
// [orig: the pattern at every edge/normal/direction site, e.g. Entity_ComputeSuspensionOrientation
//  @0x46C8E0 (@0x46C9BA..0x46CA14, `fdivrp st(3), st` @0x46C9C1);
//  Vehicle_ComputeOrientationFrom4Wheels @0x459310
//  (`fdivr st, st(5)` @0x4593A6 over flt_7C32BC); Entity_ApplyBoneAttachmentTransform @0x45A750
//  (`fdivrp st(3), st` @0x45A8A8, `fdivr st, st(4)` @0x45A910);
//  Entity_RebuildOrientationMatrixFromAxes @0x4632E0; Entity_BuildOrientationFromVectors @0x458DF0
//  (`fdivr ds:flt_7C32BC` @0x458F34); Entity_ProcessTrackedVehiclePhysics @0x47C1C0 (`fdivrp
//  st(3), st` @0x47C68A); Entity_ProcessLightVehiclePhysics @0x479600 (the downhill @0x47C13D);
//  Entity_ProcessWheeledVehiclePhysics @0x475DE0 (@0x477B34; the downhill @0x479566);
//  Entity_UpdateTankVehiclePhysics @0x488AB0 (@0x48A6FA); Entity_UpdateLightVehiclePhysics
//  @0x483FE0 (@0x4862A0, @0x48642A, @0x486484); Terrain_CalcSectorRepulsionForce @0x5CBF50
//  (@0x5CC13D)]
template <typename T>
inline void fixed_normalize(const T v[3], int32_t out[3]) {
    const double n = std::sqrt(double(v[0]) * double(v[0]) + double(v[1]) * double(v[1]) +
                               double(v[2]) * double(v[2]));
    if (n <= 0.0) {
        out[0] = out[1] = out[2] = 0;
        return;
    }
    const double k = 65536.0 / n;
    out[0] = static_cast<int32_t>(double(v[0]) * k);
    out[1] = static_cast<int32_t>(double(v[1]) * k);
    out[2] = static_cast<int32_t>(double(v[2]) * k);
}

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
inline constexpr double from_fixed(int32_t v) { return io::fp16_16_to_double(v); }

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
