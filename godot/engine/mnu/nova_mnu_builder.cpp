#include "nova_mnu_builder.h"

#include "nova_mnu_button.h"
#include "nova_mnu_checkbox.h"
#include "nova_mnu_label.h"
#include "nova_mnu_screen.h"

#include "fnt/nova_fnt_resource.h"
#include "mns_stylesheet.h"
#include "resource_index/nova_resource_root.h"
#include "rtxt/rtxt_string_file.h"

#include <godot_cpp/classes/atlas_texture.hpp>
#include <godot_cpp/classes/button_group.hpp>
#include <godot_cpp/classes/color_rect.hpp>
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
// renders on top (port of the outline pass in create_window).
void add_outline(Control *node, const std::vector<mnu::Appearance> &apps) {
	bool found = false;
	const Color outline_color = get_appearance_color(apps, "outline", "default", found);
	if (!found) {
		return;
	}
	const int bw = 1;
	struct Edge {
		const char *name;
		float al, at, ar, ab;
		float ol, ot, orr, ob;
	};
	const Edge edges[] = {
		{ "BorderTop", 0, 0, 1, 0, 0, 0, 0, (float)bw },
		{ "BorderBottom", 0, 1, 1, 1, 0, (float)-bw, 0, 0 },
		{ "BorderLeft", 0, 0, 0, 1, 0, 0, (float)bw, 0 },
		{ "BorderRight", 1, 0, 1, 1, (float)-bw, 0, 0, 0 },
	};
	for (const Edge &e : edges) {
		ColorRect *r = memnew(ColorRect);
		r->set_name(e.name);
		r->set_color(outline_color);
		r->set_anchor(SIDE_LEFT, e.al);
		r->set_anchor(SIDE_TOP, e.at);
		r->set_anchor(SIDE_RIGHT, e.ar);
		r->set_anchor(SIDE_BOTTOM, e.ab);
		r->set_offset(SIDE_LEFT, e.ol);
		r->set_offset(SIDE_TOP, e.ot);
		r->set_offset(SIDE_RIGHT, e.orr);
		r->set_offset(SIDE_BOTTOM, e.ob);
		r->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
		node->add_child(r);
	}
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

Control *build_window(MnuBuildContext &ctx, const mnu::Window &w, NameTracker &names,
		const mnu::Font &inherited_font, const mnu::Frame &inherited_frame, ButtonGroupMap &groups);

void build_children(MnuBuildContext &ctx, Control *parent, const mnu::Window &w,
		const mnu::Font &font, const mnu::Frame &frame, NameTracker &names, ButtonGroupMap &groups) {
	for (const auto &child : w.children) {
		Control *node = build_window(ctx, child, names, font, frame, groups);
		if (node) {
			parent->add_child(node);
		}
	}
}

Control *build_window(MnuBuildContext &ctx, const mnu::Window &w, NameTracker &names,
		const mnu::Font &inherited_font, const mnu::Frame &inherited_frame, ButtonGroupMap &groups) {
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
		default:
			node = build_placeholder(ctx, w, font);
			break;
	}

	if (node == nullptr) {
		node = memnew(Control);
	}

	node->set_name(names.get_unique(to_gd(w.name)));
	apply_position(ctx, node, w);

	if (w.hidden) {
		node->set_visible(false);
	}
	if (ctx.edit_mode) {
		node->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	}

	// Own frame takes priority over inherited for child inheritance.
	const mnu::Frame &frame_to_inherit = has_frame(w.frame) ? w.frame : inherited_frame;
	build_children(ctx, node, w, font, frame_to_inherit, names, groups);

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

Control *mnu_build_screen(const mnu::Screen &screen, MnuBuildContext &ctx) {
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
			screen.root_window.frame, groups);
	if (root) {
		screen_node->add_child(root);
	}
	return screen_node;
}

} // namespace godot
