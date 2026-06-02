#pragma once

#include <godot_cpp/classes/color_rect.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/texture_button.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/string.hpp>

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
	String hover_sound_trigger_;
	String hover_sound_file_;
	String click_sound_trigger_;
	String click_sound_file_;
	NovaMnuMenu *menu_ = nullptr;
	bool edit_mode_ = false;

	ColorRect *underline_ = nullptr;
	Ref<Texture2D> tex_unchecked_;
	Ref<Texture2D> tex_checked_;

	void on_toggled(bool p_pressed);
	void on_mouse_entered();
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
	void set_hover_sound(const String &p_trigger, const String &p_file) {
		hover_sound_trigger_ = p_trigger;
		hover_sound_file_ = p_file;
	}
	void set_click_sound(const String &p_trigger, const String &p_file) {
		click_sound_trigger_ = p_trigger;
		click_sound_file_ = p_file;
	}
};

} // namespace godot
