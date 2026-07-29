#pragma once

// Internal to libs/world's collision TUs — not part of world/collision.h. Split
// out of collision.cpp (quality campaign W3-2); the bodies are unchanged.
//
// The fixed-point math every collision TU shares: the retail sqrt/ftol chain, the
// BAM conversion constants, and the two lookups the queries and the resolvers both
// need. Header-inline because each is a handful of lines on a hot path.

#include "world/collision.h"

#include <cmath>
#include <cstdint>

#include <io/bam.h>

namespace opennova::world {

struct Entity; // world/entity.h — only the reference below is needed here

namespace detail {

inline int32_t abs32(int32_t v) { return opennova::io::bam_abs(v); }

// [orig: dbl_7C19D8 = 2^31/pi — BAM per radian]
constexpr double kBamPerRadian = 683565275.5764316;
// [orig: dbl_7C57B8 = -2^31/pi — the NEGATED BAM-per-radian the ladder-contact
// leg multiplies its atan2 results by (the target-relative subtraction then
// yields target + atan*BAM).]
constexpr double kNegBamPerRadian = -683565275.5764316;
// [orig: dbl_7C3608 = 2*pi/2^32 — radians per BAM32]
constexpr double kRadianPerBam = 1.4629180792671596e-9;
// [orig: flt_7C19E0 = 2147352576.0 — every sqrt is min-clamped to this before
// _ftol2_sse so the int cast can't overflow]
constexpr double kFtolClamp = 2147352576.0;

inline int32_t sqrt_ftol(double squared_len) {
    double len = std::sqrt(squared_len);
    if (len > kFtolClamp) len = kFtolClamp;
    return static_cast<int32_t>(len);
}

// Distance of `p` from the ray line through `start` along normalized `dir`
// (16.16), computed exactly like the original: project (float sqrt + ftol).
// [orig: the shared projection block in raycast_entity_collision @ 0x4139a4 /
// Entity_RaycastCollisionModel @ 0x4131a1 / Entity_FindNearestByRay @ 0x413d02]
inline int32_t ray_line_distance(const int32_t start[3], const int32_t dir[3], const int32_t p[3]) {
    const int64_t t = (static_cast<int64_t>(dir[1]) * (p[1] - start[1]) +
                       static_cast<int64_t>(dir[0]) * (p[0] - start[0]) +
                       static_cast<int64_t>(dir[2]) * (p[2] - start[2])) >> 16;
    int32_t d[3];
    for (int i = 0; i < 3; ++i) {
        const int32_t closest =
            start[i] + static_cast<int32_t>((static_cast<int64_t>(dir[i]) * t + 0x8000) >> 16);
        d[i] = abs32(closest - p[i]);
    }
    return sqrt_ftol(static_cast<double>(d[0]) * d[0] + static_cast<double>(d[1]) * d[1] +
                     static_cast<double>(d[2]) * d[2]);
}

inline int32_t vec_len_ftol(int32_t x, int32_t y, int32_t z) {
    return sqrt_ftol(static_cast<double>(x) * x + static_cast<double>(y) * y +
                     static_cast<double>(z) * z);
}

inline uint64_t ceil_sqrt_u64(uint64_t value) {
    // Restoring bit-by-bit integer square root. All intermediate values stay
    // within u64, including the maximum three-axis Q16 squared sum used when a
    // CollisionModel is finalized.
    uint64_t remainder = value;
    uint64_t root = 0;
    uint64_t bit = uint64_t{1} << 62; // highest power of four representable in u64
    while (bit > remainder) bit >>= 2;
    while (bit != 0) {
        if (remainder >= root + bit) {
            remainder -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return root + (remainder != 0 ? 1u : 0u);
}

inline uint64_t int32_magnitude(int32_t value) {
    return value < 0 ? static_cast<uint64_t>(-static_cast<int64_t>(value))
                     : static_cast<uint64_t>(value);
}

inline int32_t person_effective_radius(int32_t section, int32_t authored_radius,
                                int32_t extra_radius) {
    const int32_t scale = section == 14 ? 65 : 45;
    int32_t effective =
            extra_radius + 0xCCC +
            static_cast<int32_t>(static_cast<int64_t>(scale) * authored_radius / 100);
    if ((section == 15 || section == 16) && effective > 0x3000)
        effective = 0x3000;
    return effective;
}

// [orig: Math_TransformPointWithTranslation22 @ 0x412f60 — translate THEN rotate:
// used with the inverse matrix (t = -t_fwd) so local = R^T * (p - t_fwd).]
inline void transform_translate_then_rotate(const int32_t m[16], const int32_t in[3],
                                             int32_t out[3]) {
    const int32_t tx = in[0] + m[3];
    const int32_t ty = in[1] + m[7];
    const int32_t tz = in[2] + m[11];
    out[0] = static_cast<int32_t>((static_cast<int64_t>(ty) * m[1] +
                                   static_cast<int64_t>(tx) * m[0] +
                                   static_cast<int64_t>(tz) * m[2] + 0x200000) >> 22);
    out[1] = static_cast<int32_t>((static_cast<int64_t>(ty) * m[5] +
                                   static_cast<int64_t>(tx) * m[4] +
                                   static_cast<int64_t>(tz) * m[6] + 0x200000) >> 22);
    out[2] = static_cast<int32_t>((static_cast<int64_t>(ty) * m[9] +
                                   static_cast<int64_t>(tx) * m[8] +
                                   static_cast<int64_t>(tz) * m[10] + 0x200000) >> 22);
}

// Defined in collision_query.cpp — the contact broad-phase bound test, also used
// by the LOS and resolver legs.
bool contact_query_overlaps_bound(const int32_t target_pos[3], int32_t target_bound_radius,
                                  const ContactQuery &q);

// Defined in collision_world.cpp — entity position/radius in 16.16 engine units,
// read by the table build, the trace views and the resolvers.
void entity_pos_fixed(const Entity &e, int32_t out[3]);
int32_t entity_bound_radius(const CollisionWorld &cw, const CollisionModel *model,
                            int32_t uniform_scale_q16 = 0);
int32_t entity_proximity_radius(const CollisionWorld &cw, const Entity &e,
                                const CollisionModel *fallback_model);

} // namespace detail
} // namespace opennova::world
