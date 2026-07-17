#include "nova_mnu_combo.h"

#include "mnu_outline.h"
#include "nova_mnu_menu.h"

#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/color_rect.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/scroll_container.hpp>
#include <godot_cpp/classes/style_box_flat.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/transform2d.hpp>

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

Control *NovaMnuCombo::popup_root() const {
	if (popup_root_id_.is_null()) {
		return nullptr;
	}
	return Object::cast_to<Control>(ObjectDB::get_instance(popup_root_id_));
}

bool NovaMnuCombo::is_popup_open() const {
	return popup_root() != nullptr;
}

Control *NovaMnuCombo::get_popup() const {
	return popup_root() != nullptr ? popup_ : nullptr;
}

// [orig: CComboWnd @ 0x65be40 (embedded CListWnd at this+1536); CComboWnd_Render
// @ 0x65bfd0; the popup is the CListWnd's own window rect (this+13) set from the
// authored <LIST_BOX> POSITION.]
//
// While open, the dropdown owns the mouse: the original routes every mouse event
// exclusively to the open CListWnd and pumps hover/press only on it, so every
// other widget is input-dead until the list closes [orig: dispatch_mouse_event
// @ 0x63ab00 (g_ui_open_popup_wnd gate @ 0x63abb5); scene_end_frame @ 0x63e600
// (@ 0x63e691); CWnd_IsVisibleInHierarchy @ 0x646290 (@ 0x646299)]. The reimpl
// hosts that exclusivity as a full-menu transparent catcher (the overlay) added
// as the owning menu's LAST child, with the popup box inside it: last-in-tree
// wins Godot mouse picking and draw order, the catcher swallows everything that
// misses the box, and the menu enforces the one-open-dropdown invariant
// [orig: g_ui_active_combo_wnd, single-open toggle @ 0x65c210]. A combo without
// an owning menu (bare host/test builds; the original has no such case — every
// CComboWnd lives in a scene) keeps the legacy child-of-combo popup, which gets
// draw-on-top via z but no input exclusivity. docs/mnu/menu-re.md D-MNU-11/12.
void NovaMnuCombo::open_popup() {
	if (behavior_.edit_mode) {
		return;
	}
	close_popup();

	NovaMnuMenu *menu = behavior_.menu;
	const bool overlay_mode = menu != nullptr && is_inside_tree() &&
			menu->is_inside_tree() && menu->is_ancestor_of(this);

	popup_ = memnew(Control);
	popup_->set_name("Popup");
	popup_->set_mouse_filter(Control::MOUSE_FILTER_STOP); // in-box presses never reach the catcher
	if (!overlay_mode) {
		popup_->set_z_as_relative(false);
		popup_->set_z_index(4096); // legacy path: lift above siblings in the same CanvasLayer
	}
	Control *host = this;
	Vector2 combo_origin; // the combo's origin expressed in host space
	if (overlay_mode) {
		// One dropdown per menu: opening this one closes the active one first
		// [orig: combobox_handle_event @ 0x65c210 sends the active combo 0x3000001].
		menu->register_open_combo(this);

		Control *overlay = memnew(Control);
		overlay->set_name("ComboPopupOverlay");
		overlay->set_mouse_filter(Control::MOUSE_FILTER_STOP);
		overlay->connect("gui_input", callable_mp(this, &NovaMnuCombo::on_overlay_gui_input));
		menu->add_child(overlay); // last child of the menu: wins picking and draws on top
		// Cover the menu rect explicitly (anchor presets lay out deferred; the rect
		// is needed now for the popup placement below). Menus are fixed-size in both
		// hosts (the shell pins 800x600 design space). A degenerate menu rect (bare
		// test hosts) falls back to covering the viewport.
		const Vector2 cover = menu->get_size();
		if (cover.x < 1.0f || cover.y < 1.0f) {
			const Transform2D vp_to_menu = menu->get_global_transform().affine_inverse();
			const Rect2 vp = menu->get_viewport_rect();
			overlay->set_position(vp_to_menu.xform(vp.position));
			overlay->set_size(vp.size);
		} else {
			overlay->set_position(Vector2());
			overlay->set_size(cover);
		}
		popup_root_id_ = overlay->get_instance_id();
		host = overlay;
		const Transform2D combo_to_overlay =
				overlay->get_global_transform().affine_inverse() * get_global_transform();
		combo_origin = combo_to_overlay.xform(Vector2());
	} else {
		popup_root_id_ = popup_->get_instance_id();
	}

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
		// drop below the combo, clamped so a long list scrolls instead of running
		// off-canvas. In overlay mode the clamp is against the overlay (menu design
		// space, scale-correct); the legacy path keeps the raw window extent.
		width = get_size().x;
		const float natural = static_cast<float>(items_.size() * item_height);
		float avail;
		if (overlay_mode) {
			avail = host->get_size().y - (combo_origin.y + get_size().y);
		} else {
			avail = get_viewport_rect().size.y - (get_global_position().y + get_size().y);
		}
		if (avail < static_cast<float>(item_height)) {
			avail = static_cast<float>(item_height); // never collapse to nothing
		}
		pos_x = 0.0f;
		pos_y = get_size().y;
		height = natural < avail ? natural : avail;
	}
	popup_->set_position(combo_origin + Vector2(pos_x, pos_y));
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

	host->add_child(popup_);
	emit_signal("popup_opened");
}

