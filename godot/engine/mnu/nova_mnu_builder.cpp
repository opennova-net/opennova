#include "nova_mnu_builder.h"

#include "mnu_itemlist_common.h"
#include "mnu_outline.h"
#include "nova_mnu_button.h"
#include "nova_mnu_checkbox.h"
#include "nova_mnu_combo.h"
#include "nova_mnu_edit.h"
#include "nova_mnu_goto.h"
#include "nova_mnu_globe.h"
#include "nova_mnu_label.h"
#include "nova_mnu_list.h"
#include "nova_mnu_map.h"
#include "nova_mnu_marquee.h"
#include "nova_mnu_multi.h"
#include "nova_mnu_document.h"
#include "nova_mnu_multiline_edit.h"
#include "nova_mnu_screen.h"
#include "nova_mnu_scroll.h"
#include "nova_mnu_spinlist.h"
#include "nova_mnu_table.h"

#include "fnt/nova_fnt_resource.h"
#include "mns_stylesheet.h"
#include "resource_index/nova_resource_root.h"
#include "rtxt/rtxt_string_file.h"

#include <godot_cpp/classes/atlas_texture.hpp>
#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/button_group.hpp>
#include <godot_cpp/classes/color_rect.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/font_file.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/label_settings.hpp>
#include <godot_cpp/classes/nine_patch_rect.hpp>
#include <godot_cpp/classes/panel.hpp>
#include <godot_cpp/classes/style_box_flat.hpp>
#include <godot_cpp/classes/texture_button.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/variant/color.hpp>

#include <cctype>
#include <map>
#include <set>
#include <string>

using namespace godot;

