// HudFrameCompiler + GameFont pins: the witnessed element walk emits the
// typed draw list (quads/tris/glyphs) with the ported policy math intact.
// [orig: HUD_RenderOverlays @ 0x5a7bb0; CGameFont_MeasureText @ 0x674e70;
//  CGameFont_DrawText @ 0x6752c0]

#include <hud/game_font.h>
#include <hud/hud_frame.h>
#include <hud/hud_math.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using opennova::hud::GameFont;
using opennova::hud::GameFontState;
using opennova::hud::HudDrawList;
using opennova::hud::HudFrameCompiler;
using opennova::hud::HudFrameState;
using opennova::hud::HudLayout;
using opennova::hud::HudMinimapCompiler;
using opennova::hud::HudMinimapInput;

namespace {

int failures = 0;

#define CHECK(cond, msg)                                                       \
	do {                                                                       \
		if (!(cond)) {                                                         \
			std::fprintf(stderr, "FAIL: %s\n", msg);                           \
			++failures;                                                        \
		}                                                                      \
	} while (0)

// A synthetic 1-page font: every glyph 8x16 px on the 256-page grid,
// spacing 2, design width 800 (scale 1).
fnt_font_t make_font() {
	fnt_font_t font{};
	fnt_init_blank(&font, 1, 2);
	font.design_width = 800;
	for (uint32_t i = 0; i < FNT_GLYPH_COUNT; ++i) {
		font.glyphs[i].page = 0;
		font.glyphs[i].uv.u0 = 0.0f;
		font.glyphs[i].uv.v0 = 0.0f;
		font.glyphs[i].uv.u1 = 8.0f / 256.0f;
		font.glyphs[i].uv.v1 = 16.0f / 256.0f;
	}
	return font;
}

void test_measure_advance_and_trailing_pad(const fnt_font_t *font) {
	GameFont gf;
	gf.set_font(font);
	int w = 0;
	int h = 0;
	// Each glyph advances floor(8 + (2-1) + 0.5) = 9; the final width strips
	// the trailing (spacing-1) pad: 3 glyphs -> 27 - 1 = 26.
	gf.measure("abc", 1.0f, 1.0f, &w, &h);
	CHECK(w == 26, "measure width = glyph advances minus the trailing pad");
	CHECK(h == 16, "measure height = the SPACE glyph's v-extent");

	// Controls and the three retail control bytes are skipped.
	int w2 = 0;
	gf.measure("a\x7f\x01"
			   "bc",
			1.0f, 1.0f, &w2, &h);
	CHECK(w2 == w, "0x7F and control bytes measure nothing");

	// Newline folds the max and adds a line.
	int w3 = 0;
	int h3 = 0;
	gf.measure("abc\nabcd", 1.0f, 1.0f, &w3, &h3);
	CHECK(w3 == 35, "multiline width is the widest line");
	CHECK(h3 == 32, "each newline adds one line height");
}

void test_layout_pages_bold_underline(const fnt_font_t *font) {
	GameFont gf;
	gf.set_font(font);
	const auto run = gf.layout("ab", 100.0f, 50.0f, 1.0f, 1.0f, 0u,
			0xFF102030u);
	CHECK(run.quads.size() == 2, "one quad per glyph on the one page");
	CHECK(run.quads[0].x_top_left == 99.5f,
			"the drawer's -0.5 vertex offset is kept");
	CHECK(run.quads[1].x_top_left == 108.5f,
			"the second glyph starts at the floored advance");
	CHECK(run.underlines.empty(), "no underline without the flag");

	const auto bold = gf.layout("a", 0.0f, 0.0f, 1.0f, 1.0f,
			opennova::hud::kFontStyleBold, 0xFF000000u);
	CHECK(bold.quads.size() == 2, "bold double-strikes each glyph");
	CHECK(bold.quads[1].x_top_left == bold.quads[0].x_top_left + 1.0f,
			"the second strike lands one pixel right");

	const auto lined = gf.layout("a", 0.0f, 0.0f, 1.0f, 1.0f,
			opennova::hud::kFontStyleUnderline, 0xFF404040u);
	CHECK(lined.underlines.size() == 1, "underline emits one segment");
	CHECK((lined.underlines[0].color & 0xFFFFFFu) == 0x808080u,
			"the underline color doubles the text color");
}

void test_format_tags(const fnt_font_t *font) {
	GameFont gf;
	gf.set_font(font);
	GameFontState state;
	int idx = 0;
	CHECK(GameFont::parse_format_tag("<B>", &idx, &state), "well-formed <B>");
	CHECK(state.bold, "<B> sets bold");
	idx = 0;
	CHECK(GameFont::parse_format_tag("<-B>", &idx, &state), "<-B> parses");
	CHECK(!state.bold, "<-B> clears bold");
	idx = 0;
	CHECK(!GameFont::parse_format_tag("<B", &idx, &state),
			"an unterminated tag reports failure");
	idx = 0;
	GameFontState color_state;
	CHECK(GameFont::parse_format_tag("<C102030>", &idx, &color_state),
			"hex color tag parses");
	CHECK((color_state.color_xor & 0xFFFFFFu) == 0x102030u,
			"the color XOR folds the tag color");
	// Measurement skips tags entirely.
	int w = 0;
	int h = 0;
	gf.measure("<B>ab", 1.0f, 1.0f, &w, &h);
	int w2 = 0;
	gf.measure("ab", 1.0f, 1.0f, &w2, &h);
	CHECK(w == w2, "tags measure zero width");
}

void test_compiler_health_and_order(const fnt_font_t *font) {
	HudFrameCompiler compiler;
	HudLayout layout;
	layout.health_rect = {10.0f, 20.0f, 100.0f, 8.0f, true};
	layout.heat_rect = {200.0f, 20.0f, 40.0f, 8.0f, true};
	compiler.configure(layout, font);

	HudFrameState state;
	state.ticks = 100;
	state.health_fraction = 0.5f;
	const HudDrawList &list = compiler.compile(state, 1024.0f, 768.0f);
	// Health at 0.5: the mid band fill + the wireframe border.
	CHECK(list.quads.size() == 2, "health emits fill + border");
	CHECK(list.quads[0].filled && !list.quads[1].filled,
			"fill first, border on top");
	CHECK(list.quads[0].color == layout.tag_middle,
			"0.5 health takes the middle band color");

	// Heat with an armed weapon.
	HudFrameState hot = state;
	hot.weapon.active = true;
	hot.weapon.heat = 0x8000;
	const HudDrawList &heat = compiler.compile(hot, 1024.0f, 768.0f);
	CHECK(heat.quads.size() == 4, "heat adds its border + fill");

	// The message ring: pushed lines draw until expiry.
	compiler.push_message("hello", 100);
	const HudDrawList &with_msg = compiler.compile(state, 1024.0f, 768.0f);
	CHECK(!with_msg.glyphs.empty(), "a live message emits glyphs");
	HudFrameState later = state;
	later.ticks = 100 + 930 + 1;
	const HudDrawList &expired = compiler.compile(later, 1024.0f, 768.0f);
	CHECK(expired.glyphs.empty(), "the 930-tick life expires the line");
}

void test_compiler_crosshair(const fnt_font_t *font) {
	HudFrameCompiler compiler;
	HudLayout layout;
	layout.crosshair_texture_valid = true;
	layout.crosshair_tex_w = 64;
	layout.crosshair_tex_h = 64;
	compiler.configure(layout, font);

	HudFrameState state;
	state.weapon.active = true;
	state.aimed_shot_available = false;
	const HudDrawList &list = compiler.compile(state, 1024.0f, 768.0f);
	// Five regions: four 5-vertex strips (3 tris) + one 4-vertex center
	// (2 tris) = 14 triangles.
	CHECK(list.tris.size() == 14, "the five tapered regions emit 14 triangles");

	HudFrameState aimed = state;
	aimed.aimed_shot_available = true;
	const HudDrawList &hidden = compiler.compile(aimed, 1024.0f, 768.0f);
	CHECK(hidden.tris.empty(), "a settled aimed shot hides the reticle");
}

// The hud_color_index scheme swap (the D-HUD-20 good-tier residue + the
// master overlay color). [orig: HUD_InitTeamColorTable @ 0x51f240 table
// immediates; the good-tier index test @ 0x5a3c9e (== 2 -> tagcolor_good,
// else g_hudColorTable[index]); the attach labels' g_hudActiveColor reads
// @ 0x5a362d; table[2] sourced per frame from the hudpos hud_textcolor
// @ 0x5a8100]
void test_compiler_hud_color_schemes(const fnt_font_t *font) {
	HudFrameCompiler compiler;
	HudLayout layout;
	compiler.configure(layout, font);

	HudFrameState state;
	opennova::hud::HudFriendlyTag tag;
	tag.screen_x = 200.0f;
	tag.screen_y = 100.0f;
	tag.dist_q16 = 100 << 16;
	tag.entity_id = 24;
	state.friendly_tags.push_back(tag);

	// Scheme 1 (green): the GOOD tier swaps off tagcolor_good onto table[1].
	state.hud_color_index = 1;
	const HudDrawList &green = compiler.compile(state, 1024.0f, 768.0f);
	const uint32_t green_tier = (217u << 24) | (0xFF00FF00u & 0xFFFFFFu);
	CHECK(!green.glyphs.empty() &&
					green.glyphs[0].color ==
							opennova::hud::half_bright_keep_alpha(green_tier),
			"scheme 1 puts the good tier on the table green");

	// The middle tier never scheme-swaps.
	state.friendly_tags[0].health_ratio_fp16 = 0x8000;
	const HudDrawList &mid = compiler.compile(state, 1024.0f, 768.0f);
	const uint32_t middle = (217u << 24) | (layout.tag_middle & 0xFFFFFFu);
	CHECK(!mid.glyphs.empty() &&
					mid.glyphs[0].color ==
							opennova::hud::half_bright_keep_alpha(middle),
			"the middle tier ignores the scheme");
	state.friendly_tags[0].health_ratio_fp16 = 0x10000;

	// Scheme 2 (the default) keeps tagcolor_good -- the retail identity.
	state.hud_color_index = 2;
	const HudDrawList &def = compiler.compile(state, 1024.0f, 768.0f);
	const uint32_t good = (217u << 24) | (layout.tag_good & 0xFFFFFFu);
	CHECK(!def.glyphs.empty() &&
					def.glyphs[0].color ==
							opennova::hud::half_bright_keep_alpha(good),
			"scheme 2 keeps the hudpos tagcolor_good");

	// The attach labels ride the master overlay color: the wire box frames at
	// the raw scheme color (light blue, scheme 3).
	state.friendly_tags.clear();
	opennova::hud::HudAttachLabel label;
	label.screen_x = 300.0f;
	label.screen_y = 60.0f;
	label.text = "Sit";
	label.nearest = true;
	state.attach_labels.push_back(label);
	state.hud_color_index = 3;
	const HudDrawList &blue = compiler.compile(state, 1024.0f, 768.0f);
	bool saw_blue_box = false;
	for (const opennova::hud::HudQuad &q : blue.quads) {
		saw_blue_box = saw_blue_box || q.color == 0xFF80A0FFu;
	}
	CHECK(saw_blue_box, "scheme 3 draws the attach box light blue");

	// Scheme 2 tracks the LIVE hudpos hud_textcolor (the per-frame table[2]
	// refresh), alpha forced FF like the init path.
	HudLayout tinted = layout;
	tinted.hud_text = 0x00123456u;
	compiler.configure(tinted, font);
	state.hud_color_index = 2;
	const HudDrawList &track = compiler.compile(state, 1024.0f, 768.0f);
	bool saw_tinted_box = false;
	for (const opennova::hud::HudQuad &q : track.quads) {
		saw_tinted_box = saw_tinted_box || q.color == 0xFF123456u;
	}
	CHECK(saw_tinted_box, "scheme 2 sources hud_textcolor live, alpha-forced");
}

void test_spinmap_projection_zoom_and_clip() {
	HudMinimapInput input;
	input.rect_x1 = 810.0f;
	input.rect_y1 = 552.0f;
	input.rect_x2 = 1020.0f;
	input.rect_y2 = 762.0f;
	input.player_heading_bam = 0;
	float x = 0.0f;
	float y = 0.0f;
	CHECK(opennova::hud::project_spinmap_point(input, 0, 0, false, x, y),
			"player projects inside spinmap");
	CHECK(x == 915.0f && y == 657.0f, "player projects at authored center");
	// World-per-pixel = zoom / (rect_w * 200) [orig: flt_7D2290 @0x5a5f40];
	// the default visible radius is 65536/400 = 163.84 world units.
	const float scale = 65536.0f / (210.0f * 200.0f);
	float fx = 0.0f;
	float fy = 0.0f;
	CHECK(opennova::hud::project_spinmap_point(input, 100 << 16, 0, false,
			fx, fy), "a 100-unit-forward point projects inside");
	CHECK(std::fabs(fx - 915.0f) < 0.01f &&
			std::fabs(fy - (657.0f - 100.0f / scale)) < 0.01f,
			"heading zero puts +X (ahead) above center");
	// Facing +X, mission +Y (the player's LEFT) lands left of center — the
	// witnessed transform negates mission Y before the rotation.
	// [orig: Terrain_FixedPointToWorldFloat @0x607060]
	float lx = 0.0f;
	float ly = 0.0f;
	CHECK(opennova::hud::project_spinmap_point(input, 0, 100 << 16, false,
			lx, ly), "a 100-unit-left point projects inside");
	CHECK(std::fabs(ly - 657.0f) < 0.01f &&
			std::fabs(lx - (915.0f - 100.0f / scale)) < 0.01f,
			"mission +Y draws on the player's left, not mirrored");
	// The backing/stencil fan sits four physical pixels inside the scaled
	// half-height: 105 - 4 = 101 px on this square authored rect.
	CHECK(!opennova::hud::project_spinmap_point(input, 200 << 16, 0,
			false, x, y), "a point past the stencil's world radius is clipped");
	CHECK(!opennova::hud::project_spinmap_point(input, 200 << 16, 0,
			true, x, y) && std::fabs(y - (657.0f - 101.0f)) < 0.01f,
			"edge clamp lands on the stencil circle");
	// Zoom OUT grows the world-extent value x1.15 toward 0x100000; IN shrinks
	// x0.85 toward 4096. [orig: cases 361/360 @0x49beaf/@0x49bcb0]
	CHECK(opennova::hud::spinmap_zoom_step(65536, 1) == 75366,
			"zoom out applies the retail x1.15 truncation");
	CHECK(opennova::hud::spinmap_zoom_step(65536, -1) == 55705,
			"zoom in applies the retail x0.85 truncation");
	CHECK(opennova::hud::spinmap_zoom_step(4096, -1) == 4096,
			"zoom clamps at 4096");
	CHECK(opennova::hud::spinmap_zoom_step(0x100000, 1) == 0x100000,
			"zoom clamps at 0x100000");

	// The live retail probe observes 00TRa's authored 0.61 as the
	// complementary 0.39 Q16 value. The per-frame scale divides that value
	// by the independently scaled rect HEIGHT (281 px at 1920x1080), not its
	// 375 px width.
	opennova::hud::HudMapControl control;
	control.set_mission_map_zoom(0.61f);
	CHECK(control.spawn_zoom_q16 == 25559 && control.zoom_q16 == 25559,
			"00TRa map_zoom initializes the retail complementary Q16 zoom");
	CHECK(control.spawn_big_zoom_q16 == 204472 &&
			control.big_zoom_q16 == 204472,
			"the big-map spawn zoom uses the same complementary factor");

	HudMinimapInput widescreen;
	widescreen.rect_x1 = 840.0f;
	widescreen.rect_y1 = 20.0f;
	widescreen.rect_x2 = 1040.0f;
	widescreen.rect_y2 = 220.0f;
	widescreen.surface_w = 1920.0f;
	widescreen.surface_h = 1080.0f;
	widescreen.zoom_q16 = 25559;
	float center_x = 0.0f;
	float center_y = 0.0f;
	float ahead_x = 0.0f;
	float ahead_y = 0.0f;
	CHECK(opennova::hud::project_spinmap_point(
			widescreen, 0, 0, false, center_x, center_y),
			"the widescreen player center remains visible");
	CHECK(opennova::hud::project_spinmap_point(
			widescreen, 50 << 16, 0, false, ahead_x, ahead_y),
			"a retail-probe comparison point remains visible");
	const float retail_scale = 25559.0f / (281.0f * 200.0f);
	CHECK(std::fabs(ahead_x - center_x) < 0.01f &&
			std::fabs((center_y - ahead_y) - 50.0f / retail_scale) < 0.01f,
			"widescreen projection uses the retail 281px-height scale");
	float edge_x = 0.0f;
	float edge_y = 0.0f;
	CHECK(!opennova::hud::project_spinmap_point(
			widescreen, 1000 << 16, 0, true, edge_x, edge_y),
			"a distant widescreen point clamps to the spinmap edge");
	CHECK(std::fabs(std::hypot(edge_x - center_x, edge_y - center_y) -
			136.5f) < 0.05f,
			"the corner stencil reaches the probed retail backing-ring radius");
}

void test_spinmap_mesh_layers_and_waypoint(const fnt_font_t *font) {
	HudFrameCompiler compiler;
	HudLayout layout;
	layout.spinmap_rect = {810.0f, 552.0f, 210.0f, 210.0f, true};
	layout.hud_text = 0xFF6080FFu;
	compiler.configure(layout, font);
	HudFrameState state;
	state.minimap.terrain.present = true;
	state.minimap.terrain.sector_count = 16;
	state.minimap.terrain.sector_rows = 16;
	state.minimap.terrain.sector_grid.fill(1);
	state.minimap.terrain.water_present = true;
	opennova::hud::HudMinimapMarker high;
	high.icon = 3;
	high.x = 32 << 16;
	high.color = 0xFF304080u;
	high.entity_known = 1;
	state.minimap.markers.push_back(high);
	opennova::hud::HudMinimapMarker low = high;
	low.icon = 10;
	low.x = -(32 << 16);
	state.minimap.markers.push_back(low);
	// An unknown-entity regular marker stays retained but does not draw.
	// [orig: render_minimap_slot_blip @0x5be4b8 entity[538] gate]
	opennova::hud::HudMinimapMarker hidden = high;
	hidden.entity_known = 0;
	state.minimap.markers.push_back(hidden);
	state.waypoint.present = true;
	state.waypoint.world_x = 1024 << 16;
	state.waypoint.world_z = 40 << 16; // 40 wu above -> "above" tricolor
	state.waypoint.distance_m = 1024;
	const HudDrawList &list = compiler.compile(state, 1024.0f, 768.0f);
	CHECK(list.map.visible, "an authored HUDSPINMAP rect compiles the spinmap");
	CHECK(list.map.backing.size() == 32, "spinmap backing is a 32-sided fan");
	CHECK(!list.map.terrain.empty(), "sector routing emits clipped terrain triangles");
	CHECK(list.map.terrain_water.size() == list.map.terrain.size(),
			"depthspin redraws the same clipped sector geometry as the colormap");
	// The terrain tint bakes the forced-opaque alpha and the fixed-function
	// MODULATE2X doubling. [orig: color|0xFF000000 @0x6071C0; _FFP.fx
	//  TSSColor MODULATE2X]
	CHECK(!list.map.terrain.empty() &&
			list.map.terrain[0].color == 0xFFC0C0C0u,
			"terrain tiles carry the doubled opaque tint");
	for (const auto &tri : list.map.terrain_water) {
		CHECK(tri.color == 0xFF16476Bu,
				"depthspin carries the capture-matched opaque shore color");
		for (const auto *v : {&tri.a, &tri.b, &tri.c}) {
			CHECK(v->u >= -1e-6f && v->u <= 127.0f / 256.0f + 1e-6f &&
					v->v >= -1e-6f && v->v <= 127.0f / 256.0f + 1e-6f,
					"sector id 1 selects depthspin's exact 127px top-left quadrant");
		}
	}
	// Two live markers + ONE waypoint tip cell + the compass ring sprite;
	// the unknown-entity marker is culled. The far waypoint clamps at the
	// disc, so the tip is the rotated chevron (cell 7).
	// [orig: the single strip-cell submit @0x59953d, cell = lodLevel]
	CHECK(list.map.sprites.size() == 4,
			"two live markers, the waypoint tip cell, and the compass ring");
	CHECK(list.map.sprites[0].layer <= list.map.sprites[1].layer,
			"marker sprites retain four-layer ordering");
	CHECK(list.map.sprites[0].color == 0xFF6080FFu &&
			list.map.sprites[1].color == 0xFF6080FFu,
			"ordinary TSDicon markers bake the saturating MODULATE2X RGB stage");
	CHECK(list.map.sprites[2].color == 0xFF7F5000u,
			"the waypoint tip stays net-raw after retail's halve/double cancellation");
	CHECK(list.map.sprites.back().texture == 1,
			"the compass ring rides texture slot 1");
	const auto &compass = list.map.sprites.back();
	CHECK(std::fabs(compass.u0 - 0.05f) < 1e-6f &&
			std::fabs(compass.v0 - 0.05f) < 1e-6f &&
			std::fabs(compass.u1 - 0.95f) < 1e-6f &&
			std::fabs(compass.v1 - 0.95f) < 1e-6f,
			"the compass crops five-percent texture padding like retail");
	CHECK(list.map.lines.size() == 1, "the waypoint state line is compiled");
	// Above-player waypoint: the orange state color, NET-RAW at submit (the
	// halve under the 2X-caps flag and the vertex re-double cancel).
	// [orig: sub_590970 0xFF7F5000; halve @0x599397 + 2c @0x5993c6]
	CHECK(list.map.lines[0].color == 0xFF7F5000u,
			"the state line carries the raw above-tricolor");
	// The distance label is LIVE by default — the retail global is BSS
	// (zero) and only an authored NONZERO SPINMAPWPDISTOFF suppresses it.
	// [orig: dword_27237C0 (.data, no file bytes); read @0x5a7a6a]
	bool default_distance_label = false;
	for (const auto &lab : list.map.labels) {
		if (std::string(lab.text) == "1.02k") default_distance_label = true;
	}
	CHECK(default_distance_label,
			"the ring-edge distance label compiles by default");
	for (const auto &tri : list.map.terrain) {
		for (const auto *v : {&tri.a, &tri.b, &tri.c}) {
			const float nx = (v->x - list.map.center_x) / list.map.radius_x;
			const float ny = (v->y - list.map.center_y) / list.map.radius_y;
			CHECK(nx * nx + ny * ny <= 1.02f,
					"terrain vertices stay inside the 32-sided clip");
		}
	}
	// An authored NONZERO SPINMAPWPDISTOFF suppresses the distance text.
	layout.spinmap_wp_dist_off = 17;
	compiler.update_layout(layout);
	{
		const HudDrawList &off = compiler.compile(state, 1024.0f, 768.0f);
		bool suppressed_label = false;
		for (const auto &lab : off.map.labels) {
			if (std::string(lab.text) == "1.02k") suppressed_label = true;
		}
		CHECK(!suppressed_label,
				"authored nonzero SPINMAPWPDISTOFF suppresses the label");
	}
	layout.spinmap_wp_dist_off = 0;
	compiler.update_layout(layout);
	// The M-cycle big map: mode 3 compiles the fullscreen north-up pass —
	// grid rules appear, the compass ring does not (mask 0xAF937 has
	// neither bit9 nor bit6), and the view centers on the design screen.
	// [orig: HUD_BuildMapOverlayView @0x5a7e10; HUD_CycleMapMode @0x520bc0]
	{
		HudFrameState big_state = state;
		big_state.minimap.map_mode = 3;
		big_state.minimap.player_x = 1035 << 16;
		big_state.minimap.player_heading_bam = 0x10000000;
		big_state.minimap.terrain.present = true;
		big_state.minimap.terrain.sector_count = 16;
		big_state.minimap.terrain.sector_rows = 16;
		big_state.minimap.terrain.sector_grid.fill(1);
		// A live-entity marker facing world-east (the -90-fold base): on the
		// north-up map its facing must be world-stable, not player-relative.
		big_state.minimap.markers.clear();
		opennova::hud::HudMinimapMarker east = high;
		east.x = big_state.minimap.player_x + (32 << 16);
		east.heading_bam = 0x40000000;
		big_state.minimap.markers.push_back(east);
		const HudDrawList &big = compiler.compile(big_state, 1024.0f, 768.0f);
		CHECK(big.map.visible, "the corner spinmap still compiles under a map mode");
		CHECK(big.big_map.visible, "mode 3 compiles the big-map pass");
		CHECK(std::fabs(big.big_map.center_x - 511.0f) < 2.0f &&
				std::fabs(big.big_map.center_y - 383.0f) < 2.0f,
				"the fullscreen pass centers on the design screen");
		bool big_has_compass = false;
		for (const auto &sprite : big.big_map.sprites) {
			if (sprite.texture == 1) big_has_compass = true;
		}
		CHECK(!big_has_compass, "the big map draws no compass ring (no bit6)");
		CHECK(big.big_map.lines_under.size() > 4,
				"the axis-aligned big map rules its 300-wu grid lines under "
				"the marker layer");
		// The blip rotates against the fixed north-up base, not the player
		// heading; the corner pass keeps the player-relative form.
		CHECK(!big.big_map.sprites.empty() &&
				std::fabs(big.big_map.sprites[0].rotation_rad) < 1e-4f,
				"north-up blips hold world-stable facing");
		CHECK(!big.map.sprites.empty() &&
				std::fabs(big.map.sprites[0].rotation_rad) > 1.0f,
				"corner blips stay player-relative");
		bool letter_label = false;
		bool player_column_letter = false;
		bool player_readout = false;
		for (const auto &lab : big.big_map.labels) {
			if (lab.text[0] >= 'A' && lab.text[0] <= 'Z' && lab.text[1] == 0) {
				letter_label = true;
				// The player stands at 1035 wu: the M-map gap containing that
				// point is lettered by the SAME -150-fold formula the
				// MAPCOORDS readout uses -> 'C'. [orig: the @0x5a5f40 grid
				// branch letters line_x - origin on the -150 lattice]
				if (std::string(lab.text) == "C") player_column_letter = true;
			}
			if (lab.text[0] == '(') {
				// The grid leg closes with the on-map player readout in the
				// large font: "(C,-1)" for this pose at origin zero.
				player_readout = std::string(lab.text) == "(C,-1)" &&
						lab.font == 1;
			}
		}
		CHECK(letter_label, "grid column letters ride the big-map label list");
		CHECK(player_column_letter,
				"the player's gap letter agrees with the MAPCOORDS formula");
		CHECK(player_readout,
				"the big map draws the large-font player readout");
		// Every grid label rides the LARGE slot.
		// [orig: HUD_DrawTextCentered_HalfBright(g_hudLabelFontLarge, ...)]
		bool all_large = true;
		for (const auto &lab : big.big_map.labels) {
			if (lab.font != 1) all_large = false;
		}
		CHECK(all_large, "big-map grid labels select the large font slot");
		// Mode 2: the 400x400 north-up window at (20,20).
		big_state.minimap.map_mode = 2;
		const HudDrawList &win = compiler.compile(big_state, 1024.0f, 768.0f);
		CHECK(win.big_map.visible &&
				std::fabs(win.big_map.center_x - 219.0f) < 2.0f &&
				std::fabs(win.big_map.center_y - 219.0f) < 2.0f,
				"mode 2 compiles the 20,20..419,419 window");
	}

	// Special-bank slots whose lifetime floored to zero stay claimed in the
	// bank but the render walk skips them.
	// [orig: MapOverlay_RenderAllByLayer @0x5be794]
	{
		HudFrameState expiry_state = state;
		expiry_state.minimap.markers.clear();
		expiry_state.waypoint.present = false;
		opennova::hud::HudMinimapMarker live_special;
		live_special.flags = 0x40;
		live_special.icon = 12;
		live_special.remaining_ticks = 5;
		expiry_state.minimap.markers.push_back(live_special);
		opennova::hud::HudMinimapMarker expired = live_special;
		expired.remaining_ticks = 0;
		expiry_state.minimap.markers.push_back(expired);
		const HudDrawList &pass = compiler.compile(expiry_state, 1024.0f,
				768.0f);
		int special_sprites = 0;
		for (const auto &sprite : pass.map.sprites) {
			if (sprite.half_w == 6.0f && sprite.texture == 0) ++special_sprites;
		}
		CHECK(special_sprites == 1,
				"an expired special slot is skipped by the draw pass");
	}

	// Footprint markers skip the icon quad for their polygon feed; markers
	// on the upright policy hold angle zero regardless of heading.
	// [orig: the Building leg @0x597a84 -> render_collision_wireframe
	//  @0x596800; the upright branches of draw_minimap_blip @0x597890]
	{
		HudFrameState fp_state = state;
		fp_state.minimap.markers.clear();
		fp_state.waypoint.present = false;
		opennova::hud::HudMinimapMarker building;
		building.handle = 0x2042;
		building.icon = 0;
		building.entity_known = 1;
		building.footprint = 1;
		fp_state.minimap.markers.push_back(building);
		opennova::hud::HudMinimapFootprint footprint;
		footprint.handle = 0x2042;
		footprint.fill_argb = 0xFFA0A0A0u;
		footprint.fill_xy_q16 = {20 << 16, -(4 << 16), 24 << 16,
				-(4 << 16), 22 << 16, 4 << 16};
		footprint.edge_xy_q16 = {20 << 16, -(4 << 16), 24 << 16, -(4 << 16)};
		opennova::hud::hud_minimap_finalize_footprint(footprint);
		fp_state.map_footprints.push_back(footprint);
		opennova::hud::HudMinimapMarker upright;
		upright.icon = 13;
		upright.x = -(10 << 16);
		upright.heading_bam = 0x30000000;
		upright.entity_known = 1;
		upright.rotate = 0;
		fp_state.minimap.markers.push_back(upright);
		const HudDrawList &fp_list = compiler.compile(fp_state, 1024.0f,
				768.0f);
		CHECK(!fp_list.map.overlays.empty(),
				"a footprint marker fills its polygon feed");
		CHECK(fp_list.map.sprites.size() == 2,
				"the footprint marker draws no icon sprite (badge + compass remain)");
		bool found_upright = false;
		for (const auto &sprite : fp_list.map.sprites) {
			if (sprite.texture == 0 &&
					std::fabs(sprite.rotation_rad) < 1e-4f) {
				found_upright = true;
			}
		}
		CHECK(found_upright, "an upright-policy marker holds angle zero");
		bool found_edge = false;
		for (const auto &line : fp_list.map.lines_under) {
			if (line.color == 0x80000000u) found_edge = true;
		}
		CHECK(!found_edge,
				"the completed retail footprint pass leaves no observable "
				"black silhouette stroke");
	}

	// The grid label compiles at the authored MAPCOORDS position once its
	// suppressor clears; the column letters ride the base-26 formatter.
	// [orig: sub_59CB40; HUD_FormatGridCoordinate @0x598600]
	layout.map_coords_x = 530.0f;
	layout.map_coords_y = 720.0f;
	layout.map_coords_off = 0;
	compiler.update_layout(layout);
	{
		HudFrameState grid_state = state;
		grid_state.minimap.player_x = 1035 << 16; // (1035-150)/300 -> C
		grid_state.minimap.player_y = -(180 << 16);
		const HudDrawList &grid = compiler.compile(grid_state, 1024.0f,
				768.0f);
		bool found = false;
		for (const auto &lab : grid.map.labels) {
			if (std::string(lab.text) == "(C,-1)" && lab.align == 1) {
				found = true;
			}
		}
		CHECK(found, "the grid label formats (C,-1) at origin zero, right-aligned");
	}
}

// Friendly tags (D-HUD-20) [orig: HUD_DrawEntityLabel @ 0x5a39b0]: the
// witnessed gates, tier colors, distance alpha, fallback name, medic plate,
// and the BRIEF tick form, over the default (retail) tag colors.
void test_compiler_friendly_tags(const fnt_font_t *font) {
	HudFrameCompiler compiler;
	HudLayout layout;
	compiler.configure(layout, font);

	HudFrameState state;
	opennova::hud::HudFriendlyTag tag;
	tag.screen_x = 200.0f;
	tag.screen_y = 100.0f;
	tag.dist_q16 = 100 << 16;
	tag.entity_id = 24; // the fallback table's "SGT  Brown"
	state.friendly_tags.push_back(tag);

	// FULL (the boot default): centered half-bright text of the fallback name.
	const HudDrawList &full = compiler.compile(state, 1024.0f, 768.0f);
	CHECK(full.glyphs.size() == 11,
			"the fallback '^SGT  Brown' lays out 11 glyphs");
	// 100 m -> alpha 255 - 192*50/250 = 217; good tier = tagcolor_good with
	// the alpha-preserving half-bright fold.
	const uint32_t good = (217u << 24) | (layout.tag_good & 0xFFFFFFu);
	CHECK(!full.glyphs.empty() &&
					full.glyphs[0].color ==
							opennova::hud::half_bright_keep_alpha(good),
			"good tier rides tagcolor_good + the distance alpha");
	// Centered on x=200: 11 glyphs at advance 9 minus the trailing pad = 98
	// wide -> cursor 151 -> the -0.5 vertex offset.
	CHECK(!full.glyphs.empty() && full.glyphs[0].x_top_left == 150.5f,
			"the text centers on the projected x");
	CHECK(full.quads.empty(), "no medic plate without the medic flag");

	// The health tiers swap the color.
	state.friendly_tags[0].health_ratio_fp16 = 0x8000;
	const HudDrawList &mid = compiler.compile(state, 1024.0f, 768.0f);
	const uint32_t middle = (217u << 24) | (layout.tag_middle & 0xFFFFFFu);
	CHECK(!mid.glyphs.empty() &&
					mid.glyphs[0].color ==
							opennova::hud::half_bright_keep_alpha(middle),
			"the middle tier rides tagcolor_middle");
	state.friendly_tags[0].health_ratio_fp16 = 0x10000;

	// The medic plate: white square + the two red cross bars, left of the text.
	state.friendly_tags[0].medic = true;
	const HudDrawList &medic = compiler.compile(state, 1024.0f, 768.0f);
	CHECK(medic.quads.size() == 3, "the medic plate is three untextured quads");
	if (medic.quads.size() == 3) {
		CHECK(medic.quads[0].color == ((217u << 24) | 0xFFFFFFu),
				"the plate is white at the tag alpha");
		CHECK(medic.quads[1].color == ((217u << 24) | 0xFF0000u),
				"the cross bars are red at the tag alpha");
		// left = 200 - (98/2 + 16) - 0.5, a fontH/2 = 8 px square at the text top.
		CHECK(medic.quads[0].x0 == 134.5f && medic.quads[0].y0 == 91.5f,
				"the plate sits one fontH left of the text");
		CHECK(medic.quads[0].x1 - medic.quads[0].x0 == 8.0f,
				"the plate is a fontH/2 square");
	}
	state.friendly_tags[0].medic = false;

	// Gates: too close, fogged, FARBRIEF far, OFF.
	state.friendly_tags[0].dist_q16 = 0x4000;
	CHECK(compiler.compile(state, 1024.0f, 768.0f).glyphs.empty(),
			"under 0.5 u nothing draws");
	state.friendly_tags[0].dist_q16 = 100 << 16;
	state.fog_dist_q16 = 50 << 16;
	CHECK(compiler.compile(state, 1024.0f, 768.0f).glyphs.empty(),
			"the fog cull hides the tag");
	state.fog_dist_q16 = INT32_MAX;
	state.friendly_tag_mode = 1;
	state.friendly_tags[0].dist_q16 = 400 << 16;
	state.fog_dist_q16 = 1000 << 16;
	CHECK(compiler.compile(state, 1024.0f, 768.0f).glyphs.empty(),
			"FARBRIEF draws nothing past 300 m");
	state.friendly_tags[0].dist_q16 = 100 << 16;
	CHECK(!compiler.compile(state, 1024.0f, 768.0f).glyphs.empty(),
			"FARBRIEF draws text under 300 m");
	state.friendly_tag_mode = 0;
	CHECK(compiler.compile(state, 1024.0f, 768.0f).glyphs.empty(),
			"OFF draws nothing");

	// BRIEF: the three 1-px tick lines replace the text.
	state.friendly_tag_mode = 3;
	const HudDrawList &brief = compiler.compile(state, 1024.0f, 768.0f);
	CHECK(brief.glyphs.empty(), "BRIEF draws no text");
	CHECK(brief.lines.size() == 3, "BRIEF draws the three tick lines");
	if (brief.lines.size() == 3) {
		CHECK(brief.lines[1].x0 == 200.0f && brief.lines[1].y0 == 96.0f &&
						brief.lines[1].y1 == 104.0f,
				"the tick spans +-fontH/4 around the projected point");
	}

	// A slot entry with an empty callsign draws the single bar.
	state.friendly_tag_mode = 2;
	state.friendly_tags[0].player = true;
	state.friendly_tags[0].name.clear();
	const HudDrawList &bar = compiler.compile(state, 1024.0f, 768.0f);
	CHECK(bar.glyphs.empty() && bar.lines.size() == 1,
			"an empty player callsign draws the bar form");

	// The speaking pulse lifts the color.
	state.friendly_tags[0].player = false;
	state.friendly_tags[0].speaking = true;
	state.speaking_level255 = 255;
	const HudDrawList &speak = compiler.compile(state, 1024.0f, 768.0f);
	const uint32_t pulsed = (217u << 24) |
			(opennova::hud::friendly_tag_speaking_blend(
					 layout.tag_good, 255) &
					0xFFFFFFu);
	CHECK(!speak.glyphs.empty() &&
					speak.glyphs[0].color ==
							opennova::hud::half_bright_keep_alpha(pulsed),
			"the speaking tag pulses toward white");
}

// The overlay label fonts: friendly tags draw with the NORMAL label font,
// attach labels with the BOLD one, both at the slot scale, each in its own
// draw-list page namespace. [orig: HUD_InitAllFonts @ 0x51ee20; tag font
// g_hudLabelFont @ 0x5a3a0c; attach font (the bold slot) @ 0x5a3680; the slot
// scales enter the draw/measure/char-height helpers @ 0x580680/@ 0x580ab0/
// @ 0x580a80]
void test_compiler_label_fonts(const fnt_font_t *font) {
	HudFrameCompiler compiler;
	HudLayout layout;
	compiler.configure(layout, font);
	compiler.configure_label_fonts(font, font, font, 2.0f, 2.0f);

	HudFrameState state;
	opennova::hud::HudFriendlyTag tag;
	tag.screen_x = 200.0f;
	tag.screen_y = 100.0f;
	tag.dist_q16 = 100 << 16;
	tag.entity_id = 24; // "^SGT  Brown", 11 glyphs
	state.friendly_tags.push_back(tag);
	opennova::hud::HudAttachLabel label;
	label.screen_x = 300.0f;
	label.screen_y = 60.0f;
	label.text = "Sit";
	state.attach_labels.push_back(label);

	const HudDrawList &list = compiler.compile(state, 1024.0f, 768.0f);
	size_t normal_glyphs = 0;
	size_t bold_glyphs = 0;
	const opennova::hud::GameFontQuad *first_tag_glyph = nullptr;
	for (const opennova::hud::GameFontQuad &g : list.glyphs) {
		if (g.page ==
				static_cast<uint32_t>(opennova::hud::kHudFontSlotLabel) *
						FNT_MAX_PAGES) {
			if (first_tag_glyph == nullptr) {
				first_tag_glyph = &g;
			}
			++normal_glyphs;
		}
		if (g.page ==
				static_cast<uint32_t>(opennova::hud::kHudFontSlotLabelBold) *
						FNT_MAX_PAGES) {
			++bold_glyphs;
		}
	}
	CHECK(normal_glyphs == 11,
			"tag glyphs ride the normal label font's page namespace");
	CHECK(bold_glyphs == 3,
			"attach glyphs ride the bold label font's page namespace");
	// Scale 2: the 98-wide line centers as 200 - 98 -> 101.5 after the -0.5
	// offset; fontH = 16*2, text top = y - fontH/2 -> 83.5.
	CHECK(first_tag_glyph != nullptr &&
					first_tag_glyph->x_top_left == 101.5f &&
					first_tag_glyph->y_top == 83.5f,
			"the slot scale doubles the centered layout metrics");

	// Null label fonts fall back to the hudpos font at scale 1 (page 0).
	compiler.configure_label_fonts(nullptr, nullptr, nullptr, 1.0f, 1.0f);
	const HudDrawList &fallback = compiler.compile(state, 1024.0f, 768.0f);
	bool all_page0 = !fallback.glyphs.empty();
	for (const opennova::hud::GameFontQuad &g : fallback.glyphs) {
		all_page0 = all_page0 && g.page == 0;
	}
	CHECK(all_page0, "absent label fonts fall back to the hudpos font");
}

} // namespace

int main() {
	fnt_font_t font = make_font();
	test_measure_advance_and_trailing_pad(&font);
	test_layout_pages_bold_underline(&font);
	test_format_tags(&font);
	test_compiler_health_and_order(&font);
	test_compiler_crosshair(&font);
	test_compiler_hud_color_schemes(&font);
	test_spinmap_projection_zoom_and_clip();
	test_spinmap_mesh_layers_and_waypoint(&font);
	test_compiler_friendly_tags(&font);
	test_compiler_label_fonts(&font);
	fnt_free(&font);
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("hud_frame_compiler_test OK\n");
	return 0;
}
