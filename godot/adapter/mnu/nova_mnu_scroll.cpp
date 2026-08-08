#include "nova_mnu_scroll.h"

#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/range.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/rect2.hpp>

using namespace godot;

static double clampd(double v, double lo, double hi) {
	if (hi < lo) {
		hi = lo;
	}
	return v < lo ? lo : (v > hi ? hi : v);
}

double NovaMnuScroll::usable_max() const {
	const double um = max_ - page_;
	return um < min_ ? min_ : um;
}

void NovaMnuScroll::build_parts() {
	track_ = memnew(TextureRect);
	track_->set_name("Track");
	track_->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	track_->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
	track_->set_stretch_mode(TextureRect::STRETCH_SCALE);
	if (track_tex_.is_valid()) {
		track_->set_texture(track_tex_);
	}
	add_child(track_);

	arrow_up_ = memnew(TextureButton);
	arrow_up_->set_name("ArrowUp");
	arrow_up_->set_ignore_texture_size(true);
	arrow_up_->set_stretch_mode(TextureButton::STRETCH_SCALE);
	if (up_tex_.is_valid()) {
		arrow_up_->set_texture_normal(up_tex_);
	}
	if (up_hover_tex_.is_valid()) {
		arrow_up_->set_texture_hover(up_hover_tex_);
	}
	if (up_pressed_tex_.is_valid()) {
		arrow_up_->set_texture_pressed(up_pressed_tex_);
	}
	if (up_disabled_tex_.is_valid()) {
		arrow_up_->set_texture_disabled(up_disabled_tex_);
	}
	add_child(arrow_up_);

	arrow_down_ = memnew(TextureButton);
	arrow_down_->set_name("ArrowDown");
	arrow_down_->set_ignore_texture_size(true);
	arrow_down_->set_stretch_mode(TextureButton::STRETCH_SCALE);
	if (down_tex_.is_valid()) {
		arrow_down_->set_texture_normal(down_tex_);
	}
	if (down_hover_tex_.is_valid()) {
		arrow_down_->set_texture_hover(down_hover_tex_);
	}
	if (down_pressed_tex_.is_valid()) {
		arrow_down_->set_texture_pressed(down_pressed_tex_);
	}
	if (down_disabled_tex_.is_valid()) {
		arrow_down_->set_texture_disabled(down_disabled_tex_);
	}
	add_child(arrow_down_);

	shuttle_ = memnew(TextureRect);
	shuttle_->set_name("Shuttle");
	// All pointer interaction is handled by the scroll's own _gui_input, so the
	// shuttle (and track) stay click-through and the scroll sees the whole rect.
	shuttle_->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	shuttle_->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
	shuttle_->set_stretch_mode(TextureRect::STRETCH_SCALE);
	if (shuttle_tex_.is_valid()) {
		shuttle_->set_texture(shuttle_tex_);
	}
	add_child(shuttle_);

	if (behavior_.edit_mode) {
		arrow_up_->set_disabled(true);
		arrow_down_->set_disabled(true);
		if (shuttle_disabled_tex_.is_valid()) {
			shuttle_->set_texture(shuttle_disabled_tex_);
		}
	}
}

void NovaMnuScroll::layout() {
	if (track_ == nullptr) {
		return;
	}
	const Vector2 sz = get_size();
	const float extent = (float)arrow_extent_;
	const float axis_len = vertical_ ? sz.y : sz.x;
	const float cross_len = vertical_ ? sz.x : sz.y;
	const float track_len = axis_len - 2.0f * extent > 0.0f ? axis_len - 2.0f * extent : 0.0f;

	// Shuttle length: proportional to page/range when a range is set, else a fixed
	// grabber.
	float shuttle_len;
	const double range = max_ - min_;
	if (range > 0.0 && page_ > 0.0) {
		shuttle_len = (float)clampd(track_len * (page_ / range), 12.0, (double)track_len);
	} else {
		shuttle_len = track_len < 16.0f ? track_len : 16.0f;
	}
	travel_ = track_len - shuttle_len > 0.0f ? track_len - shuttle_len : 0.0f;

	const double denom = usable_max() - min_;
	const double frac = denom > 0.0 ? clampd((value_ - min_) / denom, 0.0, 1.0) : 0.0;
	const float shuttle_off = extent + (float)(frac * travel_);

	if (vertical_) {
		arrow_up_->set_position(Vector2(0, 0));
		arrow_up_->set_size(Vector2(cross_len, extent));
		arrow_down_->set_position(Vector2(0, sz.y - extent));
		arrow_down_->set_size(Vector2(cross_len, extent));
		track_->set_position(Vector2(0, extent));
		track_->set_size(Vector2(cross_len, track_len));
		shuttle_->set_position(Vector2(0, shuttle_off));
		shuttle_->set_size(Vector2(cross_len, shuttle_len));
	} else {
		arrow_up_->set_position(Vector2(0, 0));
		arrow_up_->set_size(Vector2(extent, cross_len));
		arrow_down_->set_position(Vector2(sz.x - extent, 0));
		arrow_down_->set_size(Vector2(extent, cross_len));
		track_->set_position(Vector2(extent, 0));
		track_->set_size(Vector2(track_len, cross_len));
		shuttle_->set_position(Vector2(shuttle_off, 0));
		shuttle_->set_size(Vector2(shuttle_len, cross_len));
	}
}

