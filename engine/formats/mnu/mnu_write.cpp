// The writer: each record as the element it puts down (the writer's words, mnu_write.h), the
// writer's own layout, the write issues, and the serialization entry points, which hand a
// document read from a file to its text layout (mnu_text_layout.h). docs/mnu/menu-re.md ("The
// writer") has the rules with their witnesses.
#include <formats/mnu/mnu_write.h>

#include <formats/mnu/mnu_text_layout.h>

#include <algorithm>
#include <utility>

#include <base/io/strutil.h>

namespace opennova::mnu {

namespace {

using opennova::strutil::iequals;

std::string upper(std::string s) {
	for (char &c : s)
		if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
	return s;
}

bool in_list(const std::string &key, const char *const *keys) {
	for (size_t i = 0; keys[i]; ++i)
		if (key == keys[i]) return true;
	return false;
}

// The attribute keys each parse reads (written_vocabulary), as the reader's walks compare them
// (mnu.cpp's TreeReader): a key outside its element's set is a token the game reads as nothing.
const char *const kWindowKeys[] = {"TYPE", "NAME", "FORM", "HIDDEN", "DISABLE", "GLOBAL_VAR", "DRAW_FRAME",
                                   "MODAL", "CHECKED", "AS_BUTTON", "PASSWORD", "NUMBER", "READONLY", "MAXCHAR",
                                   "MAXVAL", "MINVAL", "PLAYERLIST", "SERVERLIST", nullptr};
// A part is its owner's own widget (its TYPE is not read); a LIST_BOX's sb_edge_pad is the combo's.
const char *const kPartKeys[] = {"NAME", "FORM", "HIDDEN", "DISABLE", "GLOBAL_VAR", "DRAW_FRAME", "MODAL",
                                 "CHECKED", "AS_BUTTON", "PASSWORD", "NUMBER", "READONLY", "MAXCHAR", "MAXVAL",
                                 "MINVAL", "PLAYERLIST", "SERVERLIST", "SB_EDGE_PAD", nullptr};
const char *const kAppearanceKeys[] = {"STATE", "TYPE", "MAP_STATE", "HEIGHT", "FLAGS", nullptr};
const char *const kSoundKeys[] = {"STATE", "TRIGGER", nullptr};
const char *const kActionKeys[] = {"TYPE", "FILE", "@FIELD", "STATE", "TARGET_FORM", "EXTERNAL_BROWSER", "TOGGLE",
                                   "TEST", nullptr};
const char *const kHotkeyKeys[] = {"VIRTUAL", nullptr};
const char *const kStringKeys[] = {"TYPE", "JUSTIFY", "VJUSTIFY", "EDGE", "WRAP", nullptr};
const char *const kTypeKeys[] = {"TYPE", nullptr};
const char *const kItemsKeys[] = {"JUSTIFY", "VJUSTIFY", "MULTISELECT", nullptr};
const char *const kItemKeys[] = {"TYPE", "VALUE", "JUSTIFY", "VJUSTIFY", "PAIRS_LIST", "COLUMN", nullptr};
const char *const kStencilKeys[] = {"SIZE", "INSETX", "INSETY", nullptr};
const char *const kColumnKeys[] = {"COUNT", "SPACING", nullptr};
const char *const kHeaderKeys[] = {"COLUMN", "JUSTIFY", "VJUSTIFY", "SORT", "WIDTH", "TYPE", "@SORT0", "@SORT1",
                                   "@SORT2", nullptr};
const char *const kBodyKeys[] = {"COLUMN", "JUSTIFY", "VJUSTIFY", "CUSTOM_DRAW", "BITMAP_DRAW", "BITMAP_TEXT",
                                 "SCALE_BITMAP", "BITMAP_FLAGS", nullptr};
const char *const kSubstKeys[] = {"COLUMN", "VALUE", "URL", "FILE", nullptr};
const char *const kNoKeys[] = {nullptr};

const char *const *vocabulary_of(WrittenKind kind) {
	switch (kind) {
	case WrittenKind::Window: return kWindowKeys;
	case WrittenKind::Part: return kPartKeys;
	case WrittenKind::Appearance: return kAppearanceKeys;
	case WrittenKind::Sound: return kSoundKeys;
	case WrittenKind::Action: return kActionKeys;
	case WrittenKind::Hotkey: return kHotkeyKeys;
	case WrittenKind::String: return kStringKeys;
	case WrittenKind::ToggleString: return kTypeKeys;
	case WrittenKind::Items: return kItemsKeys;
	case WrittenKind::Item: return kItemKeys;
	case WrittenKind::Stencil: return kStencilKeys;
	case WrittenKind::Column: return kColumnKeys;
	case WrittenKind::Header: return kHeaderKeys;
	case WrittenKind::Body: return kBodyKeys;
	case WrittenKind::Subst: return kSubstKeys;
	default: return kNoKeys;
	}
}

// --- digests -------------------------------------------------------------------------------------

// FNV-1a over the bytes of each part, each part ended by a byte no text holds alone.
struct Digest {
	uint64_t h = 1469598103934665603ull;
	void add(const std::string &s) {
		for (char c : s) {
			h ^= static_cast<uint8_t>(c);
			h *= 1099511628211ull;
		}
		h ^= 0xFF;
		h *= 1099511628211ull;
	}
	void add(uint64_t v) {
		for (int i = 0; i < 8; ++i) {
			h ^= static_cast<uint8_t>(v >> (8 * i));
			h *= 1099511628211ull;
		}
	}
};

void digest(WrittenElement &e) {
	Digest own;
	own.add(uint64_t(e.kind));
	own.add(uint64_t(e.form));
	own.add(e.tag);
	for (const WrittenAttribute &a : e.attributes) {
		own.add(a.key);
		own.add(a.name);
		own.add(a.value);
		own.add(uint64_t(a.bare));
	}
	own.add(e.text);
	e.own_digest = own.h;
	Digest tree;
	tree.add(e.own_digest);
	for (WrittenElement &child : e.children) {
		digest(child);
		tree.add(child.key);
		tree.add(child.tree_digest);
	}
	e.tree_digest = tree.h;
}

// --- the writer's words ----------------------------------------------------------------------------

void put(std::vector<WrittenAttribute> &a, const char *name, const std::string &value) {
	if (!value.empty()) a.push_back({upper(name), name, value, false});
}
void put_int(std::vector<WrittenAttribute> &a, const char *name, bool has, int value) {
	if (has) a.push_back({upper(name), name, std::to_string(value), false});
}
void put_bare(std::vector<WrittenAttribute> &a, const char *name, bool on) {
	if (on) a.push_back({upper(name), name, std::string(), true});
}

std::string list_key(uint64_t source) { return "#" + std::to_string(source); }

WrittenElement element(WrittenKind kind, const std::string &tag, WrittenForm form, std::string key,
                       std::string list = std::string()) {
	WrittenElement e;
	e.kind = kind;
	e.tag = tag;
	e.form = form;
	e.key = std::move(key);
	e.list = std::move(list);
	return e;
}

WrittenElement leaf(const char *tag, const std::string &text, const char *key = nullptr) {
	WrittenElement e = element(WrittenKind::Plain, tag, WrittenForm::Leaf, key ? key : tag);
	e.text = text;
	return e;
}

// The attributes a parse keeps in order under names that may repeat, each keyed by its name and
// its place among those of the name.
void put_kept(std::vector<WrittenAttribute> &out, WrittenKind kind, const std::vector<ElementAttribute> &kept) {
	std::vector<std::pair<std::string, size_t>> seen;
	for (const ElementAttribute &a : kept) {
		const std::string name = upper(a.name);
		size_t place = 0;
		auto it = std::find_if(seen.begin(), seen.end(), [&](const auto &s) { return s.first == name; });
		if (it == seen.end()) seen.emplace_back(name, 1);
		else place = it->second++;
		out.push_back({written_key(kind, a.name, place), a.name, a.value, !a.has_value});
	}
}

bool is_space_text(const std::string &text) {
	return std::all_of(text.begin(), text.end(), [](char c) {
		return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
	});
}

bool empty_row(const Appearance &a) {
	return a.state.empty() && a.type.empty() && a.value.empty() && !a.has_map_state && !a.has_height && a.flags.empty();
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
	void appearance(const Appearance &a, const char *tag, const char *list, std::vector<WrittenElement> &out);
	WrittenElement item(const Item &i);
	void items(const Items &items, std::vector<WrittenElement> &out);
	void column(const TableColumn &column, std::vector<WrittenElement> &out);
	WrittenElement extra(const Element &x, const char *list);
};

void Words::appearance(const Appearance &a, const char *tag, const char *list, std::vector<WrittenElement> &out) {
	if (empty_row(a)) return; // nothing to write (an element with no attributes stops the parse)
	WrittenElement e = element(WrittenKind::Appearance, tag, WrittenForm::Leaf, list_key(a.source), list);
	e.source = a.source;
	put(e.attributes, "type", a.type);
	put(e.attributes, "state", a.state);
	put_int(e.attributes, "map_state", a.has_map_state, a.map_state);
	put_int(e.attributes, "height", a.has_height, a.height);
	put(e.attributes, "flags", a.flags);
	e.text = a.value;
	out.push_back(std::move(e));
}

WrittenElement Words::item(const Item &i) {
	WrittenElement e = element(WrittenKind::Item, "ITEM", WrittenForm::Leaf, list_key(i.source), "ITEM");
	e.source = i.source;
	put(e.attributes, "type", i.type);
	put(e.attributes, "value", i.value);
	put(e.attributes, "justify", i.justify);
	put(e.attributes, "vjustify", i.vjustify);
	put_bare(e.attributes, "PAIRS_LIST", i.pairs_list);
	put_int(e.attributes, "column", i.has_column, i.column);
	e.text = i.text;
	return e;
}

void Words::items(const Items &items, std::vector<WrittenElement> &out) {
	if (!items.present) return;
	WrittenElement e = element(WrittenKind::Items, "ITEMS", WrittenForm::Container, "ITEMS");
	put(e.attributes, "justify", items.justify);
	put(e.attributes, "vjustify", items.vjustify);
	put_bare(e.attributes, "MULTISELECT", items.multiselect);
	for (const Appearance &a : items.appearances) appearance(a, "APPEARANCE", "APPEARANCE", e.children);
	for (const Item &i : items.items) e.children.push_back(item(i));
	for (const TableRow &row : items.rows) {
		WrittenElement r = element(WrittenKind::Row, "ROW", WrittenForm::Container, list_key(row.source), "ROW");
		r.source = row.source;
		for (const Item &cell : row.cells) r.children.push_back(item(cell));
		e.children.push_back(std::move(r));
	}
	out.push_back(std::move(e));
}

void Words::column(const TableColumn &c, std::vector<WrittenElement> &out) {
	const std::vector<SortKeyPlace> places = sort_key_places(c);
	const bool keys = places[0].slot || places[1].slot || places[2].slot;
	if (!c.has_count && !c.has_spacing && c.headers.empty() && c.bodies.empty() && c.substitutions.empty() && !keys)
		return;
	WrittenElement e = element(WrittenKind::Column, "COLUMN", WrittenForm::Container, "COLUMN");
	put_int(e.attributes, "count", c.has_count, c.count);
	put_int(e.attributes, "spacing", c.has_spacing, c.spacing);
	const char *const key_tokens[] = {c.primary_sort_token.empty() ? kPrimarySortTokens[0] : c.primary_sort_token.c_str(),
	                                  "SECONDARY_SORT", "TERTIARY_SORT"};
	for (size_t i = 0; i < c.headers.size(); ++i) {
		const TableHeader &h = c.headers[i];
		WrittenElement header = element(WrittenKind::Header, "HEADER", WrittenForm::Leaf, list_key(h.source), "HEADER");
		header.source = h.source;
		// A sort key written before COLUMN takes this HEADER's index, one written after it
		// the index before (the walk runs last authored first).
		std::vector<WrittenAttribute> before, after;
		for (int k = 0; k < 3; ++k)
			if (places[k].slot && places[k].placed && places[k].header == i)
				(places[k].own ? before : after).push_back({"@SORT" + std::to_string(k), key_tokens[k], std::string(), true});
		put(header.attributes, "justify", h.justify);
		put(header.attributes, "vjustify", h.vjustify);
		header.attributes.insert(header.attributes.end(), before.begin(), before.end());
		put_int(header.attributes, "column", h.has_column, h.column);
		header.attributes.insert(header.attributes.end(), after.begin(), after.end());
		put(header.attributes, "sort", h.sort);
		put_int(header.attributes, "width", h.has_width, h.width);
		put(header.attributes, "type", h.type);
		header.text = h.text;
		e.children.push_back(std::move(header));
	}
	for (const TableBody &b : c.bodies) {
		WrittenElement body = element(WrittenKind::Body, "BODY", WrittenForm::Leaf, list_key(b.source), "BODY");
		body.source = b.source;
		put(body.attributes, "justify", b.justify);
		put(body.attributes, "vjustify", b.vjustify);
		put_int(body.attributes, "column", b.has_column, b.column);
		// The draw kind retail keeps is the first authored of the three: written first.
		const bool on[] = {b.custom_draw, b.bitmap_draw, b.bitmap_text};
		for (int k = 0; k < 3; ++k)
			if (on[k] && iequals(b.display, kBodyDisplays[k])) put_bare(body.attributes, kBodyDisplays[k], true);
		for (int k = 0; k < 3; ++k)
			if (on[k] && !iequals(b.display, kBodyDisplays[k])) put_bare(body.attributes, kBodyDisplays[k], true);
		put(body.attributes, "BITMAP_FLAGS", b.bitmap_flags);
		put_bare(body.attributes, "SCALE_BITMAP", b.scale_bitmap);
		e.children.push_back(std::move(body));
	}
	for (const TableSubst &s : c.substitutions) {
		WrittenElement subst = element(WrittenKind::Subst, "SUBST", WrittenForm::Leaf, list_key(s.source), "SUBST");
		subst.source = s.source;
		put_int(subst.attributes, "column", s.has_column, s.column);
		put(subst.attributes, "value", s.value);
		put_bare(subst.attributes, "URL", s.is_url);
		put_bare(subst.attributes, "FILE", s.is_file);
		subst.text = s.file;
		e.children.push_back(std::move(subst));
	}
	out.push_back(std::move(e));
}

WrittenElement Words::extra(const Element &x, const char *list) {
	// Text beside child elements is written compact, so no layout whitespace joins it.
	const WrittenForm form = x.children.empty()        ? WrittenForm::Leaf
	                         : !is_space_text(x.text) ? WrittenForm::Compact
	                                                  : WrittenForm::Container;
	WrittenElement e = element(WrittenKind::Element, x.tag, form, list_key(x.source), list);
	e.source = x.source;
	put_kept(e.attributes, WrittenKind::Element, x.attributes);
	e.text = x.text;
	for (const Element &child : x.children) e.children.push_back(extra(child, "ELEMENT"));
	return e;
}

WrittenElement Words::window(const Window &w, const char *tag, bool part, std::vector<WrittenAttribute> extra_attributes) {
	const WrittenKind kind = part ? WrittenKind::Part : WrittenKind::Window;
	WrittenElement e = element(kind, tag, WrittenForm::Container, part ? std::string(tag) : list_key(w.source),
	                           part ? std::string() : std::string("WINDOW"));
	e.source = w.source;
	e.attributes = std::move(extra_attributes);
	if (!part) put(e.attributes, "type", w.type_token.empty() ? window_type_name(w.type) : w.type_token);
	put(e.attributes, "name", w.name);
	put_bare(e.attributes, "DRAW_FRAME", w.draw_frame);
	put_bare(e.attributes, "HIDDEN", w.hidden);
	put_bare(e.attributes, "MODAL", w.modal);
	put_bare(e.attributes, "READONLY", w.readonly);
	put_bare(e.attributes, "DISABLE", w.disabled);
	put_bare(e.attributes, "CHECKED", w.checked);
	put_bare(e.attributes, "AS_BUTTON", w.as_button);
	put_bare(e.attributes, "NUMBER", w.number);
	put_int(e.attributes, "MINVAL", w.has_minval, w.minval);
	put_int(e.attributes, "MAXVAL", w.has_maxval, w.maxval);
	put_int(e.attributes, "MAXCHAR", w.has_maxchar, w.maxchar);
	put_bare(e.attributes, "GLOBAL_VAR", w.global_var);
	put_bare(e.attributes, "PASSWORD", w.password);
	put_int(e.attributes, "FORM", w.has_form, w.form);
	put_kept(e.attributes, kind, w.extra_attributes);
	window_elements(w, e.children);
	return e;
}

void Words::window_elements(const Window &w, std::vector<WrittenElement> &out) {
	if (w.has_group) out.push_back(leaf("GROUP", std::to_string(w.group)));
	for (const Hotkey &h : w.hotkeys) {
		WrittenElement e = element(WrittenKind::Hotkey, "HOTKEY", WrittenForm::Leaf, list_key(h.source), "HOTKEY");
		e.source = h.source;
		put_bare(e.attributes, "VIRTUAL", h.virtual_key);
		e.text = h.value;
		out.push_back(std::move(e));
	}
	for (const Action &a : w.actions) {
		WrittenElement e = element(WrittenKind::Action, "ACTION", WrittenForm::Leaf, list_key(a.source), "ACTION");
		e.source = a.source;
		put(e.attributes, "type", a.type);
		put(e.attributes, "state", a.state);
		put(e.attributes, "file", a.file);
		if (!a.field.empty())
			e.attributes.push_back({"@FIELD", a.field_attr.empty() ? std::string(kActionFieldAttributes[0]) : a.field_attr,
			                        a.field, false});
		put_int(e.attributes, "target_form", a.has_target_form, a.target_form);
		put_bare(e.attributes, "TOGGLE", a.toggle);
		put(e.attributes, "test", a.test);
		put_bare(e.attributes, "EXTERNAL_BROWSER", a.external_browser);
		e.text = a.target;
		out.push_back(std::move(e));
	}
	const Frame &f = w.frame;
	const bool has_stencil = !f.stencil.empty() || f.has_stencil_size || f.has_insetx || f.has_insety;
	if (has_stencil || !f.brush.empty() || !f.monogram.empty()) {
		WrittenElement frame = element(WrittenKind::Plain, "FRAME", WrittenForm::Container, "FRAME");
		if (has_stencil) {
			WrittenElement stencil = element(WrittenKind::Stencil, "STENCIL", WrittenForm::Leaf, "STENCIL");
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
		WrittenElement position = element(WrittenKind::Position, "POSITION", WrittenForm::Container, "POSITION");
		const std::pair<const char *, std::pair<bool, int>> edges[] = {{"LEFT", {pos.has_left, pos.left}},
		                                                                {"TOP", {pos.has_top, pos.top}},
		                                                                {"RIGHT", {pos.has_right, pos.right}},
		                                                                {"BOTTOM", {pos.has_bottom, pos.bottom}}};
		for (const auto &edge : edges) {
			if (!edge.second.first) continue;
			WrittenElement e = leaf(edge.first, std::to_string(edge.second.second));
			e.kind = WrittenKind::Edge;
			position.children.push_back(std::move(e));
		}
		out.push_back(std::move(position));
	}
	if (w.has_scroll_extent)
		out.push_back(leaf(w.scroll_extent_is_width ? "WIDTH" : "HEIGHT", std::to_string(w.scroll_extent), "SCROLL_EXTENT"));
	if (!w.orientation.empty()) out.push_back(leaf("ORIENTATION", w.orientation));
	for (const Appearance &a : w.appearances) appearance(a, "APPEARANCE", "APPEARANCE", out);
	for (const Appearance &a : w.shuttle) appearance(a, "SHUTTLE", "SHUTTLE", out);
	for (const Appearance &a : w.scrollup) appearance(a, "SCROLLUP", "SCROLLUP", out);
	for (const Appearance &a : w.scrolldown) appearance(a, "SCROLLDOWN", "SCROLLDOWN", out);
	if (w.has_text_rsrc) out.push_back(leaf("TEXT_RSRC", w.text_rsrc));
	for (size_t i = 0; i < w.datasources.size(); ++i) {
		WrittenElement e = leaf("DATASOURCE", w.datasources[i]);
		e.key = "DATASOURCE#" + std::to_string(i);
		e.list = "DATASOURCE";
		out.push_back(std::move(e));
	}
	if (!w.private_data.empty()) out.push_back(leaf("PRIVATE_DATA", w.private_data));
	if (!w.cursor.file.empty() || !w.cursor.flags.empty()) {
		WrittenElement cursor = element(WrittenKind::Plain, "CURSOR", WrittenForm::Container, "CURSOR");
		if (!w.cursor.file.empty()) cursor.children.push_back(leaf("FILE", w.cursor.file));
		if (!w.cursor.flags.empty()) cursor.children.push_back(leaf("FLAGS", w.cursor.flags));
		out.push_back(std::move(cursor));
	}
	const Font &font = w.font;
	if (!font.empty()) {
		WrittenElement e = element(WrittenKind::Plain, "FONT", WrittenForm::Container, "FONT");
		const std::pair<const char *, const std::string *> slots[] = {
		    {"NAME", &font.name},
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
			if (!slot.second->empty()) e.children.push_back(leaf(slot.first, *slot.second));
		out.push_back(std::move(e));
	}
	const String &s = w.string_data;
	if (s.present) {
		WrittenElement e = element(WrittenKind::String, "STRING", WrittenForm::Leaf, "STRING");
		put(e.attributes, "type", s.type);
		put(e.attributes, "justify", s.justify);
		put(e.attributes, "vjustify", s.vjustify);
		put_int(e.attributes, "edge", s.has_edge, s.edge);
		put_bare(e.attributes, "WRAP", s.wrap);
		e.text = s.value;
		out.push_back(std::move(e));
	}
	if (w.toggle_string.present) {
		WrittenElement e = element(WrittenKind::ToggleString, "TOGGLE_STRING", WrittenForm::Leaf, "TOGGLE_STRING");
		put(e.attributes, "type", w.toggle_string.type);
		e.text = w.toggle_string.value;
		out.push_back(std::move(e));
	}
	for (const Sound &snd : w.sounds) {
		WrittenElement e = element(WrittenKind::Sound, "SOUND", WrittenForm::Leaf, list_key(snd.source), "SOUND");
		e.source = snd.source;
		put(e.attributes, "state", snd.state);
		put(e.attributes, "trigger", snd.trigger);
		e.text = snd.file;
		out.push_back(std::move(e));
	}
	items(w.items, out);
	// The combo's sb_edge_pad rides on the LIST_BOX element with the list's own attributes.
	if (w.list_box) {
		std::vector<WrittenAttribute> pad;
		put_int(pad, "sb_edge_pad", w.has_sb_edge_pad, w.sb_edge_pad);
		out.push_back(window(*w.list_box, "LIST_BOX", true, std::move(pad)));
	}
	if (w.spinup) out.push_back(window(*w.spinup, "SPINUP", true));
	if (w.spindown) out.push_back(window(*w.spindown, "SPINDOWN", true));
	column(w.table_data.column, out);
	if (w.table_data.has_min_item_height)
		out.push_back(leaf("MIN_ITEM_HEIGHT", std::to_string(w.table_data.min_item_height)));
	if (w.table_data.has_fixed_header_height)
		out.push_back(leaf("FIXED_HEADER_HEIGHT", std::to_string(w.table_data.fixed_header_height)));
	if (w.scrollbar) out.push_back(window(*w.scrollbar, "SCROLLBAR", true));
	for (const Element &x : w.extras) out.push_back(extra(x, "EXTRA"));
	for (const Window &child : w.children) out.push_back(window(child, "WINDOW", false));
}

WrittenElement Words::screen(const Screen &s) {
	WrittenElement e = element(WrittenKind::Screen, "SCREEN", WrittenForm::Container, list_key(s.source), "SCREEN");
	e.source = s.source;
	if (!s.name.empty()) e.children.push_back(leaf("NAME", s.name));
	if (s.has_music_var) e.children.push_back(leaf("MUSICVAR", std::to_string(s.music_var)));
	for (const Window &root : s.roots) e.children.push_back(window(root, "WINDOW", false));
	return e;
}

// --- write issues ------------------------------------------------------------

bool has_quote(const std::string &value) {
	return value.find('"') != std::string::npos || value.find('\'') != std::string::npos;
}

bool known(const std::string &value, const char *const *tokens) { return known_token(value, tokens); }

// Whether the writer puts any child element in a window (retail creates no WINDOW
// without one; a part without one crashes it).
bool writes_elements(const Window &w) { return !written_window_elements(w).empty(); }

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
	void element(const std::string &window, const std::string &locator, const Element &e);
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
		value(window, locator, "flags", a.flags);
		if (read && !known(a.state, kAppearanceStates))
			add(window, locator, "state",
			    std::string("An ") + tag + " with no STATE retail knows (DEFAULT, DISABLED, MOUSEOVER, SELECTED) stops "
			                               "this window's parse in retail: what follows it and its child windows are lost.");
	}
}

void IssueCheck::element(const std::string &window, const std::string &locator, const Element &e) {
	for (size_t i = 0; i < e.attributes.size(); ++i) value(window, at(locator, "attribute", i), "value", e.attributes[i].value);
	for (size_t i = 0; i < e.children.size(); ++i) element(window, at(locator, "element", i), e.children[i]);
}

void IssueCheck::window(const Window &w, const std::string &locator, const char *tag, bool part) {
	const std::string &name = w.name;
	if (!part) value(name, locator, "type", w.type_token);
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
	}
	for (size_t i = 0; i < column.bodies.size(); ++i) {
		const TableBody &b = column.bodies[i];
		const std::string row = at(locator, "column.body", i);
		value(name, row, "justify", b.justify);
		value(name, row, "vjustify", b.vjustify);
		value(name, row, "bitmap_flags", b.bitmap_flags);
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
	for (size_t i = 0; i < w.extra_attributes.size(); ++i)
		value(name, at(locator, "attribute", i), "value", w.extra_attributes[i].value);
	for (size_t i = 0; i < w.extras.size(); ++i) element(name, at(locator, "element", i), w.extras[i]);
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

// --- the writer's words, public to the library -----------------------------------------------------

bool written_vocabulary(WrittenKind kind, const std::string &key) {
	if (kind == WrittenKind::Element) return true;
	return in_list(key.substr(0, key.find('#')), vocabulary_of(kind));
}

std::string written_key(WrittenKind kind, const std::string &name, size_t place) {
	const std::string up = upper(name);
	if (kind == WrittenKind::Action && in_list(up, kActionFieldAttributes)) return "@FIELD";
	if (kind == WrittenKind::Header) {
		if (in_list(up, kPrimarySortTokens)) return "@SORT0";
		if (up == "SECONDARY_SORT") return "@SORT1";
		if (up == "TERTIARY_SORT") return "@SORT2";
	}
	if (kind == WrittenKind::Element || ((kind == WrittenKind::Window || kind == WrittenKind::Part) &&
	                                     in_list(up, kExtraAttributes)))
		return up + "#" + std::to_string(place);
	return up;
}

std::vector<WrittenElement> written_screens(const Document &doc) {
	Words words;
	std::vector<WrittenElement> out;
	out.reserve(doc.screens.size());
	for (const Screen &s : doc.screens) {
		out.push_back(words.screen(s));
		digest(out.back());
	}
	return out;
}

std::vector<WrittenElement> written_window_elements(const Window &w) {
	Words words;
	std::vector<WrittenElement> out;
	words.window_elements(w, out);
	return out;
}

std::string written_open_tag(const WrittenElement &e) {
	std::string out = "<" + e.tag;
	for (const WrittenAttribute &a : e.attributes) out += " " + a.name + a.value_token();
	return out + ">";
}

void render_written(const WrittenElement &e, const WrittenStyle &style, const std::string &indent, std::string &out) {
	out += written_open_tag(e);
	render_written_content(e, style, indent, out);
	out += "</" + e.tag + ">";
}

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
	if (text_layout_applies(doc)) return text_layout_generate(*doc.text_layout, screens, style);
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
	// The byte order mark the file had (the loader reads past it), else the writer's own.
	const bool laid_out = text_layout_applies(doc);
	if (doc.source_encoding != SourceEncoding::Utf16LE) {
		if (doc.source_encoding == SourceEncoding::Utf8Bom) out.insert(out.end(), {0xEF, 0xBB, 0xBF});
		out.insert(out.end(), text.begin(), text.end());
		return true;
	}
	// The model's UTF-8 as little-endian UTF-16 after the byte order mark.
	if (laid_out && doc.text_layout->bom.size() == 2) out.insert(out.end(), doc.text_layout->bom.begin(), doc.text_layout->bom.end());
	else out.insert(out.end(), {0xFF, 0xFE});
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
