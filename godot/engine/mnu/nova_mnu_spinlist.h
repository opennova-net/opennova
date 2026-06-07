#pragma once

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

#include "nova_mnu_widget_behavior.h"

namespace godot {

class NovaMnuMenu;

// A spinner (type="spinlist"): a value Label cycled by SpinUp / SpinDown button
// children. The .mnu seeds a value set; a runtime host replaces it via set_values()
// (e.g. the cyclable difficulty / option list). Up/down wrap and emit
// value_changed(index, value) and relay through the owning menu. Inert in edit_mode
// (the spin buttons are built disabled and nothing is wired).
class NovaMnuSpinList : public Control {
	GDCLASS(NovaMnuSpinList, Control)

private:
	MnuWidgetBehavior behavior_;
	PackedStringArray values_;
	int index_ = 0;
	Label *value_label_ = nullptr;

	void on_spin_up();
	void on_spin_down();
	void update_label();

protected:
	static void _bind_methods();

public:
	void _ready() override;

	// --- Build-time configuration (called by nova_mnu_builder) ---
	void set_menu(NovaMnuMenu *p_menu) { behavior_.set_menu(p_menu); }
	void set_edit_mode(bool p_edit) { behavior_.set_edit_mode(p_edit); }
	void set_click_sound(const String &p_trigger, const String &p_file) {
		behavior_.set_click_sound(p_trigger, p_file);
	}

	// --- Runtime data binding ---
	void set_values(const PackedStringArray &p_values);
	void set_value_index(int p_index); // programmatic; clamps, no emit
	int get_value_index() const { return index_; }
	String get_value() const;
	int get_value_count() const { return values_.size(); }
	// Advance by delta with wrap-around; updates the label, plays the click sound,
	// and emits value_changed (the SpinUp/SpinDown buttons drive this).
	void cycle(int p_delta);
};

} // namespace godot
