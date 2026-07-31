#include "nova_mnu_builder.h"

#include "util/nova_string_convert.h"

#include "mnu_item_cell.h"
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

#include "cbin/cbin_credits_resource.h"
#include "cbin/nova_credits_player.h"
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

using opennova::to_gd;
using opennova::to_std;

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

// Map a widget's <SOUND> nodes onto the three per-state slots. The original
// keys each sound by its STATE attribute, NOT by the trigger string: MOUSEIN=1,
// MOUSEOUT=2, SELECTED=3 (case-insensitive full-token match), and stores
// {trigger, bank} per state. The trigger is an opaque LWF sound-set name; the
// element text names the .lwf bank file.
// [orig: CUIElement_ParseXMLDefinition @ 0x648120 — SOUND branch; slots at
//  elem+0xA0, state mask at elem+0x9C. A SOUND missing STATE or TRIGGER fails
//  the element parse there (E_FAIL); we keep authoring lenient (ADR 0002) and
//  simply leave the slot unset.]
MnuWidgetSounds make_widget_sounds(const std::vector<mnu::Sound> &sounds) {
	MnuWidgetSounds out;
	for (const auto &s : sounds) {
		const String state = to_gd(s.state).to_upper();
		int slot = 0;
		if (state == "MOUSEIN") {
			slot = MNU_SOUND_MOUSEIN;
		} else if (state == "MOUSEOUT") {
			slot = MNU_SOUND_MOUSEOUT;
		} else if (state == "SELECTED") {
			slot = MNU_SOUND_SELECTED;
		} else {
			continue;
		}
		// Triggers are matched case-insensitively at playback (stricmp in the
		// original); keep the authored casing for round-trip displays.
		out.set_slot(slot, to_gd(s.trigger), to_gd(s.file));
	}
	return out;
}

Ref<Texture2D> get_texture(MnuBuildContext &ctx,
		const std::vector<mnu::Appearance> &apps, const std::string &state);

template <typename Scrollbar>
MnuScrollbarStyle make_scrollbar_style(MnuBuildContext &ctx, const Scrollbar &scrollbar,
		int p_edge_pad = 0) {
	MnuScrollbarStyle style;
	style.present = scrollbar.present;
	style.edge_pad = MAX(p_edge_pad, 0);
	if (!scrollbar.present) {
		return style;
	}
	const mnu::Position &position = scrollbar.position;
	if (position.has_left && position.has_top) {
		float width = position.has_right ? (float)(position.right - position.left) : 0.0f;
		float height = position.has_bottom ? (float)(position.bottom - position.top) : 0.0f;
		style.rect = Rect2((float)position.left, (float)position.top, width, height);
		style.has_rect = true;
	}
	style.track = get_texture(ctx, scrollbar.track, "default");
	style.shuttle = get_texture(ctx, scrollbar.shuttle, "default");
	style.shuttle_hover = get_texture(ctx, scrollbar.shuttle, "mouseover");
	style.shuttle_pressed = get_texture(ctx, scrollbar.shuttle, "selected");
	style.shuttle_disabled = get_texture(ctx, scrollbar.shuttle, "disabled");
	style.up = get_texture(ctx, scrollbar.scrollup, "default");
	style.up_hover = get_texture(ctx, scrollbar.scrollup, "mouseover");
	style.up_pressed = get_texture(ctx, scrollbar.scrollup, "selected");
	style.up_disabled = get_texture(ctx, scrollbar.scrollup, "disabled");
	style.down = get_texture(ctx, scrollbar.scrolldown, "default");
	style.down_hover = get_texture(ctx, scrollbar.scrolldown, "mouseover");
	style.down_pressed = get_texture(ctx, scrollbar.scrolldown, "selected");
	style.down_disabled = get_texture(ctx, scrollbar.scrolldown, "disabled");
	style.sounds = make_widget_sounds(scrollbar.sounds);
	return style;
}

NovaMnuScroll *make_authored_scroll(MnuBuildContext &ctx,
		const MnuScrollbarStyle &style, bool p_vertical = true) {
	if (!style.present) {
		return nullptr;
	}
	NovaMnuScroll *scroll = memnew(NovaMnuScroll);
	scroll->set_name("Scrollbar");
	scroll->set_menu(ctx.owner);
	scroll->set_edit_mode(ctx.inert());
	scroll->set_orientation_vertical(p_vertical);
	scroll->set_track_texture(style.track);
	scroll->set_shuttle_state_textures(style.shuttle, style.shuttle_hover,
			style.shuttle_pressed, style.shuttle_disabled);
	scroll->set_arrow_state_textures(style.up, style.up_hover, style.up_pressed,
			style.up_disabled, style.down, style.down_hover, style.down_pressed,
			style.down_disabled);
	scroll->set_sounds(style.sounds);
	if (style.up.is_valid()) {
		scroll->set_arrow_extent(p_vertical ? style.up->get_height() : style.up->get_width());
	}
	if (style.has_rect) {
		Rect2 rect = style.rect;
		if (rect.size.x <= 0.0f && style.up.is_valid()) {
			rect.size.x = style.up->get_width();
		}
		if (rect.size.y <= 0.0f && style.up.is_valid() && !p_vertical) {
			rect.size.y = style.up->get_height();
		}
		scroll->set_position(rect.position);
		scroll->set_size(rect.size);
	}
	return scroll;
}

