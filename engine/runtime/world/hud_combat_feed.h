#pragma once
#include <array>
#include <runtime/hud/hud_combat.h>
#include <string>
namespace opennova::world {
class World;
struct LocalPlayerWeapon;
struct LocalPlayerViewFrame;
struct LocalPlayerViewTracker;
// World-space inputs are projected by the device using the actual play camera.
// [orig: HUD_BuildEntityInfo @0x4B8440; HUD_DrawCrosshair @0x592640]
struct HudCombatView {
	hud::HudCombatState state;
	std::array<int32_t, 3> target{}, aim{}, commander{}, impact{}, vehicle_fixed{}, vehicle_lag{};
	bool target_valid = false, aim_valid = false, commander_valid = false, impact_valid = false,
		 vehicle_fixed_valid = false, vehicle_lag_valid = false;
	std::string custom_texture, commander_texture, weapon_texture, vehicle_texture, cargo_texture;
	// The local player's position and team, the vehicle-bay logo walk's
	// viewer (hud_bay_logos.h) [orig: HUD_DrawVehicleBayLogos @0x5a2c00 --
	//  g_LocalPlayerEntity+0x162 @0x5a2c8e, +4/+8 @0x5a2cdf..0x5a2ce2].
	std::array<int32_t, 3> local_position{};
	uint8_t local_team = 0;
	bool local_valid = false;
};
void fill_hud_combat_view(World &world, LocalPlayerWeapon &weapon, LocalPlayerViewFrame &view,
		LocalPlayerViewTracker &tracker, bool can_fire);
void fill_hud_vehicle_sights(World &world, LocalPlayerWeapon &weapon, LocalPlayerViewFrame &view);
} // namespace opennova::world
