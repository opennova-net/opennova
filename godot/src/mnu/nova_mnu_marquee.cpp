#include "nova_mnu_marquee.h"

using namespace godot;

void MnuMarquee::ensure_label() {
	if (label_ != nullptr) {
		return;
	}
	label_ = memnew(Label);
	label_->set_name("Content");
	label_->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	label_->set_horizontal_alignment((HorizontalAlignment)justify_);
	label_->set_text(content_);
	if (label_font_.is_valid()) {
		label_->add_theme_font_override("font", label_font_);
	}
	if (label_font_size_ > 0) {
		label_->add_theme_font_size_override("font_size", label_font_size_);
	}
	if (has_font_color_) {
		label_->add_theme_color_override("font_color", font_color_);
	}
	add_child(label_);
}

float MnuMarquee::content_extent() const {
	if (label_ == nullptr) {
		return 0.0f;
	}
	const Vector2 ms = label_->get_minimum_size();
	return vertical_ ? ms.y : ms.x;
}

void MnuMarquee::apply_offset() {
	if (label_ == nullptr) {
		return;
	}
	if (vertical_) {
		label_->set_size(Vector2(get_size().x, label_->get_minimum_size().y));
		label_->set_position(Vector2(0, offset_));
	} else {
		label_->set_position(Vector2(offset_, 0));
	}
}

void MnuMarquee::start_scroll() {
	completed_ = false;
	offset_ = vertical_ ? get_size().y : get_size().x;
	apply_offset();
}

void MnuMarquee::_ready() {
	set_clip_contents(true);
	ensure_label();
	if (behavior_.edit_mode) {
		// Static while authoring: show the content pinned to the top.
		offset_ = 0.0f;
		apply_offset();
		set_process(false);
		return;
	}
	start_scroll();
	set_process(true);
}

void MnuMarquee::_process(double p_delta) {
	offset_ -= scroll_speed_ * (float)p_delta;
	const float extent = content_extent();
	if (offset_ <= -extent) {
		if (loop_) {
			offset_ = vertical_ ? get_size().y : get_size().x;
		} else {
			offset_ = -extent;
			if (!completed_) {
				completed_ = true;
				set_process(false);
				emit_signal("scroll_completed");
			}
		}
	}
	apply_offset();
}

void MnuMarquee::set_content(const String &p_text) {
	content_ = p_text;
	if (label_ != nullptr) {
		label_->set_text(content_);
	}
}

void MnuMarquee::reset_scroll() {
	start_scroll();
	set_process(!behavior_.edit_mode);
}

void MnuMarquee::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_content", "text"), &MnuMarquee::set_content);
	ClassDB::bind_method(D_METHOD("get_content"), &MnuMarquee::get_content);
	ClassDB::bind_method(D_METHOD("set_scroll_speed", "px_per_sec"), &MnuMarquee::set_scroll_speed);
	ClassDB::bind_method(D_METHOD("get_scroll_speed"), &MnuMarquee::get_scroll_speed);
	ClassDB::bind_method(D_METHOD("set_orientation_vertical", "vertical"), &MnuMarquee::set_orientation_vertical);
	ClassDB::bind_method(D_METHOD("is_vertical"), &MnuMarquee::is_vertical);
	ClassDB::bind_method(D_METHOD("set_loop", "loop"), &MnuMarquee::set_loop);
	ClassDB::bind_method(D_METHOD("get_loop"), &MnuMarquee::get_loop);
	ClassDB::bind_method(D_METHOD("reset_scroll"), &MnuMarquee::reset_scroll);

	ADD_SIGNAL(MethodInfo("scroll_completed"));
}
