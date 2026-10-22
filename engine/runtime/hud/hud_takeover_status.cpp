// The takeover status dialog: its composition and its element -- see
// hud_takeover_status.h.

#include <runtime/hud/hud_takeover_status.h>

#include <cstdio>
#include <string>
#include <vector>

#include <runtime/hud/hud_frame.h>
#include <runtime/hud/hud_game_text.h>
#include <runtime/hud/hud_math.h>

namespace opennova::hud {

namespace {

// A status string looked up with its fallback [orig: GameText_GetStringWithFallback
// @0x51EB90 with Overlays @0x7C4A50].
std::string overlay_text(const GameTextLookup &gametext, const char *key, const char *fallback) {
	return game_text(gametext, kGameTextOverlays, key, fallback);
}

// The 32-bit product the cuts take (imul wraps), then the truncating divide.
int32_t cut(int32_t a, int32_t b, int32_t d) {
	const int32_t product = static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b));
	return d != 0 ? product / d : 0;
}

int32_t outcode_x(int32_t x) { return (x < 0 ? 8 : 0) | (x > 1023 ? 4 : 0); }
int32_t outcode_y(int32_t y) { return (y < 0 ? 2 : 0) | (y > 767 ? 1 : 0); }

} // namespace

HudTakeoverStatusState compose_takeover_status(const HudTakeoverFacts &f, int local_team,
		const GameTextLookup &gametext) {
	HudTakeoverStatusState s;
	// No entry, or an unnumbered zone whose window is not running, draws
	// nothing [orig: @0x59B66A..0x59B689 -- `cmp byte [eax+21Ah], 0`, then
	// `cmp dword [esi+1Ch], 0`].
	if (!f.present || (f.zone_byte == 0 && !f.window_active)) return s;
	s.shown = true;
	// The name: a numbered zone's "Objective <WPNames name>", else the PSP
	// format over the zone's letter [orig: SpawnZoneList_IndexOf @0x59B6B3;
	//  sprintf("STRWPNAME%03d", index + 1) @0x59B6D7, GameText_GetString
	//  @0x59B6E9 / @0x59B6FC, sprintf @0x59B72E; index + 'A' @0x59B70F,
	//  STROVER_PSP @0x59B71D].
	if (f.zone_byte != 0) {
		char key[32];
		std::snprintf(key, sizeof key, "STRWPNAME%03d", f.zone_index + 1);
		HudTextArg name;
		name.text = game_text(gametext, kGameTextWPNames, key, "");
		s.name = hud_sprintf(game_text(gametext, kGameTextOverlays, "STROVER_OBJECTIVEPOINT_SHORT", ""),
				std::vector<HudTextArg>{ name });
	} else {
		s.name = hud_sprintf(game_text(gametext, kGameTextOverlays, "STROVER_PSP", ""),
				f.zone_index + 'A');
	}
	// The distance: past half the zone's bound radius, floored at zero, in
	// whole units [orig: `mov ebx, [ebx]; sar ebx, 1` @0x59B77B..0x59B77D,
	//  @0x59B77F..0x59B789, `sar eax, 10h` @0x59B789, "%ldm" @0x59B79A].
	const int32_t half = f.bound_radius_q16 >> 1;
	const int32_t edge = f.distance_q16 <= half ? 0 : f.distance_q16 - half;
	char text[32];
	std::snprintf(text, sizeof text, "%ldm", static_cast<long>(edge >> 16));
	s.distance = text;
	// The colour by entry[2] [orig: @0x59B7F8..0x59B819].
	s.palette = f.team_b == 1 ? 3 : (f.team_b == 2 ? 5 : 1);
	// The first bar [orig: @0x59B863..0x59B883 -- fild [esi+0Ch] / fidiv
	//  [esi+14h]; `cmp ecx, [esi+8]`, `cmp [esi+30h], 0`, fld1 / fldz].
	if (f.window_active)
		s.first_fraction = static_cast<float>(double(f.window_current) / double(f.window_limit));
	else if (f.team_a == f.team_b && f.value_active)
		s.first_fraction = 1.0f;
	std::snprintf(text, sizeof text, "%dx", int(f.count_owner) - int(f.count_other));
	s.count = text;
	s.first_count = f.zone_byte != 0 && f.window_active;
	s.window_row = f.window_active;
	// The window's status line [orig: @0x59B90D..0x59B9E1: a numbered zone's
	//  STROVER_TAKINGOVERLFP; else by the SpawnPoint def bit (`test [eax+54h],
	//  40000h` @0x59B928) and the def type (`mov eax, [eax+5Ch]` @0x59B92F):
	//  5 a building, 1 a vehicle, else a spawn point / an object; each string
	//  handed to sprintf as the format].
	if (f.window_active) {
		const char *key = nullptr;
		const char *fallback = nullptr;
		if (f.zone_byte != 0) {
			key = "STROVER_TAKINGOVERLFP";
			fallback = "!Taking over LFP";
		} else if (f.spawn_point_def) {
			if (f.def_type == 5) {
				key = "STROVER_TAKINGOVERBLDPSP";
				fallback = "!Taking over building PSP";
			} else if (f.def_type == 1) {
				key = "STROVER_TAKINGOVERVEHPSP";
				fallback = "!Taking over vehicle PSP";
			} else {
				key = "STROVER_TAKINGOVERPSP";
				fallback = "!Taking over spawn point";
			}
		} else if (f.def_type == 5) {
			key = "STROVER_TAKINGOVERBLD";
			fallback = "!Taking over building";
		} else if (f.def_type == 1) {
			key = "STROVER_TAKINGOVERVEH";
			fallback = "!Taking over vehicle";
		} else {
			key = "STROVER_TAKINGOVEROBJ";
			fallback = "!Taking over object";
		}
		s.status = hud_sprintf(overlay_text(gametext, key, fallback));
	}
	if (f.zone_byte == 0) return s; // [orig: @0x59B9EB..0x59B9FC]
	// A numbered zone's second bar and its camp line [orig: @0x59BA01..0x59BC6E].
	s.second_bar = true;
	if (f.value_active)
		s.second_fraction = static_cast<float>(double(f.value_current) / double(f.value_limit));
	s.second_count = f.value_active && f.value_rate != 0;
	if (!f.value_active) return s; // the window's line stands [orig: @0x59BAD3]
	if (f.value_target == f.value_limit) {
		// [orig: `cmp eax, [esi+28h]` @0x59BAE0, sprintf("%s") @0x59BB04]
		s.status = overlay_text(gametext, "STROVER_SETUPCAMP", "!Camp set up");
	} else if (f.value_target > 0) {
		if (f.value_rate > 0) {
			// The capturing side's camp going up [orig: `cmp [esi+8], eax`
			//  @0x59BB2D].
			s.status = f.team_b == local_team
					? overlay_text(gametext, "STROVER_SETTINGUPCAMP", "!Setting up camp")
					: overlay_text(gametext, "STROVER_SETTINGUPCAMPENEMY", "!Enemy is setting up camp");
		} else if (f.value_rate < 0) {
			// A camp coming down: a neutral owner's, else by entry[2]
			// [orig: `cmp dword [esi+4], 0` @0x59BB90, `cmp [esi+8], edx` @0x59BBCF].
			if (f.team_a == 0)
				s.status = overlay_text(gametext, "STROVER_TAKINGNEUTRALCAMP", "!Taking down neutral camp");
			else if (f.team_b == local_team)
				s.status = overlay_text(gametext, "STROVER_DESTROYINGCAMPENEMY", "!Enemy is destroying camp");
			else
				s.status = overlay_text(gametext, "STROVER_DESTROYINGCAMP", "!Destroying enemy camp");
		} else {
			s.status.clear(); // [orig: sprintf(g_EmptyStr) @0x59BC30]
		}
	} else if (f.team_b != 0) {
		// [orig: `cmp dword [esi+8], 0` @0x59BC3A, STROVER_NOCAMP as the format @0x59BC6E]
		s.status = hud_sprintf(overlay_text(gametext, "STROVER_NOCAMP", "!No camp"));
	} else {
		s.status.clear(); // [orig: @0x59BC4A]
	}
	return s;
}

