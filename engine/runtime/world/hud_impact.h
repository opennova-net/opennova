#pragma once
#include <array>
#include <cstdint>
namespace opennova::world {
class World;
struct LocalPlayerWeapon;
struct HudImpact {
	std::array<int32_t, 3> position{};
	int32_t distance_q16 = 0, radius_q16 = 0;
	bool hit = false;
};
// The mortar/designator preview uses the falling-object motor: drag BEFORE
// translation, gravity before Z, then water/nearest terrain and a bilinear snap.
// It never spawns a live round, effects, damage, or inventory mutations.
// [orig: Player_UpdatePerFrame @0x4DE350; Projectile_InitFromSpawnSlot @0x4E75A0;
// Entity_UpdateFallingObject @0x445420]
HudImpact predict_hud_impact(World &world, LocalPlayerWeapon &weapon, int32_t spread_q16);
} // namespace opennova::world
