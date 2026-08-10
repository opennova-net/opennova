#include "nova_menu_frame.h"

#include "mns_stylesheet.h"
#include "nova_mnu_document.h"
#include "resource_index/nova_resource_root.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/rect2.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <string>

using namespace godot;

namespace {

// Menus are authored in the fixed 800x600 design space
// [orig: CUIScene_SetScreenScale @ 0x639480].
constexpr float kDesignW = 800.0f;
constexpr float kDesignH = 600.0f;

Color argb_to_color(uint32_t argb) {
	return Color(((argb >> 16) & 0xFFu) / 255.0f,
			((argb >> 8) & 0xFFu) / 255.0f, (argb & 0xFFu) / 255.0f,
			((argb >> 24) & 0xFFu) / 255.0f);
}

std::string to_std(const String &s) {
	return std::string(s.utf8().get_data());
}

} // namespace

MenuFrame::MenuFrame() = default;

MenuFrame::~MenuFrame() = default;

void MenuFrame::free_fonts_() {
	fonts_.clear();
	owned_fonts_.clear();
}

void MenuFrame::collect_font_names_(const void *p_window,
		std::vector<String> &r_names) const {
	const mnu::Window &w = *static_cast<const mnu::Window *>(p_window);
	if (!w.font.name.empty()) {
		const String name = String::utf8(w.font.name.c_str());
		bool seen = false;
		for (const String &existing : r_names) {
			if (existing.nocasecmp_to(name) == 0) {
				seen = true;
				break;
			}
		}
		if (!seen) {
			r_names.push_back(name);
		}
	}
	for (const mnu::Window &child : w.children) {
		collect_font_names_(&child, r_names);
	}
}

bool MenuFrame::configure(const Ref<MnuDocument> &p_document,
		const String &p_screen_name, const Ref<ResourceRoot> &p_root,
		const Ref<MnsStyleSheet> &p_style, const Dictionary &p_text_lookup) {
	document_ = p_document;
	root_ = p_root;
	configured_ = false;
	textures_.clear();
	compiler_.clear_registered_fonts();
	free_fonts_();
	state_ = opennova::menu::MenuFrameState{};
	if (document_.is_null()) {
		queue_redraw();
		return false;
	}
	const mnu::Document &doc = document_->get_native();
	const mnu::Screen *screen = p_screen_name.is_empty()
			? doc.first_screen()
			: doc.find_screen(to_std(p_screen_name));
	if (screen == nullptr) {
		queue_redraw();
		return false;
	}

	// Stylesheet vars + the id text table feed the compiler's resolution.
	std::map<std::string, std::string> vars;
	if (p_style.is_valid()) {
		const Dictionary d = p_style->get_variables();
		const Array keys = d.keys();
		for (int64_t i = 0; i < keys.size(); ++i) {
			const String key = keys[i];
			vars[to_std(key)] = to_std(String(d[keys[i]]));
		}
	}
	compiler_.set_style_vars(vars);
	std::map<std::string, std::string> text_table;
	{
		const Array keys = p_text_lookup.keys();
		for (int64_t i = 0; i < keys.size(); ++i) {
			const String key = keys[i];
			text_table[to_std(key)] = to_std(String(p_text_lookup[keys[i]]));
		}
	}
	compiler_.set_text_lookup(text_table);

	// Fonts referenced by the tree, loaded through the VFS and registered
	// before configure() interns them. The first loadable font doubles as the
	// default (an unauthored FONT falls back to it, the shell's existing
	// fallback policy).
	std::vector<String> font_names;
	collect_font_names_(&screen->root_window, font_names);
	std::map<std::string, LoadedFont *> loaded;
	LoadedFont *default_font = nullptr;
	for (const String &name : font_names) {
		if (root_.is_null()) {
			break;
		}
		const PackedByteArray bytes = root_->read_file(name.get_file());
		if (bytes.is_empty()) {
			continue;
		}
		auto owned = std::make_unique<LoadedFont>();
		if (fnt_parse(bytes.ptr(), static_cast<size_t>(bytes.size()),
					&owned->font) != FNT_OK) {
			continue;
		}
		owned->valid = true;
		LoadedFont *font = owned.get();
		owned_fonts_.push_back(std::move(owned));
		font->pages.resize(font->font.num_pages);
		for (uint32_t page = 0;
				page < font->font.num_pages && page < FNT_MAX_PAGES; ++page) {
			const uint8_t *data = fnt_get_page_data_const(&font->font, page);
			if (data == nullptr) {
				continue;
			}
			PackedByteArray page_bytes;
			page_bytes.resize(FNT_TEXTURE_SIZE);
			std::memcpy(page_bytes.ptrw(), data, FNT_TEXTURE_SIZE);
			const Ref<Image> image = Image::create_from_data(FNT_TEXTURE_WIDTH,
					FNT_TEXTURE_HEIGHT, false, Image::FORMAT_RGBA8, page_bytes);
			if (image.is_valid()) {
				font->pages[page] = ImageTexture::create_from_image(image);
			}
		}
		loaded[to_std(name.to_lower())] = font;
		compiler_.register_font(to_std(name), &font->font);
		if (default_font == nullptr) {
			default_font = font;
		}
	}

	compiler_.configure(screen,
			default_font != nullptr ? &default_font->font : nullptr);

	// Resolve every interned texture through the VFS and report its size for
	// the engine-side rect fallbacks.
	const std::vector<std::string> &tex_names = compiler_.texture_names();
	textures_.resize(tex_names.size());
	for (size_t i = 0; i < tex_names.size(); ++i) {
		if (root_.is_null()) {
			break;
		}
		const String name = String::utf8(tex_names[i].c_str());
		const PackedByteArray bytes = root_->read_file(name.get_file());
		if (bytes.is_empty()) {
			continue;
		}
		Ref<Image> image;
		image.instantiate();
		if (image->load_tga_from_buffer(bytes) != OK) {
			continue;
		}
		const Ref<Texture2D> tex = ImageTexture::create_from_image(image);
		textures_[i] = tex;
		if (tex.is_valid()) {
			compiler_.set_texture_size(static_cast<int32_t>(i),
					tex->get_width(), tex->get_height());
		}
	}

	// Map the compiler's font slots to the loaded fonts (slot 0 = default).
	const std::vector<std::string> &slot_names = compiler_.font_names();
	fonts_.resize(slot_names.size(), nullptr);
	for (size_t i = 0; i < slot_names.size(); ++i) {
		if (slot_names[i].empty()) {
			fonts_[i] = default_font;
			continue;
		}
		std::string key = slot_names[i];
		std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});
		const auto it = loaded.find(key);
		fonts_[i] = it != loaded.end() ? it->second : default_font;
	}

	configured_ = true;
	queue_redraw();
	return true;
}

