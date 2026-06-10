#include "nova_mnu_marquee.h"

using namespace godot;

void NovaMnuMarquee::ensure_label() {
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

float NovaMnuMarquee::content_extent() const {
	if (label_ == nullptr) {
		return 0.0f;
	}
	const Vector2 ms = label_->get_minimum_size();
	return vertical_ ? ms.y : ms.x;
}

void NovaMnuMarquee::apply_offset() {
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

void NovaMnuMarquee::start_scroll() {
	completed_ = false;
	offset_ = vertical_ ? get_size().y : get_size().x;
	apply_offset();
}

void NovaMnuMarquee::_ready() {
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

void NovaMnuMarquee::_process(double p_delta) {
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

void NovaMnuMarquee::set_content(const String &p_text) {
	content_ = p_text;
	if (label_ != nullptr) {
		label_->set_text(content_);
	}
}

void NovaMnuMarquee::reset_scroll() {
	start_scroll();
	set_process(!behavior_.edit_mode);
}

void NovaMnuMarquee::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_content", "text"), &NovaMnuMarquee::set_content);
	ClassDB::bind_method(D_METHOD("get_content"), &NovaMnuMarquee::get_content);
	ClassDB::bind_method(D_METHOD("set_scroll_speed", "px_per_sec"), &NovaMnuMarquee::set_scroll_speed);
	ClassDB::bind_method(D_METHOD("get_scroll_speed"), &NovaMnuMarquee::get_scroll_speed);
	ClassDB::bind_method(D_METHOD("set_orientation_vertical", "vertical"), &NovaMnuMarquee::set_orientation_vertical);
	ClassDB::bind_method(D_METHOD("is_vertical"), &NovaMnuMarquee::is_vertical);
	ClassDB::bind_method(D_METHOD("set_loop", "loop"), &NovaMnuMarquee::set_loop);
	ClassDB::bind_method(D_METHOD("get_loop"), &NovaMnuMarquee::get_loop);
	ClassDB::bind_method(D_METHOD("reset_scroll"), &NovaMnuMarquee::reset_scroll);

	ADD_SIGNAL(MethodInfo("scroll_completed"));
}