void NovaMnuScroll::_ready() {
	build_parts();
	layout();
	connect("resized", callable_mp(this, &NovaMnuScroll::layout));
	if (!range_target_path_.is_empty()) {
		sync_range_target();
	}
	if (behavior_.edit_mode) {
		// Inert while authoring.
		return;
	}
	arrow_up_->connect("pressed", callable_mp(this, &NovaMnuScroll::on_arrow_up));
	arrow_down_->connect("pressed", callable_mp(this, &NovaMnuScroll::on_arrow_down));
	connect("mouse_entered", callable_mp(this, &NovaMnuScroll::on_sound_mouse_entered));
	connect("mouse_exited", callable_mp(this, &NovaMnuScroll::on_sound_mouse_exited));
}
void NovaMnuScroll::on_sound_mouse_entered() {
	behavior_.play_hover();
}

void NovaMnuScroll::on_sound_mouse_exited() {
	behavior_.play_mouseout();
	if (!dragging_) {
		update_shuttle_texture(false, false);
	}
}

void NovaMnuScroll::on_arrow_up() {
	behavior_.play_click();
	set_value(value_ - step_);
	behavior_.notify_value(String(get_name()), "scroll", -1, String::num(value_));
}

void NovaMnuScroll::on_arrow_down() {
	behavior_.play_click();
	set_value(value_ + step_);
	behavior_.notify_value(String(get_name()), "scroll", -1, String::num(value_));
}

void NovaMnuScroll::_gui_input(const Ref<InputEvent> &p_event) {
	if (behavior_.edit_mode || shuttle_ == nullptr) {
		return;
	}
	const Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid() && mb->get_button_index() == MOUSE_BUTTON_LEFT) {
		if (mb->is_pressed()) {
			const Rect2 sr(shuttle_->get_position(), shuttle_->get_size());
			if (sr.has_point(mb->get_position())) {
				dragging_ = true;
				update_shuttle_texture(true, true);
				behavior_.play_click();
			} else {
				// Page toward the click (above/left of the shuttle pages up).
				const float click = vertical_ ? mb->get_position().y : mb->get_position().x;
				const float sh = vertical_ ? shuttle_->get_position().y : shuttle_->get_position().x;
				const double delta = page_ > 0.0 ? page_ : step_;
				set_value(click < sh ? value_ - delta : value_ + delta);
				behavior_.play_click();
				behavior_.notify_value(String(get_name()), "scroll", -1, String::num(value_));
			}
		} else {
			dragging_ = false;
			const Rect2 sr(shuttle_->get_position(), shuttle_->get_size());
			update_shuttle_texture(sr.has_point(mb->get_position()), false);
		}
		accept_event();
		return;
	}
	const Ref<InputEventMouseMotion> mm = p_event;
	if (mm.is_valid() && !dragging_) {
		const Rect2 sr(shuttle_->get_position(), shuttle_->get_size());
		update_shuttle_texture(sr.has_point(mm->get_position()), false);
	}
	if (mm.is_valid() && dragging_ && travel_ > 0.0) {
		const double rng = usable_max() - min_;
		if (rng > 0.0) {
			const double moved = vertical_ ? mm->get_relative().y : mm->get_relative().x;
			set_value(value_ + (moved / travel_) * rng);
			behavior_.notify_value(String(get_name()), "scroll", -1, String::num(value_));
		}
		accept_event();
	}
}

void NovaMnuScroll::update_shuttle_texture(bool p_hovered, bool p_pressed) {
	if (shuttle_ == nullptr) {
		return;
	}
	if (p_pressed && shuttle_pressed_tex_.is_valid()) {
		shuttle_->set_texture(shuttle_pressed_tex_);
	} else if (p_hovered && shuttle_hover_tex_.is_valid()) {
		shuttle_->set_texture(shuttle_hover_tex_);
	} else if (shuttle_tex_.is_valid()) {
		shuttle_->set_texture(shuttle_tex_);
	}
}

Range *NovaMnuScroll::range_target() const {
	if (range_target_path_.is_empty()) {
		return nullptr;
	}
	return Object::cast_to<Range>(get_node_or_null(range_target_path_));
}