namespace {

String to_gd(const std::string &s) {
	return String::utf8(s.c_str(), static_cast<int>(s.length()));
}

std::string to_std(const String &s) {
	const CharString utf8 = s.utf8();
	return std::string(utf8.get_data(), static_cast<size_t>(utf8.length()));
}

// Case-insensitive std::string compare (matches the reference importer).
bool iequals(const std::string &a, const std::string &b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (size_t i = 0; i < a.size(); ++i) {
		if (std::tolower(static_cast<unsigned char>(a[i])) !=
				std::tolower(static_cast<unsigned char>(b[i]))) {
			return false;
		}
	}
	return true;
}

// Ensures unique node names within a screen so find_child stays predictable.
class NameTracker {
public:
	String get_unique(const String &base) {
		if (base.is_empty()) {
			return get_unique("Element");
		}
		if (used_.find(base) == used_.end()) {
			used_.insert(base);
			return base;
		}
		int n = 2;
		String name;
		do {
			name = base + String("_") + String::num_int64(n++);
		} while (used_.find(name) != used_.end());
		used_.insert(name);
		return name;
	}

private:
	std::set<String> used_;
};

using ButtonGroupMap = std::map<int, Ref<ButtonGroup>>;

bool is_button_like(mnu::WindowType t) {
	return t == mnu::WindowType::Button || t == mnu::WindowType::Radio ||
			t == mnu::WindowType::CheckBox || t == mnu::WindowType::Combo ||
			t == mnu::WindowType::SpinList || t == mnu::WindowType::Scroll;
}

// Hover/click sound split for a widget. The reference keys menu sounds by their
// trigger string and routes MOUSE/OVER triggers to the hover slot and CLICK/
// SELECT triggers to the click slot (mnu_import_plugin.cpp ~702-712). Triggers
// are uppercased so they match the (uppercase) SBF bank entry names at playback.
struct WidgetSounds {
	String hover_trigger;
	String hover_file;
	String click_trigger;
	String click_file;
};

WidgetSounds split_sounds(const std::vector<mnu::Sound> &sounds) {
	WidgetSounds out;
	for (const auto &s : sounds) {
		const String trigger = to_gd(s.trigger).to_upper();
		const String file = to_gd(s.file);
		if (trigger.contains("MOUSE") || trigger.contains("OVER")) {
			out.hover_trigger = trigger;
			out.hover_file = file;
		} else if (trigger.contains("CLICK") || trigger.contains("SELECT")) {
			out.click_trigger = trigger;
			out.click_file = file;
		}
	}
	return out;
}

// Collapse an MNU <ACTION> into the widget's plain-data action list (verb +
// window_state lowercased to match dispatch_action's comparisons).
MnuActionData make_action(const mnu::Action &act) {
	MnuActionData data;
	data.type = to_gd(act.type).to_lower();
	data.target = to_gd(act.target);
	data.file = to_gd(act.file);
	data.window_state = to_gd(act.state).to_lower();
	return data;
}

// --- Stylesheet / color resolution -----------------------------------------

// Substitute %VAR% references through the stylesheet (no-op without one).
String substitute_var(MnuBuildContext &ctx, const String &str) {
	if (ctx.stylesheet == nullptr || !str.contains("%")) {
		return str;
	}
	return ctx.stylesheet->substitute(str);
}

// Resolve a color string to a concrete hex value, or "" if it stays a %VAR%
// (unresolved) or is empty. The caller treats "" as "leave unset".
std::string resolve_color(MnuBuildContext &ctx, const std::string &color) {
	if (color.empty()) {
		return "";
	}
	const String resolved = substitute_var(ctx, to_gd(color));
	if (resolved.begins_with("%")) {
		return "";
	}
	return to_std(resolved);
}

Color parse_color(const String &hex) {
	uint8_t r, g, b, a;
	if (mnu::parse_hex_color(to_std(hex), r, g, b, a)) {
		return Color(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
	}
	return Color(1, 1, 1);
}

// --- Asset resolution (routed through NovaResourceRoot) ---------------------

// Resolve a concrete texture filename to a Texture2D. %VAR% references and empty
// names resolve to null silently; a concrete name that fails to load is recorded
// as an unresolved asset so the editor can report it.
Ref<Texture2D> resolve_texture(MnuBuildContext &ctx, const std::string &name) {
	if (name.empty()) {
		return Ref<Texture2D>();
	}
	const String gname = to_gd(name);
	if (gname.begins_with("%")) {
		return Ref<Texture2D>();
	}
	Ref<Texture2D> tex;
	if (ctx.root != nullptr) {
		tex = ctx.root->load_texture(gname);
	}
	if (tex.is_null()) {
		ctx.unresolved_assets.insert(name);
	}
	return tex;
}

// Pick the texture for an appearance state ("default", "mouseover", ...),
// building an AtlasTexture when the appearance is a sprite-sheet row.
Ref<Texture2D> get_texture(MnuBuildContext &ctx, const std::vector<mnu::Appearance> &apps,
		const std::string &state) {
	for (const auto &app : apps) {
		if (app.state == state && app.type == "image" && !app.value.empty()) {
			Ref<Texture2D> tex = resolve_texture(ctx, app.value);
			if (tex.is_valid() && app.map_state >= 0 && app.height > 0) {
				Ref<AtlasTexture> atlas;
				atlas.instantiate();
				atlas->set_atlas(tex);
				atlas->set_region(Rect2(0, app.map_state * app.height, tex->get_width(), app.height));
				return atlas;
			}
			return tex;
		}
	}
	return Ref<Texture2D>();
}

Ref<Image> resolve_image(MnuBuildContext &ctx, const std::string &name) {
	Ref<Texture2D> tex = resolve_texture(ctx, name);
	if (tex.is_null()) {
		return Ref<Image>();
	}
	return tex->get_image();
}

// Resolve a (possibly %VAR%) font name to a Godot Font. Returns null when the
// name is a still-unresolved variable or cannot be loaded; out_fixed_size
// carries the bitmap font's native pixel size for correct rendering.
Ref<Font> resolve_font(MnuBuildContext &ctx, const std::string &name, int &out_fixed_size) {
	out_fixed_size = 0;
	if (name.empty()) {
		return Ref<Font>();
	}
	const String resolved = substitute_var(ctx, to_gd(name));
	if (resolved.begins_with("%")) {
		return Ref<Font>();
	}
	Ref<Resource> font_res;
	if (ctx.root != nullptr) {
		font_res = ctx.root->load_font(resolved);
	}
	if (font_res.is_null()) {
		ctx.unresolved_assets.insert(to_std(resolved));
		return Ref<Font>();
	}
	Ref<FontFile> ff;
	Ref<NovaFntResource> nova_fnt = font_res;
	if (nova_fnt.is_valid()) {
		ff = nova_fnt->to_font_file();
	} else {
		ff = font_res; // already a FontFile
	}
	if (ff.is_valid() && ff->get_fixed_size() > 0) {
		out_fixed_size = ff->get_fixed_size();
	}
	return ff;
}

// --- Appearance predicates --------------------------------------------------

bool has_images(const std::vector<mnu::Appearance> &apps) {
	for (const auto &app : apps) {
		if (app.type == "image" && !app.value.empty() && app.value[0] != '%') {
			return true;
		}
	}
	return false;
}

bool has_color(const std::vector<mnu::Appearance> &apps) {
	for (const auto &app : apps) {
		if (app.type == "color" && !app.value.empty()) {
			return true;
		}
	}
	return false;
}

bool has_outline(const std::vector<mnu::Appearance> &apps) {
	for (const auto &app : apps) {
		if (app.type == "outline" && !app.value.empty()) {
			return true;
		}
	}
	return false;
}

Color get_appearance_color(const std::vector<mnu::Appearance> &apps, const std::string &type,
		const std::string &state, bool &found) {
	found = false;
	for (const auto &app : apps) {
		if (app.state == state && app.type == type && !app.value.empty()) {
			uint8_t r, g, b, a;
			if (mnu::parse_hex_color(app.value, r, g, b, a)) {
				found = true;
				return Color(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
			}
		}
	}
	return Color(1, 1, 1, 1);
}

bool has_frame(const mnu::Frame &frame) {
	return !frame.stencil.empty() || !frame.brush.empty() || !frame.monogram.empty();
}

// --- Frame 9-patch bake (port of sub_52D000, see mnu_import_plugin.cpp) ------
//
// Bakes the stencil (border art) + optional brush (tiling fill) into a single
// RGBA8 texture whose 4x4 cell grid feeds a NinePatchRect. Corners are mirrored
// from cell [0,0]; edges are stretched from a single sampled slice past the
// corner curve; the interior of each edge cell and the center 2x2 are cleared to
// transparent. Border insets are the original game's hardcoded 16/24/16/24.
Ref<ImageTexture> bake_frame_texture(MnuBuildContext &ctx, const mnu::Frame &frame,
		int &out_cell_w, int &out_cell_h) {
	out_cell_w = 0;
	out_cell_h = 0;

	Ref<Image> stencil_img;
	if (!frame.stencil.empty()) {
		stencil_img = resolve_image(ctx, frame.stencil);
	}
	if (stencil_img.is_null()) {
		return Ref<ImageTexture>();
	}

	// Imported textures can come back BPTC/S3TC-compressed; decompress before any
	// per-pixel access or convert (repo convention for BPTC/S3TC detail textures).
	if (stencil_img->is_compressed()) {
		stencil_img->decompress();
	}
	if (stencil_img->get_format() != Image::FORMAT_RGBA8) {
		stencil_img->convert(Image::FORMAT_RGBA8);
	}

	const int width = stencil_img->get_width();
	const int height = stencil_img->get_height();
	if (width < 4 || height < 4) {
		return Ref<ImageTexture>();
	}

	const int cell_w = width / 4;
	const int cell_h = height / 4;
	out_cell_w = cell_w;
	out_cell_h = cell_h;

	Ref<Image> brush_img;
	if (!frame.brush.empty()) {
		brush_img = resolve_image(ctx, frame.brush);
	}
	if (brush_img.is_valid()) {
		if (brush_img->is_compressed()) {
			brush_img->decompress();
		}
		if (brush_img->get_format() != Image::FORMAT_RGBA8) {
			brush_img->convert(Image::FORMAT_RGBA8);
		}
		const int brush_w = brush_img->get_width();
		const int brush_h = brush_img->get_height();
		if (brush_w > 0 && brush_h > 0) {
			for (int y = 0; y < height; ++y) {
				for (int x = 0; x < width; ++x) {
					const Color sp = stencil_img->get_pixel(x, y);
					if (sp.a > 0.0f) {
						const Color bp = brush_img->get_pixel(x % brush_w, y % brush_h);
						stencil_img->set_pixel(x, y, Color(sp.r * bp.r, sp.g * bp.g, sp.b * bp.b, sp.a));
					}
				}
			}
		}
	}

	const int border_left = 16;
	const int border_top = 24;
	const int border_right = 16;
	const int border_bottom = 24;

	// Mirror corner [0,0] into the other three corners.
	for (int y = 0; y < cell_h; ++y) {
		for (int x = 0; x < cell_w; ++x) {
			const Color pixel = stencil_img->get_pixel(x, y);
			stencil_img->set_pixel(width - 1 - x, y, pixel);
		}
	}
	for (int y = 0; y < cell_h; ++y) {
		for (int x = 0; x < cell_w; ++x) {
			const Color pixel = stencil_img->get_pixel(x, y);
			stencil_img->set_pixel(x, height - 1 - y, pixel);
		}
	}
	for (int y = 0; y < cell_h; ++y) {
		for (int x = 0; x < cell_w; ++x) {
			const Color pixel = stencil_img->get_pixel(x, y);
			stencil_img->set_pixel(width - 1 - x, height - 1 - y, pixel);
		}
	}

	const int sample_x = cell_w / 2;
	const int sample_y = cell_h / 2;

	// Top edge.
	for (int y = 0; y < border_top; ++y) {
		const Color pixel = stencil_img->get_pixel(sample_x, y);
		for (int x = cell_w; x < 3 * cell_w; ++x) {
			stencil_img->set_pixel(x, y, pixel);
		}
	}
	for (int y = border_top; y < cell_h; ++y) {
		for (int x = cell_w; x < 3 * cell_w; ++x) {
			stencil_img->set_pixel(x, y, Color(0, 0, 0, 0));
		}
	}
	// Bottom edge.
	for (int y = height - border_bottom; y < height; ++y) {
		const Color pixel = stencil_img->get_pixel(sample_x, y);
		for (int x = cell_w; x < 3 * cell_w; ++x) {
			stencil_img->set_pixel(x, y, pixel);
		}
	}
	for (int y = 3 * cell_h; y < height - border_bottom; ++y) {
		for (int x = cell_w; x < 3 * cell_w; ++x) {
			stencil_img->set_pixel(x, y, Color(0, 0, 0, 0));
		}
	}
	// Left edge.
	for (int x = 0; x < border_left; ++x) {
		const Color pixel = stencil_img->get_pixel(x, sample_y);
		for (int y = cell_h; y < 3 * cell_h; ++y) {
			stencil_img->set_pixel(x, y, pixel);
		}
	}
	for (int x = border_left; x < cell_w; ++x) {
		for (int y = cell_h; y < 3 * cell_h; ++y) {
			stencil_img->set_pixel(x, y, Color(0, 0, 0, 0));
		}
	}
	// Right edge.
	for (int x = width - border_right; x < width; ++x) {
		const Color pixel = stencil_img->get_pixel(x, sample_y);
		for (int y = cell_h; y < 3 * cell_h; ++y) {
			stencil_img->set_pixel(x, y, pixel);
		}
	}
	for (int x = 3 * cell_w; x < width - border_right; ++x) {
		for (int y = cell_h; y < 3 * cell_h; ++y) {
			stencil_img->set_pixel(x, y, Color(0, 0, 0, 0));
		}
	}

	// Clear center 2x2 cells.
	for (int cy = 1; cy <= 2; ++cy) {
		for (int cx = 1; cx <= 2; ++cx) {
			const int start_x = cx * cell_w;
			const int start_y = cy * cell_h;
			for (int y = start_y; y < start_y + cell_h; ++y) {
				for (int x = start_x; x < start_x + cell_w; ++x) {
					stencil_img->set_pixel(x, y, Color(0, 0, 0, 0));
				}
			}
		}
	}

	return ImageTexture::create_from_image(stencil_img);
}

// Render a frame onto a container: baked 9-patch border + dark center, or a flat
// dark panel fallback when the stencil cannot be resolved (port of add_frame).
void add_frame(MnuBuildContext &ctx, Control *parent, const mnu::Frame &frame) {
	if (!has_frame(frame)) {
		return;
	}

	int cell_w = 0;
	int cell_h = 0;
	Ref<ImageTexture> frame_tex = bake_frame_texture(ctx, frame, cell_w, cell_h);

	if (frame_tex.is_null()) {
		ColorRect *bg = memnew(ColorRect);
		bg->set_name("FrameBackground");
		bg->set_color(Color(0.02f, 0.02f, 0.02f, 0.92f));
		bg->set_anchors_preset(Control::PRESET_FULL_RECT);
		bg->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
		parent->add_child(bg);
		return;
	}

	const int inset_x = 16;
	const int inset_y = 24;

	ColorRect *bg = memnew(ColorRect);
	bg->set_name("FrameBackground");
	bg->set_color(Color(0.02f, 0.02f, 0.02f, 0.92f));
	bg->set_anchors_preset(Control::PRESET_FULL_RECT);
	bg->set_offset(SIDE_LEFT, inset_x);
	bg->set_offset(SIDE_TOP, inset_y);
	bg->set_offset(SIDE_RIGHT, -inset_x);
	bg->set_offset(SIDE_BOTTOM, -inset_y);
	bg->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	parent->add_child(bg);

	NinePatchRect *frame_rect = memnew(NinePatchRect);
	frame_rect->set_name("FrameBorder");
	frame_rect->set_texture(frame_tex);
	frame_rect->set_anchors_preset(Control::PRESET_FULL_RECT);
	frame_rect->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	frame_rect->set_patch_margin(SIDE_LEFT, cell_w);
	frame_rect->set_patch_margin(SIDE_TOP, cell_h);
	frame_rect->set_patch_margin(SIDE_RIGHT, cell_w);
	frame_rect->set_patch_margin(SIDE_BOTTOM, cell_h);
	frame_rect->set_draw_center(false);
	parent->add_child(frame_rect);
}

// --- Text / label -----------------------------------------------------------

// Resolve a widget's display text: a type="id" string looks up the RTXT table
// (falling back to the raw id when unavailable), then the {hot} marker is
// stripped. Literal strings are returned as-is (minus any marker).
String resolve_text(MnuBuildContext &ctx, const mnu::String &sd) {
	if (sd.value.empty()) {
		return String();
	}
	if (iequals(sd.type, "id") && ctx.text != nullptr) {
		const StringName key = to_gd(sd.value);
		if (ctx.text->has_string(key)) {
			const String resolved = ctx.text->get_string(key);
			if (!resolved.is_empty()) {
				return to_gd(mnu::strip_hotkey_marker(to_std(resolved)));
			}
		}
	}
	return to_gd(mnu::strip_hotkey_marker(sd.value));
}

void apply_label_font(MnuBuildContext &ctx, Label *lbl, const mnu::Font &font) {
	if (font.name.empty() && font.default_fg.empty()) {
		return;
	}
	Ref<LabelSettings> settings;
	settings.instantiate();
	if (!font.name.empty()) {
		int fixed = 0;
		Ref<Font> f = resolve_font(ctx, font.name, fixed);
		if (f.is_valid()) {
			settings->set_font(f);
			if (fixed > 0) {
				settings->set_font_size(fixed);
			}
		}
	}
	const std::string fg = resolve_color(ctx, font.default_fg);
	if (!fg.empty()) {
		settings->set_font_color(parse_color(to_gd(fg)));
	}
	lbl->set_label_settings(settings);
}

// --- Positioning (port of mnu_import_plugin.cpp ~1940-2049) ------------------

void apply_position(MnuBuildContext &ctx, Control *node, const mnu::Window &w) {
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
		Ref<Texture2D> tex = get_texture(ctx, w.appearances, "default");
		if (tex.is_valid()) {
			final_width = tex->get_width();
		} else if (w.type == mnu::WindowType::Combo || w.type == mnu::WindowType::SpinList) {
			final_width = 150;
		} else if (w.type == mnu::WindowType::Scroll) {
			final_width = 200;
		} else {
			final_width = 100;
		}
	} else if (w.type == mnu::WindowType::Static || w.type == mnu::WindowType::Label) {
		if (has_images(w.appearances)) {
			Ref<Texture2D> tex = get_texture(ctx, w.appearances, "default");
			if (tex.is_valid()) {
				final_width = tex->get_width();
			}
		} else {
			final_width = 150;
		}
	}

	if (height > 0) {
		final_height = height;
	} else if (w.type == mnu::WindowType::Button || w.type == mnu::WindowType::Radio ||
			w.type == mnu::WindowType::CheckBox || w.type == mnu::WindowType::Scroll) {
		Ref<Texture2D> tex = get_texture(ctx, w.appearances, "default");
		if (tex.is_valid()) {
			final_height = tex->get_height();
		} else {
			final_height = 24;
		}
	} else if (w.type == mnu::WindowType::Window && has_images(w.appearances)) {
		Ref<Texture2D> tex = get_texture(ctx, w.appearances, "default");
		if (tex.is_valid()) {
			final_height = tex->get_height();
		}
	}

	if (final_width > 0 && final_height > 0) {
		node->set_size(Vector2(final_width, final_height));
	} else if (final_width > 0) {
		node->set_size(Vector2(final_width, 0));
		if (w.type == mnu::WindowType::Window && !has_images(w.appearances)) {
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

// 1px outline border drawn with four ColorRects, added after children so it
// renders on top (port of the outline pass in create_window). The 4-rect recipe
// is shared via mnu_outline.h so the richer M9 widgets can reuse it.
void add_outline(Control *node, const std::vector<mnu::Appearance> &apps) {
	bool found = false;
	const Color outline_color = get_appearance_color(apps, "outline", "default", found);
	if (!found) {
		return;
	}
	mnu_add_outline(node, outline_color, 1);
}

// --- Per-type node construction ---------------------------------------------

Control *build_button(MnuBuildContext &ctx, const mnu::Window &w, const mnu::Font &font,
		ButtonGroupMap &groups) {
	NovaMnuButton *btn = memnew(NovaMnuButton);
	btn->set_ignore_texture_size(true);
	btn->set_stretch_mode(TextureButton::STRETCH_SCALE);
	btn->set_focus_mode(Control::FOCUS_ALL);

	Ref<Texture2D> tex_normal = get_texture(ctx, w.appearances, "default");
	Ref<Texture2D> tex_hover = get_texture(ctx, w.appearances, "mouseover");
	Ref<Texture2D> tex_pressed = get_texture(ctx, w.appearances, "selected");
	Ref<Texture2D> tex_disabled = get_texture(ctx, w.appearances, "disabled");
	if (tex_normal.is_valid()) {
		btn->set_texture_normal(tex_normal);
	}
	if (tex_hover.is_valid()) {
		btn->set_texture_hover(tex_hover);
	}
	if (tex_pressed.is_valid()) {
		btn->set_texture_pressed(tex_pressed);
	}
	if (tex_disabled.is_valid()) {
		btn->set_texture_disabled(tex_disabled);
	}

	if (w.type == mnu::WindowType::Radio) {
		btn->set_toggle_mode(true);
		if (w.checked) {
			btn->set_pressed(true);
		}
		if (w.group > 0) {
			auto it = groups.find(w.group);
			if (it == groups.end()) {
				Ref<ButtonGroup> bg;
				bg.instantiate();
				groups[w.group] = bg;
				it = groups.find(w.group);
			}
			btn->set_button_group(it->second);
		}
	}

	// Interactivity wiring (M5): actions, sounds, hover/normal colours, and the
	// back-pointer to the navigation controller. _ready() consumes these.
	btn->set_menu(ctx.owner);
	btn->set_edit_mode(ctx.edit_mode);
	for (const auto &act : w.actions) {
		btn->add_action(make_action(act));
	}
	const WidgetSounds snd = split_sounds(w.sounds);
	btn->set_hover_sound(snd.hover_trigger, snd.hover_file);
	btn->set_click_sound(snd.click_trigger, snd.click_file);
	{
		Color normal_c(1, 1, 1, 1);
		const std::string fg = resolve_color(ctx, font.default_fg);
		if (!fg.empty()) {
			normal_c = parse_color(to_gd(fg));
		}
		const std::string mo = resolve_color(ctx, font.mouseover_fg);
		const Color hover_c = mo.empty() ? normal_c : parse_color(to_gd(mo));
		btn->set_font_colors(normal_c, hover_c);
	}

	if (ctx.edit_mode) {
		btn->set_disabled(true); // inert while authoring
	}

	if (!w.string_data.value.empty()) {
		NovaMnuLabel *lbl = memnew(NovaMnuLabel);
		lbl->set_name("Label");
		if (iequals(w.string_data.type, "id")) {
			lbl->set_string_id(to_gd(w.string_data.value));
		}
		lbl->set_anchors_preset(Control::PRESET_FULL_RECT);
		lbl->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
		apply_label_font(ctx, lbl, font);

		const String justify = to_gd(w.string_data.justify).to_upper();
		if (justify == "LEFT") {
			lbl->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_LEFT);
		} else if (justify == "RIGHT") {
			lbl->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_RIGHT);
		} else {
			lbl->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
		}
		const String vjustify = to_gd(w.string_data.vjustify).to_upper();
		if (vjustify == "TOP") {
			lbl->set_vertical_alignment(VERTICAL_ALIGNMENT_TOP);
		} else if (vjustify == "BOTTOM") {
			lbl->set_vertical_alignment(VERTICAL_ALIGNMENT_BOTTOM);
		} else {
			lbl->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
		}

		lbl->set_text(resolve_text(ctx, w.string_data));
		btn->add_child(lbl);
	}
	return btn;
}

Control *build_checkbox(MnuBuildContext &ctx, const mnu::Window &w, const mnu::Font &font) {
	NovaMnuCheckBox *check = memnew(NovaMnuCheckBox);
	check->set_toggle_mode(true);
	check->set_ignore_texture_size(true);
	// Checkbox art is aspect-preserved (mnu_checkbox.gd _ready); buttons scale.
	// Set here (not in _ready) so the edit_mode preview matches runtime.
	check->set_stretch_mode(TextureButton::STRETCH_KEEP_ASPECT_CENTERED);
	check->set_focus_mode(Control::FOCUS_ALL);

	Ref<Texture2D> tex_normal = get_texture(ctx, w.appearances, "default");
	Ref<Texture2D> tex_hover = get_texture(ctx, w.appearances, "mouseover");
	Ref<Texture2D> tex_pressed = get_texture(ctx, w.appearances, "selected");
	Ref<Texture2D> tex_disabled = get_texture(ctx, w.appearances, "disabled");
	if (tex_normal.is_valid()) {
		check->set_texture_normal(tex_normal);
	}
	if (tex_hover.is_valid()) {
		check->set_texture_hover(tex_hover);
	}
	if (tex_pressed.is_valid()) {
		check->set_texture_pressed(tex_pressed);
	}
	if (tex_disabled.is_valid()) {
		check->set_texture_disabled(tex_disabled);
	}

	if (w.checked) {
		check->set_pressed(true);
	}

	// Interactivity wiring (M5): sounds, underline colour (font selected_fg),
	// back-pointer to the navigation controller. _ready() consumes these.
	check->set_menu(ctx.owner);
	check->set_edit_mode(ctx.edit_mode);
	const WidgetSounds snd = split_sounds(w.sounds);
	check->set_hover_sound(snd.hover_trigger, snd.hover_file);
	check->set_click_sound(snd.click_trigger, snd.click_file);
	{
		Color underline_c(1, 0, 0, 1);
		const std::string sfg = resolve_color(ctx, font.selected_fg);
		if (!sfg.empty()) {
			underline_c = parse_color(to_gd(sfg));
		}
		check->set_underline_color(underline_c);
	}

	if (ctx.edit_mode) {
		check->set_disabled(true);
	}

	if (!w.string_data.value.empty()) {
		NovaMnuLabel *lbl = memnew(NovaMnuLabel);
		lbl->set_name("Label");
		if (iequals(w.string_data.type, "id")) {
			lbl->set_string_id(to_gd(w.string_data.value));
		}
		lbl->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
		apply_label_font(ctx, lbl, font);

		const String vjustify = to_gd(w.string_data.vjustify).to_upper();
		if (vjustify == "TOP") {
			lbl->set_vertical_alignment(VERTICAL_ALIGNMENT_TOP);
		} else if (vjustify == "BOTTOM") {
			lbl->set_vertical_alignment(VERTICAL_ALIGNMENT_BOTTOM);
		} else {
			lbl->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
		}
		lbl->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_LEFT);
		lbl->set_text(resolve_text(ctx, w.string_data));

		int checkbox_width = 24;
		int checkbox_height = 24;
		if (tex_normal.is_valid()) {
			checkbox_width = tex_normal->get_width();
			checkbox_height = tex_normal->get_height();
		}
		lbl->set_position(Vector2(checkbox_width + 4, 0));
		lbl->set_size(Vector2(300, checkbox_height));
		check->add_child(lbl);
	}
	return check;
}

// Static / Label / Window container: frame, color/image background, optional
// text label. Children + outline are added by the common tail in build_window.
Control *build_container(MnuBuildContext &ctx, const mnu::Window &w, const mnu::Font &font,
		const mnu::Frame &inherited_frame) {
	Control *container = memnew(Control);
	container->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);

	if (has_frame(w.frame)) {
		add_frame(ctx, container, w.frame);
	} else if (w.draw_frame && has_frame(inherited_frame)) {
		add_frame(ctx, container, inherited_frame);
	}

	if (has_color(w.appearances)) {
		bool found = false;
		const Color bg_color = get_appearance_color(w.appearances, "color", "default", found);
		if (found) {
			ColorRect *rect = memnew(ColorRect);
			rect->set_name("ColorBg");
			rect->set_color(bg_color);
			rect->set_anchors_preset(Control::PRESET_FULL_RECT);
			rect->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
			container->add_child(rect);
		}
	}

	if (has_images(w.appearances)) {
		Ref<Texture2D> bg = get_texture(ctx, w.appearances, "default");
		if (bg.is_valid()) {
			TextureRect *rect = memnew(TextureRect);
			rect->set_name("Background");
			rect->set_texture(bg);
			rect->set_anchors_preset(Control::PRESET_FULL_RECT);
			rect->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
			rect->set_stretch_mode(TextureRect::STRETCH_SCALE);
			rect->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
			container->add_child(rect);
		}
	}

	if (!w.string_data.value.empty()) {
		NovaMnuLabel *lbl = memnew(NovaMnuLabel);
		lbl->set_name("Label");
		if (iequals(w.string_data.type, "id")) {
			lbl->set_string_id(to_gd(w.string_data.value));
		}
		lbl->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
		apply_label_font(ctx, lbl, font);

		int lbl_width = 0;
		if (w.position.has_right) {
			const int left = w.position.has_left ? w.position.left : 0;
			lbl_width = w.position.right - left;
		}
		const bool has_explicit_width = (lbl_width > 0) || has_images(w.appearances);

		const String justify = to_gd(w.string_data.justify).to_upper();
		if (!has_explicit_width) {
			lbl->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_LEFT);
		} else if (justify == "CENTER") {
			lbl->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
		} else if (justify == "RIGHT") {
			lbl->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_RIGHT);
		} else {
			lbl->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_LEFT);
		}

		const String vjustify = to_gd(w.string_data.vjustify).to_upper();
		if (vjustify == "BOTTOM") {
			lbl->set_vertical_alignment(VERTICAL_ALIGNMENT_BOTTOM);
		} else if (vjustify == "CENTER") {
			lbl->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
		} else {
			lbl->set_vertical_alignment(VERTICAL_ALIGNMENT_TOP);
		}

		lbl->set_text(resolve_text(ctx, w.string_data));

		if (has_explicit_width) {
			lbl->set_anchors_preset(Control::PRESET_FULL_RECT);
			lbl->set_clip_text(true);
		}
		container->add_child(lbl);
	}

	return container;
}

