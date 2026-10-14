// The writer: each record as the element it puts down (the writer's words), the writer's own
// layout, the write issues, and the serialization entry points. The file is written from the
// model alone (D-MNU-22): no original tool writes a menu, so the layout is OpenNova's own.
// docs/mnu/menu-re.md ("The writer") has the rules with their witnesses.
#include <formats/mnu/mnu.h>
#include <formats/mnu/mnu_layout.h>
#include <formats/mnu/mnu_schema.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include <base/io/strutil.h>

namespace opennova::mnu {

namespace {

using opennova::strutil::iequals;

// One attribute the writer puts down: ` name="value"`, or ` name` when bare.
struct WrittenAttribute {
	std::string name;
	std::string value;
	bool bare = false;
	// What follows the name: `="value"`, or nothing when bare.
	std::string value_token() const { return bare ? std::string() : "=\"" + value + "\""; }
};

// How the writer's layout puts an element down.
enum class WrittenForm : uint8_t {
	Leaf,      // its text and its close tag on its line (the text is the model's)
	Container, // each element in it on a line of its own, then its close tag on its own line
	Compact,   // its text, then its elements, all on its line (an extra element holding both)
};

// A record as the element the writer puts down for it.
struct WrittenElement {
	std::string tag;
	WrittenForm form = WrittenForm::Leaf;
	std::vector<WrittenAttribute> attributes;
	std::string text; // the model's text (escape_text writes it)
	std::vector<WrittenElement> children;
};

// The writer's layout: a line ending and the step each level of elements is indented by (both
// empty for one line, no indentation).
struct WrittenStyle {
	std::string eol;
	std::string unit;
};

// --- the writer's words ----------------------------------------------------------------------------

void put(std::vector<WrittenAttribute> &a, const char *name, const std::string &value) {
	if (!value.empty()) a.push_back({name, value, false});
}
void put_int(std::vector<WrittenAttribute> &a, const char *name, bool has, int value) {
	if (has) a.push_back({name, std::to_string(value), false});
}
void put_bare(std::vector<WrittenAttribute> &a, const char *name, bool on) {
	if (on) a.push_back({name, std::string(), true});
}

WrittenElement element(const std::string &tag, WrittenForm form) {
	WrittenElement e;
	e.tag = tag;
	e.form = form;
	return e;
}

WrittenElement leaf(const char *tag, const std::string &text) {
	WrittenElement e = element(tag, WrittenForm::Leaf);
	e.text = text;
	return e;
}

bool is_space_text(const std::string &text) {
	return std::all_of(text.begin(), text.end(), [](char c) {
		return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
	});
}

// Whether a window of `type` reads the field or list at `path` (schema_reads): the writer puts down
// what the window's type reads alone, as the reader keeps it.
bool reads(WindowType type, const char *path) { return schema_reads(type, path) == SchemaApplies::Reads; }

// A keyword's token as its parse's table spells it (the compare is without case); any other token is
// none the parse reads (a code it sets nothing by), written as none.
std::string canon(const std::string &value, const char *const *tokens) {
	for (const char *const *t = tokens; *t; ++t)
		if (iequals(value, *t)) return *t;
	return std::string();
}

// A number the parse reads with wcstol, written as strtol's decimal of the model's text; "" none.
std::string decimal(const std::string &value) {
	if (value.empty()) return value;
	return std::to_string(static_cast<int>(std::strtol(value.c_str(), nullptr, 10)));
}

// A JUSTIFY / VJUSTIFY pair: the keyword as its table spells it and, apart, the offset's decimal, a second
// attribute of the name (the parse keeps the first authored of each kind).
void put_justification(std::vector<WrittenAttribute> &a, const char *name, const std::string &keyword,
                       const std::string &offset, const char *const *tokens) {
	put(a, name, canon(keyword, tokens));
	put(a, name, decimal(offset));
}

// A FLAGS word's text where it is not the default (none authored reads the default).
void put_flags(std::vector<WrittenAttribute> &a, const char *name, uint32_t flags) {
	if (flags != kDefaultUiMaterialFlags) a.push_back({name, ui_material_flags_text(flags), false});
}

bool empty_row(const Appearance &a) {
	return a.state.empty() && a.type.empty() && a.value.empty() && !a.has_map_state && !a.has_height &&
	       a.flags == kDefaultUiMaterialFlags;
}

// Where the writer puts a table sort key so that retail's HEADER walk sets it to the
// model's index: on the first HEADER whose index is that value, before its COLUMN
// (`own`), else after the COLUMN of the first HEADER the running index reaches it
// before (the HEADERs are written first in the COLUMN element, so the index starts at
// 0). `placed` is false when no HEADER reaches the value.
struct SortKeyPlace {
	bool slot = false; // the key is set
	bool placed = false;
	size_t header = 0;
	bool own = true;
};

std::vector<SortKeyPlace> sort_key_places(const TableColumn &c) {
	std::vector<int> index(c.headers.size()), before(c.headers.size());
	int running = 0;
	for (size_t i = 0; i < c.headers.size(); ++i) {
		before[i] = running;
		if (c.headers[i].has_column) running = c.headers[i].column;
		index[i] = running;
	}
	std::vector<SortKeyPlace> places(3);
	const int values[] = {c.primary_sort, c.secondary_sort, c.tertiary_sort};
	for (int k = 0; k < 3; ++k) {
		SortKeyPlace &place = places[static_cast<size_t>(k)];
		place.slot = values[k] >= 0;
		if (!place.slot) continue;
		for (size_t i = 0; i < c.headers.size() && !place.placed; ++i)
			if (index[i] == values[k]) place = {true, true, i, true};
		for (size_t i = 0; i < c.headers.size() && !place.placed; ++i)
			if (before[i] == values[k] && c.headers[i].has_column) place = {true, true, i, false};
	}
	return places;
}

class Words {
public:
	WrittenElement screen(const Screen &s);
	void window_elements(const Window &w, std::vector<WrittenElement> &out);

private:
	WrittenElement window(const Window &w, const char *tag, bool part, std::vector<WrittenAttribute> extra = {});
	void appearance(const Appearance &a, const char *tag, std::vector<WrittenElement> &out, bool table = false);
	WrittenElement item(const Item &i, bool cell, WindowType type);
	void items(const Items &items, WindowType type, std::vector<WrittenElement> &out);
	void column(const TableColumn &column, std::vector<WrittenElement> &out);
	void glb_fields(const GlbFields &fields, WrittenElement &out);
	WrittenElement category(const GopherCategory &category);
	// The GLB_TABLE browser's tables: their HEADERs carry FIELD.
	bool glb_header_fields_ = false;
};

void Words::appearance(const Appearance &a, const char *tag, std::vector<WrittenElement> &out, bool table) {
	if (empty_row(a)) return; // nothing to write (an element with no attributes stops the parse)
	WrittenElement e = element(tag, WrittenForm::Leaf);
	put(e.attributes, "type", canon(a.type, table ? kTableAppearanceTypes : kAppearanceTypes));
	put(e.attributes, "state", canon(a.state, kAppearanceStates));
	put_int(e.attributes, "map_state", a.has_map_state, a.map_state);
	put_int(e.attributes, "height", a.has_height, a.height);
	put_flags(e.attributes, "flags", a.flags);
	e.text = appearance_text(a);
	out.push_back(std::move(e));
}

WrittenElement Words::item(const Item &i, bool cell, WindowType type) {
	WrittenElement e = element("ITEM", WrittenForm::Leaf);
	put(e.attributes, "type", canon(i.type, item_types_for(type, cell)));
	const std::string value = decimal(i.value);
	put(e.attributes, "value", value == "0" ? std::string() : value);
	put_justification(e.attributes, "justify", i.justify, i.justify_offset, kJustify);
	put_justification(e.attributes, "vjustify", i.vjustify, i.vjustify_offset, kVJustify);
	put_bare(e.attributes, "PAIRS_LIST", !cell && reads(type, "items.item.pairs_list") && i.pairs_list);
	if (cell) put_int(e.attributes, "column", i.has_column, i.column);
	e.text = item_text(i);
	return e;
}

void Words::items(const Items &items, WindowType type, std::vector<WrittenElement> &out) {
	if (!items.present || !reads(type, "items")) return;
	WrittenElement e = element("ITEMS", WrittenForm::Container);
	if (reads(type, "items.justify")) {
		put_justification(e.attributes, "justify", items.justify, items.justify_offset, kJustify);
		put_justification(e.attributes, "vjustify", items.vjustify, items.vjustify_offset, kVJustify);
	}
	put_bare(e.attributes, "MULTISELECT", reads(type, "items.multiselect") && items.multiselect);
	if (reads(type, "items.appearance"))
		for (const Appearance &a : items.appearances) appearance(a, "APPEARANCE", e.children, type == WindowType::Table);
	if (reads(type, "items.item"))
		for (const Item &i : items.items) e.children.push_back(item(i, false, type));
	if (reads(type, "items.row"))
		for (const TableRow &row : items.rows) {
			WrittenElement r = element("ROW", WrittenForm::Container);
			for (const Item &cell : row.cells) r.children.push_back(item(cell, true, type));
			e.children.push_back(std::move(r));
		}
	out.push_back(std::move(e));
}

void Words::column(const TableColumn &c, std::vector<WrittenElement> &out) {
	const std::vector<SortKeyPlace> places = sort_key_places(c);
	const bool keys = places[0].slot || places[1].slot || places[2].slot;
	if (!c.has_count && !c.has_spacing && c.headers.empty() && c.bodies.empty() && c.substitutions.empty() && !keys)
		return;
	WrittenElement e = element("COLUMN", WrittenForm::Container);
	put_int(e.attributes, "count", c.has_count, c.count);
	put_int(e.attributes, "spacing", c.has_spacing, c.spacing);
	const char *const key_tokens[] = {kPrimarySortTokens[0], "SECONDARY_SORT", "TERTIARY_SORT"};
	for (size_t i = 0; i < c.headers.size(); ++i) {
		const TableHeader &h = c.headers[i];
		WrittenElement header = element("HEADER", WrittenForm::Leaf);
		// A sort key written before COLUMN takes this HEADER's index, one written after it
		// the index before (the walk runs last authored first).
		std::vector<WrittenAttribute> before, after;
		for (int k = 0; k < 3; ++k)
			if (places[k].slot && places[k].placed && places[k].header == i)
				(places[k].own ? before : after).push_back({key_tokens[k], std::string(), true});
		put(header.attributes, "justify", canon(h.justify, kJustify));
		put(header.attributes, "vjustify", canon(h.vjustify, kVJustify));
		header.attributes.insert(header.attributes.end(), before.begin(), before.end());
		put_int(header.attributes, "column", h.has_column, h.column);
		header.attributes.insert(header.attributes.end(), after.begin(), after.end());
		put(header.attributes, "sort", h.sort == "1" ? "1" : !h.sort.empty() && (h.sort[0] == 'A' || h.sort[0] == 'a') ? "A" : "");
		put_int(header.attributes, "width", h.has_width, h.width);
		put(header.attributes, "type", canon(h.type, kStringTypes));
		if (glb_header_fields_) put(header.attributes, "FIELD", h.field);
		header.text = h.text;
		e.children.push_back(std::move(header));
	}
	for (const TableBody &b : c.bodies) {
		WrittenElement body = element("BODY", WrittenForm::Leaf);
		put_justification(body.attributes, "justify", b.justify, b.justify_offset, kJustify);
		put_justification(body.attributes, "vjustify", b.vjustify, b.vjustify_offset, kVJustify);
		put_int(body.attributes, "column", b.has_column, b.column);
		// The draw kind retail keeps is the first authored of the three: written first.
		const bool on[] = {b.custom_draw, b.bitmap_draw, b.bitmap_text};
		for (int k = 0; k < 3; ++k)
			if (on[k] && iequals(b.display, kBodyDisplays[k])) put_bare(body.attributes, kBodyDisplays[k], true);
		for (int k = 0; k < 3; ++k)
			if (on[k] && !iequals(b.display, kBodyDisplays[k])) put_bare(body.attributes, kBodyDisplays[k], true);
		put_flags(body.attributes, "BITMAP_FLAGS", b.bitmap_flags);
		put_bare(body.attributes, "SCALE_BITMAP", b.scale_bitmap);
		e.children.push_back(std::move(body));
	}
	for (const TableSubst &s : c.substitutions) {
		WrittenElement subst = element("SUBST", WrittenForm::Leaf);
		put_int(subst.attributes, "column", s.has_column, s.column);
		put(subst.attributes, "value", s.value);
		put_bare(subst.attributes, "URL", s.is_url);
		put_bare(subst.attributes, "FILE", s.is_file);
		subst.text = s.file;
		e.children.push_back(std::move(subst));
	}
	out.push_back(std::move(e));
}

// [orig: CLanGameBrowser_ParseXMLFieldDefinitions @ 0x65d850] count, the FORMAT (CR and LF as the \r
// and \n pairs the parse reads them from), each slot's FIELD.
void Words::glb_fields(const GlbFields &fields, WrittenElement &out) {
	if (!fields.present) return;
	WrittenElement e = element("FIELDS", WrittenForm::Container);
	put_int(e.attributes, "count", fields.has_count, fields.count);
	if (fields.has_format) {
		std::string text;
		for (const char c : fields.format) text += c == '\r' ? std::string("\\r") : c == '\n' ? std::string("\\n") : std::string(1, c);
		e.children.push_back(leaf("FORMAT", text));
	}
	for (const GlbFieldSlot &f : fields.fields) {
		WrittenElement field = element("FIELD", WrittenForm::Leaf);
		put_int(field.attributes, "which", true, f.which);
		field.text = f.text;
		e.children.push_back(std::move(field));
	}
	out.children.push_back(std::move(e));
}

// [orig: CGopherWnd_ParseCategoryXML @ 0x65a540] LEVEL; LABEL (the category static's elements), the
// ITEM and INFO rows.
WrittenElement Words::category(const GopherCategory &c) {
	WrittenElement e = element("CATEGORY", WrittenForm::Container);
	if (c.has_level) put(e.attributes, "LEVEL", c.level);
	if (c.label) e.children.push_back(window(*c.label, "LABEL", true));
	for (const GopherEntry &entry : c.entries) {
		WrittenElement row = element(entry.info ? "INFO" : "ITEM", WrittenForm::Leaf);
		put(row.attributes, "NAME", entry.name);
		if (!entry.info) put(row.attributes, "TARGET", entry.target);
		put(row.attributes, "FILTER", entry.filter);
		put(row.attributes, "TABLE", entry.table);
		row.text = entry.text;
		e.children.push_back(std::move(row));
	}
	return e;
}

WrittenElement Words::window(const Window &w, const char *tag, bool part, std::vector<WrittenAttribute> extra_attributes) {
	WrittenElement e = element(tag, WrittenForm::Container);
	e.attributes = std::move(extra_attributes);
	// The type's own name [orig: CUIScene_CreateWidgetByType @ 0x64f630 matches it without case].
	if (!part) put(e.attributes, "type", window_type_name(w.type));
	const bool edit = reads(w.type, "password");
	put(e.attributes, "name", w.name);
	put_bare(e.attributes, "DRAW_FRAME", w.draw_frame);
	put_bare(e.attributes, "HIDDEN", w.hidden);
	put_bare(e.attributes, "MODAL", w.modal);
	put_bare(e.attributes, "READONLY", edit && w.readonly);
	put_bare(e.attributes, "DISABLE", w.disabled);
	put_bare(e.attributes, "CHECKED", reads(w.type, "checked") && w.checked);
	put_bare(e.attributes, "AS_BUTTON", reads(w.type, "as_button") && w.as_button);
	put_bare(e.attributes, "NUMBER", edit && w.number);
	put_int(e.attributes, "MINVAL", edit && w.has_minval, w.minval);
	put_int(e.attributes, "MAXVAL", edit && w.has_maxval, w.maxval);
	put_int(e.attributes, "MAXCHAR", edit && w.has_maxchar, w.maxchar);
	put_bare(e.attributes, "GLOBAL_VAR", w.global_var);
	put_bare(e.attributes, "PASSWORD", edit && w.password);
	put_int(e.attributes, "FORM", w.has_form, w.form);
	put_bare(e.attributes, "PLAYERLIST", reads(w.type, "glb_playerlist") && w.glb_playerlist);
	window_elements(w, e.children);
	return e;
}

void Words::window_elements(const Window &w, std::vector<WrittenElement> &out) {
	const WindowType t = w.type;
	if (w.has_group && reads(t, "group")) out.push_back(leaf("GROUP", std::to_string(w.group)));
	for (const Hotkey &h : w.hotkeys) {
		WrittenElement e = element("HOTKEY", WrittenForm::Leaf);
		put_bare(e.attributes, "VIRTUAL", h.virtual_key);
		e.text = h.value;
		out.push_back(std::move(e));
	}
	for (const Action &a : w.actions) {
		WrittenElement e = element("ACTION", WrittenForm::Leaf);
		put(e.attributes, "type", canon(a.type, kActionTypes));
		put(e.attributes, "state", canon(a.state, kActionStates));
		put(e.attributes, "file", a.file);
		if (!a.field.empty()) e.attributes.push_back({kActionFieldAttributes[0], a.field, false});
		put_int(e.attributes, "target_form", a.has_target_form, a.target_form);
		put_bare(e.attributes, "TOGGLE", a.toggle);
		put(e.attributes, "test", canon(a.test, kActionTests + 1)); // LT is the parse's 0, as none
		put_bare(e.attributes, "EXTERNAL_BROWSER", a.external_browser);
		e.text = a.target;
		out.push_back(std::move(e));
	}
	const Frame &f = w.frame;
	const bool has_stencil = !f.stencil.empty() || f.has_stencil_size || f.has_insetx || f.has_insety;
	if (has_stencil || !f.brush.empty() || !f.monogram.empty()) {
		WrittenElement frame = element("FRAME", WrittenForm::Container);
		if (has_stencil) {
			WrittenElement stencil = element("STENCIL", WrittenForm::Leaf);
			put_int(stencil.attributes, "size", f.has_stencil_size, f.stencil_size);
			put_int(stencil.attributes, "insetx", f.has_insetx, f.insetx);
			put_int(stencil.attributes, "insety", f.has_insety, f.insety);
			stencil.text = f.stencil;
			frame.children.push_back(std::move(stencil));
		}
		if (!f.brush.empty()) frame.children.push_back(leaf("BRUSH", f.brush));
		if (!f.monogram.empty()) frame.children.push_back(leaf("MONOGRAM", f.monogram));
		out.push_back(std::move(frame));
	}
	const Position &pos = w.position;
	if (pos.has_left || pos.has_top || pos.has_right || pos.has_bottom) {
		WrittenElement position = element("POSITION", WrittenForm::Container);
		const std::pair<const char *, std::pair<bool, int>> edges[] = {{"LEFT", {pos.has_left, pos.left}},
		                                                                {"TOP", {pos.has_top, pos.top}},
		                                                                {"RIGHT", {pos.has_right, pos.right}},
		                                                                {"BOTTOM", {pos.has_bottom, pos.bottom}}};
		for (const auto &edge : edges) {
			if (!edge.second.first) continue;
			position.children.push_back(leaf(edge.first, std::to_string(edge.second.second)));
		}
		out.push_back(std::move(position));
	}
	const bool scroll = reads(t, "shuttle");
	if (scroll && w.has_scroll_extent)
		out.push_back(leaf(w.scroll_extent_is_width ? "WIDTH" : "HEIGHT", std::to_string(w.scroll_extent)));
	if (scroll && !w.orientation.empty()) out.push_back(leaf("ORIENTATION", w.orientation));
	for (const Appearance &a : w.appearances) appearance(a, "APPEARANCE", out);
	if (scroll) {
		for (const Appearance &a : w.shuttle) appearance(a, "SHUTTLE", out);
		for (const Appearance &a : w.scrollup) appearance(a, "SCROLLUP", out);
		for (const Appearance &a : w.scrolldown) appearance(a, "SCROLLDOWN", out);
	}
	if (w.has_text_rsrc) out.push_back(leaf("TEXT_RSRC", w.text_rsrc));
	if (reads(t, "datasource"))
		for (const std::string &datasource : w.datasources) out.push_back(leaf("DATASOURCE", datasource));
	if (!w.private_data.empty()) out.push_back(leaf("PRIVATE_DATA", w.private_data));
	if (!w.cursor.file.empty() || w.cursor.flags != kDefaultUiMaterialFlags) {
		WrittenElement cursor = element("CURSOR", WrittenForm::Container);
		if (!w.cursor.file.empty()) cursor.children.push_back(leaf("FILE", w.cursor.file));
		if (w.cursor.flags != kDefaultUiMaterialFlags)
			cursor.children.push_back(leaf("FLAGS", ui_material_flags_text(w.cursor.flags)));
		out.push_back(std::move(cursor));
	}
	const Font &font = w.font;
	if (!font.empty()) {
		WrittenElement e = element("FONT", WrittenForm::Container);
		if (!font.name.empty()) e.children.push_back(leaf("NAME", font.name));
		// Each colour authored, as color_word writes it: its %NAME%, else eight upper-case hex digits.
		const std::pair<const char *, const Color *> slots[] = {
		    {"DEFAULT_FG", &font.default_fg},
		    {"DEFAULT_BG", &font.default_bg},
		    {"MOUSEOVER_FG", &font.mouseover_fg},
		    {"MOUSEOVER_BG", &font.mouseover_bg},
		    {"SELECTED_FG", &font.selected_fg},
		    {"SELECTED_BG", &font.selected_bg},
		    {"DISABLED_FG", &font.disabled_fg},
		    {"DISABLED_BG", &font.disabled_bg},
		};
		for (const auto &slot : slots)
			if (slot.second->present) e.children.push_back(leaf(slot.first, color_word(*slot.second)));
		out.push_back(std::move(e));
	}
	const String &s = w.string_data;
	if (s.present && reads(t, "string")) {
		WrittenElement e = element("STRING", WrittenForm::Leaf);
		put(e.attributes, "type", canon(s.type, kStringTypes));
		put_justification(e.attributes, "justify", s.justify, s.justify_offset, kJustify);
		put_justification(e.attributes, "vjustify", s.vjustify, s.vjustify_offset, kVJustify);
		put_int(e.attributes, "edge", s.has_edge, s.edge);
		put_bare(e.attributes, "WRAP", s.wrap);
		e.text = s.value;
		out.push_back(std::move(e));
	}
	if (w.toggle_string.present && reads(t, "toggle_string")) {
		WrittenElement e = element("TOGGLE_STRING", WrittenForm::Leaf);
		put(e.attributes, "type", canon(w.toggle_string.type, kStringTypes));
		e.text = w.toggle_string.value;
		out.push_back(std::move(e));
	}
	for (const Sound &snd : w.sounds) {
		WrittenElement e = element("SOUND", WrittenForm::Leaf);
		put(e.attributes, "state", canon(snd.state, kSoundStates));
		put(e.attributes, "trigger", snd.trigger);
		e.text = snd.file;
		out.push_back(std::move(e));
	}
	items(w.items, t, out);
	// The combo's sb_edge_pad rides on the LIST_BOX element with the list's own attributes.
	if (w.list_box && reads(t, "list_box")) {
		std::vector<WrittenAttribute> pad;
		put_int(pad, "sb_edge_pad", w.has_sb_edge_pad, w.sb_edge_pad);
		out.push_back(window(*w.list_box, "LIST_BOX", true, std::move(pad)));
	}
	if (w.spinup && reads(t, "spinup")) out.push_back(window(*w.spinup, "SPINUP", true));
	if (w.spindown && reads(t, "spindown")) out.push_back(window(*w.spindown, "SPINDOWN", true));
	if (reads(t, "column")) column(w.table_data.column, out);
	if (w.table_data.has_min_item_height && reads(t, "min_item_height"))
		out.push_back(leaf("MIN_ITEM_HEIGHT", std::to_string(w.table_data.min_item_height)));
	if (w.table_data.has_fixed_header_height && reads(t, "fixed_header_height"))
		out.push_back(leaf("FIXED_HEADER_HEIGHT", std::to_string(w.table_data.fixed_header_height)));
	if (w.scrollbar && reads(t, "scrollbar")) out.push_back(window(*w.scrollbar, "SCROLLBAR", true));
	if (w.join_button && reads(t, "join_button")) out.push_back(window(*w.join_button, "JOIN_BUTTON", true));
	if (w.search_button && reads(t, "search_button")) out.push_back(window(*w.search_button, "SEARCH_BUTTON", true));
	if (reads(t, "glb_table")) {
		const bool saved = glb_header_fields_;
		glb_header_fields_ = true;
		if (w.glb_table) out.push_back(window(*w.glb_table, "GLB_TABLE", true));
		if (w.glb_info_player_details) out.push_back(window(*w.glb_info_player_details, "GLB_INFO_PLAYER_DETAILS", true));
		glb_header_fields_ = saved;
		if (w.glb_info_button) out.push_back(window(*w.glb_info_button, "GLB_INFO_BUTTON", true));
		if (w.glb_info_server_details) {
			WrittenElement details = window(*w.glb_info_server_details, "GLB_INFO_SERVER_DETAILS", true);
			glb_fields(w.glb_server_fields, details);
			out.push_back(std::move(details));
		}
		if (w.glb_nw_info) out.push_back(window(*w.glb_nw_info, "GLB_NW_INFO", true));
		if (w.has_glb_target) out.push_back(leaf("TARGET", w.glb_target));
	}
	if (reads(t, "gopher_targets"))
		for (const std::string &target : w.gopher_targets) out.push_back(leaf("TARGET", target));
	if (reads(t, "gopher_categories") && !w.gopher_categories.empty()) {
		WrittenElement filters = element("FILTERS", WrittenForm::Container);
		for (const GopherCategory &c : w.gopher_categories) filters.children.push_back(category(c));
		out.push_back(std::move(filters));
	}
	if (w.gopher_back && reads(t, "gopher_back")) out.push_back(window(*w.gopher_back, "GOPHER_BACK", true));
	for (const Window &child : w.children) out.push_back(window(child, "WINDOW", false));
}

WrittenElement Words::screen(const Screen &s) {
	WrittenElement e = element("SCREEN", WrittenForm::Container);
	if (!s.name.empty()) e.children.push_back(leaf("NAME", s.name));
	if (s.has_music_var) e.children.push_back(leaf("MUSICVAR", std::to_string(s.music_var)));
	for (const Window &root : s.roots) e.children.push_back(window(root, "WINDOW", false));
	return e;
}

// The writer's words for each screen of a document.
std::vector<WrittenElement> written_screens(const Document &doc) {
	Words words;
	std::vector<WrittenElement> out;
	out.reserve(doc.screens.size());
	for (const Screen &s : doc.screens) out.push_back(words.screen(s));
	return out;
}

// The open tag of an element: `<TAG attributes...>`.
std::string written_open_tag(const WrittenElement &e) {
	std::string out = "<" + e.tag;
	for (const WrittenAttribute &a : e.attributes) out += " " + a.name + a.value_token();
	return out + ">";
}

void render_written_content(const WrittenElement &e, const WrittenStyle &style, const std::string &indent,
                            std::string &out);

// An element in the writer's layout; `indent` is what stands before its own first line, so each
// element in it goes on a new line at `indent` and one `unit` more.
void render_written(const WrittenElement &e, const WrittenStyle &style, const std::string &indent, std::string &out) {
	out += written_open_tag(e);
	render_written_content(e, style, indent, out);
	out += "</" + e.tag + ">";
}

// What stands between an element's open and close tags.
void render_written_content(const WrittenElement &e, const WrittenStyle &style, const std::string &indent,
                            std::string &out) {
	switch (e.form) {
	case WrittenForm::Leaf: out += escape_text(e.text); break;
	case WrittenForm::Compact: {
		out += escape_text(e.text);
		const WrittenStyle one_line;
		for (const WrittenElement &child : e.children) render_written(child, one_line, std::string(), out);
		break;
	}
	case WrittenForm::Container: {
		const std::string inner = indent + style.unit;
		for (const WrittenElement &child : e.children) {
			out += style.eol;
			out += inner;
			render_written(child, style, inner, out);
		}
		out += style.eol;
		out += indent;
		break;
	}
	}
}

// --- write issues ------------------------------------------------------------

bool has_quote(const std::string &value) {
	return value.find('"') != std::string::npos || value.find('\'') != std::string::npos;
}

bool known(const std::string &value, const char *const *tokens) { return known_token(value, tokens); }

// Whether the writer puts any child element in a window (retail creates no WINDOW
// without one; a part without one crashes it).
bool writes_elements(const Window &w) {
	Words words;
	std::vector<WrittenElement> out;
	words.window_elements(w, out);
	return !out.empty();
}

// Each issue names the record that holds the value by its lists' element paths and the
// field by its element path (WriteIssue::locator; the names the editor's menu table
// gives them), so the editor shows it on the field that causes it.
class IssueCheck {
public:
	explicit IssueCheck(std::vector<WriteIssue> &out) : out_(out) {}
	void screen(const Screen &s, size_t index);

private:
	void add(const std::string &window, const std::string &locator, const std::string &field,
	         const std::string &message) {
		out_.push_back({screen_, window, locator, field, message});
	}
	void value(const std::string &window, const std::string &locator, const std::string &field, const std::string &v) {
		if (has_quote(v))
			add(window, locator, field,
			    field + " holds a quote: retail's reader ends a value at a quote of either kind, so it cannot be saved.");
	}
	// The rows' values; `read` when retail's parse reads (and so stops at) this list.
	void rows(const std::string &window, const std::string &owner, const char *list, const char *tag,
	          const std::vector<Appearance> &rows, bool read);
	void window(const Window &w, const std::string &locator, const char *tag, bool part);
	// A FLAGS word the table's names cannot spell (a bit no row holds).
	void flags(const std::string &window, const std::string &locator, const std::string &field, uint32_t word) {
		if (word != kDefaultUiMaterialFlags && ui_material_flags_text(word).empty())
			add(window, locator, field,
			    field + " holds a bit no flag name of the game's table sets: no FLAGS text reads it, so it cannot be "
			            "saved.");
	}
	static std::string at(const std::string &owner, const char *list, size_t index) {
		return owner + "/" + list + ":" + std::to_string(index);
	}

