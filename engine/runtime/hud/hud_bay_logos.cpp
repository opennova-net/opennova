// The vehicle-bay logo walk -- see hud_bay_logos.h.

#include <runtime/hud/hud_bay_logos.h>

#include <algorithm>
#include <cmath>

namespace opennova::hud {

void vehicle_bay_logo_walk(const std::vector<HudMinimapMarker> &markers,
		const std::array<int32_t, 3> &local_position, uint8_t local_team,
		std::vector<HudBayLogo> &out) {
	out.clear();
	// The 1160 slots from word_28E5620 in address order: transient (328),
	// persistent (328), special (504) [orig: @0x5a2c18 / @0x5a2c1d, stride
	// 32 @0x5a2e1b].
	for (const HudMinimapBank bank : { HudMinimapBank::kTransient, HudMinimapBank::kPersistent,
				 HudMinimapBank::kSpecial }) {
		for (const HudMinimapMarker &m : markers) {
			if (static_cast<HudMinimapBank>(m.bank) != bank)
				continue;
			// [orig: the free handle @0x5a2c39, `test byte [ebx+3], 40h`
			//  @0x5a2c49, the pool entity @0x5a2c6b and its def +0x20
			//  @0x5a2c77..0x5a2c7c]
			if (m.handle == 0xFFFF || (m.flags & 0x40u) != 0 || !m.entity_known)
				continue;
			// The viewer's team or neutral [orig: entity+0x162 against
			//  g_LocalPlayerEntity+0x162 @0x5a2c82..0x5a2c98].
			if (m.team != local_team && m.team != 0)
				continue;
			// ItemDefAttrib2 bit 0, the vehicle bay [orig: `test byte
			//  [ebp+58h], 1` @0x5a2c9e].
			if ((m.entity_bits & kMarkerEntityVehicleBay) == 0)
				continue;
			// Team 1 palette[3], team 2 palette[5], else palette[1]
			// [orig: @0x5a2ca8..0x5a2cd3].
			const uint8_t palette = m.team == 1 ? 3 : (m.team == 2 ? 5 : 1);
			// The planar distance, capped then truncated [orig: @0x5a2cd9..0x5a2d10
			//  -- fild dx/dy, fsqrt, fcom against flt_7C19E0 = 2147418112.0,
			//  _ftol2_sse].
			const double dx = double(int32_t(uint32_t(m.entity_x) - uint32_t(local_position[0])));
			const double dy = double(int32_t(uint32_t(m.entity_y) - uint32_t(local_position[1])));
			const double r = std::min(std::sqrt(dx * dx + dy * dy), 2147418112.0);
			const int32_t d = static_cast<int32_t>(r);
			if (d > kBayLogoRangeQ16)
				continue;
			// [orig: @0x5a2d20..0x5a2d4a -- a negative alpha skips, unreachable
			//  past the range test]
			int32_t alpha = static_cast<int32_t>(double(kBayLogoRangeQ16 - d) * kBayLogoAlphaSlope);
			if (alpha > 255)
				alpha = 255;
			else if (alpha < 0)
				continue;
			// The type by the bay's spawn families: air first, then land,
			// then water; none draws nothing [orig: def+0xAD8 @0x5a2d6f;
			//  `test cl, 2` @0x5a2d80 -> 5, `test cl, 1` @0x5a2db5 -> 6,
			//  `test cl, 4` @0x5a2de6 -> 7].
			uint8_t type = 0;
			if ((m.bay_groups & 2u) != 0)
				type = kMarkerTypeLogoHelo;
			else if ((m.bay_groups & 1u) != 0)
				type = kMarkerTypeLogoHumm;
			else if ((m.bay_groups & 4u) != 0)
				type = kMarkerTypeLogoBoat;
			else
				continue;
			HudBayLogo logo;
			logo.position = { m.entity_x, m.entity_y,
				int32_t(uint32_t(m.entity_z) + uint32_t(kBayLogoLiftQ16)) };
			logo.type = type;
			logo.palette = palette;
			logo.alpha = static_cast<uint8_t>(alpha);
			out.push_back(logo);
		}
	}
}

} // namespace opennova::hud
