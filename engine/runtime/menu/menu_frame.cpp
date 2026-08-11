// The menu frame compiler — the witnessed CWnd Draw walk over a parsed .mnu
// screen, structural translation onto the typed draw list.
// [orig: Menu_RenderFrame @ 0x54b7c0 -> CUIScene_DrawScreensAndCursor
//  @ 0x63bf60 -> the Draw vtable family, cited per pass below]
// Witness record: docs/mnu/menu-re.md ("Widget render dispatch").

#include "menu/menu_frame_internal.h"

#include <io/strutil.h>

#include <algorithm>
#include <climits>
#include <cstring>

namespace opennova::menu {

namespace {

bool iequals(const std::string &a, const char *b) {
	return opennova::strutil::iequals(a, b);
}

// The visual-state slot an APPEARANCE STATE attribute parses to
// [orig: CUIElement_ParseXMLDefinition @ 0x6483d4..0x64845e —
//  DEFAULT=0, DISABLED=1, MOUSEOVER=2, SELECTED=3].
int appearance_state_slot(const std::string &state) {
	if (iequals(state, "disabled")) {
		return kStateDisabled;
	}
	if (iequals(state, "mouseover")) {
		return kStateMouseover;
	}
	if (iequals(state, "selected")) {
		return kStateSelected;
	}
	return kStateDefault; // DEFAULT and unknown tokens keep the parse default
}

uint32_t argb_from_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
	return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(r) << 16) |
			(static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}

mnu::RectEdges offset_rect(const mnu::RectEdges &rect, int dx, int dy) {
	return {rect.left + dx, rect.top + dy, rect.right + dx, rect.bottom + dy};
}

} // namespace

MenuFrameCompiler::MenuFrameCompiler() = default;
MenuFrameCompiler::~MenuFrameCompiler() = default;

void MenuFrameCompiler::set_style_vars(
		const std::map<std::string, std::string> &vars) {
	style_vars_.clear();
	for (const auto &kv : vars) {
		style_vars_[opennova::strutil::to_lower(kv.first)] = kv.second;
	}
}

void MenuFrameCompiler::set_text_lookup(
		const std::map<std::string, std::string> &table) {
	text_lookup_.clear();
	for (const auto &kv : table) {
		text_lookup_[opennova::strutil::to_lower(kv.first)] = kv.second;
	}
}

void MenuFrameCompiler::register_font(const std::string &name,
		const fnt_font_t *font) {
	registered_fonts_[opennova::strutil::to_lower(name)] = font;
}

void MenuFrameCompiler::clear_registered_fonts() {
	registered_fonts_.clear();
}

// Whole-value %VAR% resolution, case-insensitive, unresolved stays literal
// [orig: NapiXML_ExpandVariablesInText @ 0x63a000; ADR 0005 per-field form].
std::string MenuFrameCompiler::resolve_var(const std::string &value) const {
	if (value.size() < 2 || value.front() != '%' || value.back() != '%') {
		return value;
	}
	const std::string key =
			opennova::strutil::to_lower(value.substr(1, value.size() - 2));
	const auto it = style_vars_.find(key);
	return it == style_vars_.end() ? value : it->second;
}

// A FONT color string -> 0xAARRGGBB with alpha forced opaque (the text sink
// forces 0xFF alpha; menus never pass the 0x10000 keep-alpha flag)
// [orig: font_cache_draw_text_scaled @ 0x653170]. An unparsed value keeps the
// zeroed ctor field: opaque black.
uint32_t MenuFrameCompiler::resolve_text_color(const std::string &value) const {
	uint8_t r = 0;
	uint8_t g = 0;
	uint8_t b = 0;
	uint8_t a = 0;
	if (!mnu::parse_hex_color(resolve_var(value), r, g, b, a)) {
		return 0xFF000000u;
	}
	return argb_from_rgba(r, g, b, 0xFF);
}

// String content resolution: type=="id" looks the value up in the registered
// text table [orig: CUIStringTable_LookupString @ 0x6527c0]; a miss or a
// literal type keeps the raw text. The {hot} marker never draws.
std::string MenuFrameCompiler::resolve_text_value(const std::string &type,
		const std::string &raw) const {
	std::string value = resolve_var(raw);
	if (iequals(type, "id")) {
		const auto it = text_lookup_.find(opennova::strutil::to_lower(value));
		if (it != text_lookup_.end()) {
			value = it->second;
		}
	}
	return mnu::strip_hotkey_marker(value);
}

int32_t MenuFrameCompiler::intern_texture(const std::string &name) {
	if (name.empty()) {
		return kMenuTexNone;
	}
	const std::string resolved = resolve_var(name);
	if (resolved.empty()) {
		return kMenuTexNone;
	}
	const std::string key = opennova::strutil::to_lower(resolved);
	for (size_t i = 0; i < texture_names_.size(); ++i) {
		if (opennova::strutil::to_lower(texture_names_[i]) == key) {
			return static_cast<int32_t>(i);
		}
	}
	texture_names_.push_back(resolved);
	texture_sizes_.emplace_back(0, 0);
	return static_cast<int32_t>(texture_names_.size()) - 1;
}

int32_t MenuFrameCompiler::intern_font(const std::string &name) {
	const std::string resolved = resolve_var(name);
	const std::string key = opennova::strutil::to_lower(resolved);
	for (size_t i = 0; i < font_names_.size(); ++i) {
		if (opennova::strutil::to_lower(font_names_[i]) == key) {
			return static_cast<int32_t>(i);
		}
	}
	font_names_.push_back(resolved);
	const auto it = registered_fonts_.find(key);
	fonts_.push_back(it == registered_fonts_.end() ? default_font_
												   : it->second);
	return static_cast<int32_t>(font_names_.size()) - 1;
}

void MenuFrameCompiler::set_texture_size(int32_t slot, int width, int height) {
	if (slot >= 0 && slot < static_cast<int32_t>(texture_sizes_.size())) {
		texture_sizes_[static_cast<size_t>(slot)] = {width, height};
	}
}

std::pair<int, int>
MenuFrameCompiler::state_texture_size(const StatePass &pass) const {
	if (pass.texture < 0 ||
			pass.texture >= static_cast<int32_t>(texture_sizes_.size())) {
		return { 0, 0 };
	}
	const auto &size = texture_sizes_[static_cast<size_t>(pass.texture)];
	return { size.first,
		mnu::appearance_extent_height(pass.has_image_height,
				pass.image_height, size.second) };
}

// --- configure ---------------------------------------------------------------

void MenuFrameCompiler::build_state_passes(
		const std::vector<mnu::Appearance> &rows, StatePass (&states)[4]) {
	for (const mnu::Appearance &ap : rows) {
		const int slot = appearance_state_slot(ap.state);
		StatePass &pass = states[slot];
		pass.present = true;
		if (iequals(ap.type, "color")) {
			// [orig: COLOR=1 -> the stretched fill with the entry color]
			uint8_t r = 0;
			uint8_t g = 0;
			uint8_t b = 0;
			uint8_t a = 0;
			if (mnu::parse_hex_color(resolve_var(ap.value), r, g, b, a)) {
				pass.has_color = true;
				pass.color = argb_from_rgba(r, g, b, a);
			}
		} else if (iequals(ap.type, "outline")) {
			// [orig: OUTLINE=8 -> CUIElement_DrawOutlineRect @ 0x647fc0]
			uint8_t r = 0;
			uint8_t g = 0;
			uint8_t b = 0;
			uint8_t a = 0;
			if (mnu::parse_hex_color(resolve_var(ap.value), r, g, b, a)) {
				pass.has_outline = true;
				pass.outline = argb_from_rgba(r, g, b, a);
			}
		} else if (iequals(ap.type, "image") || ap.type.empty()) {
			// [orig: IMAGE=2 -> stretched into the element rect @ 0x647e40]
			pass.texture = intern_texture(ap.value);
			pass.has_map_state = ap.has_map_state;
			pass.map_state = ap.map_state;
			pass.has_image_height = ap.has_height;
			pass.image_height = ap.height;
		}
		// type="cursor" is the shell-owned custom hook (the CURSOR=4 bit ->
		// the vtable+28 event); the compiler emits nothing for it.
	}
}

void MenuFrameCompiler::configure(const mnu::Screen *screen,
		const fnt_font_t *default_font) {
	screen_ = screen;
	default_font_ = default_font;
	texture_names_.clear();
	texture_sizes_.clear();
	font_names_.clear();
	fonts_.clear();
	nodes_.clear();
	edit_scroll_.clear();
	screen_cursor_ = kMenuTexNone;
	// Slot 0 = the default font (an unnamed <FONT> resolves here).
	font_names_.push_back(std::string());
	fonts_.push_back(default_font_);
	if (screen_ != nullptr) {
		screen_cursor_ = intern_texture(screen_->cursor_file);
		build_node(screen_->root_window, -1);
	}
}

int MenuFrameCompiler::build_node(const mnu::Window &w, int parent) {
	const int index = static_cast<int>(nodes_.size());
	nodes_.push_back(WidgetNode{});
	{
		WidgetNode node;
		node.window = &w;
		node.parent = parent;
		build_state_passes(w.appearances, node.states);
		build_state_passes(w.items.appearances, node.items_states);
		build_state_passes(w.list_box.appearances, node.popup_states);
		build_state_passes(w.list_box.items.appearances, node.popup_items_states);
		auto build_scrollbar = [&](const auto &source,
									   WidgetNode::ScrollbarVisual &visual) {
			visual.present = source.present;
			visual.position = source.position;
			visual.has_position = source.position.has_left && source.position.has_top;
			build_state_passes(source.track, visual.track);
			build_state_passes(source.shuttle, visual.shuttle);
			build_state_passes(source.scrollup, visual.up);
			build_state_passes(source.scrolldown, visual.down);
		};
		build_scrollbar(w.table_data.scrollbar, node.embedded_scrollbar);
		build_scrollbar(w.list_box.scrollbar, node.popup_scrollbar);
		node.popup_scrollbar.edge_pad =
				w.list_box.has_sb_edge_pad ? std::max(w.list_box.sb_edge_pad, 0) : 0;
		if (w.type == mnu::WindowType::Scroll) {
			node.scrollbar.present = true;
			node.scrollbar.vertical = !iequals(w.orientation, "HORIZONTAL");
			// HEIGHT/WIDTH both feed the original's one along-axis child
			// extent. Shipped horizontal bars author HEIGHT and vertical bars
			// author WIDTH; either spelling is accepted, with ctor default 20.
			// [orig: CScrollWnd_Construct @ 0x64c450;
			// CUIScrollWidget_ParseExtendedXMLDef @ 0x64c6d0]
			if (node.scrollbar.vertical) {
				if (w.has_scroll_width && w.scroll_width > 0) {
					node.scrollbar.part_extent = w.scroll_width;
				} else if (w.has_scroll_height && w.scroll_height > 0) {
					node.scrollbar.part_extent = w.scroll_height;
				}
			} else if (w.has_scroll_height && w.scroll_height > 0) {
				node.scrollbar.part_extent = w.scroll_height;
			} else if (w.has_scroll_width && w.scroll_width > 0) {
				node.scrollbar.part_extent = w.scroll_width;
			}
			for (int state = 0; state < 4; ++state) {
				node.scrollbar.track[state] = node.states[state];
			}
			build_state_passes(w.shuttle, node.scrollbar.shuttle);
			build_state_passes(w.scrollup, node.scrollbar.up);
			build_state_passes(w.scrolldown, node.scrollbar.down);
		}
		// FONT: the draw-time walk takes the first self-or-ancestor widget
		// with a font; its 8 colors ride along in parse order
		// [orig: CWnd_GetFontAndColors @ 0x646a70].
		if (!w.font.empty()) {
			node.font = w.font.name.empty() ? 0 : intern_font(w.font.name);
			node.colors[kStateDefault] = resolve_text_color(w.font.default_fg);
			node.colors[kStateDisabled] =
					resolve_text_color(w.font.disabled_fg);
			node.colors[kStateMouseover] =
					resolve_text_color(w.font.mouseover_fg);
			node.colors[kStateSelected] =
					resolve_text_color(w.font.selected_fg);
		} else if (parent >= 0) {
			node.font = nodes_[static_cast<size_t>(parent)].font;
			std::memcpy(node.colors, nodes_[static_cast<size_t>(parent)].colors,
					sizeof(node.colors));
		} else {
			node.font = 0;
			for (uint32_t &c : node.colors) {
				c = 0xFF000000u; // zeroed ctor fields draw opaque black
			}
		}
		// FRAME inheritance [orig: CWnd_FindInheritedFrameBlock @ 0x647190].
		const bool has_frame =
				!w.frame.stencil.empty() || !w.frame.brush.empty();
		if (has_frame) {
			node.frame_owner = index;
			node.frame_stencil = intern_texture(w.frame.stencil);
			node.frame_brush = intern_texture(w.frame.brush);
		} else if (parent >= 0) {
			const WidgetNode &p = nodes_[static_cast<size_t>(parent)];
			node.frame_owner = p.frame_owner;
			node.frame_stencil = p.frame_stencil;
			node.frame_brush = p.frame_brush;
		}
		// CURSOR inheritance [orig: the +276 parent walk @ 0x647a00].
		if (!w.cursor.file.empty()) {
			node.cursor = intern_texture(w.cursor.file);
		} else if (parent >= 0) {
			node.cursor = nodes_[static_cast<size_t>(parent)].cursor;
		}
		// Spin arrows: default-state art (see WidgetNode note).
		auto arrow_pass = [&](const mnu::SpinButton &btn) {
			const mnu::Appearance *selected = nullptr;
			for (const mnu::Appearance &ap : btn.appearances) {
				if ((iequals(ap.type, "image") || ap.type.empty()) &&
						appearance_state_slot(ap.state) == kStateDefault) {
					selected = &ap;
					break;
				}
			}
			if (selected == nullptr) {
				for (const mnu::Appearance &ap : btn.appearances) {
					if (iequals(ap.type, "image") || ap.type.empty()) {
						selected = &ap;
						break;
					}
				}
			}
			StatePass pass;
			if (selected != nullptr) {
				pass.present = true;
				pass.texture = intern_texture(selected->value);
				pass.has_map_state = selected->has_map_state;
				pass.map_state = selected->map_state;
				pass.has_image_height = selected->has_height;
				pass.image_height = selected->height;
			}
			return pass;
		};
		node.spin_up = arrow_pass(w.spinup);
		node.spin_down = arrow_pass(w.spindown);
		// Item rows [orig: id -> text via the string table, image filename
		// loaded, color wcstoul base 16 @ 0x64bd10].
		auto build_items = [&](const std::vector<mnu::Item> &rows,
									std::vector<WidgetNode::ItemVisual> &out) {
			for (const mnu::Item &item : rows) {
				WidgetNode::ItemVisual visual;
				if (iequals(item.type, "image")) {
					visual.kind = WidgetNode::ItemVisual::kImage;
					visual.texture = intern_texture(item.text);
				} else if (iequals(item.type, "color")) {
					visual.kind = WidgetNode::ItemVisual::kColor;
					// [orig: CSpinListWnd_Render @ 0x64b220 — forced opaque]
					visual.color = mnu::item_color_argb(resolve_var(item.text));
				} else {
					visual.kind = WidgetNode::ItemVisual::kText;
					visual.text = resolve_text_value(item.type, item.text);
				}
				out.push_back(visual);
			}
		};
		build_items(w.items.items, node.items);
		build_items(w.list_box.items.items, node.popup_items);
		nodes_[static_cast<size_t>(index)] = std::move(node);
	}
	for (const mnu::Window &child : w.children) {
		build_node(child, index);
	}
	return index;
}

