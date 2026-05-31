#include "nova_mnu_combo.h"

#include "mnu_outline.h"

#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/color_rect.hpp>
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

void NovaMnuCombo::open_popup() {
	if (behavior_.edit_mode) {
		return;
	}
	close_popup();

	popup_ = memnew(Control);
	popup_->set_name("Popup");
	popup_->set_z_as_relative(false);
	popup_->set_z_index(4096); // lift above sibling widgets in the same CanvasLayer
	const float width = get_size().x;
	const float height = static_cast<float>(items_.size() * min_item_height_);
	popup_->set_position(Vector2(0, get_size().y));
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

	VBoxContainer *rows = memnew(VBoxContainer);
	rows->set_name("Rows");
	rows->set_anchors_preset(Control::PRESET_FULL_RECT);
	rows->add_theme_constant_override("separation", 0);
	popup_->add_child(rows);

	Ref<StyleBoxFlat> hover_sb;
	hover_sb.instantiate();
	hover_sb->set_bg_color(selection_color_);

	for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
		Button *row = memnew(Button);
		row->set_name(String("Item") + String::num_int64(i));
		row->set_text(items_[i].text);
		row->set_flat(true);
		row->set_custom_minimum_size(Vector2(0, min_item_height_));
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
