#pragma once

// The in-world vehicle-bay logos: the walk over the map-overlay slots that
// picks every local-team or neutral vehicle bay within 150 units, its logo
// type and its distance alpha. The device projects each admitted point and
// the frame compiler draws it through the marker drawer's types 5/6/7
// (HudFrameCompiler::element_vehicle_bay_logos).
// [orig: HUD_DrawVehicleBayLogos (ex Radar_DrawBlips, an IDB misnomer: no
//  radar, no mask bit) @0x5a2c00, called from HUD_RenderAllOverlays @0x5a8535
//  after HUD_DrawZoneStatusPanel; witness record docs/interface/hud-re.md
//  "The vehicle-bay logos"]

#include <array>
#include <cstdint>
#include <vector>

#include <runtime/hud/hud_combat.h>
#include <runtime/hud/hud_minimap.h>

namespace opennova::hud {

// The range cap and the alpha slope [orig: `cmp eax, 960000h` @0x5a2d15;
// flt_7D9DD8 = 0.00012969970703125 @0x5a2d2f]: full alpha inside 120 units,
// fading to 0 at 150.
inline constexpr int32_t kBayLogoRangeQ16 = 0x960000;
inline constexpr double kBayLogoAlphaSlope = 0.00012969970703125;
// The logo's anchor sits 8 units above the entity [orig: `add esi, 80000h`
// @0x5a2d75].
inline constexpr int32_t kBayLogoLiftQ16 = 0x80000;
// HUD_DrawEntityMarker's types for the three bay families.
inline constexpr uint8_t kMarkerTypeLogoHelo = 5;
inline constexpr uint8_t kMarkerTypeLogoHumm = 6;
inline constexpr uint8_t kMarkerTypeLogoBoat = 7;

// The walk: the transient, persistent then special bank (retail's three
// contiguous banks, 1160 slots), skipping a free handle, a special (0x40)
// slot, an unresolved entity, a team neither the viewer's nor 0, and a def
// without the VehicleBay attrib2 bit. `out` is cleared first.
void vehicle_bay_logo_walk(const std::vector<HudMinimapMarker> &markers,
		const std::array<int32_t, 3> &local_position, uint8_t local_team,
		std::vector<HudBayLogo> &out);

} // namespace opennova::hud
