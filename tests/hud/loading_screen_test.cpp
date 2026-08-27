// Pins the loading screen spec's shell-facing rules (runtime/hud/
// loading_screen.h): the .bms -> .pcx sidecar name, the present-due rule and
// the composited resource names.

#include <runtime/hud/loading_screen.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

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

	// The sidecar rule: the file part, its extension replaced or appended.
	check(loading_sidecar_image_name("00TRg.bms") == "00TRg.pcx", "sidecar .bms");
	check(loading_sidecar_image_name("TDH_I5A.BMS") == "TDH_I5A.pcx", "sidecar .BMS");
	check(loading_sidecar_image_name("dvxi5") == "dvxi5.pcx", "sidecar appended");
	check(loading_sidecar_image_name("maps/ASH_I5A.bms") == "ASH_I5A.pcx", "sidecar strips dir");
	check(loading_sidecar_image_name("maps\\ASH_I5A.bms") == "ASH_I5A.pcx",
			"sidecar strips backslash dir");

	// The present-due rule: interval elapsed, reported changed, or trailing.
	check(loading_present_due(100, false, 50, 50), "due at the interval");
	check(!loading_present_due(99, false, 50, 50), "not due under the interval");
	check(loading_present_due(0, true, 50, 50), "due when reported changed");
	check(loading_present_due(0, false, 40, 50), "due while displayed trails");
	check(!loading_present_due(0, false, 55, 50), "not due when displayed leads");

	// The composited resource names.
	check(std::strcmp(kLoadingFallbackImage, "loadscrn.pcx") == 0, "fallback image");
	check(std::strcmp(kLoadingFontSmall, "Arials18.fnt") == 0, "small font");
	check(std::strcmp(kLoadingFontLarge, "Arial22.fnt") == 0, "large font");
	check(std::strcmp(kLoadingServerMessageLabelKey, "LT_SERVERMSG") == 0, "label key");
	check(std::strcmp(kLoadingServerMessageLabelFallback, "Message from Game Server") == 0,
			"label fallback");

	if (g_failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", g_failures);
		return EXIT_FAILURE;
	}
	std::puts("loading_screen_test: ok");
	return EXIT_SUCCESS;
}
