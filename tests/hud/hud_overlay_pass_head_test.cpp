// The gameplay overlay pass's head (hud/hud_frame.h HudFrameCompiler::overlay_pass_runs): the
// round-over latch and the white hit flash return before the pass draws anything (the health bar
// here), while the scene frame's other legs (the end-of-round overlay here) draw as on any frame.
// The flags' fill: the local view's white word reaches the combat state.
// [orig: HUD_RenderAllOverlays @0x5A8070 - g_RoundOver @0x5A8084, g_ScreenFlashWhiteHit
//  @0x5A8098, the return @0x5A80DB; HUD_DrawEndRoundStatsOverlay @0x5b7cd0, outside the pass]
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
	state->spawn_success_gate = true;
	const HudDrawList &latched = compiler->compile(*state, 1024.0f, 768.0f);
	CHECK(!drew(latched, HudElement::Health));
	CHECK(drew(latched, HudElement::EndRoundOverlay));
	state->spawn_success_gate = false;

	// The white hit flash, alike.
	state->combat.white_flash = true;
	const HudDrawList &flashed = compiler->compile(*state, 1024.0f, 768.0f);
	CHECK(!drew(flashed, HudElement::Health));
	CHECK(drew(flashed, HudElement::EndRoundOverlay));
	state->combat.white_flash = false;

	CHECK(drew(compiler->compile(*state, 1024.0f, 768.0f), HudElement::Health));
	CHECK(HudFrameCompiler::overlay_pass_runs(*state));
	state->spawn_success_gate = true;
	CHECK(!HudFrameCompiler::overlay_pass_runs(*state));
}

} // namespace

int main() {
	test_the_head_returns_before_the_pass();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("hud_overlay_pass_head_test OK\n");
	return 0;
}
