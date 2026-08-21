// THE MOUNTED-VEHICLE PANEL element: the silhouette plus one marker per seat,
// drawn while the local player rides something
// [orig: HUD_DrawVehicleHealthBars @0x5A4FD0].
//
// The pieces this assembles all landed separately: the layout comes from the
// item's VEHICLE_HUD block (def.h, parsed in #536), the colour banding and
// marker geometry from hud/hud_vehicle_panel.h (#537), and the base corner from
// the HUDVEHSTANCEPOS anchor joined to the rider's stance offset (#540). This
// is the walk that puts them on screen.
//
// Draw order is the witnessed one and it matters: the silhouette first, then
// each seat box, then the labels, and the local player's own-seat X LAST so it
// is never painted over by a later seat.

#include <hud/hud_frame.h>
#include <hud/hud_vehicle_panel.h>

#include <string>

namespace opennova::hud {

namespace {

// The seat's fill colour, or the empty-seat treatment.
uint32_t band_color(const HudLayout &layout, SeatHealthBand band) {
	switch (band) {
	case SeatHealthBand::Good:
		return layout.stance_good;
	case SeatHealthBand::Middle:
		return layout.stance_middle;
	default:
		return layout.stance_bad;
	}
}

} // namespace

void HudFrameCompiler::element_vehicle_panel(const HudFrameState &state, float w,
		float h) {
	const HudVehiclePanelState &vp = state.vehicle_panel;
	if (!vp.shown) return;

	// The base rides the stance, so the whole panel moves with the stance icon.
	int base_x = 0;
	int base_y = 0;
	vehicle_panel_base(vp.anchor_x, vp.anchor_y, vp.stance_offset_x,
			vp.stance_offset_y, base_x, base_y);

	// 1. The silhouette, tinted by the HULL's band -- the vehicle's own health,
	// not any rider's. A missing texture leaves the seats readable rather than
	// dropping the panel.
	if (vp.silhouette_valid && vp.silhouette_w > 0 && vp.silhouette_h > 0) {
		const uint32_t tint =
				band_color(layout_, seat_health_band(vp.hull_health, vp.hull_max_health));
		emit_rect_uv(sx(static_cast<float>(base_x), w),
				sy(static_cast<float>(base_y), h),
				sx(static_cast<float>(base_x + vp.silhouette_w), w),
				sy(static_cast<float>(base_y + vp.silhouette_h), h),
				0.0f, 0.0f, 1.0f, 1.0f, tint, kHudTexVehiclePanel);
	}

	// 2. The seat boxes. An OCCUPIED seat is a filled rect banded by its rider's
	// health; an EMPTY one draws nothing here and shows its seat-select digit
	// below instead.
	for (const HudVehicleSeat &seat : vp.seats) {
		if (!seat.occupied) continue;
		int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
		seat_marker_rect(base_x, base_y, seat.x, seat.y, x0, y0, x1, y1);
		const uint32_t fill =
				band_color(layout_, seat_health_band(seat.health, seat.max_health));
		emit_rect(sx(static_cast<float>(x0), w), sy(static_cast<float>(y0), h),
				sx(static_cast<float>(x1), w), sy(static_cast<float>(y1), h), fill,
				true);
	}

	// 3. The empty seats' select labels, centred in the box. Retail draws
	// the digit white through the half-bright text path, which lands grey
	// [orig: HUD_DrawTextCentered_HalfBright(&g_hudLabelFontBold, x, y, buf,
	//  0xFFFFFFFF) @0x5a5322 -- white x half-bright = 0x7F7F7F].
	if (font_.font() != nullptr) {
		for (const HudVehicleSeat &seat : vp.seats) {
			if (seat.occupied || seat.label.empty()) continue;
			emit_text(seat.label.c_str(),
					static_cast<float>(seat_label_x(base_x, seat.x)),
					static_cast<float>(seat_label_y(base_y, seat.y)), w, h,
					0xFF7F7F7Fu, kFontAlignCenter);
		}

		// 4. The local player's own seat, marked LAST so nothing draws over it.
		for (const HudVehicleSeat &seat : vp.seats) {
			if (!seat.own_seat) continue;
			emit_text("X", static_cast<float>(seat_label_x(base_x, seat.x)),
					static_cast<float>(seat_label_y(base_y, seat.y)), w, h,
					0xFF7F7F7Fu, kFontAlignCenter);
			break;
		}
	}
}

} // namespace opennova::hud