// --- compile helpers ---------------------------------------------------------

const MenuWidgetState *MenuFrameCompiler::state_for(
		const MenuFrameState &state, int index) const {
	for (const MenuWidgetState &ws : state.widgets) {
		if (ws.index == index) {
			return &ws;
		}
	}
	return nullptr;
}

// The pump verdict [orig: widget_process_mouse_event @ 0x647a00 — disabled
// -> 1; hit + button down -> 3; hit + button up -> 2; else 0].
int MenuFrameCompiler::pump_visual_state(const mnu::Window &w,
		const MenuWidgetState *ws) const {
	if (w.disabled || (ws != nullptr && ws->disabled)) {
		return kStateDisabled;
	}
	if (ws != nullptr && ws->pressed) {
		return kStateSelected;
	}
	if (ws != nullptr && ws->hovered) {
		return kStateMouseover;
	}
	return kStateDefault;
}

// The availability fallback [orig: CWnd_SetVisualState @ 0x646340 — a state
// with no authored appearance falls to 0; no default -> -1 = no appearance
// pass at all (text keeps the default color pair)].
int MenuFrameCompiler::appearance_state_with_fallback(const WidgetNode &node,
		int state) const {
	if (state >= 0 && state < 4 && node.states[state].present) {
		return state;
	}
	if (node.states[kStateDefault].present) {
		return kStateDefault;
	}
	return -1;
}

// The element rect: the three-stage POSITION solve in design space
// [orig: the parse tail @ 0x648120 -> adjust_rect_to_text_size @ 0x6575f0].
mnu::RectEdges MenuFrameCompiler::solve_rect(const WidgetNode &node,
		const MenuWidgetState *ws) const {
	const mnu::Window &w = *node.window;
	int max_w = 0;
	int max_h = 0;
	for (int slot_state = 0; slot_state < 4; ++slot_state) {
		const StatePass &pass = node.states[slot_state];
		if (pass.texture < 0) {
			continue;
		}
		const auto size = state_texture_size(pass);
		max_w = std::max(max_w, size.first);
		max_h = std::max(max_h, size.second);
	}
	mnu::RectEdges rect = mnu::position_rect(w.position.has_left,
			w.position.left, w.position.has_top, w.position.top,
			w.position.has_right, w.position.right, w.position.has_bottom,
			w.position.bottom, max_w, max_h);
	if (mnu::window_type_is_text_sized(w.type)) {
		const std::string text = widget_text(node, ws);
		if (!text.empty() &&
				(rect.right <= rect.left || rect.bottom <= rect.top)) {
			int tw = 0;
			int th = 0;
			measure_text(node, text, &tw, &th);
			rect = mnu::adjust_rect_to_text_size(rect, tw, th,
					w.string_data.justify, w.string_data.vjustify);
		}
	}
	return rect;
}

std::string MenuFrameCompiler::widget_text(const WidgetNode &node,
		const MenuWidgetState *ws) const {
	if (ws != nullptr && ws->has_text) {
		return ws->text;
	}
	const mnu::Window &w = *node.window;
	if (w.string_data.present) {
		return resolve_text_value(w.string_data.type, w.string_data.value);
	}
	return std::string();
}

const fnt_font_t *MenuFrameCompiler::font_for(const WidgetNode &node) const {
	const fnt_font_t *font = fonts_[static_cast<size_t>(node.font)];
	return font != nullptr ? font : default_font_;
}

void MenuFrameCompiler::measure_text(const WidgetNode &node,
		const std::string &text, int *out_w, int *out_h) const {
	*out_w = 0;
	*out_h = 0;
	const fnt_font_t *font = font_for(node);
	if (font == nullptr) {
		return;
	}
	hud::GameFont gf;
	gf.set_font(font);
	// Menu measurement runs at scale 1 in design space
	// [orig: CStaticWnd_DrawLabel @ 0x656fb0 fld1 before the measure].
	gf.measure(text.c_str(), 1.0f, 1.0f, out_w, out_h);
}

// --- emitters ----------------------------------------------------------------

// Per-element int truncation of the scaled coordinate
// [orig: CUIElement_DrawStretchedTexture @ 0x647d40 — (int)(edge * scale)].
float MenuFrameCompiler::emit_x(int design, float scale) {
	return static_cast<float>(
			static_cast<int>(static_cast<double>(design) * scale));
}

void MenuFrameCompiler::push_quad(const MenuQuad &quad) {
	draw_list_.draw_ops.push_back(
			{ MenuDrawList::DrawOp::Kind::Quad,
					static_cast<int32_t>(draw_list_.quads.size()) });
	draw_list_.quads.push_back(quad);
}

void MenuFrameCompiler::push_line(const MenuLine &line) {
	draw_list_.draw_ops.push_back(
			{ MenuDrawList::DrawOp::Kind::Line,
					static_cast<int32_t>(draw_list_.lines.size()) });
	draw_list_.lines.push_back(line);
}

void MenuFrameCompiler::push_font_run(const MenuDrawList::FontRun &run) {
	draw_list_.draw_ops.push_back(
			{ MenuDrawList::DrawOp::Kind::FontRun,
					static_cast<int32_t>(draw_list_.font_runs.size()) });
	draw_list_.font_runs.push_back(run);
}

void MenuFrameCompiler::emit_rect_quad(const mnu::RectEdges &design,
		const WalkScale &s, uint32_t color, int32_t texture, bool tiled,
		float tile_u, float tile_v) {
	MenuQuad quad;
	quad.x0 = emit_x(design.left, s.x);
	quad.y0 = emit_x(design.top, s.y);
	quad.x1 = emit_x(design.right, s.x);
	quad.y1 = emit_x(design.bottom, s.y);
	quad.color = color;
	quad.texture = texture;
	quad.tiled = tiled;
	if (tiled) {
		quad.u1 = tile_u;
		quad.v1 = tile_v;
	}
	push_quad(quad);
}

// MAP_STATE/HEIGHT selects one source row while the IMAGE pass still stretches
// into the authored destination rect. The typed list carries that source band
// as normalized UVs [orig: CUIElement_ParseXMLDefinition @ 0x648120;
// CUIElement_DrawTextureNative @ 0x647e40 ->
// CTextureManager_DrawScaledRect @ 0x654e60].
void MenuFrameCompiler::emit_state_texture(const mnu::RectEdges &design,
		const WalkScale &s,
		const StatePass &pass) {
	if (pass.texture < 0) {
		return;
	}
	emit_rect_quad(design, s, 0xFFFFFFFFu, pass.texture, false, 1.0f, 1.0f);
	if (!pass.has_map_state || pass.map_state < 0 || !pass.has_image_height ||
			pass.image_height <= 0) {
		return;
	}
	const int texture_height =
			texture_sizes_[static_cast<size_t>(pass.texture)].second;
	if (texture_height <= 0) {
		return;
	}
	MenuQuad &quad = draw_list_.quads.back();
	quad.v0 = static_cast<float>(pass.map_state * pass.image_height) /
			static_cast<float>(texture_height);
	quad.v1 = static_cast<float>((pass.map_state + 1) * pass.image_height) /
			static_cast<float>(texture_height);
}

void MenuFrameCompiler::emit_state_pass(const mnu::RectEdges &design,
		const WalkScale &s,
		const StatePass &pass) {
	if (pass.has_color) {
		emit_rect_quad(design, s, pass.color, kMenuTexNone, false, 1.0f, 1.0f);
	}
	emit_state_texture(design, s, pass);
	if (pass.has_outline) {
		emit_outline(design, s, pass.outline);
	}
}

// [orig: CUIElement_DrawOutlineRect @ 0x647fc0 — top edge to right-1, then
//  down, back, and up only when the rect has height]
void MenuFrameCompiler::emit_outline(const mnu::RectEdges &design,
		const WalkScale &s, uint32_t color) {
	const float x0 = emit_x(design.left, s.x);
	const float y0 = emit_x(design.top, s.y);
	const float x1 = emit_x(design.right, s.x) - 1.0f;
	const float y1 = emit_x(design.bottom, s.y) - 1.0f;
	push_line({ x0, y0, x1, y0, color });
	if (y1 > y0) {
		push_line({ x1, y0, x1, y1, color });
		push_line({ x1, y1, x0, y1, color });
		push_line({ x0, y1, x0, y0, color });
	}
}

// The per-state appearance passes in the witnessed pass order: COLOR fill,
// IMAGE, OUTLINE (the CURSOR/custom hook stays shell-side)
// [orig: CUIElement_Draw @ 0x64a8a0 / CStaticWnd_Render @ 0x657b10].
// `appearance_slot` is the FINAL slot (fallback or force already applied);
// -1 skips every pass.
void MenuFrameCompiler::emit_appearance(const WidgetNode &node,
		const mnu::RectEdges &rect, const WalkScale &s, int appearance_slot) {
	if (appearance_slot < 0 || appearance_slot > 3) {
		return;
	}
	emit_state_pass(rect, s, node.states[appearance_slot]);
}

