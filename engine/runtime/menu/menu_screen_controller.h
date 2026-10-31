#pragma once

// A screen controller: what the game's code does with the controls of a screen it drives by NAME,
// the per-screen registration of the original (a row bound to the control the screen holds, run
// on its event) [orig: CUIScene_RegisterControlCallback @ 0x63c060 -> CUIScene_BindControlCallbacks
// @ 0x63af80; the screen's INIT / activate run from UI_DispatchScreenEvent @ 0x54e6a0]. A
// controller holds the screen's populate order, its cascades (a pick that refills the lists after
// it) and its open and close, over the engine models the screen reads (the avatar table, the
// loadout pages, the deploy rows, the stat rows, ...); the embedder hands it the runtime's events
// while it holds the document and does the device work it asks for (a preview, an icon) itself.
//
// Two ways a controller gets a document, both through menu::MenuShell (menu_shell.h):
// - a delegate the shell hands a whole document it opens, by the owner name menu::MenuCommands
//   gives that document (the LAN screens "lan", PLAYER_INFO "player_info"): MenuShell::add_controller;
// - a screen a mission opens by name (weapon.mnu WEAPON, death.mnu DEATH, stat.mnu STAT, ...),
//   whose presenter opens the document itself: MenuShell::claim.

#include <string>

namespace opennova::menu {

class MenuRuntime;
struct MenuEvent;

class MenuScreenController {
public:
	virtual ~MenuScreenController() = default;
	// The delegate name the shell hands documents to (MenuCommands::owner); "" for a controller only
	// ever claimed.
	virtual const char *owner() const { return ""; }
	// The document it drives was opened on `screen` (and wired): the screen's populate, as its INIT.
	virtual void open(MenuRuntime &menu, const std::string &file, const std::string &screen) = 0;
	// One of the runtime's events while it holds the document, after the shell's own handling.
	virtual void on_event(MenuRuntime &menu, const MenuEvent &event) {
		(void)menu;
		(void)event;
	}
	// Another document opened over it, or its screen closed: it lets go of what it holds of this one.
	virtual void close(MenuRuntime &menu) { (void)menu; }
};

} // namespace opennova::menu