// Placeholder for the interactive widgets whose behavior lands in M5 (combo,
// spinlist, list, edit, table, scroll, ...). Resolves the background texture and
// any text so the layout still reads; a subtle panel marks its bounds.
Control *build_placeholder(MnuBuildContext &ctx, const mnu::Window &w, const mnu::Font &font) {
	Panel *panel = memnew(Panel);

	Ref<Texture2D> bg = get_texture(ctx, w.appearances, "default");
	if (bg.is_valid()) {
		TextureRect *rect = memnew(TextureRect);
		rect->set_name("Background");
		rect->set_texture(bg);
		rect->set_anchors_preset(Control::PRESET_FULL_RECT);
		rect->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
		rect->set_stretch_mode(TextureRect::STRETCH_SCALE);
		rect->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
		panel->add_child(rect);
	}

	const String text = resolve_text(ctx, w.string_data);
	if (!text.is_empty()) {
		Label *lbl = memnew(Label);
		lbl->set_name("Label");
		lbl->set_anchors_preset(Control::PRESET_FULL_RECT);
		lbl->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
		lbl->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
		lbl->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
		apply_label_font(ctx, lbl, font);
		lbl->set_text(text);
		panel->add_child(lbl);
	}
	return panel;
}

// --- M9 interactive widgets -------------------------------------------------