bool clip_design_line(int32_t &x0, int32_t &y0, int32_t &x1, int32_t &y1) {
	int32_t oc0 = outcode_x(x0) | outcode_y(y0);
	int32_t oc1 = outcode_x(x1) | outcode_y(y1);
	if ((oc0 & oc1) != 0) return false;
	if ((oc0 | oc1) == 0) return true;
	const int32_t dx = oc0 ^ oc1;
	if ((dx & 8) != 0) {
		if ((oc0 & 8) != 0) {
			y0 = y0 - cut(x0, y1 - y0, x1 - x0);
			x0 = 0;
		} else {
			y1 = y1 - cut(x1, y0 - y1, x0 - x1);
			x1 = 0;
		}
	}
	if ((dx & 4) != 0) {
		if ((oc0 & 4) != 0) {
			y0 += cut(1023 - x0, y1 - y0, x1 - x0);
			x0 = 1023;
		} else {
			y1 += cut(1023 - x1, y0 - y1, x0 - x1);
			x1 = 1023;
		}
	}
	oc0 = outcode_y(y0);
	oc1 = outcode_y(y1);
	if ((oc0 & oc1) != 0) return false;
	const int32_t dy = oc0 ^ oc1;
	if ((dy & 2) != 0) {
		if ((oc0 & 2) != 0) {
			x0 -= cut(y0, x1 - x0, y1 - y0);
			y0 = 0;
		} else {
			x1 -= cut(y1, x0 - x1, y0 - y1);
			y1 = 0;
		}
	}
	if ((dy & 1) != 0) {
		if ((oc0 & 1) != 0) {
			x0 = cut(767 - y0, x1 - x0, y1 - y0) + x0;
			y0 = 767;
		} else {
			x1 = cut(767 - y1, x0 - x1, y0 - y1) + x1;
			y1 = 767;
		}
	}
	return true;
}

