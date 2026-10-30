#pragma once

// vehicle.mnu's VEHICLE screen as the code registers it, over the session's VehicleMenu
// (vehicle_menu.h carries the screen's rules): ACCEPT and CANCEL to VehicleMenu_OnButton with user
// data 0 / 1, ITEM_LIST's select to its row pick, ITEM_DISPLAY's custom draw to the preview
// [orig: VehicleMenu_RegisterCallbacks @0x5641C0 — @0x5641d9, @0x5641f7, @0x564215, @0x564238].
// The screen's open is the menu's: the init on its first parse and every open's activate
// (VehicleMenu::open); the list shows the rows and the selection the menu holds, refilled when its
// poll refills them; a click closes the screen. The embedder hands the session and draws the
// preview model (vehicle_preview_view) itself.

#include <cstdint>
#include <string>
#include <vector>

#include <runtime/inmatch/vehicle_menu.h>
#include <runtime/menu/menu_screen_controller.h>

namespace opennova::def {
struct DefItemsFile;
}
namespace opennova::world {
class World;
}

namespace opennova::inmatch {

// The session the screen runs over: the role's client runtime (the joiner's or the listen host's
// own), the world and the mission's items.def rows (null: none loaded).
struct VehicleMenuSession {
	ClientRuntime *runtime = nullptr;
	const world::World *world = nullptr;
	const def::DefItemsFile *items = nullptr;
	explicit operator bool() const { return runtime != nullptr && world != nullptr; }
};

class VehicleMenuController : public menu::MenuScreenController {
public:
	class Host {
	public:
		virtual ~Host() = default;
		// The session now (empty: the screen does nothing).
		virtual VehicleMenuSession session() = 0;
		// The click's tail: the screen closes back to the mission.
		virtual void close() = 0;
	};
	void set_host(Host *host) { host_ = host; }

	// UI_OpenMenuScreen("vehicle.mnu", "VEHICLE")'s events (VehicleMenu::open), then the list.
	void open(menu::MenuRuntime &menu, const std::string &file, const std::string &screen) override;
	// ITEM_LIST's pick [orig: UI_OnItemListSelectionChanged @0x563B30]; ACCEPT / CANCEL's click.
	void on_event(menu::MenuRuntime &menu, const menu::MenuEvent &event) override;
	// Once a frame while the screen is up: the rows and the selection the menu holds into ITEM_LIST
	// when they changed (its poll refills them every 64 frames while a reply stands).
	void refresh(menu::MenuRuntime &menu);
	// ACCEPT (false) or CANCEL (true): the menu's click (C2S 0x40 for ACCEPT on a bay with a
	// selection), then the close [orig: VehicleMenu_OnButton @0x563B60]. The Escape close is
	// CANCEL's.
	void click(bool cancel);
	// The ITEM_DISPLAY model: the selected group's items.def graphic ("" the clear alone).
	std::string preview_graphic();

private:
	Host *host_ = nullptr;
	std::vector<VehicleMenuRow> shown_rows_;
	int shown_selection_ = -2;
};

} // namespace opennova::inmatch