// The 8-piece frame + tiled brush fill. Retail submits 0x7F7F7F through its
// modulate-2x material (therefore no tint); MenuQuad carries the effective
// ordinary-multiply backend color, so the equivalent value is white.
// resolves [orig: CUIElement_DrawFrame @ 0x64a210 over init_border_materials
// @ 0x646f70].
void MenuFrameCompiler::emit_frame(const WidgetNode &node,
		const mnu::RectEdges &rect, const WalkScale &s) {
	if (node.frame_owner < 0) {
		return;
	}
	const mnu::Frame &frame =
			nodes_[static_cast<size_t>(node.frame_owner)].window->frame;
	const bool has_stencil = node.frame_stencil >= 0;
	const bool has_brush = node.frame_brush >= 0;
	if (!has_stencil && !has_brush) {
		return;
	}
	const int w = rect.right - rect.left;
	const int h = rect.bottom - rect.top;
	if (has_brush) {
		const auto &size =
				texture_sizes_[static_cast<size_t>(node.frame_brush)];
		const float tiles_u = size.first > 0
				? static_cast<float>(w) / static_cast<float>(size.first)
				: 1.0f;
		const float tiles_v = size.second > 0
				? static_cast<float>(h) / static_cast<float>(size.second)
				: 1.0f;
		emit_rect_quad(rect, s, 0xFFFFFFFFu, node.frame_brush, true, tiles_u,
				tiles_v);
	}
	if (!has_stencil) {
		return;
	}
	const auto &stencil_size =
			texture_sizes_[static_cast<size_t>(node.frame_stencil)];
	if (stencil_size.first <= 0) {
		return;
	}
	const int tile = mnu::frame_stencil_tile_size(
			frame.has_stencil_size ? frame.stencil_size : 0,
			stencil_size.first);
	if (tile <= 0) {
		return;
	}
	const int insetx = frame.has_insetx ? frame.insetx : 0;
	const int insety = frame.has_insety ? frame.insety : 0;
	const auto pieces = mnu::frame_border_layout(tile, insetx, insety);
	const float tex_w = static_cast<float>(stencil_size.first);
	const float tex_h = static_cast<float>(stencil_size.second);
	for (const mnu::FrameBorderPiece &piece : pieces) {
		const mnu::FrameTileRect uv =
				mnu::frame_tile_rect(tile, piece.tile_col, piece.tile_row);
		mnu::RectEdges dest;
		dest.left = rect.left +
				static_cast<int>(
						piece.anchor_left * static_cast<float>(w) +
						piece.off_left);
		dest.top = rect.top +
				static_cast<int>(piece.anchor_top * static_cast<float>(h) +
						piece.off_top);
		dest.right = rect.left +
				static_cast<int>(
						piece.anchor_right * static_cast<float>(w) +
						piece.off_right);
		dest.bottom = rect.top +
				static_cast<int>(
						piece.anchor_bottom * static_cast<float>(h) +
						piece.off_bottom);
		MenuQuad quad;
		quad.x0 = emit_x(dest.left, s.x);
		quad.y0 = emit_x(dest.top, s.y);
		quad.x1 = emit_x(dest.right, s.x);
		quad.y1 = emit_x(dest.bottom, s.y);
		quad.color = 0xFFFFFFFFu;
		quad.texture = node.frame_stencil;
		if (tex_w > 0.0f && tex_h > 0.0f) {
			quad.u0 = static_cast<float>(uv.x) / tex_w;
			quad.v0 = static_cast<float>(uv.y) / tex_h;
			quad.u1 = static_cast<float>(uv.x + uv.size) / tex_w;
			quad.v1 = static_cast<float>(uv.y + uv.size) / tex_h;
		}
		push_quad(quad);
	}
}

// Glyph runs: layout in design space at the anamorphic pair — the anchor is
// scaled with the per-element truncation, and the glyphs scale by the same
// pair [orig: font_cache_draw_text_scaled @ 0x653170 scales the anchor and
// forwards scaleX/scaleY into CGameFont_DrawText @ 0x6752c0].
void MenuFrameCompiler::emit_glyph_run(const WidgetNode &node,
		const std::string &text, int design_x, int design_y,
		const WalkScale &s, uint32_t color, int caret) {
	const fnt_font_t *font = font_for(node);
	if (font == nullptr || text.empty()) {
		return;
	}
	hud::GameFont gf;
	gf.set_font(font);
	const size_t first = draw_list_.glyphs.size();
	const size_t underline_first = draw_list_.underlines.size();
	const float x = emit_x(design_x, s.x);
	const float y = emit_x(design_y, s.y);
	const hud::GameFontRun run =
			gf.layout(text.c_str(), x, y, s.x, s.y, 0u, color);
	draw_list_.glyphs.insert(draw_list_.glyphs.end(), run.quads.begin(),
			run.quads.end());
	draw_list_.underlines.insert(draw_list_.underlines.end(),
			run.underlines.begin(), run.underlines.end());
	if (caret >= 0) {
		emit_caret(gf, text, x, y, s, color, caret);
	}
	const size_t count = draw_list_.glyphs.size() - first;
	if (count > 0) {
		MenuDrawList::FontRun fr;
		fr.font = node.font;
		fr.first = static_cast<int32_t>(first);
		fr.count = static_cast<int32_t>(count);
		fr.underline_first = static_cast<int32_t>(underline_first);
		fr.underline_count =
				static_cast<int32_t>(draw_list_.underlines.size() - underline_first);
		push_font_run(fr);
	}
}

// The caret: an underscore drawn at the caret character's x, x-stretched to
// that character's width; the char at end-of-text measures as '_' itself
// [orig: draw_text_with_cursor @ 0x6533b0 — the left-run measure, the
//  (spacing-1)*design_scale + 1 gap terms (CGameFont_GetSpacingPad
//  @ 0x6741e0), then the stretched '_' strike].
void MenuFrameCompiler::emit_caret(hud::GameFont &gf, const std::string &text,
		float x, float y, const WalkScale &s, uint32_t color, int caret) {
	const fnt_font_t *font = gf.font();
	const int len = static_cast<int>(text.size());
	const int at = std::clamp(caret, 0, len);
	int left_w = 0;
	int left_h = 0;
	if (at > 0) {
		gf.measure(text.substr(0, static_cast<size_t>(at)).c_str(), 1.0f,
				1.0f, &left_w, &left_h);
	}
	const float pad = static_cast<float>(font->glyph_spacing - 1) *
					fnt_design_scale(font->design_width) +
			1.0f;
	float cursor_design_x = static_cast<float>(left_w);
	if (left_w > 0) {
		cursor_design_x += pad;
	}
	if (at > 0 && at < len) {
		cursor_design_x += pad;
	}
	const char under = at < len ? text[static_cast<size_t>(at)] : '_';
	const char under_str[2] = {under == '\0' ? '_' : under, '\0'};
	int under_w = 0;
	int under_h = 0;
	gf.measure(under_str, 1.0f, 1.0f, &under_w, &under_h);
	int bar_w = 0;
	gf.measure("_", 1.0f, 1.0f, &bar_w, &under_h);
	const float stretch = bar_w > 0
			? static_cast<float>(under_w) / static_cast<float>(bar_w)
			: 1.0f;
	const hud::GameFontRun run = gf.layout("_",
			x + cursor_design_x * s.x, y, s.x * stretch, s.y, 0u, color);
	draw_list_.glyphs.insert(draw_list_.glyphs.end(), run.quads.begin(),
			run.quads.end());
}

// The single-line text pass shared by the static family
// [orig: CStaticWnd_DrawLabel @ 0x656fb0 — edge-inset available width,
//  truncate-to-fit, justify from the truncated width, then the state-colored
//  draw; the wrap path (+760 -> sub_653D60) is deferred (D-MNU-13)].
// [orig: CComboWnd_Render @ 0x65c05b..0x65c083 — this[183] = row_text(list,
// selected_row(list)); CStaticWnd_DrawLabel; restore. Text-only: image/color
// rows contribute their stored text (possibly empty).]
std::string MenuFrameCompiler::combo_face_text(const WidgetNode &node,
		const MenuWidgetState *ws) const {
	const int selected = ws != nullptr ? ws->selected_item : 0;
	if (ws != nullptr && ws->has_items) {
		if (selected < 0 || selected >= static_cast<int>(ws->items.size())) {
			return std::string();
		}
		return ws->items[static_cast<size_t>(selected)];
	}
	const mnu::Window &w = *node.window;
	const std::vector<WidgetNode::ItemVisual> &rows =
			w.list_box.items.present ? node.popup_items : node.items;
	if (selected < 0 || selected >= static_cast<int>(rows.size())) {
		return std::string();
	}
	return rows[static_cast<size_t>(selected)].text;
}

void MenuFrameCompiler::emit_widget_text(const WidgetNode &node,
		const mnu::RectEdges &rect, const WalkScale &s, int color_state,
		const MenuWidgetState *ws, int caret,
		const std::string *override_text) {
	const mnu::Window &w = *node.window;
	const std::string text =
			override_text != nullptr ? *override_text : widget_text(node, ws);
	if (text.empty()) {
		return;
	}
	const int edge = w.string_data.has_edge ? w.string_data.edge : 0;
	const int avail = (rect.right - rect.left) - 2 * edge;
	if (avail < 1) {
		return; // [orig: the < 1 early-out]
	}
	int full_w = 0;
	int line_h = 0;
	measure_text(node, text, &full_w, &line_h);
	// Truncate to the largest prefix strictly below the span
	// [orig: the prefix-measure loop @ 0x657199..0x6571e4].
	std::string drawn = text;
	int drawn_w = full_w;
	if (full_w > avail) {
		int fit = static_cast<int>(text.size());
		for (int n = 1; n <= static_cast<int>(text.size()); ++n) {
			int wpx = 0;
			int hpx = 0;
			measure_text(node, text.substr(0, static_cast<size_t>(n)), &wpx,
					&hpx);
			drawn_w = wpx;
			if (wpx >= avail) {
				fit = n - 1;
				break;
			}
		}
		drawn = text.substr(0, static_cast<size_t>(std::max(fit, 0)));
	}
	// Justify [orig: the +744 word — low nibble 1=center/2=right else left;
	// high 0x10=vcenter/0x20=vbottom else top].
	int x = rect.left;
	if (iequals(w.string_data.justify, "center")) {
		x = ((rect.right - rect.left) >> 1) - (drawn_w >> 1) + rect.left;
	} else if (iequals(w.string_data.justify, "right")) {
		x = rect.right - drawn_w;
	}
	int y = rect.top;
	if (iequals(w.string_data.vjustify, "center")) {
		y = ((rect.bottom - rect.top) >> 1) - (line_h >> 1) + rect.top;
	} else if (iequals(w.string_data.vjustify, "bottom")) {
		y = rect.bottom - line_h;
	}
	x += edge; // [orig: the edge term added into the draw x]
	const int state = color_state >= 0 && color_state < 4 ? color_state : 0;
	emit_glyph_run(node, drawn, x, y, s, node.colors[state], caret);
}

// The edit render [orig: CEditWnd_Render @ 0x6619e0]: focus forces visual
// state 2; PASSWORD masks with '*'; the scroll window keeps the caret
// visible; the caret blinks while (time & 0x3FF) > 0x200 and is drawn
// relative to the scrolled text.
void MenuFrameCompiler::emit_edit(int index, const WidgetNode &node,
		const mnu::RectEdges &rect, const WalkScale &s, int visual,
		const MenuFrameState &frame, const MenuWidgetState *ws) {
	const mnu::Window &w = *node.window;
	const bool focused = ws != nullptr && ws->focused && !w.readonly;
	std::string text = widget_text(node, ws);
	if (w.password) {
		text.assign(text.size(), '*');
	}
	const bool blink_on = (frame.time_ms & 0x3FFu) > 0x200u;
	if (text.empty()) {
		// An empty focused edit still blinks its caret at the text anchor
		// [orig: the cursor leg of draw_text_with_cursor @ 0x6533b0 runs for
		//  the empty string — the char at the caret is the NUL -> '_'].
		if (focused && blink_on) {
			const fnt_font_t *font = font_for(node);
			if (font != nullptr) {
				hud::GameFont gf;
				gf.set_font(font);
				const int edge =
						w.string_data.has_edge ? w.string_data.edge : 0;
				int line_h = 0;
				int tw = 0;
				measure_text(node, "_", &tw, &line_h);
				int y = rect.top;
				if (iequals(w.string_data.vjustify, "center")) {
					y = ((rect.bottom - rect.top) >> 1) - (line_h >> 1) +
							rect.top;
				} else if (iequals(w.string_data.vjustify, "bottom")) {
					y = rect.bottom - line_h;
				}
				const size_t first = draw_list_.glyphs.size();
				emit_caret(gf, std::string(), emit_x(rect.left + edge, s.x),
						emit_x(y, s.y), s, node.colors[visual >= 0 && visual < 4
													? visual
													: 0],
						0);
				const size_t count = draw_list_.glyphs.size() - first;
				if (count > 0) {
					MenuDrawList::FontRun fr;
					fr.font = node.font;
					fr.first = static_cast<int32_t>(first);
					fr.count = static_cast<int32_t>(count);
					push_font_run(fr);
				}
			}
		}
		return;
	}
	// The scroll window [orig: update_edit_scroll_range @ 0x661790 over
	// start +792 / end +796, fitting via EditWnd_CountCharsFitting
	// @ 0x6616b0]: caret before the window scrolls back; past the window
	// scrolls forward so the caret is the last visible char.
	EditScroll &scroll = edit_scroll_[index];
	const int len = static_cast<int>(text.size());
	const int caret =
			ws != nullptr ? std::clamp(ws->caret, 0, len) : 0;
	const int edge = w.string_data.has_edge ? w.string_data.edge : 0;
	const int avail = (rect.right - rect.left) - 2 * edge;
	auto fit_from = [&](int start) {
		int n = 0;
		while (start + n < len) {
			int wpx = 0;
			int hpx = 0;
			measure_text(node,
					text.substr(static_cast<size_t>(start),
							static_cast<size_t>(n + 1)),
					&wpx, &hpx);
			if (wpx >= avail) {
				break;
			}
			++n;
		}
		return n;
	};
	scroll.start = std::clamp(scroll.start, 0, len);
	if (caret < scroll.start) {
		scroll.start = caret;
		scroll.end = scroll.start + fit_from(scroll.start);
	} else if (caret > scroll.end || scroll.end > len) {
		scroll.end = caret;
		const int fit = fit_from(std::max(scroll.end - avail, 0));
		scroll.start = std::max(scroll.end - std::max(fit, 1), 0);
	} else if (scroll.end == 0 && len > 0) {
		scroll.end = fit_from(scroll.start);
	}
	const std::string visible = text.substr(
			static_cast<size_t>(scroll.start));
	// The caret blink gate [orig: (GetTickCount() & 0x3FF) > 0x200
	// @ 0x661c63]; -1 draws no caret.
	int caret_draw = -1;
	if (focused && blink_on) {
		caret_draw = caret - scroll.start;
	}
	MenuWidgetState local;
	if (ws != nullptr) {
		local = *ws;
	}
	local.has_text = true;
	local.text = visible;
	emit_widget_text(node, rect, s, visual, &local, caret_draw);
}