// Apply a widget's MNU font + default foreground colour to a themed input Control
// (LineEdit/TextEdit) through theme overrides; those controls don't take a
// LabelSettings the way NovaMnuLabel does.
void apply_input_font(MnuBuildContext &ctx, Control *node, const mnu::Font &font) {
	if (!font.name.empty()) {
		int fixed = 0;
		Ref<Font> f = resolve_font(ctx, font.name, fixed);
		if (f.is_valid()) {
			node->add_theme_font_override("font", f);
			if (fixed > 0) {
				node->add_theme_font_size_override("font_size", fixed);
			}
		}
	}
	const std::string fg = resolve_color(ctx, font.default_fg);
	if (!fg.empty()) {
		node->add_theme_color_override("font_color", parse_color(to_gd(fg)));
	}
}

// Single-line text field (type="edit"). Seeds STRING text; a host drives content
// at runtime. Inert + non-editable in edit_mode.
Control *build_edit(MnuBuildContext &ctx, const mnu::Window &w, const mnu::Font &font) {
	NovaMnuEdit *edit = memnew(NovaMnuEdit);
	edit->set_menu(ctx.owner);
	edit->set_edit_mode(ctx.edit_mode);

	const WidgetSounds snd = split_sounds(w.sounds);
	edit->set_hover_sound(snd.hover_trigger, snd.hover_file);
	edit->set_click_sound(snd.click_trigger, snd.click_file);

	apply_input_font(ctx, edit, font);

	// Colour background -> StyleBoxFlat (LineEdit can't host a render-behind child).
	if (has_color(w.appearances)) {
		bool found = false;
		const Color bg = get_appearance_color(w.appearances, "color", "default", found);
		if (found) {
			Ref<StyleBoxFlat> sb;
			sb.instantiate();
			sb->set_bg_color(bg);
			edit->add_theme_stylebox_override("normal", sb);
		}
	}

	const String text = resolve_text(ctx, w.string_data);
	if (!text.is_empty()) {
		edit->set_text(text);
	}
	if (!w.hotkey.empty()) {
		edit->set_hotkey(to_gd(w.hotkey), w.hotkey_virtual);
	}

	if (ctx.edit_mode) {
		edit->set_editable(false);
		if (text.is_empty()) {
			edit->set_placeholder("Edit");
		}
	}
	return edit;
}

