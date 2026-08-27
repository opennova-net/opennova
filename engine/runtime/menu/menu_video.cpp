#include <runtime/menu/menu_video.h>

namespace opennova::menu {

const std::array<MenuVideoSlotSpec, kMenuVideoSlotCount>& menu_video_slots() {
	// [orig: UI_CreateMenuBinkVideos @ 0x54b590 — the three BinkSlot_Create
	//  calls: main (0,75)-(800,525), header (0,0)-(800,75),
	//  footer (0,525)-(800,600); loop=1 each]
	static const std::array<MenuVideoSlotSpec, kMenuVideoSlotCount> kSlots = {{
			{MenuVideoSlot::kMain, "main.bik", 0, 75, 800, 525, true},
			{MenuVideoSlot::kHeader, "header.bik", 0, 0, 800, 75, true},
			{MenuVideoSlot::kFooter, "footer.bik", 0, 525, 800, 600, true},
	}};
	return kSlots;
}

bool menu_video_slot_visible(MenuVideoSlot slot, bool startup_screen) {
	// [orig: UI_OnStartupScreenActivate @ 0x5557f0 — Enable(main),
	//  Disable(header), Disable(footer); every sub-screen activate installs
	//  the inverse trio, UI_EnableHeaderFooterBinkStrips @ 0x556b50]
	if (slot == MenuVideoSlot::kMain) {
		return startup_screen;
	}
	return !startup_screen;
}

bool menu_video_startup_screen(const std::string& screen_name) {
	// STARTUP is the one screen whose activate handler shows the center
	// movie [orig: UI_OnStartupScreenActivate @ 0x5557f0, bound to the
	// STARTUP screen; every other shipped screen's activate installs the
	// strips].
	return screen_name == "STARTUP";
}

std::string menu_video_resolve(
		const std::string& expansion, const char* file,
		const std::function<bool(const std::string&)>& exists) {
	// [orig: UI_CreateMenuBinkVideos @ 0x54b590 — expansion\<exp>\<file>
	//  preferred when File_ExistsOnDisk @ 0x562d80; BinkOpen failure leaves
	//  the slot empty with no error UI]
	if (!exists) {
		return std::string();
	}
	if (!expansion.empty()) {
		const std::string overridden =
				"expansion/" + expansion + "/" + file;
		if (exists(overridden)) {
			return overridden;
		}
	}
	if (exists(file)) {
		return std::string(file);
	}
	return std::string();
}

}  // namespace opennova::menu