// The dialog in retail's order: the stdbox, the name and the distance in the
// large slot, the rule, the bars, the excess counts and the status line in the
// bold slot, every text through the half-bright drawers [orig:
// HUD_DrawLabelBox @0x59B6AB; HUD_DrawTextLeftScaled @0x59B7B5;
// HUD_DrawTextRightAlignedScaled @0x59B7D9; Render_ClipAndDrawLine2DScaled
// @0x59B7F3; HUD_DrawProgressBar @0x59B8A1 / @0x59BA71; HUD_DrawTextLeftScaled
// (g_HUDLabelFontBold) @0x59B8F6 / @0x59BAC8 / @0x59BC85].
void HudFrameCompiler::element_takeover_status(const HudFrameState &state, float w, float h) {
	const HudTakeoverStatusState &ts = state.takeover_status;
	if (!ts.shown || w <= 0 || h <= 0) return;
	const int32_t surface_w = int32_t(w), surface_h = int32_t(h);
	const int x = layout_.lfp_takeover_x;
	const int y = layout_.lfp_takeover_y;
	emit_label_box(float(x + kTakeoverBoxLeft), float(y + kTakeoverBoxTop),
			float(x + kTakeoverBoxRight), float(y + kTakeoverBoxBottom), std::string(),
			0xFFFFFFFFu, w, h);
	// The slots fall back like every label element's.
	const bool have_large = label_font_large_.font() != nullptr;
	const bool have_bold = label_font_bold_.font() != nullptr;
	const GameFont &large = have_large ? label_font_large_ : (have_bold ? label_font_bold_ : font_);
	const float large_scale = have_large ? label_large_scale_ : (have_bold ? label_scale_ : 1.0f);
	const GameFont &bold = have_bold ? label_font_bold_ : font_;
	const float bold_scale = have_bold ? label_scale_ : 1.0f;
	const uint32_t text_color = half_bright_argb(kTakeoverTextColor);
	if (!ts.name.empty())
		emit_slot_text(large, large_scale, ts.name.c_str(), sx(float(x), w), sy(float(y), h),
				text_color, 0u);
	if (!ts.distance.empty())
		emit_slot_text(large, large_scale, ts.distance.c_str(), sx(float(x + kTakeoverDistanceDx), w),
				sy(float(y), h), text_color, kFontAlignRight);
	// The rule: clipped in design space, scaled, drawn opaque unless both ends
	// meet [orig: Render_ClipAndDrawLine2DScaled @0x5D3160 -- the scale
	//  @0x5D3372 / @0x5D3391, the same-point skip @0x5D33AB, `or 0FF000000h`
	//  @0x5D33EE].
	{
		int32_t lx0 = x, ly0 = y + kTakeoverRuleDy;
		int32_t lx1 = x + kTakeoverRuleWidth, ly1 = y + kTakeoverRuleDy;
		if (clip_design_line(lx0, ly0, lx1, ly1)) {
			lx0 = design_to_screen_x(lx0, surface_w);
			ly0 = design_to_screen_y(ly0, surface_h);
			lx1 = design_to_screen_x(lx1, surface_w);
			ly1 = design_to_screen_y(ly1, surface_h);
			if (lx0 != lx1 || ly0 != ly1)
				draw_list_.lines.push_back({ float(lx0), float(ly0), float(lx1), float(ly1), 1.0f,
						kTakeoverRuleColor | 0xFF000000u });
		}
	}
	const uint32_t color = hud_palette(ts.palette);
	const auto bar = [&](int row, float fraction) {
		emit_progress_bar(design_to_screen_x(x, surface_w), design_to_screen_y(row, surface_h),
				design_to_screen_x(x + kTakeoverBarWidth, surface_w),
				design_to_screen_y(row + kTakeoverBarHeight, surface_h), color, color, fraction, false);
	};
	const auto count = [&](int row) {
		emit_slot_text(bold, bold_scale, ts.count.c_str(), sx(float(x + kTakeoverCountDx), w),
				sy(float(row - 1), h), half_bright_argb(color), 0u);
	};
	int row = y + kTakeoverBarDy;
	bar(row, ts.first_fraction);
	if (ts.first_count) count(row);
	if (ts.window_row) row += kTakeoverRowStep;
	if (ts.second_bar) {
		bar(row, ts.second_fraction);
		if (ts.second_count) count(row);
		row += kTakeoverRowStep;
	}
	if (!ts.status.empty())
		emit_slot_text(bold, bold_scale, ts.status.c_str(), sx(float(x), w), sy(float(row), h),
				half_bright_argb(color), 0u);
	++draw_list_.elements_drawn;
}

} // namespace opennova::hud
