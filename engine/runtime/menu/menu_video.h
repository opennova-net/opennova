#pragma once

// The menu backdrop movie slots. The authored <APPEARANCE type="custom"> on
// main.mnu's MAIN window and the LOGO_SPLASH_HDR/FTR windows registers NO
// draw handler and paints nothing — the shell decodes three looping Bink
// movies and draws them as stretched quads UNDER the whole compiled menu
// every frame, so they show through the unpainted regions. This module is
// the witnessed slot/gating/resolution policy; decode and upload are the
// embedder's device leg.
// [orig: UI_CreateMenuBinkVideos @ 0x54b590 (slots, files, design rects,
//  expansion override, loop); Menu_RenderFrame @ 0x54b7c0 (Bink update +
//  draw BEFORE the scene walk); BinkVideoSlot_Draw @ 0x5674d0 (one
//  stretched quad, gated on the visible flag only — decode continues while
//  hidden); BinkVideo_UpdateAllSlots @ 0x5676f0 has exactly one caller, so
//  menu movies never tick in-game]
// Witness record: docs/mnu/menu-re.md ("The menu backdrop (Bink underlay)").

#include <array>
#include <functional>
#include <string>

namespace opennova::menu {

enum class MenuVideoSlot : int {
	kMain = 0,
	kHeader = 1,
	kFooter = 2,
};

inline constexpr int kMenuVideoSlotCount = 3;

// One backdrop slot: the movie basename and its fixed 800x600 design-space
// rect. The rect scales to the device with the same anamorphic pair as every
// widget, int-truncated; the movie stretches into it as one quad — no
// letterboxing, no aspect preservation.
// [orig: UI_CreateMenuBinkVideos @ 0x54b590 ->
//  CUIScene_ScaleRectDesignToDevice @ 0x63b210]
struct MenuVideoSlotSpec {
	MenuVideoSlot slot;
	const char* file;
	int left = 0;
	int top = 0;
	int right = 0;
	int bottom = 0;
	bool loop = true;
};

// The witnessed three-slot table: main.bik (0,75)-(800,525), header.bik
// (0,0)-(800,75), footer.bik (0,525)-(800,600), loop on for all three.
// [orig: UI_CreateMenuBinkVideos @ 0x54b590]
const std::array<MenuVideoSlotSpec, kMenuVideoSlotCount>& menu_video_slots();

// Per-screen draw gate: the STARTUP front page shows the center movie alone;
// every other shipped screen shows the header+footer strips alone. Only the
// DRAW is gated — a hidden movie keeps advancing on its own clock.
// [orig: UI_OnStartupScreenActivate @ 0x5557f0 (main on, strips off);
//  UI_EnableHeaderFooterBinkStrips @ 0x556b50 + the per-screen inlined
//  trios (strips on, main off)]
bool menu_video_slot_visible(MenuVideoSlot slot, bool startup_screen);

// Whether `screen_name` takes the front-page layout of the gate above. The
// witnessed toggles live in each screen's activate handler; STARTUP is the
// one screen that shows the center movie.
bool menu_video_startup_screen(const std::string& screen_name);

// The source the slot plays: the mounted expansion's copy wins when one
// exists on disk. `expansion` is the folder name under expansion/ ("" =
// none); `exists` answers for a path RELATIVE to the resource root. Returns
// the relative path chosen ("expansion/<exp>/<file>" or "<file>"), or ""
// when neither exists — retail skips the slot silently (no draw, no error).
// [orig: UI_CreateMenuBinkVideos @ 0x54b590 + File_ExistsOnDisk @ 0x562d80]
std::string menu_video_resolve(
		const std::string& expansion, const char* file,
		const std::function<bool(const std::string&)>& exists);

}  // namespace opennova::menu
