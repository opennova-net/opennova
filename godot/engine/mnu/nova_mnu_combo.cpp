#include "nova_mnu_combo.h"

#include "mnu_outline.h"

#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/color_rect.hpp>
#include <godot_cpp/classes/scroll_container.hpp>
#include <godot_cpp/classes/style_box_flat.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/node_path.hpp>

using namespace godot;

void NovaMnuCombo::_ready() {
	selected_label_ = Object::cast_to<Label>(get_node_or_null(NodePath("SelectedText")));
	update_selected_label();
	if (behavior_.edit_mode) {
		// Inert while authoring: the closed state shows, the popup never opens.
		return;
	}
	connect("pressed", callable_mp(this, &NovaMnuCombo::on_pressed));
	connect("mouse_entered", callable_mp(this, &NovaMnuCombo::on_sound_mouse_entered));
	connect("mouse_exited", callable_mp(this, &NovaMnuCombo::on_sound_mouse_exited));
}
void NovaMnuCombo::on_sound_mouse_entered() {
	behavior_.play_hover();
}

void NovaMnuCombo::on_sound_mouse_exited() {
	behavior_.play_mouseout();
}

void NovaMnuCombo::on_pressed() {
	behavior_.play_click();
	if (is_popup_open()) {
		close_popup();
	} else {
		open_popup();
	}
}

void NovaMnuCombo::update_selected_label() {
	if (selected_label_ == nullptr) {
		return;
	}
	if (selected_index_ >= 0 && selected_index_ < static_cast<int>(items_.size())) {
		selected_label_->set_text(items_[selected_index_].text);
	} else {
		selected_label_->set_text(String());
	}
}

void NovaMnuCombo::clear_items() {
	items_.clear();
	selected_index_ = -1;
	close_popup();
	update_selected_label();
}

int NovaMnuCombo::add_item(const String &p_text, const String &p_value) {
	items_.push_back(ComboItem{ p_text, p_value });
	return static_cast<int>(items_.size()) - 1;
}

void NovaMnuCombo::set_items(const PackedStringArray &p_texts) {
	items_.clear();
	for (int i = 0; i < p_texts.size(); ++i) {
		items_.push_back(ComboItem{ p_texts[i], String() });
	}
	if (selected_index_ >= static_cast<int>(items_.size())) {
		selected_index_ = -1;
	}
	close_popup();
	update_selected_label();
}

String NovaMnuCombo::get_item_text(int p_index) const {
	if (p_index < 0 || p_index >= static_cast<int>(items_.size())) {
		return String();
	}
	return items_[p_index].text;
}

String NovaMnuCombo::get_item_value(int p_index) const {
	if (p_index < 0 || p_index >= static_cast<int>(items_.size())) {
		return String();
	}
	return items_[p_index].value;
}

void NovaMnuCombo::select_silent(int p_index) {
	if (p_index < -1 || p_index >= static_cast<int>(items_.size())) {
		return;
	}
	selected_index_ = p_index;
	update_selected_label();
}

void NovaMnuCombo::select(int p_index) {
	if (p_index < -1 || p_index >= static_cast<int>(items_.size())) {
		return;
	}
	selected_index_ = p_index;
	update_selected_label();
	const String value = p_index >= 0 ? items_[p_index].value : String();
	const String text = p_index >= 0 ? items_[p_index].text : String();
	emit_signal("item_selected", p_index, value);
	behavior_.notify_value(String(get_name()), "combo", p_index, text);
}

String NovaMnuCombo::get_selected_value() const {
	return get_item_value(selected_index_);
}

void NovaMnuCombo::on_row_pressed(int p_index) {
	select(p_index);
	close_popup();
}

// Per-row height. [orig: CListWnd_DrawItems @ 0x643f30: row_height = height("W")
// in the list font (sub_653680 @ 0x653680), overridden by this+201 (the <MI> /
// <MIN_ITEM_HEIGHT> value) only when >= 0]. The reimpl mirrors that: the authored
// MIN_ITEM_HEIGHT wins; otherwise the item font's line height; the 16px default is
// the last resort (headless / no font). docs/mnu/menu-re.md D-MNU-8.
int NovaMnuCombo::effective_item_height() const {
	if (has_explicit_item_height_) {
		return min_item_height_;
	}
	if (item_font_.is_valid()) {
		const int fs = item_font_size_ > 0 ? item_font_size_ : 16;
		const int h = static_cast<int>(item_font_->get_height(fs) + 0.5f);
		if (h > 0) {
			return h;
		}
	}
	return min_item_height_;
}

