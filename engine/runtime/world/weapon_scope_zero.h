#pragma once

#include <array>
#include <cstdint>

namespace opennova::world {
struct AmmoTableEntry;

// WeaponDef +0x84/+0x88/+0x8C/+0x9C/+0xA0, the 40-entry elevation table +0x3B0
// and the bake's +0xF0 max-range output.
// [orig: WeaponSlot_CalcElevationTable @0x545100; WeaponSlot_InitFromDef @0x53ee70]
struct WeaponScopeZero {
    int32_t max_steps = 0;
    int32_t min_steps = 0;
    int32_t step_metres = 0;
    int32_t default_metres = 0;
    // The `scope_paralax_distance` key, atof * 65535 -> WeaponDef+0x8C: the
    // parallax height the zero-yaw atan2 reads; 0 = key absent (the leg is
    // skipped). The weapon.def parse feeds it [orig: WeaponDefs_ParseLineCallback
    // @0x544e4e..0x544e80].
    int32_t paralax_distance_q16 = 0;
    // WeaponDef+0xF0: the furthest range (Q16) the bake's synthetic rounds
    // reached at their lifetime's expiry or their first tick below the ammo's
    // min-stable speed, over every solve. No retail reader traced
    // [orig: store @0x5453f8].
    int32_t max_range_q16 = 0;
    std::array<int32_t, 40> elevation{};
};

void weapon_scope_zero_bake(WeaponScopeZero &zero, const AmmoTableEntry &ammo);
int16_t weapon_scope_zero_initial(const WeaponScopeZero &zero);
// The zero-step adjust: capped at +0x84, floored at -1 (the automatic
// rangefinder zero) offline or when the mpattrib rules word carries 0x10000,
// else at 0, then at +0x88 when set.
// [orig: Player_AdjustWeaponZoomLevel @0x4dbd0c..0x4dbd3f -- `cmp
//  g_napi_np_ctx.is_in_session,0` @0x4dbd0c, `test g_rules_flags,10000h`
//  @0x4dbd15 (g_rules_flags @0x24D1E34 = the mpattrib word), the -1 floor
//  @0x4dbd29..0x4dbd2e, the 0 floor @0x4dbd21..0x4dbd25]
int16_t weapon_scope_zero_adjust(const WeaponScopeZero &zero, int16_t current,
    int delta, bool in_session, bool auto_scope_zero);
int32_t weapon_scope_zero_pitch(const WeaponScopeZero &zero, int16_t step);
// The zero-yaw term (retail MountSlot+8 = WeaponSlotState::zero_yaw): atan2(+0x8C,
// (+0x9C * step) << 16 floored at 100 m) in BAM, 0 without a parallax key. The
// slot installs (local_weapon_install, VehicleSystem::prepare_weapon_slot) seed
// it outside the flags & 3 elevation gate; the adjust recomputes it and alone
// negates it for a negative +0x8C (its caller's leg). Consumer, NOT ported: the
// main-scene view builder sub_5D27F0 adds MountSlot+8 to the view yaw and takes
// MountSlot+4 off the pitch under Player_CanFireWeapon
// [orig: WeaponSlot_InitFromDef @0x53ef4f..0x53ef8b; Player_AdjustWeaponZoomLevel
//  @0x4dbd91..0x4dbde3; sub_5D27F0 @0x5d2859..0x5d285c]
int32_t weapon_scope_zero_yaw(const WeaponScopeZero &zero, int16_t step);
}
