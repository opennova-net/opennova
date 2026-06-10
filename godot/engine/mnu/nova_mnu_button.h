#pragma once

#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/texture_button.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/string.hpp>

#include <vector>

#include "nova_mnu_widget_common.h"

namespace godot {

class NovaMnuMenu;

// A menu button (port of mnu_button.gd). Carries navigation actions and hover/
// click sound triggers; on press it dispatches its actions through the owning
// NovaMnuMenu navigation controller and asks it to play sounds. Hovering swaps
// the child "Label" colour between the normal and mouseover font colours.
//
// In edit_mode the builder leaves it disabled and it connects no signals, so the
// ONED preview canvas never navigates or plays audio while authoring.
class NovaMnuButton : public TextureButton {
	GDCLASS(NovaMnuButton, TextureButton)

private:
	std::vector<MnuActionData> actions_;
	MnuWidgetSounds sounds_;
	Color font_color_ = Color(1, 1, 1, 1);
	Color font_hover_color_ = Color(1, 1, 1, 1);
	NovaMnuMenu *menu_ = nullptr;
	bool edit_mode_ = false;
	Label *label_ = nullptr;

	void on_pressed();
	void on_mouse_entered();
	void on_mouse_exited();
	void play_state_sound(int p_state);
	void set_label_color(const Color &p_color);
	void execute_action(const MnuActionData &p_action);

protected:
	static void _bind_methods();

public:
	void _ready() override;

	// --- Build-time configuration (called by nova_mnu_builder) ---
	void set_menu(NovaMnuMenu *p_menu) { menu_ = p_menu; }
	void set_edit_mode(bool p_edit) { edit_mode_ = p_edit; }
	void add_action(const MnuActionData &p_action) { actions_.push_back(p_action); }
	void set_sounds(const MnuWidgetSounds &p_sounds) { sounds_ = p_sounds; }
	void set_font_colors(const Color &p_normal, const Color &p_hover) {
		font_color_ = p_normal;
		font_hover_color_ = p_hover;
	}

	int get_action_count() const { return static_cast<int>(actions_.size()); }
};

} // namespace godot
