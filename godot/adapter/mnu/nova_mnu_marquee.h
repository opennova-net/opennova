#pragma once

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/string.hpp>

#include "nova_mnu_widget_behavior.h"

namespace godot {

class NovaMnuMenu;

// A scrolling credits/marquee window (type="marquee" / marquee_wnd). The builder
// resolves the DATASOURCE text file through the resource root (falling back to the
// STRING text or a sample in edit_mode); a shell can also push content at runtime.
// The clipped Label auto-scrolls (vertically by default, horizontally if
// ORIENTATION is HORIZONTAL). Static (non-scrolling) in edit_mode.
//
// Honest scope: a plain text scroller. Rich .cbin credits (images, columns) stay
// with NovaCreditsPlayer.
class NovaMnuMarquee : public Control {
	GDCLASS(NovaMnuMarquee, Control)

private:
	MnuWidgetBehavior behavior_;
	Label *label_ = nullptr;
	String content_;
	float scroll_speed_ = 30.0f; // pixels per second
	float offset_ = 0.0f;
	bool vertical_ = true;
	bool loop_ = true;
	bool completed_ = false;
	Ref<Font> label_font_;
	int label_font_size_ = 0;
	bool has_font_color_ = false;
	Color font_color_ = Color(1, 1, 1, 1);
	int justify_ = 1; // HORIZONTAL_ALIGNMENT_CENTER

	void ensure_label();
	void apply_offset();
	float content_extent() const;
	void start_scroll();

protected:
	static void _bind_methods();

public:
	void _ready() override;
	void _process(double p_delta) override;

	// --- Build-time configuration ---
	void set_menu(NovaMnuMenu *p_menu) { behavior_.set_menu(p_menu); }
	void set_edit_mode(bool p_edit) { behavior_.set_edit_mode(p_edit); }
	void set_label_font(const Ref<Font> &p_font, int p_size) {
		label_font_ = p_font;
		label_font_size_ = p_size;
	}
	void set_label_font_color(const Color &p_color) {
		font_color_ = p_color;
		has_font_color_ = true;
	}
	void set_justify(int p_justify) { justify_ = p_justify; }

	// --- Runtime data binding ---
	void set_content(const String &p_text);
	String get_content() const { return content_; }
	void set_scroll_speed(float p_px_per_sec) { scroll_speed_ = p_px_per_sec; }
	float get_scroll_speed() const { return scroll_speed_; }
	void set_orientation_vertical(bool p_vertical) { vertical_ = p_vertical; }
	bool is_vertical() const { return vertical_; }
	void set_loop(bool p_loop) { loop_ = p_loop; }
	bool get_loop() const { return loop_; }
	void reset_scroll();
};

} // namespace godot
