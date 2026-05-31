#include "nova_mnu_builder.h"

#include "nova_mnu_screen.h"

#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/check_box.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/panel.hpp>
#include <godot_cpp/classes/style_box_flat.hpp>
#include <godot_cpp/variant/color.hpp>

#include <string>

using namespace godot;

namespace {

String to_gd(const std::string &s) {
	return String::utf8(s.c_str(), static_cast<int>(s.length()));
}

bool is_button_like(mnu::WindowType t) {
	return t == mnu::WindowType::Button || t == mnu::WindowType::Radio ||
			t == mnu::WindowType::CheckBox || t == mnu::WindowType::Combo ||
			t == mnu::WindowType::SpinList || t == mnu::WindowType::Scroll;
}

// Resolve the text a widget should display. Until RTXT/MNS resolution lands
// (M4) this returns the raw value (a string id like "MM_Start" shows literally).
String display_text(const mnu::Window &w, const MnuBuildContext &ctx) {
	(void)ctx;
	return to_gd(w.string_data.value);
}

// Replicates the reference positioning/sizing math (mnu_import_plugin.cpp
// ~1940-2049): MNU coordinates are absolute pixels; anchors stay at top-left and
// the node is placed via set_position. Size comes from explicit bounds, else a
// per-type default (textures, which would refine these, arrive in M4).
void apply_position(Control *node, const mnu::Window &w) {
	node->set_anchor(SIDE_LEFT, 0);
	node->set_anchor(SIDE_TOP, 0);
	node->set_anchor(SIDE_RIGHT, 0);
	node->set_anchor(SIDE_BOTTOM, 0);

	const int x = w.position.has_left ? w.position.left : 0;
	const int y = w.position.has_top ? w.position.top : 0;

	int width = 0;
	int height = 0;
	if (w.position.has_right) {
		const int left = w.position.has_left ? w.position.left : 0;
		width = w.position.right - left;
	}
	if (w.position.has_bottom) {
		const int top = w.position.has_top ? w.position.top : 0;
		height = w.position.bottom - top;
	}

	node->set_position(Vector2(x, y));

	int final_width = 0;
	int final_height = 0;

	if (width > 0) {
		final_width = width;
	} else if (is_button_like(w.type)) {
		if (w.type == mnu::WindowType::Combo || w.type == mnu::WindowType::SpinList) {
			final_width = 150;
		} else if (w.type == mnu::WindowType::Scroll) {
			final_width = 200;
		} else {
			final_width = 100;
		}
	} else if (w.type == mnu::WindowType::Static || w.type == mnu::WindowType::Label) {
		final_width = 150;
	}

	if (height > 0) {
		final_height = height;
	} else if (w.type == mnu::WindowType::Button || w.type == mnu::WindowType::Radio ||
			w.type == mnu::WindowType::CheckBox || w.type == mnu::WindowType::Scroll) {
		final_height = 24;
	}

	if (final_width > 0 && final_height > 0) {
		node->set_size(Vector2(final_width, final_height));
	} else if (final_width > 0) {
		node->set_size(Vector2(final_width, 0));
		if (w.type == mnu::WindowType::Window) {
			node->set_anchor(SIDE_BOTTOM, 1);
			node->set_offset(SIDE_BOTTOM, 0);
		}
	} else if (final_height > 0) {
		node->set_size(Vector2(0, final_height));
		if (w.type == mnu::WindowType::Window) {
			node->set_anchor(SIDE_RIGHT, 1);
			node->set_offset(SIDE_RIGHT, 0);
		}
	} else if (w.type == mnu::WindowType::Window) {
		node->set_anchor(SIDE_RIGHT, 1);
		node->set_anchor(SIDE_BOTTOM, 1);
	}
}

// Attach a full-rect, click-through Label child for the widget's text.
void add_text_label(Control *parent, const String &text) {
	if (text.is_empty()) {
		return;
	}
	Label *lbl = memnew(Label);
	lbl->set_name("Label");
	lbl->set_anchors_preset(Control::PRESET_FULL_RECT);
	lbl->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	lbl->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
	lbl->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	lbl->set_text(text);
	parent->add_child(lbl);
}

// Placeholder frame: a bordered, semi-transparent panel behind the content so a
// FRAME element reads visually until the real 9-patch bake lands (M4).
void add_placeholder_frame(Control *node) {
	Panel *panel = memnew(Panel);
	panel->set_name("FramePlaceholder");
	panel->set_anchors_preset(Control::PRESET_FULL_RECT);
	panel->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	Ref<StyleBoxFlat> sb;
	sb.instantiate();
	sb->set_bg_color(Color(0.05f, 0.07f, 0.10f, 0.55f));
	sb->set_border_color(Color(0.55f, 0.6f, 0.7f, 0.9f));
	sb->set_border_width_all(2);
	panel->add_theme_stylebox_override("panel", sb);
	// Behind siblings added later.
	node->add_child(panel);
	node->move_child(panel, 0);
}

Control *build_window(const mnu::Window &w, const MnuBuildContext &ctx);

void build_children(Control *parent, const mnu::Window &w, const MnuBuildContext &ctx) {
	for (const auto &child : w.children) {
		Control *node = build_window(child, ctx);
		if (node) {
			parent->add_child(node);
		}
	}
}

Control *build_window(const mnu::Window &w, const MnuBuildContext &ctx) {
	Control *node = nullptr;
	const String text = display_text(w, ctx);

	switch (w.type) {
		case mnu::WindowType::Button:
		case mnu::WindowType::Radio: {
			Button *btn = memnew(Button);
			btn->set_text(text);
			btn->set_clip_text(true);
			if (ctx.edit_mode) {
				btn->set_disabled(true); // no action dispatch while authoring
			}
			node = btn;
		} break;
		case mnu::WindowType::CheckBox: {
			CheckBox *cb = memnew(CheckBox);
			cb->set_text(text);
			cb->set_pressed(w.checked);
			if (ctx.edit_mode) {
				cb->set_disabled(true);
			}
			node = cb;
		} break;
		case mnu::WindowType::Static:
		case mnu::WindowType::Label: {
			if (!text.is_empty()) {
				Label *lbl = memnew(Label);
				lbl->set_text(text);
				node = lbl;
			} else {
				node = memnew(Control);
			}
		} break;
		case mnu::WindowType::Window: {
			node = memnew(Control);
		} break;
		default: {
			// List / Combo / SpinList / Edit / Table / Scroll / Map / ... : a
			// placeholder panel until the real widget port (M5).
			Panel *panel = memnew(Panel);
			node = panel;
		} break;
	}

	if (node == nullptr) {
		node = memnew(Control);
	}

	node->set_name(to_gd(w.name).is_empty() ? String(mnu::window_type_name(w.type)) : to_gd(w.name));
	apply_position(node, w);

	if (ctx.edit_mode) {
		node->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	}

	// Frame placeholder for container windows that declare one.
	if (!w.frame.stencil.empty() || !w.frame.brush.empty()) {
		add_placeholder_frame(node);
	}

	// Labels/buttons already carry their own text; for other placeholder widgets
	// surface the text as a child label so the layout reads.
	if (w.type != mnu::WindowType::Button && w.type != mnu::WindowType::Radio &&
			w.type != mnu::WindowType::CheckBox && w.type != mnu::WindowType::Static &&
			w.type != mnu::WindowType::Label) {
		add_text_label(node, text);
	}

	if (w.hidden) {
		node->set_visible(false);
	}

	build_children(node, w, ctx);
	return node;
}

} // namespace

namespace godot {

Control *mnu_build_screen(const mnu::Screen &screen, const MnuBuildContext &ctx) {
	NovaMnuScreen *screen_node = memnew(NovaMnuScreen);
	screen_node->set_name(to_gd(screen.name).is_empty() ? String("Screen") : to_gd(screen.name));
	screen_node->set_screen_name(to_gd(screen.name));
	screen_node->set_music_var(screen.music_var);
	screen_node->set_cursor_file(to_gd(screen.cursor_file));
	screen_node->set_anchors_preset(Control::PRESET_FULL_RECT);
	if (ctx.edit_mode) {
		screen_node->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	}

	Control *root = build_window(screen.root_window, ctx);
	if (root) {
		screen_node->add_child(root);
	}
	return screen_node;
}

} // namespace godot