void NovaMnuCombo::close_popup() {
	Control *root = popup_root();
	popup_root_id_ = ObjectID();
	popup_ = nullptr;
	if (root == nullptr) {
		return;
	}
	if (behavior_.menu != nullptr) {
		// [orig: UI_ClearActiveComboWnd @ 0x646400 on every close path]
		behavior_.menu->unregister_open_combo(this);
	}
	// Detach before the deferred free so the dying catcher neither draws, eats
	// input, nor squats on the node name for the remainder of the frame
	// (single-open swaps spawn the replacement overlay in the same frame).
	if (root->get_parent() != nullptr) {
		root->get_parent()->remove_child(root);
	}
	root->queue_free();
	emit_signal("popup_closed");
}

// A left press that reaches the catcher missed the popup box. The original closes
// the dropdown only when the point is outside BOTH the closed cell and the list
// rect; a press on the (input-dead) closed cell does nothing while the list is
// open. Either way nothing beneath the overlay ever sees the press -- the
// dismissing click is consumed, exactly like the original's exclusive routing.
// [orig: combobox_handle_event @ 0x65c190, outside check @ 0x65c290 on events
// 0x1000002/0x1000004]
void NovaMnuCombo::on_overlay_gui_input(const Ref<InputEvent> &p_event) {
	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_null() || !mb->is_pressed() || mb->get_button_index() != MOUSE_BUTTON_LEFT) {
		return;
	}
	Control *overlay = popup_root();
	if (overlay == nullptr) {
		return;
	}
	const Transform2D combo_to_overlay =
			overlay->get_global_transform().affine_inverse() * get_global_transform();
	const Rect2 combo_rect(combo_to_overlay.xform(Vector2()), get_size());
	if (!combo_rect.has_point(mb->get_position())) {
		close_popup();
	}
}

void NovaMnuCombo::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_EXIT_TREE: {
			// The overlay is parked on the menu, not under this combo -- never leave
			// it behind. [orig: CWnd_Destructor clears the popup/capture globals
			// @ 0x6474a2..0x6474bc]
			close_popup();
		} break;
		case NOTIFICATION_VISIBILITY_CHANGED: {
			// A hidden dropdown owner would leave an unreachable exclusive overlay
			// (the original's equivalent widget fails CWnd_IsVisibleInHierarchy
			// @ 0x646290 and self-heals on the next outside press); close eagerly.
			if (!is_visible_in_tree() && is_popup_open()) {
				close_popup();
			}
		} break;
		default:
			break;
	}
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
	ClassDB::bind_method(D_METHOD("get_popup"), &NovaMnuCombo::get_popup);

	ADD_SIGNAL(MethodInfo("item_selected", PropertyInfo(Variant::INT, "index"),
			PropertyInfo(Variant::STRING, "value")));
	ADD_SIGNAL(MethodInfo("popup_opened"));
	ADD_SIGNAL(MethodInfo("popup_closed"));
}