bool MenuFrame::is_configured() const {
	return configured_;
}

opennova::menu::MenuWidgetState &MenuFrame::widget_(int p_index) {
	for (opennova::menu::MenuWidgetState &ws : state_.widgets) {
		if (ws.index == p_index) {
			return ws;
		}
	}
	opennova::menu::MenuWidgetState ws;
	ws.index = p_index;
	state_.widgets.push_back(ws);
	return state_.widgets.back();
}

void MenuFrame::clear_widget_states() {
	state_.widgets.clear();
	queue_redraw();
}

void MenuFrame::set_widget_shown_override(int p_index, bool p_shown) {
	opennova::menu::MenuWidgetState &ws = widget_(p_index);
	ws.hide = !p_shown;
	ws.show = p_shown;
	queue_redraw();
}

void MenuFrame::clear_widget_shown_override(int p_index) {
	opennova::menu::MenuWidgetState &ws = widget_(p_index);
	ws.hide = false;
	ws.show = false;
	queue_redraw();
}

void MenuFrame::set_widget_disabled(int p_index, bool p_disabled) {
	widget_(p_index).disabled = p_disabled;
	queue_redraw();
}

void MenuFrame::set_widget_hovered(int p_index, bool p_hovered) {
	widget_(p_index).hovered = p_hovered;
	queue_redraw();
}

void MenuFrame::set_widget_pressed(int p_index, bool p_pressed) {
	widget_(p_index).pressed = p_pressed;
	queue_redraw();
}

void MenuFrame::set_widget_checked(int p_index, bool p_checked) {
	opennova::menu::MenuWidgetState &ws = widget_(p_index);
	ws.has_checked = true;
	ws.checked = p_checked;
	queue_redraw();
}

void MenuFrame::set_widget_focused(int p_index, bool p_focused) {
	widget_(p_index).focused = p_focused;
	queue_redraw();
}

void MenuFrame::set_widget_caret(int p_index, int p_caret) {
	widget_(p_index).caret = p_caret;
	queue_redraw();
}

void MenuFrame::set_widget_text(int p_index, const String &p_text) {
	opennova::menu::MenuWidgetState &ws = widget_(p_index);
	ws.has_text = true;
	ws.text = to_std(p_text);
	queue_redraw();
}

void MenuFrame::clear_widget_text(int p_index) {
	opennova::menu::MenuWidgetState &ws = widget_(p_index);
	ws.has_text = false;
	ws.text.clear();
	queue_redraw();
}

