#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <opennova/retail_abi/address32.h>
#include <opennova/retail_abi/fixed_types.h>

namespace opennova::retail::jo_1_7_5_7 {

struct WeaponPose {
    FloatVec3 position;
    BamAngles3 rotation;
};

static_assert(sizeof(WeaponPose) == 0x18);
static_assert(offsetof(WeaponPose, position) == 0x0);
static_assert(offsetof(WeaponPose, rotation) == 0xC);
static_assert(std::is_standard_layout_v<WeaponPose>);
static_assert(std::is_trivially_copyable_v<WeaponPose>);

// Runtime AdmDef entry. AdmDef_GetEntryByIndex indexes 255 entries with a
// stride of 0x460, establishing that the AdmDef table is the weapon-definition
// table rather than a second, hook-owned representation.
struct WeaponDef {
    std::uint8_t unknown_0000[0x14];

    // [orig: 0x00543737] The bounded name copy establishes this capacity.
    char name[32];
    std::uint8_t unknown_0034[0x70];

    std::int32_t special_hold;
    std::int32_t attack_anim;
    std::uint8_t unknown_00ac[0x48];

    // Parser-scaled float translation (file value * 256) followed by BAM
    // rotation. The camera ftol's the position into its 16.16 accumulator.
    // The alternate pose is also called TPos/AltCamOffset in the RE record.
    WeaponPose hip_pose;
    WeaponPose aimed_pose;

    std::uint8_t unknown_0124[0x24];
    float render_fov;
    std::uint8_t unknown_014c[0x314];
};

static_assert(sizeof(WeaponDef) == 0x460);
static_assert(alignof(WeaponDef) == alignof(std::uint32_t));
static_assert(offsetof(WeaponDef, unknown_0000) == 0x000);
static_assert(offsetof(WeaponDef, name) == 0x014);
static_assert(offsetof(WeaponDef, unknown_0034) == 0x034);
static_assert(offsetof(WeaponDef, special_hold) == 0x0A4);
static_assert(offsetof(WeaponDef, attack_anim) == 0x0A8);
static_assert(offsetof(WeaponDef, unknown_00ac) == 0x0AC);
static_assert(offsetof(WeaponDef, hip_pose) == 0x0F4);
static_assert(offsetof(WeaponDef, hip_pose) + offsetof(WeaponPose, rotation) == 0x100);
static_assert(offsetof(WeaponDef, aimed_pose) == 0x10C);
static_assert(offsetof(WeaponDef, aimed_pose) + offsetof(WeaponPose, rotation) == 0x118);
static_assert(offsetof(WeaponDef, unknown_0124) == 0x124);
static_assert(offsetof(WeaponDef, render_fov) == 0x148);
static_assert(offsetof(WeaponDef, unknown_014c) == 0x14C);
static_assert(std::is_standard_layout_v<WeaponDef>);
static_assert(std::is_trivially_copyable_v<WeaponDef>);

inline constexpr std::uint32_t kWeaponDefCount = 255;

}  // namespace opennova::retail::jo_1_7_5_7