// Multi-line text field (type="multiline_edit"). Honors the READONLY flag.
Control *build_multiline_edit(MnuBuildContext &ctx, const mnu::Window &w, const mnu::Font &font) {
	NovaMnuMultilineEdit *edit = memnew(NovaMnuMultilineEdit);
	edit->set_menu(ctx.owner);
	edit->set_edit_mode(ctx.edit_mode);

	apply_input_font(ctx, edit, font);

	const String text = resolve_text(ctx, w.string_data);
	if (!text.is_empty()) {
		edit->set_text(text);
	} else if (ctx.edit_mode) {
		edit->set_placeholder("Multiline edit");
	}

	edit->set_readonly(w.readonly); // host can flip at runtime
	if (ctx.edit_mode) {
		edit->set_editable(false);
	}
	return edit;
}

// Resolve a list/spin/combo item's display text: type="id" looks up the RTXT
// table (falling back to the raw id), otherwise the literal text. Mirrors
// resolve_text but for the per-item String stored on mnu::Item.
String resolve_item_text(MnuBuildContext &ctx, const mnu::Item &item) {
	if (item.text.empty()) {
		return String();
	}
	if (iequals(item.type, "id") && ctx.text != nullptr) {
		const StringName key = to_gd(item.text);
		if (ctx.text->has_string(key)) {
			const String resolved = ctx.text->get_string(key);
			if (!resolved.is_empty()) {
				return to_gd(mnu::strip_hotkey_marker(to_std(resolved)));
			}
		}
	}
	return to_gd(mnu::strip_hotkey_marker(item.text));
}

// Theme an ItemList from the widget font + the ITEMS selection colour.
void apply_list_theme(MnuBuildContext &ctx, ItemList *list, const mnu::Window &w,
		const mnu::Font &font) {
	if (!font.name.empty()) {
		int fixed = 0;
		Ref<Font> f = resolve_font(ctx, font.name, fixed);
		if (f.is_valid()) {
			list->add_theme_font_override("font", f);
			if (fixed > 0) {
				list->add_theme_font_size_override("font_size", fixed);
			}
		}
	}
	const std::string fg = resolve_color(ctx, font.default_fg);
	if (!fg.empty()) {
		list->add_theme_color_override("font_color", parse_color(to_gd(fg)));
	}
	const std::string sel = resolve_color(ctx, w.items.selection_color);
	if (!sel.empty()) {
		Ref<StyleBoxFlat> sb;
		sb.instantiate();
		sb->set_bg_color(parse_color(to_gd(sel)));
		list->add_theme_stylebox_override("selected", sb);
		list->add_theme_stylebox_override("selected_focus", sb);
	}
}

// Seed the .mnu's own ITEM rows; in edit_mode inject a few synthetic rows when
// the list is empty so the WYSIWYG canvas still reads.
void seed_list_items(MnuBuildContext &ctx, ItemList *list, const mnu::Window &w) {
	for (const auto &it : w.items.items) {
		list->add_item(resolve_item_text(ctx, it));
	}
	if (ctx.edit_mode && w.items.items.empty()) {
		list->add_item("Sample 1");
		list->add_item("Sample 2");
		list->add_item("Sample 3");
	}
}

// Single-select list (type="list").
Control *build_list(MnuBuildContext &ctx, const mnu::Window &w, const mnu::Font &font) {
	NovaMnuList *list = memnew(NovaMnuList);
	list->set_select_mode(ItemList::SELECT_SINGLE);
	list->set_menu(ctx.owner);
	list->set_edit_mode(ctx.edit_mode);
	const WidgetSounds snd = split_sounds(w.sounds);
	list->set_hover_sound(snd.hover_trigger, snd.hover_file);
	list->set_click_sound(snd.click_trigger, snd.click_file);
	apply_list_theme(ctx, list, w, font);
	seed_list_items(ctx, list, w);
	if (ctx.edit_mode) {
		list->set_focus_mode(Control::FOCUS_NONE);
	}
	return list;
}

// Multi-select list (type="multi").
Control *build_multi(MnuBuildContext &ctx, const mnu::Window &w, const mnu::Font &font) {
	NovaMnuMulti *list = memnew(NovaMnuMulti);
	list->set_select_mode(ItemList::SELECT_MULTI);
	list->set_menu(ctx.owner);
	list->set_edit_mode(ctx.edit_mode);
	const WidgetSounds snd = split_sounds(w.sounds);
	list->set_hover_sound(snd.hover_trigger, snd.hover_file);
	list->set_click_sound(snd.click_trigger, snd.click_file);
	apply_list_theme(ctx, list, w, font);
	seed_list_items(ctx, list, w);
	if (ctx.edit_mode) {
		list->set_focus_mode(Control::FOCUS_NONE);
	}
	return list;
}

// Build one SpinUp/SpinDown button from a parsed SpinButton, positioned relative
// to the spinlist's own rect (MNU spin positions are absolute board coordinates).
// The buttons carry no nav actions: they only drive the parent spinlist.
void add_spin_button(MnuBuildContext &ctx, Control *spin, const mnu::Window &w,
		const mnu::SpinButton &sb, const char *name) {
	if (!sb.present) {
		return;
	}
	NovaMnuButton *btn = memnew(NovaMnuButton);
	btn->set_name(name);
	btn->set_ignore_texture_size(true);
	btn->set_stretch_mode(TextureButton::STRETCH_SCALE);
	Ref<Texture2D> tn = get_texture(ctx, sb.appearances, "default");
	Ref<Texture2D> th = get_texture(ctx, sb.appearances, "mouseover");
	if (tn.is_valid()) {
		btn->set_texture_normal(tn);
	}
	if (th.is_valid()) {
		btn->set_texture_hover(th);
	}
	btn->set_menu(ctx.owner);
	btn->set_edit_mode(ctx.edit_mode);
	if (ctx.edit_mode) {
		btn->set_disabled(true);
	}
	if (sb.position.has_left) {
		const int base_x = w.position.has_left ? w.position.left : 0;
		const int base_y = w.position.has_top ? w.position.top : 0;
		const int rx = sb.position.left - base_x;
		const int ry = sb.position.top - base_y;
		const int rw = sb.position.has_right ? sb.position.right - sb.position.left : 16;
		const int rh = sb.position.has_bottom ? sb.position.bottom - sb.position.top : 12;
		btn->set_position(Vector2(rx, ry));
		btn->set_size(Vector2(rw, rh));
	}
	spin->add_child(btn);
}