void MenuFrame::set_widget_selection(int p_index, int p_selected_item,
		int p_hover_item, int p_scroll_row) {
	opennova::menu::MenuWidgetState &ws = widget_(p_index);
	ws.selected_item = p_selected_item;
	ws.hover_item = p_hover_item;
	ws.scroll_row = p_scroll_row;
	queue_redraw();
}

void MenuFrame::set_widget_popup_open(int p_index, bool p_open) {
	widget_(p_index).popup_open = p_open;
	queue_redraw();
}

void MenuFrame::set_time_ms(int64_t p_ms) {
	state_.time_ms = static_cast<uint32_t>(p_ms);
	queue_redraw();
}

int MenuFrame::process_mouse(const Vector2 &p_position, bool p_button_down) {
	if (!is_configured()) {
		return -1;
	}
	const Vector2 scale = design_scale_();
	const opennova::menu::MenuFrameCompiler::MouseClaim claim =
			compiler_.pump_mouse(state_, p_position.x, p_position.y,
					p_button_down, scale.x, scale.y);
	state_.cursor_x = p_position.x;
	state_.cursor_y = p_position.y;
	queue_redraw();
	return claim.hovered;
}

void MenuFrame::set_cursor_state(bool p_visible, const Vector2 &p_position) {
	state_.cursor_visible = p_visible;
	state_.cursor_x = p_position.x;
	state_.cursor_y = p_position.y;
	queue_redraw();
}

Vector2 MenuFrame::design_scale_() const {
	const Vector2 size = get_size();
	if (size.x > 1.0f && size.y > 1.0f) {
		return Vector2(size.x / kDesignW, size.y / kDesignH);
	}
	return Vector2(1.0f, 1.0f);
}

Dictionary MenuFrame::get_draw_list_stats() {
	Dictionary out;
	int64_t quads = 0;
	int64_t quads_textured = 0;
	int64_t lines = 0;
	int64_t glyphs = 0;
	int64_t widgets = 0;
	if (configured_) {
		const Vector2 scale = design_scale_();
		const opennova::menu::MenuDrawList &list =
				compiler_.compile(state_, scale.x, scale.y);
		quads = static_cast<int64_t>(list.quads.size());
		for (const opennova::menu::MenuQuad &quad : list.quads) {
			if (quad.texture != opennova::menu::kMenuTexNone) {
				++quads_textured;
			}
		}
		lines = static_cast<int64_t>(list.lines.size());
		glyphs = static_cast<int64_t>(list.glyphs.size());
		widgets = list.widgets_drawn;
	}
	out["quads"] = quads;
	out["quads_textured"] = quads_textured;
	out["lines"] = lines;
	out["glyphs"] = glyphs;
	out["widgets_drawn"] = widgets;
	return out;
}

void MenuFrame::_notification(int p_what) {
	if (p_what == NOTIFICATION_RESIZED) {
		queue_redraw();
	}
}

