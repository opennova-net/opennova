// Pins the witnessed menu-backdrop policy: the three-slot table, the
// per-screen draw gate, and the expansion-first source resolution
// (docs/mnu/menu-re.md "The menu backdrop (Bink underlay)";
// [orig: UI_CreateMenuBinkVideos @ 0x54b590]).

#include "menu/menu_video.h"

#include <cstdio>
#include <cstring>
#include <set>
#include <string>

namespace {

int g_failures = 0;

void expect(bool ok, const char* what) {
	if (!ok) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		++g_failures;
	}
}

}  // namespace

int main() {
	using opennova::menu::MenuVideoSlot;
	using opennova::menu::menu_video_resolve;
	using opennova::menu::menu_video_slot_visible;
	using opennova::menu::menu_video_slots;
	using opennova::menu::menu_video_startup_screen;

	// The witnessed slot table, verbatim.
	const auto& slots = menu_video_slots();
	expect(slots.size() == 3, "three slots");
	expect(slots[0].slot == MenuVideoSlot::kMain &&
					std::strcmp(slots[0].file, "main.bik") == 0 &&
					slots[0].left == 0 && slots[0].top == 75 &&
					slots[0].right == 800 && slots[0].bottom == 525 &&
					slots[0].loop,
			"main slot: main.bik (0,75)-(800,525) looping");
	expect(slots[1].slot == MenuVideoSlot::kHeader &&
					std::strcmp(slots[1].file, "header.bik") == 0 &&
					slots[1].left == 0 && slots[1].top == 0 &&
					slots[1].right == 800 && slots[1].bottom == 75 &&
					slots[1].loop,
			"header slot: header.bik (0,0)-(800,75) looping");
	expect(slots[2].slot == MenuVideoSlot::kFooter &&
					std::strcmp(slots[2].file, "footer.bik") == 0 &&
					slots[2].left == 0 && slots[2].top == 525 &&
					slots[2].right == 800 && slots[2].bottom == 600 &&
					slots[2].loop,
			"footer slot: footer.bik (0,525)-(800,600) looping");

	// The draw gate: STARTUP -> main only; sub-screens -> strips only.
	expect(menu_video_slot_visible(MenuVideoSlot::kMain, true),
			"startup shows main");
	expect(!menu_video_slot_visible(MenuVideoSlot::kHeader, true),
			"startup hides header");
	expect(!menu_video_slot_visible(MenuVideoSlot::kFooter, true),
			"startup hides footer");
	expect(!menu_video_slot_visible(MenuVideoSlot::kMain, false),
			"sub-screen hides main");
	expect(menu_video_slot_visible(MenuVideoSlot::kHeader, false),
			"sub-screen shows header");
	expect(menu_video_slot_visible(MenuVideoSlot::kFooter, false),
			"sub-screen shows footer");

	// The front page is the STARTUP screen and nothing else.
	expect(menu_video_startup_screen("STARTUP"), "STARTUP is the front page");
	expect(!menu_video_startup_screen("SINGLE_PLAYER"),
			"SINGLE_PLAYER is a strips screen");
	expect(!menu_video_startup_screen(""), "empty screen is not the front page");
	expect(!menu_video_startup_screen("startup"),
			"screen names are case-exact");

	// Resolution: expansion copy wins; root fallback; silent miss.
	const std::set<std::string> disk = {
			"expansion/jox01/main.bik", "main.bik", "header.bik"};
	const auto exists = [&disk](const std::string& rel) {
		return disk.count(rel) != 0;
	};
	expect(menu_video_resolve("jox01", "main.bik", exists) ==
					"expansion/jox01/main.bik",
			"expansion main.bik wins");
	expect(menu_video_resolve("jox01", "header.bik", exists) == "header.bik",
			"root fallback when the expansion lacks the file");
	expect(menu_video_resolve("", "main.bik", exists) == "main.bik",
			"no expansion -> root");
	expect(menu_video_resolve("jox01", "footer.bik", exists).empty(),
			"missing everywhere -> empty (silent skip)");

	if (g_failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("menu_video_test: all checks passed\n");
	return 0;
}
