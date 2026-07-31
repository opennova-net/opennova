#pragma once

#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/texture_button.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/rect2.hpp>

#include <vector>

#include "nova_mnu_widget_behavior.h"

namespace godot {

class Control;
class NovaMnuMenu;
class NovaMnuScroll;

// A dropdown/combobox (type="combo"). The closed state is a TextureButton showing
// the selected item; clicking opens a popup styled from the parsed LIST_BOX. The
// popup is an in-tree layered Control (not a native Popup/Window) so it stays
// inside the menu's CanvasLayer transform, which matters in the ONED editor canvas
// that renders the menu at a fixed scaled 640x480; a native popup would escape it.
//
// While open, the original engine routes mouse input EXCLUSIVELY to the dropdown
// list: the WM-message dispatch sends events only to the open list [orig:
// dispatch_mouse_event @ 0x63ab00], the per-frame hover/press pump runs only on it
// [orig: scene_end_frame @ 0x63e600], and visible-in-hierarchy fails for every
// widget outside the popup subtree [orig: CWnd_IsVisibleInHierarchy @ 0x646290].
// Only one dropdown can be open per scene [orig: g_ui_active_combo_wnd @
// 0x31C16D0], and a press outside both the closed cell and the list closes it,
// consumed [orig: combobox_handle_event @ 0x65c190]. The reimpl shells those
// semantics as a full-menu modal overlay added as the owning NovaMnuMenu's last
// child (top of tree = wins Godot picking and draw): the transparent catcher makes
// every other widget mouse-dead and implements the outside-press close, the menu
// tracks the single active combo, and screen navigation closes the popup [orig:
// CUIScene_SelectNodeByName @ 0x63b6b0]. docs/mnu/menu-re.md D-MNU-11/12.
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
	// The spawned popup root: the full-menu overlay (menu-built combos) or the
	// popup box itself (bare/shell combos with no owning menu). Tracked by id so
	// close stays safe across menu teardown ordering; popup_ is the styled box
	// with the rows, valid only while popup_root() resolves.
	ObjectID popup_root_id_;
	Control *popup_ = nullptr;
	NovaMnuScroll *popup_scrollbar_ = nullptr;

	// Popup styling resolved at build time.
	bool has_popup_bg_color_ = false;
	Color popup_bg_color_ = Color(0.06f, 0.09f, 0.12f, 1.0f);
	Ref<Texture2D> popup_bg_tex_;
	bool has_popup_outline_ = false;
	Color popup_outline_color_ = Color(0.2f, 0.25f, 0.35f, 1.0f);
	Color selection_color_ = Color(0.2f, 0.4f, 0.8f, 1.0f);
	int min_item_height_ = 16;
	bool has_explicit_item_height_ = false; // MIN_ITEM_HEIGHT authored on the LIST_BOX
	// The authored <LIST_BOX> POSITION rect, combo-relative in 800x600 design space.
	// When set, the dropdown opens at exactly this rect (the embedded CListWnd's own
	// window rect in the original); otherwise it falls back to a below-combo box.
	bool has_popup_rect_ = false;
	Rect2 popup_rect_;
	Ref<Font> item_font_;
	int item_font_size_ = 0;
	bool has_item_font_color_ = false;
	Color item_font_color_ = Color(1, 1, 1, 1);
	int item_align_ = 0; // HORIZONTAL_ALIGNMENT_LEFT
	int item_valign_ = 1; // VERTICAL_ALIGNMENT_CENTER
	int item_edge_ = 0;
	MnuScrollbarStyle scrollbar_style_;

	void on_pressed();
	void on_sound_mouse_entered();
	void on_sound_mouse_exited();
	void on_row_pressed(int p_index);
	// The modal catcher's input: a left press outside both the closed cell and the
	// popup box closes the dropdown; everything else is swallowed (exclusivity).
	void on_overlay_gui_input(const Ref<InputEvent> &p_event);
	void on_popup_scroll_input(const Ref<InputEvent> &p_event);
	void update_selected_label();
	// Resolve the live popup root (overlay or box), or null when closed/freed.
	Control *popup_root() const;
	// Per-row height: the authored MIN_ITEM_HEIGHT when present, else the item
	// font's line height (the original measures a "W" glyph), else the 16px default.
	int effective_item_height() const;

protected:
	static void _bind_methods();
	void _notification(int p_what);

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
			has_explicit_item_height_ = true;
		}
	}
	// The authored <LIST_BOX> POSITION rect (combo-relative, design space).
	void set_popup_rect(const Rect2 &p_rect) {
		popup_rect_ = p_rect;
		has_popup_rect_ = true;
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
	void set_item_vertical_alignment(int p_align) { item_valign_ = p_align; }
	void set_item_edge(int p_edge) { item_edge_ = p_edge > 0 ? p_edge : 0; }
	void set_scrollbar_style(const MnuScrollbarStyle &p_style) {
		scrollbar_style_ = p_style;
	}

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
	bool is_popup_open() const;
	// The styled popup box holding the rows (null while closed). Script-visible so
	// shells/tests can reach the rows wherever the box is parented.
	Control *get_popup() const;
};

} // namespace godot
