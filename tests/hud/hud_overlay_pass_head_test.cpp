// The gameplay overlay pass's head (hud/hud_frame.h HudFrameCompiler::overlay_pass_runs): the
// round-over latch and the white hit flash return before the pass draws anything (the health bar
// here), while the scene frame's other legs (the end-of-round overlay here) draw as on any frame.
// The flags' fill: the local view's white word reaches the combat state.
// The HUD item flash timers tick at the pass's head, past those returns and the level-3 one, so
// a gated stretch's ticks land on the next pass whole, a timer armed inside it included, and the
// overlay-buffer reset zeroes their last tick. The big map, drawn outside the pass, reads the
// entity-info record's last build (the waypoint) and the altitude colour the pass last set.
// [orig: HUD_RenderAllOverlays @0x5A8070 - g_RoundOver @0x5A8084, g_ScreenFlashWhiteHit
//  @0x5A8098, the return @0x5A80DB, g_HUDDetailLevel @0x5A80C4, the HUD_TickItemFlashTimers
//  call @0x5A80FB; HUD_TickItemFlashTimers @0x59A9E0; HUD_ResetAllOverlayBuffers @0x59DD7D;
//  HUD_DrawEndRoundStatsOverlay @0x5b7cd0, outside the pass]
#include <runtime/hud/hud_frame.h>

#include "common/test_font.h"

#include <cstdio>
#include <memory>

using namespace opennova::hud;