// Spinner (type="spinlist"): a value Label cycled by SpinUp/SpinDown children.
Control *build_spinlist(MnuBuildContext &ctx, const mnu::Window &w, const mnu::Font &font) {
	NovaMnuSpinList *spin = memnew(NovaMnuSpinList);
	spin->set_menu(ctx.owner);
	spin->set_edit_mode(ctx.edit_mode);
	const WidgetSounds snd = split_sounds(w.sounds);
	spin->set_click_sound(snd.click_trigger, snd.click_file);

	NovaMnuLabel *lbl = memnew(NovaMnuLabel);
	lbl->set_name("Value");
	lbl->set_anchors_preset(Control::PRESET_FULL_RECT);
	lbl->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	apply_label_font(ctx, lbl, font);
	const String justify = to_gd(w.items.justify).to_upper();
	if (justify == "LEFT") {
		lbl->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_LEFT);
	} else if (justify == "RIGHT") {
		lbl->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_RIGHT);
	} else {
		lbl->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	}
	lbl->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
	spin->add_child(lbl);

	PackedStringArray values;
	for (const auto &it : w.items.items) {
		values.push_back(resolve_item_text(ctx, it));
	}
	if (ctx.edit_mode && values.is_empty()) {
		values.push_back("--"); // empty-value placeholder for the WYSIWYG canvas
	}
	spin->set_values(values);

	add_spin_button(ctx, spin, w, w.spinup, "SpinUp");
	add_spin_button(ctx, spin, w, w.spindown, "SpinDown");
	return spin;
}

// Dropdown (type="combo"). Closed TextureButton + selected-text label; the LIST_BOX
// supplies the popup styling and any seed options (a host can repopulate at runtime).
Control *build_combo(MnuBuildContext &ctx, const mnu::Window &w, const mnu::Font &font) {
	NovaMnuCombo *combo = memnew(NovaMnuCombo);
	combo->set_ignore_texture_size(true);
	combo->set_stretch_mode(TextureButton::STRETCH_SCALE);
	combo->set_focus_mode(Control::FOCUS_ALL);

	Ref<Texture2D> tn = get_texture(ctx, w.appearances, "default");
	Ref<Texture2D> th = get_texture(ctx, w.appearances, "mouseover");
	Ref<Texture2D> tp = get_texture(ctx, w.appearances, "selected");
	Ref<Texture2D> td = get_texture(ctx, w.appearances, "disabled");
	if (tn.is_valid()) {
		combo->set_texture_normal(tn);
	}
	if (th.is_valid()) {
		combo->set_texture_hover(th);
	}
	if (tp.is_valid()) {
		combo->set_texture_pressed(tp);
	}
	if (td.is_valid()) {
		combo->set_texture_disabled(td);
	}

	combo->set_menu(ctx.owner);
	combo->set_edit_mode(ctx.edit_mode);
	const WidgetSounds snd = split_sounds(w.sounds);
	combo->set_hover_sound(snd.hover_trigger, snd.hover_file);
	combo->set_click_sound(snd.click_trigger, snd.click_file);

	// Closed-state label.
	NovaMnuLabel *lbl = memnew(NovaMnuLabel);
	lbl->set_name("SelectedText");
	lbl->set_anchors_preset(Control::PRESET_FULL_RECT);
	lbl->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	apply_label_font(ctx, lbl, font);
	lbl->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
	const String j = to_gd(w.list_box.items.justify).to_upper();
	HorizontalAlignment halign = HORIZONTAL_ALIGNMENT_LEFT;
	if (j == "CENTER") {
		halign = HORIZONTAL_ALIGNMENT_CENTER;
	} else if (j == "RIGHT") {
		halign = HORIZONTAL_ALIGNMENT_RIGHT;
	}
	lbl->set_horizontal_alignment(halign);
	combo->add_child(lbl);
	combo->set_item_alignment((int)halign);

	// Items (the parser mirrors LIST_BOX items into w.items; prefer list_box).
	const std::vector<mnu::Item> &src = !w.list_box.items.items.empty()
			? w.list_box.items.items
			: w.items.items;
	for (const auto &it : src) {
		combo->add_item(resolve_item_text(ctx, it), to_gd(it.value));
	}

	// Popup styling from the LIST_BOX.
	bool found = false;
	const Color bgc = get_appearance_color(w.list_box.appearances, "color", "default", found);
	if (found) {
		combo->set_popup_bg_color(bgc);
	}
	Ref<Texture2D> bgt = get_texture(ctx, w.list_box.appearances, "default");
	if (bgt.is_valid()) {
		combo->set_popup_bg_texture(bgt);
	}
	bool ofound = false;
	const Color oc = get_appearance_color(w.list_box.appearances, "outline", "default", ofound);
	if (ofound) {
		combo->set_popup_outline_color(oc);
	}
	const std::string sel_src = w.list_box.items.selection_color.empty()
			? w.items.selection_color
			: w.list_box.items.selection_color;
	const std::string sel = resolve_color(ctx, sel_src);
	if (!sel.empty()) {
		combo->set_selection_color(parse_color(to_gd(sel)));
	}
	if (w.list_box.min_item_height > 0) {
		combo->set_min_item_height(w.list_box.min_item_height);
	}

	// Popup item font.
	if (!font.name.empty()) {
		int fixed = 0;
		Ref<Font> f = resolve_font(ctx, font.name, fixed);
		if (f.is_valid()) {
			combo->set_item_font(f, fixed);
		}
	}
	const std::string fg = resolve_color(ctx, font.default_fg);
	if (!fg.empty()) {
		combo->set_item_font_color(parse_color(to_gd(fg)));
	}

	if (ctx.edit_mode) {
		combo->set_disabled(true);
	}
	if (combo->get_item_count() > 0) {
		combo->select_silent(0);
	}
	return combo;
}

// Themed scrollbar / slider (type="scroll"). Resolves the track / shuttle / arrow
// art and orientation; the value range is bound by a host at runtime.
Control *build_scroll(MnuBuildContext &ctx, const mnu::Window &w) {
	NovaMnuScroll *scroll = memnew(NovaMnuScroll);
	scroll->set_menu(ctx.owner);
	scroll->set_edit_mode(ctx.edit_mode);
	const bool vertical = !iequals(w.orientation, "HORIZONTAL");
	scroll->set_orientation_vertical(vertical);
	scroll->set_track_texture(get_texture(ctx, w.appearances, "default"));
	scroll->set_shuttle_textures(get_texture(ctx, w.shuttle, "default"),
			get_texture(ctx, w.shuttle, "mouseover"));
	scroll->set_arrow_textures(get_texture(ctx, w.scrollup, "default"),
			get_texture(ctx, w.scrollup, "mouseover"), get_texture(ctx, w.scrolldown, "default"),
			get_texture(ctx, w.scrolldown, "mouseover"));
	Ref<Texture2D> up = get_texture(ctx, w.scrollup, "default");
	if (up.is_valid()) {
		scroll->set_arrow_extent(vertical ? up->get_height() : up->get_width());
	}
	const WidgetSounds snd = split_sounds(w.sounds);
	scroll->set_click_sound(snd.click_trigger, snd.click_file);
	return scroll;
}

