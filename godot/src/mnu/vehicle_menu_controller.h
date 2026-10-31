#pragma once

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <runtime/inmatch/vehicle_menu_controller.h>

#include <memory>

#include "mnu/menu_driver.h"

namespace godot {

class Simulation;

// The VEHICLE screen's controller (engine inmatch::VehicleMenuController) over the session a
// Simulation runs: the presenter claims it on its driver at each open (MenuDriver.claim_screen),
// calls refresh() once a frame and click() for the Escape close, and closes its frame on
// close_requested. The ITEM_DISPLAY pane's view math is here for the device that draws it.
class VehicleMenuController : public MenuScreenBinding {
	GDCLASS(VehicleMenuController, MenuScreenBinding)

	class Host;
	friend class Host;

protected:
	static void _bind_methods();

public:
	VehicleMenuController();
	~VehicleMenuController() override;

	opennova::menu::MenuScreenController *screen_controller() override { return &controller_; }

	// The driver whose document is vehicle.mnu and the session the screen runs over.
	void setup(const Ref<MenuDriver> &p_driver, const Ref<Simulation> &p_sim);
	// Once a frame while the screen is up: the list as the menu's poll left it.
	void refresh();
	// ACCEPT (false) or CANCEL (true), then close_requested.
	void click(bool p_cancel);
	// The ITEM_DISPLAY model's graphic ("" the clear alone), in the machine's code page.
	String preview_graphic();

	// The ITEM_DISPLAY preview (engine inmatch::vehicle_preview_view): the eye for a model of
	// `p_bounds` (its centre at the origin), the model's turn at `p_tick_ms` (the engine's
	// render-space pose matrix, its x / z swapped into Godot's), the field across the rect's width
	// in degrees, and the clear colour.
	static Vector3 preview_eye(const AABB &p_bounds);
	static Basis preview_model_basis(int64_t p_tick_ms);
	static float preview_fov();
	static Color preview_clear_color();

private:
	MenuDriver *driver_() const;
	Simulation *sim_() const;

	opennova::inmatch::VehicleMenuController controller_;
	std::unique_ptr<Host> host_;
	ObjectID driver_id_;
	ObjectID sim_id_;
};

} // namespace godot
