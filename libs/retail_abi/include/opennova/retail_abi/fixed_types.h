#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace opennova::retail {

// Float vector used by retail records that keep already-scaled coordinates in
// x87-compatible storage rather than integer fixed point.
struct FloatVec3 {
    float x;
    float y;
    float z;
};

// Signed 16.16 fixed-point vector used by the retail simulation.
struct FixedVec3 {
    std::int32_t x;
    std::int32_t y;
    std::int32_t z;
};

// Binary-angle-measure components in an axis-oriented record.
struct BamVec3 {
    std::int32_t x;
    std::int32_t y;
    std::int32_t z;
};

// Binary-angle-measure components whose entity semantics are established.
struct BamAngles3 {
    std::int32_t yaw;
    std::int32_t pitch;
    std::int32_t roll;
};

struct FixedPose3 {
    FixedVec3 position;
    BamVec3 rotation;
};

static_assert(sizeof(FloatVec3) == 0xC);
static_assert(alignof(FloatVec3) == alignof(float));
static_assert(offsetof(FloatVec3, x) == 0x0);
static_assert(offsetof(FloatVec3, y) == 0x4);
static_assert(offsetof(FloatVec3, z) == 0x8);
static_assert(std::is_standard_layout_v<FloatVec3>);
static_assert(std::is_trivially_copyable_v<FloatVec3>);

static_assert(sizeof(FixedVec3) == 0xC);
static_assert(alignof(FixedVec3) == alignof(std::int32_t));
static_assert(offsetof(FixedVec3, x) == 0x0);
static_assert(offsetof(FixedVec3, y) == 0x4);
static_assert(offsetof(FixedVec3, z) == 0x8);
static_assert(std::is_standard_layout_v<FixedVec3>);
static_assert(std::is_trivially_copyable_v<FixedVec3>);

static_assert(sizeof(BamVec3) == 0xC);
static_assert(alignof(BamVec3) == alignof(std::int32_t));
static_assert(offsetof(BamVec3, x) == 0x0);
static_assert(offsetof(BamVec3, y) == 0x4);
static_assert(offsetof(BamVec3, z) == 0x8);
static_assert(std::is_standard_layout_v<BamVec3>);
static_assert(std::is_trivially_copyable_v<BamVec3>);

static_assert(sizeof(BamAngles3) == 0xC);
static_assert(alignof(BamAngles3) == alignof(std::int32_t));
static_assert(offsetof(BamAngles3, yaw) == 0x0);
static_assert(offsetof(BamAngles3, pitch) == 0x4);
static_assert(offsetof(BamAngles3, roll) == 0x8);
static_assert(std::is_standard_layout_v<BamAngles3>);
static_assert(std::is_trivially_copyable_v<BamAngles3>);

static_assert(sizeof(FixedPose3) == 0x18);
static_assert(alignof(FixedPose3) == alignof(std::int32_t));
static_assert(offsetof(FixedPose3, position) == 0x0);
static_assert(offsetof(FixedPose3, rotation) == 0xC);
static_assert(std::is_standard_layout_v<FixedPose3>);
static_assert(std::is_trivially_copyable_v<FixedPose3>);

}  // namespace opennova::retail
