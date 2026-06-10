#pragma once

#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/texture_button.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/color.hpp>

#include <vector>

#include "nova_mnu_widget_behavior.h"

namespace godot {

class Control;
class NovaMnuMenu;

// A dropdown/combobox (type="combo"). The closed state is a TextureButton showing
// the selected item; clicking opens a popup styled from the parsed LIST_BOX. The
// popup is an in-tree layered Control (not a native Popup/Window) so it stays
// inside the menu's CanvasLayer transform, which matters in the ONED editor canvas
// that renders the menu at a fixed scaled 640x480; a native popup would escape it.
//
// The .mnu is a template: options may be seeded from LIST_BOX/ITEMS or populated at
// runtime via set_items()/add_item() (server browsers, option lists). Selection
// emits item_selected(index, value) and relays through the owning menu. In edit_mode
// the closed state shows but the popup never opens.
class NovaMnuCombo : public TextureButton {
	GDCLASS(NovaMnuCombo, TextureButton)

private:
	struct ComboItem {
		String text;
		String value;
	};

	MnuWidgetBehavior behavior_;
	std::vector<ComboItem> items_;
	int selected_index_ = -1;

	Label *selected_label_ = nullptr;
	Control *popup_ = nullptr;

	// Popup styling resolved at build time.
	bool has_popup_bg_color_ = false;
	Color popup_bg_color_ = Color(0.06f, 0.09f, 0.12f, 1.0f);
	Ref<Texture2D> popup_bg_tex_;
	bool has_popup_outline_ = false;
	Color popup_outline_color_ = Color(0.2f, 0.25f, 0.35f, 1.0f);
	Color selection_color_ = Color(0.2f, 0.4f, 0.8f, 1.0f);
	int min_item_height_ = 16;
	Ref<Font> item_font_;
	int item_font_size_ = 0;
	bool has_item_font_color_ = false;
	Color item_font_color_ = Color(1, 1, 1, 1);
	int item_align_ = 0; // HORIZONTAL_ALIGNMENT_LEFT

	void on_pressed();
	void on_sound_mouse_entered();
	void on_sound_mouse_exited();
	void on_row_pressed(int p_index);
	void update_selected_label();

protected:
	static void _bind_methods();

public:
	void _ready() override;

	// --- Build-time configuration ---
	void set_menu(NovaMnuMenu *p_menu) { behavior_.set_menu(p_menu); }
	void set_edit_mode(bool p_edit) { behavior_.set_edit_mode(p_edit); }
	void set_sounds(const MnuWidgetSounds &p_sounds) { behavior_.set_sounds(p_sounds); }
	void set_popup_bg_color(const Color &p_color) {
		popup_bg_color_ = p_color;
		has_popup_bg_color_ = true;
	}
	void set_popup_bg_texture(const Ref<Texture2D> &p_tex) { popup_bg_tex_ = p_tex; }
	void set_popup_outline_color(const Color &p_color) {
		popup_outline_color_ = p_color;
		has_popup_outline_ = true;
	}
	void set_selection_color(const Color &p_color) { selection_color_ = p_color; }
	void set_min_item_height(int p_h) {
		if (p_h > 0) {
			min_item_height_ = p_h;
		}
	}
	void set_item_font(const Ref<Font> &p_font, int p_size) {
		item_font_ = p_font;
		item_font_size_ = p_size;
	}
	void set_item_font_color(const Color &p_color) {
		item_font_color_ = p_color;
		has_item_font_color_ = true;
	}
	void set_item_alignment(int p_align) { item_align_ = p_align; }

	// --- Runtime data binding ---
	void clear_items();
	int add_item(const String &p_text, const String &p_value = String());
	void set_items(const PackedStringArray &p_texts);
	int get_item_count() const { return static_cast<int>(items_.size()); }
	String get_item_text(int p_index) const;
	String get_item_value(int p_index) const;
	void select(int p_index);
	void select_silent(int p_index);
	int get_selected() const { return selected_index_; }
	String get_selected_value() const;

	void open_popup();
	void close_popup();
	bool is_popup_open() const { return popup_ != nullptr; }
};

} // namespace godot
