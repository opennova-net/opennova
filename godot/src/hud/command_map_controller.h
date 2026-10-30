#pragma once

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <runtime/menu/command_map_controller.h>

#include <memory>

#include "mnu/menu_driver.h"

namespace godot {

class CommandMapScreen;
class MapViewWindow;
class RtxtStringFile;
class WorldView;

// The CMAP screen's controller (engine menu::CommandMapController) over its devices: the presenter
// builds it with the menu (setup: the driver, the world view whose session and frame clock it
// reads, the tables' runner, the map window mounted over MAP / ORDERS_MAP or null, the gametext
// table and the HUD overlay getter), claims it on the driver at each open
// (MenuDriver.claim_screen), calls frame() once a frame and forwards the map's
// waypoint_dialog_requested, and closes its frame on close_requested.
class CommandMapController : public MenuScreenBinding {
	GDCLASS(CommandMapController, MenuScreenBinding)

	class Host;
	friend class Host;

protected:
	static void _bind_methods();

public:
	CommandMapController();
	~CommandMapController() override;

	opennova::menu::MenuScreenController *screen_controller() override { return &controller_; }

	void setup(const Ref<MenuDriver> &p_driver, const Ref<WorldView> &p_view,
			const Ref<CommandMapScreen> &p_screen, MapViewWindow *p_map_window,
			const Ref<RtxtStringFile> &p_gametext, const Callable &p_hud_source);
	void frame();
	void waypoint_dialog_requested(const Vector2i &p_point);
	// Read seams (tests, diagnostics): the map control the delete button follows and its state.
	String get_bound_map() const { return String(controller_.bound_map().c_str()); }
	bool is_delete_bound() const { return controller_.delete_bound(); }

private:
	MenuDriver *driver_() const;
	MapViewWindow *map_window_() const;

	opennova::menu::CommandMapController controller_;
	std::unique_ptr<Host> host_;
	ObjectID driver_id_;
	ObjectID map_window_id_;
	Ref<WorldView> view_;
	Ref<CommandMapScreen> screen_;
	Ref<RtxtStringFile> gametext_;
	Callable hud_source_;
};

} // namespace godot