void NovaMnuScroll::sync_range_target() {
	Range *range = range_target();
	if (range == nullptr) {
		return;
	}
	const Callable value_cb = callable_mp(this, &NovaMnuScroll::on_range_value_changed);
	const Callable changed_cb = callable_mp(this, &NovaMnuScroll::sync_range_target);
	if (!range->is_connected("value_changed", value_cb)) {
		range->connect("value_changed", value_cb);
	}
	if (!range->is_connected("changed", changed_cb)) {
		range->connect("changed", changed_cb);
	}
	syncing_range_ = true;
	min_ = range->get_min();
	max_ = range->get_max();
	page_ = range->get_page();
	step_ = range->get_step();
	value_ = range->get_value();
	syncing_range_ = false;
	layout();
}

void NovaMnuScroll::on_range_value_changed(double p_value) {
	if (syncing_range_) {
		return;
	}
	syncing_range_ = true;
	value_ = clampd(p_value, min_, usable_max());
	syncing_range_ = false;
	layout();
}

void NovaMnuScroll::apply_scroll_target() {
	if (!range_target_path_.is_empty()) {
		Range *range = range_target();
		if (range != nullptr && !syncing_range_) {
			syncing_range_ = true;
			range->set_value(value_);
			syncing_range_ = false;
		}
		return;
	}
	if (scroll_target_path_.is_empty()) {
		return;
	}
	Control *c = Object::cast_to<Control>(get_node_or_null(scroll_target_path_));
	if (c == nullptr) {
		return;
	}
	Vector2 p = c->get_position();
	if (vertical_) {
		p.y = (float)-value_;
	} else {
		p.x = (float)-value_;
	}
	c->set_position(p);
}

void NovaMnuScroll::set_min(double p_v) {
	min_ = p_v;
	set_value(value_); // re-clamp + re-layout
}

void NovaMnuScroll::set_max(double p_v) {
	max_ = p_v;
	set_value(value_);
}

void NovaMnuScroll::set_page(double p_v) {
	page_ = p_v;
	set_value(value_);
}

void NovaMnuScroll::set_value(double p_v) {
	const double clamped = clampd(p_v, min_, usable_max());
	const bool changed = clamped != value_;
	value_ = clamped;
	layout();
	apply_scroll_target();
	if (changed) {
		emit_signal("value_changed", value_);
	}
}

void NovaMnuScroll::set_range(double p_min, double p_max, double p_page) {
	min_ = p_min;
	max_ = p_max;
	page_ = p_page;
	set_value(value_);
}

void NovaMnuScroll::link_scroll_target(const NodePath &p_path) {
	range_target_path_ = NodePath();
	scroll_target_path_ = p_path;
	apply_scroll_target();
}

void NovaMnuScroll::link_range_target(const NodePath &p_path) {
	scroll_target_path_ = NodePath();
	range_target_path_ = p_path;
	if (is_inside_tree()) {
		sync_range_target();
	}
}

double NovaMnuScroll::get_ratio() const {
	const double denom = usable_max() - min_;
	return denom > 0.0 ? (value_ - min_) / denom : 0.0;
}

void NovaMnuScroll::set_ratio(double p_ratio) {
	set_value(min_ + clampd(p_ratio, 0.0, 1.0) * (usable_max() - min_));
}

void NovaMnuScroll::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_orientation_vertical", "vertical"), &NovaMnuScroll::set_orientation_vertical);
	ClassDB::bind_method(D_METHOD("is_vertical"), &NovaMnuScroll::is_vertical);
	ClassDB::bind_method(D_METHOD("set_min", "value"), &NovaMnuScroll::set_min);
	ClassDB::bind_method(D_METHOD("get_min"), &NovaMnuScroll::get_min);
	ClassDB::bind_method(D_METHOD("set_max", "value"), &NovaMnuScroll::set_max);
	ClassDB::bind_method(D_METHOD("get_max"), &NovaMnuScroll::get_max);
	ClassDB::bind_method(D_METHOD("set_page", "value"), &NovaMnuScroll::set_page);
	ClassDB::bind_method(D_METHOD("get_page"), &NovaMnuScroll::get_page);
	ClassDB::bind_method(D_METHOD("set_step", "value"), &NovaMnuScroll::set_step);
	ClassDB::bind_method(D_METHOD("get_step"), &NovaMnuScroll::get_step);
	ClassDB::bind_method(D_METHOD("set_value", "value"), &NovaMnuScroll::set_value);
	ClassDB::bind_method(D_METHOD("get_value"), &NovaMnuScroll::get_value);
	ClassDB::bind_method(D_METHOD("set_range", "min", "max", "page"), &NovaMnuScroll::set_range);
	ClassDB::bind_method(D_METHOD("link_scroll_target", "path"), &NovaMnuScroll::link_scroll_target);
	ClassDB::bind_method(D_METHOD("link_range_target", "path"), &NovaMnuScroll::link_range_target);
	ClassDB::bind_method(D_METHOD("get_ratio"), &NovaMnuScroll::get_ratio);
	ClassDB::bind_method(D_METHOD("set_ratio", "ratio"), &NovaMnuScroll::set_ratio);

	ADD_SIGNAL(MethodInfo("value_changed", PropertyInfo(Variant::FLOAT, "value")));
}
