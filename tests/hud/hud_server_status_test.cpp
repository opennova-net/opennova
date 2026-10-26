// The authority's server-status page (hud/hud_server_status.h): the throttle,
// the roster grid and cells (the team colours, the grey and dimmed states,
// the idle column, the empty and past-the-limit cells, the dedicated host's
// slot shift), the class letters, the player score list's rows, and the
// compile: the black clear, the roster, the server line, the team block, the
// clock, the bottom row, the ticker, the console lines, the quit dialog (on
// the page and in the scene frame's gameplay overlays) and the score list.
// [orig: Server_DrawStatusScreen @0x50a2d0; HUD_DrawPlayerScoreList
//  @0x500300; UI_DrawQuitConfirmDialog @0x5b8eb0;
//  HUD_DrawServerConsoleLines @0x5ba0a0]
#include <runtime/hud/game_font.h>
#include <runtime/hud/hud_frame.h>
#include <runtime/hud/hud_math.h>
#include <runtime/hud/hud_server_status.h>

#include "common/test_font.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace opennova::hud;
using opennova::fnt::FNT_MAX_PAGES;
using opennova::fnt::fnt_font_t;

namespace {

int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

constexpr uint32_t page_of(int slot) { return static_cast<uint32_t>(slot) * FNT_MAX_PAGES; }

// The glyph runs of one font slot: consecutive glyphs on one baseline.
struct Run {
	float x = 0.0f;
	float y = 0.0f;
	size_t count = 0;
	uint32_t color = 0;
};
std::vector<Run> runs_on(const HudDrawList &list, int slot) {
	std::vector<Run> out;
	float last_right = -1e9f;
	for (const GameFontQuad &g : list.glyphs) {
		if (g.page != page_of(slot)) continue;
		if (!out.empty() && out.back().y == g.y_top && g.x_top_left >= last_right - 0.5f &&
				g.x_top_left <= last_right + 2.0f) {
			++out.back().count;
		} else {
			out.push_back({g.x_top_left, g.y_top, 1, g.color});
		}
		last_right = g.x_top_right;
	}
	return out;
}
bool near(float a, float b) { return std::fabs(a - b) <= 1.0f; }
const Run *run_at(const std::vector<Run> &runs, float x, float y) {
	for (const Run &r : runs)
		if (near(r.x, x) && near(r.y, y)) return &r;
	return nullptr;
}
const Run *run_at_y(const std::vector<Run> &runs, float y) {
	for (const Run &r : runs)
		if (near(r.y, y)) return &r;
	return nullptr;
}

ServerStatusPageState listen_page(int capacity) {
	ServerStatusPageState page;
	page.mp_session_peer = true;
	page.capacity = capacity;
	page.slot_limit = capacity;
	page.slots.assign(static_cast<size_t>(capacity), ServerStatusSlot{});
	page.text.empty_slot = "OPEN";
	page.text.server_lan = "LAN";
	page.text.server_novaworld = "NW";
	page.text.game_type_abbreviation = "TDM";
	page.text.frames = "FR";
	page.text.cpu = "CPU";
	page.text.start_timer = "ST";
	page.text.total_logins = "TL";
	page.text.current_logins = "CL";
	page.text.team_wins = "WINS";
	page.text.team_scores = "SCORE";
	page.text.team1 = "T1";
	page.text.team2 = "T2";
	page.text.ties = "TIE";
	page.text.score_list_title = "LIST";
	page.server_name = "srv";
	return page;
}

void test_throttle() {
	uint32_t last = 0;
	// The first draw seeds the stamp one second back and draws.
	CHECK(server_status_page_due(&last, 50000, true));
	CHECK(last == 50000);
	CHECK(!server_status_page_due(&last, 50199, true));
	CHECK(server_status_page_due(&last, 50200, true));
	// An inactive window waits 10 s.
	CHECK(!server_status_page_due(&last, 50200 + 9999, false));
	CHECK(server_status_page_due(&last, 50200 + 10000, false));
	// The peek answers the same without moving the stamp.
	uint32_t peek = 0;
	CHECK(server_status_page_due_at(peek, 50000, true));
	CHECK(!server_status_page_due_at(peek, 50000, false));
	peek = 50000;
	CHECK(!server_status_page_due_at(peek, 50199, true));
	CHECK(server_status_page_due_at(peek, 50200, true));
	CHECK(server_status_page_due_at(peek, 50200, true) && peek == 50000);
	CHECK(!server_status_page_due_at(peek, 50000 + 9999, false));
	CHECK(server_status_page_due_at(peek, 50000 + 10000, false));
}

void test_grid_cells_and_codes() {
	// 8 slots: one column doubled to two, four rows 135 apart, columns 505 apart.
	ServerStatusRosterGrid g = server_status_roster_grid(8);
	CHECK(g.columns == 2 && g.rows == 4 && g.column_step == 505 && g.row_step == 135);
	// 64 slots: three columns of 22 rows.
	g = server_status_roster_grid(64);
	CHECK(g.columns == 3 && g.rows == 22 && g.column_step == 336 && g.row_step == 24);
	// 90 slots: three columns (not doubled) of 30.
	g = server_status_roster_grid(90);
	CHECK(g.columns == 3 && g.rows == 30);
	CHECK(server_status_class_code(5) == 'M' && server_status_class_code(8) == 'R' &&
			server_status_class_code(1) == 'd' && server_status_class_code(0) == '?' &&
			server_status_class_code(10) == '?');

	ServerStatusPageState page = listen_page(4);
	ServerStatusSlot &host = page.slots[0];
	host.active = true;
	host.local = true;
	host.in_game = true;
	host.team = 1;
	host.class_word = 8;
	host.name = "Host";
	host.idle_seconds = 50; // the host's own slot never shows an idle column
	ServerStatusRosterCell c = server_status_roster_cell(page, 0, 0xFFFFFFFFu);
	CHECK(c.text == "#00 R:Host" && c.color == 0xFF00AFFFu);
	ServerStatusSlot &joiner = page.slots[1];
	joiner.active = true;
	joiner.in_game = true;
	joiner.team = 2;
	joiner.class_word = 5;
	joiner.name = "Joe";
	joiner.idle_seconds = 1;
	c = server_status_roster_cell(page, 1, 0);
	CHECK(c.text == "#01 M:Joe" && c.color == 0xFFFF0000u);
	joiner.idle_seconds = 2;
	CHECK(server_status_roster_cell(page, 1, 0).text == "#01 M:Joe (2)");
	// A dead entity dims the three base team colours; team 3 keeps its own.
	joiner.entity_dead = true;
	CHECK(server_status_roster_cell(page, 1, 0).color == 0xFFC01010u);
	joiner.team = 0;
	CHECK(server_status_roster_cell(page, 1, 0).color == 0xFF00A000u);
	joiner.team = 3;
	CHECK(server_status_roster_cell(page, 1, 0).color == 0xFFFFFF00u);
	// Not in game: grey whatever the team.
	joiner.in_game = false;
	CHECK(server_status_roster_cell(page, 1, 0).color == 0xFFA0A0A0u);
	// The status word's rights colours (its writers only store 0).
	joiner.in_game = true;
	joiner.entity_dead = false;
	joiner.status_word = 1u | 0x02u;
	CHECK(server_status_roster_cell(page, 1, 0).color == (0xFFC08000u | 0x3F0000u));
	joiner.status_word = 0;
	// An empty slot and a slot past the limit.
	c = server_status_roster_cell(page, 2, 0x12345678u);
	CHECK(c.text == "#02 OPEN" && c.color == 0xFF505050u);
	c = server_status_roster_cell(page, 4, 0x12345678u);
	CHECK(c.text.empty() && c.color == 0x12345678u);
	page.slot_limit = 3;
	CHECK(server_status_roster_cell(page, 3, 7u).text.empty());
}

void test_score_rows() {
	ServerStatusPageState page = listen_page(20);
	page.game_type = 0x10000u;
	page.slots[0].active = true;
	page.slots[0].local = true;
	page.slots[1].active = true;
	page.slots[1].team = 1;
	page.slots[1].points = 7;
	page.slots[1].name = "a";
	page.slots[17].active = true;
	page.slots[17].team = 2;
	page.slots[17].spectator = true;
	page.slots[17].loading = true;
	page.slots[17].name = "s";
	CHECK(!server_status_score_row(page, 0).drawn); // the host's own slot
	ServerStatusScoreRow r = server_status_score_row(page, 1);
	CHECK(r.drawn && r.x == 130 && r.y == 210 && r.text == " 1   7 a" && r.color == 0xFF80A0FFu);
	r = server_status_score_row(page, 2);
	CHECK(r.drawn && r.text == "--" && r.color == 0x0000FF00u && r.y == 240);
	r = server_status_score_row(page, 17);
	CHECK(r.x == 330 && r.y == 210 && r.text == "17 --- s" && r.color == 0xFFFF40FFu);
	page.slots[17].loading = false;
	CHECK(server_status_score_row(page, 17).color == 0xFFFF5050u);
	// KOTH: the hill seconds; CTF: the captures; a non-team type keeps the
	// default colour.
	page.game_type = 0x10001u;
	page.slots[1].objective_seconds = 125;
	CHECK(server_status_score_row(page, 1).text == " 1  2:05 a");
	page.game_type = 0x10004u;
	page.slots[1].flag_captures = 3;
	CHECK(server_status_score_row(page, 1).text == " 1  3 a");
	page.game_type = 0u;
	CHECK(server_status_score_row(page, 1).color == 0x0000FF00u);
}

void test_page_compile(const fnt_font_t *font) {
	HudLayout layout;
	HudFrameCompiler compiler;
	compiler.configure(layout, font);
	compiler.configure_label_fonts(font, font, font, 1.0f, 1.0f, font);
	HudFrameState state;
	ServerStatusPageState page = listen_page(8);
	page.game_type = 0x10000u;
	page.round_wins_team1 = 2;
	page.round_wins_team2 = 1;
	page.rounds_played = 4;
	page.team_points[0] = 30;
	page.team_points[1] = 20;
	page.round_time_remaining = 62 * 3725; // 1:02:05
	page.frames = 61;
	page.cpu_percent = 12;
	page.pre_round_delay = 75;
	page.total_logins = 3;
	page.slots[0].active = true;
	page.slots[0].local = true;
	page.slots[0].in_game = true;
	page.slots[0].team = 1;
	page.slots[0].class_word = 8;
	page.slots[0].name = "Host";
	page.slots[5].active = true;
	page.slots[5].name = "Joe";
	for (int i = 0; i < 5; ++i) compiler.push_feed_line("line" + std::to_string(i), 0u, 0);

	CHECK(compiler.compile_server_status_page(state, page, 10000, true, 1024.0f, 768.0f));
	const HudDrawList &list = compiler.server_status_page_list();
	// The black clear comes first and covers the surface.
	CHECK(!list.quads.empty() && list.quads[0].filled && list.quads[0].color == 0xFF000000u &&
			list.quads[0].x0 == 0.0f && list.quads[0].x1 == 1024.0f && list.quads[0].y1 == 768.0f);
	const std::vector<Run> bold = runs_on(list, kHudFontSlotLabelBold);
	// The roster: two columns of four from (7, 52), 135 px rows, 505 px columns.
	const Run *first = run_at(bold, 7.0f, 52.0f);
	CHECK(first != nullptr && first->color == half_bright_argb(0xFF00AFFFu));
	const Run *open = run_at(bold, 7.0f, 52.0f + 135.0f);
	CHECK(open != nullptr && open->color == half_bright_argb(0xFF505050u));
	// Slot 5 (column 1, row 1) is active but not in game: grey.
	const Run *joe = run_at(bold, 512.0f, 52.0f + 135.0f);
	CHECK(joe != nullptr && joe->color == half_bright_argb(0xFFA0A0A0u));
	// The team block: the wins, the ties and the points.
	CHECK(run_at_y(bold, 130.0f) != nullptr && run_at_y(bold, 100.0f) != nullptr);
	CHECK(run_at_y(bold, 190.0f) != nullptr); // ties
	CHECK(run_at_y(bold, 290.0f) != nullptr); // team 1's points
	// The clock right-aligned at 1000 and the server line centred at 512.
	const Run *line10 = run_at_y(bold, 10.0f);
	CHECK(line10 != nullptr);
	// The bottom row at y 704: frames "FR 63+" at x 16.
	CHECK(run_at(bold, 16.0f, 704.0f) != nullptr);
	// The ticker at (700, 736) on the first draw.
	CHECK(run_at(bold, 700.0f, 736.0f) != nullptr);
	// The console lines: the four newest SYSTEM lines, oldest on top, a zero
	// colour drawn white.
	const Run *c0 = run_at(bold, 10.0f, 608.0f);
	const Run *c3 = run_at(bold, 10.0f, 680.0f);
	CHECK(c0 != nullptr && c3 != nullptr && c0->color == half_bright_argb(0xFFFFFFFFu));

	// The throttle keeps the list; the next due draw moves the ticker 5 px.
	const size_t glyphs_before = list.glyphs.size();
	CHECK(!compiler.compile_server_status_page(state, page, 10100, true, 1024.0f, 768.0f));
	CHECK(compiler.server_status_page_list().glyphs.size() == glyphs_before);
	CHECK(compiler.compile_server_status_page(state, page, 10200, true, 1024.0f, 768.0f));
	CHECK(run_at(runs_on(compiler.server_status_page_list(), kHudFontSlotLabelBold), 705.0f,
			736.0f) != nullptr);

	// The quit dialog on the page, in the Impact38 slot centred on 512 at 364.
	state.quit_dialog_open = true;
	state.quit_dialog_text = "Q";
	CHECK(compiler.compile_server_status_page(state, page, 10400, true, 1024.0f, 768.0f));
	const std::vector<Run> impact = runs_on(compiler.server_status_page_list(), kHudFontSlotImpact38);
	CHECK(impact.size() == 1 && near(impact[0].y, 364.0f) && impact[0].x > 500.0f &&
			impact[0].x < 512.0f);
	// ... and in the scene frame's gameplay overlays, except at the blank level.
	CHECK(runs_on(compiler.compile(state, 1024.0f, 768.0f), kHudFontSlotImpact38).size() == 1);
	state.hud_detail_level = 3;
	CHECK(runs_on(compiler.compile(state, 1024.0f, 768.0f), kHudFontSlotImpact38).empty());
	state.hud_detail_level = 0;
	state.quit_dialog_open = false;

	// The score list: its titled box and a row per slot but the host's own.
	page.score_list_open = true;
	CHECK(compiler.compile_server_status_page(state, page, 10600, true, 1024.0f, 768.0f));
	const std::vector<Run> with_list = runs_on(compiler.server_status_page_list(),
			kHudFontSlotLabelBold);
	CHECK(run_at(with_list, 130.0f, 210.0f) != nullptr);  // slot 1 "--"
	CHECK(run_at(with_list, 130.0f, 180.0f) == nullptr);  // slot 0, the host's
	CHECK(run_at(with_list, 115.0f, 102.0f) != nullptr);  // the STRSRV23 title

	// A dedicated host lists slot i + 1 and shows the CHAT ring, tags off.
	page.mp_session_peer = false;
	page.score_list_open = false;
	compiler.push_chat_line("chat", 0xFF00FF00u, 0);
	CHECK(compiler.compile_server_status_page(state, page, 10800, true, 1024.0f, 768.0f));
	const std::vector<Run> ded = runs_on(compiler.server_status_page_list(), kHudFontSlotLabelBold);
	const Run *ded_first = run_at(ded, 7.0f, 52.0f);
	CHECK(ded_first != nullptr && ded_first->color == half_bright_argb(0xFF505050u));
	CHECK(run_at(ded, 10.0f, 680.0f) != nullptr && run_at(ded, 10.0f, 608.0f) == nullptr);
}

// A dedicated host's round end: the page's end-round leg draws the overlay's
// lines in the snapshot twin g_HUDColors.active. No gameplay pass precedes the
// page, so the twin keeps the HUD init's seed, table[index] | 0xFF000000 with
// entry 2 the 0xFF010101 placeholder, and at the default index the half-bright
// draw lays the lines black on the page's black clear: retail's Serve Only host
// shows no banner (the twin read 0xFF010101 there through a round end while the
// transition's first-pass latch g_EndRoundOverlayFirstPass set). The quit dialog reads the
// same twin; a listen host's page reads the twin its scene frames restamped.
// [orig: HUD_DrawEndRoundStatsOverlay @0x5B7CD0 (`mov ebp, g_HUDColors.active`
//  @0x5B7CE6); UI_DrawQuitConfirmDialog @0x5B8EB0 (@0x5B8EB5);
//  HUD_InitTeamColorTable @0x51F240 (entry 2 @0x51F24F, the twin @0x51F2DE),
//  run by Game_StartMission's first load @0x5258C1 after HUD_InitOverlaySystem
//  @0x525733; HUD_DrawTextCentered_HalfBright @0x580680 (@0x58068F, @0x5806AB);
//  the restamp HUD_DrawGameTimerOverlay @0x59CCD0]
void test_dedicated_host_twin(const fnt_font_t *font) {
	HudLayout layout;
	const auto make = [&](HudFrameCompiler &compiler) {
		compiler.configure(layout, font);
		compiler.configure_label_fonts(font, font, font, 1.0f, 1.0f, font);
	};
	const auto impact_colors = [](const HudDrawList &list, uint32_t want, size_t lines) {
		const std::vector<Run> impact = runs_on(list, kHudFontSlotImpact38);
		bool ok = impact.size() == lines;
		for (const Run &r : impact) ok = ok && r.color == want;
		return ok;
	};
	HudFrameState state;
	state.round_over = true;
	state.end_round.shown = true;
	state.end_round.lines = {{"D", 300}, {"S", 414}};
	ServerStatusPageState page = listen_page(4);
	page.mp_session_peer = false;
	page.game_type = 0x10000u;
	CHECK(half_bright_argb(0xFF010101u) == 0xFF000000u);
	{
		// The default index 2: the placeholder, black.
		HudFrameCompiler compiler;
		make(compiler);
		CHECK(compiler.compile_server_status_page(state, page, 10000, true, 1024.0f, 768.0f));
		CHECK(impact_colors(compiler.server_status_page_list(), 0xFF000000u, 2));
		// The page draws nothing to restamp it: the next due page keeps the seed.
		CHECK(compiler.compile_server_status_page(state, page, 10200, true, 1024.0f, 768.0f));
		CHECK(impact_colors(compiler.server_status_page_list(), 0xFF000000u, 2));
		// The quit dialog's text, the same twin.
		state.end_round.shown = false;
		state.quit_dialog_open = true;
		state.quit_dialog_text = "Q";
		CHECK(compiler.compile_server_status_page(state, page, 10400, true, 1024.0f, 768.0f));
		CHECK(impact_colors(compiler.server_status_page_list(), 0xFF000000u, 1));
		state.quit_dialog_open = false;
		state.end_round.shown = true;
	}
	{
		// Another index seeds its own entry: yellow at 4.
		HudFrameCompiler compiler;
		make(compiler);
		state.hud_color_index = 4;
		CHECK(compiler.compile_server_status_page(state, page, 10000, true, 1024.0f, 768.0f));
		CHECK(impact_colors(compiler.server_status_page_list(), half_bright_argb(0xFFF0F000u), 2));
		state.hud_color_index = 2;
	}
	{
		// The hudcolor cycle stores the palette entry as it stands (D-NET-NEW-L8-42): no
		// gameplay pass has refreshed a dedicated host's entry 2, so the cycle round to 2
		// stores the HUD init's placeholder, black [orig: case 10 @0x49AFC7, the store
		// @0x49AFE7; the refresh HUD_RenderAllOverlays @0x5A810C].
		HudFrameCompiler compiler;
		make(compiler);
		state.hud_color_index = 1;
		CHECK(compiler.compile_server_status_page(state, page, 10000, true, 1024.0f, 768.0f));
		CHECK(impact_colors(compiler.server_status_page_list(), half_bright_argb(0xFF00FF00u), 2));
		state.hud_color_index = 2;
		CHECK(compiler.compile_server_status_page(state, page, 10200, true, 1024.0f, 768.0f));
		CHECK(impact_colors(compiler.server_status_page_list(), 0xFF000000u, 2));
	}
	{
		// A listen host's scene frame (before the round-over latch) refreshed entry 2 from the
		// hudpos text colour, so the same cycle stores that colour.
		HudFrameCompiler compiler;
		make(compiler);
		state.hud_color_index = 1;
		state.end_round.shown = false;
		state.round_over = false;
		(void)compiler.compile(state, 1024.0f, 768.0f);
		state.round_over = true;
		state.end_round.shown = true;
		state.hud_color_index = 2;
		page.mp_session_peer = true;
		CHECK(compiler.compile_server_status_page(state, page, 10000, true, 1024.0f, 768.0f));
		CHECK(impact_colors(compiler.server_status_page_list(), half_bright_argb(layout.hud_text), 2));
		page.mp_session_peer = false;
	}
	{
		// A scene frame the pass returns at its head refreshes nothing: under the round-over
		// latch, or while the white hit flash burns, the cycle to 2 still stores the
		// placeholder (V-ROLL-AU N1) [orig: HUD_RenderAllOverlays @0x5A8084 / @0x5A8098, both
		// ahead of the store @0x5A810C].
		for (int arm = 0; arm < 2; ++arm) {
			HudFrameCompiler compiler;
			make(compiler);
			state.hud_color_index = 1;
			state.end_round.shown = false;
			state.round_over = arm == 0;
			state.combat.white_flash = arm == 1;
			(void)compiler.compile(state, 1024.0f, 768.0f);
			state.combat.white_flash = false;
			state.round_over = true;
			state.end_round.shown = true;
			state.hud_color_index = 2;
			page.mp_session_peer = true;
			CHECK(compiler.compile_server_status_page(state, page, 10000, true, 1024.0f, 768.0f));
			CHECK(impact_colors(compiler.server_status_page_list(), 0xFF000000u, 2));
			page.mp_session_peer = false;
		}
	}
	{
		// A listen host's scene frame restamped the twin from the hudpos text
		// colour before its page went up.
		HudFrameCompiler compiler;
		make(compiler);
		state.end_round.shown = false;
		state.round_over = false;
		(void)compiler.compile(state, 1024.0f, 768.0f);
		state.round_over = true;
		state.end_round.shown = true;
		page.mp_session_peer = true;
		CHECK(compiler.compile_server_status_page(state, page, 10000, true, 1024.0f, 768.0f));
		CHECK(impact_colors(compiler.server_status_page_list(), half_bright_argb(layout.hud_text), 2));
	}
}

// The page stands in for the whole scene frame: a dedicated host's (no session peer) always, a
// peer's while its status view is up on the authority (GameLoop_RenderFrame @0x521355,
// @0x521CD6..0x521CEF).
void test_main_frame_draws_scene() {
	using opennova::hud::main_frame_draws_scene;
	// A dedicated host: the page whatever the view word holds.
	CHECK(!main_frame_draws_scene(false, true, true));
	CHECK(!main_frame_draws_scene(false, true, false));
	// A listen host or a single player: the page while the view is up.
	CHECK(!main_frame_draws_scene(true, true, true));
	CHECK(main_frame_draws_scene(true, true, false));
	// A joiner (no authority) draws the scene even with the word set.
	CHECK(main_frame_draws_scene(true, false, true));
	CHECK(main_frame_draws_scene(true, false, false));
}

} // namespace

int main() {
	fnt_font_t font = test_font::uniform_test_font();
	test_throttle();
	test_grid_cells_and_codes();
	test_score_rows();
	test_page_compile(&font);
	test_dedicated_host_twin(&font);
	test_main_frame_draws_scene();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("hud_server_status_test OK\n");
	return 0;
}
