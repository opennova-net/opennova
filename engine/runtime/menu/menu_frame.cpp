// The menu frame compiler — the witnessed CWnd Draw walk over a parsed .mnu
// screen, structural translation onto the typed draw list.
// [orig: Menu_RenderFrame @ 0x54b7c0 -> CUIScene_DrawScreensAndCursor
//  @ 0x63bf60 -> the Draw vtable family, cited per pass below]
// Witness record: docs/mnu/menu-re.md ("Widget render dispatch").

#include <runtime/menu/menu_frame_internal.h>
#include <runtime/menu/menu_screen_inputs.h>

#include <base/io/strutil.h>
#include <formats/mns/mns.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

using namespace opennova::fnt;

namespace opennova::menu {

namespace {

bool iequals(const std::string &a, const char *b) {
	return opennova::strutil::iequals(a, b);
}

// The texture-cache key of an IMAGE load: the name and the FLAGS word it was
// loaded with [orig: CTextureManager_LoadOrFindTexture @ 0x654980 — stricmp on
// the name, then the subtype compare]. The FLAGS text stands for its word (the
// token table sub_646CC0 @ 0x646cc0 is not ported).
std::string texture_load_key(const std::string &name, const std::string &flags) {
	return opennova::strutil::to_lower(name) + '\n' + opennova::strutil::to_lower(flags);
}

// The JUSTIFY and VJUSTIFY of a list's ITEMS block or of one of its ITEMs over the layout so far
// [orig: CListWnd_ParseXMLDefinition @ 0x6457ef..0x645907 (ITEMS), @ 0x6459b2..0x645b62 (an
// ITEM)]: JUSTIFY LEFT 0 / CENTER 1 / RIGHT 2, another token read by wcstol as the x offset, the
// justification kept; VJUSTIFY TOP 0 / CENTER 0x10 / BOTTOM 0x20, another the y offset. An empty
// value is the reader's to leave out (docs/mnu/menu-re.md "Crash and hang cases").
void list_row_justify(const std::string &justify, const std::string &vjustify, int *io_justify,
		int *io_vjustify, int *io_x, int *io_y) {
	if (!justify.empty()) {
		if (iequals(justify, "LEFT")) {
			*io_justify = 0;
		} else if (iequals(justify, "CENTER")) {
			*io_justify = 1;
		} else if (iequals(justify, "RIGHT")) {
			*io_justify = 2;
		} else {
			*io_x = static_cast<int>(std::strtol(justify.c_str(), nullptr, 10));
		}
	}
	if (!vjustify.empty()) {
		if (iequals(vjustify, "TOP")) {
			*io_vjustify = 0;
		} else if (iequals(vjustify, "CENTER")) {
			*io_vjustify = 0x10;
		} else if (iequals(vjustify, "BOTTOM")) {
			*io_vjustify = 0x20;
		} else {
			*io_y = static_cast<int>(std::strtol(vjustify.c_str(), nullptr, 10));
		}
	}
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

void MenuFrameCompiler::set_text_tables(const MenuTextTables *tables) {
	text_tables_ = tables;
}

void MenuFrameCompiler::register_font(const std::string &name,
		const fnt_font_t *font) {
	if (font != nullptr) {
		registered_fonts_[opennova::strutil::to_lower(name)] = font;
	}
}

void MenuFrameCompiler::clear_registered_fonts() {
	registered_fonts_.clear();
	font_names_.assign(1, std::string());
	fonts_.assign(1, nullptr);
}

// Whole-value %VAR% resolution, case-insensitive, unresolved stays literal; the name
// ends where the game's expansion ends it (mns::variable_reference_at)
// [orig: NapiXML_ExpandVariablesInText @ 0x63a000; ADR 0005 per-field form].
std::string MenuFrameCompiler::resolve_var(const std::string &value) const {
	if (!mns::is_variable_reference(value)) {
		return value;
	}
	const std::string key = opennova::strutil::to_lower(mns::variable_name(value));
	const auto it = style_vars_.find(key);
	return it == style_vars_.end() ? value : it->second;
}

// A FONT color: wcstoul(text, 16) [orig: the FONT arm @ 0x648d14..0x648e64]; the
// text sink forces alpha 0xFF (menus never pass the 0x10000 keep-alpha flag)
// [orig: CFontCache_DrawTextScaled @ 0x653170]. An absent or unparsed value
// keeps the zeroed ctor field: opaque black.
uint32_t MenuFrameCompiler::resolve_text_color(const std::string &value) const {
	return mnu::color_value(resolve_var(value)) | 0xFF000000u;
}

// String content resolution: type=="id" looks the value up through the widget's
// string table [orig: CUIStringTable_LookupString @ 0x6527c0 with
// CWnd_GetInheritedTextRsrc @ 0x646AB0]; a miss or a literal type keeps the raw
// text. The first {hot} marker never draws and its following byte identifies the
// button accelerator [orig: CButtonWnd_SetLabel @ 0x6572F0].
MenuFrameCompiler::ResolvedText MenuFrameCompiler::resolve_text_value(
		const std::string *table, const std::string &type,
		const std::string &raw) const {
	std::string value = resolve_var(raw);
	if (iequals(type, "id") && text_tables_ != nullptr) {
		if (const std::string *text = text_tables_->lookup(table, value)) {
			value = *text;
		}
	}
	ResolvedText resolved;
	resolved.text = mnu::strip_hotkey_marker(value, &resolved.hotkey,
			&resolved.hotkey_pos);
	return resolved;
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

int32_t MenuFrameCompiler::font_slot_(const std::string &name) {
	const std::string resolved = resolve_var(name);
	const std::string key = opennova::strutil::to_lower(resolved);
	const auto registered = registered_fonts_.find(key);
	if (resolved.empty() || registered == registered_fonts_.end()) {
		return 0;
	}
	for (size_t i = 1; i < font_names_.size(); ++i) {
		if (opennova::strutil::to_lower(font_names_[i]) == key) {
			return static_cast<int32_t>(i);
		}
	}
	font_names_.push_back(resolved);
	fonts_.push_back(registered->second);
	return static_cast<int32_t>(font_names_.size()) - 1;
}

const fnt_font_t *MenuFrameCompiler::font_at_(int32_t slot) const {
	return slot > 0 && slot < static_cast<int32_t>(fonts_.size())
			? fonts_[static_cast<size_t>(slot)]
			: nullptr;
}

void MenuFrameCompiler::set_texture_size(int32_t slot, int width, int height) {
	if (slot >= 0 && slot < static_cast<int32_t>(texture_sizes_.size())) {
		texture_sizes_[static_cast<size_t>(slot)] = {width, height};
	}
}

bool MenuFrameCompiler::texture_loaded_(int32_t slot) const {
	if (slot < 0 || slot >= static_cast<int32_t>(texture_sizes_.size())) {
		return false;
	}
	const auto &size = texture_sizes_[static_cast<size_t>(slot)];
	return size.first > 0 && size.second > 0;
}

// --- configure ---------------------------------------------------------------

// The first IMAGE load of a texture fixes the band height every later user of
// that cache entry draws [orig: CTextureManager_LoadOrFindTexture @ 0x654980 —
// a cache hit returns the slot as loaded; texFormat (the HEIGHT, -1 = the
// texture's) reaches CEffect_BeginPassTraced only on the first load].
void MenuFrameCompiler::note_window_loads_(const mnu::Window &w) {
	const auto note = [this](const std::vector<mnu::Appearance> &rows) {
		for (const mnu::Appearance &ap : rows) {
			if (appearance_state_slot(ap.state) < 0 || !iequals(ap.type, "image")) {
				continue;
			}
			const std::string name = resolve_var(ap.value);
			if (name.empty()) {
				continue;
			}
			texture_loads_.emplace(texture_load_key(name, ap.flags),
					ap.has_height ? ap.height : -1);
		}
	};
	// The base parse's rows, then its child windows (created at the end of the
	// base parse, @ 0x649789), then what the type's own parse reads after it:
	// ITEMS rows, the parts and the scroll rows (their element order among
	// themselves is not kept by the model).
	note(w.appearances);
	for (const mnu::Window &child : w.children) {
		note_window_loads_(child);
	}
	note(w.items.appearances);
	for (const mnu::WindowPart *part : {&w.list_box, &w.spinup, &w.spindown, &w.scrollbar}) {
		if (part->present()) {
			note_window_loads_(**part);
		}
	}
	note(w.shuttle);
	note(w.scrollup);
	note(w.scrolldown);
}

void MenuFrameCompiler::note_texture_loads(const mnu::Document &doc) {
	for (const mnu::Screen &screen : doc.screens) {
		for (const mnu::Window &root : screen.roots) {
			note_window_loads_(root);
		}
	}
}

void MenuFrameCompiler::reset_texture_loads() {
	texture_loads_.clear();
}

int32_t MenuFrameCompiler::band_height_(const std::string &name,
		const std::string &flags) const {
	const auto found = texture_loads_.find(texture_load_key(resolve_var(name), flags));
	return found == texture_loads_.end() ? -1 : found->second;
}

// One window's APPEARANCE rows into its four state records [orig:
// CUIElement_ParseXMLDefinition @ 0x648120, the APPEARANCE arm @ 0x6483d4..
// 0x648634]: a row with a STATE the parse knows sets that state's availability
// bit and ORs its TYPE's flag (IMAGE, COLOR, CUSTOM, OUTLINE; a missing or
// unknown TYPE ORs nothing: an availability marker); the row writes its type's
// value, so the last row of each type wins; COLOR / OUTLINE read the text with
// wcstoul(16) as it stands (garbage 0, six digits alpha 0).
void MenuFrameCompiler::build_state_passes(
		const std::vector<mnu::Appearance> &rows, StatePass (&states)[4],
		WidgetNode *image_rows_of, const char *list, bool part_rows) {
	for (size_t r = 0; r < rows.size(); ++r) {
		const mnu::Appearance &ap = rows[r];
		if (list != nullptr) {
			note_appearance_row_(rows, r, list, part_rows);
		}
		const int slot = appearance_state_slot(ap.state);
		if (slot < 0) {
			continue;
		}
		StatePass &pass = states[slot];
		pass.present = true;
		if (iequals(ap.type, "color")) {
			pass.has_color = true;
			pass.color = mnu::color_value(resolve_var(ap.value));
		} else if (iequals(ap.type, "outline")) {
			// [orig: OUTLINE=8 -> CUIElement_DrawOutlineRect @ 0x647fc0]
			pass.has_outline = true;
			pass.outline = mnu::color_value(resolve_var(ap.value));
		} else if (iequals(ap.type, "image")) {
			// [orig: IMAGE=2 -> stretched into the element rect @ 0x647e40; the
			// handle is 0 when the load fails, @ 0x6485c8]
			pass.has_image = true;
			pass.texture = intern_texture(ap.value);
			pass.map_state = ap.has_map_state ? ap.map_state : 0;
			pass.band_height = pass.texture >= 0 ? band_height_(ap.value, ap.flags) : -1;
			pass.image_row = static_cast<int>(r);
			pass.row_height = ap.has_height ? ap.height : -1;
			if (image_rows_of != nullptr) {
				image_rows_of->image_rows.push_back(
						{ pass.texture, ap.has_height ? ap.height : -1 });
			}
		} else if (iequals(ap.type, "custom")) {
			// [orig: CUSTOM=4 -> CUIElement_DispatchCustomDrawEvent @ 0x647f10: the
			// event-1 hook; it draws nothing itself]
			pass.custom = true;
		}
	}
}

void MenuFrameCompiler::configure(const mnu::Screen *screen) {
	screen_ = screen;
	texture_names_.clear();
	texture_sizes_.clear();
	font_names_.assign(1, std::string()); // slot 0 = no font
	fonts_.assign(1, nullptr);
	nodes_.clear();
	document_nodes_ = 0;
	edit_scroll_.clear();
	marquee_scroll_.clear();
	notes_.clear();
	building_ = -1;
	if (screen_ == nullptr) {
		return;
	}
	for (const mnu::Window &root : screen_->roots) {
		note_window_loads_(root);
	}
	// Every root window of the screen, in document order: the walks visit
	// them forward, so the last root draws on top and wins the hit
	// [orig: CUIScene_DrawScreensAndCursor @ 0x63bf60; CUIScene_EndFrame
	// @ 0x63e600 walks them in reverse].
	for (const mnu::Window &root : screen_->roots) {
		build_node(root, -1, -1, false);
	}
	document_nodes_ = static_cast<int>(nodes_.size());
	// The spin lists' arrows: parsed by the list before they are attached,
	// then children after the authored ones [orig: CSpinListWnd_Create
	// @ 0x64bc40 -> CSpinListWnd_CreateUpDownChildren @ 0x64b8b0].
	for (int i = 0; i < document_nodes_; ++i) {
		const mnu::Window &w = *nodes_[static_cast<size_t>(i)].window;
		if (w.type != mnu::WindowType::SpinList) {
			continue;
		}
		const int root = nodes_[static_cast<size_t>(i)].root;
		if (w.spinup) {
			const int up = build_node(*w.spinup, i, root, true);
			nodes_[static_cast<size_t>(i)].spin_up = up;
			nodes_[static_cast<size_t>(up)].part_kind = 1;
		}
		if (w.spindown) {
			const int down = build_node(*w.spindown, i, root, true);
			nodes_[static_cast<size_t>(i)].spin_down = down;
			nodes_[static_cast<size_t>(down)].part_kind = 2;
		}
	}
	for (size_t i = 0; i < nodes_.size(); ++i) {
		resolve_node_(static_cast<int>(i));
	}
	settle_part_notes_();
}

int MenuFrameCompiler::build_node(const mnu::Window &w, int parent, int root,
		bool part) {
	const int index = static_cast<int>(nodes_.size());
	nodes_.push_back(WidgetNode{});
	building_ = index;
	note_window_(w);
	{
		WidgetNode node;
		node.window = &w;
		node.parent = parent;
		node.root = root >= 0 ? root : index;
		node.part = part;
		build_state_passes(w.appearances, node.states, &node, "appearance");
		build_state_passes(w.items.appearances, node.items_states, nullptr, "items.appearance");
		// The LIST_BOX and SCROLLBAR parts are windows of their own; an
		// absent part reads as an empty window.
		static const mnu::Window kNoPart;
		const mnu::Window &list_box = w.list_box ? *w.list_box : kNoPart;
		build_state_passes(list_box.appearances, node.popup_states, nullptr, "list_box", true);
		build_state_passes(list_box.items.appearances, node.popup_items_states, nullptr, "list_box",
				true);
		auto build_scrollbar = [&](const mnu::WindowPart &source,
									   WidgetNode::ScrollbarVisual &visual, const char *list) {
			const mnu::Window &scroll = source ? *source : kNoPart;
			visual.present = source.present();
			visual.position = scroll.position;
			visual.has_position = scroll.position.has_left && scroll.position.has_top;
			build_state_passes(scroll.appearances, visual.track, nullptr, list, true);
			build_state_passes(scroll.shuttle, visual.shuttle, nullptr, list, true);
			build_state_passes(scroll.scrollup, visual.up, nullptr, list, true);
			build_state_passes(scroll.scrolldown, visual.down, nullptr, list, true);
		};
		build_scrollbar(w.scrollbar, node.embedded_scrollbar, "scrollbar");
		build_scrollbar(list_box.scrollbar, node.popup_scrollbar, "list_box");
		node.popup_scrollbar.edge_pad =
				w.has_sb_edge_pad ? std::max(w.sb_edge_pad, 0) : 0;
		if (w.type == mnu::WindowType::Scroll) {
			node.scrollbar.present = true;
			node.scrollbar.vertical = !iequals(w.orientation, mnu::kHorizontalOrientation);
			// HEIGHT and WIDTH write the original's one along-axis child
			// extent (the last authored wins), ctor default 20.
			// [orig: CScrollWnd_Construct @ 0x64c450;
			// CUIScrollWidget_ParseExtendedXMLDef @ 0x64c6d0]
			if (w.has_scroll_extent && w.scroll_extent > 0) {
				node.scrollbar.part_extent = w.scroll_extent;
			}
			for (int state = 0; state < 4; ++state) {
				node.scrollbar.track[state] = node.states[state];
			}
			build_state_passes(w.shuttle, node.scrollbar.shuttle, nullptr, "shuttle");
			build_state_passes(w.scrollup, node.scrollbar.up, nullptr, "scrollup");
			build_state_passes(w.scrolldown, node.scrollbar.down, nullptr, "scrolldown");
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
		nodes_[static_cast<size_t>(index)] = std::move(node);
	}
	for (const mnu::Window &child : w.children) {
		build_node(child, index, nodes_[static_cast<size_t>(index)].root, part);
	}
	return index;
}

// What needs the parent chain in place: the font and its colors, the string
// table, the cursor, and the item rows (which resolve their ids through the
// table).
void MenuFrameCompiler::resolve_node_(int index) {
	WidgetNode &node = nodes_[static_cast<size_t>(index)];
	const mnu::Window &w = *node.window;
	const WidgetNode &root = nodes_[static_cast<size_t>(node.root)];
	// FONT: the first self-or-ancestor whose FONT loaded, its colors with it
	// [orig: CWnd_GetFontAndColors @ 0x646a70 walks +0xFC and stops at the first
	// nonzero handle]. A part parsed before it is attached measured with its
	// own FONT only (its +0xFC is 0 at parse).
	const auto font_of = [this](const mnu::Window &win) -> int32_t {
		return win.font.name.empty() ? 0 : font_slot_(win.font.name);
	};
	node.font = 0;
	for (int i = index; i >= 0; i = nodes_[static_cast<size_t>(i)].parent) {
		const mnu::Window &owner = *nodes_[static_cast<size_t>(i)].window;
		const int32_t slot = font_of(owner);
		if (slot == 0) {
			continue;
		}
		node.font = slot;
		node.colors[kStateDefault] = resolve_text_color(owner.font.default_fg);
		node.colors[kStateDisabled] = resolve_text_color(owner.font.disabled_fg);
		node.colors[kStateMouseover] = resolve_text_color(owner.font.mouseover_fg);
		node.colors[kStateSelected] = resolve_text_color(owner.font.selected_fg);
		break;
	}
	// The parse reached as far as the part (an arrow is parsed with no parent).
	node.measure_font = 0;
	int parse_root = index;
	for (int i = index; i >= 0;) {
		const WidgetNode &n = nodes_[static_cast<size_t>(i)];
		parse_root = i;
		const int32_t slot = font_of(*n.window);
		if (slot != 0 && node.measure_font == 0) {
			node.measure_font = slot;
		}
		if (n.part && (n.parent < 0 || !nodes_[static_cast<size_t>(n.parent)].part)) {
			break;
		}
		i = n.parent;
	}
	if (!node.part) {
		parse_root = node.root;
	}
	// TEXT_RSRC: the widget's own, else its root window's [orig:
	// CWnd_GetInheritedTextRsrc @ 0x646AB0]. A part read its strings before it
	// was attached: its own, else the arrow's it hangs under. The CURSOR is the
	// widget's own here; the own-or-root choice waits for the loads
	// (inherited_cursor_).
	node.text_table = window_text_rsrc(w, nodes_[static_cast<size_t>(parse_root)].window);
	node.cursor = intern_texture(w.cursor.file);
	// Item rows [orig: id -> text via the string table, image filename loaded,
	// color wcstoul base 16 @ 0x64bd10].
	// A list's rows lay out by the ITEMS block's JUSTIFY / VJUSTIFY, an ITEM by its own over them
	// (WidgetNode::RowLayout).
	auto row_layout = [](const mnu::Items &items) {
		WidgetNode::RowLayout layout;
		list_row_justify(items.justify, items.vjustify, &layout.justify, &layout.vjustify,
				&layout.x, &layout.y);
		return layout;
	};
	node.row_layout = row_layout(w.items);
	node.popup_row_layout = w.list_box ? row_layout(w.list_box->items) : WidgetNode::RowLayout();
	// A spin list's ITEM of TYPE IMAGE draws its texture and one of TYPE COLOR its colour [orig:
	// CUISpinList_ParseXMLDefinition @ 0x64c253..0x64c2de]; a list's (a LIST, a LAN_LIST, a combo's
	// LIST_BOX) compares its TYPE with ID alone and takes any other's text as written for the row's
	// label, an image's file name or a colour's digits [orig: CListWnd_ParseXMLDefinition
	// @ 0x645b0f..0x645b26, the literal row @ 0x645c1b..0x645c6f]: its rows draw no image and no colour
	// (CListWnd_InsertRow @ 0x644f20 keeps none).
	auto build_items = [&](const std::vector<mnu::Item> &rows, const std::string *table,
								const WidgetNode::RowLayout &list_layout, bool spin,
								std::vector<WidgetNode::ItemVisual> &out) {
		out.clear();
		for (const mnu::Item &item : rows) {
			WidgetNode::ItemVisual visual;
			visual.layout = list_layout;
			list_row_justify(item.justify, item.vjustify, &visual.layout.justify,
					&visual.layout.vjustify, &visual.layout.x, &visual.layout.y);
			if (spin && iequals(item.type, "image")) {
				visual.kind = WidgetNode::ItemVisual::kImage;
				visual.texture = intern_texture(item.text);
			} else if (spin && iequals(item.type, "color")) {
				visual.kind = WidgetNode::ItemVisual::kColor;
				// [orig: CSpinListWnd_Render @ 0x64b220 — forced opaque]
				visual.color = mnu::item_color_argb(resolve_var(item.text));
			} else {
				visual.kind = WidgetNode::ItemVisual::kText;
				visual.text = resolve_text_value(table, item.type, item.text).text;
			}
			out.push_back(visual);
		}
	};
	build_items(w.items.items, node.text_table, node.row_layout, w.type == mnu::WindowType::SpinList,
			node.items);
	// A combo's LIST_BOX list is attached before its parse: its own TEXT_RSRC,
	// else the root's, never the combo's [orig: CComboWnd_ParseXMLDefinition
	// @ 0x65c0d0 -> CWnd_SetParentAndAttach].
	if (w.list_box) {
		build_items(w.list_box->items.items, window_text_rsrc(*w.list_box, root.window),
				node.popup_row_layout, false, node.popup_items);
	} else {
		node.popup_items.clear();
	}
	// A TABLE's columns: the XML set-up, its HEADER labels through the string
	// table above [orig: CTableWnd_ParseXMLContentDefinition @ 0x6427d0].
	if (w.type == mnu::WindowType::Table) {
		build_table_columns_(node);
	}
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

// The pump verdict [orig: CWnd_ProcessMouseEvent @ 0x647a00 — disabled
// -> 1; hit + button down -> 3; hit + button up -> 2; else 0]. A spin list's
// arrows are its child buttons: the one under the mouse is the one hit, and
// the list itself is not [orig: the child hit-test walk @ 0x647a90..0x647ab0
// keeps a parent from claiming a point a child contains]. For an arrow `ws` is
// its spin list's row (the claim rides there with the arrow it is over).
int MenuFrameCompiler::pump_visual_state(const WidgetNode &node,
		const MenuWidgetState *ws, const MenuWidgetState *own) const {
	const mnu::Window &w = *node.window;
	if (node.part) {
		if (disabled_(w, own)) {
			return kStateDisabled;
		}
		if (node.part_kind != 0 && ws != nullptr && ws->spin_part == node.part_kind) {
			if (ws->pressed) {
				return kStateSelected;
			}
			if (ws->hovered) {
				return kStateMouseover;
			}
		}
		return kStateDefault;
	}
	if (disabled_(w, ws)) {
		return kStateDisabled;
	}
	if (ws != nullptr && ws->spin_part != 0) {
		return kStateDefault;
	}
	if (ws != nullptr && ws->pressed) {
		return kStateSelected;
	}
	if (ws != nullptr && ws->hovered) {
		return kStateMouseover;
	}
	return kStateDefault;
}

// The pump's availability fallback [orig: CWnd_ProcessMouseEvent @ 0x647a00,
// @ 0x647c89..0x647cb5 — an authored state is kept as it is (the jnz @ 0x647ca9),
// an unauthored one falls to DEFAULT, and with no DEFAULT either to -1 = no
// appearance pass; the result is +236, which every render reads for the
// appearance AND the label colors]. (CWnd_SetVisualState @ 0x646340, which
// checks DEFAULT for every state, serves only the captured scroll thumb.)
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

// The element rect, fixed at parse [orig: the parse tail @ 0x649736 ->
// CStaticWnd_AdjustRectToTextSize @ 0x6575f0]: POSITION; a degenerate axis takes the
// IMAGE rows' largest extents; a text-sized type then sizes a still degenerate
// axis from its authored STRING (the parse-time text, measured with the font the
// parse reached).
mnu::RectEdges MenuFrameCompiler::solve_rect(const WidgetNode &node) const {
	return solve_rect_at(node, node.window->position);
}

// The widget's own rect as the walks use it: the rect a CWnd_SetRect gave it (a
// moved widget keeps it), else the parse-time solve; a spin arrow is never moved
// this way [orig: CWnd_SetRect @0x646560 — CopyRect into +0xD0].
mnu::RectEdges MenuFrameCompiler::node_rect_(const WidgetNode &node,
		const MenuWidgetState *ws) const {
	if (ws != nullptr && ws->has_rect && !node.part) {
		return ws->rect;
	}
	return solve_rect(node);
}

mnu::RectEdges MenuFrameCompiler::solve_rect_at(const WidgetNode &node,
		const mnu::Position &position) const {
	const mnu::Window &w = *node.window;
	mnu::ImageExtents extents;
	for (const WidgetNode::ImageRow &row : node.image_rows) {
		int tw = 0;
		int th = 0;
		if (texture_loaded_(row.texture)) {
			tw = texture_sizes_[static_cast<size_t>(row.texture)].first;
			th = texture_sizes_[static_cast<size_t>(row.texture)].second;
		}
		extents.add(tw, th, row.height);
	}
	mnu::RectEdges rect = mnu::position_rect(position.has_left,
			position.left, position.has_top, position.top,
			position.has_right, position.right, position.has_bottom,
			position.bottom, static_cast<int>(extents.width),
			static_cast<int>(extents.height));
	if (mnu::window_type_is_text_sized(w.type) || node.part) {
		const std::string text = w.string_data.present
				? resolve_text_value(node.text_table, w.string_data.type,
						  w.string_data.value)
						  .text
				: std::string();
		if (!text.empty() &&
				(rect.right <= rect.left || rect.bottom <= rect.top)) {
			int tw = 0;
			int th = 0;
			measure_with_(font_at_(node.measure_font), text, &tw, &th);
			rect = mnu::adjust_rect_to_text_size(rect, tw, th,
					w.string_data.justify, w.string_data.vjustify);
		}
	}
	return rect;
}

MenuFrameCompiler::ResolvedText MenuFrameCompiler::resolved_widget_text(
		const WidgetNode &node,
		const MenuWidgetState *ws) const {
	if (ws != nullptr && ws->has_text) {
		ResolvedText resolved;
		if (node.window->type == mnu::WindowType::Button) {
			// A runtime relabel is raw text: retail's SetLabel strips the
			// mnemonic marker but runs no %VAR% pass (that pass is parse-time,
			// D-MNU-1) [orig: CButtonWnd_SetLabel @ 0x6572F0].
			resolved.text = mnu::strip_hotkey_marker(ws->text, &resolved.hotkey,
					&resolved.hotkey_pos);
			return resolved;
		}
		resolved.text = ws->text;
		return resolved;
	}
	const mnu::Window &w = *node.window;
	if (w.string_data.present) {
		return resolve_text_value(node.text_table, w.string_data.type,
				w.string_data.value);
	}
	return ResolvedText();
}

std::string MenuFrameCompiler::widget_text(const WidgetNode &node,
		const MenuWidgetState *ws) const {
	return resolved_widget_text(node, ws).text;
}

const fnt_font_t *MenuFrameCompiler::font_for(const WidgetNode &node) const {
	return font_at_(node.font);
}

void MenuFrameCompiler::measure_with_(const fnt_font_t *font,
		const std::string &text, int *out_w, int *out_h) const {
	*out_w = 0;
	*out_h = 0;
	if (font == nullptr) {
		// [orig: font_cache_ensure_font_loaded @ 0x653de0 returns 0 for handle 0:
		// font_cache_measure_text @ 0x6532a0 writes nothing]
		return;
	}
	hud::GameFont gf;
	gf.set_font(font);
	// Menu measurement runs at scale 1 in design space
	// [orig: CStaticWnd_DrawLabel @ 0x656fb0 fld1 before the measure].
	gf.measure(text.c_str(), 1.0f, 1.0f, out_w, out_h);
}

void MenuFrameCompiler::measure_text(const WidgetNode &node,
		const std::string &text, int *out_w, int *out_h) const {
	measure_with_(font_for(node), text, out_w, out_h);
}

// --- emitters ----------------------------------------------------------------

// Per-element int truncation of the scaled coordinate
// [orig: CUIElement_DrawStretchedTexture @ 0x647d40 — (int)(edge * scale)].
float MenuFrameCompiler::emit_x(int design, float scale) {
	return menu_scaled_edge(design, scale);
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
	} else if (texture != kMenuTexNone && texture_loaded_(texture)) {
		const auto &size = texture_sizes_[static_cast<size_t>(texture)];
		set_image_uv(quad, size.first, size.second, 0, size.second);
	}
	push_quad(quad);
}

// Every menu image draws through the tiled-texture strip, which adds half a texel
// to both ends of its U and V: the image's texel columns 0.5 .. W + 0.5 and its band's
// rows start + 0.5 .. end + 0.5 run across the quad, so on Direct3D 9's pixel centres
// a 1:1 draw samples each texel's centre and a stretched one lands where the game's
// does [orig: Render_DrawTiledTextureStrip @ 0x67aed0 — u0 = 0.5 / texW, u1 = 0.5 /
// texW + imageW / texW, v0 = start / texH + 0.5 / texH, v1 = end / texH + 0.5 / texH
// @ 0x67b032..0x67b0b8; its alternate arm (positions minus half a pixel instead) reads
// g_TiledTextureHalfPixelPositions, which nothing sets]. texW and texH are the power-of-
// two tile the image was copied into, so in the image's own texels the offsets are the
// same half texel (menu_image_texture_side).
void MenuFrameCompiler::set_image_uv(MenuQuad &quad, int width, int height,
		int64_t band_start, int64_t band_end) {
	if (width <= 0 || height <= 0) {
		return;
	}
	const float w = static_cast<float>(width);
	const float h = static_cast<float>(height);
	quad.u0 = 0.5f / w;
	quad.u1 = 0.5f / w + 1.0f;
	quad.v0 = static_cast<float>(band_start) / h + 0.5f / h;
	quad.v1 = static_cast<float>(band_end) / h + 0.5f / h;
}

// The IMAGE pass draws the band [MAP_STATE * H, (MAP_STATE + 1) * H) of the
// texture stretched over the rect, H being the height the texture's first load
// baked in (the texture's own height when that load had no HEIGHT); a band that
// starts below 0 or ends past the texture (an unsigned compare) draws nothing
// [orig: CUIElement_DrawTextureNative @ 0x647e40 -> CTextureManager_DrawScaledRect
// @ 0x654e60 -> Render_DrawTiledTextureStrip @ 0x67aed0, the -65280 return]. So a
// MAP_STATE of 1 or more with no HEIGHT draws nothing, a HEIGHT alone crops row
// 0, and a HEIGHT of 0 stretches texel row 0 (v0 == v1, row 0's centre, through
// the strip's half texel: set_image_uv). A texture that did not load draws
// nothing (its handle is 0).
void MenuFrameCompiler::emit_state_texture(const mnu::RectEdges &design,
		const WalkScale &s,
		const StatePass &pass) {
	if (!pass.has_image || !texture_loaded_(pass.texture)) {
		return;
	}
	const int texture_height =
			texture_sizes_[static_cast<size_t>(pass.texture)].second;
	const int64_t band = pass.band_height == -1 ? texture_height : pass.band_height;
	const int64_t start = static_cast<int64_t>(pass.map_state) * band;
	const int64_t end = static_cast<int64_t>(pass.map_state + 1) * band;
	if (start < 0 || static_cast<uint32_t>(end) > static_cast<uint32_t>(texture_height) ||
			end < 0) {
		return;
	}
	emit_rect_quad(design, s, 0xFFFFFFFFu, pass.texture, false, 1.0f, 1.0f);
	const int texture_width =
			texture_sizes_[static_cast<size_t>(pass.texture)].first;
	set_image_uv(draw_list_.quads.back(), texture_width, texture_height, start, end);
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
// IMAGE, OUTLINE (the CUSTOM pass dispatches a draw event to the shell; there is
// no CURSOR type)
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

// The tiled fill is stencil atlas cell (3, 0), copied by retail into a
// SIZE-by-SIZE border_fill_material. The eight border pieces use the stencil
// cell as texture stage 0 and the brush as stage 1. Retail submits 0x7F7F7F;
// stage 0's modulate-2x cancels that tint, then stage 1 produces the effective
// 2 * stencil * brush material carried here as texture + texture2.
// [orig: CUIElement_InitBorderMaterials @ 0x646f70; CUIElement_DrawFrame @ 0x64a210;
// RenderState_DecodeModeColorStage @ 0x681080 for material mode 0x651].
MenuFrameCompiler::FrameGate MenuFrameCompiler::frame_gate_(const WidgetNode &node,
		int *tile) const {
	*tile = 0;
	if (node.frame_owner < 0) {
		return FrameGate::NoFrame;
	}
	if (node.frame_stencil < 0) {
		return FrameGate::NoStencil;
	}
	const auto &stencil_size =
			texture_sizes_[static_cast<size_t>(node.frame_stencil)];
	if (stencil_size.first <= 0 || stencil_size.second <= 0) {
		return FrameGate::NoStencil;
	}
	const mnu::Frame &frame =
			nodes_[static_cast<size_t>(node.frame_owner)].window->frame;
	*tile = mnu::frame_stencil_tile_size(
			frame.has_stencil_size ? frame.stencil_size : 0,
			stencil_size.first);
	return *tile <= 0 ? FrameGate::TileZero : FrameGate::Draws;
}

void MenuFrameCompiler::emit_frame(const WidgetNode &node,
		const mnu::RectEdges &rect, const WalkScale &s) {
	int tile = 0;
	if (frame_gate_(node, &tile) != FrameGate::Draws) {
		return;
	}
	const mnu::Frame &frame =
			nodes_[static_cast<size_t>(node.frame_owner)].window->frame;
	const bool has_brush = node.frame_brush >= 0;
	const auto &stencil_size =
			texture_sizes_[static_cast<size_t>(node.frame_stencil)];
	const float tex_w = static_cast<float>(stencil_size.first);
	const float tex_h = static_cast<float>(stencil_size.second);
	// The window's rect on the device, each edge truncated
	// [orig: CUIScene_ScaleRectDesignToDevice @ 0x63b210 from @ 0x64a255].
	const float left_edge = emit_x(rect.left, s.x);
	const float top_edge = emit_x(rect.top, s.y);
	const float right_edge = emit_x(rect.right, s.x);
	const float bottom_edge = emit_x(rect.bottom, s.y);
	// The fill: the cell-sized copy of stencil cell (3, 0), at UV (x + 0.5) / cell
	// over the rect, in diffuse 0xFF7F7F7F through the material's MODULATE2X: the
	// texel times 254/255 [orig: @ 0x64a281..0x64a2b3 -> draw_textured_quad_from_rect
	// @ 0x64a000, UV (pos + 0.5) / size @ 0x64a0cb; border_fill_material made with
	// mode 0x651 by CUIElement_InitBorderMaterials @ 0x64715a].
	const mnu::FrameTileRect fill_uv = mnu::frame_tile_rect(tile, 3, 0);
	MenuQuad fill;
	fill.x0 = left_edge;
	fill.y0 = top_edge;
	fill.x1 = right_edge;
	fill.y1 = bottom_edge;
	fill.color = 0xFFFEFEFEu;
	fill.texture = node.frame_stencil;
	fill.tiled = true;
	fill.u0 = static_cast<float>(fill_uv.x) / tex_w;
	fill.v0 = static_cast<float>(fill_uv.y) / tex_h;
	fill.u1 = static_cast<float>(fill_uv.x + fill_uv.size) / tex_w;
	fill.v1 = static_cast<float>(fill_uv.y + fill_uv.size) / tex_h;
	push_quad(fill);
	if (!has_brush) {
		return;
	}
	// The eight pieces on the device, in floats: the cell scaled by the pair,
	// the left and top hung a cell out and pulled back by the scaled inset, the
	// right and bottom an inset in and one pixel more, each sum a float (the
	// x87 unit's single precision, menu_scaled_edge)
	// [orig: CUIElement_DrawFrame @ 0x64a2d7 (left = L - cell * sx + sx * INSETX),
	// @ 0x64a318 (top), @ 0x64a332 (the top row's bottom), @ 0x64a3c0 (right =
	// R - sx * INSETX - 1.0), @ 0x64a4bb (bottom = B - INSETY * sy - 1.0)].
	const float cell = static_cast<float>(tile);
	const float inset_x = static_cast<float>(frame.insetx);
	const float inset_y = static_cast<float>(frame.insety);
	const float cell_w = cell * s.x;
	const float cell_h = cell * s.y;
	const float left = (left_edge - cell * s.x) + s.x * inset_x;
	const float top = (top_edge - cell * s.y) + inset_y * s.y;
	const float top_row_end = cell * s.y + top;
	const float right = (right_edge - s.x * inset_x) - 1.0f;
	const float bottom = (bottom_edge - inset_y * s.y) - 1.0f;
	const float bottom_row_end = bottom + cell * s.y;
	struct Piece {
		int col;
		int row;
		float x0, y0, x1, y1;
	};
	// TL, top, TR, left, right, BL, bottom, BR, in the draw order
	// [orig: @ 0x64a392, 0x64a417, 0x64a489, 0x64a51b, 0x64a5a7, 0x64a63d,
	// 0x64a6c5, 0x64a73d].
	const Piece pieces[] = {
		{ 0, 0, left, top, left + cell_w, top_row_end },
		{ 1, 0, left + cell_w, top, right, top_row_end },
		{ 2, 0, right, top, right + cell_w, top_row_end },
		{ 0, 1, left, top_row_end, left + cell_w, bottom },
		{ 2, 1, right, top_row_end, right + cell_w, bottom },
		{ 0, 2, left, bottom, left + cell_w, bottom_row_end },
		{ 1, 2, left + cell_w, bottom, right, bottom_row_end },
		{ 2, 2, right, bottom, right + cell_w, bottom_row_end },
	};
	// The brush's period: its HEIGHT across x and its WIDTH down y, the
	// dimensions the border block stores swapped [orig: CUIElement_InitBorderMaterials
	// @ 0x6470ee..0x647116 stores the brush's height at +0x27C and its width at
	// +0x280; CUIElement_DrawFrame passes +0x27C (frameInfo + 324) as the x divisor
	// @ 0x64a384 and +0x280 as the y divisor @ 0x64a372].
	const auto &brush_size = texture_sizes_[static_cast<size_t>(node.frame_brush)];
	for (const Piece &piece : pieces) {
		const mnu::FrameTileRect uv = mnu::frame_tile_rect(tile, piece.col, piece.row);
		MenuQuad quad;
		quad.x0 = piece.x0;
		quad.y0 = piece.y0;
		quad.x1 = piece.x1;
		quad.y1 = piece.y1;
		quad.color = 0xFF7F7F7Fu;
		quad.texture = node.frame_stencil;
		quad.texture2 = node.frame_brush;
		quad.texture2_period_x = static_cast<float>(brush_size.second);
		quad.texture2_period_y = static_cast<float>(brush_size.first);
		if (tex_w > 0.0f && tex_h > 0.0f) {
			quad.u0 = static_cast<float>(uv.x) / tex_w;
			quad.v0 = static_cast<float>(uv.y) / tex_h;
			quad.u1 = static_cast<float>(uv.x + uv.size) / tex_w;
			quad.v1 = static_cast<float>(uv.y + uv.size) / tex_h;
		}
		push_quad(quad);
	}
}

namespace {

// The text sink's colour: it halves the RGB on a modulate-2x device (the UI
// half-bright mode, the device caps' modulate flag) and forces the alpha
// opaque, menus never passing the keep-alpha flag 0x10000; the font page's
// MODULATE2X doubles it back on the device (hud::kFontPageMaterialWord), so a
// menu text reads at its colour with each channel's low bit lost
// [orig: CFontCache_DrawTextScaled @0x653170 — g_UIHalfBrightMode @0x31C3760
// (caps dword 6, set from the adapter's TextureOpCaps unless the device's
// no-modulate-2x workaround, CGfxDevice_QueryAdapterCaps @0x67df72..0x67df8e)
// -> (c >> 1) & 0x7F7F7F @0x6531e7..0x6531eb, | 0xFF000000 @0x6531db].
uint32_t menu_text_sink_color(uint32_t color) {
	return ((color >> 1) & 0x7F7F7Fu) | 0xFF000000u;
}

} // namespace

// Glyph runs: layout in design space at the anamorphic pair — the anchor is
// the design point times the pair, untruncated (menu_text_anchor), and the
// glyphs scale by the same pair [orig: CFontCache_DrawTextScaled @ 0x653170
// scales the anchor and forwards scaleX/scaleY into CGameFont_DrawText
// @ 0x6752c0].
void MenuFrameCompiler::emit_glyph_run(const WidgetNode &node,
		const std::string &text, int design_x, int design_y,
		const WalkScale &s, uint32_t color, int caret) {
	emit_glyph_run_with_(node.font, text, design_x, design_y, s, color, caret);
}

void MenuFrameCompiler::emit_glyph_run_with_(int32_t font_slot,
		const std::string &text, int design_x, int design_y,
		const WalkScale &s, uint32_t color, int caret) {
	// No font up the chain: nothing drawn [orig: font_cache_ensure_font_loaded
	// @ 0x653de0 returns 0 for handle 0, gating CFontCache_DrawTextScaled].
	const fnt_font_t *font = font_at_(font_slot);
	if (font == nullptr || text.empty()) {
		return;
	}
	hud::GameFont gf;
	gf.set_font(font);
	const size_t first = draw_list_.glyphs.size();
	const size_t underline_first = draw_list_.underlines.size();
	const float x = menu_text_anchor(design_x, s.x);
	const float y = menu_text_anchor(design_y, s.y);
	color = menu_text_sink_color(color);
	const hud::GameFontRun run =
			gf.layout(text.c_str(), x, y, s.x, s.y, 0u, color);
	draw_list_.glyphs.insert(draw_list_.glyphs.end(), run.quads.begin(),
			run.quads.end());
	draw_list_.underlines.insert(draw_list_.underlines.end(),
			run.underlines.begin(), run.underlines.end());
	if (caret >= 0) {
		emit_caret(gf, text, design_x, design_y, s, color, caret);
	}
	const size_t count = draw_list_.glyphs.size() - first;
	if (count > 0) {
		MenuDrawList::FontRun fr;
		fr.font = font_slot;
		fr.first = static_cast<int32_t>(first);
		fr.count = static_cast<int32_t>(count);
		fr.underline_first = static_cast<int32_t>(underline_first);
		fr.underline_count =
				static_cast<int32_t>(draw_list_.underlines.size() - underline_first);
		push_font_run(fr);
	}
}

// The caret: an underscore drawn at the caret character's x, x-stretched to
// that character's width; the char at end-of-text measures as '_' itself.
// The caret's design x is the anchor plus the left run's width, plus a gap of
// trunc((spacing - 1) * design_scale) + 2 after a non-empty left run and again
// when the caret sits inside the text (CGameFont_GetSpacingPad's
// trunc(...) + 1, and the caller's + 1). The strike is the '_' glyph at the
// stretched scale pair, so its anchor is that design x divided by the stretch,
// truncated, then multiplied back by the sink: retail's pen lands where the
// division's truncation leaves it [orig: CFontCache_DrawTextWithCursor
// @ 0x6533b0 — the left-run measure @0x6534b7, the gaps @0x6534de..0x6534eb
// and @0x653583..0x653590 (CGameFont_GetSpacingPad @ 0x6741e0: ftol((spacing
// - 1) * design) + 1), the char and '_' widths @0x65352a / @0x653550, the
// stretch charW / underW * scaleX @0x6535f0..0x653600 and the anchor
// ftol(underW / charW * x) @0x653605..0x65360f, drawn through
// CFontCache_DrawTextScaled @0x653624].
void MenuFrameCompiler::emit_caret(hud::GameFont &gf, const std::string &text,
		int design_x, int design_y, const WalkScale &s, uint32_t color, int caret) {
	const fnt_font_t *font = gf.font();
	const int len = static_cast<int>(text.size());
	const int at = std::clamp(caret, 0, len);
	int left_w = 0;
	int left_h = 0;
	if (at > 0) {
		gf.measure(text.substr(0, static_cast<size_t>(at)).c_str(), 1.0f,
				1.0f, &left_w, &left_h);
	}
	const int gap = static_cast<int>(static_cast<float>(font->glyph_spacing - 1) *
							fnt_design_scale(font->design_width)) +
			2;
	int caret_x = design_x + left_w;
	if (left_w > 0) {
		caret_x += gap;
	}
	if (at > 0 && at < len) {
		caret_x += gap;
	}
	const char under = at < len ? text[static_cast<size_t>(at)] : '_';
	const char under_str[2] = {under == '\0' ? '_' : under, '\0'};
	int char_w = 0;
	int under_h = 0;
	gf.measure(under_str, 1.0f, 1.0f, &char_w, &under_h);
	int bar_w = 0;
	gf.measure("_", 1.0f, 1.0f, &bar_w, &under_h);
	if (char_w <= 0 || bar_w <= 0) {
		return; // a strike of no width (or an infinite one): nothing shows
	}
	// Float arithmetic throughout, the x87 unit's single precision
	// (menu_scaled_edge).
	const float stretch_x =
			static_cast<float>(char_w) / static_cast<float>(bar_w) * s.x;
	const int strike_x = static_cast<int>(
			static_cast<float>(bar_w) / static_cast<float>(char_w) *
			static_cast<float>(caret_x));
	const hud::GameFontRun run = gf.layout("_", menu_text_anchor(strike_x, stretch_x),
			menu_text_anchor(design_y, s.y), stretch_x, s.y, 0u, color);
	draw_list_.glyphs.insert(draw_list_.glyphs.end(), run.quads.begin(),
			run.quads.end());
}

bool MenuFrameCompiler::fit_label_(const WidgetNode &node, const std::string &text,
		int avail, std::string *drawn, int *drawn_w, int *line_h) const {
	if (avail < 1) {
		return false; // [orig: the < 1 early-out]
	}
	int full_w = 0;
	measure_text(node, text, &full_w, line_h);
	// Truncate to the largest prefix strictly below the span
	// [orig: the prefix-measure loop @ 0x657199..0x6571e4].
	*drawn = text;
	*drawn_w = full_w;
	if (full_w > avail) {
		int fit = static_cast<int>(text.size());
		for (int n = 1; n <= static_cast<int>(text.size()); ++n) {
			int wpx = 0;
			int hpx = 0;
			measure_text(node, text.substr(0, static_cast<size_t>(n)), &wpx,
					&hpx);
			*drawn_w = wpx;
			if (wpx >= avail) {
				fit = n - 1;
				break;
			}
		}
		*drawn = text.substr(0, static_cast<size_t>(std::max(fit, 0)));
	}
	return true;
}

// The single-line text pass shared by the static family
// [orig: CStaticWnd_DrawLabel @ 0x656fb0 — edge-inset available width,
//  truncate-to-fit, justify from the truncated width, then the state-colored
//  draw; the wrap path (+760 -> sub_653D60) is deferred (D-MNU-13)].
void MenuFrameCompiler::emit_widget_text(const WidgetNode &node,
		const mnu::RectEdges &rect, const WalkScale &s, int color_state,
		const MenuWidgetState *ws, int caret,
		const std::string *override_text) {
	const mnu::Window &w = *node.window;
	ResolvedText resolved;
	if (override_text != nullptr) {
		resolved.text = *override_text;
	} else {
		resolved = resolved_widget_text(node, ws);
	}
	const std::string &text = resolved.text;
	if (text.empty()) {
		return;
	}
	const int edge = w.string_data.has_edge ? w.string_data.edge : 0;
	const int avail = (rect.right - rect.left) - 2 * edge;
	std::string drawn;
	int drawn_w = 0;
	int line_h = 0;
	if (!fit_label_(node, text, avail, &drawn, &drawn_w, &line_h)) {
		return;
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
	// The button mnemonic is drawn by retail's caret leg — the '_' glyph
	// stretched to the marked character's width at its prefix offset, the
	// same CFontCache_DrawTextWithCursor path the edit caret rides (emit_caret).
	// Only when the marked byte survived prefix truncation.
	// [orig: CStaticWnd_DrawLabel passes the +740 offset @0x657270 ->
	//  CFontCache_DrawTextWithCursor @0x6533b0 — prefix measure + pad @0x6534b7..
	//  0x6534eb, '_' @0x6534f7/@0x653550, char-width stretch
	//  @0x6535d8..0x65360f]
	int drawn_caret = caret;
	if (drawn_caret < 0 && w.type == mnu::WindowType::Button &&
			resolved.hotkey_pos >= 0 &&
			resolved.hotkey_pos < static_cast<int>(drawn.size())) {
		drawn_caret = resolved.hotkey_pos;
	}
	emit_glyph_run(node, drawn, x, y, s, node.colors[state], drawn_caret);
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
	const bool blink_on = menu_caret_shown(frame.time_ms);
	if (text.empty()) {
		// An empty focused edit still blinks its caret at the text anchor
		// [orig: the cursor leg of CFontCache_DrawTextWithCursor @ 0x6533b0 runs for
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
				// The strike goes through the text sink, which halves its
				// colour like any menu text's (menu_text_sink_color).
				emit_caret(gf, std::string(), rect.left + edge, y, s,
						menu_text_sink_color(node.colors[visual >= 0 && visual < 4
										? visual
										: 0]),
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
	// The scroll window [orig: CEditWnd_UpdateScrollRange @ 0x661790 over
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

// The current item of a spin list [orig: CSpinListWnd_Render @ 0x64b220]: none for a row the game
// disabled (bit 2 of its flags, @ 0x64b334, CSpinListWnd_SetItemEnabled @ 0x64bbd0); laid out from the
// window's own rect, the parent-relative one (CopyRect of +0xD0 @ 0x64b31d), by the item's own alignment
// word and offsets, the ITEMS block's where the ITEM sets none (WidgetNode::ItemVisual::layout, as
// CUISpinList_ParseXMLDefinition @ 0x64bd10 stores them @ 0x64c237..0x64c24f): vertically first, then
// horizontally, then its left and top moved by the item's x and y offsets (@ 0x64b4d8, @ 0x64b4df).
// - An image moves the whole rect by deltas made of its texture's HEIGHT on both axes (retail's own,
//   CTextureManager_GetTextureHeight @ 0x64b3b6 / 0x64b3e5 / 0x64b455 / 0x64b486): centred by half the
//   rect less half that height, a bottom or right one by the own rect's bottom (less the height) or right
//   (less half of it) taken as the delta itself (@ 0x64b3c9, @ 0x64b468); then it stretches the texture
//   into that rect moved by the ancestors' origins (CUIElement_DrawTextureNative @ 0x64b509, which adds
//   them @ 0x647e6f through CWnd_AccumulateAncestorOffset @ 0x6465e0).
// - A colour fills the whole widget rect, forced opaque (@ 0x64b52c..0x64b539).
// - A text measures itself and lays out by its own extent (@ 0x64b3cd..0x64b41a, @ 0x64b46c..0x64b4b9),
//   moves by the ancestors' origins (@ 0x64b547), then draws through CFontCache_DrawTextWithCursor
//   @ 0x64b5a7 with the item's alignment word, which moves a word of exactly 1 left by half the text's
//   width again and of exactly 2 by all of it [orig: CFontCache_DrawTextWithCursor @ 0x65341f..0x653436],
//   and with the item's own colour index (+8), which CSpinListWnd_InsertItem zeroes (@ 0x64b88d) and no
//   code writes again: the DEFAULT pair whatever the widget's state (the switch @ 0x653445 sends 0 there).
void MenuFrameCompiler::emit_item_cell(const WidgetNode &node, const mnu::RectEdges &local,
		const mnu::RectEdges &rect, const WalkScale &s, const MenuWidgetState *ws) {
	const int selected = ws != nullptr ? ws->selected_item : 0;
	const bool runtime_rows = ws != nullptr && ws->has_items;
	const int row_count = runtime_rows ? static_cast<int>(ws->items.size())
									   : static_cast<int>(node.items.size());
	if (selected < 0 || selected >= row_count) {
		return;
	}
	if (ws != nullptr && static_cast<size_t>(selected) < ws->disabled_items.size() &&
			ws->disabled_items[static_cast<size_t>(selected)] != 0) {
		return;
	}
	// A row the game added is text, laid out by the ITEMS block's alignment.
	WidgetNode::ItemVisual runtime_item;
	if (runtime_rows) {
		runtime_item.kind = WidgetNode::ItemVisual::kText;
		runtime_item.text = ws->items[static_cast<size_t>(selected)];
		runtime_item.layout = node.row_layout;
	}
	const WidgetNode::ItemVisual &item = runtime_rows
			? runtime_item
			: node.items[static_cast<size_t>(selected)];
	const int justify = item.layout.justify;
	const int vjustify = item.layout.vjustify;
	// The ancestors' origins, added once the item is laid out in the own rect.
	const int origin_x = rect.left - local.left;
	const int origin_y = rect.top - local.top;
	mnu::RectEdges rc = local;
	const int w = rc.right - rc.left;
	const int h = rc.bottom - rc.top;
	switch (item.kind) {
		case WidgetNode::ItemVisual::kColor: {
			emit_rect_quad(rect, s, item.color, kMenuTexNone, false, 1.0f, 1.0f);
			break;
		}
		case WidgetNode::ItemVisual::kImage: {
			if (item.texture < 0) {
				break;
			}
			const int tex_h = texture_sizes_[static_cast<size_t>(item.texture)].second;
			const int half_tex_h = static_cast<int>(static_cast<unsigned>(tex_h) >> 1);
			int dy = 0;
			if (vjustify == 0x10) {
				dy = (h >> 1) - half_tex_h;
			} else if (vjustify == 0x20) {
				dy = rc.bottom - tex_h;
			}
			int dx = 0;
			if (justify == 1) {
				dx = (w >> 1) - half_tex_h;
			} else if (justify == 2) {
				dx = rc.right - half_tex_h;
			}
			rc = offset_rect(rc, dx, dy);
			rc.left += item.layout.x;
			rc.top += item.layout.y;
			emit_rect_quad(offset_rect(rc, origin_x, origin_y), s, 0xFFFFFFFFu, item.texture, false, 1.0f,
					1.0f);
			break;
		}
		case WidgetNode::ItemVisual::kText: {
			if (item.text.empty()) {
				break;
			}
			int tw = 0;
			int th = 0;
			measure_text(node, item.text, &tw, &th);
			if (vjustify == 0x10) {
				rc.top += (h >> 1) - (th >> 1);
			} else if (vjustify == 0x20) {
				rc.top = rc.bottom - th;
			}
			if (justify == 1) {
				rc.left += (w >> 1) - (tw >> 1);
			} else if (justify == 2) {
				rc.left = rc.right - tw;
			}
			int x = rc.left + item.layout.x + origin_x;
			const int y = rc.top + item.layout.y + origin_y;
			const int align = justify | vjustify;
			if (align == 1 || align == 2) {
				x -= align == 1 ? tw >> 1 : tw;
			}
			emit_glyph_run(node, item.text, x, y, s, node.colors[kStateDefault], -1);
			break;
		}
	}
}

// One list row's text [orig: CListWnd_DrawItems @ 0x6440f7..0x644345], the list's and a combo
// dropdown's alike. The text is measured whole; wider than the row with the edges (EDGE, +756)
// and the edge pad (+0xFF8, which only a combo's parse sets: CComboWnd_ParseXMLDefinition
// @ 0x65c149; a list's is 0) it draws its widest prefix narrower than the span left, the width
// then the prefix's one character longer (@ 0x644101..0x644170). It lays out in the row rect by
// the row's justify word against that width (@ 0x64426a..0x6442dc), moves by the row's x / y
// offsets (@ 0x6442f8..0x644308), and draws through CFontCache_DrawTextWithCursor, which takes
// the word as its alignment and moves a word of exactly 1 left by half the drawn text's width
// again, of exactly 2 by all of it [orig: CFontCache_DrawTextWithCursor @ 0x65341f..0x653436].
void MenuFrameCompiler::emit_list_row_text_(const WidgetNode &node, const mnu::RectEdges &row,
		const std::string &text, int align, int offset_x, int offset_y, int edge, int edge_pad,
		const WalkScale &s, uint32_t color) {
	if (text.empty()) {
		return;
	}
	int width = 0;
	int height = 0;
	measure_text(node, text, &width, &height);
	std::string drawn = text;
	const int row_w = row.right - row.left;
	if (edge_pad + width + 2 * edge > row_w) {
		const int span = row_w - 2 * edge - edge_pad;
		size_t count = 0;
		do {
			++count;
			measure_text(node, text.substr(0, count), &width, &height);
		} while (width < span && count < text.size());
		drawn = text.substr(0, count - 1);
	}
	int x = row.left;
	int y = row.top;
	if ((align & 0xF0) == 0x10) {
		y += ((row.bottom - row.top) >> 1) - (height >> 1);
	} else if ((align & 0xF0) == 0x20) {
		y = row.bottom - height;
	}
	if ((align & 0xF) == 1) {
		x = ((row.right - row.left) >> 1) - (width >> 1) + row.left;
	} else if ((align & 0xF) == 2) {
		x = row.right - width;
	}
	x += offset_x;
	y += offset_y;
	if (align == 1 || align == 2) {
		int drawn_w = 0;
		int drawn_h = 0;
		measure_text(node, drawn, &drawn_w, &drawn_h);
		x -= align == 1 ? drawn_w >> 1 : drawn_w;
	}
	emit_glyph_run(node, drawn, x, y, s, color, -1);
}

// List rows [orig: CListWnd_DrawItems @ 0x643f30]: each visible row first
// draws the ITEMS per-state appearance for its style index (selection/hover
// highlight) into the row rect, then the row text with the SAME style index
// selecting the FONT color pair (emit_list_row_text_). Height = font "W" else
// MIN_ITEM_HEIGHT; rows run from the scroll row and clip to the widget rect. A
// row the game added lays out by the ITEMS block's justification, an authored
// ITEM by its own (WidgetNode::RowLayout).
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
		const int color_state = style >= 0 ? style : kStateDefault;
		if (runtime_rows) {
			const WidgetNode::RowLayout &layout = node.row_layout;
			emit_list_row_text_(node, row, ws->items[static_cast<size_t>(i)], layout.align(),
					layout.x, layout.y, edge, 0, s, node.colors[color_state]);
		} else {
			const WidgetNode::ItemVisual &item =
					node.items[static_cast<size_t>(i)];
			if (item.kind == WidgetNode::ItemVisual::kText) {
				emit_list_row_text_(node, row, item.text, item.layout.align(), item.layout.x,
						item.layout.y, edge, 0, s, node.colors[color_state]);
			}
		}
		y += row_h;
	}
}

// A widget's cursor handle [orig: CWnd_GetInheritedCursorTexture @ 0x646AD0 and
// the pump's inlined copy @ 0x647ad0: its own +0x114, else its topmost
// ancestor's, the root window's]. The handle is what loaded: the CURSOR load
// zeroes it when the texture fails [orig: CTextureManager_LoadOrFindTexture
// @ 0x654980 writes *outHandle = 0 first; the CURSOR arm @ 0x6495b0], so an
// authored FILE that did not load reads as none.
int MenuFrameCompiler::inherited_cursor_owner_(int index) const {
	if (index < 0 || index >= static_cast<int>(nodes_.size())) {
		return -1;
	}
	const WidgetNode &node = nodes_[static_cast<size_t>(index)];
	if (texture_loaded_(node.cursor)) {
		return index;
	}
	return texture_loaded_(nodes_[static_cast<size_t>(node.root)].cursor) ? node.root : -1;
}

int32_t MenuFrameCompiler::inherited_cursor_(int index) const {
	const int owner = inherited_cursor_owner_(index);
	return owner < 0 ? kMenuTexNone : nodes_[static_cast<size_t>(owner)].cursor;
}

// The first root window in document order whose cursor loaded [orig:
// CUIScene_EndFrame @ 0x63e600 — the root loop stops at the first
// CWnd_GetInheritedCursorTexture(root) != 0].
int MenuFrameCompiler::first_root_cursor_owner_() const {
	for (int i = 0; i < document_nodes_; ++i) {
		const WidgetNode &node = nodes_[static_cast<size_t>(i)];
		if (node.parent < 0) {
			const int owner = inherited_cursor_owner_(i);
			if (owner >= 0) {
				return owner;
			}
		}
	}
	return -1;
}

int32_t MenuFrameCompiler::first_root_cursor_() const {
	const int owner = first_root_cursor_owner_();
	return owner < 0 ? kMenuTexNone : nodes_[static_cast<size_t>(owner)].cursor;
}

// The frame's cursor [orig: CWnd_ProcessMouseEvent @ 0x647ad0..0x647b09
// stamps the claimed widget's own-or-root cursor into g_UIFrameCursorTexture;
// CUIScene_EndFrame @ 0x63e600: none stamped -> the capture widget's own-or-root
// (and no further, even when it has none), else the first root window in
// document order that has one]. A spin arrow is its own button: over one, the
// claim stamps the arrow's own-or-root, not the list's [orig:
// CSpinListWnd_CreateUpDownChildren @ 0x64b8b0 attaches the arrows as child
// windows the pump claims]. The capture is a press's (MenuClickLatch: the window
// it captured, a spin arrow's the arrow; an open dropdown's list, a child of the
// combo with no CURSOR of its own, its root's), else the scroll pump's part
// capture (retail's child-button capture of the scrollbar parts).
int MenuFrameCompiler::claim_cursor_owner_(int hovered, int spin_part, int capture,
		int capture_part) const {
	const auto window_of = [this](int index, int part) {
		if (index < 0 || index >= static_cast<int>(nodes_.size())) {
			return index;
		}
		const WidgetNode &node = nodes_[static_cast<size_t>(index)];
		if (part == 1 || part == 2) {
			const int arrow = part == 1 ? node.spin_up : node.spin_down;
			return arrow >= 0 ? arrow : index;
		}
		// The dropdown's list and a scrollbar's windows draw no cursor of their own:
		// the root's [orig: CWnd_ProcessMouseEvent @ 0x647ad0 — own, else the root's].
		return part == kMenuPumpPartDropdown || menu_pump_part_scroll(part) ? node.root : index;
	};
	const int stamped = inherited_cursor_owner_(window_of(hovered, spin_part));
	if (stamped >= 0) {
		return stamped;
	}
	if (capture >= 0 && capture < static_cast<int>(nodes_.size())) {
		return inherited_cursor_owner_(window_of(capture, capture_part));
	}
	return first_root_cursor_owner_();
}

int32_t MenuFrameCompiler::claim_cursor_(int hovered, int spin_part, int capture,
		int capture_part) const {
	const int owner = claim_cursor_owner_(hovered, spin_part, capture, capture_part);
	return owner < 0 ? kMenuTexNone : nodes_[static_cast<size_t>(owner)].cursor;
}

MenuFrameCompiler::FrameCursor MenuFrameCompiler::frame_cursor(const MenuFrameState &state) const {
	FrameCursor out;
	if (screen_ == nullptr || nodes_.empty()) {
		return out;
	}
	// The claim the pump stamped, whatever its visual state [orig: the stamp
	// @ 0x647b09 comes before the pump's visual-state verdict].
	const int claim = state.cursor_claim >= 0 && state.cursor_claim < static_cast<int>(nodes_.size())
			? state.cursor_claim
			: -1;
	out.owner = claim_cursor_owner_(claim, claim >= 0 ? state.cursor_spin_part : 0,
			state.cursor_capture, state.cursor_capture_part);
	if (out.owner < 0) {
		return out;
	}
	out.texture = nodes_[static_cast<size_t>(out.owner)].cursor;
	const auto &size = texture_sizes_[static_cast<size_t>(out.texture)];
	out.width = size.first;
	out.height = size.second;
	return out;
}

// The cursor pass [orig: CUIScene_DrawScreensAndCursor @ 0x63bf60]: the
// frame's cursor (frame_cursor: the stamped claim's) drawn LAST at the raw
// mouse position at native texture size, UNSCALED (the cursor never scales
// with the menu); nothing when no window's cursor loaded (@ 0x63bfa2).
void MenuFrameCompiler::emit_cursor(const MenuFrameState &state) {
	if (!state.cursor_visible) {
		return;
	}
	const FrameCursor cursor = frame_cursor(state);
	if (cursor.owner < 0) {
		return;
	}
	MenuQuad quad;
	quad.x0 = state.cursor_x;
	quad.y0 = state.cursor_y;
	quad.x1 = state.cursor_x + static_cast<float>(cursor.width);
	quad.y1 = state.cursor_y + static_cast<float>(cursor.height);
	quad.texture = cursor.texture;
	// Through the strip like every menu image [orig: CTextureManager_DrawScaledRect
	// @ 0x63c046 at scale 1].
	set_image_uv(quad, cursor.width, cursor.height, 0, cursor.height);
	push_quad(quad);
}

// --- the walk ----------------------------------------------------------------

// The claim walk over a window and its subtree [orig: CWnd_ProcessMouseEvent
// @ 0x647a00, vtable+20, down the tree from CUIScene_EndFrame @ 0x63e600]. A
// hidden window holds nothing and pumps nothing below it (@ 0x647a21;
// CWnd_HitTestPoint @ 0x646706). A window takes the claim where its own rect
// holds the point and none of its shown children's subtrees does (@ 0x647a97..
// 0x647ab7, CWnd_HitTestPoint recursing @ 0x647ab0: a disabled child's rect
// still keeps its parent from the claim), the first such window front to back
// winning (the claim flag scene+16, @ 0x647a7c / 0x647b7f): the last of them in
// the draw's pre-order. It must be visible in the hierarchy, enabled at every
// level up its chain (CWnd_IsVisibleInHierarchy @ 0x646290, the gate
// @ 0x647a27): a disabled window never takes it and a window behind it may
// (D-MNU-33). GLB_TABLE's pump slot pumps its children alone, so it never takes
// the claim (update_table_cell_values @ 0x65e0b0). A widget's own windows are
// its last children: a spin list's arrows, and a scrollbar's windows (a SCROLL
// widget's buttons; the scroll window of a LIST, LAN_LIST or TABLE, its
// buttons in front of its track; menu_click.h), which take the claim as their
// owner's index with their part. The editor's pick (`pump` false) takes every
// shown window by its rect, its arrows its own, its scrollbar its owner's.
int MenuFrameCompiler::hit_walk(int index, int origin_x, int origin_y,
		const MenuFrameState &state, float mx, float my, float sx, float sy,
		bool pump, bool enabled, HitClaim *io_claim, bool *io_holds) const {
	const WidgetNode &node = nodes_[static_cast<size_t>(index)];
	const mnu::Window &w = *node.window;
	const MenuWidgetState *ws = state_for(state, index);
	int next = index + 1;
	// The same shown gate as the draw walk (node_shown); a hidden subtree
	// never hits.
	if (!node_shown(w, ws)) {
		for (size_t c = 0; c < w.children.size(); ++c) {
			next = skip_widget(next);
		}
		return next;
	}
	const mnu::RectEdges local = node_rect_(node, ws);
	const mnu::RectEdges rect = offset_rect(local, origin_x, origin_y);
	const bool live = enabled && !disabled_(w, ws);
	// Raw mouse against the SCALED rect (the same per-element truncation the
	// draw emits with).
	const bool own = mx >= emit_x(rect.left, sx) && mx < emit_x(rect.right, sx) &&
			my >= emit_x(rect.top, sy) && my < emit_x(rect.bottom, sy);
	bool child_holds = false;
	for (size_t c = 0; c < w.children.size(); ++c) {
		next = hit_walk(next, rect.left, rect.top, state, mx, my, sx, sy, pump,
				live, io_claim, &child_holds);
	}
	ScrollParts parts;
	if (w.type == mnu::WindowType::SpinList) {
		// The SPINUP/SPINDOWN arrows are the list's last child windows, hit by
		// their own rects (art or none; a 0x0 arrow is never hit), and shipped
		// menus author them OUTSIDE the parent rect (mp.mnu GAME_TYPE: -18..-2 /
		// 217..233 against a 0..215 widget) — the arrow claims, riding the spin
		// widget's claim with the arrow it is (D-MNU-16)
		// [orig: CSpinListWnd_CreateUpDownChildren @ 0x64b8b0; CWnd_HitTestPoint
		// @ 0x646700].
		const int arrow = spin_arrow_hit_(node, rect, state, mx, my, sx, sy);
		if (arrow != 0) {
			child_holds = true;
			const int arrow_node = arrow == 1 ? node.spin_up : node.spin_down;
			const bool arrow_live = live && arrow_node >= 0 &&
					!arrow_disabled_(arrow_node, state);
			if (!pump || arrow_live) {
				*io_claim = HitClaim{ index, arrow };
			}
		}
	} else if (pump && scroll_windows_(index, state, &parts)) {
		const int part = scroll_window_part_(parts, mx, my, sx, sy);
		const bool standalone = w.type == mnu::WindowType::Scroll;
		// A SCROLL widget's track is the widget's own claim; an owner's scroll
		// window is a child of its own, its track included.
		if (part != 0 && !(standalone && part == kMenuPumpPartScroll)) {
			child_holds = true;
			if (live) {
				*io_claim = HitClaim{ index, part };
			}
		}
	}
	if (own && !child_holds &&
			(!pump || (live && w.type != mnu::WindowType::GlbTable))) {
		*io_claim = HitClaim{ index, 0 };
	}
	*io_holds = *io_holds || own || child_holds;
	return next;
}

MenuFrameCompiler::MouseClaim MenuFrameCompiler::pump_mouse(
		MenuFrameState &io_state, float mouse_x, float mouse_y,
		bool button_down, float scale_x, float scale_y,
		const MenuPumpWindow &capture) {
	MouseClaim claim;
	if (screen_ == nullptr || nodes_.empty()) {
		return claim;
	}
	const bool press = button_down && !pump_down_;
	pump_down_ = button_down;
	// The moves reach the captured window ahead of the frame's pump: a
	// shuttle holding the capture drags (drag_scroll_shuttle_); the press's own
	// sample is no move.
	if (button_down && !press) {
		drag_scroll_shuttle_(io_state, capture, mouse_x, mouse_y, scale_x, scale_y, &claim);
	}
	HitClaim hit;
	// A press's capture holds the claim to the captured window [orig:
	// CWnd_ProcessMouseEvent @ 0x647a88..0x647b02].
	if (capture.valid()) {
		capture_hit_(io_state, capture, mouse_x, mouse_y, scale_x, scale_y, &hit);
	} else {
		hit_roots_(io_state, mouse_x, mouse_y, scale_x, scale_y, true, &hit);
	}
	for (MenuWidgetState &row : io_state.widgets) {
		row.hovered = false;
		row.pressed = false;
		row.spin_part = 0;
	}
	fill_claim_(hit, &claim);
	claim.cursor = claim_cursor_(hit.index, hit.part, capture.index, capture.part);
	// The stamp the cursor pass reads, the claim's own [orig:
	// CWnd_ProcessMouseEvent @ 0x647b09, past the visibility gate @ 0x647a27],
	// and the capture it falls back to.
	io_state.cursor_claim = hit.index;
	io_state.cursor_spin_part = hit.part;
	io_state.cursor_capture = capture.index;
	io_state.cursor_capture_part = capture.part;
	// The claimed widget's visual state (a scrollbar window's own is D-MNU-13's
	// residue).
	if (hit.index < 0 || hit.index >= document_nodes_ || menu_pump_part_scroll(hit.part)) {
		return claim;
	}
	MenuWidgetState *row = nullptr;
	for (MenuWidgetState &candidate : io_state.widgets) {
		if (candidate.index == hit.index) {
			row = &candidate;
			break;
		}
	}
	if (row == nullptr) {
		MenuWidgetState fresh;
		fresh.index = hit.index;
		io_state.widgets.push_back(fresh);
		row = &io_state.widgets.back();
	}
	row->pressed = button_down;
	row->hovered = !button_down;
	row->spin_part = hit.part;
	return claim;
}

// The claim as the MouseClaim reads it: a widget (or its spin arrow), else a
// scrollbar's window by its owner.
void MenuFrameCompiler::fill_claim_(const HitClaim &hit, MouseClaim *claim) {
	if (menu_pump_part_scroll(hit.part)) {
		claim->scroll_owner = hit.index;
		claim->scroll_part = hit.part;
		return;
	}
	claim->hovered = hit.index;
	claim->spin_part = hit.part;
}

int MenuFrameCompiler::widget_index(const std::string &name) const {
	if (name.empty()) {
		return -1;
	}
	for (size_t i = 0; i < static_cast<size_t>(document_nodes_); ++i) {
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

int MenuFrameCompiler::walk_widget(int index, int origin_x, int origin_y,
		const MenuFrameState &state, const WalkScale &s) {
	const WidgetNode &node = nodes_[static_cast<size_t>(index)];
	const mnu::Window &w = *node.window;
	// A part's row is its own (what code or an ACTION wrote on the window a lookup found in the
	// spin list, MenuRuntime::frame_index); its claim rides its spin list's.
	const MenuWidgetState *ws = state_for(state, index);
	int next = index + 1;
	// The shown gate (node_shown) [orig: every Draw impl early-outs on the
	// shown flag +224]. A hidden widget's subtree still consumes its indices.
	// A RADIOEDIT's render alone has no gate: it renders its children, each by
	// its own flag (CWnd_IsShown @ 0x646280 reads the window's own), its radio
	// made shown at its create [orig: 0x65d310, the RADIOEDIT's render, no
	// CWnd_IsShown call; sub_65D210 @ 0x65d2aa], so a hidden RADIOEDIT still
	// draws its radio and what it holds (the pump still skips it).
	if (!node_shown(w, ws) && w.type != mnu::WindowType::RadioEdit) {
		for (size_t c = 0; c < w.children.size(); ++c) {
			next = skip_widget(next);
		}
		return next;
	}
	const mnu::RectEdges local = node_rect_(node, ws);
	const mnu::RectEdges rect = offset_rect(local, origin_x, origin_y);
	// +236 as the pump leaves it: the verdict, then the availability fallback
	// (an authored state kept, else DEFAULT, else -1) [orig:
	// CWnd_ProcessMouseEvent @ 0x647c89..0x647cb5]. Every render reads it
	// for the appearance AND the label colors (-1 colors as DEFAULT: the
	// color switch @ 0x653445 sends anything but 1, 2, 3 to the default pair).
	const MenuWidgetState *pump_ws = node.part ? state_for(state, node.parent) : ws;
	const int visual = appearance_state_with_fallback(node, pump_visual_state(node, pump_ws, ws));
	const int color_state = visual < 0 ? kStateDefault : visual;
	const bool checked =
			ws != nullptr && ws->has_checked ? ws->checked : w.checked;
	++draw_list_.widgets_drawn;
	const int32_t clip_first = ws != nullptr && ws->has_clip
			? static_cast<int32_t>(draw_list_.draw_ops.size())
			: -1;
	switch (w.type) {
		case mnu::WindowType::Static:
		case mnu::WindowType::Button: {
			// [orig: CStaticWnd_Render @ 0x657b10 — frame -> appearance ->
			//  text -> children]
			if (w.draw_frame) {
				emit_frame(node, rect, s);
			}
			emit_appearance(node, rect, s, visual);
			emit_widget_text(node, rect, s, color_state, ws, -1);
			break;
		}
		// A RADIOEDIT draws nothing of its own: its render draws its parts [orig: 0x65d310, the
		// RADIOEDIT's render (IDB CEffect_SetParamFromTypedData), its children alone], a RADIOEDIT_EDIT
		// made hidden and a RADIOEDIT_RADIO made shown, each parsed from the RADIOEDIT's own element at
		// its origin [orig: sub_65D210 @ 0x65d210, the edit @ 0x65d259 / 0x65d25f, the radio @ 0x65d29f
		// / 0x65d2aa], so at rest it is the radio of its element (the swap to the edit on a second
		// activation, CRadioEditWnd_HandleEvent @ 0x65d589, is not drawn: D-MNU-13).
		case mnu::WindowType::RadioEdit:
		case mnu::WindowType::Radio: {
			// [orig: CRadioWnd_Render @ 0x656e20 — checked forces +236 = 3
			//  around the WHOLE static render (@ 0x656e33), with no availability
			//  check: appearance AND label colors]
			const int forced = checked ? kStateSelected : visual;
			if (w.draw_frame) {
				emit_frame(node, rect, s);
			}
			emit_appearance(node, rect, s, forced);
			emit_widget_text(node, rect, s, forced < 0 ? kStateDefault : forced, ws, -1);
			break;
		}
		case mnu::WindowType::CheckBox: {
			// [orig: CCheckWnd_Render @ 0x64ae20 — with a state at all (+236
			//  not -1), checked draws slot 3 DIRECTLY (no availability check:
			//  an unauthored selected slot draws nothing); the label uses +236]
			if (w.draw_frame) {
				emit_frame(node, rect, s);
			}
			const int slot = visual >= 0 && checked ? kStateSelected : visual;
			emit_appearance(node, rect, s, slot);
			emit_checkbox_label(node, rect, s, color_state, ws);
			break;
		}
		case mnu::WindowType::Edit: {
			// [orig: CEditWnd_Render @ 0x6619e0 — focus (non-readonly)
			//  forces state 2 for appearance AND colors, with no availability
			//  check]
			const bool focused = ws != nullptr && ws->focused && !w.readonly;
			const int edit_state = focused ? kStateMouseover : visual;
			if (w.draw_frame) {
				emit_frame(node, rect, s);
			}
			emit_appearance(node, rect, s, edit_state);
			emit_edit(index, node, rect, s, edit_state < 0 ? kStateDefault : edit_state,
					state, ws);
			break;
		}
		case mnu::WindowType::MultilineEdit: {
			// [orig: CMEditWnd_Render @ 0x6608e0 — frame -> appearance for
			//  +236 (no focus forcing, unlike the single-line sibling) -> the
			//  wrapped drawer -> children]
			if (w.draw_frame) {
				emit_frame(node, rect, s);
			}
			emit_appearance(node, rect, s, visual);
			emit_multiline_edit(node, rect, s, color_state, state, ws);
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
			// [orig: CSpinListWnd_Render @ 0x64b220 — frame -> appearance ->
			//  the item cell -> children (the arrows are its last children)]
			if (w.draw_frame) {
				emit_frame(node, rect, s);
			}
			emit_appearance(node, rect, s, visual);
			emit_item_cell(node, local, rect, s, ws);
			break;
		}
		case mnu::WindowType::List:
		case mnu::WindowType::LanList: {
			// [orig: CListWnd_DrawItems @ 0x643f30 — frame -> appearance ->
			//  rows -> children]
			if (w.draw_frame) {
				emit_frame(node, rect, s);
			}
			emit_appearance(node, rect, s, visual);
			emit_list_rows(node, rect, s, ws);
			emit_row_scrollbar_(index, node, rect, s, state, ws);
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
			emit_appearance(node, rect, s, visual);
			// The swapped row text draws unconditionally — an empty selected
			// row leaves a blank face, never the widget's own authored TEXT.
			const std::string face = combo_face_text(node, ws);
			emit_widget_text(node, rect, s, color_state, ws, -1, &face);
			if (ws != nullptr && ws->popup_open) {
				// Deferred to the post-walk overlay pass: retail's witnessed
				// walk paints popups inline yet renders them on top (that
				// mechanism is unwalked); open popups draw menu-top by the
				// D-MNU-12 decision, as the Control-tree overlay did.
				deferred_popups_.push_back(index);
			}
			break;
		}
		case mnu::WindowType::Scroll: {
			// The track COLOR/OUTLINE use the full widget, its IMAGE uses the
			// middle span, and the two arrow children plus shuttle share this rect.
			// [orig: CScrollWnd_Render @ 0x64c5c0; scroll COLOR sink CUIWidget_DrawFullUVQuad @ 0x64ce70;
			// scroll IMAGE sink CUIWidget_DrawInsetImageQuad @ 0x64cf70; CUIScrollbar_CalcThumbRect
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
						range_min, range_max, page, value, visual);
			}
			break;
		}
		case mnu::WindowType::Table: {
			// [orig: CUITable_Render @ 0x6411d0 — frame -> appearance ->
			//  header + the sort indicator -> data rows -> children]
			if (w.draw_frame) {
				emit_frame(node, rect, s);
			}
			emit_appearance(node, rect, s, visual);
			emit_table(index, node, rect, s, ws);
			emit_row_scrollbar_(index, node, rect, s, state, ws);
			break;
		}
		case mnu::WindowType::Marquee: {
			// [orig: CMarqueeWnd_Render @ 0x65cf90 — frame -> appearance ->
			//  the credits scroller -> children; no label pass]
			if (w.draw_frame) {
				emit_frame(node, rect, s);
			}
			emit_appearance(node, rect, s, visual);
			emit_marquee(index, node, rect, s, state, ws);
			break;
		}
		default: {
			// [orig: CUIElement_Draw @ 0x64a8a0 — appearance BEFORE frame
			//  for generic containers, the CUSTOM pass last among them]
			emit_appearance(node, rect, s, visual);
			mark_custom_slot_(index, node, visual, state);
			if (w.draw_frame) {
				emit_frame(node, rect, s);
			}
			break;
		}
	}
	// The widget's own passes ran inside its clip viewport; the viewport is
	// restored before the children draw [orig: CStaticWnd_Render @ 0x657b10 —
	// CWnd_ApplyClipViewport @0x657b22, CWnd_RestoreViewport @0x657be8].
	if (clip_first >= 0) clip_ops_(clip_first, ws->clip, s);
	// Children in authored array order [orig: the forward child walk — later
	// siblings paint over earlier ones].
	for (size_t c = 0; c < w.children.size(); ++c) {
		next = walk_widget(next, rect.left, rect.top, state, s);
	}
	// A spin list's arrows: its last children, whole buttons drawn by the
	// static render (frame, appearance for their own state, label) [orig:
	// CSpinListWnd_CreateUpDownChildren @ 0x64b8b0 appends them; the arrow
	// vtable's +24 is CStaticWnd_Render @ 0x657b10].
	for (const int arrow : {node.spin_up, node.spin_down}) {
		if (arrow >= 0) {
			walk_widget(arrow, rect.left, rect.top, state, s);
		}
	}
	if (index == state.mount_index && mount_split_ < 0)
		mount_split_ = static_cast<int32_t>(draw_list_.draw_ops.size());
	return next;
}

void MenuFrameCompiler::mark_custom_slot_(int index, const WidgetNode &node,
		int appearance_slot, const MenuFrameState &state) {
	if (index != state.custom_slot_index || appearance_slot < 0 || appearance_slot > 3) return;
	if (!node.states[appearance_slot].custom) return;
	draw_list_.custom_slot_op = static_cast<int32_t>(draw_list_.draw_ops.size());
}

const MenuDrawList &MenuFrameCompiler::compile(const MenuFrameState &state,
		float scale_x, float scale_y) {
	draw_list_.quads.clear();
	draw_list_.lines.clear();
	draw_list_.glyphs.clear();
	draw_list_.underlines.clear();
	draw_list_.font_runs.clear();
	draw_list_.draw_ops.clear();
	draw_list_.overlay_op_start = 0;
	draw_list_.custom_slot_op = -1;
	draw_list_.widgets_drawn = 0;
	if (screen_ == nullptr || nodes_.empty()) {
		return draw_list_;
	}
	WalkScale s;
	s.x = scale_x;
	s.y = scale_y;
	deferred_popups_.clear();
	mount_split_ = -1;
	// Every root window, forward [orig: CUIScene_DrawScreensAndCursor
	// @ 0x63bf60].
	for (int next = 0; next < document_nodes_;) {
		next = walk_widget(next, 0, 0, state, s);
	}
	// Everything from here on is the menu-top overlay (popups, then cursor),
	// and with a mounted widget everything after its subtree too.
	draw_list_.overlay_op_start = mount_split_ >= 0
			? mount_split_
			: static_cast<int32_t>(draw_list_.draw_ops.size());
	if (draw_list_.custom_slot_op >= 0 && draw_list_.custom_slot_op < draw_list_.overlay_op_start)
		draw_list_.overlay_op_start = draw_list_.custom_slot_op;
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