// The multiline edit render [orig: CMEditWnd_Render @ 0x6608e0]: frame ->
// appearance for the RAW pump state (unlike the single-line sibling, focus
// does NOT force state 2) -> the wrapped drawer. The caret blinks on the
// same (time & 0x3FF) > 0x200 gate; PASSWORD masks with '*'; the vertical
// scroll is line-based — MenuWidgetState.scroll_row carries the
// first-visible-line count (widget +3912), fed by the embedder from
// multiline_line_counts().
void MenuFrameCompiler::emit_multiline_edit(const WidgetNode &node,
		const mnu::RectEdges &rect, const WalkScale &s, int visual,
		const MenuFrameState &frame, const MenuWidgetState *ws) {
	const mnu::Window &w = *node.window;
	std::string text = widget_text(node, ws);
	if (w.password) {
		text.assign(text.size(), '*');
	}
	const bool focused = ws != nullptr && ws->focused && !w.readonly;
	const bool blink_on = (frame.time_ms & 0x3FFu) > 0x200u;
	int caret = -1;
	if (focused && blink_on && ws != nullptr) {
		caret = std::clamp(ws->caret, 0,
				static_cast<int>(text.size()));
	}
	if (text.empty() && caret < 0) {
		return;
	}
	const int state = visual >= 0 && visual < 4 ? visual : 0;
	const int first_line = ws != nullptr ? std::max(ws->scroll_row, 0) : 0;
	// The edge inset rides the draw x [orig: widget[189] added into the pen
	// origin]; the block-alignment leg (whole-unwrapped-text measure) only
	// engages for fitting text — shipped multiline edits author none, so the
	// compiled path keeps the top-left origin.
	const int edge = w.string_data.has_edge ? w.string_data.edge : 0;
	const mnu::RectEdges pen{rect.left + edge, rect.top, rect.right,
			rect.bottom};
	emit_wrapped_text(node, pen, s, node.colors[state], text, first_line,
			caret);
}

// The wrapped-text drawer, clip-bottom mode [orig: draw_text_wrapped
// @ 0x653710 via the 0x40000 wrapper draw_text_wrapped_clipped @ 0x653D60].
// Break rules, exactly: the line accumulates chars measured at the widget
// scale pair against trunc(wrapW * scaleX); an explicit LF (only 0x0A — CR
// is a drawn glyph) or the terminator breaks at the char; overflow breaks at
// the last space of the line (consumed), else the overflowing char starts
// the next line. Skipped lines (the scroll window) advance nothing. Each
// drawn line justifies and advances by its 1.0-scale measure; the bottom
// clip stops when the NEXT line's bottom would overflow. The caret pass
// draws "|" centered at the accumulated (scaled) width — the original mixes
// the scaled accumulator into the design-space pen, preserved verbatim.
void MenuFrameCompiler::emit_wrapped_text(const WidgetNode &node,
		const mnu::RectEdges &rect, const WalkScale &s, uint32_t color,
		const std::string &text, int first_visible_line, int caret) {
	const fnt_font_t *font = font_for(node);
	if (font == nullptr) {
		return;
	}
	hud::GameFont gf;
	gf.set_font(font);
	int wrap_w = rect.right - rect.left;
	if (wrap_w <= 0) {
		wrap_w = 0x10000; // [orig: 0 -> unbounded]
	}
	int avail_h = rect.bottom - rect.top;
	if (avail_h <= 0) {
		avail_h = 0x10000;
	}
	const int threshold =
			static_cast<int>(static_cast<float>(wrap_w) * s.x); // ftol
	const int bottom = rect.top + avail_h;
	const int x = rect.left;
	int cur_y = rect.top;
	int line_start = 0;
	int last_space = 0; // index 0 doubles as "none" [orig quirk]
	int line_no = 0;
	int prev_accum = 0; // scaled width of [line_start, i) for the caret pass
	const int len = static_cast<int>(text.size());
	int i = 0;
	while (true) {
		const char c = i < len ? text[static_cast<size_t>(i)] : '\0';
		if (c == ' ') {
			last_space = i;
		}
		if (caret >= 0 && i == caret) {
			int cw = 0;
			int ch = 0;
			gf.measure("|", 1.0f, 1.0f, &cw, &ch);
			emit_glyph_run(node, "|", x + prev_accum - (cw >> 1), cur_y, s,
					color, -1);
		}
		int accum_w = 0;
		int accum_h = 0;
		if (i >= line_start) {
			gf.measure(text.substr(static_cast<size_t>(line_start),
								 static_cast<size_t>(i - line_start) +
										 (c != '\0' ? 1u : 0u))
							   .c_str(),
					s.x, s.y, &accum_w, &accum_h);
		}
		int break_at;
		int next;
		if (accum_w <= threshold) {
			if (c != '\n' && c != '\0') {
				prev_accum = accum_w;
				++i;
				continue;
			}
			break_at = i;
			next = i + 1;
		} else if (last_space != 0) {
			break_at = last_space;
			next = last_space + 1;
		} else {
			break_at = i; // the overflowing char starts the next line
			next = i;
			if (next <= line_start) {
				// Finite-progress guard: a single glyph wider than the
				// widget (cannot occur with shipped fonts/rects).
				next = line_start + 1;
			}
		}
		++line_no;
		const char at_break =
				break_at < len ? text[static_cast<size_t>(break_at)] : '\0';
		if (line_no > first_visible_line) {
			const std::string line = text.substr(
					static_cast<size_t>(line_start),
					static_cast<size_t>(break_at - line_start));
			int lw = 0;
			int lh = 0;
			gf.measure(line.c_str(), 1.0f, 1.0f, &lw, &lh);
			if (lh <= 0) {
				// Blank lines keep the font line height (the "W" measure the
				// widget family uses for row heights).
				int tw = 0;
				gf.measure("W", 1.0f, 1.0f, &tw, &lh);
			}
			if (!line.empty()) {
				emit_glyph_run(node, line, x, cur_y, s, color, -1);
			}
			if (at_break == '\0') {
				return;
			}
			cur_y += lh;
			if (cur_y + lh > bottom) {
				return; // [orig: the 0x40000 bottom clip]
			}
		} else if (at_break == '\0') {
			// The original returns only on the drawn branch, relying on the
			// clamped scroll range; the guard keeps a mis-clamped embedder
			// finite without changing clamped behavior.
			return;
		}
		prev_accum = 0;
		last_space = 0;
		line_start = next;
		i = next;
	}
}

// The checkbox label [orig: CCheckWnd_DrawLabel @ 0x64aa20]: non-AS_BUTTON
// pins the label at rect.right + 2 with the h-justify ignored (v-justify
// honored); AS_BUTTON honors both justifies inside the full rect. Colors
// follow the RAW pump state, not the checked-forced slot.
void MenuFrameCompiler::emit_checkbox_label(const WidgetNode &node,
		const mnu::RectEdges &rect, const WalkScale &s, int color_state,
		const MenuWidgetState *ws) {
	const mnu::Window &w = *node.window;
	const std::string text = widget_text(node, ws);
	if (text.empty()) {
		return;
	}
	int tw = 0;
	int th = 0;
	measure_text(node, text, &tw, &th);
	int y = rect.top;
	if (iequals(w.string_data.vjustify, "center")) {
		y = ((rect.bottom - rect.top) >> 1) - (th >> 1) + rect.top;
	} else if (iequals(w.string_data.vjustify, "bottom")) {
		y = rect.bottom - th;
	}
	int x;
	if (w.as_button) {
		x = rect.left;
		if (iequals(w.string_data.justify, "center")) {
			x = ((rect.right - rect.left) >> 1) - (tw >> 1) + rect.left;
		} else if (iequals(w.string_data.justify, "right")) {
			x = rect.right - tw;
		}
	} else {
		x = rect.right + 2; // [orig: label_rect.left = right + 2]
	}
	const int state = color_state >= 0 && color_state < 4 ? color_state : 0;
	emit_glyph_run(node, text, x, y, s, node.colors[state], -1);
}

// The selected-item cell (spinlist; the combo closed cell shares it)
// [orig: CSpinListWnd_Render @ 0x64b220 — text / native-size image aligned
// per the ITEMS justify (spinlist default centre/centre) / full-rect opaque
// color swatch].
void MenuFrameCompiler::emit_item_cell(const WidgetNode &node,
		const mnu::RectEdges &rect, const WalkScale &s, int color_state,
		const MenuWidgetState *ws) {
	const int selected = ws != nullptr ? ws->selected_item : 0;
	const bool runtime_rows = ws != nullptr && ws->has_items;
	const int row_count = runtime_rows ? static_cast<int>(ws->items.size())
									   : static_cast<int>(node.items.size());
	if (selected < 0 || selected >= row_count) {
		return;
	}
	// Runtime-seeded rows are text-only (the Control-tree set_items shape).
	WidgetNode::ItemVisual runtime_item;
	if (runtime_rows) {
		runtime_item.kind = WidgetNode::ItemVisual::kText;
		runtime_item.text = ws->items[static_cast<size_t>(selected)];
	}
	const WidgetNode::ItemVisual &item = runtime_rows
			? runtime_item
			: node.items[static_cast<size_t>(selected)];
	const mnu::Window &w = *node.window;
	const std::string &jh = w.items.justify;
	const std::string &jv = w.items.vjustify;
	switch (item.kind) {
		case WidgetNode::ItemVisual::kColor: {
			emit_rect_quad(rect, s, item.color, kMenuTexNone, false, 1.0f,
					1.0f);
			break;
		}
		case WidgetNode::ItemVisual::kImage: {
			if (item.texture < 0) {
				break;
			}
			const auto &size =
					texture_sizes_[static_cast<size_t>(item.texture)];
			const int iw = size.first;
			const int ih = size.second;
			int x = rect.left;
			int y = rect.top;
			if (jh.empty() || iequals(jh, "center")) {
				x = rect.left + ((rect.right - rect.left) - iw) / 2;
			} else if (iequals(jh, "right")) {
				x = rect.right - iw;
			}
			if (jv.empty() || iequals(jv, "center")) {
				y = rect.top + ((rect.bottom - rect.top) - ih) / 2;
			} else if (iequals(jv, "bottom")) {
				y = rect.bottom - ih;
			}
			emit_rect_quad({x, y, x + iw, y + ih}, s, 0xFFFFFFFFu,
					item.texture, false, 1.0f, 1.0f);
			break;
		}
		case WidgetNode::ItemVisual::kText: {
			int tw = 0;
			int th = 0;
			measure_text(node, item.text, &tw, &th);
			int x = rect.left;
			int y = rect.top;
			if (iequals(jh, "center")) {
				x = rect.left + ((rect.right - rect.left) - tw) / 2;
			} else if (iequals(jh, "right")) {
				x = rect.right - tw;
			}
			if (iequals(jv, "center")) {
				y = rect.top + ((rect.bottom - rect.top) - th) / 2;
			} else if (iequals(jv, "bottom")) {
				y = rect.bottom - th;
			}
			const int state =
					color_state >= 0 && color_state < 4 ? color_state : 0;
			emit_glyph_run(node, item.text, x, y, s, node.colors[state], -1);
			break;
		}
	}
}

