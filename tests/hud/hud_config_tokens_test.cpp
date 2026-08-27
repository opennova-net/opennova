// Pins the HUD config-token policy (runtime/hud/hud_config_tokens.h): the
// persisted defaults, the clamps the settings round trip applies, and the
// action cycles (hudcolor 0..5, huddetail 0..3, showhud (flags + 1) & 3).

#include <runtime/hud/hud_config_tokens.h>

#include <cstdio>
#include <cstdlib>

namespace {

int g_failures = 0;

void check(bool ok, const char *what) {
	if (!ok) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		++g_failures;
	}
}

} // namespace

int main() {
	using namespace opennova::hud;

	// The hud_color_index token: default 2, six schemes, wrap on cycle.
	check(kHudColorIndexDefault == 2, "color default 2");
	check(clamp_hud_color_index(-4) == 0, "color clamp low");
	check(clamp_hud_color_index(5) == 5, "color clamp keeps 5");
	check(clamp_hud_color_index(6) == 5, "color clamp high");
	check(next_hud_color_index(0) == 1, "color next 0 -> 1");
	check(next_hud_color_index(5) == 0, "color next 5 -> 0");

	// The hud_detail token: default 0, 0..3, level 3 is the blank HUD.
	check(kHudDetailLevelDefault == 0, "detail default 0");
	check(kHudDetailLevelBlank == 3, "detail blank 3");
	check(clamp_hud_detail_level(-1) == 0, "detail clamp low");
	check(clamp_hud_detail_level(9) == 3, "detail clamp high");
	check(next_hud_detail_level(0) == 1, "detail next 0 -> 1");
	check(next_hud_detail_level(2) == 3, "detail next 2 -> 3");
	check(next_hud_detail_level(3) == 0, "detail next 3 -> 0");

	// The showhud flags: default both bits, cycle (flags + 1) & 3.
	check(kShowHudFlagsDefault == 3, "showhud default 3");
	check((kShowHudFlagGun | kShowHudFlagSpinmap) == kShowHudFlagsDefault,
			"showhud default = gun | spinmap");
	check(next_showhud_flags(3) == 0, "showhud next 3 -> 0");
	check(next_showhud_flags(0) == 1, "showhud next 0 -> 1");
	check(next_showhud_flags(2) == 3, "showhud next 2 -> 3");

	// The friendly-tag session default and the anchor lift (0x4000 in 16.16).
	check(kFriendlyTagModeDefault == 2, "friendly tag mode default FULL");
	check(kFriendlyTagLiftUnits == 0x4000 / 65536.0f, "friendly tag lift 0x4000");

	if (g_failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", g_failures);
		return EXIT_FAILURE;
	}
	std::puts("hud_config_tokens_test: ok");
	return EXIT_SUCCESS;
}