// [orig: CComboWnd @ 0x65be40 (embedded CListWnd at this+1536); CComboWnd_Render
// @ 0x65bfd0; the popup is the CListWnd's own window rect (this+13) set from the
// authored <LIST_BOX> POSITION, drawn over whatever sits beneath it.]
void NovaMnuCombo::open_popup() {
	if (behavior_.edit_mode) {
		return;
	}
	close_popup();

	popup_ = memnew(Control);
	popup_->set_name("Popup");
	popup_->set_z_as_relative(false);
	popup_->set_z_index(4096); // lift above sibling widgets in the same CanvasLayer
	const int item_height = effective_item_height();
	float pos_x;
	float pos_y;
	float width;
	float height;
	if (has_popup_rect_) {
		// Faithful path: the authored <LIST_BOX> POSITION rect (combo-relative, design
		// space). This is the engine's behavior -- the dropdown is a fixed rect that can
		// sit below, beside, or above the combo (e.g. PLAYERVOICE opens upward with a
		// negative TOP), drawn over whatever is beneath. Recomputing it below the combo
		// dropped the semi-transparent list onto sibling combos, whose text bled through
		// (docs/mnu/menu-re.md D-MNU-7).
		pos_x = popup_rect_.position.x;
		pos_y = popup_rect_.position.y;
		width = popup_rect_.size.x;
		height = popup_rect_.size.y;
	} else {
		// Fallback for combos with no authored LIST_BOX rect (e.g. host-built browsers):
		// drop below the combo, clamped to the window so a long list scrolls instead of
		// running off-canvas. The popup only opens at runtime (edit_mode suppresses it),
		// so get_viewport_rect() is the game window extent.
		width = get_size().x;
		const float natural = static_cast<float>(items_.size() * item_height);
		float avail = get_viewport_rect().size.y - (get_global_position().y + get_size().y);
		if (avail < static_cast<float>(item_height)) {
			avail = static_cast<float>(item_height); // never collapse to nothing
		}
		pos_x = 0.0f;
		pos_y = get_size().y;
		height = natural < avail ? natural : avail;
	}
	popup_->set_position(Vector2(pos_x, pos_y));
	popup_->set_size(Vector2(width, height));

	if (popup_bg_tex_.is_valid()) {
		TextureRect *bg = memnew(TextureRect);
		bg->set_name("Background");
		bg->set_texture(popup_bg_tex_);
		bg->set_anchors_preset(Control::PRESET_FULL_RECT);
		bg->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
		bg->set_stretch_mode(TextureRect::STRETCH_SCALE);
		bg->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
		popup_->add_child(bg);
	} else if (has_popup_bg_color_) {
		ColorRect *bg = memnew(ColorRect);
		bg->set_name("Background");
		bg->set_color(popup_bg_color_);
		bg->set_anchors_preset(Control::PRESET_FULL_RECT);
		bg->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
		popup_->add_child(bg);
	}

	// Rows live inside a ScrollContainer so an overflowing list scrolls within the
	// clamped popup instead of spilling past the canvas. The background above stays
	// full-rect behind the (transparent) scroller; the outline below frames it.
	ScrollContainer *scroll = memnew(ScrollContainer);
	scroll->set_name("Scroll");
	scroll->set_anchors_preset(Control::PRESET_FULL_RECT);
	scroll->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
	popup_->add_child(scroll);

	VBoxContainer *rows = memnew(VBoxContainer);
	rows->set_name("Rows");
	rows->set_h_size_flags(Control::SIZE_EXPAND_FILL); // fill popup width; height stays natural so it scrolls
	rows->add_theme_constant_override("separation", 0);
	scroll->add_child(rows);

	Ref<StyleBoxFlat> hover_sb;
	hover_sb.instantiate();
	hover_sb->set_bg_color(selection_color_);

	for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
		Button *row = memnew(Button);
		row->set_name(String("Item") + String::num_int64(i));
		row->set_text(items_[i].text);
		row->set_flat(true);
		row->set_custom_minimum_size(Vector2(0, item_height));
		row->set_text_alignment(static_cast<HorizontalAlignment>(item_align_));
		if (item_font_.is_valid()) {
			row->add_theme_font_override("font", item_font_);
		}
		if (item_font_size_ > 0) {
			row->add_theme_font_size_override("font_size", item_font_size_);
		}
		if (has_item_font_color_) {
			row->add_theme_color_override("font_color", item_font_color_);
		}
		row->add_theme_stylebox_override("hover", hover_sb);
		row->connect("pressed", callable_mp(this, &NovaMnuCombo::on_row_pressed).bind(i));
		rows->add_child(row);
	}

	if (has_popup_outline_) {
		mnu_add_outline(popup_, popup_outline_color_, 1);
	}

	add_child(popup_);
	emit_signal("popup_opened");
}

void NovaMnuCombo::close_popup() {
	if (popup_ == nullptr) {
		return;
	}
	popup_->queue_free();
	popup_ = nullptr;
	emit_signal("popup_closed");
}

void NovaMnuCombo::_bind_methods() {
	ClassDB::bind_method(D_METHOD("clear_items"), &NovaMnuCombo::clear_items);
	ClassDB::bind_method(D_METHOD("add_item", "text", "value"), &NovaMnuCombo::add_item,
			DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("set_items", "texts"), &NovaMnuCombo::set_items);
	ClassDB::bind_method(D_METHOD("get_item_count"), &NovaMnuCombo::get_item_count);
	ClassDB::bind_method(D_METHOD("get_item_text", "index"), &NovaMnuCombo::get_item_text);
	ClassDB::bind_method(D_METHOD("get_item_value", "index"), &NovaMnuCombo::get_item_value);
	ClassDB::bind_method(D_METHOD("select", "index"), &NovaMnuCombo::select);
	ClassDB::bind_method(D_METHOD("select_silent", "index"), &NovaMnuCombo::select_silent);
	ClassDB::bind_method(D_METHOD("get_selected"), &NovaMnuCombo::get_selected);
	ClassDB::bind_method(D_METHOD("get_selected_value"), &NovaMnuCombo::get_selected_value);
	ClassDB::bind_method(D_METHOD("open_popup"), &NovaMnuCombo::open_popup);
	ClassDB::bind_method(D_METHOD("close_popup"), &NovaMnuCombo::close_popup);
	ClassDB::bind_method(D_METHOD("is_popup_open"), &NovaMnuCombo::is_popup_open);

	ADD_SIGNAL(MethodInfo("item_selected", PropertyInfo(Variant::INT, "index"),
			PropertyInfo(Variant::STRING, "value")));
	ADD_SIGNAL(MethodInfo("popup_opened"));
	ADD_SIGNAL(MethodInfo("popup_closed"));
}