void MenuFrame::_draw() {
	if (!configured_) {
		return;
	}
	const Vector2 scale = design_scale_();
	const opennova::menu::MenuDrawList &list =
			compiler_.compile(state_, scale.x, scale.y);
	// Quads and lines in compiler order; glyphs above them per font run
	// (retail's text draws ride the same walk after each widget's art).
	for (const opennova::menu::MenuQuad &quad : list.quads) {
		const Rect2 rect(quad.x0, quad.y0, quad.x1 - quad.x0,
				quad.y1 - quad.y0);
		const Color color = argb_to_color(quad.color);
		Ref<Texture2D> tex;
		if (quad.texture >= 0 &&
				quad.texture < static_cast<int32_t>(textures_.size())) {
			tex = textures_[static_cast<size_t>(quad.texture)];
		}
		if (tex.is_null()) {
			if (quad.texture == opennova::menu::kMenuTexNone) {
				draw_rect(rect, color, true);
			}
			// An unresolved texture draws nothing [orig: every draw is gated
			// on a successful texture load].
			continue;
		}
		if (quad.tiled) {
			draw_texture_rect(tex, rect, true, color);
		} else if (quad.u0 != 0.0f || quad.v0 != 0.0f || quad.u1 != 1.0f ||
				quad.v1 != 1.0f) {
			const Vector2 tex_size = tex->get_size();
			draw_texture_rect_region(tex, rect,
					Rect2(quad.u0 * tex_size.x, quad.v0 * tex_size.y,
							(quad.u1 - quad.u0) * tex_size.x,
							(quad.v1 - quad.v0) * tex_size.y),
					color);
		} else {
			draw_texture_rect(tex, rect, false, color);
		}
	}
	for (const opennova::menu::MenuLine &line : list.lines) {
		draw_line(Vector2(line.x0, line.y0), Vector2(line.x1, line.y1),
				argb_to_color(line.color), 1.0f);
	}
	// Glyphs: each font run renders over its own font's page textures.
	for (const opennova::menu::MenuDrawList::FontRun &run : list.font_runs) {
		LoadedFont *font = nullptr;
		if (run.font >= 0 && run.font < static_cast<int32_t>(fonts_.size())) {
			font = fonts_[static_cast<size_t>(run.font)];
		}
		if (font == nullptr) {
			continue;
		}
		const int32_t end = run.first + run.count;
		for (int32_t i = run.first;
				i < end && i < static_cast<int32_t>(list.glyphs.size()); ++i) {
			const opennova::hud::GameFontQuad &glyph =
					list.glyphs[static_cast<size_t>(i)];
			if (glyph.page >= font->pages.size()) {
				continue;
			}
			const Ref<Texture2D> page = font->pages[glyph.page];
			if (page.is_null()) {
				continue;
			}
			PackedVector2Array points;
			points.resize(4);
			points.set(0, Vector2(glyph.x_top_left, glyph.y_top));
			points.set(1, Vector2(glyph.x_top_right, glyph.y_top));
			points.set(2, Vector2(glyph.x_bottom_right, glyph.y_bottom));
			points.set(3, Vector2(glyph.x_bottom_left, glyph.y_bottom));
			PackedVector2Array uvs;
			uvs.resize(4);
			uvs.set(0, Vector2(glyph.u0, glyph.v0));
			uvs.set(1, Vector2(glyph.u1, glyph.v0));
			uvs.set(2, Vector2(glyph.u1, glyph.v1));
			uvs.set(3, Vector2(glyph.u0, glyph.v1));
			PackedColorArray colors;
			colors.push_back(argb_to_color(glyph.color));
			draw_polygon(points, colors, uvs, page);
		}
	}
	for (const opennova::hud::GameFontUnderline &underline : list.underlines) {
		draw_line(Vector2(underline.x0, underline.y),
				Vector2(underline.x1, underline.y),
				argb_to_color(underline.color), 1.0f);
	}
}

void MenuFrame::_bind_methods() {
	ClassDB::bind_method(
			D_METHOD("configure", "document", "screen_name", "root", "style",
					"text_lookup"),
			&MenuFrame::configure);
	ClassDB::bind_method(D_METHOD("is_configured"), &MenuFrame::is_configured);
	ClassDB::bind_method(D_METHOD("clear_widget_states"),
			&MenuFrame::clear_widget_states);
	ClassDB::bind_method(
			D_METHOD("set_widget_shown_override", "index", "shown"),
			&MenuFrame::set_widget_shown_override);
	ClassDB::bind_method(D_METHOD("clear_widget_shown_override", "index"),
			&MenuFrame::clear_widget_shown_override);
	ClassDB::bind_method(D_METHOD("set_widget_disabled", "index", "disabled"),
			&MenuFrame::set_widget_disabled);
	ClassDB::bind_method(D_METHOD("set_widget_hovered", "index", "hovered"),
			&MenuFrame::set_widget_hovered);
	ClassDB::bind_method(D_METHOD("set_widget_pressed", "index", "pressed"),
			&MenuFrame::set_widget_pressed);
	ClassDB::bind_method(D_METHOD("set_widget_checked", "index", "checked"),
			&MenuFrame::set_widget_checked);
	ClassDB::bind_method(D_METHOD("set_widget_focused", "index", "focused"),
			&MenuFrame::set_widget_focused);
	ClassDB::bind_method(D_METHOD("set_widget_caret", "index", "caret"),
			&MenuFrame::set_widget_caret);
	ClassDB::bind_method(D_METHOD("set_widget_text", "index", "text"),
			&MenuFrame::set_widget_text);
	ClassDB::bind_method(D_METHOD("clear_widget_text", "index"),
			&MenuFrame::clear_widget_text);
	ClassDB::bind_method(
			D_METHOD("set_widget_selection", "index", "selected_item",
					"hover_item", "scroll_row"),
			&MenuFrame::set_widget_selection);
	ClassDB::bind_method(D_METHOD("set_widget_popup_open", "index", "open"),
			&MenuFrame::set_widget_popup_open);
	ClassDB::bind_method(D_METHOD("set_time_ms", "ms"),
			&MenuFrame::set_time_ms);
	ClassDB::bind_method(D_METHOD("process_mouse", "position", "button_down"),
			&MenuFrame::process_mouse);
	ClassDB::bind_method(D_METHOD("set_cursor_state", "visible", "position"),
			&MenuFrame::set_cursor_state);
	ClassDB::bind_method(D_METHOD("get_draw_list_stats"),
			&MenuFrame::get_draw_list_stats);
}
