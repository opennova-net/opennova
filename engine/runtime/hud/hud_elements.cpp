// Each element of the HUD's walk by its token, and the box of what it drew: a tool's record of the
// walk's own draws (hud_elements.h), the walk being HUD_RenderOverlays' [orig: HUD_RenderAllOverlays
// @0x5a8070 -> HUD_RenderOverlays @0x5a7bb0].
#include <runtime/hud/hud_elements.h>

#include <algorithm>
#include <cstring>
#include <iterator>

#include <runtime/hud/hud_frame.h>

namespace opennova::hud {

namespace {

constexpr const char *kTokens[] = {
	"sights_card",
	"breath_bar",
	"service_prompt",
	"inset_cues",
	"game_info",
	"frame",
	"health",
	"instruments",
	"optical_cues",
	"stance",
	"ammo_count",
	"weapon_name",
	"clip_indicator",
	"targeting",
	"crosshair",
	"heat",
	"clock",
	"power",
	"waypoint",
	"team_id_line",
	"weapon_slot_bar",
	"scope_details",
	"capture_point_labels",
	"lfp_panel",
	"vehicle_bay_logos",
	"spinmap",
	"attach_labels",
	"friendly_tags",
	"vehicle_panel",
	"feed",
	"squad_orders",
	"tip",
	"end_round_statistics",
	"message_log",
	"scoreboard",
	"end_round_overlay",
	"voice_menus",
	"paused_text",
	"chat_input",
	"kill_announcement",
	"tip_alternate",
	"briefing",
	"objectives",
	"help_screen",
	"quit_dialog",
	"net_quality",
};
static_assert(std::size(kTokens) == kHudElementCount, "every HudElement has exactly one token");

// The bounds a box grows to hold.
struct Bounds {
	bool any = false;
	float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
	void add(float x, float y) {
		if (!any) {
			x0 = x1 = x;
			y0 = y1 = y;
			any = true;
			return;
		}
		x0 = std::min(x0, x);
		y0 = std::min(y0, y);
		x1 = std::max(x1, x);
		y1 = std::max(y1, y);
	}
};

void add_pass(Bounds &bounds, const HudMapPass &pass) {
	if (!pass.visible) return;
	bounds.add(pass.clip_x1, pass.clip_y1);
	bounds.add(pass.clip_x2, pass.clip_y2);
}

} // namespace

const char *hud_element_token(HudElement element) {
	const size_t index = static_cast<size_t>(element);
	return index < kHudElementCount ? kTokens[index] : "";
}

bool hud_element_from_token(const char *token, HudElement &out) {
	if (!token) return false;
	for (size_t i = 0; i < kHudElementCount; ++i)
		if (std::strcmp(token, kTokens[i]) == 0) {
			out = static_cast<HudElement>(i);
			return true;
		}
	return false;
}

std::vector<HudElementBox> hud_element_boxes(const HudDrawList &list) {
	std::vector<HudElementBox> out;
	out.reserve(list.element_spans.size());
	for (const HudElementSpan &span : list.element_spans) {
		Bounds bounds;
		for (size_t i = span.begin.quads; i < span.end.quads && i < list.quads.size(); ++i) {
			bounds.add(list.quads[i].x0, list.quads[i].y0);
			bounds.add(list.quads[i].x1, list.quads[i].y1);
		}
		for (size_t i = span.begin.tris; i < span.end.tris && i < list.tris.size(); ++i)
			for (const HudTriVertex *v : {&list.tris[i].a, &list.tris[i].b, &list.tris[i].c}) bounds.add(v->x, v->y);
		for (size_t i = span.begin.lines; i < span.end.lines && i < list.lines.size(); ++i) {
			bounds.add(list.lines[i].x0, list.lines[i].y0);
			bounds.add(list.lines[i].x1, list.lines[i].y1);
		}
		for (size_t i = span.begin.glyphs; i < span.end.glyphs && i < list.glyphs.size(); ++i) {
			const GameFontQuad &g = list.glyphs[i];
			bounds.add(std::min(g.x_top_left, g.x_bottom_left), g.y_top);
			bounds.add(std::max(g.x_top_right, g.x_bottom_right), g.y_bottom);
		}
		for (size_t i = span.begin.underlines; i < span.end.underlines && i < list.underlines.size(); ++i) {
			bounds.add(list.underlines[i].x0, list.underlines[i].y);
			bounds.add(list.underlines[i].x1, list.underlines[i].y + 1.0f);
		}
		if (span.map) add_pass(bounds, list.map);
		if (span.big_map) add_pass(bounds, list.big_map);
		if (!bounds.any) continue;
		HudElementBox box;
		box.element = span.element;
		box.x0 = std::min(bounds.x0, bounds.x1);
		box.y0 = std::min(bounds.y0, bounds.y1);
		box.x1 = std::max(bounds.x0, bounds.x1);
		box.y1 = std::max(bounds.y0, bounds.y1);
		out.push_back(box);
	}
	return out;
}

} // namespace opennova::hud
