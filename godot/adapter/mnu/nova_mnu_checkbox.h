#pragma once

#include <godot_cpp/classes/color_rect.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/texture_button.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/string.hpp>

#include "nova_mnu_widget_common.h"

namespace godot {

class NovaMnuMenu;

// A menu checkbox (port of mnu_checkbox.gd): a toggle TextureButton that swaps
// its normal texture between the unchecked (default) and checked (selected) art
// and draws an underline in the font's selected colour while checked. Plays
// click/hover sounds through the owning NovaMnuMenu. Inert in edit_mode.
class NovaMnuCheckBox : public TextureButton {
	GDCLASS(NovaMnuCheckBox, TextureButton)

private:
	Color underline_color_ = Color(1, 0, 0, 1);
	float underline_thickness_ = 2.0f;
	MnuWidgetSounds sounds_;
	NovaMnuMenu *menu_ = nullptr;
	bool edit_mode_ = false;

	ColorRect *underline_ = nullptr;
	Ref<Texture2D> tex_unchecked_;
	Ref<Texture2D> tex_checked_;

	void on_toggled(bool p_pressed);
	void on_mouse_entered();
	void on_mouse_exited();
	void play_state_sound(int p_state);
	void update_appearance();
	void update_underline_position();

protected:
	static void _bind_methods();

public:
	void _ready() override;

	// --- Build-time configuration (called by nova_mnu_builder) ---
	void set_menu(NovaMnuMenu *p_menu) { menu_ = p_menu; }
	void set_edit_mode(bool p_edit) { edit_mode_ = p_edit; }
	void set_underline_color(const Color &p_color) { underline_color_ = p_color; }
	void set_sounds(const MnuWidgetSounds &p_sounds) { sounds_ = p_sounds; }
};

} // namespace godot
