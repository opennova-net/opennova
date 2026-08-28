#include "mnu/menu_frame.h"

#include <runtime/menu/options_policy.h>

#include "mnu/mns_stylesheet.h"
#include "mnu/mnu_document.h"
#include "resource_index/resource_root.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
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

// The EDIT_RESULT_* re-exports track menu/menu_edit.h EditKeyResult; pin the
// documented GDScript contract (0 none / 1 changed / 2 commit) so an engine
// enum reorder cannot silently change the bound values.
static_assert(MenuFrame::EDIT_RESULT_NONE == 0);
static_assert(MenuFrame::EDIT_RESULT_CHANGED == 1);
static_assert(MenuFrame::EDIT_RESULT_COMMIT == 2);

namespace {

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

MenuFrame::~MenuFrame() {
	if (overlay_canvas_item_.is_valid()) {
		RenderingServer::get_singleton()->free_rid(overlay_canvas_item_);
	}
}

void MenuFrame::ensure_overlay_canvas_item_() {
	if (overlay_canvas_item_.is_valid()) {
		return;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	overlay_canvas_item_ = rs->canvas_item_create();
	rs->canvas_item_set_parent(overlay_canvas_item_, get_canvas_item());
	// One z above the frame and every sibling child it draws in front of.
	rs->canvas_item_set_z_as_relative_to_parent(overlay_canvas_item_, true);
	rs->canvas_item_set_z_index(overlay_canvas_item_, 1);
}

void MenuFrame::free_fonts_() {
	fonts_.clear();
	owned_fonts_.clear();
}

void MenuFrame::collect_font_names_(const void *p_window,
		std::vector<String> &r_names) const {
	const opennova::mnu::Window &w = *static_cast<const opennova::mnu::Window *>(p_window);
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
	for (const opennova::mnu::Window &child : w.children) {
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
	cursor_slot_ = -1;
	press_claim_ = -1;
	mouse_claim_ = -1;
	unresolved_assets_ = 0;
	if (document_.is_null()) {
		queue_redraw();
		return false;
	}
	const opennova::mnu::Document &doc = document_->get_native();
	const opennova::mnu::Screen *screen = p_screen_name.is_empty()
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
	std::vector<String> raw_font_names;
	collect_font_names_(&screen->root_window, raw_font_names);
	// Authored FONT names are frequently stylesheet variables
	// (%DEF_FONTNAME%); resolve them the way the compiler interns them so the
	// VFS load and the slot mapping key on the SAME resolved name.
	std::vector<String> font_names;
	for (const String &raw : raw_font_names) {
		const String resolved = String::utf8(
				compiler_.resolve_style_var(to_std(raw)).c_str());
		bool seen = false;
		for (const String &existing : font_names) {
			if (existing.nocasecmp_to(resolved) == 0) {
				seen = true;
				break;
			}
		}
		if (!seen) {
			font_names.push_back(resolved);
		}
	}
	std::map<std::string, LoadedFont *> loaded;
	LoadedFont *default_font = nullptr;
	for (const String &name : font_names) {
		if (root_.is_null()) {
			++unresolved_assets_;
			continue;
		}
		const PackedByteArray bytes = root_->read_file(name.get_file());
		if (bytes.is_empty()) {
			++unresolved_assets_;
			continue;
		}
		auto owned = std::make_unique<LoadedFont>();
		if (fnt_parse(bytes.ptr(), static_cast<size_t>(bytes.size()),
					&owned->font) != FNT_OK) {
			++unresolved_assets_;
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
			++unresolved_assets_;
			continue;
		}
		const String name = String::utf8(tex_names[i].c_str());
		const PackedByteArray bytes = root_->read_file(name.get_file());
		if (bytes.is_empty()) {
			++unresolved_assets_;
			continue;
		}
		Ref<Image> image;
		image.instantiate();
		if (image->load_tga_from_buffer(bytes) != OK) {
			++unresolved_assets_;
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

void MenuFrame::set_widget_shown_override(int p_index, bool p_shown) {
	opennova::menu::MenuWidgetState &ws = widget_(p_index);
	ws.hide = !p_shown;
	ws.show = p_shown;
	queue_redraw();
}

void MenuFrame::set_widget_disabled(int p_index, bool p_disabled) {
	widget_(p_index).disabled = p_disabled;
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

void MenuFrame::set_widget_hover_item(int p_index, int p_row) {
	opennova::menu::MenuWidgetState &ws = widget_(p_index);
	if (ws.hover_item == p_row) {
		return;
	}
	ws.hover_item = p_row;
	queue_redraw();
}

int MenuFrame::get_widget_hover_item(int p_index) const {
	for (const opennova::menu::MenuWidgetState &ws : state_.widgets) {
		if (ws.index == p_index) {
			return ws.hover_item;
		}
	}
	return -1;
}

void MenuFrame::set_widget_selection(int p_index, int p_selected_item,
		int p_hover_item, int p_scroll_row) {
	opennova::menu::MenuWidgetState &ws = widget_(p_index);
	ws.selected_item = p_selected_item;
	ws.hover_item = p_hover_item;
	ws.scroll_row = p_scroll_row;
	queue_redraw();
}

void MenuFrame::set_widget_scroll_range(int p_index, int p_minimum,
		int p_maximum, int p_page,
		int p_value) {
	opennova::menu::MenuWidgetState &ws = widget_(p_index);
	if (p_minimum > p_maximum) {
		p_minimum = 0;
		p_maximum = 0;
	}
	ws.has_scroll_range = true;
	ws.scroll_min = p_minimum;
	ws.scroll_max = p_maximum;
	ws.scroll_page = p_page;
	ws.scroll_value = std::clamp(p_value, p_minimum, p_maximum);
	queue_redraw();
}

void MenuFrame::set_widget_popup_open(int p_index, bool p_open) {
	widget_(p_index).popup_open = p_open;
	queue_redraw();
}

void MenuFrame::set_widget_items(int p_index,
		const PackedStringArray &p_items) {
	opennova::menu::MenuWidgetState &ws = widget_(p_index);
	ws.has_items = true;
	ws.items.clear();
	ws.items.reserve(static_cast<size_t>(p_items.size()));
	for (int64_t i = 0; i < p_items.size(); ++i) {
		ws.items.push_back(to_std(p_items[i]));
	}
	queue_redraw();
}

void MenuFrame::set_widget_selected_set(int p_index,
		const PackedInt32Array &p_rows) {
	opennova::menu::MenuWidgetState &ws = widget_(p_index);
	ws.selected_items.clear();
	ws.selected_items.reserve(static_cast<size_t>(p_rows.size()));
	for (int64_t i = 0; i < p_rows.size(); ++i) {
		ws.selected_items.push_back(p_rows[i]);
	}
	queue_redraw();
}

void MenuFrame::set_widget_table_rows(int p_index,
		const TypedArray<PackedStringArray> &p_rows) {
	opennova::menu::MenuWidgetState &ws = widget_(p_index);
	ws.table_rows.clear();
	ws.table_rows.reserve(static_cast<size_t>(p_rows.size()));
	for (int64_t r = 0; r < p_rows.size(); ++r) {
		const PackedStringArray row = p_rows[r];
		std::vector<std::string> cells;
		cells.reserve(static_cast<size_t>(row.size()));
		for (int64_t c = 0; c < row.size(); ++c) {
			cells.push_back(to_std(row[c]));
		}
		ws.table_rows.push_back(std::move(cells));
	}
	queue_redraw();
}

void MenuFrame::set_widget_marquee_lines(int p_index,
		const PackedStringArray &p_lines) {
	opennova::menu::MenuWidgetState &ws = widget_(p_index);
	ws.marquee_lines.clear();
	ws.marquee_lines.reserve(static_cast<size_t>(p_lines.size()));
	for (int64_t i = 0; i < p_lines.size(); ++i) {
		ws.marquee_lines.push_back(to_std(p_lines[i]));
	}
	ws.marquee_reset = true; // fresh content restarts the roll
	queue_redraw();
}

int MenuFrame::widget_count() const {
	return compiler_.widget_count();
}

String MenuFrame::widget_name(int p_index) const {
	return String::utf8(compiler_.widget_name(p_index).c_str());
}

int MenuFrame::widget_kind(int p_index) const {
	return compiler_.widget_kind(p_index);
}

String MenuFrame::widget_authored_text(int p_index) const {
	return String::utf8(compiler_.widget_authored_text(p_index).c_str());
}

bool MenuFrame::is_widget_disabled(int p_index) const {
	return compiler_.widget_disabled(p_index, state_);
}

bool MenuFrame::is_widget_shown(int p_index) const {
	return compiler_.widget_shown(p_index, state_);
}

Rect2 MenuFrame::widget_rect(int p_index) const {
	opennova::mnu::RectEdges rect;
	if (!compiler_.widget_rect(p_index, state_, &rect)) {
		return Rect2();
	}
	return Rect2(static_cast<float>(rect.left), static_cast<float>(rect.top),
			static_cast<float>(rect.right - rect.left),
			static_cast<float>(rect.bottom - rect.top));
}

int MenuFrame::item_count(int p_index) const {
	return compiler_.item_count(p_index, state_);
}

String MenuFrame::get_widget_text(int p_index) const {
	for (const opennova::menu::MenuWidgetState &ws : state_.widgets) {
		if (ws.index == p_index && ws.has_text) {
			return String::utf8(ws.text.c_str());
		}
	}
	return String::utf8(compiler_.widget_authored_text(p_index).c_str());
}

int MenuFrame::get_widget_caret(int p_index) const {
	for (const opennova::menu::MenuWidgetState &ws : state_.widgets) {
		if (ws.index == p_index) {
			return ws.caret;
		}
	}
	return -1;
}

int MenuFrame::hit_test(const Vector2 &p_position) const {
	if (!configured_) {
		return -1;
	}
	const Vector2 scale = design_scale_();
	return compiler_.hit_widget(state_, p_position.x, p_position.y, scale.x,
			scale.y);
}

int MenuFrame::list_row_at(int p_index, const Vector2 &p_position) const {
	if (!configured_) {
		return -1;
	}
	const Vector2 scale = design_scale_();
	return compiler_.list_row_at(p_index, state_, p_position.x, p_position.y,
			scale.x, scale.y);
}

int MenuFrame::list_visible_rows(int p_index) const {
	return compiler_.list_visible_rows(p_index, state_);
}

Rect2 MenuFrame::combo_popup_rect(int p_index) const {
	opennova::mnu::RectEdges rect;
	if (!compiler_.combo_popup_rect(p_index, state_, &rect)) {
		return Rect2();
	}
	return Rect2(static_cast<float>(rect.left), static_cast<float>(rect.top),
			static_cast<float>(rect.right - rect.left),
			static_cast<float>(rect.bottom - rect.top));
}

bool MenuFrame::combo_popup_contains(int p_index,
		const Vector2 &p_position) const {
	if (!configured_) {
		return false;
	}
	const Vector2 scale = design_scale_();
	return compiler_.combo_popup_contains(p_index, state_, p_position.x,
			p_position.y, scale.x, scale.y);
}

int MenuFrame::combo_popup_row_at(int p_index,
		const Vector2 &p_position) const {
	if (!configured_) {
		return -1;
	}
	const Vector2 scale = design_scale_();
	return compiler_.combo_popup_row_at(p_index, state_, p_position.x,
			p_position.y, scale.x, scale.y);
}

int MenuFrame::scroll_hit_at(int p_index, const Vector2 &p_position) const {
	if (!configured_) {
		return 0;
	}
	const Vector2 scale = design_scale_();
	return compiler_.scroll_hit_at(p_index, state_, p_position.x, p_position.y,
			scale.x, scale.y);
}

int MenuFrame::spin_arrow_at(int p_index, const Vector2 &p_position) const {
	if (!configured_) {
		return 0;
	}
	const Vector2 scale = design_scale_();
	return compiler_.spin_arrow_at(p_index, state_, p_position.x, p_position.y,
			scale.x, scale.y);
}

int MenuFrame::table_row_at(int p_index, const Vector2 &p_position) const {
	if (!configured_) {
		return -1;
	}
	const Vector2 scale = design_scale_();
	return compiler_.table_row_at(p_index, state_, p_position.x, p_position.y,
			scale.x, scale.y);
}

int MenuFrame::hotkey_widget(const String &p_key, bool p_virtual) const {
	if (!configured_) {
		return -1;
	}
	return compiler_.hotkey_widget(to_std(p_key), p_virtual, state_);
}

Vector2i MenuFrame::multiline_line_counts(int p_index) const {
	int fit = 0;
	int total = 0;
	if (configured_) {
		compiler_.multiline_line_counts(p_index, state_, &fit, &total);
	}
	return Vector2i(fit, total);
}

bool MenuFrame::edit_char(int p_index, int p_unicode) {
	if (!configured_) {
		return false;
	}
	// The router's printable filter and the ops both live in engine
	// menu/menu_edit.h (the witnesses ride the engine header).
	if (!opennova::menu::edit_char_insertable(p_unicode)) {
		return false;
	}
	opennova::menu::EditLimits limits;
	if (!compiler_.widget_edit_limits(p_index, &limits)) {
		return false;
	}
	opennova::menu::MenuWidgetState &ws = widget_(p_index);
	opennova::menu::EditField field;
	field.text = ws.has_text ? ws.text : compiler_.widget_authored_text(p_index);
	field.caret = ws.caret >= 0 ? ws.caret : static_cast<int>(field.text.size());
	field.caret = std::min(field.caret, static_cast<int>(field.text.size()));
	const bool changed = opennova::menu::edit_insert_char(field, limits,
			static_cast<char>(p_unicode));
	ws.has_text = true;
	ws.text = field.text;
	ws.caret = field.caret;
	queue_redraw();
	return changed;
}

int MenuFrame::edit_key(int p_index, int p_key, bool p_shift) {
	if (!configured_) {
		return 0;
	}
	opennova::menu::MenuWidgetState &ws = widget_(p_index);
	opennova::menu::EditField field;
	field.text = ws.has_text ? ws.text : compiler_.widget_authored_text(p_index);
	field.caret = ws.caret >= 0 ? ws.caret : static_cast<int>(field.text.size());
	field.caret = std::min(field.caret, static_cast<int>(field.text.size()));
	const opennova::menu::EditKeyResult result =
			opennova::menu::edit_apply_key(field, p_key, 1, p_shift);
	ws.has_text = true;
	ws.text = field.text;
	ws.caret = field.caret;
	queue_redraw();
	return static_cast<int>(result); // the pinned EDIT_RESULT_* contract
}

Ref<Texture2D> MenuFrame::get_cursor_texture() const {
	if (cursor_slot_ >= 0 &&
			cursor_slot_ < static_cast<int32_t>(textures_.size())) {
		return textures_[static_cast<size_t>(cursor_slot_)];
	}
	return Ref<Texture2D>();
}

int MenuFrame::get_unresolved_asset_count() const {
	return unresolved_assets_;
}

void MenuFrame::set_time_ms(int64_t p_ms) {
	state_.time_ms = static_cast<uint32_t>(p_ms);
	queue_redraw();
}

int MenuFrame::widget_index(const String &p_name) const {
	return compiler_.widget_index(p_name.utf8().get_data());
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
	cursor_slot_ = claim.cursor;
	// Activation edges: press lands on the button-down edge over the claim;
	// a click is the release edge while the SAME widget still owns the claim
	// (moving off the widget before release cancels — the standard control
	// contract the Control-tree buttons implemented). A press a scrollbar
	// part consumed never arms a click — the part keeps the mouse until
	// release, like retail's child-BUTTON capture.
	if (p_button_down && !mouse_button_down_ && claim.hovered >= 0 &&
			claim.scroll_index < 0) {
		press_claim_ = claim.hovered;
		emit_signal("widget_pressed", claim.hovered);
	} else if (!p_button_down && mouse_button_down_) {
		if (press_claim_ >= 0 && press_claim_ == claim.hovered) {
			emit_signal("widget_clicked", press_claim_);
		}
		press_claim_ = -1;
	}
	mouse_button_down_ = p_button_down;
	mouse_claim_ = claim.hovered;
	if (claim.scroll_value_changed) {
		emit_signal("scroll_value_changed", claim.scroll_index,
				claim.scroll_value);
	}
	queue_redraw();
	return claim.hovered;
}

bool MenuFrame::process_popup_mouse(int p_index, const Vector2 &p_position,
		bool p_button_down) {
	if (!is_configured()) {
		return false;
	}
	const Vector2 scale = design_scale_();
	const opennova::menu::MenuFrameCompiler::MouseClaim claim =
			compiler_.pump_popup_mouse(state_, p_index, p_position.x,
					p_position.y, p_button_down, scale.x, scale.y);
	state_.cursor_x = p_position.x;
	state_.cursor_y = p_position.y;
	if (claim.scroll_value_changed) {
		// The popup row window moved: the new first-visible row.
		emit_signal("scroll_value_changed", claim.scroll_index,
				claim.scroll_value);
	}
	if (claim.scroll_index >= 0) {
		queue_redraw();
	}
	return claim.scroll_index >= 0;
}

bool MenuFrame::process_mouse_wheel(const Vector2 &p_position, int p_steps) {
	if (!is_configured()) {
		return false;
	}
	const Vector2 scale = design_scale_();
	opennova::menu::MenuFrameCompiler::MouseClaim claim;
	if (!compiler_.pump_mouse_wheel(state_, p_position.x, p_position.y,
				p_steps, scale.x, scale.y, &claim)) {
		return false;
	}
	if (claim.scroll_value_changed) {
		// The row window moved: the new first-visible row.
		emit_signal("scroll_value_changed", claim.scroll_index,
				claim.scroll_value);
		queue_redraw();
	}
	return true;
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
		// The design space and its witness live at menu/menu_frame.h.
		return Vector2(
				size.x / static_cast<float>(opennova::menu::kMenuDesignWidth),
				size.y / static_cast<float>(opennova::menu::kMenuDesignHeight));
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
	ensure_overlay_canvas_item_();
	RenderingServer *rs = RenderingServer::get_singleton();
	rs->canvas_item_clear(overlay_canvas_item_);
	if (!configured_) {
		return;
	}
	const Vector2 scale = design_scale_();
	const opennova::menu::MenuDrawList &list =
			compiler_.compile(state_, scale.x, scale.y);
	// The marquee reset request is one-shot: the compile above consumed it.
	for (opennova::menu::MenuWidgetState &ws : state_.widgets) {
		ws.marquee_reset = false;
	}
	// Apply the compiler's one painter-order stream across primitive kinds.
	// The original walks screens/children forward and each widget emits its
	// frame, appearance, text, then children in vtable+24 order; batching all
	// glyphs after all quads lets later backgrounds leak earlier text through.
	// The engine-side MenuDrawList witness record in docs/mnu/menu-re.md owns
	// the original address correspondence; this adapter only replays it.
	// Ops before overlay_op_start paint on this Control's own canvas item; the
	// menu-top overlay (open popups + cursor) paints on the z-above child item
	// so frame-child mounts never cover it.
	RID target = get_canvas_item();
	const auto apply_quad = [&](const opennova::menu::MenuQuad &quad) {
		const Rect2 rect(quad.x0, quad.y0, quad.x1 - quad.x0, quad.y1 - quad.y0);
		const Color color = argb_to_color(quad.color);
		Ref<Texture2D> tex;
		if (quad.texture >= 0 &&
				quad.texture < static_cast<int32_t>(textures_.size())) {
			tex = textures_[static_cast<size_t>(quad.texture)];
		}
		if (tex.is_null()) {
			if (quad.texture == opennova::menu::kMenuTexNone) {
				rs->canvas_item_add_rect(target, rect, color);
			}
			// An unresolved texture draws nothing [orig: every draw is gated
			// on a successful texture load].
			return;
		}
		if (quad.tiled) {
			rs->canvas_item_add_texture_rect(target, rect, tex->get_rid(), true,
					color);
		} else if (quad.u0 != 0.0f || quad.v0 != 0.0f || quad.u1 != 1.0f ||
				quad.v1 != 1.0f) {
			const Vector2 tex_size = tex->get_size();
			rs->canvas_item_add_texture_rect_region(target, rect, tex->get_rid(),
					Rect2(quad.u0 * tex_size.x, quad.v0 * tex_size.y,
							(quad.u1 - quad.u0) * tex_size.x,
							(quad.v1 - quad.v0) * tex_size.y),
					color);
		} else {
			rs->canvas_item_add_texture_rect(target, rect, tex->get_rid(), false,
					color);
		}
	};
	const auto apply_line = [&](const opennova::menu::MenuLine &line) {
		rs->canvas_item_add_line(target, Vector2(line.x0, line.y0),
				Vector2(line.x1, line.y1), argb_to_color(line.color), 1.0f);
	};
	const auto apply_font_run =
			[&](const opennova::menu::MenuDrawList::FontRun &run) {
				LoadedFont *font = nullptr;
				if (run.font >= 0 && run.font < static_cast<int32_t>(fonts_.size())) {
					font = fonts_[static_cast<size_t>(run.font)];
				}
				if (font == nullptr) {
					return;
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
					rs->canvas_item_add_polygon(target, points, colors, uvs,
							page->get_rid());
				}
				const int32_t underline_end = run.underline_first + run.underline_count;
				for (int32_t i = run.underline_first;
						i < underline_end &&
						i < static_cast<int32_t>(list.underlines.size());
						++i) {
					const opennova::hud::GameFontUnderline &underline =
							list.underlines[static_cast<size_t>(i)];
					rs->canvas_item_add_line(target, Vector2(underline.x0, underline.y),
							Vector2(underline.x1, underline.y),
							argb_to_color(underline.color), 1.0f);
				}
			};
	for (size_t op_index = 0; op_index < list.draw_ops.size(); ++op_index) {
		if (static_cast<int32_t>(op_index) == list.overlay_op_start) {
			target = overlay_canvas_item_;
		}
		const opennova::menu::MenuDrawList::DrawOp &op = list.draw_ops[op_index];
		switch (op.kind) {
			case opennova::menu::MenuDrawList::DrawOp::Kind::Quad:
				if (op.index >= 0 && op.index < static_cast<int32_t>(list.quads.size())) {
					apply_quad(list.quads[static_cast<size_t>(op.index)]);
				}
				break;
			case opennova::menu::MenuDrawList::DrawOp::Kind::Line:
				if (op.index >= 0 && op.index < static_cast<int32_t>(list.lines.size())) {
					apply_line(list.lines[static_cast<size_t>(op.index)]);
				}
				break;
			case opennova::menu::MenuDrawList::DrawOp::Kind::FontRun:
				if (op.index >= 0 &&
						op.index < static_cast<int32_t>(list.font_runs.size())) {
					apply_font_run(list.font_runs[static_cast<size_t>(op.index)]);
				}
				break;
		}
	}
}

Array MenuFrame::options_scroll_ranges() {
	Array out;
	for (const opennova::menu::OptionsScrollRange &r : opennova::menu::kOptionsScrollRanges) {
		Dictionary row;
		row["control"] = String(r.control);
		row["minimum"] = r.minimum;
		row["maximum"] = r.maximum;
		row["page"] = r.page;
		out.push_back(row);
	}
	return out;
}

Array MenuFrame::video_quality_controls() {
	Array out;
	for (const opennova::menu::VideoQualityControl &c : opennova::menu::kVideoQualityControls) {
		Dictionary row;
		row["control"] = String(c.control);
		row["value"] = String(c.semantic_value);
		out.push_back(row);
	}
	return out;
}

int MenuFrame::video_gamma_reference() {
	return opennova::menu::kVideoGammaReference;
}

PackedStringArray MenuFrame::video_preset_buttons() {
	PackedStringArray out;
	for (const char *name : opennova::menu::kVideoPresetButtons) out.push_back(String(name));
	return out;
}

void MenuFrame::_bind_methods() {
	ClassDB::bind_static_method("MenuFrame", D_METHOD("options_scroll_ranges"),
			&MenuFrame::options_scroll_ranges);
	ClassDB::bind_static_method("MenuFrame", D_METHOD("video_quality_controls"),
			&MenuFrame::video_quality_controls);
	ClassDB::bind_static_method("MenuFrame", D_METHOD("video_gamma_reference"),
			&MenuFrame::video_gamma_reference);
	ClassDB::bind_static_method("MenuFrame", D_METHOD("video_preset_buttons"),
			&MenuFrame::video_preset_buttons);
	ClassDB::bind_method(
			D_METHOD("configure", "document", "screen_name", "root", "style",
					"text_lookup"),
			&MenuFrame::configure);
	ClassDB::bind_method(D_METHOD("is_configured"), &MenuFrame::is_configured);
	ClassDB::bind_method(
			D_METHOD("set_widget_shown_override", "index", "shown"),
			&MenuFrame::set_widget_shown_override);
	ClassDB::bind_method(D_METHOD("set_widget_disabled", "index", "disabled"),
			&MenuFrame::set_widget_disabled);
	ClassDB::bind_method(D_METHOD("set_widget_checked", "index", "checked"),
			&MenuFrame::set_widget_checked);
	ClassDB::bind_method(D_METHOD("set_widget_focused", "index", "focused"),
			&MenuFrame::set_widget_focused);
	ClassDB::bind_method(D_METHOD("set_widget_caret", "index", "caret"),
			&MenuFrame::set_widget_caret);
	ClassDB::bind_method(D_METHOD("set_widget_text", "index", "text"),
			&MenuFrame::set_widget_text);
	ClassDB::bind_method(D_METHOD("set_widget_hover_item", "index", "row"),
			&MenuFrame::set_widget_hover_item);
	ClassDB::bind_method(D_METHOD("get_widget_hover_item", "index"),
			&MenuFrame::get_widget_hover_item);
	ClassDB::bind_method(D_METHOD("scroll_hit_at", "index", "position"),
			&MenuFrame::scroll_hit_at);
	ClassDB::bind_method(
			D_METHOD("set_widget_selection", "index", "selected_item",
					"hover_item", "scroll_row"),
			&MenuFrame::set_widget_selection);
	ClassDB::bind_method(D_METHOD("set_widget_scroll_range", "index", "minimum",
								 "maximum", "page", "value"),
			&MenuFrame::set_widget_scroll_range);
	ClassDB::bind_method(D_METHOD("set_widget_popup_open", "index", "open"),
			&MenuFrame::set_widget_popup_open);
	ClassDB::bind_method(D_METHOD("set_time_ms", "ms"),
			&MenuFrame::set_time_ms);
	ClassDB::bind_method(D_METHOD("process_mouse", "position", "button_down"),
			&MenuFrame::process_mouse);
	ClassDB::bind_method(D_METHOD("process_popup_mouse", "index", "position",
								 "button_down"),
			&MenuFrame::process_popup_mouse);
	ClassDB::bind_method(D_METHOD("process_mouse_wheel", "position", "steps"),
			&MenuFrame::process_mouse_wheel);
	ClassDB::bind_method(D_METHOD("widget_index", "name"),
			&MenuFrame::widget_index);
	ADD_SIGNAL(MethodInfo("widget_pressed",
			PropertyInfo(Variant::INT, "index")));
	ADD_SIGNAL(MethodInfo("widget_clicked",
			PropertyInfo(Variant::INT, "index")));
	// The engine pump's CScrollWnd interaction result: a standalone Scroll's
	// authored-range value, or an embedded row owner's new first-visible row.
	ADD_SIGNAL(MethodInfo("scroll_value_changed",
			PropertyInfo(Variant::INT, "index"),
			PropertyInfo(Variant::INT, "value")));
	ClassDB::bind_method(D_METHOD("set_cursor_state", "visible", "position"),
			&MenuFrame::set_cursor_state);
	ClassDB::bind_method(D_METHOD("get_draw_list_stats"),
			&MenuFrame::get_draw_list_stats);
	ClassDB::bind_method(D_METHOD("set_widget_items", "index", "items"),
			&MenuFrame::set_widget_items);
	ClassDB::bind_method(D_METHOD("set_widget_selected_set", "index", "rows"),
			&MenuFrame::set_widget_selected_set);
	ClassDB::bind_method(D_METHOD("set_widget_table_rows", "index", "rows"),
			&MenuFrame::set_widget_table_rows);
	ClassDB::bind_method(D_METHOD("set_widget_marquee_lines", "index", "lines"),
			&MenuFrame::set_widget_marquee_lines);
	ClassDB::bind_method(D_METHOD("widget_count"), &MenuFrame::widget_count);
	ClassDB::bind_method(D_METHOD("widget_name", "index"),
			&MenuFrame::widget_name);
	ClassDB::bind_method(D_METHOD("widget_kind", "index"),
			&MenuFrame::widget_kind);
	ClassDB::bind_method(D_METHOD("widget_authored_text", "index"),
			&MenuFrame::widget_authored_text);
	ClassDB::bind_method(D_METHOD("is_widget_disabled", "index"),
			&MenuFrame::is_widget_disabled);
	ClassDB::bind_method(D_METHOD("is_widget_shown", "index"),
			&MenuFrame::is_widget_shown);
	ClassDB::bind_method(D_METHOD("widget_rect", "index"),
			&MenuFrame::widget_rect);
	ClassDB::bind_method(D_METHOD("item_count", "index"),
			&MenuFrame::item_count);
	ClassDB::bind_method(D_METHOD("get_widget_text", "index"),
			&MenuFrame::get_widget_text);
	ClassDB::bind_method(D_METHOD("get_widget_caret", "index"),
			&MenuFrame::get_widget_caret);
	ClassDB::bind_method(D_METHOD("hit_test", "position"),
			&MenuFrame::hit_test);
	ClassDB::bind_method(D_METHOD("list_row_at", "index", "position"),
			&MenuFrame::list_row_at);
	ClassDB::bind_method(D_METHOD("list_visible_rows", "index"),
			&MenuFrame::list_visible_rows);
	ClassDB::bind_method(D_METHOD("combo_popup_rect", "index"),
			&MenuFrame::combo_popup_rect);
	ClassDB::bind_method(
			D_METHOD("combo_popup_contains", "index", "position"),
			&MenuFrame::combo_popup_contains);
	ClassDB::bind_method(D_METHOD("combo_popup_row_at", "index", "position"),
			&MenuFrame::combo_popup_row_at);
	ClassDB::bind_method(D_METHOD("spin_arrow_at", "index", "position"),
			&MenuFrame::spin_arrow_at);
	ClassDB::bind_method(D_METHOD("table_row_at", "index", "position"),
			&MenuFrame::table_row_at);
	ClassDB::bind_method(D_METHOD("hotkey_widget", "key", "is_virtual"),
			&MenuFrame::hotkey_widget);
	ClassDB::bind_method(D_METHOD("multiline_line_counts", "index"),
			&MenuFrame::multiline_line_counts);
	ClassDB::bind_method(D_METHOD("edit_char", "index", "unicode"),
			&MenuFrame::edit_char);
	ClassDB::bind_method(D_METHOD("edit_key", "index", "key", "shift"),
			&MenuFrame::edit_key);
	ClassDB::bind_method(D_METHOD("get_cursor_texture"),
			&MenuFrame::get_cursor_texture);
	ClassDB::bind_method(D_METHOD("get_unresolved_asset_count"),
			&MenuFrame::get_unresolved_asset_count);
	BIND_CONSTANT(DESIGN_WIDTH);
	BIND_CONSTANT(DESIGN_HEIGHT);
	BIND_CONSTANT(EDIT_KEY_BACKSPACE);
	BIND_CONSTANT(EDIT_KEY_ENTER);
	BIND_CONSTANT(EDIT_KEY_END);
	BIND_CONSTANT(EDIT_KEY_HOME);
	BIND_CONSTANT(EDIT_KEY_LEFT);
	BIND_CONSTANT(EDIT_KEY_RIGHT);
	BIND_CONSTANT(EDIT_KEY_DELETE);
	BIND_CONSTANT(EDIT_RESULT_CHANGED);
	BIND_CONSTANT(EDIT_RESULT_COMMIT);
	BIND_CONSTANT(SCROLL_HIT_SHUTTLE);
}
