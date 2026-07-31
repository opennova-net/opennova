#pragma once

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>

#include <vector>

#include "nova_mnu_widget_common.h"

namespace godot {

class NovaMnuMenu;

// A logical navigation marker (type="goto"). Renders nothing and is zero-size; it
// carries the MNU actions plus an optional hotkey. A shell (or, later, a menu-level
// hotkey router) calls trigger() to dispatch its actions through the owning menu.
//
// A goto with actions and no hotkey is an "immediate goto" in the reference (it
// fires when its screen is shown). That on-show firing belongs to the navigation
// owner (it must key off screen visibility, not node _ready, so a goto on a hidden
// screen does not fire at build time), so M9 stores the fire_on_show flag and
// exposes trigger() but does not auto-fire. In edit_mode it is fully inert.
class NovaMnuGoto : public Control {
	GDCLASS(NovaMnuGoto, Control)

private:
	std::vector<MnuActionData> actions_;
	NovaMnuMenu *menu_ = nullptr;
	bool edit_mode_ = false;
	String hotkey_;
	bool hotkey_virtual_ = false;
	bool fire_on_show_ = false;

protected:
	static void _bind_methods();

public:
	// --- Build-time configuration (called by nova_mnu_builder) ---
	void set_menu(NovaMnuMenu *p_menu) { menu_ = p_menu; }
	void set_edit_mode(bool p_edit) { edit_mode_ = p_edit; }
	void add_action(const MnuActionData &p_action) { actions_.push_back(p_action); }
	void set_hotkey(const String &p_hotkey, bool p_virtual) {
		hotkey_ = p_hotkey;
		hotkey_virtual_ = p_virtual;
	}
	void set_fire_on_show(bool p_value) { fire_on_show_ = p_value; }

	String get_hotkey() const { return hotkey_; }
	bool get_fire_on_show() const { return fire_on_show_; }
	int get_action_count() const { return static_cast<int>(actions_.size()); }

	// Dispatch all of this goto's actions through the owning menu. No-op in edit_mode
	// or without a menu.
	void trigger();
};

} // namespace godot