// List rows [orig: CListWnd_DrawItems @ 0x643f30]: each visible row first
// draws the ITEMS per-state appearance for its style index (selection/hover
// highlight) into the row rect, then the row text with the SAME style index
// selecting the FONT color pair. Height = font "W" else MIN_ITEM_HEIGHT;
// rows run from the scroll row and clip to the widget rect.
void MenuFrameCompiler::emit_list_rows(const WidgetNode &node,
		const mnu::RectEdges &rect, const WalkScale &s,
		const MenuWidgetState *ws) {
	const mnu::Window &w = *node.window;
	const int row_h = row_height_(node);
	const bool runtime_rows = ws != nullptr && ws->has_items;
	const int row_count = runtime_rows ? static_cast<int>(ws->items.size())
									   : static_cast<int>(node.items.size());
	if (row_h <= 0 || row_count == 0) {
		return;
	}
	const int selected = ws != nullptr ? ws->selected_item : -1;
	const int hovered = ws != nullptr ? ws->hover_item : -1;
	const int first = ws != nullptr ? std::max(ws->scroll_row, 0) : 0;
	const int edge = w.string_data.has_edge ? w.string_data.edge : 0;
	int y = rect.top;
	for (int i = first; i < row_count; ++i) {
		if (y + row_h > rect.bottom) {
			break;
		}
		const mnu::RectEdges row{rect.left, y, rect.right, y + row_h};
		// The row style index: selection wins over hover
		// [orig: the per-item style dword +12, data set by the widget].
		// MULTI lists draw every row in the selected set with the
		// selection style.
		bool multi_selected = false;
		if (ws != nullptr) {
			for (const int32_t sel : ws->selected_items) {
				if (sel == i) {
					multi_selected = true;
					break;
				}
			}
		}
		int style = -1;
		if (i == selected || multi_selected) {
			style = kStateSelected;
		} else if (i == hovered) {
			style = kStateMouseover;
		}
		if (style >= 0 && node.items_states[style].present) {
			const StatePass &pass = node.items_states[style];
			// The row rect inflates -1 horizontally [orig: InflateRect -1,0].
			const mnu::RectEdges hi{ row.left + 1, row.top, row.right - 1, row.bottom };
			emit_state_pass(hi, s, pass);
		}
		if (runtime_rows) {
			const std::string &text = ws->items[static_cast<size_t>(i)];
			if (!text.empty()) {
				const int color_state = style >= 0 ? style : kStateDefault;
				emit_glyph_run(node, text, row.left + edge, row.top, s,
						node.colors[color_state], -1);
			}
		} else {
			const WidgetNode::ItemVisual &item =
					node.items[static_cast<size_t>(i)];
			if (item.kind == WidgetNode::ItemVisual::kText &&
					!item.text.empty()) {
				const int color_state = style >= 0 ? style : kStateDefault;
				emit_glyph_run(node, item.text, row.left + edge, row.top, s,
						node.colors[color_state], -1);
			}
		}
		y += row_h;
	}
}

// Spin up/down arrows: parent-relative child windows sized by the
// three-stage fallback over their appearance art
// [orig: CSpinListWnd_CreateUpDownChildren @ 0x64b8b0].
void MenuFrameCompiler::emit_spin_arrows(const WidgetNode &node,
		const mnu::RectEdges &rect, const WalkScale &s) {
	const mnu::Window &w = *node.window;
	const auto emit_arrow = [&](const mnu::SpinButton &btn,
									const StatePass &pass) {
		if (!btn.present || pass.texture < 0) {
			return;
		}
		const auto size = state_texture_size(pass);
		const mnu::RectEdges local = mnu::spin_button_rect(
				btn.position.has_left, btn.position.left, btn.position.has_top,
				btn.position.top, btn.position.has_right, btn.position.right,
				btn.position.has_bottom, btn.position.bottom, size.first, size.second);
		emit_state_texture(offset_rect(local, rect.left, rect.top), s, pass);
	};
	emit_arrow(w.spinup, node.spin_up);
	emit_arrow(w.spindown, node.spin_down);
}

// The cursor pass [orig: CUIScene_DrawScreensAndCursor @ 0x63bf60]: the
// hovered widget's inherited CURSOR texture, else the screen default, drawn
// LAST at the raw mouse position at native texture size, UNSCALED (the
// cursor never scales with the menu).
void MenuFrameCompiler::emit_cursor(const MenuFrameState &state) {
	if (!state.cursor_visible) {
		return;
	}
	int32_t slot = kMenuTexNone;
	for (const MenuWidgetState &ws : state.widgets) {
		if (ws.hovered && ws.index >= 0 &&
				ws.index < static_cast<int32_t>(nodes_.size())) {
			slot = nodes_[static_cast<size_t>(ws.index)].cursor;
			break;
		}
	}
	if (slot < 0) {
		slot = screen_cursor_;
	}
	if (slot < 0) {
		return;
	}
	const auto &size = texture_sizes_[static_cast<size_t>(slot)];
	if (size.first <= 0 || size.second <= 0) {
		return;
	}
	MenuQuad quad;
	quad.x0 = state.cursor_x;
	quad.y0 = state.cursor_y;
	quad.x1 = state.cursor_x + static_cast<float>(size.first);
	quad.y1 = state.cursor_y + static_cast<float>(size.second);
	quad.texture = slot;
	push_quad(quad);
}

// --- the walk ----------------------------------------------------------------

int MenuFrameCompiler::hit_walk(int index, int origin_x, int origin_y,
		const MenuFrameState &state, float mx, float my, float sx, float sy,
		int *io_hit) const {
	const WidgetNode &node = nodes_[static_cast<size_t>(index)];
	const mnu::Window &w = *node.window;
	const MenuWidgetState *ws = state_for(state, index);
	int next = index + 1;
	// The same shown gate as the draw walk; a hidden subtree never hits.
	bool shown = !w.hidden;
	if (ws != nullptr) {
		if (ws->hide) {
			shown = false;
		}
		if (ws->show) {
			shown = true;
		}
	}
	if (!shown) {
		for (size_t c = 0; c < w.children.size(); ++c) {
			next = skip_widget(next);
		}
		return next;
	}
	const mnu::RectEdges local = solve_rect(node, ws);
	const mnu::RectEdges rect = offset_rect(local, origin_x, origin_y);
	// Raw mouse against the SCALED rect (the same per-element truncation the
	// draw emits with).
	if (mx >= emit_x(rect.left, sx) && mx < emit_x(rect.right, sx) &&
			my >= emit_x(rect.top, sy) && my < emit_x(rect.bottom, sy)) {
		*io_hit = index; // later in draw order = front-most; the claim
	} else if (w.type == mnu::WindowType::SpinList &&
			spin_arrow_hit_(node, rect, mx, my, sx, sy) != 0) {
		// The SPINUP/SPINDOWN arrows are child WINDOWS carrying their own
		// rects in this same walk, and shipped menus author them OUTSIDE the
		// parent rect (mp.mnu GAME_TYPE: −18..−2 / 217..233 against a
		// 0..215 widget) — an arrow hit claims the spin widget exactly like
		// retail's child-window claim (D-MNU-16)
		// [orig: CSpinListWnd_CreateUpDownChildren @ 0x64b8b0].
		*io_hit = index;
	}
	for (size_t c = 0; c < w.children.size(); ++c) {
		next = hit_walk(next, rect.left, rect.top, state, mx, my, sx, sy,
				io_hit);
	}
	return next;
}

MenuFrameCompiler::MouseClaim MenuFrameCompiler::pump_mouse(
		MenuFrameState &io_state, float mouse_x, float mouse_y,
		bool button_down, float scale_x, float scale_y) {
	MouseClaim claim;
	if (screen_ == nullptr || nodes_.empty()) {
		return claim;
	}
	int hit = -1;
	hit_walk(0, 0, 0, io_state, mouse_x, mouse_y, scale_x, scale_y, &hit);
	for (MenuWidgetState &row : io_state.widgets) {
		row.hovered = false;
		row.pressed = false;
	}
	claim.hovered = hit;
	claim.cursor = screen_cursor_;
	if (hit < 0) {
		return claim;
	}
	const WidgetNode &node = nodes_[static_cast<size_t>(hit)];
	if (node.cursor != kMenuTexNone) {
		claim.cursor = node.cursor;
	}
	const MenuWidgetState *existing = state_for(io_state, hit);
	// A disabled claimant still owns the mouse (blocking widgets beneath)
	// but keeps visual state 1 — no hover/press write.
	if (existing != nullptr && existing->disabled) {
		return claim;
	}
	MenuWidgetState *row = nullptr;
	for (MenuWidgetState &candidate : io_state.widgets) {
		if (candidate.index == hit) {
			row = &candidate;
			break;
		}
	}
	if (row == nullptr) {
		MenuWidgetState fresh;
		fresh.index = hit;
		io_state.widgets.push_back(fresh);
		row = &io_state.widgets.back();
	}
	row->pressed = button_down;
	row->hovered = !button_down;
	return claim;
}