// Table view (type="table"). Builds the column template + header cells, the clipped
// viewport over a host-populated rows container, and an embedded NovaMnuScroll. Rows
// are added at runtime; the SUBST elements resolve to value->image cells.
Control *build_table(MnuBuildContext &ctx, const mnu::Window &w, const mnu::Font &font) {
	NovaMnuTable *table = memnew(NovaMnuTable);
	table->set_menu(ctx.owner);
	table->set_edit_mode(ctx.edit_mode);
	const mnu::TableData &td = w.table_data;

	const int ncols = td.column.count > 0 ? td.column.count
										  : static_cast<int>(td.column.headers.size());
	const int rowh = td.min_item_height > 0 ? td.min_item_height : 16;
	table->set_row_height(rowh);
	table->set_column_spacing(td.column.spacing);
	table->set_multiselect(td.multiselect);

	// Which columns render bitmaps (from BODY bitmap_draw).
	std::map<int, bool> bitmap_cols;
	for (const auto &b : td.column.bodies) {
		if (b.bitmap_draw) {
			bitmap_cols[b.column] = true;
		}
	}

	// Resolve the cell font once for header + body cells.
	Ref<Font> cell_font;
	int cell_font_size = 0;
	if (!font.name.empty()) {
		cell_font = resolve_font(ctx, font.name, cell_font_size);
	}
	bool has_cell_color = false;
	Color cell_color(1, 1, 1, 1);
	{
		const std::string fg = resolve_color(ctx, font.default_fg);
		if (!fg.empty()) {
			has_cell_color = true;
			cell_color = parse_color(to_gd(fg));
			table->set_cell_font_color(cell_color);
		}
	}
	if (cell_font.is_valid()) {
		table->set_cell_font(cell_font, cell_font_size);
	}

	auto find_header = [&](int col) -> const mnu::TableHeader * {
		for (const auto &h : td.column.headers) {
			if (h.column == col) {
				return &h;
			}
		}
		return nullptr;
	};

	// Column defs (width + justify + bitmap) for the body cell layout.
	for (int c = 0; c < ncols; ++c) {
		const mnu::TableHeader *h = find_header(c);
		const int width = h && h->width > 0 ? h->width : 80;
		HorizontalAlignment ha = HORIZONTAL_ALIGNMENT_LEFT;
		if (h) {
			const String j = to_gd(h->justify).to_upper();
			if (j == "CENTER") {
				ha = HORIZONTAL_ALIGNMENT_CENTER;
			} else if (j == "RIGHT") {
				ha = HORIZONTAL_ALIGNMENT_RIGHT;
			}
		}
		table->add_column(width, (int)ha, bitmap_cols.count(c) > 0);
	}

	// Colours from the ITEMS outline / selection.
	bool has_outline = false;
	Color outline(0.2f, 0.25f, 0.35f, 1.0f);
	{
		const std::string oc = resolve_color(ctx, td.outline_color);
		if (!oc.empty()) {
			has_outline = true;
			outline = parse_color(to_gd(oc));
		}
	}
	Color selection(0.2f, 0.4f, 0.8f, 1.0f);
	{
		const std::string sc = resolve_color(ctx, td.selection_color);
		if (!sc.empty()) {
			selection = parse_color(to_gd(sc));
		}
	}
	table->set_colors(outline, has_outline, selection);

	// Value->image substitutions.
	for (const auto &s : td.column.substitutions) {
		if (s.is_file && !s.file.empty()) {
			table->add_substitution(s.column, to_gd(s.value), resolve_texture(ctx, s.file));
		}
	}

	// Header row + cells (sortable headers are Buttons that drive header_clicked).
	Control *header = memnew(Control);
	header->set_name("HeaderRow");
	int hx = 0;
	for (int c = 0; c < ncols; ++c) {
		const mnu::TableHeader *h = find_header(c);
		const int width = h && h->width > 0 ? h->width : 80;
		const String text = h ? to_gd(h->text) : String();
		const bool sortable = h && !h->sort.empty();
		if (sortable) {
			Button *hb = memnew(Button);
			hb->set_name(String("Header") + String::num_int64(c));
			hb->set_text(text);
			hb->set_flat(true);
			hb->set_position(Vector2(hx, 0));
			hb->set_size(Vector2(width, rowh));
			if (cell_font.is_valid()) {
				hb->add_theme_font_override("font", cell_font);
			}
			if (cell_font_size > 0) {
				hb->add_theme_font_size_override("font_size", cell_font_size);
			}
			if (has_cell_color) {
				hb->add_theme_color_override("font_color", cell_color);
			}
			hb->set_disabled(ctx.edit_mode);
			hb->connect("pressed", callable_mp(table, &NovaMnuTable::header_clicked).bind(c));
			header->add_child(hb);
		} else {
			NovaMnuLabel *hl = memnew(NovaMnuLabel);
			hl->set_name(String("Header") + String::num_int64(c));
			hl->set_text(text);
			hl->set_position(Vector2(hx, 0));
			hl->set_size(Vector2(width, rowh));
			hl->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
			hl->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
			if (cell_font.is_valid()) {
				hl->add_theme_font_override("font", cell_font);
			}
			header->add_child(hl);
		}
		hx += width + td.column.spacing;
	}
	table->add_child(header);

	// Clipped viewport over a host-populated rows container.
	Control *viewport = memnew(Control);
	viewport->set_name("Viewport");
	viewport->set_clip_contents(true);
	viewport->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	Control *rows = memnew(Control);
	rows->set_name("Rows");
	rows->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	viewport->add_child(rows);
	table->add_child(viewport);

	// Embedded vertical scrollbar from the TableScrollbar art.
	NovaMnuScroll *sb = nullptr;
	if (td.scrollbar.present) {
		sb = memnew(NovaMnuScroll);
		sb->set_name("Scrollbar");
		sb->set_menu(ctx.owner);
		sb->set_edit_mode(ctx.edit_mode);
		sb->set_orientation_vertical(true);
		sb->set_shuttle_textures(get_texture(ctx, td.scrollbar.shuttle, "default"),
				get_texture(ctx, td.scrollbar.shuttle, "mouseover"));
		sb->set_arrow_textures(get_texture(ctx, td.scrollbar.scrollup, "default"),
				get_texture(ctx, td.scrollbar.scrollup, "mouseover"),
				get_texture(ctx, td.scrollbar.scrolldown, "default"),
				get_texture(ctx, td.scrollbar.scrolldown, "mouseover"));
		table->add_child(sb);
	}

	table->set_parts(header, viewport, rows, sb);

	// In edit_mode, seed a few inert sample rows so the table reads in the canvas.
	if (ctx.edit_mode) {
		for (int r = 0; r < 3; ++r) {
			PackedStringArray cells;
			for (int c = 0; c < ncols; ++c) {
				cells.push_back("--");
			}
			table->add_row_values(cells);
		}
	}
	return table;
}

// Add the configured appearance (image or colour) as a "Background" behind a view
// widget; in edit_mode, fall back to a subtle labeled placeholder so the bounds read.
void add_view_background(MnuBuildContext &ctx, Control *parent, const mnu::Window &w,
		const mnu::Font &font, const char *label_text, bool allow_placeholder) {
	Ref<Texture2D> bg = get_texture(ctx, w.appearances, "default");
	if (bg.is_valid()) {
		TextureRect *rect = memnew(TextureRect);
		rect->set_name("Background");
		rect->set_texture(bg);
		rect->set_anchors_preset(Control::PRESET_FULL_RECT);
		rect->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
		rect->set_stretch_mode(TextureRect::STRETCH_SCALE);
		rect->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
		parent->add_child(rect);
		return;
	}
	if (has_color(w.appearances)) {
		bool found = false;
		const Color c = get_appearance_color(w.appearances, "color", "default", found);
		if (found) {
			ColorRect *rect = memnew(ColorRect);
			rect->set_name("ColorBg");
			rect->set_color(c);
			rect->set_anchors_preset(Control::PRESET_FULL_RECT);
			rect->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
			parent->add_child(rect);
			return;
		}
	}
	if (allow_placeholder && ctx.edit_mode) {
		Panel *panel = memnew(Panel);
		panel->set_name("Placeholder");
		panel->set_anchors_preset(Control::PRESET_FULL_RECT);
		panel->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
		parent->add_child(panel);
		Label *lbl = memnew(Label);
		lbl->set_name("PlaceholderLabel");
		lbl->set_anchors_preset(Control::PRESET_FULL_RECT);
		lbl->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
		lbl->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
		lbl->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
		apply_label_font(ctx, lbl, font);
		lbl->set_text(label_text);
		parent->add_child(lbl);
	}
}

// Tactical map (type="map"): styled background; a host supplies the map + markers.
Control *build_map(MnuBuildContext &ctx, const mnu::Window &w, const mnu::Font &font) {
	NovaMnuMap *map = memnew(NovaMnuMap);
	map->set_menu(ctx.owner);
	map->set_edit_mode(ctx.edit_mode);
	add_view_background(ctx, map, w, font, "Map", true);
	return map;
}

// Campaign globe (type="globe"): styled background; a host supplies the globe image.
Control *build_globe(MnuBuildContext &ctx, const mnu::Window &w, const mnu::Font &font) {
	NovaMnuGlobe *globe = memnew(NovaMnuGlobe);
	globe->set_menu(ctx.owner);
	globe->set_edit_mode(ctx.edit_mode);
	add_view_background(ctx, globe, w, font, "Globe", true);
	return globe;
}

// Resolve a marquee's DATASOURCE text through the resource root; records the name as
// unresolved (like a texture/font) when it cannot be found.
String resolve_marquee_datasource(MnuBuildContext &ctx, const mnu::Window &w) {
	if (w.datasource.empty()) {
		return String();
	}
	if (ctx.root != nullptr) {
		const String path = ctx.root->resolve_file(to_gd(w.datasource));
		if (!path.is_empty()) {
			return FileAccess::get_file_as_string(path);
		}
	}
	ctx.unresolved_assets.insert(w.datasource);
	return String();
}

