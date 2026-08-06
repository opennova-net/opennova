#pragma once

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

#include <vector>

#include "mnu_item_cell.h"
#include "nova_mnu_widget_behavior.h"

namespace godot {

class NovaMnuMenu;

// A spinner (type="spinlist"): a value cell cycled by SpinUp / SpinDown button
// children. Each item is text, an image, or a color swatch (the original draws the
// selected item three ways: text, a native-size image, or a full-rect color swatch
// [orig: CSpinListWnd_Render @ 0x64b220]). The .mnu seeds the item set; a runtime shell
// can replace the text values via set_values() (e.g. the cyclable difficulty list).
// Up/down wrap and emit value_changed(index, value) and relay through the owning menu.
// Inert in edit_mode (the spin buttons are built disabled and nothing is wired).
class NovaMnuSpinList : public Control {
	GDCLASS(NovaMnuSpinList, Control)

private:
	MnuWidgetBehavior behavior_;
	std::vector<MnuItemVisual> visuals_;
	int index_ = 0;
	Control *value_mount_ = nullptr;

	void on_spin_up();
	void on_sound_mouse_entered();
	void on_sound_mouse_exited();
	void on_spin_down();
	void update_value_cell();

protected:
	static void _bind_methods();

public:
	void _ready() override;

	// --- Build-time configuration (called by nova_mnu_builder) ---
	void set_menu(NovaMnuMenu *p_menu) { behavior_.set_menu(p_menu); }
	void set_edit_mode(bool p_edit) { behavior_.set_edit_mode(p_edit); }
	void set_sounds(const MnuWidgetSounds &p_sounds) { behavior_.set_sounds(p_sounds); }
	// The full per-item visuals (text / image / color), from the builder.
	void set_item_visuals(const std::vector<MnuItemVisual> &p_visuals);

	// --- Runtime data binding ---
	void set_values(const PackedStringArray &p_values); // text-only items (shell)
	void set_value_index(int p_index); // programmatic; clamps, no emit
	int get_value_index() const { return index_; }
	String get_value() const;
	// The selected item's `value=` attribute (the semantic value, e.g. SERVERTYPE 0/1), as
	// opposed to get_value()'s localized display text. Empty when the item carries no value.
	String get_value_attr() const;
	int get_value_count() const { return static_cast<int>(visuals_.size()); }
	// Advance by delta with wrap-around; updates the cell, plays the click sound,
	// and emits value_changed (the SpinUp/SpinDown buttons drive this).
	void cycle(int p_delta);
};

} // namespace godot