int MenuFrameCompiler::widget_index(const std::string &name) const {
	if (name.empty()) {
		return -1;
	}
	for (size_t i = 0; i < nodes_.size(); ++i) {
		const mnu::Window *w = nodes_[i].window;
		if (w == nullptr || w->name.size() != name.size()) {
			continue;
		}
		bool match = true;
		for (size_t c = 0; c < name.size(); ++c) {
			const char a = w->name[c];
			const char b = name[c];
			const char al = (a >= 'A' && a <= 'Z') ? char(a - 'A' + 'a') : a;
			const char bl = (b >= 'A' && b <= 'Z') ? char(b - 'A' + 'a') : b;
			if (al != bl) {
				match = false;
				break;
			}
		}
		if (match) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

int MenuFrameCompiler::skip_widget(int index) const {
	const WidgetNode &node = nodes_[static_cast<size_t>(index)];
	int next = index + 1;
	for (size_t c = 0; c < node.window->children.size(); ++c) {
		next = skip_widget(next);
	}
	return next;
}

// --- widget queries ----------------------------------------------------------

int MenuFrameCompiler::row_height_(const WidgetNode &node) const {
	// [orig: authored MIN_ITEM_HEIGHT wins, else the "W" measure —
	//  CListWnd_DrawItems @ 0x643f30 (D-MNU-8)]
	const mnu::Window &w = *node.window;
	// A combo's rows live in its nested LIST_BOX. Direct LIST/MULTI/LAN_LIST
	// syntax stores the sibling MIN_ITEM_HEIGHT in table_data.
	if (w.type == mnu::WindowType::Combo) {
		if (w.list_box.has_min_item_height && w.list_box.min_item_height >= 0) {
			return w.list_box.min_item_height;
		}
	} else if (w.table_data.has_min_item_height &&
			w.table_data.min_item_height >= 0) {
		return w.table_data.min_item_height;
	}
	int tw = 0;
	int row_h = 0;
	measure_text(node, "W", &tw, &row_h);
	return row_h;
}

void MenuFrameCompiler::table_row_heights_(const WidgetNode &node,
		int *header_height,
		int *body_row_height) const {
	int em_w = 0;
	int em_h = 0;
	measure_text(node, "W", &em_w, &em_h);
	if (header_height != nullptr) {
		*header_height = em_h;
	}
	if (body_row_height != nullptr) {
		const mnu::TableData &table = node.window->table_data;
		*body_row_height = table.has_min_item_height && table.min_item_height > 0
				? table.min_item_height
				: em_h;
	}
}

bool MenuFrameCompiler::widget_shown(
		int index, const MenuFrameState &state) const {
	return widget_shown_(index, state);
}

bool MenuFrameCompiler::widget_shown_(int index, const MenuFrameState &state) const {
	// The draw/hit walk's shown gate over the widget AND its ancestors.
	int i = index;
	while (i >= 0) {
		const WidgetNode &node = nodes_[static_cast<size_t>(i)];
		bool shown = !node.window->hidden;
		const MenuWidgetState *ws = state_for(state, i);
		if (ws != nullptr) {
			if (ws->hide) {
				shown = false;
			}
			if (ws->show) {
				shown = true;
			}
		}
		if (!shown) {
			return false;
		}
		i = node.parent;
	}
	return true;
}

int MenuFrameCompiler::widget_count() const {
	return static_cast<int>(nodes_.size());
}

std::string MenuFrameCompiler::widget_name(int index) const {
	if (index < 0 || index >= static_cast<int>(nodes_.size())) {
		return std::string();
	}
	return nodes_[static_cast<size_t>(index)].window->name;
}

int MenuFrameCompiler::widget_kind(int index) const {
	if (index < 0 || index >= static_cast<int>(nodes_.size())) {
		return -1;
	}
	return static_cast<int>(nodes_[static_cast<size_t>(index)].window->type);
}

std::string MenuFrameCompiler::widget_authored_text(int index) const {
	if (index < 0 || index >= static_cast<int>(nodes_.size())) {
		return std::string();
	}
	return widget_text(nodes_[static_cast<size_t>(index)], nullptr);
}

bool MenuFrameCompiler::widget_disabled(int index,
		const MenuFrameState &state) const {
	if (index < 0 || index >= static_cast<int>(nodes_.size())) {
		return false;
	}
	const WidgetNode &node = nodes_[static_cast<size_t>(index)];
	const MenuWidgetState *ws = state_for(state, index);
	return node.window->disabled || (ws != nullptr && ws->disabled);
}

bool MenuFrameCompiler::widget_edit_limits(int index, EditLimits *out) const {
	if (out == nullptr || index < 0 ||
			index >= static_cast<int>(nodes_.size())) {
		return false;
	}
	// [orig: the parsed edit constraints — read-only widget[194], numeric
	//  widget[196] with the [min widget[202], max widget[201]] range, max len
	//  widget[200]; edit_widget_insert_char @ 0x661ee0]
	const mnu::Window &w = *nodes_[static_cast<size_t>(index)].window;
	*out = EditLimits{};
	out->read_only = w.readonly;
	out->numeric = w.number;
	out->min_value = w.has_minval ? w.minval : 0;
	out->max_value = w.has_maxval ? w.maxval : 0;
	if (w.number && !w.has_minval && !w.has_maxval) {
		// NUMBER with no authored range: the range gate never rejects.
		out->min_value = LONG_MIN;
		out->max_value = LONG_MAX;
	} else if (w.number && !w.has_maxval) {
		out->max_value = LONG_MAX;
	} else if (w.number && !w.has_minval) {
		out->min_value = LONG_MIN;
	}
	out->max_len = w.has_maxchar ? w.maxchar : -1;
	return true;
}

bool MenuFrameCompiler::widget_rect(int index, const MenuFrameState &state,
		mnu::RectEdges *out) const {
	if (out == nullptr || index < 0 ||
			index >= static_cast<int>(nodes_.size())) {
		return false;
	}
	// absolute = own solved rect + every ancestor's solved origin (the same
	// accumulation the draw walk threads through origin_x/origin_y).
	mnu::RectEdges rect = solve_rect(nodes_[static_cast<size_t>(index)],
			state_for(state, index));
	int p = nodes_[static_cast<size_t>(index)].parent;
	while (p >= 0) {
		const mnu::RectEdges pr = solve_rect(nodes_[static_cast<size_t>(p)],
				state_for(state, p));
		rect = offset_rect(rect, pr.left, pr.top);
		p = nodes_[static_cast<size_t>(p)].parent;
	}
	*out = rect;
	return true;
}

int MenuFrameCompiler::item_count(int index, const MenuFrameState &state) const {
	if (index < 0 || index >= static_cast<int>(nodes_.size())) {
		return 0;
	}
	const WidgetNode &node = nodes_[static_cast<size_t>(index)];
	const MenuWidgetState *ws = state_for(state, index);
	if (ws != nullptr && ws->has_items) {
		return static_cast<int>(ws->items.size());
	}
	// A combo's selectable rows are its popup rows when the nested LIST_BOX
	// collection is authored (D-MNU-7/8).
	const mnu::Window &w = *node.window;
	if (w.type == mnu::WindowType::Combo && w.list_box.items.present) {
		return static_cast<int>(node.popup_items.size());
	}
	return static_cast<int>(node.items.size());
}

int MenuFrameCompiler::list_row_at(int index, const MenuFrameState &state,
		float mx, float my, float sx, float sy) const {
	mnu::RectEdges rect;
	if (!widget_rect(index, state, &rect)) {
		return -1;
	}
	const WidgetNode &node = nodes_[static_cast<size_t>(index)];
	const int row_h = row_height_(node);
	const int count = item_count(index, state);
	if (row_h <= 0 || count == 0) {
		return -1;
	}
	if (mx < emit_x(rect.left, sx) || mx >= emit_x(rect.right, sx)) {
		return -1;
	}
	const MenuWidgetState *ws = state_for(state, index);
	const int first = ws != nullptr ? std::max(ws->scroll_row, 0) : 0;
	const int visible = std::max((rect.bottom - rect.top) / row_h, 0);
	mnu::RectEdges scrollbar_rect;
	if (count > visible &&
			resolve_scrollbar_rect(node, ScrollbarKind::Embedded, rect, 0,
					rect.bottom - rect.top, 22, &scrollbar_rect) &&
			mx >= emit_x(scrollbar_rect.left, sx) &&
			mx < emit_x(scrollbar_rect.right, sx) &&
			my >= emit_x(scrollbar_rect.top, sy) &&
			my < emit_x(scrollbar_rect.bottom, sy)) {
		return -1;
	}
	int y = rect.top;
	for (int i = first; i < count; ++i) {
		if (y + row_h > rect.bottom) {
			break;
		}
		if (my >= emit_x(y, sy) && my < emit_x(y + row_h, sy)) {
			return i;
		}
		y += row_h;
	}
	return -1;
}

int MenuFrameCompiler::list_visible_rows(int index,
		const MenuFrameState &state) const {
	mnu::RectEdges rect;
	if (!widget_rect(index, state, &rect)) {
		return 0;
	}
	const int row_h = row_height_(nodes_[static_cast<size_t>(index)]);
	if (row_h <= 0) {
		return 0;
	}
	return (rect.bottom - rect.top) / row_h;
}

bool MenuFrameCompiler::combo_popup_rect(int index, const MenuFrameState &state,
		mnu::RectEdges *out) const {
	if (out == nullptr || index < 0 ||
			index >= static_cast<int>(nodes_.size())) {
		return false;
	}
	const mnu::Window &w = *nodes_[static_cast<size_t>(index)].window;
	if (!w.list_box.present) {
		return false;
	}
	mnu::RectEdges rect;
	if (!widget_rect(index, state, &rect)) {
		return false;
	}
	const mnu::RectEdges local = mnu::position_rect(
			w.list_box.position.has_left, w.list_box.position.left,
			w.list_box.position.has_top, w.list_box.position.top,
			w.list_box.position.has_right, w.list_box.position.right,
			w.list_box.position.has_bottom, w.list_box.position.bottom, 0, 0);
	*out = offset_rect(local, rect.left, rect.top);
	return true;
}

bool MenuFrameCompiler::combo_popup_contains(int index,
		const MenuFrameState &state, float mx, float my, float sx,
		float sy) const {
	mnu::RectEdges popup;
	if (!combo_popup_rect(index, state, &popup)) {
		return false;
	}
	return mx >= emit_x(popup.left, sx) && mx < emit_x(popup.right, sx) &&
			my >= emit_x(popup.top, sy) && my < emit_x(popup.bottom, sy);
}

int MenuFrameCompiler::combo_popup_row_at(int index,
		const MenuFrameState &state, float mx, float my, float sx,
		float sy) const {
	mnu::RectEdges popup;
	if (!combo_popup_rect(index, state, &popup)) {
		return -1;
	}
	const WidgetNode &node = nodes_[static_cast<size_t>(index)];
	const int row_h = row_height_(node);
	const int count = item_count(index, state);
	if (row_h <= 0 || count == 0) {
		return -1;
	}
	if (mx < emit_x(popup.left, sx) || mx >= emit_x(popup.right, sx)) {
		return -1;
	}
	const MenuWidgetState *ws = state_for(state, index);
	const int first = ws != nullptr ? std::max(ws->scroll_row, 0) : 0;
	const int visible = std::max((popup.bottom - popup.top) / row_h, 0);
	mnu::RectEdges scrollbar_rect;
	if (count > visible &&
			resolve_scrollbar_rect(node, ScrollbarKind::Popup, popup, 0,
					popup.bottom - popup.top, 22, &scrollbar_rect) &&
			mx >= emit_x(scrollbar_rect.left, sx) &&
			mx < emit_x(scrollbar_rect.right, sx) &&
			my >= emit_x(scrollbar_rect.top, sy) &&
			my < emit_x(scrollbar_rect.bottom, sy)) {
		// The original routes the child scrollbar before the list rows.
		// Part interaction is deferred, but the covered strip must not select
		// a row. [orig: CListWnd child walk @ 0x643f30]
		return -1;
	}
	int y = popup.top;
	for (int i = first; i < count; ++i) {
		if (y + row_h > popup.bottom) {
			break;
		}
		if (my >= emit_x(y, sy) && my < emit_x(y + row_h, sy)) {
			return i;
		}
		y += row_h;
	}
	return -1;
}

// Shared arrow hit over the widget's ABSOLUTE rect: 0 none, 1 up, 2 down —
// the pump's claim walk and the driver's press routing use the same rects
// [orig: CSpinListWnd_CreateUpDownChildren @ 0x64b8b0 child rects].
int MenuFrameCompiler::spin_arrow_hit_(const WidgetNode &node,
		const mnu::RectEdges &rect, float mx, float my, float sx,
		float sy) const {
	const mnu::Window &w = *node.window;
	const auto arrow_hit = [&](const mnu::SpinButton &btn,
								   const StatePass &pass) {
		if (!btn.present || pass.texture < 0) {
			return false;
		}
		const auto size = state_texture_size(pass);
		const mnu::RectEdges local = mnu::spin_button_rect(
				btn.position.has_left, btn.position.left, btn.position.has_top,
				btn.position.top, btn.position.has_right, btn.position.right,
				btn.position.has_bottom, btn.position.bottom, size.first,
				size.second);
		const mnu::RectEdges abs = offset_rect(local, rect.left, rect.top);
		return mx >= emit_x(abs.left, sx) && mx < emit_x(abs.right, sx) &&
				my >= emit_x(abs.top, sy) && my < emit_x(abs.bottom, sy);
	};
	if (arrow_hit(w.spinup, node.spin_up)) {
		return 1;
	}
	if (arrow_hit(w.spindown, node.spin_down)) {
		return 2;
	}
	return 0;
}

int MenuFrameCompiler::spin_arrow_at(int index, const MenuFrameState &state,
		float mx, float my, float sx, float sy) const {
	if (index < 0 || index >= static_cast<int>(nodes_.size())) {
		return 0;
	}
	mnu::RectEdges rect;
	if (!widget_rect(index, state, &rect)) {
		return 0;
	}
	const WidgetNode &node = nodes_[static_cast<size_t>(index)];
	return spin_arrow_hit_(node, rect, mx, my, sx, sy);
}

int MenuFrameCompiler::table_row_at(int index, const MenuFrameState &state,
		float mx, float my, float sx, float sy) const {
	if (index < 0 || index >= static_cast<int>(nodes_.size())) {
		return -1;
	}
	const WidgetNode &node = nodes_[static_cast<size_t>(index)];
	const mnu::TableData &table = node.window->table_data;
	if (table.column.headers.empty()) {
		return -1;
	}
	const MenuWidgetState *ws = state_for(state, index);
	if (ws == nullptr || ws->table_rows.empty()) {
		return -1;
	}
	mnu::RectEdges rect;
	if (!widget_rect(index, state, &rect)) {
		return -1;
	}
	int header_h = 0;
	int row_h = 0;
	table_row_heights_(node, &header_h, &row_h);
	if (row_h <= 0) {
		return -1;
	}
	if (mx < emit_x(rect.left, sx) || mx >= emit_x(rect.right, sx)) {
		return -1;
	}
	// Retail stores at least one visible row even when the body is shorter
	// than a full row, then derives the child scrollbar page/range from it.
	// [orig: CTableWnd_RecalcLayout @ 0x63f1a0, clamp @ 0x63f276]
	const int visible =
			row_h > 0 ? std::max((rect.bottom - rect.top - header_h) / row_h, 1) : 0;
	mnu::RectEdges scrollbar_rect;
	if (static_cast<int>(ws->table_rows.size()) > visible &&
			resolve_scrollbar_rect(node, ScrollbarKind::Embedded, rect, 0,
					rect.bottom - rect.top, 22, &scrollbar_rect) &&
			mx >= emit_x(scrollbar_rect.left, sx) &&
			mx < emit_x(scrollbar_rect.right, sx) &&
			my >= emit_x(scrollbar_rect.top, sy) &&
			my < emit_x(scrollbar_rect.bottom, sy)) {
		return -1;
	}
	const int first = ws->scroll_row > 0 ? ws->scroll_row : 0;
	int y = rect.top + header_h;
	for (size_t r = static_cast<size_t>(first); r < ws->table_rows.size(); ++r) {
		if (y + row_h > rect.bottom) {
			break;
		}
		if (my >= emit_x(y, sy) && my < emit_x(y + row_h, sy)) {
			return static_cast<int>(r);
		}
		y += row_h;
	}
	return -1;
}

bool MenuFrameCompiler::multiline_line_counts(int index,
		const MenuFrameState &state, int *fit_lines, int *total_lines) const {
	if (fit_lines == nullptr || total_lines == nullptr || index < 0 ||
			index >= static_cast<int>(nodes_.size())) {
		return false;
	}
	*fit_lines = 0;
	*total_lines = 0;
	const WidgetNode &node = nodes_[static_cast<size_t>(index)];
	const mnu::Window &w = *node.window;
	const MenuWidgetState *ws = state_for(state, index);
	std::string text = widget_text(node, ws);
	if (w.password) {
		text.assign(text.size(), '*');
	}
	const fnt_font_t *font = font_for(node);
	if (font == nullptr || text.empty()) {
		return true;
	}
	hud::GameFont gf;
	gf.set_font(font);
	const mnu::RectEdges rect = solve_rect(node, ws);
	int wrap_w = rect.right - rect.left;
	if (wrap_w <= 0) {
		wrap_w = 0x10000;
	}
	const int rect_h = rect.bottom - rect.top;
	// The measure twin replays the drawer's break rules at scale 1.0
	// [orig: font_cache_count_wrapped_lines @ 0x653b90].
	std::vector<int> heights;
	int line_start = 0;
	int last_space = 0;
	const int len = static_cast<int>(text.size());
	int i = 0;
	while (true) {
		const char c = i < len ? text[static_cast<size_t>(i)] : '\0';
		if (c == ' ') {
			last_space = i;
		}
		int accum_w = 0;
		int accum_h = 0;
		if (i >= line_start) {
			gf.measure(text.substr(static_cast<size_t>(line_start),
								 static_cast<size_t>(i - line_start) +
										 (c != '\0' ? 1u : 0u))
							   .c_str(),
					1.0f, 1.0f, &accum_w, &accum_h);
		}
		int break_at;
		int next;
		if (accum_w <= wrap_w) {
			if (c != '\n' && c != '\0') {
				++i;
				continue;
			}
			break_at = i;
			next = i + 1;
		} else if (last_space != 0) {
			break_at = last_space;
			next = last_space + 1;
		} else {
			break_at = i;
			next = i <= line_start ? line_start + 1 : i;
		}
		int lw = 0;
		int lh = 0;
		gf.measure(text.substr(static_cast<size_t>(line_start),
							 static_cast<size_t>(break_at - line_start))
						   .c_str(),
				1.0f, 1.0f, &lw, &lh);
		if (lh <= 0) {
			int tw = 0;
			gf.measure("W", 1.0f, 1.0f, &tw, &lh);
		}
		heights.push_back(lh);
		if (break_at >= len) {
			break;
		}
		last_space = 0;
		line_start = next;
		i = next;
	}
	const int total = static_cast<int>(heights.size());
	*total_lines = total;
	// fit = the last n with accumH(lines 1..n-1) + 2*h_n <= rectH; the final
	// line only needs accumH(all but last) <= rectH [orig: @ 0x653b90].
	int accum_h = 0;
	int fit = 0;
	for (int n = 1; n <= total; ++n) {
		const int h_n = heights[static_cast<size_t>(n - 1)];
		if (n == total) {
			if (accum_h <= rect_h) {
				fit = total;
			}
			break;
		}
		if (accum_h + 2 * h_n <= rect_h) {
			fit = n;
		}
		accum_h += h_n;
	}
	*fit_lines = fit;
	return true;
}

int MenuFrameCompiler::hit_widget(const MenuFrameState &state, float mx,
		float my, float sx, float sy) const {
	if (screen_ == nullptr || nodes_.empty()) {
		return -1;
	}
	int hit = -1;
	hit_walk(0, 0, 0, state, mx, my, sx, sy, &hit);
	return hit;
}

int MenuFrameCompiler::hotkey_widget(const std::string &key, bool virtual_key,
		const MenuFrameState &state) const {
	if (key.empty() || nodes_.empty()) {
		return -1;
	}
	// The accelerator scan: pre-order over the shown tree — a hidden subtree
	// never matches (a hidden BACK must not eat ESC); VIRTUAL rows and
	// character rows are separate namespaces, and VK_RETURN/VK_ENTER are
	// interchangeable (the only virtual keys shipped menus author are
	// VK_ESCAPE/VK_RETURN) [orig: the screen hotkey registration
	// CUIWidget_AddScreenHotkey @ 0x5674a8 family; the Control-tree port's
	// find_hotkey_target semantics, docs/mnu/menu-re.md "Hotkeys"].
	const auto normalize_vk = [](const std::string &value) {
		std::string folded = opennova::strutil::to_lower(value);
		if (folded == "vk_enter") {
			folded = "vk_return";
		}
		return folded;
	};
	const std::string want = virtual_key
			? normalize_vk(key)
			: opennova::strutil::to_lower(key);
	int idx = 0;
	const int n = static_cast<int>(nodes_.size());
	while (idx < n) {
		const WidgetNode &node = nodes_[static_cast<size_t>(idx)];
		const mnu::Window &w = *node.window;
		bool shown = !w.hidden;
		const MenuWidgetState *ws = state_for(state, idx);
		if (ws != nullptr) {
			if (ws->hide) {
				shown = false;
			}
			if (ws->show) {
				shown = true;
			}
		}
		if (!shown) {
			idx = skip_widget(idx);
			continue;
		}
		for (const mnu::Hotkey &hk : w.hotkeys) {
			if (hk.virtual_key != virtual_key) {
				continue;
			}
			const std::string have = virtual_key
					? normalize_vk(hk.value)
					: opennova::strutil::to_lower(hk.value);
			if (have == want) {
				return idx;
			}
		}
		++idx;
	}
	return -1;
}

int MenuFrameCompiler::walk_widget(int index, int origin_x, int origin_y,
		const MenuFrameState &state, const WalkScale &s) {
	const WidgetNode &node = nodes_[static_cast<size_t>(index)];
	const mnu::Window &w = *node.window;
	const MenuWidgetState *ws = state_for(state, index);
	int next = index + 1;
	// The shown gate [orig: every Draw impl early-outs on the shown flag
	// +224]. A hidden widget's subtree still consumes its indices.
	bool shown = !w.hidden;
	if (ws != nullptr) {
		if (ws->hide) {
			shown = false;
		}
		if (ws->show) {
			shown = true;
		}
	}
	if (!shown) {
		for (size_t c = 0; c < w.children.size(); ++c) {
			next = skip_widget(next);
		}
		return next;
	}
	const mnu::RectEdges local = solve_rect(node, ws);
	const mnu::RectEdges rect = offset_rect(local, origin_x, origin_y);
	const int pump = pump_visual_state(w, ws);
	const bool checked =
			ws != nullptr && ws->has_checked ? ws->checked : w.checked;
	++draw_list_.widgets_drawn;
	switch (w.type) {
		case mnu::WindowType::Static:
		case mnu::WindowType::Label:
		case mnu::WindowType::Button:
		case mnu::WindowType::Goto: {
			// [orig: CStaticWnd_Render @ 0x657b10 — frame -> appearance ->
			//  text -> children]
			if (w.draw_frame) {
				emit_frame(node, rect, s);
			}
			emit_appearance(node, rect, s,
					appearance_state_with_fallback(node, pump));
			emit_widget_text(node, rect, s, pump, ws, -1);
			break;
		}
		case mnu::WindowType::Radio: {
			// [orig: CRadioWnd_Render @ 0x656e20 — checked forces state 3
			//  around the WHOLE static render: appearance AND label colors]
			const int visual = checked ? kStateSelected : pump;
			if (w.draw_frame) {
				emit_frame(node, rect, s);
			}
			emit_appearance(node, rect, s,
					appearance_state_with_fallback(node, visual));
			emit_widget_text(node, rect, s, visual, ws, -1);
			break;
		}
		case mnu::WindowType::CheckBox: {
			// [orig: CCheckWnd_Render @ 0x64ae20 — checked forces slot 3
			//  DIRECTLY (no availability fallback: an unauthored selected
			//  slot draws nothing); the label uses the RAW pump state]
			if (w.draw_frame) {
				emit_frame(node, rect, s);
			}
			const int slot = checked
					? kStateSelected
					: appearance_state_with_fallback(node, pump);
			emit_appearance(node, rect, s, slot);
			emit_checkbox_label(node, rect, s, pump, ws);
			break;
		}
		case mnu::WindowType::Edit: {
			// [orig: CEditWnd_Render @ 0x6619e0 — focus (non-readonly)
			//  forces state 2 for appearance AND colors]
			const bool focused = ws != nullptr && ws->focused && !w.readonly;
			const int visual = focused ? kStateMouseover : pump;
			if (w.draw_frame) {
				emit_frame(node, rect, s);
			}
			emit_appearance(node, rect, s,
					appearance_state_with_fallback(node, visual));
			emit_edit(index, node, rect, s, visual, state, ws);
			break;
		}
		case mnu::WindowType::MultilineEdit: {
			// [orig: CMEditWnd_Render @ 0x6608e0 — frame -> appearance for
			//  the RAW pump state (no focus forcing, unlike the single-line
			//  sibling) -> the wrapped drawer -> children]
			if (w.draw_frame) {
				emit_frame(node, rect, s);
			}
			emit_appearance(node, rect, s,
					appearance_state_with_fallback(node, pump));
			emit_multiline_edit(node, rect, s, pump, state, ws);
			int fit_lines = 0;
			int total_lines = 0;
			multiline_line_counts(index, state, &fit_lines, &total_lines);
			mnu::RectEdges scrollbar_rect;
			if (total_lines > fit_lines &&
					resolve_scrollbar_rect(node, ScrollbarKind::Embedded, rect, 0,
							rect.bottom - rect.top, 22, &scrollbar_rect)) {
				emit_scrollbar(node, ScrollbarKind::Embedded, scrollbar_rect, s, 0,
						std::max(total_lines - fit_lines, 0), fit_lines - 1,
						ws != nullptr ? ws->scroll_row : 0, kStateDefault);
			}
			break;
		}
		case mnu::WindowType::SpinList: {
			// [orig: CSpinListWnd_Render @ 0x64b220 + the arrow child
			//  windows @ 0x64b8b0]
			if (w.draw_frame) {
				emit_frame(node, rect, s);
			}
			emit_appearance(node, rect, s,
					appearance_state_with_fallback(node, pump));
			emit_item_cell(node, rect, s, pump, ws);
			emit_spin_arrows(node, rect, s);
			break;
		}
		case mnu::WindowType::List:
		case mnu::WindowType::Multi:
		case mnu::WindowType::LanList: {
			// [orig: CListWnd_DrawItems @ 0x643f30 — frame -> appearance ->
			//  rows -> children]
			if (w.draw_frame) {
				emit_frame(node, rect, s);
			}
			emit_appearance(node, rect, s,
					appearance_state_with_fallback(node, pump));
			const int row_height = row_height_(node);
			const int row_count = ws != nullptr && ws->has_items
					? static_cast<int>(ws->items.size())
					: static_cast<int>(node.items.size());
			const int visible_rows =
					row_height > 0 ? std::max((rect.bottom - rect.top) / row_height, 0) : 0;
			mnu::RectEdges scrollbar_rect;
			const bool show_scrollbar =
					row_count > visible_rows &&
					resolve_scrollbar_rect(node, ScrollbarKind::Embedded, rect, 0,
							rect.bottom - rect.top, 22, &scrollbar_rect);
			emit_list_rows(node, rect, s, ws);
			if (show_scrollbar) {
				emit_scrollbar(node, ScrollbarKind::Embedded, scrollbar_rect, s, 0,
						std::max(row_count - visible_rows, 0), visible_rows - 1,
						ws != nullptr ? ws->scroll_row : 0, kStateDefault);
			}
			break;
		}
		case mnu::WindowType::Combo: {
			// [orig: CComboWnd_Render @ 0x65bfd0 — frame, appearance, then the
			//  LABEL temporarily swapped to the embedded list's SELECTED row
			//  text (@ 0x65c05b..0x65c083: this[183] = row_text(list,
			//  selected_row(list)); CStaticWnd_DrawLabel; restore) — the
			//  closed face IS the list selection, text-only, laid out by the
			//  combo's OWN STRING block. Shipped options.mnu authors rows only
			//  inside <LIST_BOX>, so a face reading widget-level ITEMS alone
			//  rendered blank (D-MNU-15's runtime-rows fix carried the same
			//  root cause for authored rows).]
			if (w.draw_frame) {
				emit_frame(node, rect, s);
			}
			emit_appearance(node, rect, s,
					appearance_state_with_fallback(node, pump));
			const std::string face = combo_face_text(node, ws);
			if (!face.empty()) {
				emit_widget_text(node, rect, s, pump, ws, -1, &face);
			} else {
				emit_widget_text(node, rect, s, pump, ws, -1);
			}
			if (ws != nullptr && ws->popup_open) {
				// The open dropdown is DEFERRED to the post-walk overlay pass:
				// retail's own tree order (options.mnu authors WATERQUALITY
				// before the rows its 80px popup covers) draws the popup
				// inline per the witnessed walk, yet renders it visually on
				// top — the topmost mechanism is unwalked. The reimpl hosts
				// open popups menu-top by decision (D-MNU-12), as the
				// Control-tree overlay did.
				deferred_popups_.push_back(index);
			}
			break;
		}
		case mnu::WindowType::Scroll: {
			// The track COLOR/OUTLINE use the full widget, its IMAGE uses the
			// middle span, and the two arrow children plus shuttle share this rect.
			// [orig: CScrollWnd_Render @ 0x64c5c0; scroll COLOR sink @ 0x64ce70;
			// scroll IMAGE sink @ 0x64cf70; CUIScrollbar_CalcThumbRect
			// @ 0x64cba0]
			if (w.draw_frame) {
				emit_frame(node, rect, s);
			}
			mnu::RectEdges scrollbar_rect;
			if (resolve_scrollbar_rect(node, ScrollbarKind::Standalone, rect, 0, 0, 0,
						&scrollbar_rect)) {
				const int range_min =
						ws != nullptr && ws->has_scroll_range ? ws->scroll_min : 0;
				const int range_max =
						ws != nullptr && ws->has_scroll_range ? ws->scroll_max : 0;
				const int page =
						ws != nullptr && ws->has_scroll_range ? ws->scroll_page : 10;
				const int value =
						ws != nullptr && ws->has_scroll_range ? ws->scroll_value : 0;
				emit_scrollbar(node, ScrollbarKind::Standalone, scrollbar_rect, s,
						range_min, range_max, page, value,
						appearance_state_with_fallback(node, pump));
			}
			break;
		}
		case mnu::WindowType::Table: {
			// [orig: CUITable_Render @ 0x6411d0 — frame -> appearance ->
			//  header + rule dividers -> data rows -> children]
			if (w.draw_frame) {
				emit_frame(node, rect, s);
			}
			emit_appearance(node, rect, s,
					appearance_state_with_fallback(node, pump));
			emit_table(index, node, rect, s, pump, ws);
			int header_height = 0;
			int row_height = 0;
			table_row_heights_(node, &header_height, &row_height);
			const int body_height = std::max(rect.bottom - rect.top - header_height, 0);
			const int visible_rows =
					row_height > 0 ? std::max(body_height / row_height, 1) : 0;
			const int row_count =
					ws != nullptr ? static_cast<int>(ws->table_rows.size()) : 0;
			mnu::RectEdges scrollbar_rect;
			if (row_count > visible_rows &&
					resolve_scrollbar_rect(node, ScrollbarKind::Embedded, rect, 0,
							rect.bottom - rect.top, 22, &scrollbar_rect)) {
				emit_scrollbar(node, ScrollbarKind::Embedded, scrollbar_rect, s, 0,
						std::max(row_count - visible_rows, 0), visible_rows - 1,
						ws != nullptr ? ws->scroll_row : 0, kStateDefault);
			}
			break;
		}
		case mnu::WindowType::Marquee: {
			// [orig: CMarqueeWnd_Render @ 0x65cf90 — frame -> appearance ->
			//  the credits scroller -> children]
			if (w.draw_frame) {
				emit_frame(node, rect, s);
			}
			emit_appearance(node, rect, s,
					appearance_state_with_fallback(node, pump));
			emit_marquee(index, node, rect, s, state, ws);
			break;
		}
		default: {
			// [orig: CUIElement_Draw @ 0x64a8a0 — appearance BEFORE frame
			//  for generic containers; the RADIOEDIT interior stays deferred
			//  (D-MNU-13)]
			emit_appearance(node, rect, s,
					appearance_state_with_fallback(node, pump));
			if (w.draw_frame) {
				emit_frame(node, rect, s);
			}
			break;
		}
	}
	// Children in authored array order [orig: the forward child walk — later
	// siblings paint over earlier ones].
	for (size_t c = 0; c < w.children.size(); ++c) {
		next = walk_widget(next, rect.left, rect.top, state, s);
	}
	return next;
}

// The witnessed table interior [orig: CUITable_Render @ 0x6411d0; the full
// walk: docs/mnu/menu-re.md "Table render"]. The compiled path draws the
// header labels, the character-profiled taper dividers, and the seeded data
// rows as text cells. Image/substitution/custom cells and per-row appearance
// records are deferred with the Control-tree renderer (D-MNU-13 follow-up);
// no shipped .mnu drives them through the compiled path yet.
void MenuFrameCompiler::emit_table(int index, const WidgetNode &node,
		const mnu::RectEdges &rect, const WalkScale &s, int visual,
		const MenuWidgetState *ws) {
	(void)index;
	const mnu::TableData &table = node.window->table_data;
	const std::vector<mnu::TableHeader> &headers = table.column.headers;
	if (headers.empty()) {
		return;
	}
	// The "W" measure supplies the default row/header heights [orig: the
	// font_cache_measure_text_default("W") probe; authored min_item_height
	// wins when present].
	int header_h = 0;
	int row_h = 0;
	table_row_heights_(node, &header_h, &row_h);
	const int gap = table.column.has_spacing ? table.column.spacing : 0;
	const uint32_t color = node.colors[static_cast<size_t>(
			visual >= 0 && visual < 4 ? visual : 0)];
	// Header labels + the taper rule divider under each column with >16px of
	// headroom [orig: the header walk; divider color 0xFF7F7F7F]. The rule
	// PROFILE string is not authored in the XML model (the runtime sets it),
	// so the compiled divider draws the plain full-taper line row.
	int x = rect.left;
	for (const mnu::TableHeader &h : headers) {
		const int width = h.has_width ? h.width : 0;
		if (width <= 0) {
			continue;
		}
		const std::string label = resolve_text_value(h.type, h.text);
		int text_w = 0;
		int text_h = 0;
		measure_text(node, label, &text_w, &text_h);
		int tx = x;
		if (h.justify == "CENTER") {
			tx = x + (width - text_w) / 2;
		} else if (h.justify == "RIGHT") {
			tx = x + width - text_w;
		}
		emit_glyph_run(node, label, tx, rect.top, s, color, -1);
		if (width - text_w > 16) {
			// One divider segment centered in the headroom band [orig:
			// draw_rule_line @ 0x6410a0 — 0xFF7F7F7F].
			const int seg_left = x + text_w + 1;
			const int seg_right = x + width - 1;
			const int seg_y = rect.top + header_h / 2;
			push_line(MenuLine{ emit_x(seg_left, s.x), emit_x(seg_y, s.y),
					emit_x(seg_right, s.x), emit_x(seg_y, s.y),
					0xFF7F7F7Fu });
		}
		x += width + gap;
	}
	if (ws == nullptr || ws->table_rows.empty()) {
		return;
	}
	// Data rows: the scroll window is first-visible + as many rows as fit
	// below the header [orig: the visible-row walk].
	const int first = ws->scroll_row > 0 ? ws->scroll_row : 0;
	int y = rect.top + header_h;
	for (size_t r = static_cast<size_t>(first); r < ws->table_rows.size();
			++r) {
		if (y + row_h > rect.bottom) {
			break;
		}
		const std::vector<std::string> &row = ws->table_rows[r];
		x = rect.left;
		for (size_t c = 0; c < headers.size(); ++c) {
			const mnu::TableHeader &h = headers[c];
			const int width = h.has_width ? h.width : 0;
			if (width <= 0) {
				continue;
			}
			if (c < row.size() && !row[c].empty()) {
				emit_glyph_run(node, row[c], x, y, s, color, -1);
			}
			x += width + gap;
		}
		y += row_h;
	}
}

// The witnessed credits scroller [orig: CMarqueeWnd_Render @ 0x65cf90 ->
// render_scrolling_credits @ 0x65ca00; docs/mnu/menu-re.md "Marquee credits
// scroller"]: per-frame scroll off the widget's rate with a whole-roll reset
// when the last line passes the top. The compiled path drives text lines
// (the shipped credits datasource is text); image nodes and the 50px edge
// fade band ride the D-MNU-13 follow-up with the Control-tree renderer.
void MenuFrameCompiler::emit_marquee(int index, const WidgetNode &node,
		const mnu::RectEdges &rect, const WalkScale &s,
		const MenuFrameState &frame, const MenuWidgetState *ws) {
	if (ws == nullptr || ws->marquee_lines.empty()) {
		// No seeded roll: the static-family label draw keeps the authored
		// STRING visible (the pre-cutover behavior).
		emit_widget_text(node, rect, s, kStateDefault, ws, -1);
		return;
	}
	int em_w = 0;
	int line_h = 0;
	measure_text(node, "W", &em_w, &line_h);
	if (line_h <= 0) {
		line_h = 12;
	}
	MarqueeScroll &roll = marquee_scroll_[index];
	if (!roll.valid || ws->marquee_reset) {
		roll.offset = 0.0;
		roll.last_ms = frame.time_ms;
		roll.valid = true;
	}
	// The scroll rate in design pixels/second; the original stores a
	// per-frame rate at this+0x2DC — one line every ~1.5 s at the menu tick
	// maps to line_h / 1.5 px/s.
	const double rate_px_per_ms =
			static_cast<double>(line_h) / 1500.0;
	const uint32_t now = frame.time_ms;
	if (now > roll.last_ms) {
		roll.offset += rate_px_per_ms * static_cast<double>(now - roll.last_ms);
	}
	roll.last_ms = now;
	const int total_h =
			static_cast<int>(ws->marquee_lines.size()) * line_h;
	// Whole-roll reset when the LAST line passes the top.
	if (roll.offset > static_cast<double>(total_h)) {
		roll.offset = 0.0;
	}
	const uint32_t color = node.colors[0];
	int y = rect.bottom - static_cast<int>(roll.offset);
	for (const std::string &line : ws->marquee_lines) {
		if (y + line_h > rect.top && y < rect.bottom && !line.empty()) {
			int text_w = 0;
			int text_h = 0;
			measure_text(node, line, &text_w, &text_h);
			const int tx =
					rect.left + (rect.right - rect.left - text_w) / 2;
			emit_glyph_run(node, line, tx, y, s, color, -1);
		}
		y += line_h;
	}
}

const MenuDrawList &MenuFrameCompiler::compile(const MenuFrameState &state,
		float scale_x, float scale_y) {
	draw_list_.quads.clear();
	draw_list_.lines.clear();
	draw_list_.glyphs.clear();
	draw_list_.underlines.clear();
	draw_list_.font_runs.clear();
	draw_list_.draw_ops.clear();
	draw_list_.widgets_drawn = 0;
	if (screen_ == nullptr || nodes_.empty()) {
		return draw_list_;
	}
	WalkScale s;
	s.x = scale_x;
	s.y = scale_y;
	deferred_popups_.clear();
	walk_widget(0, 0, 0, state, s);
	// The open-dropdown overlay pass (D-MNU-12): popups collected during the
	// walk paint after every widget, before the cursor.
	for (int index : deferred_popups_) {
		const WidgetNode &node = nodes_[static_cast<size_t>(index)];
		mnu::RectEdges rect;
		if (!widget_rect(index, state, &rect)) {
			continue;
		}
		const MenuWidgetState *ws = state_for(state, index);
		if (ws != nullptr && ws->popup_open) {
			emit_combo_popup(node, rect, s, ws);
		}
	}
	deferred_popups_.clear();
	emit_cursor(state);
	return draw_list_;
}

} // namespace opennova::menu