// Scrolling credits (type="marquee"): resolves DATASOURCE text (or STRING / a sample
// in edit_mode); a host can repush content at runtime.
Control *build_marquee(MnuBuildContext &ctx, const mnu::Window &w, const mnu::Font &font) {
	NovaMnuMarquee *m = memnew(NovaMnuMarquee);
	m->set_menu(ctx.owner);
	m->set_edit_mode(ctx.edit_mode);
	m->set_orientation_vertical(!iequals(w.orientation, "HORIZONTAL"));
	add_view_background(ctx, m, w, font, "Marquee", false);

	if (!font.name.empty()) {
		int fixed = 0;
		Ref<Font> f = resolve_font(ctx, font.name, fixed);
		if (f.is_valid()) {
			m->set_label_font(f, fixed);
		}
	}
	const std::string fg = resolve_color(ctx, font.default_fg);
	if (!fg.empty()) {
		m->set_label_font_color(parse_color(to_gd(fg)));
	}
	const String j = to_gd(w.string_data.justify).to_upper();
	if (j == "LEFT") {
		m->set_justify((int)HORIZONTAL_ALIGNMENT_LEFT);
	} else if (j == "RIGHT") {
		m->set_justify((int)HORIZONTAL_ALIGNMENT_RIGHT);
	} else {
		m->set_justify((int)HORIZONTAL_ALIGNMENT_CENTER);
	}

	String content = resolve_marquee_datasource(ctx, w);
	if (content.is_empty()) {
		content = resolve_text(ctx, w.string_data);
	}
	if (content.is_empty() && ctx.edit_mode) {
		content = "Credits";
	}
	m->set_content(content);
	return m;
}

// Logical navigation marker (type="goto"): invisible, zero-size, carries actions
// and an optional hotkey. trigger() dispatches through the owning menu.
Control *build_goto(MnuBuildContext &ctx, const mnu::Window &w) {
	NovaMnuGoto *go = memnew(NovaMnuGoto);
	go->set_menu(ctx.owner);
	go->set_edit_mode(ctx.edit_mode);
	for (const auto &act : w.actions) {
		go->add_action(make_action(act));
	}
	if (!w.hotkey.empty()) {
		go->set_hotkey(to_gd(w.hotkey), w.hotkey_virtual);
	} else if (!w.actions.empty()) {
		go->set_fire_on_show(true);
	}
	go->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	return go;
}

Control *build_window(MnuBuildContext &ctx, const mnu::Window &w, NameTracker &names,
		const mnu::Font &inherited_font, const mnu::Frame &inherited_frame, ButtonGroupMap &groups,
		int widget_id);

void build_children(MnuBuildContext &ctx, Control *parent, const mnu::Window &w,
		const mnu::Font &font, const mnu::Frame &frame, NameTracker &names, ButtonGroupMap &groups,
		int widget_id) {
	// The document id-tree mirrors mnu::Document by child index, so child[i]'s stable
	// id is get_child_ids(widget_id)[i]. -1 when no document is associated.
	PackedInt32Array child_ids;
	if (ctx.document != nullptr && widget_id >= 0) {
		child_ids = ctx.document->get_child_ids(widget_id);
	}
	for (int i = 0; i < static_cast<int>(w.children.size()); ++i) {
		const int child_id = i < child_ids.size() ? child_ids[i] : -1;
		Control *node = build_window(ctx, w.children[i], names, font, frame, groups, child_id);
		if (node) {
			parent->add_child(node);
		}
	}
}

Control *build_window(MnuBuildContext &ctx, const mnu::Window &w, NameTracker &names,
		const mnu::Font &inherited_font, const mnu::Frame &inherited_frame, ButtonGroupMap &groups,
		int widget_id) {
	// Merge fonts (child overrides parent), then pass down to descendants.
	mnu::Font font = inherited_font;
	if (!w.font.name.empty()) {
		font.name = w.font.name;
	}
	if (!w.font.default_fg.empty()) {
		font.default_fg = w.font.default_fg;
	}
	if (!w.font.mouseover_fg.empty()) {
		font.mouseover_fg = w.font.mouseover_fg;
	}
	if (!w.font.selected_fg.empty()) {
		font.selected_fg = w.font.selected_fg;
	}
	if (!w.font.disabled_fg.empty()) {
		font.disabled_fg = w.font.disabled_fg;
	}

	Control *node = nullptr;
	switch (w.type) {
		case mnu::WindowType::Button:
		case mnu::WindowType::Radio:
			node = build_button(ctx, w, font, groups);
			break;
		case mnu::WindowType::CheckBox:
			node = build_checkbox(ctx, w, font);
			break;
		case mnu::WindowType::Static:
		case mnu::WindowType::Label:
		case mnu::WindowType::Window:
			node = build_container(ctx, w, font, inherited_frame);
			break;
		case mnu::WindowType::Edit:
			node = build_edit(ctx, w, font);
			break;
		case mnu::WindowType::MultilineEdit:
			node = build_multiline_edit(ctx, w, font);
			break;
		case mnu::WindowType::List:
			node = build_list(ctx, w, font);
			break;
		case mnu::WindowType::Multi:
			node = build_multi(ctx, w, font);
			break;
		case mnu::WindowType::SpinList:
			node = build_spinlist(ctx, w, font);
			break;
		case mnu::WindowType::Combo:
			node = build_combo(ctx, w, font);
			break;
		case mnu::WindowType::Scroll:
			node = build_scroll(ctx, w);
			break;
		case mnu::WindowType::Table:
			node = build_table(ctx, w, font);
			break;
		case mnu::WindowType::Map:
			node = build_map(ctx, w, font);
			break;
		case mnu::WindowType::Globe:
			node = build_globe(ctx, w, font);
			break;
		case mnu::WindowType::Marquee:
			node = build_marquee(ctx, w, font);
			break;
		case mnu::WindowType::Goto:
			node = build_goto(ctx, w);
			break;
		default:
			node = build_placeholder(ctx, w, font);
			break;
	}

	if (node == nullptr) {
		node = memnew(Control);
	}

	node->set_name(names.get_unique(to_gd(w.name)));
	// Tag with the stable document id so the editor can map this Control back to its
	// widget (and read its rendered size for widgets whose document rect is sizeless).
	if (widget_id >= 0) {
		node->set_meta("mnu_widget_id", widget_id);
	}
	apply_position(ctx, node, w);

	if (w.hidden) {
		node->set_visible(false);
	}
	if (ctx.edit_mode) {
		node->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	}

	// Own frame takes priority over inherited for child inheritance.
	const mnu::Frame &frame_to_inherit = has_frame(w.frame) ? w.frame : inherited_frame;
	build_children(ctx, node, w, font, frame_to_inherit, names, groups, widget_id);

	// Outline draws on top of children.
	if ((w.type == mnu::WindowType::Window || w.type == mnu::WindowType::Static ||
				w.type == mnu::WindowType::Label) &&
			has_outline(w.appearances)) {
		add_outline(node, w.appearances);
	}

	return node;
}

} // namespace

namespace godot {

Control *mnu_build_screen(const mnu::Screen &screen, MnuBuildContext &ctx, int root_window_id) {
	NovaMnuScreen *screen_node = memnew(NovaMnuScreen);
	screen_node->set_name(to_gd(screen.name).is_empty() ? String("Screen") : to_gd(screen.name));
	screen_node->set_screen_name(to_gd(screen.name));
	screen_node->set_music_var(screen.music_var);
	screen_node->set_cursor_file(to_gd(screen.cursor_file));
	screen_node->set_edit_mode(ctx.edit_mode);
	screen_node->set_anchors_preset(Control::PRESET_FULL_RECT);
	if (ctx.edit_mode) {
		screen_node->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	}

	// Resolve the cursor art (MNU keeps a single cursor on the root window;
	// Screen.cursor_file mirrors it) so the screen can apply it when shown.
	std::string cursor_name = screen.cursor_file;
	if (cursor_name.empty()) {
		cursor_name = screen.root_window.cursor.file;
	}
	if (!cursor_name.empty()) {
		Ref<Texture2D> cursor_tex = resolve_texture(ctx, cursor_name);
		if (cursor_tex.is_valid()) {
			screen_node->set_cursor_texture(cursor_tex);
		}
	}

	NameTracker names;
	ButtonGroupMap groups;
	Control *root = build_window(ctx, screen.root_window, names, screen.root_window.font,
			screen.root_window.frame, groups, root_window_id);
	if (root) {
		screen_node->add_child(root);
	}
	return screen_node;
}

} // namespace godot