// Collapse an MNU <ACTION> into the widget's plain-data action list (verb +
// window_state lowercased to match dispatch_action's comparisons).
MnuActionData make_action(const mnu::Action &act) {
	MnuActionData data;
	data.type = to_gd(act.type).to_lower();
	data.target = to_gd(act.target);
	data.file = to_gd(act.file);
	data.window_state = to_gd(act.state).to_lower();
	data.source = to_gd(act.source);
	data.field = to_gd(act.field);
	data.has_target_form = act.has_target_form;
	data.target_form = act.target_form;
	data.external_browser = act.external_browser;
	data.toggle = act.toggle;
	data.test = to_gd(act.test);
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
		// Retail UI image loads force loose-first for this lookup, independent of /d.
		// [orig: CUIImage_LoadTextureFromFile @ 0x6541ba]
		tex = ctx.root->load_texture(gname, NovaResourceRoot::LOOKUP_FORCE_LOOSE_FIRST);
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
			if (tex.is_valid() && app.has_map_state && app.map_state >= 0 &&
					app.has_height && app.height > 0) {
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

Color get_appearance_color(MnuBuildContext &ctx, const std::vector<mnu::Appearance> &apps,
		const std::string &type, const std::string &state, bool &found) {
	found = false;
	for (const auto &app : apps) {
		if (app.state == state && app.type == type && !app.value.empty()) {
			// The value is frequently a %VAR% (e.g. %COLOR_BLACK% / %TRIM_COLOR% on a
			// LIST_BOX background or outline); resolve it through the stylesheet before
			// parsing the hex [orig: NapiXML_ExpandVariablesInText @ 0x63a000]. Without
			// this every %VAR% color appearance silently failed (transparent combo
			// popups, container backgrounds, outlines).
			const std::string hex = resolve_color(ctx, app.value);
			uint8_t r, g, b, a;
			if (!hex.empty() && mnu::parse_hex_color(hex, r, g, b, a)) {
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

// --- Frame border + fill ([orig: CUIElement_DrawFrame @ 0x64a210]) -----------
//
// The stencil texture is a grid of SIZE x SIZE tiles (canonically 4*SIZE on a
// side): row 0 = top-left / top edge / top-right corners (+ the fill tile at
// column 3), row 1 = left edge / right edge (columns 0 and 2), row 2 =
// bottom-left / bottom edge / bottom-right [orig: init_border_materials
// @ 0x646f70 slices these UV rects; the row-3 variants it also bakes are not
// drawn by the frame pass]. The 8 border pieces hang OUTSIDE the window rect
// by SIZE, pulled back in by the authored STENCIL INSETX/INSETY (default 0);
// edge pieces stretch between the corners; the BRUSH tiles across the whole
// rect underneath. The original modulates every quad by 0x7F7F7F - neutral in
// its modulate-2x fixed-function path - which maps to no tint here.

Ref<Texture2D> frame_tile(const Ref<Texture2D> &stencil, int size, int col, int row) {
	Ref<AtlasTexture> tile;
	tile.instantiate();
	tile->set_atlas(stencil);
	tile->set_region(Rect2(col * size, row * size, size, size));
	return tile;
}

TextureRect *frame_piece(Control *parent, const String &name, const Ref<Texture2D> &tex) {
	TextureRect *r = memnew(TextureRect);
	r->set_name(name);
	r->set_texture(tex);
	r->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
	r->set_stretch_mode(TextureRect::STRETCH_SCALE);
	r->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	parent->add_child(r);
	return r;
}

// Anchor one border piece: the anchors pick which window edges it follows and
// the offsets carry the element-local geometry from the original drawer.
void frame_anchor(TextureRect *r, float ax0, float ax1, float ay0, float ay1,
		float p_left, float p_top, float p_right, float p_bottom) {
	r->set_anchor(SIDE_LEFT, ax0);
	r->set_anchor(SIDE_RIGHT, ax1);
	r->set_anchor(SIDE_TOP, ay0);
	r->set_anchor(SIDE_BOTTOM, ay1);
	r->set_offset(SIDE_LEFT, p_left);
	r->set_offset(SIDE_TOP, p_top);
	r->set_offset(SIDE_RIGHT, p_right);
	r->set_offset(SIDE_BOTTOM, p_bottom);
}

// Render a frame onto a container: the tiling brush fill + the 8-piece stencil
// border, or a flat dark panel fallback when neither texture resolves (a
// visible affordance for missing assets; the original simply draws nothing).
void add_frame(MnuBuildContext &ctx, Control *parent, const mnu::Frame &frame) {
	if (!has_frame(frame)) {
		return;
	}

	Ref<Texture2D> stencil;
	if (!frame.stencil.empty()) {
		stencil = resolve_texture(ctx, frame.stencil);
	}
	Ref<Texture2D> brush;
	if (!frame.brush.empty()) {
		brush = resolve_texture(ctx, frame.brush);
	}

	if (stencil.is_null() && brush.is_null()) {
		// The original draws nothing when neither frame texture resolves: every draw
		// in CUIElement_DrawFrame is guarded by a successful texture load
		// [orig: CUIElement_DrawFrame @ 0x64a210 -> sub_654370 >= 0 checks]. So at
		// runtime an unresolved frame is simply absent (no opaque panel). In the
		// editor only, leave a faint placeholder so an author can see the region.
		if (ctx.edit_mode) {
			ColorRect *bg = memnew(ColorRect);
			bg->set_name("FramePlaceholder");
			bg->set_color(Color(1.0f, 1.0f, 1.0f, 0.04f));
			bg->set_anchors_preset(Control::PRESET_FULL_RECT);
			bg->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
			parent->add_child(bg);
		}
		return;
	}

	// The brush tiles across the whole window rect, under the border
	// [orig: the center quad @ 0x64a210 wraps the brush over rcDst].
	if (brush.is_valid()) {
		TextureRect *fill = memnew(TextureRect);
		fill->set_name("FrameFill");
		fill->set_texture(brush);
		fill->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
		fill->set_stretch_mode(TextureRect::STRETCH_TILE);
		fill->set_anchors_preset(Control::PRESET_FULL_RECT);
		fill->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
		parent->add_child(fill);
	}

	if (stencil.is_valid()) {
		// SIZE is the authored STENCIL attr; a canonical stencil is a 4x4 tile
		// grid, so width/4 recovers the tile size when it is not authored.
		int size = frame.has_stencil_size ? frame.stencil_size : 0;
		if (size <= 0) {
			size = (int)stencil->get_width() / 4;
		}
		if (size > 0) {
			const float ix = (float)(frame.has_insetx ? frame.insetx : 0);
			const float iy = (float)(frame.has_insety ? frame.insety : 0);
			const float x0 = -(float)size + ix; // border overhangs the rect
			const float y0 = -(float)size + iy; // [orig: left - SIZE + INSETX]
			const float xr = -ix - 1.0f; // against the right edge (anchor 1)
			const float yb = -iy - 1.0f; // against the bottom edge (anchor 1)

			TextureRect *p = nullptr;
			p = frame_piece(parent, "FrameTL", frame_tile(stencil, size, 0, 0));
			frame_anchor(p, 0, 0, 0, 0, x0, y0, x0 + size, y0 + size);
			p = frame_piece(parent, "FrameTop", frame_tile(stencil, size, 1, 0));
			frame_anchor(p, 0, 1, 0, 0, x0 + size, y0, xr, y0 + size);
			p = frame_piece(parent, "FrameTR", frame_tile(stencil, size, 2, 0));
			frame_anchor(p, 1, 1, 0, 0, xr, y0, xr + size, y0 + size);
			p = frame_piece(parent, "FrameLeft", frame_tile(stencil, size, 0, 1));
			frame_anchor(p, 0, 0, 0, 1, x0, y0 + size, x0 + size, yb);
			p = frame_piece(parent, "FrameRight", frame_tile(stencil, size, 2, 1));
			frame_anchor(p, 1, 1, 0, 1, xr, y0 + size, xr + size, yb);
			p = frame_piece(parent, "FrameBL", frame_tile(stencil, size, 0, 2));
			frame_anchor(p, 0, 0, 1, 1, x0, yb, x0 + size, yb + size);
			p = frame_piece(parent, "FrameBottom", frame_tile(stencil, size, 1, 2));
			frame_anchor(p, 0, 1, 1, 1, x0 + size, yb, xr, yb + size);
			p = frame_piece(parent, "FrameBR", frame_tile(stencil, size, 2, 2));
			frame_anchor(p, 1, 1, 1, 1, xr, yb, xr + size, yb + size);
		}
	}

	// MONOGRAM is parsed and round-tripped but intentionally NOT drawn: the shipped
	// engine never renders the menu monogram. The window render path draws frame +
	// appearance + text + children only [orig: CStaticWnd_Render @ 0x657b10], and
	// CUIElement_DrawFrame @ 0x64a210 has no monogram pass; the only "monogram.tga"
	// use is the loading screen (Game_StartMission @ 0x525aa3), not menus. The earlier
	// centered-heuristic draw produced a stray glyph in the middle of framed panels.
}

// --- Text / label -----------------------------------------------------------

// Resolve a widget's display text: a type="id" string looks up the RTXT table
// (falling back to the raw id when unavailable), then the {hot} marker is
// stripped. Literal strings are returned as-is (minus any marker).
String resolve_text(MnuBuildContext &ctx, const mnu::String &sd) {
	if (!sd.present || sd.value.empty()) {
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
	// %VAR% in literal text resolves through the stylesheet, the same as colors
	// and fonts. The engine expands %VAR% over the whole buffer pre-parse, so a
	// var can appear in ANY field [orig: NapiXML_ExpandVariablesInText
	// @ 0x63a000]; we keep raw tokens in the document for round-trip and expand
	// per consumed field at build time (see ADR 0005).
	const String text = substitute_var(ctx, to_gd(sd.value));
	return to_gd(mnu::strip_hotkey_marker(to_std(text)));
}

// Build a LabelSettings from the widget font (name + default fg color), or null when
// the font carries nothing to apply. Shared by plain labels and item cells.
Ref<LabelSettings> make_label_settings(MnuBuildContext &ctx, const mnu::Font &font) {
	if (font.name.empty() && font.default_fg.empty()) {
		return Ref<LabelSettings>();
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
	return settings;
}

void apply_label_font(MnuBuildContext &ctx, Label *lbl, const mnu::Font &font) {
	Ref<LabelSettings> settings = make_label_settings(ctx, font);
	if (settings.is_valid()) {
		lbl->set_label_settings(settings);
	}
}

// --- Positioning ([orig: CUIElement_ParseXMLDefinition @ 0x648120]) ----------

// Per-image contributions to the degenerate-axis fallback: width is the texture
// width; height is the authored HEIGHT attr (the sprite-sheet frame height)
// when present, else the texture height. The original tracks the max across
// every IMAGE appearance while parsing [orig: APPEARANCE branch @ 0x648120,
// max_texture_width/height accumulation].
void measure_appearance_extents(MnuBuildContext &ctx, const std::vector<mnu::Appearance> &apps,
		int &r_max_w, int &r_max_h) {
	r_max_w = 0;
	r_max_h = 0;
	for (const auto &app : apps) {
		if (app.type != "image" || app.value.empty()) {
			continue;
		}
		Ref<Texture2D> tex = resolve_texture(ctx, app.value);
		if (tex.is_null()) {
			continue;
		}
		r_max_w = MAX(r_max_w, (int)tex->get_width());
		const int h = app.has_height && app.height > 0
				? app.height
				: (int)tex->get_height();
		r_max_h = MAX(r_max_h, h);
	}
}

// Widget families whose original parse ends in the text-extent rect adjustment
// [orig: adjust_rect_to_text_size @ 0x6575f0, reached from the shared static/
//  button text-widget parse @ 0x657c30 (a vtable init slot) and the edit
//  override @ 0x661d10].
bool is_text_sized(mnu::WindowType t) {
	return t == mnu::WindowType::Static || t == mnu::WindowType::Label ||
			t == mnu::WindowType::Button || t == mnu::WindowType::Radio ||
			t == mnu::WindowType::CheckBox || t == mnu::WindowType::Edit ||
			t == mnu::WindowType::Marquee;
}

// Faithful three-stage layout. The original has NO per-type default sizes and
// no texture-derived sizing outside these fallbacks; the texture is stretched
// INTO whatever rect results (UV 0..1) [orig: sub_647D40 @ 0x647d40].
void apply_position(MnuBuildContext &ctx, Control *node, const mnu::Window &w,
		const mnu::Font &font) {
	node->set_anchor(SIDE_LEFT, 0);
	node->set_anchor(SIDE_TOP, 0);
	node->set_anchor(SIDE_RIGHT, 0);
	node->set_anchor(SIDE_BOTTOM, 0);

	// Stage 1: the authored POSITION rect. LEFT/TOP/RIGHT/BOTTOM are absolute
	// parent-relative edges (ULX/ULY/WIDTH/HEIGHT aliases fold into the same
	// fields at parse); missing edges read 0, like the original's zeroed
	// element fields [orig: POSITION branch @ 0x648120].
	int left = w.position.has_left ? w.position.left : 0;
	int top = w.position.has_top ? w.position.top : 0;
	int right = w.position.has_right ? w.position.right : 0;
	int bottom = w.position.has_bottom ? w.position.bottom : 0;

	// Stage 2: a degenerate axis (right<=left / bottom<=top) falls back to the
	// largest appearance image [orig: parse tail right<=left -> left+max_w,
	// bottom<=top -> top+max_h].
	if (right <= left || bottom <= top) {
		int max_w = 0;
		int max_h = 0;
		measure_appearance_extents(ctx, w.appearances, max_w, max_h);
		if (right <= left) {
			right = left + max_w;
		}
		if (bottom <= top) {
			bottom = top + max_h;
		}
	}

	// Stage 3: text widgets size a still-degenerate axis from the measured
	// string, with the authored point as the anchor the JUSTIFY/VJUSTIFY flags
	// align to (left edge / centre / right edge; top / centre / bottom)
	// [orig: adjust_rect_to_text_size @ 0x6575f0].
	if ((right <= left || bottom <= top) && is_text_sized(w.type) &&
			w.string_data.present && !w.string_data.value.empty()) {
		const String text = resolve_text(ctx, w.string_data);
		if (!text.is_empty()) {
			int fixed = 0;
			Ref<Font> f = resolve_font(ctx, font.name, fixed);
			Vector2 ts;
			if (f.is_valid()) {
				ts = f->get_string_size(text, HORIZONTAL_ALIGNMENT_LEFT, -1.0f,
						fixed > 0 ? fixed : 16);
			} else {
				// No resolvable font (missing assets / headless tests): a nominal
				// glyph box keeps text widgets visible and hittable.
				ts = Vector2(8.0f * (float)text.length(), 16.0f);
			}
			const int tw = (int)(ts.x + 0.5f);
			const int th = (int)(ts.y + 0.5f);
			if (right <= left) {
				const int anchor = left; // right==left after stage 2
				if (iequals(w.string_data.justify, "center")) {
					left = anchor - tw / 2;
					right = left + tw;
				} else if (iequals(w.string_data.justify, "right")) {
					left = anchor - tw; // the right edge stays at the anchor
				} else {
					right = anchor + tw;
				}
			}
			if (bottom <= top) {
				const int anchor = top;
				if (iequals(w.string_data.vjustify, "center")) {
					top = anchor - th / 2;
					bottom = top + th;
				} else if (iequals(w.string_data.vjustify, "bottom")) {
					top = anchor - th; // the bottom edge stays at the anchor
				} else {
					bottom = anchor + th;
				}
			}
		}
	}

	node->set_position(Vector2(left, top));
	node->set_size(Vector2(MAX(right - left, 0), MAX(bottom - top, 0)));
}

// 1px outline border drawn with four ColorRects, added after children so it
// renders on top (port of the outline pass in create_window). The 4-rect recipe
// is shared via mnu_outline.h so the richer M9 widgets can reuse it.
void add_outline(MnuBuildContext &ctx, Control *node, const std::vector<mnu::Appearance> &apps) {
	bool found = false;
	const Color outline_color = get_appearance_color(ctx, apps, "outline", "default", found);
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
		if (w.has_group && w.group > 0) {
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
	btn->set_edit_mode(ctx.inert());
	for (const auto &act : w.actions) {
		btn->add_action(make_action(act));
	}
	btn->set_sounds(make_widget_sounds(w.sounds));
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

	if (ctx.inert() || w.disabled) {
		btn->set_disabled(true); // inert while authoring
	}

	if (w.string_data.present && !w.string_data.value.empty()) {
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
	// The original stretches every element texture into its POSITION rect
	// (UV 0..1) [orig: sub_647D40 @ 0x647d40] - checkboxes included; the old
	// keep-aspect choice came from the reference repo, not the engine.
	check->set_stretch_mode(TextureButton::STRETCH_SCALE);
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
	check->set_edit_mode(ctx.inert());
	check->set_sounds(make_widget_sounds(w.sounds));
	{
		Color underline_c(1, 0, 0, 1);
		const std::string sfg = resolve_color(ctx, font.selected_fg);
		if (!sfg.empty()) {
			underline_c = parse_color(to_gd(sfg));
		}
		check->set_underline_color(underline_c);
	}

	if (ctx.inert() || w.disabled) {
		check->set_disabled(true);
	}

	if (w.string_data.present && !w.string_data.value.empty()) {
		NovaMnuLabel *lbl = memnew(NovaMnuLabel);
		lbl->set_name("Label");
		if (iequals(w.string_data.type, "id")) {
			lbl->set_string_id(to_gd(w.string_data.value));
		}
		lbl->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
		apply_label_font(ctx, lbl, font);

		const String justify = to_gd(w.string_data.justify).to_upper();
		const String vjustify = to_gd(w.string_data.vjustify).to_upper();
		if (vjustify == "TOP") {
			lbl->set_vertical_alignment(VERTICAL_ALIGNMENT_TOP);
		} else if (vjustify == "BOTTOM") {
			lbl->set_vertical_alignment(VERTICAL_ALIGNMENT_BOTTOM);
		} else {
			lbl->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
		}
		if (justify == "RIGHT") {
			lbl->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_RIGHT);
		} else if (justify == "CENTER") {
			lbl->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
		} else {
			lbl->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_LEFT);
		}
		lbl->set_text(resolve_text(ctx, w.string_data));

		if (w.as_button) {
			// AS_BUTTON changes checkbox label layout only: retail leaves the
			// label inside the full widget rect and honors its authored justify.
			// Toggle/event behavior and selected appearance remain checkbox-like.
			// [orig: CCheckWnd_DrawLabel @ 0x64aa20; flag parsed @ 0x64ad90]
			lbl->set_anchors_preset(Control::PRESET_FULL_RECT);
		} else {
			int checkbox_width = 24;
			int checkbox_height = 24;
			if (tex_normal.is_valid()) {
				checkbox_width = tex_normal->get_width();
				checkbox_height = tex_normal->get_height();
			}
			lbl->set_position(Vector2(checkbox_width + 2, 0));
			lbl->set_size(Vector2(300, checkbox_height));
		}
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

	// A window draws a frame ONLY when DRAW_FRAME is set; the textures are its own
	// <FRAME> if present, else the nearest inherited <FRAME>. A window can define a
	// <FRAME> purely to hand textures down to framed descendants without drawing one
	// itself (e.g. the root MAIN, which carries the camo BOXTILE brush but no
	// DRAW_FRAME). The render path gates the frame draw on the DRAW_FRAME flag while
	// leaving the appearance/texture passes ungated, so a window's own image still
	// shows [orig: CStaticWnd_Render @ 0x657b10 -> field +0x134 guards CUIElement_DrawFrame].
	if (w.draw_frame) {
		if (has_frame(w.frame)) {
			add_frame(ctx, container, w.frame);
		} else if (has_frame(inherited_frame)) {
			add_frame(ctx, container, inherited_frame);
		}
	}

	if (has_color(w.appearances)) {
		bool found = false;
		const Color bg_color = get_appearance_color(ctx, w.appearances, "color", "default", found);
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

	if (w.string_data.present && !w.string_data.value.empty()) {
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

Control *build_shell_owned_placeholder(MnuBuildContext &ctx, const mnu::Window &w,
		const mnu::Font &font) {
	Control *panel = build_placeholder(ctx, w, font);
	panel->set_meta("mnu_shell_owned", true);
	panel->set_meta("mnu_shell_widget_type",
			to_gd(mnu::window_type_name(w.type)).to_upper());
	if (ctx.edit_mode) {
		// These retail factories exist, but their rows/content are populated by
		// multiplayer or news shells. The authoring canvas labels that boundary
		// explicitly instead of fabricating representative server data.
		Label *notice = memnew(Label);
		notice->set_name("ShellOwnedPreview");
		notice->set_anchors_preset(Control::PRESET_FULL_RECT);
		notice->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
		notice->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
		notice->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
		notice->set_text(to_gd(mnu::window_type_name(w.type)).to_upper() +
				"\nGame-supplied runtime data");
		notice->add_theme_color_override("font_color", Color(0.72, 0.78, 0.86, 0.9));
		panel->add_child(notice);
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
	const std::string disabled_fg = resolve_color(ctx, font.disabled_fg);
	if (!fg.empty()) {
		const Color normal = parse_color(to_gd(fg));
		node->add_theme_color_override("font_color", normal);
		node->add_theme_color_override("font_placeholder_color", normal);
		node->add_theme_color_override("font_selected_color", normal);
	}
	const std::string selected_fg = resolve_color(ctx, font.selected_fg);
	if (!selected_fg.empty()) {
		node->add_theme_color_override(
				"font_selected_color", parse_color(to_gd(selected_fg)));
	}
	if (!fg.empty() || !disabled_fg.empty()) {
		const Color normal = !fg.empty()
				? parse_color(to_gd(fg))
				: node->get_theme_color("font_color");
		if (NovaMnuEdit *edit = Object::cast_to<NovaMnuEdit>(node)) {
			const Color disabled = !disabled_fg.empty()
					? parse_color(to_gd(disabled_fg))
					: edit->get_theme_color("font_uneditable_color");
			edit->set_text_state_colors(normal, disabled);
		} else if (NovaMnuMultilineEdit *edit =
						   Object::cast_to<NovaMnuMultilineEdit>(node)) {
			const Color disabled = !disabled_fg.empty()
					? parse_color(to_gd(disabled_fg))
					: edit->get_theme_color("font_readonly_color");
			edit->set_text_state_colors(normal, disabled);
		}
	}
}

// Single-line text field (type="edit"). Seeds STRING text; a shell drives content
// at runtime. Inert + non-editable in edit_mode.
Control *build_edit(MnuBuildContext &ctx, const mnu::Window &w, const mnu::Font &font) {
	NovaMnuEdit *edit = memnew(NovaMnuEdit);
	edit->set_menu(ctx.owner);
	edit->set_edit_mode(ctx.edit_mode);

	edit->set_sounds(make_widget_sounds(w.sounds));
	edit->set_numeric_constraints(w.number, w.has_minval, w.minval,
			w.has_maxval, w.maxval);
	if (w.has_maxchar) {
		edit->set_max_length(MAX(w.maxchar, 0));
	}
	edit->set_secret(w.password);

	apply_input_font(ctx, edit, font);

	// Colour background -> StyleBoxFlat (LineEdit can't shell a render-behind child).
	if (has_color(w.appearances)) {
		bool found = false;
		const Color bg = get_appearance_color(ctx, w.appearances, "color", "default", found);
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
	if (!w.hotkeys.empty()) {
		edit->set_hotkey(to_gd(w.hotkeys.front().value), w.hotkeys.front().virtual_key);
	}

	edit->set_runtime_enabled(!w.disabled);
	if ((ctx.edit_mode || w.disabled) && text.is_empty()) {
		edit->set_placeholder("Edit");
	}
	return edit;
}

// RADIOEDIT is a composite in the retail client: it owns a radio child and an
// edit child, initially presenting the radio. Selecting an already-selected
// radio swaps to the focused editor; focus loss copies the edited text back.
// [orig: sub_65D210 @ 0x65d210; RadioEditWnd_handle_event @ 0x65d540]
Control *build_radio_edit(MnuBuildContext &ctx, const mnu::Window &w,
		const mnu::Font &font, ButtonGroupMap &groups) {
	NovaMnuEdit *edit = Object::cast_to<NovaMnuEdit>(build_edit(ctx, w, font));
	ERR_FAIL_NULL_V(edit, nullptr);

	TextureButton *radio = memnew(TextureButton);
	radio->set_name("Radio");
	radio->set_anchors_preset(Control::PRESET_FULL_RECT);
	radio->set_ignore_texture_size(true);
	radio->set_stretch_mode(TextureButton::STRETCH_SCALE);
	radio->set_focus_mode(Control::FOCUS_ALL);
	radio->set_toggle_mode(true);
	radio->set_pressed(w.checked);

	const Ref<Texture2D> tex_normal = get_texture(ctx, w.appearances, "default");
	const Ref<Texture2D> tex_hover = get_texture(ctx, w.appearances, "mouseover");
	const Ref<Texture2D> tex_pressed = get_texture(ctx, w.appearances, "selected");
	const Ref<Texture2D> tex_disabled = get_texture(ctx, w.appearances, "disabled");
	if (tex_normal.is_valid()) {
		radio->set_texture_normal(tex_normal);
	}
	if (tex_hover.is_valid()) {
		radio->set_texture_hover(tex_hover);
	}
	if (tex_pressed.is_valid()) {
		radio->set_texture_pressed(tex_pressed);
	}
	if (tex_disabled.is_valid()) {
		radio->set_texture_disabled(tex_disabled);
	}
	if (w.has_group && w.group > 0) {
		auto it = groups.find(w.group);
		if (it == groups.end()) {
			Ref<ButtonGroup> bg;
			bg.instantiate();
			groups[w.group] = bg;
			it = groups.find(w.group);
		}
		radio->set_button_group(it->second);
	}
	if (ctx.inert() || w.disabled) {
		radio->set_disabled(true);
	}

	NovaMnuLabel *label = memnew(NovaMnuLabel);
	label->set_name("Label");
	label->set_anchors_preset(Control::PRESET_FULL_RECT);
	label->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	apply_label_font(ctx, label, font);
	if (w.string_data.present && iequals(w.string_data.type, "id")) {
		label->set_string_id(to_gd(w.string_data.value));
	}
	const String justify = w.string_data.present
			? to_gd(w.string_data.justify).to_upper()
			: String();
	label->set_horizontal_alignment(justify == "LEFT" ? HORIZONTAL_ALIGNMENT_LEFT :
			justify == "RIGHT" ? HORIZONTAL_ALIGNMENT_RIGHT :
									 HORIZONTAL_ALIGNMENT_CENTER);
	const String vjustify = w.string_data.present
			? to_gd(w.string_data.vjustify).to_upper()
			: String();
	label->set_vertical_alignment(vjustify == "TOP" ? VERTICAL_ALIGNMENT_TOP :
			vjustify == "BOTTOM" ? VERTICAL_ALIGNMENT_BOTTOM :
								   VERTICAL_ALIGNMENT_CENTER);
	label->set_text(resolve_text(ctx, w.string_data));
	radio->add_child(label);
	edit->add_child(radio);
	edit->set_radio_parts(radio, label);
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

	edit->set_readonly(w.readonly); // shell can flip at runtime
	edit->set_runtime_enabled(!w.disabled);
	const MnuScrollbarStyle scrollbar_style =
			make_scrollbar_style(ctx, w.table_data.scrollbar);
	if (NovaMnuScroll *scrollbar = make_authored_scroll(ctx, scrollbar_style)) {
		edit->add_child(scrollbar);
		edit->set_authored_scrollbar(scrollbar);
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

// Resolve one ITEM to its visual: text/id (string), image (the element-text filename
// as a texture), or color (the element-text hex as an opaque swatch). Mirrors the
// original item parse + draw: type IMAGE loads the text as a texture, type COLOR reads
// the text as a base-16 RRGGBB value forced opaque [orig: CUISpinList_ParseXMLDefinition
// @ 0x64bd10 (wcstoul base 16) + CSpinListWnd_Render @ 0x64b220 (color | 0xFF000000)].
MnuItemVisual resolve_item(MnuBuildContext &ctx, const mnu::Item &item) {
	MnuItemVisual v;
	v.text = resolve_item_text(ctx, item);
	v.value = to_gd(item.value); // the `value=` attribute (semantic value), kept beside the display text
	if (iequals(item.type, "image")) {
		Ref<Texture2D> tex = resolve_texture(ctx, item.text);
		if (tex.is_valid()) {
			v.kind = MnuItemVisual::IMAGE;
			v.texture = tex;
		}
		// An unresolved image falls back to TEXT (the filename), as elsewhere.
	} else if (iequals(item.type, "color")) {
		const std::string hex = resolve_color(ctx, item.text);
		if (!hex.empty()) {
			v.kind = MnuItemVisual::COLOR;
			v.color = parse_color(to_gd(hex));
			v.color.a = 1.0f; // the original forces full alpha (| 0xFF000000)
		}
	}
	return v;
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
	MnuItemListTextPalette palette;
	palette.normal = list->get_theme_color("font_color");
	palette.hovered = list->get_theme_color("font_hovered_color");
	palette.hovered_selected =
			list->get_theme_color("font_hovered_selected_color");
	palette.selected = list->get_theme_color("font_selected_color");
	if (!fg.empty()) {
		palette.normal = parse_color(to_gd(fg));
		list->add_theme_color_override("font_color", palette.normal);
	}
	palette.disabled = palette.normal;
	palette.disabled.a *= 0.5f;

	const std::string hovered_fg = resolve_color(ctx, font.mouseover_fg);
	if (!hovered_fg.empty()) {
		palette.hovered = parse_color(to_gd(hovered_fg));
		list->add_theme_color_override("font_hovered_color", palette.hovered);
	}
	const std::string selected_fg = resolve_color(ctx, font.selected_fg);
	if (!selected_fg.empty()) {
		palette.selected = parse_color(to_gd(selected_fg));
		list->add_theme_color_override("font_selected_color", palette.selected);
		// MNU has no distinct hovered+selected slot. Keep the selected
		// foreground while the selected row is hovered.
		palette.hovered_selected = palette.selected;
		list->add_theme_color_override(
				"font_hovered_selected_color", palette.hovered_selected);
	}
	const std::string disabled_fg = resolve_color(ctx, font.disabled_fg);
	if (!disabled_fg.empty()) {
		palette.disabled = parse_color(to_gd(disabled_fg));
		palette.has_disabled = true;
	}
	if (NovaMnuList *mnu_list = Object::cast_to<NovaMnuList>(list)) {
		mnu_list->set_item_text_palette(palette);
		mnu_list->set_runtime_enabled(!w.disabled);
	} else if (NovaMnuMulti *mnu_multi = Object::cast_to<NovaMnuMulti>(list)) {
		mnu_multi->set_item_text_palette(palette);
		mnu_multi->set_runtime_enabled(!w.disabled);
	}
	const std::string sel = w.items.present
			? resolve_color(ctx, w.items.selection_color)
			: std::string();
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
	if (w.items.present) {
		for (const auto &it : w.items.items) {
			list->add_item(resolve_item_text(ctx, it));
		}
	}
	if (ctx.edit_mode && (!w.items.present || w.items.items.empty())) {
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
	list->set_sounds(make_widget_sounds(w.sounds));
	apply_list_theme(ctx, list, w, font);
	if (w.items.present) {
		const String justify = to_gd(w.items.justify).to_upper();
		const String vjustify = to_gd(w.items.vjustify).to_upper();
		list->set_item_alignment(
				justify == "CENTER" ? HORIZONTAL_ALIGNMENT_CENTER :
				justify == "RIGHT" ? HORIZONTAL_ALIGNMENT_RIGHT :
										 HORIZONTAL_ALIGNMENT_LEFT,
				vjustify == "TOP" ? VERTICAL_ALIGNMENT_TOP :
				vjustify == "BOTTOM" ? VERTICAL_ALIGNMENT_BOTTOM :
									   VERTICAL_ALIGNMENT_CENTER);
	}
	seed_list_items(ctx, list, w);
	if (w.table_data.has_min_item_height) {
		list->set_min_item_height(w.table_data.min_item_height);
	}
	const MnuScrollbarStyle scrollbar_style =
			make_scrollbar_style(ctx, w.table_data.scrollbar);
	if (NovaMnuScroll *scrollbar = make_authored_scroll(ctx, scrollbar_style)) {
		list->add_child(scrollbar);
		list->set_authored_scrollbar(scrollbar);
	}
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
	list->set_sounds(make_widget_sounds(w.sounds));
	apply_list_theme(ctx, list, w, font);
	if (w.items.present) {
		const String justify = to_gd(w.items.justify).to_upper();
		const String vjustify = to_gd(w.items.vjustify).to_upper();
		list->set_item_alignment(
				justify == "CENTER" ? HORIZONTAL_ALIGNMENT_CENTER :
				justify == "RIGHT" ? HORIZONTAL_ALIGNMENT_RIGHT :
										 HORIZONTAL_ALIGNMENT_LEFT,
				vjustify == "TOP" ? VERTICAL_ALIGNMENT_TOP :
				vjustify == "BOTTOM" ? VERTICAL_ALIGNMENT_BOTTOM :
									   VERTICAL_ALIGNMENT_CENTER);
	}
	seed_list_items(ctx, list, w);
	if (w.table_data.has_min_item_height) {
		list->set_min_item_height(w.table_data.min_item_height);
	}
	const MnuScrollbarStyle scrollbar_style =
			make_scrollbar_style(ctx, w.table_data.scrollbar);
	if (NovaMnuScroll *scrollbar = make_authored_scroll(ctx, scrollbar_style)) {
		list->add_child(scrollbar);
		list->set_authored_scrollbar(scrollbar);
	}
	if (ctx.edit_mode) {
		list->set_focus_mode(Control::FOCUS_NONE);
	}
	return list;
}

// Build one SpinUp/SpinDown button from a parsed SpinButton. The buttons carry no
// nav actions: they only drive the parent spinlist.
//
// SPINUP/SPINDOWN POSITION is parent-relative to the spinlist: the original parses
// each into a child window and accumulates ancestor offsets at draw, so e.g. a right
// arrow at LEFT=56 sits just right of a 45px-wide value box, a left arrow at LEFT=-27
// just left of it [orig: CSpinListWnd_CreateUpDownChildren @ 0x64b8b0 ->
// CWnd_AccumulateAncestorOffset @ 0x6465e0]. The buttons are Godot children of the
// spinlist, so the authored coords are used directly (the prior code subtracted the
// spinlist origin, throwing the arrows far off and producing the doubled/misplaced
// look). A missing far edge sizes from the appearance texture (the three-stage POSITION
// fallback), not a fixed default.
void add_spin_button(MnuBuildContext &ctx, Control *spin, const mnu::Window &w,
		const mnu::SpinButton &sb, const char *name) {
	(void)w;
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
	if (ctx.inert() || w.disabled) {
		btn->set_disabled(true);
	}
	if (sb.position.has_left || sb.position.has_top) {
		const int x = sb.position.has_left ? sb.position.left : 0;
		const int y = sb.position.has_top ? sb.position.top : 0;
		int ext_w = 0;
		int ext_h = 0;
		measure_appearance_extents(ctx, sb.appearances, ext_w, ext_h);
		int rw = sb.position.has_right ? (sb.position.right - sb.position.left)
									   : (ext_w > 0 ? ext_w : 16);
		int rh = sb.position.has_bottom ? (sb.position.bottom - sb.position.top)
										: (ext_h > 0 ? ext_h : 12);
		btn->set_position(Vector2(x, y));
		btn->set_size(Vector2(MAX(rw, 0), MAX(rh, 0)));
	}
	spin->add_child(btn);
}

// Spinner (type="spinlist"): a value cell (text / image / color swatch) cycled by
// SpinUp/SpinDown children [orig: CSpinListWnd_Render @ 0x64b220].
Control *build_spinlist(MnuBuildContext &ctx, const mnu::Window &w, const mnu::Font &font) {
	NovaMnuSpinList *spin = memnew(NovaMnuSpinList);
	spin->set_menu(ctx.owner);
	spin->set_edit_mode(ctx.edit_mode);
	spin->set_sounds(make_widget_sounds(w.sounds));

	// The value cell shells a text label, an image preview, or a color swatch; the
	// spinlist shows the right one per selected item.
	Control *value_mount = memnew(Control);
	value_mount->set_name("Value");
	value_mount->set_anchors_preset(Control::PRESET_FULL_RECT);
	value_mount->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	const String justify = w.items.present
			? to_gd(w.items.justify).to_upper()
			: String();
	HorizontalAlignment halign = HORIZONTAL_ALIGNMENT_CENTER; // spinlist default is centered
	if (justify == "LEFT") {
		halign = HORIZONTAL_ALIGNMENT_LEFT;
	} else if (justify == "RIGHT") {
		halign = HORIZONTAL_ALIGNMENT_RIGHT;
	}
	mnu_build_item_cell(value_mount, halign, make_label_settings(ctx, font));
	spin->add_child(value_mount);

	std::vector<MnuItemVisual> visuals;
	if (w.items.present) {
		for (const auto &it : w.items.items) {
			visuals.push_back(resolve_item(ctx, it));
		}
	}
	if (ctx.edit_mode && visuals.empty()) {
		MnuItemVisual placeholder;
		placeholder.text = "--"; // empty-value placeholder for the WYSIWYG canvas
		visuals.push_back(placeholder);
	}
	spin->set_item_visuals(visuals);

	add_spin_button(ctx, spin, w, w.spinup, "SpinUp");
	add_spin_button(ctx, spin, w, w.spindown, "SpinDown");
	return spin;
}

// Dropdown (type="combo"). Closed TextureButton + selected-text label; the LIST_BOX
// supplies the popup styling and any seed options (a shell can repopulate at runtime).
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
	combo->set_sounds(make_widget_sounds(w.sounds));

	// Closed-state label.
	NovaMnuLabel *lbl = memnew(NovaMnuLabel);
	lbl->set_name("SelectedText");
	lbl->set_anchors_preset(Control::PRESET_FULL_RECT);
	lbl->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	apply_label_font(ctx, lbl, font);
	const String closed_j = w.string_data.present
			? to_gd(w.string_data.justify).to_upper()
			: String();
	HorizontalAlignment closed_halign = HORIZONTAL_ALIGNMENT_CENTER;
	if (closed_j == "LEFT") {
		closed_halign = HORIZONTAL_ALIGNMENT_LEFT;
	} else if (closed_j == "RIGHT") {
		closed_halign = HORIZONTAL_ALIGNMENT_RIGHT;
	}
	lbl->set_horizontal_alignment(closed_halign);
	const String closed_vj = w.string_data.present
			? to_gd(w.string_data.vjustify).to_upper()
			: String();
	lbl->set_vertical_alignment(closed_vj == "TOP" ? VERTICAL_ALIGNMENT_TOP :
			closed_vj == "BOTTOM" ? VERTICAL_ALIGNMENT_BOTTOM :
									VERTICAL_ALIGNMENT_CENTER);
	combo->add_child(lbl);

	// Top-level ITEMS and LIST_BOX/ITEMS are independent authored containers.
	// An authored nested container wins even when it is intentionally empty;
	// otherwise fall back to an authored top-level container.
	const mnu::Items *active_items = nullptr;
	if (w.list_box.present && w.list_box.items.present) {
		active_items = &w.list_box.items;
	} else if (w.items.present) {
		active_items = &w.items;
	}
	const std::string row_justify = active_items && !active_items->justify.empty()
			? active_items->justify
			: (w.list_box.present && w.list_box.string_data.present
							? w.list_box.string_data.justify
							: std::string());
	const String row_j = to_gd(row_justify).to_upper();
	HorizontalAlignment row_halign = HORIZONTAL_ALIGNMENT_LEFT;
	if (row_j == "CENTER") {
		row_halign = HORIZONTAL_ALIGNMENT_CENTER;
	} else if (row_j == "RIGHT") {
		row_halign = HORIZONTAL_ALIGNMENT_RIGHT;
	}
	combo->set_item_alignment((int)row_halign);
	const std::string row_vjustify = active_items && !active_items->vjustify.empty()
			? active_items->vjustify
			: (w.list_box.present && w.list_box.string_data.present
							? w.list_box.string_data.vjustify
							: std::string());
	const String row_vj = to_gd(row_vjustify).to_upper();
	combo->set_item_vertical_alignment(row_vj == "TOP" ? VERTICAL_ALIGNMENT_TOP :
			row_vj == "BOTTOM" ? VERTICAL_ALIGNMENT_BOTTOM :
								 VERTICAL_ALIGNMENT_CENTER);
	if (w.list_box.present && w.list_box.string_data.present &&
			w.list_box.string_data.has_edge) {
		combo->set_item_edge(w.list_box.string_data.edge);
	}

	if (active_items != nullptr) {
		for (const auto &it : active_items->items) {
			combo->add_item(resolve_item_text(ctx, it), to_gd(it.value));
		}
	}

	// Popup styling from the LIST_BOX.
	if (w.list_box.present) {
		bool found = false;
		const Color bgc =
				get_appearance_color(ctx, w.list_box.appearances, "color", "default", found);
		if (found) {
			combo->set_popup_bg_color(bgc);
		}
		Ref<Texture2D> bgt = get_texture(ctx, w.list_box.appearances, "default");
		if (bgt.is_valid()) {
			combo->set_popup_bg_texture(bgt);
		}
		bool ofound = false;
		const Color oc =
				get_appearance_color(ctx, w.list_box.appearances, "outline", "default", ofound);
		if (ofound) {
			combo->set_popup_outline_color(oc);
		}
	}
	const std::string sel_src =
			active_items != nullptr ? active_items->selection_color : std::string();
	const std::string sel = resolve_color(ctx, sel_src);
	if (!sel.empty()) {
		combo->set_selection_color(parse_color(to_gd(sel)));
	}
	if (w.list_box.present && w.list_box.has_min_item_height &&
			w.list_box.min_item_height > 0) {
		combo->set_min_item_height(w.list_box.min_item_height);
	}
	combo->set_scrollbar_style(w.list_box.present
					? make_scrollbar_style(ctx, w.list_box.scrollbar,
							  w.list_box.has_sb_edge_pad ? w.list_box.sb_edge_pad : 0)
					: MnuScrollbarStyle());
	// The authored <LIST_BOX> POSITION is the dropdown's combo-relative rect (design
	// space). The original opens the embedded CListWnd at exactly this rect; passing
	// it through lets below/beside/upward dropdowns land where authored instead of a
	// recomputed below-combo box. [orig: CComboWnd @ 0x65be40; CListWnd rect this+13
	// from LIST_BOX POSITION] (docs/mnu/menu-re.md D-MNU-7).
	const mnu::Position &lbp = w.list_box.position;
	if (w.list_box.present && lbp.width() > 0 && lbp.height() > 0) {
		combo->set_popup_rect(Rect2(lbp.left, lbp.top, lbp.width(), lbp.height()));
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

	if (ctx.inert() || w.disabled) {
		combo->set_disabled(true);
	}
	if (combo->get_item_count() > 0) {
		combo->select_silent(0);
	}
	return combo;
}

// Themed scrollbar / slider (type="scroll"). Resolves the track / shuttle / arrow
// art and orientation; the value range is bound by a shell at runtime.
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
	scroll->set_sounds(make_widget_sounds(w.sounds));
	return scroll;
}

// Table view (type="table"). Builds the column template + header cells, the clipped
// viewport over a shell-populated rows container, and an embedded NovaMnuScroll. Rows
// are added at runtime; the SUBST elements resolve to value->image cells.
Control *build_table(MnuBuildContext &ctx, const mnu::Window &w, const mnu::Font &font) {
	NovaMnuTable *table = memnew(NovaMnuTable);
	table->set_menu(ctx.owner);
	table->set_edit_mode(ctx.edit_mode);
	const mnu::TableData &td = w.table_data;

	const int ncols = td.column.has_count
			? MAX(td.column.count, 0)
			: static_cast<int>(td.column.headers.size());
	const int rowh = td.has_min_item_height && td.min_item_height > 0
			? td.min_item_height
			: 16;
	table->set_row_height(rowh);
	const int column_spacing = td.column.has_spacing ? td.column.spacing : 0;
	table->set_column_spacing(column_spacing);
	table->set_multiselect(w.items.present && td.multiselect);

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
			if ((h.has_column ? h.column : 0) == col) {
				return &h;
			}
		}
		return nullptr;
	};
	auto find_body = [&](int col) -> const mnu::TableBody * {
		for (const auto &b : td.column.bodies) {
			if ((b.has_column ? b.column : 0) == col) {
				return &b;
			}
		}
		return nullptr;
	};
	auto to_halign = [](const std::string &j, HorizontalAlignment fallback) -> HorizontalAlignment {
		const String u = to_gd(j).to_upper();
		if (u == "CENTER") {
			return HORIZONTAL_ALIGNMENT_CENTER;
		}
		if (u == "RIGHT") {
			return HORIZONTAL_ALIGNMENT_RIGHT;
		}
		if (u == "LEFT") {
			return HORIZONTAL_ALIGNMENT_LEFT;
		}
		return fallback;
	};
	auto to_valign = [](const std::string &j, VerticalAlignment fallback) -> VerticalAlignment {
		const String u = to_gd(j).to_upper();
		if (u == "TOP") {
			return VERTICAL_ALIGNMENT_TOP;
		}
		if (u == "BOTTOM") {
			return VERTICAL_ALIGNMENT_BOTTOM;
		}
		if (u == "CENTER") {
			return VERTICAL_ALIGNMENT_CENTER;
		}
		return fallback;
	};

	// Column defs drive body alignment and rendering policy. BITMAP_FLAGS remains a
	// preservation-only field: NovaResourceRoot imports decoded textures, so applying
	// legacy DirectDraw flags here without a proven mapping would invent semantics
	// (ADR 0002). BITMAP_DRAW/SCALE_BITMAP/CUSTOM_DRAW have direct runtime equivalents.
	for (int c = 0; c < ncols; ++c) {
		const mnu::TableHeader *h = find_header(c);
		const mnu::TableBody *b = find_body(c);
		const int width = h && h->has_width && h->width > 0 ? h->width : 80;
		const HorizontalAlignment header_align =
				h ? to_halign(h->justify, HORIZONTAL_ALIGNMENT_LEFT) : HORIZONTAL_ALIGNMENT_LEFT;
		const HorizontalAlignment body_align = b ? to_halign(b->justify, header_align) : header_align;
		const VerticalAlignment header_valign =
				h ? to_valign(h->vjustify, VERTICAL_ALIGNMENT_CENTER) : VERTICAL_ALIGNMENT_CENTER;
		const VerticalAlignment body_valign =
				b ? to_valign(b->vjustify, header_valign) : header_valign;
		table->add_column(width, (int)body_align, b && b->bitmap_draw, (int)body_valign,
				b && b->scale_bitmap, b && b->custom_draw);
	}

	// Colours from the ITEMS outline / selection.
	bool has_outline = false;
	Color outline(0.2f, 0.25f, 0.35f, 1.0f);
	{
		const std::string oc =
				w.items.present ? resolve_color(ctx, td.outline_color) : std::string();
		if (!oc.empty()) {
			has_outline = true;
			outline = parse_color(to_gd(oc));
		}
	}
	Color selection(0.2f, 0.4f, 0.8f, 1.0f);
	{
		const std::string sc =
				w.items.present ? resolve_color(ctx, td.selection_color) : std::string();
		if (!sc.empty()) {
			selection = parse_color(to_gd(sc));
		}
	}
	table->set_colors(outline, has_outline, selection);

	// Value->image substitutions.
	for (const auto &s : td.column.substitutions) {
		if (s.is_file && !s.file.empty()) {
			table->add_substitution(s.has_column ? s.column : 0, to_gd(s.value),
					resolve_texture(ctx, s.file));
		}
	}

	// Header row + cells (sortable headers are Buttons that drive header_clicked).
	Control *header = memnew(Control);
	header->set_name("HeaderRow");
	int hx = 0;
	for (int c = 0; c < ncols; ++c) {
		const mnu::TableHeader *h = find_header(c);
		const int width = h && h->has_width && h->width > 0 ? h->width : 80;
		// Header text resolves type="id" through the RTXT table (the old code drew the
		// raw id) [orig: type="id" branch @ 0x64344a -> CUIStringTable_LookupString
		// @ 0x6434df]. ctx.text is already in scope.
		String text;
		if (h != nullptr) {
			mnu::String header_string;
			header_string.present = true;
			header_string.type = h->type;
			header_string.justify = h->justify;
			header_string.vjustify = h->vjustify;
			header_string.value = h->text;
			text = resolve_text(ctx, header_string);
		}
		const HorizontalAlignment header_align =
				h ? to_halign(h->justify, HORIZONTAL_ALIGNMENT_CENTER) : HORIZONTAL_ALIGNMENT_CENTER;
		const VerticalAlignment header_valign =
				h ? to_valign(h->vjustify, VERTICAL_ALIGNMENT_CENTER) : VERTICAL_ALIGNMENT_CENTER;
		const bool sortable = h && !h->sort.empty();
		if (sortable) {
			Button *hb = memnew(Button);
			hb->set_name(String("Header") + String::num_int64(c));
			hb->set_text(text);
			hb->set_text_alignment(header_align);
			hb->set_flat(true);
			hb->set_position(Vector2(hx, 0));
			hb->set_size(Vector2(width, rowh));
			// Button supplies sort input while a child Label supplies the HEADER's
			// vertical alignment, which BaseButton itself does not expose.
			const Color transparent(1, 1, 1, 0);
			hb->add_theme_color_override("font_color", transparent);
			hb->add_theme_color_override("font_hover_color", transparent);
			hb->add_theme_color_override("font_pressed_color", transparent);
			hb->add_theme_color_override("font_hover_pressed_color", transparent);
			hb->add_theme_color_override("font_focus_color", transparent);
			hb->add_theme_color_override("font_disabled_color", transparent);
			hb->add_theme_color_override("font_outline_color", transparent);
			NovaMnuLabel *header_text = memnew(NovaMnuLabel);
			header_text->set_name("HeaderText");
			header_text->set_text(text);
			header_text->set_anchors_preset(Control::PRESET_FULL_RECT);
			header_text->set_horizontal_alignment(header_align);
			header_text->set_vertical_alignment(header_valign);
			header_text->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
			if (cell_font.is_valid()) {
				header_text->add_theme_font_override("font", cell_font);
			}
			if (cell_font_size > 0) {
				header_text->add_theme_font_size_override("font_size", cell_font_size);
			}
			if (has_cell_color) {
				header_text->add_theme_color_override("font_color", cell_color);
			}
			hb->add_child(header_text);
			hb->set_disabled(ctx.inert() || w.disabled);
			hb->connect("pressed", callable_mp(table, &NovaMnuTable::header_clicked).bind(c));
			header->add_child(hb);
		} else {
			NovaMnuLabel *hl = memnew(NovaMnuLabel);
			hl->set_name(String("Header") + String::num_int64(c));
			hl->set_text(text);
			hl->set_position(Vector2(hx, 0));
			hl->set_size(Vector2(width, rowh));
			hl->set_horizontal_alignment(header_align);
			hl->set_vertical_alignment(header_valign);
			hl->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
			if (cell_font.is_valid()) {
				hl->add_theme_font_override("font", cell_font);
			}
			if (cell_font_size > 0) {
				hl->add_theme_font_size_override("font_size", cell_font_size);
			}
			if (has_cell_color) {
				hl->add_theme_color_override("font_color", cell_color);
			}
			header->add_child(hl);
		}
		hx += width + column_spacing;
	}
	// Header underline (the ITEMS %TRIM_COLOR% outline read as a header rule).
	if (has_outline) {
		ColorRect *rule = memnew(ColorRect);
		rule->set_name("HeaderRule");
		rule->set_color(outline);
		rule->set_anchors_preset(Control::PRESET_BOTTOM_WIDE);
		rule->set_offset(SIDE_TOP, -1);
		rule->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
		header->add_child(rule);
	}
	table->add_child(header);

	// Clipped viewport over a shell-populated rows container.
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
	const MnuScrollbarStyle table_scrollbar_style =
			make_scrollbar_style(ctx, td.scrollbar);
	NovaMnuScroll *sb = make_authored_scroll(ctx, table_scrollbar_style);
	if (sb != nullptr) {
		// Honor the authored <SCROLLBAR><POSITION> (parent-relative to the table)
		// instead of a hardcoded 16px right strip [orig: table SCROLLBAR delegate
		// @ 0x643b22]. Width from the art when the rect is degenerate.
		if (table_scrollbar_style.has_rect) {
			table->set_scrollbar_rect(table_scrollbar_style.rect);
		}
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
		const Color c = get_appearance_color(ctx, w.appearances, "color", "default", found);
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

// Tactical map (type="map"): styled background; a shell supplies the map + markers.
Control *build_map(MnuBuildContext &ctx, const mnu::Window &w, const mnu::Font &font) {
	NovaMnuMap *map = memnew(NovaMnuMap);
	map->set_menu(ctx.owner);
	map->set_edit_mode(ctx.edit_mode);
	add_view_background(ctx, map, w, font, "Map", true);
	return map;
}

// Campaign globe (type="globe"): styled background; a shell supplies the globe image.
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

// Scrolling credits (type="marquee" / "marquee_wnd"). A marquee_wnd DATASOURCE is a
// CBIN-encrypted credits config (ENV scroll settings + TEXT entries with ~C/~F/~I/~J/<CR>),
// not plain text — reading it as a string surfaced the "CBIN" magic instead of credits.
// A CBIN datasource is routed to the dedicated scroller; a plain-text datasource (e.g.
// credits.txt) falls through to the simple marquee [orig: marquee_load_credits_from_ini
// @ 0x65c5a0]. A shell can repush content at runtime.
Control *build_marquee(MnuBuildContext &ctx, const mnu::Window &w, const mnu::Font &font) {
	if (!w.datasource.empty() && ctx.root != nullptr) {
		const PackedByteArray bytes = ctx.root->read_file(to_gd(w.datasource));
		Ref<CbinCreditsResource> credits = CbinCreditsResource::from_cbin_bytes(bytes);
		if (credits.is_valid()) {
			NovaCreditsPlayer *player = memnew(NovaCreditsPlayer);
			player->set_name("Credits");
			player->set_anchors_preset(Control::PRESET_FULL_RECT);
			player->set_clip_contents(true);
			player->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
			player->set_credits_resource(credits);
			if (!ctx.edit_mode) {
				player->set_autoplay(true); // roll the credits at runtime
			}
			return player;
		}
		if (bytes.is_empty()) {
			ctx.unresolved_assets.insert(w.datasource);
		}
	}

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
	const String j = w.string_data.present
			? to_gd(w.string_data.justify).to_upper()
			: String();
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
	go->set_edit_mode(ctx.inert());
	for (const auto &act : w.actions) {
		go->add_action(make_action(act));
	}
	if (!w.hotkeys.empty()) {
		go->set_hotkey(to_gd(w.hotkeys.front().value), w.hotkeys.front().virtual_key);
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
		case mnu::WindowType::RadioEdit:
			node = build_radio_edit(ctx, w, font, groups);
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
		case mnu::WindowType::GlbTable:
		case mnu::WindowType::LanList:
		case mnu::WindowType::Gopher:
			node = build_shell_owned_placeholder(ctx, w, font);
			break;
		default:
			node = build_placeholder(ctx, w, font);
			break;
	}

	if (node == nullptr) {
		node = memnew(Control);
	}

	node->set_name(names.get_unique(to_gd(w.name)));
	// WINDOW ENABLE/DISABLE mutates this node's local state. Descendants retain
	// their own authored state, so re-enabling a parent cannot accidentally wake
	// a child authored with DISABLE.
	node->set_meta("mnu_local_enabled", !w.disabled);
	// Tag with the stable document id so the editor can map this Control back to its
	// widget (and read its rendered size for widgets whose document rect is sizeless).
	if (widget_id >= 0) {
		node->set_meta("mnu_widget_id", widget_id);
	}
	// Keep every authored accelerator and its VIRTUAL distinction. The menu walks
	// Controls in document order, then each row in authored order; this covers
	// shipped dual bindings such as jo_cmap's VK_ESCAPE + literal "V".
	PackedStringArray virtual_hotkeys;
	PackedStringArray character_hotkeys;
	for (const mnu::Hotkey &hotkey : w.hotkeys) {
		if (hotkey.virtual_key) {
			virtual_hotkeys.push_back(to_gd(hotkey.value).to_upper());
		} else {
			character_hotkeys.push_back(to_gd(hotkey.value));
		}
	}
	if (!virtual_hotkeys.is_empty()) {
		node->set_meta("mnu_virtual_hotkeys", virtual_hotkeys);
	}
	if (!character_hotkeys.is_empty()) {
		node->set_meta("mnu_character_hotkeys", character_hotkeys);
	}
	apply_position(ctx, node, w, font);

	if (w.hidden) {
		node->set_visible(false);
	}
	if (w.disabled) {
		node->set_process_mode(Node::PROCESS_MODE_DISABLED);
	}
	// Author mode makes the whole tree click-through so the canvas owns gestures; the
	// interactive preview leaves buttons clickable (they keep their default STOP).
	if (ctx.inert()) {
		node->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	}

	// Own frame takes priority over inherited for child inheritance.
	const mnu::Frame &frame_to_inherit = has_frame(w.frame) ? w.frame : inherited_frame;
	build_children(ctx, node, w, font, frame_to_inherit, names, groups, widget_id);

	// Outline draws on top of children.
	if ((w.type == mnu::WindowType::Window || w.type == mnu::WindowType::Static ||
				w.type == mnu::WindowType::Label) &&
			has_outline(w.appearances)) {
		add_outline(ctx, node, w.appearances);
	}

	return node;
}

} // namespace

namespace godot {

Control *mnu_build_screen(const mnu::Screen &screen, MnuBuildContext &ctx, int root_window_id) {
	NovaMnuScreen *screen_node = memnew(NovaMnuScreen);
	screen_node->set_name(to_gd(screen.name).is_empty() ? String("Screen") : to_gd(screen.name));
	screen_node->set_screen_name(to_gd(screen.name));
	screen_node->set_music_var(screen.has_music_var ? screen.music_var : 0);
	screen_node->set_cursor_file(to_gd(screen.cursor_file));
	screen_node->set_edit_mode(ctx.edit_mode);
	screen_node->set_anchors_preset(Control::PRESET_FULL_RECT);
	if (ctx.inert()) {
		screen_node->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	} else if (ctx.interactive) {
		// Interactive preview: live buttons consume their own clicks, while view
		// gestures (wheel zoom / middle-drag pan) fall through this screen to the
		// editor canvas behind it.
		screen_node->set_mouse_filter(Control::MOUSE_FILTER_PASS);
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
