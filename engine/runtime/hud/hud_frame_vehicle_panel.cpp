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

#include <runtime/hud/hud_frame.h>
#include <runtime/hud/hud_vehicle_panel.h>

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
	// The interface texture gates the entire retail panel, including seats.
	// [orig: HUD_DrawVehicleHealthBars @0x5A5038]
	if (!vp.shown || !vp.silhouette_valid) return;

	// The base rides the stance, so the whole panel moves with the stance icon.
	int base_x = 0;
	int base_y = 0;
	vehicle_panel_base(vp.anchor_x, vp.anchor_y, vp.stance_offset_x,
			vp.stance_offset_y, base_x, base_y);

	// 1. The silhouette, tinted by the HULL's band -- the vehicle's own health,
	// not any rider's. The texture gate above applies to every panel element.
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
		// Riders and the driver band through the inline unsigned/signed pair;
		// an emplacement occupant through the clamped shared classifier
		// [orig: @0x5a507f/@0x5a508a vs @0x5a54d3..0x5a54e3].
		const SeatHealthBand band = seat.is_emplacement
				? emplacement_health_band(seat.health, seat.max_health)
				: seat_health_band(seat.health, seat.max_health);
		const uint32_t fill = band_color(layout_, band);
		emit_rect(sx(static_cast<float>(x0), w), sy(static_cast<float>(y0), h),
				sx(static_cast<float>(x1), w), sy(static_cast<float>(y1), h), fill,
				true);
	}

	// 3. The empty seats' select labels, in the BOLD label font at its slot
	// scale, centred ON the box: x at the box centre, the text's TOP at
	// centre + 1 - h/2 of the measured label (CGameFont_DrawText takes y as
	// the top; the half-bright wrapper passes x/y straight through @0x580680).
	// Retail draws the digit white through that half-bright path, which lands
	// grey [orig: HUD_MeasureTextWH(&g_hudLabelFontBold, buf, &w, &h), then
	//  HUD_DrawTextCentered_HalfBright(&g_hudLabelFontBold, cx, cy + 1 - h/2,
	//  buf, 0xFFFFFFFF) @0x5a52f0..0x5a5322; the driver digit @0x5a57ee.. and
	//  the "X" @0x5a586b.. take the same road; white x half-bright = 0x7F7F7F].
	const bool have_bold = label_font_bold_.font() != nullptr;
	const GameFont &bf = have_bold ? label_font_bold_ : font_;
	const float bscale = have_bold ? label_scale_ : 1.0f;
	if (bf.font() != nullptr) {
		const auto centred_label = [&](const char *t, int cx, int cy) {
			int tw = 0;
			int th = 0;
			bf.measure(t, bscale, bscale, &tw, &th);
			// The +1 is design units (scaled with the anchor); the half height
			// is the measured surface-px height, floor-halved like retail's
			// integer h/2.
			const float top = sy(static_cast<float>(cy + 1), h) -
					static_cast<float>(th / 2);
			const GameFontRun run = bf.layout(t, sx(static_cast<float>(cx), w),
					top, bscale, bscale, kFontAlignCenter, 0xFF7F7F7Fu);
			draw_list_.glyphs.insert(draw_list_.glyphs.end(), run.quads.begin(),
					run.quads.end());
		};
		for (const HudVehicleSeat &seat : vp.seats) {
			if (seat.occupied || seat.label.empty()) continue;
			centred_label(seat.label.c_str(), seat_label_x(base_x, seat.x),
					seat_label_y(base_y, seat.y));
		}

		// 4. The local player's own seat, marked LAST so nothing draws over it
		// [orig: the "X" arm @0x5a586b runs after every seat arm].
		for (const HudVehicleSeat &seat : vp.seats) {
			if (!seat.own_seat) continue;
			centred_label("X", seat_label_x(base_x, seat.x),
					seat_label_y(base_y, seat.y));
			break;
		}
	}
}

} // namespace opennova::hud
