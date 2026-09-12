#pragma once

#include <array>
#include <cstdint>

namespace opennova::world {
struct AmmoTableEntry;

// WeaponDef +0x84/+0x88/+0x9C/+0xA0 and the 40-entry elevation table +0x3B0.
// [orig: WeaponSlot_CalcElevationTable @0x545100; WeaponSlot_InitFromDef @0x53ee70]
struct WeaponScopeZero {
    int32_t max_steps = 0;
    int32_t min_steps = 0;
    int32_t step_metres = 0;
    int32_t default_metres = 0;
    std::array<int32_t, 40> elevation{};
};

void weapon_scope_zero_bake(WeaponScopeZero &zero, const AmmoTableEntry &ammo);
int16_t weapon_scope_zero_initial(const WeaponScopeZero &zero);
// -1 is automatic rangefinder zero: allowed offline or in team games.
// [orig: Player_AdjustWeaponZoomLevel @0x4dbcc0]
int16_t weapon_scope_zero_adjust(const WeaponScopeZero &zero, int16_t current,
    int delta, bool in_session, bool team_game);
int32_t weapon_scope_zero_pitch(const WeaponScopeZero &zero, int16_t step);
}