	std::vector<WriteIssue> &out_;
	std::string screen_;
};

void IssueCheck::rows(const std::string &window, const std::string &owner, const char *list, const char *tag,
                      const std::vector<Appearance> &list_rows, bool read) {
	for (size_t i = 0; i < list_rows.size(); ++i) {
		const Appearance &a = list_rows[i];
		if (empty_row(a)) continue;
		const std::string locator = at(owner, list, i);
		value(window, locator, "type", a.type);
		value(window, locator, "state", a.state);
		flags(window, locator, "flags", a.flags);
		if (read && !known(a.state, kAppearanceStates))
			add(window, locator, "state",
			    std::string("An ") + tag + " with no STATE retail knows (DEFAULT, DISABLED, MOUSEOVER, SELECTED) stops "
			                               "this window's parse in retail: what follows it and its child windows are lost.");
	}
}

void IssueCheck::window(const Window &w, const std::string &locator, const char *tag, bool part) {
	const std::string &name = w.name;
	value(name, locator, "name", w.name);
	if (!writes_elements(w)) {
		if (part)
			add(name, locator, "", std::string("An empty ") + tag + " crashes retail (its widget parses no element).");
		else
			add(name, locator, "", "Retail creates no WINDOW without a child element: this window would be lost.");
	}
	const bool scroll = w.type == WindowType::Scroll;
	const bool list = w.type == WindowType::List || w.type == WindowType::LanList || w.type == WindowType::Table;
	rows(name, locator, "appearance", "APPEARANCE", w.appearances, true);
	rows(name, locator, "shuttle", "SHUTTLE", w.shuttle, scroll);
	rows(name, locator, "scrollup", "SCROLLUP", w.scrollup, scroll);
	rows(name, locator, "scrolldown", "SCROLLDOWN", w.scrolldown, scroll);
	if (w.items.present) rows(name, locator, "items.appearance", "ITEMS APPEARANCE", w.items.appearances, list);
	for (size_t i = 0; i < w.sounds.size(); ++i) {
		const Sound &s = w.sounds[i];
		const std::string row = at(locator, "sound", i);
		value(name, row, "state", s.state);
		value(name, row, "trigger", s.trigger);
		if (!known(s.state, kSoundStates) || s.trigger.empty())
			add(name, row, known(s.state, kSoundStates) ? "trigger" : "state",
			    "A SOUND needs a STATE retail knows (MOUSEIN, MOUSEOUT, SELECTED) and a TRIGGER; without them it stops "
			    "this window's parse in retail.");
	}
	for (size_t i = 0; i < w.actions.size(); ++i) {
		const Action &a = w.actions[i];
		const std::string row = at(locator, "action", i);
		value(name, row, "type", a.type);
		value(name, row, "state", a.state);
		value(name, row, "file", a.file);
		value(name, row, "field", a.field);
		value(name, row, "test", a.test);
		if (iequals(a.type, "SCREEN") && a.file.empty())
			add(name, row, "file", "A SCREEN action with no FILE crashes retail when it is activated.");
	}
	if (w.string_data.present) {
		value(name, locator, "string.justify", w.string_data.justify);
		value(name, locator, "string.vjustify", w.string_data.vjustify);
		value(name, locator, "string.type", w.string_data.type);
	}
	if (w.toggle_string.present) value(name, locator, "toggle_string.type", w.toggle_string.type);
	if (w.items.present) {
		value(name, locator, "items.justify", w.items.justify);
		value(name, locator, "items.vjustify", w.items.vjustify);
		auto item = [&](const std::string &row, const Item &i) {
			value(name, row, "type", i.type);
			value(name, row, "value", i.value);
			value(name, row, "justify", i.justify);
			value(name, row, "vjustify", i.vjustify);
		};
		for (size_t i = 0; i < w.items.items.size(); ++i) item(at(locator, "items.item", i), w.items.items[i]);
		for (size_t r = 0; r < w.items.rows.size(); ++r)
			for (size_t c = 0; c < w.items.rows[r].cells.size(); ++c)
				item(at(at(locator, "items.row", r), "item", c), w.items.rows[r].cells[c]);
	}
	const TableColumn &column = w.table_data.column;
	for (size_t i = 0; i < column.headers.size(); ++i) {
		const TableHeader &h = column.headers[i];
		const std::string row = at(locator, "column.header", i);
		value(name, row, "justify", h.justify);
		value(name, row, "vjustify", h.vjustify);
		value(name, row, "sort", h.sort);
		value(name, row, "type", h.type);
		value(name, row, "field", h.field);
	}
	for (size_t i = 0; i < column.bodies.size(); ++i) {
		const TableBody &b = column.bodies[i];
		const std::string row = at(locator, "column.body", i);
		value(name, row, "justify", b.justify);
		value(name, row, "vjustify", b.vjustify);
		flags(name, row, "bitmap_flags", b.bitmap_flags);
	}
	for (size_t i = 0; i < column.substitutions.size(); ++i)
		value(name, at(locator, "column.subst", i), "value", column.substitutions[i].value);
	const std::vector<SortKeyPlace> places = sort_key_places(column);
	const char *const keys[] = {"column.primary_sort", "column.secondary_sort", "column.tertiary_sort"};
	for (size_t k = 0; k < places.size(); ++k)
		if (places[k].slot && !places[k].placed)
			add(name, locator, keys[k],
			    "A table sort key names a column no HEADER's index reaches, so no HEADER can carry it: it cannot be "
			    "saved.");
	flags(name, locator, "cursor.flags", w.cursor.flags);
	// The GLB_TABLE's FIELDS: a FIELD with no count, or at the count, writes past the browser's slots.
	const GlbFields &glb = w.glb_server_fields;
	for (size_t i = 0; glb.present && i < glb.fields.size(); ++i) {
		const GlbFieldSlot &f = glb.fields[i];
		if (!glb.has_count || f.which < 0 || f.which >= glb.count)
			add(name, at(locator, "glb_server_fields.field", i), "which",
			    "A FIELD slot outside 0 to one less than the count writes past the browser's slots in retail.");
	}
	if (glb.has_format && (glb.format.find("\\r") != std::string::npos || glb.format.find("\\n") != std::string::npos))
		add(name, locator, "glb_server_fields.format",
		    "A FORMAT holding a backslash before r or n reads back as a line break: it cannot be saved.");
	for (size_t i = 0; i < w.gopher_categories.size(); ++i) {
		const GopherCategory &c = w.gopher_categories[i];
		const std::string row = at(locator, "gopher_category", i);
		value(name, row, "level", c.level);
		if (!c.label && c.entries.empty())
			add(name, row, "", "An empty CATEGORY crashes retail (its parse reads no element).");
		for (size_t e = 0; e < c.entries.size(); ++e) {
			const GopherEntry &entry = c.entries[e];
			const std::string at_entry = at(row, "entry", e);
			value(name, at_entry, "name", entry.name);
			value(name, at_entry, "target", entry.target);
			value(name, at_entry, "filter", entry.filter);
			value(name, at_entry, "table", entry.table);
		}
		if (c.label) window(*c.label, at(row, "label", 0), "LABEL", true);
	}
	const std::pair<const WindowPart *, const char *> typed_parts[] = {
	        {&w.join_button, "JOIN_BUTTON"},
	        {&w.search_button, "SEARCH_BUTTON"},
	        {&w.glb_table, "GLB_TABLE"},
	        {&w.glb_info_button, "GLB_INFO_BUTTON"},
	        {&w.glb_info_player_details, "GLB_INFO_PLAYER_DETAILS"},
	        {&w.glb_info_server_details, "GLB_INFO_SERVER_DETAILS"},
	        {&w.glb_nw_info, "GLB_NW_INFO"},
	        {&w.gopher_back, "GOPHER_BACK"},
	};
	for (const auto &[part, part_tag] : typed_parts)
		if (*part) window(**part, at(locator, strutil::to_lower(part_tag).c_str(), 0), part_tag, true);
	// [orig: CTableWnd_RecalcLayout @ 0x63f1a0, idiv @ 0x63f26b]
	if (w.type == WindowType::Table && w.table_data.has_min_item_height && w.table_data.min_item_height == 0)
		add(name, locator, "min_item_height", "MIN_ITEM_HEIGHT 0 divides by zero when retail creates the table.");
	// [orig: CEditWnd_ParseXMLProperties @ 0x661e5f; CCheckWnd_ParseXMLDefinition @ 0x64ae05]
	const bool edit = w.type == WindowType::Edit || w.type == WindowType::MultilineEdit ||
	                  w.type == WindowType::RadioEdit;
	if (w.global_var && (edit || (w.type == WindowType::CheckBox && w.checked)))
		add(name, locator, "global_var",
		    edit ? "GLOBAL_VAR on an edit window faults when retail parses it (the scene is not set yet)."
		         : "GLOBAL_VAR on a CHECKED checkbox faults when retail parses it (the scene is not set yet).");
	if (w.list_box) window(*w.list_box, at(locator, "list_box", 0), "LIST_BOX", true);
	if (w.spinup) window(*w.spinup, at(locator, "spinup", 0), "SPINUP", true);
	if (w.spindown) window(*w.spindown, at(locator, "spindown", 0), "SPINDOWN", true);
	if (w.scrollbar) window(*w.scrollbar, at(locator, "scrollbar", 0), "SCROLLBAR", true);
	for (size_t i = 0; i < w.children.size(); ++i) window(w.children[i], at(locator, "window", i), "WINDOW", false);
}

void IssueCheck::screen(const Screen &s, size_t index) {
	screen_ = s.name;
	const std::string locator = std::to_string(index);
	if (s.name.empty() && !s.roots.empty())
		add("", locator, "name", "A SCREEN with no NAME crashes retail as it creates the screen's first window.");
	for (size_t i = 0; i < s.roots.size(); ++i) window(s.roots[i], at(locator, "window", i), "WINDOW", false);
}

WrittenStyle canonical_style(bool pretty, int indent_size) {
	WrittenStyle style;
	if (pretty) {
		style.eol = "\n";
		style.unit.assign(static_cast<size_t>(std::max(0, indent_size)), ' ');
	}
	return style;
}

} // namespace

// --- the writer --------------------------------------------------------------

// Element text: retail's reader turns '<' into a tag and '&' into an entity; both
// are written as the two entities it decodes back [orig: XML_ParseCharEntity
// @ 0x769cc0, table @ 0x85a628]. Everything else is written as it is (no trim or
// collapse reads it back unchanged).
std::string escape_text(const std::string &text) {
	std::string out;
	out.reserve(text.size());
	for (char c : text) {
		if (c == '<') out += "&lt;";
		else if (c == '&') out += "&amp;";
		else out.push_back(c);
	}
	return out;
}

std::vector<WriteIssue> write_issues(const Document &doc) {
	std::vector<WriteIssue> issues;
	IssueCheck check(issues);
	for (size_t i = 0; i < doc.screens.size(); ++i) check.screen(doc.screens[i], i);
	return issues;
}

std::string serialize(const Document &doc, bool pretty, int indent_size) {
	const std::vector<WrittenElement> screens = written_screens(doc);
	const WrittenStyle style = canonical_style(pretty, indent_size);
	std::string out;
	for (const WrittenElement &screen : screens) {
		render_written(screen, style, std::string(), out);
		out += style.eol;
	}
	return out;
}

bool serialize_bytes(const Document &doc, std::vector<uint8_t> &out,
                     std::string &error, bool pretty, int indent_size) {
	out.clear();
	error.clear();
	const std::vector<WriteIssue> issues = write_issues(doc);
	if (!issues.empty()) {
		error = issues.front().message;
		return false;
	}
	const std::string text = serialize(doc, pretty, indent_size);
	if (doc.source_encoding != SourceEncoding::Utf16LE) {
		if (doc.source_encoding == SourceEncoding::Utf8Bom) out.insert(out.end(), {0xEF, 0xBB, 0xBF});
		out.insert(out.end(), text.begin(), text.end());
		return true;
	}
	// The model's UTF-8 as little-endian UTF-16 after the byte order mark.
	out.insert(out.end(), {0xFF, 0xFE});
	auto unit = [&](uint32_t u) {
		out.push_back(static_cast<uint8_t>(u & 0xFF));
		out.push_back(static_cast<uint8_t>(u >> 8));
	};
	for (size_t i = 0; i < text.size();) {
		const uint8_t lead = static_cast<uint8_t>(text[i]);
		uint32_t cp = lead;
		size_t count = 1;
		if (lead >= 0xF0) { cp = lead & 0x07u; count = 4; }
		else if (lead >= 0xE0) { cp = lead & 0x0Fu; count = 3; }
		else if (lead >= 0xC0) { cp = lead & 0x1Fu; count = 2; }
		if (i + count > text.size()) {
			error = "MNU text ends in an incomplete UTF-8 sequence";
			out.clear();
			return false;
		}
		for (size_t k = 1; k < count; ++k) cp = (cp << 6) | (static_cast<uint8_t>(text[i + k]) & 0x3Fu);
		i += count;
		if (cp > 0xFFFF) {
			cp -= 0x10000;
			unit(0xD800 | (cp >> 10));
			unit(0xDC00 | (cp & 0x3FF));
		} else {
			unit(cp);
		}
	}
	return true;
}

} // namespace opennova::mnu
