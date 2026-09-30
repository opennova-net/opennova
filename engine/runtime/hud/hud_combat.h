#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace opennova::hud {
// The targeting and instrument inputs of HUD_BuildEntityInfo. Projection is
// supplied by the device; all eligibility and draw policy remains native.
// [orig: HUD_DrawCrosshair @0x592640; HUD_DrawScopeOverlayDetails @0x59E420;
// HUD_RenderOverlays @0x5A7BB0; HUD_DrawAltitudeBar @0x59F050]
struct HudProjectedPoint {
	float x = 0, y = 0; // surface pixels
	uint8_t clip = 0; // left/right/top/bottom/near = 1/2/4/8/16
	bool valid = false;
};
struct HudSprite {
	int width = 0, height = 0;
	bool valid = false;
};
struct HudCombatLayout {
	HudSprite driver_crosshair, vehicle_fixed, vehicle_lag;
	HudSprite target, target_friendly, custom_aim, commander;
	HudSprite weapon, vehicle, cargo, parachute, armor;
	int impact_x = 0, impact_y = 0;
	int icon_x = 0, icon_y = 0, gear_x = 0, gear_y = 0;
	int cargo_x = 0, cargo_y = 0;
	int parachute_x = 0, parachute_y = 0, armor_x = 0, armor_y = 0;
	int agl_tick_width = 0, agl_left = 0, agl_right = 0, agl_y = 0, agl_height = 0;
	uint32_t agl_color = 0xFF000000u;
};
// Active type-1 designation points from the received 0x6B link table.
// [orig: SpawnPoint_FindNearestByTypeAndTeam @0x5BBF10]
struct HudDesignationPoint {
	int32_t x = 0, y = 0, radius_q16 = 0;
	uint8_t team = 0;
};
struct HudServiceState {
	uint8_t preround_seconds = 0, reload_seconds = 0;
	uint32_t owned_zone_mask = 0;
};
struct HudCombatState {
	// The client death-screen latch, the ONE flag the crosshair, instrument and
	// scope passes test. The local dead bit and the death lerp camera are not
	// HUD gates: between the death and the latch those passes still draw.
	// [orig: g_DeathScreenActive @0xA860EC -- HUD_DrawCrosshair @0x592646,
	//  HUD_RenderOverlays @0x5A7BBC, HUD_RenderAllOverlays @0x5A850D]
	bool death_screen = false;
	// Armory/bay use one line; an eligible FARP replaces it later in the walk.
	int service_prompt = 0, service_wait_seconds = 0;
	bool service_above_declutter = false;
	std::string service_text;

	bool target_cursor = false, target_friendly = false, target_locked = false;
	bool target_brackets = false;
	bool custom_aim = false, commander = false;
	bool hit_feedback = false;
	bool inset = false, inset_friendly = false, impact_map = false, impact_distance = false,
		 designator = false;
	bool driver_crosshair = false, vehicle_fixed = false, vehicle_lag = false,
		 vehicle_lag_center = false;
	HudProjectedPoint vehicle_fixed_point, vehicle_lag_point;
	// The mortar distance template, Overlays/STROVER_DIST. Unresolved it is
	// GameText_GetString's miss, "" [orig: @0x5A8961 -> the miss @0x51EC08].
	std::string target_name, impact_format;
	int32_t impact_x_q16 = 0, impact_y_q16 = 0, impact_radius_q16 = 0;
	uint16_t impact_distance_m = 0;
	float inset_fov_over_zoom = 0, designator_scale = 1;
	uint32_t designator_color = 0x60FF0000u;
	HudProjectedPoint impact_point;
	HudProjectedPoint target_point, aim_point, commander_point;
	// The nearest FARP of the proximity list (def attrib2 0x2000, zone-owned
	// or unnumbered) within 0x40000000 Q16 — hudInfo+0x10 and its position
	// g_TrackedTargetPos, rebuilt every frame [orig: HUD_BuildEntityInfo
	// @0x4b891a..0x4b89e3 after the memset @0x5a80b1].
	bool farp_present = false;
	int32_t farp_x_q16 = 0, farp_y_q16 = 0;
	bool vehicle_controls = false, parachute = false, armor = false, carrying = false;
	uint64_t vehicle_identity = 0;
	std::string weapon_identity;
	int gear = 2;
	std::array<std::string, 3> gear_text = { "!Med", "!Low", "!High" };
	int32_t altitude_agl_q16 = 0, altitude_q16 = 0, vertical_velocity_q16 = 0;
	std::string altitude_text = "altitude";
};
// Shared weapon/vehicle silhouette flash. The original keeps one stamp and
// separate previous weapon/root identities. Zero elapsed is promoted to one.
// [orig: sub_59A710 @0x59A74F..0x59A78F; HUD_DrawTargetEntityOverlay @0x59A5E2]
int hud_silhouette_alpha(int elapsed, int ramp, int base, int maximum);
} // namespace opennova::hud