namespace {

int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

bool drew(const HudDrawList &list, HudElement element) {
	for (const HudElementSpan &span : list.element_spans)
		if (span.element == element) return true;
	return false;
}

void test_the_head_returns_before_the_pass() {
	const opennova::fnt::fnt_font_t font = test_font::uniform_test_font();
	auto compiler = std::make_unique<HudFrameCompiler>();
	HudLayout layout;
	layout.health_rect = {10.0f, 20.0f, 100.0f, 8.0f, true};
	compiler->configure(layout, &font);
	compiler->configure_label_fonts(&font, &font, &font, 1.0f, 1.0f, &font);
	auto state = std::make_unique<HudFrameState>();
	state->health_fraction = 1.0f;
	state->end_round.shown = true;
	state->end_round.lines = {{"D", 300}};

	const HudDrawList &open = compiler->compile(*state, 1024.0f, 768.0f);
	CHECK(drew(open, HudElement::Health));
	CHECK(drew(open, HudElement::EndRoundOverlay));

	// The round-over latch: no pass, the end-of-round overlay still up.
	state->round_over = true;
	const HudDrawList &latched = compiler->compile(*state, 1024.0f, 768.0f);
	CHECK(!drew(latched, HudElement::Health));
	CHECK(drew(latched, HudElement::EndRoundOverlay));
	state->round_over = false;

	// The white hit flash, alike.
	state->combat.white_flash = true;
	const HudDrawList &flashed = compiler->compile(*state, 1024.0f, 768.0f);
	CHECK(!drew(flashed, HudElement::Health));
	CHECK(drew(flashed, HudElement::EndRoundOverlay));
	state->combat.white_flash = false;

	CHECK(drew(compiler->compile(*state, 1024.0f, 768.0f), HudElement::Health));
	CHECK(HudFrameCompiler::overlay_pass_runs(*state));
	state->round_over = true;
	CHECK(!HudFrameCompiler::overlay_pass_runs(*state));
}

// Retail's arithmetic over the witnessed order: the first pass after a gated stretch subtracts
// everything since the last pass that ran, the stretch before the arm included.
void test_the_item_flash_ticks_behind_the_head() {
	const opennova::fnt::fnt_font_t font = test_font::uniform_test_font();
	auto compiler = std::make_unique<HudFrameCompiler>();
	HudLayout layout;
	layout.health_rect = {10.0f, 20.0f, 100.0f, 8.0f, true};
	compiler->configure(layout, &font);
	compiler->configure_label_fonts(&font, &font, &font, 1.0f, 1.0f, &font);
	auto state = std::make_unique<HudFrameState>();
	state->health_fraction = 1.0f;

	// The pass runs at tick 100: the timers' last tick is 100.
	state->ticks = 100;
	compiler->compile(*state, 1024.0f, 768.0f);

	// Each gate in turn: frames up to tick 150 never tick, a timer armed at 160 inside the
	// stretch keeps its value, and the first pass after it, at 170, takes all 70 ticks.
	for (int gate = 0; gate < 3; ++gate) {
		state->round_over = gate == 0;
		state->combat.white_flash = gate == 1;
		state->hud_detail_level = gate == 2 ? 3 : 0;
		for (int t = 110; t <= 150; t += 10) {
			state->ticks = t + 1000 * gate;
			compiler->compile(*state, 1024.0f, 768.0f);
		}
		compiler->set_item_flash(3, 0x80);
		state->ticks = 160 + 1000 * gate;
		compiler->compile(*state, 1024.0f, 768.0f);
		CHECK(compiler->item_flash().timer(3) == 0x80);
		state->round_over = false;
		state->combat.white_flash = false;
		state->hud_detail_level = 0;
		state->ticks = 170 + 1000 * gate;
		compiler->compile(*state, 1024.0f, 768.0f);
		// 0x80 - 70, where a per-frame countdown from the arm would leave 0x80 - 10.
		CHECK(compiler->item_flash().timer(3) == 0x80 - 70);
		// The next stretch starts from this pass: lapse the timer and step the clock on.
		compiler->set_item_flash(3, 0);
		state->ticks = 1100 + 1000 * gate;
		compiler->compile(*state, 1024.0f, 768.0f);
	}

	// The overlay-buffer reset zeroes the last tick and keeps the timers: the next pass's
	// delta is the whole clock, which lapses a timer at or under it.
	state->ticks = 4000;
	compiler->compile(*state, 1024.0f, 768.0f);
	compiler->set_item_flash(5, 0x800);
	compiler->set_item_flash(6, 0x2000);
	compiler->reset_overlay_buffers();
	CHECK(compiler->item_flash().timer(5) == 0x800);
	state->ticks = 4010;
	compiler->compile(*state, 1024.0f, 768.0f);
	CHECK(compiler->item_flash().timer(5) == 0);
	CHECK(compiler->item_flash().timer(6) == 0x2000 - 4010);
}

// The waypoint pointer's altitude band on a map pass: the raw tricolor on the tip or the
// 2c-saturated one on the line (blue below, orange above, green level), -1 for none.
int pointer_band(const HudMapPass &pass) {
	const uint32_t raw[3] = {0xFF7F5000u, 0xFF20407Fu, 0xFF007000u};
	const uint32_t lit[3] = {0xFFFEA000u, 0xFF4080FEu, 0xFF00E000u};
	for (int band = 0; band < 3; ++band) {
		for (const HudMapSprite &sp : pass.sprites)
			if (sp.color == raw[band]) return band;
		for (const HudMapLine &ln : pass.lines)
			if (ln.color == raw[band] || ln.color == lit[band]) return band;
	}
	return -1;
}

// The big map draws outside the pass from the entity-info record's last build and the altitude
// colour the pass last set: the head builds the record past its round-over and white-flash
// returns (ahead of the level-3 one), the colour is set past the level-3 return, only while the
// record's waypoint is active [orig: HUD_RenderAllOverlays @0x5A8070 - the HUD_BuildEntityInfo
// call @0x5A80BC, g_HUDDetailLevel @0x5A80C4, the HUD_UpdateWaypointAltitudeColor call
// @0x5A815F; HUD_UpdateWaypointAltitudeColor @0x590970; HUD_DrawMapOverlay @0x5A5F40 from
// HUD_BuildMapOverlayView (@0x5A805A)].
void test_the_big_map_reads_the_last_record_and_altitude_colour() {
	const opennova::fnt::fnt_font_t font = test_font::uniform_test_font();
	auto compiler = std::make_unique<HudFrameCompiler>();
	compiler->configure(HudLayout{}, &font);
	compiler->configure_label_fonts(&font, &font, &font, 1.0f, 1.0f, &font);
	auto state = std::make_unique<HudFrameState>();
	state->minimap.map_mode = 3;
	state->minimap.terrain.present = true;
	state->minimap.terrain.sector_count = 16;
	state->minimap.terrain.sector_rows = 16;
	state->minimap.terrain.sector_grid.fill(1);
	state->waypoint.present = true;
	state->waypoint.world_x = 200 << 16;
	state->waypoint.world_z = 3 << 16; // above the player at z 0

	// No record before the first pass that gets past the head: no pointer.
	state->round_over = true;
	CHECK(pointer_band(compiler->compile(*state, 1024.0f, 768.0f).big_map) == -1);
	state->round_over = false;
	CHECK(pointer_band(compiler->compile(*state, 1024.0f, 768.0f).big_map) == 0);

	// Level 3: the record is built, the colour is not: the player climbs past the waypoint
	// and the big map keeps the orange.
	state->hud_detail_level = 3;
	state->minimap.player_z = 6 << 16;
	CHECK(pointer_band(compiler->compile(*state, 1024.0f, 768.0f).big_map) == 0);
	state->hud_detail_level = 0;
	CHECK(pointer_band(compiler->compile(*state, 1024.0f, 768.0f).big_map) == 1);

	// The round-over latch and the white flash build no record: a waypoint cleared under
	// either still points on the big map until the next pass.
	state->waypoint.present = false;
	state->round_over = true;
	CHECK(pointer_band(compiler->compile(*state, 1024.0f, 768.0f).big_map) == 1);
	state->round_over = false;
	state->combat.white_flash = true;
	CHECK(pointer_band(compiler->compile(*state, 1024.0f, 768.0f).big_map) == 1);
	state->combat.white_flash = false;
	CHECK(pointer_band(compiler->compile(*state, 1024.0f, 768.0f).big_map) == -1);

	// No active waypoint keeps the colour: a waypoint back at the level shows the kept blue
	// on a level-3 frame, the green once the pass sets it.
	state->waypoint.present = true;
	state->waypoint.world_z = 6 << 16;
	state->hud_detail_level = 3;
	CHECK(pointer_band(compiler->compile(*state, 1024.0f, 768.0f).big_map) == 1);
	state->hud_detail_level = 0;
	CHECK(pointer_band(compiler->compile(*state, 1024.0f, 768.0f).big_map) == 2);
}

// The scene frame calls the pass only past its call site's gates (the F1 help, the dead bit, a
// death.mnu trigger or its latch, a chase view on foot, camera mode 4 and up): an uncalled pass
// draws nothing and does none of its head's work (no record build, no item flash tick), while
// the scene frame's own legs (the end-of-round overlay, the big map over the last record) draw.
// The radar gates the HUD hands the session leave the call site out: the session tests it itself.
// [orig: Render_ProcessMainSceneFrame @0x5CAB2B..0x5CAD04 - the HUD_RenderAllOverlays calls
//  @0x5CACE3 / @0x5CAD04, the big map @0x5CAC50 -> HUD_BuildMapOverlayView @0x5CAD15;
//  HUD_RenderAllOverlays @0x5A8070 - the HUD_BuildEntityInfo call @0x5A80BC, the
//  HUD_TickItemFlashTimers call @0x5A80FB]
void test_the_scene_frame_call_site_gates_the_pass() {
	const opennova::fnt::fnt_font_t font = test_font::uniform_test_font();
	auto compiler = std::make_unique<HudFrameCompiler>();
	HudLayout layout;
	layout.health_rect = {10.0f, 20.0f, 100.0f, 8.0f, true};
	compiler->configure(layout, &font);
	compiler->configure_label_fonts(&font, &font, &font, 1.0f, 1.0f, &font);
	auto state = std::make_unique<HudFrameState>();
	state->health_fraction = 1.0f;
	state->end_round.shown = true;
	state->end_round.lines = {{"D", 300}};
	state->minimap.map_mode = 3;
	state->minimap.terrain.present = true;
	state->minimap.terrain.sector_count = 16;
	state->minimap.terrain.sector_rows = 16;
	state->minimap.terrain.sector_grid.fill(1);
	state->waypoint.present = true;
	state->waypoint.world_x = 200 << 16;
	state->waypoint.world_z = 3 << 16; // above the player at z 0

	// A called pass at tick 100 builds the record and takes the item flash's last tick.
	state->ticks = 100;
	const HudDrawList &called = compiler->compile(*state, 1024.0f, 768.0f);
	CHECK(drew(called, HudElement::Health));
	CHECK(drew(called, HudElement::EndRoundOverlay));
	CHECK(pointer_band(called.big_map) == 0);

	// The uncalled stretch: no pass draw, the frame's own legs still up; a waypoint cleared
	// inside it still points on the big map (no record build); a timer armed inside it keeps
	// its count.
	state->overlay_pass_called = false;
	CHECK(!HudFrameCompiler::overlay_pass_runs(*state));
	state->waypoint.present = false;
	compiler->set_item_flash(2, 0x80);
	state->ticks = 140;
	const HudDrawList &uncalled = compiler->compile(*state, 1024.0f, 768.0f);
	CHECK(!drew(uncalled, HudElement::Health));
	CHECK(drew(uncalled, HudElement::EndRoundOverlay));
	CHECK(pointer_band(uncalled.big_map) == 0);
	CHECK(compiler->item_flash().timer(2) == 0x80);

	// The next called pass, at tick 150, rebuilds the record and takes the whole 50 ticks.
	state->overlay_pass_called = true;
	state->ticks = 150;
	const HudDrawList &back = compiler->compile(*state, 1024.0f, 768.0f);
	CHECK(drew(back, HudElement::Health));
	CHECK(pointer_band(back.big_map) == -1);
	CHECK(compiler->item_flash().timer(2) == 0x80 - 50);

	// The radar gates over an authored corner map: the pass and the map site, called or not.
	auto radar = std::make_unique<HudFrameCompiler>();
	HudLayout corner;
	corner.spinmap_rect = {800.0f, 20.0f, 200.0f, 200.0f, true};
	radar->configure(corner, &font);
	auto view = std::make_unique<HudFrameState>();
	const uint32_t both = HudFrameCompiler::kRadarGatePass | HudFrameCompiler::kRadarGateMapSite;
	CHECK(radar->radar_frame_gates(*view) == both);
	view->overlay_pass_called = false;
	CHECK(radar->radar_frame_gates(*view) == both);
}

} // namespace

int main() {
	test_the_head_returns_before_the_pass();
	test_the_item_flash_ticks_behind_the_head();
	test_the_big_map_reads_the_last_record_and_altitude_colour();
	test_the_scene_frame_call_site_gates_the_pass();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("hud_overlay_pass_head_test OK\n");
	return 0;
}
