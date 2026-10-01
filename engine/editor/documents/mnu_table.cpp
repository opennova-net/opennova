// The menu's table (mnu_table.h): each record kind's fields with the slot the reader fills, the
// lists a record holds with the vector (or part) behind each, the defaults a new record takes, and
// what the editor shows of each, as rows of the one table shape.
#include "mnu_table.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cstring>
#include <type_traits>
#include <utility>

#include <base/io/strutil.h>
#include <formats/mnu/mnu_schema.h>

namespace opennova::editor {

namespace {

using strutil::iequals;

// --- the slots ---------------------------------------------------------------------------------------

enum class Type { Integer, Flag, Text };

// How the writer decides a field is written.
enum class Presence {
	Always,   // always written (a flag: written when yes; element text: with its element)
	Bit,      // a has_* bit: optional; Clear unsets the bit and keeps the value
	NonEmpty, // written when not empty
	Block,    // the field is a block's own present bit (STRING, TOGGLE_STRING, ITEMS, a part): 0 leaves
	          // the block out and keeps its content (mnu.h's presence contract)
};

// What a field names outside its record. Sound: a .lwf bank by file name; Credits: a marquee's
// credits file by file name; Screen: a screen by NAME in the menu file the ACTION's FILE names; Window:
// a window by NAME on the screen the ACTION's window is on. Dynamic: a sibling field decides
// (reference_on resolves it per record).
enum class Ref {
	None, Font, MenuTexture, StyleVar, TextTable, TextId, Menu, Sound, Credits, Screen, Window, Dynamic,
};

struct Choice {
	const char *name = "";
	int64_t value = 0;
};

// Where a field's value lives in its record. A plain field names one slot (a text, a number or a flag)
// and, for a Bit or a Block field, the presence bit; a field with its own rules (a window's TYPE, a
// part's toggle, a BODY's draw kind, a table sort key) reads and writes through its own functions.
struct Entry {
	const char *path = "";  // the element path: "name", "position.left", "string.value"
	Type type = Type::Text;
	size_t width = 0;       // a text's capacity in bytes, the terminator included (0 for numbers)
	Presence presence = Presence::Always;
	Ref reference = Ref::None;
	std::vector<Choice> choices; // the tokens the parse compares (a text field takes any text)
	const char *block = "";  // the enclosing block's field ("string", "items", ...; "" = none)
	// The choices are the tokens the parse knows, the text any list of them or other words (the
	// material FLAGS: ui_material_flag_choices).
	bool open = false;
	std::string *(*text)(void *) = nullptr;
	int *(*number)(void *) = nullptr;
	bool *(*flag)(void *) = nullptr;
	bool *(*bit)(void *) = nullptr;
	bool (*custom_get)(void *, Value &) = nullptr;
	bool (*custom_set)(void *, const Value &, std::string &) = nullptr;
	bool (*custom_present)(void *) = nullptr;
};

mnu::Window &W(void *r) { return *static_cast<mnu::Window *>(r); }

const std::vector<Choice> &justify_choices() {
	static const std::vector<Choice> c = {{"", 0}, {"LEFT", 1}, {"CENTER", 2}, {"RIGHT", 3}};
	return c;
}
const std::vector<Choice> &vjustify_choices() {
	static const std::vector<Choice> c = {{"", 0}, {"TOP", 1}, {"CENTER", 2}, {"BOTTOM", 3}};
	return c;
}
const std::vector<Choice> &id_choices() {
	static const std::vector<Choice> c = {{"", 0}, {"ID", 1}};
	return c;
}

Entry text(const char *path, size_t width, std::string *(*slot)(void *), Presence presence = Presence::NonEmpty,
           Ref reference = Ref::None, std::vector<Choice> choices = {}, const char *block = "") {
	Entry e;
	e.path = path;
	e.type = Type::Text;
	e.width = width;
	e.presence = presence;
	e.reference = reference;
	e.choices = std::move(choices);
	e.block = block;
	e.text = slot;
	return e;
}

// A FLAGS text: no text (the element not written: the writer leaves an empty FLAGS out), then the
// material flag tokens, any text taken (ui_material_flag_choices).
Entry material_flags(Entry e) {
	e.choices = {{"", 0}};
	for (const mnu::SchemaChoice &token : mnu::ui_material_flag_choices()) e.choices.push_back({token.name, token.value});
	e.open = true;
	return e;
}

Entry number(const char *path, int *(*slot)(void *), bool *(*bit)(void *), const char *block = "") {
	Entry e;
	e.path = path;
	e.type = Type::Integer;
	e.presence = bit ? Presence::Bit : Presence::Always;
	e.block = block;
	e.number = slot;
	e.bit = bit;
	return e;
}

Entry flag(const char *path, bool *(*slot)(void *), const char *block = "") {
	Entry e;
	e.path = path;
	e.type = Type::Flag;
	e.block = block;
	e.flag = slot;
	return e;
}

Entry toggle(const char *path, bool *(*bit)(void *)) {
	Entry e;
	e.path = path;
	e.type = Type::Flag;
	e.presence = Presence::Block;
	e.bit = bit;
	return e;
}

// A window's TYPE: the factory's match of the token, the token written as typed (a token it does not
// match builds a generic window [orig: CUIScene_CreateWidgetByType @ 0x64f630]).
bool type_get(void *r, Value &out) {
	const mnu::Window &w = W(r);
	out = w.type_token.empty() ? std::string(mnu::window_type_name(w.type)) : w.type_token;
	return true;
}
bool type_set(void *r, const Value &value, std::string &) {
	mnu::Window &w = W(r);
	const std::string &token = std::get<std::string>(value);
	w.type = mnu::parse_window_type(token);
	w.type_token = token;
	return true;
}

// A part's default: written, with a typeless DEFAULT appearance (a part with no element would crash
// retail).
mnu::Window &default_part(mnu::WindowPart &part, mnu::WindowType type) {
	mnu::Window &w = part.author(type);
	if (w.appearances.empty()) {
		mnu::Appearance appearance;
		appearance.state = "default";
		w.appearances.push_back(appearance);
	}
	return w;
}

// A part's toggle: 0 leaves the part out and keeps its window, 1 writes it again. A part that was
// never authored is added to its list, not toggled on: the toggle changes no list's length.
template <mnu::WindowPart mnu::Window::*Member> bool part_get(void *r, Value &out) {
	out = int64_t((W(r).*Member).present() ? 1 : 0);
	return true;
}
template <mnu::WindowPart mnu::Window::*Member, mnu::WindowType Type>
bool part_set(void *r, const Value &value, std::string &error) {
	mnu::WindowPart &part = W(r).*Member;
	if (std::get<int64_t>(value) == 0) {
		part.hide();
		return true;
	}
	if (!part.latent()) {
		error = "This window has no such part yet: add one first.";
		return false;
	}
	part.author(Type);
	return true;
}
template <mnu::WindowPart mnu::Window::*Member> bool part_present(void *r) { return (W(r).*Member).present(); }
template <mnu::WindowPart mnu::Window::*Member, mnu::WindowType Type> Entry part_toggle(const char *path) {
	Entry e;
	e.path = path;
	e.type = Type::Flag;
	e.presence = Presence::Block;
	e.custom_get = &part_get<Member>;
	e.custom_set = &part_set<Member, Type>;
	e.custom_present = &part_present<Member>;
	return e;
}

// A token the writer puts down as an attribute's NAME (an ACTION's FIELD / SOURCE / NAME slot, the
// table's primary sort key): only one the reader matches, spelled as it keeps it; any other would
// write an attribute retail reads as something else, or faults on.
bool name_token(const std::vector<Choice> &choices, const Value &value, std::string &slot, std::string &error,
                const char *what) {
	const std::string &text = std::get<std::string>(value);
	for (const Choice &choice : choices)
		if (iequals(text, choice.name)) {
			slot = choice.name;
			return true;
		}
	error = what;
	return false;
}

const std::vector<Choice> &sort_token_choices() {
	static const std::vector<Choice> c = {{"", 0}, {"PRIMARY_SORT", 1}, {"DEFAULT_SORT", 2}};
	return c;
}
bool sort_token_set(void *r, const Value &value, std::string &error) {
	return name_token(sort_token_choices(), value, W(r).table_data.column.primary_sort_token, error,
	                  "The primary sort key is written as PRIMARY_SORT or DEFAULT_SORT (none writes PRIMARY_SORT).");
}

// A table sort key: the column index the HEADER walk binds it to, -1 when no HEADER sets it.
template <int mnu::TableColumn::*Member> bool sort_present(void *r) { return W(r).table_data.column.*Member >= 0; }
template <int mnu::TableColumn::*Member> Entry sort_key(const char *path) {
	Entry e;
	e.path = path;
	e.type = Type::Integer;
	e.number = [](void *r) -> int * { return &(W(r).table_data.column.*Member); };
	e.custom_present = &sort_present<Member>;
	return e;
}

std::vector<Entry> window_entries() {
	std::vector<Entry> out;
	out.push_back(text("name", 128, [](void *r) { return &W(r).name; }));
	{
		Entry type;
		type.path = "type";
		type.type = Type::Text;
		type.width = 32;
		for (int i = 0; i < mnu::kWindowTypeCount; ++i)
			type.choices.push_back({mnu::window_type_name(mnu::WindowType(i)), i});
		type.custom_get = &type_get;
		type.custom_set = &type_set;
		out.push_back(type);
	}
	out.push_back(flag("hidden", [](void *r) { return &W(r).hidden; }));
	out.push_back(flag("disable", [](void *r) { return &W(r).disabled; }));
	out.push_back(flag("checked", [](void *r) { return &W(r).checked; }));
	out.push_back(flag("draw_frame", [](void *r) { return &W(r).draw_frame; }));
	out.push_back(flag("modal", [](void *r) { return &W(r).modal; }));
	out.push_back(flag("readonly", [](void *r) { return &W(r).readonly; }));
	out.push_back(flag("as_button", [](void *r) { return &W(r).as_button; }));
	out.push_back(flag("number", [](void *r) { return &W(r).number; }));
	out.push_back(flag("global_var", [](void *r) { return &W(r).global_var; }));
	out.push_back(flag("password", [](void *r) { return &W(r).password; }));
	out.push_back(number("group", [](void *r) { return &W(r).group; }, [](void *r) { return &W(r).has_group; }));
	out.push_back(number("form", [](void *r) { return &W(r).form; }, [](void *r) { return &W(r).has_form; }));
	out.push_back(number("minval", [](void *r) { return &W(r).minval; }, [](void *r) { return &W(r).has_minval; }));
	out.push_back(number("maxval", [](void *r) { return &W(r).maxval; }, [](void *r) { return &W(r).has_maxval; }));
	out.push_back(number("maxchar", [](void *r) { return &W(r).maxchar; }, [](void *r) { return &W(r).has_maxchar; }));
	out.push_back(number("position.left", [](void *r) { return &W(r).position.left; },
	                     [](void *r) { return &W(r).position.has_left; }));
	out.push_back(number("position.top", [](void *r) { return &W(r).position.top; },
	                     [](void *r) { return &W(r).position.has_top; }));
	out.push_back(number("position.right", [](void *r) { return &W(r).position.right; },
	                     [](void *r) { return &W(r).position.has_right; }));
	out.push_back(number("position.bottom", [](void *r) { return &W(r).position.bottom; },
	                     [](void *r) { return &W(r).position.has_bottom; }));
	out.push_back(text("frame.stencil", 128, [](void *r) { return &W(r).frame.stencil; }, Presence::NonEmpty,
	                   Ref::MenuTexture));
	out.push_back(number("frame.stencil_size", [](void *r) { return &W(r).frame.stencil_size; },
	                     [](void *r) { return &W(r).frame.has_stencil_size; }));
	out.push_back(number("frame.insetx", [](void *r) { return &W(r).frame.insetx; },
	                     [](void *r) { return &W(r).frame.has_insetx; }));
	out.push_back(number("frame.insety", [](void *r) { return &W(r).frame.insety; },
	                     [](void *r) { return &W(r).frame.has_insety; }));
	out.push_back(text("frame.brush", 128, [](void *r) { return &W(r).frame.brush; }, Presence::NonEmpty,
	                   Ref::MenuTexture));
	out.push_back(text("frame.monogram", 128, [](void *r) { return &W(r).frame.monogram; }, Presence::NonEmpty,
	                   Ref::MenuTexture));
	// SCROLL: the one along-axis extent a window-level HEIGHT or WIDTH sets, with its spelling.
	out.push_back(number("scroll_extent", [](void *r) { return &W(r).scroll_extent; },
	                     [](void *r) { return &W(r).has_scroll_extent; }));
	out.push_back(flag("scroll_extent_width", [](void *r) { return &W(r).scroll_extent_is_width; }));
	out.push_back(text("orientation", 32, [](void *r) { return &W(r).orientation; }, Presence::NonEmpty, Ref::None,
	                   {{"", 0}, {"HORIZONTAL", 1}, {"VERTICAL", 2}}));
	{
		Entry rsrc = text("text_rsrc", 64, [](void *r) { return &W(r).text_rsrc; }, Presence::Bit, Ref::TextTable);
		rsrc.bit = [](void *r) { return &W(r).has_text_rsrc; };
		out.push_back(rsrc);
	}
	out.push_back(text("private_data", 1024, [](void *r) { return &W(r).private_data; }));
	out.push_back(text("cursor.file", 64, [](void *r) { return &W(r).cursor.file; }, Presence::NonEmpty,
	                   Ref::MenuTexture));
	out.push_back(material_flags(text("cursor.flags", 64, [](void *r) { return &W(r).cursor.flags; })));
	out.push_back(text("font.name", 64, [](void *r) { return &W(r).font.name; }, Presence::NonEmpty, Ref::Font));
	const Ref color = Ref::StyleVar;
	out.push_back(text("font.default_fg", 32, [](void *r) { return &W(r).font.default_fg; }, Presence::NonEmpty, color));
	out.push_back(text("font.default_bg", 32, [](void *r) { return &W(r).font.default_bg; }, Presence::NonEmpty, color));
	out.push_back(text("font.mouseover_fg", 32, [](void *r) { return &W(r).font.mouseover_fg; }, Presence::NonEmpty, color));
	out.push_back(text("font.mouseover_bg", 32, [](void *r) { return &W(r).font.mouseover_bg; }, Presence::NonEmpty, color));
	out.push_back(text("font.selected_fg", 32, [](void *r) { return &W(r).font.selected_fg; }, Presence::NonEmpty, color));
	out.push_back(text("font.selected_bg", 32, [](void *r) { return &W(r).font.selected_bg; }, Presence::NonEmpty, color));
	out.push_back(text("font.disabled_fg", 32, [](void *r) { return &W(r).font.disabled_fg; }, Presence::NonEmpty, color));
	out.push_back(text("font.disabled_bg", 32, [](void *r) { return &W(r).font.disabled_bg; }, Presence::NonEmpty, color));
	// STRING [orig: CUIButtonWidget_ParseXMLAttributes @ 0x657c30].
	out.push_back(toggle("string", [](void *r) { return &W(r).string_data.present; }));
	out.push_back(text("string.type", 16, [](void *r) { return &W(r).string_data.type; }, Presence::NonEmpty, Ref::None,
	                   id_choices(), "string"));
	out.push_back(text("string.justify", 64, [](void *r) { return &W(r).string_data.justify; }, Presence::NonEmpty,
	                   Ref::None, justify_choices(), "string"));
	out.push_back(text("string.vjustify", 64, [](void *r) { return &W(r).string_data.vjustify; }, Presence::NonEmpty,
	                   Ref::None, vjustify_choices(), "string"));
	out.push_back(number("string.edge", [](void *r) { return &W(r).string_data.edge; },
	                     [](void *r) { return &W(r).string_data.has_edge; }, "string"));
	out.push_back(flag("string.wrap", [](void *r) { return &W(r).string_data.wrap; }, "string"));
	out.push_back(text("string.value", 1024, [](void *r) { return &W(r).string_data.value; }, Presence::Always,
	                   Ref::Dynamic, {}, "string"));
	// TOGGLE_STRING [orig: CButtonWnd_ParseTooltipXML @ 0x658170].
	out.push_back(toggle("toggle_string", [](void *r) { return &W(r).toggle_string.present; }));
	out.push_back(text("toggle_string.type", 16, [](void *r) { return &W(r).toggle_string.type; }, Presence::NonEmpty,
	                   Ref::None, id_choices(), "toggle_string"));
	out.push_back(text("toggle_string.value", 1024, [](void *r) { return &W(r).toggle_string.value; },
	                   Presence::Always, Ref::Dynamic, {}, "toggle_string"));
	out.push_back(toggle("items", [](void *r) { return &W(r).items.present; }));
	out.push_back(flag("items.multiselect", [](void *r) { return &W(r).items.multiselect; }, "items"));
	out.push_back(text("items.justify", 64, [](void *r) { return &W(r).items.justify; }, Presence::NonEmpty, Ref::None,
	                   justify_choices(), "items"));
	out.push_back(text("items.vjustify", 64, [](void *r) { return &W(r).items.vjustify; }, Presence::NonEmpty, Ref::None,
	                   vjustify_choices(), "items"));
	out.push_back(part_toggle<&mnu::Window::list_box, mnu::WindowType::List>("list_box"));
	out.push_back(number("list_box.sb_edge_pad", [](void *r) { return &W(r).sb_edge_pad; },
	                     [](void *r) { return &W(r).has_sb_edge_pad; }, "list_box"));
	out.push_back(part_toggle<&mnu::Window::spinup, mnu::WindowType::Button>("spinup"));
	out.push_back(part_toggle<&mnu::Window::spindown, mnu::WindowType::Button>("spindown"));
	out.push_back(number("column.count", [](void *r) { return &W(r).table_data.column.count; },
	                     [](void *r) { return &W(r).table_data.column.has_count; }));
	out.push_back(number("column.spacing", [](void *r) { return &W(r).table_data.column.spacing; },
	                     [](void *r) { return &W(r).table_data.column.has_spacing; }));
	out.push_back(sort_key<&mnu::TableColumn::primary_sort>("column.primary_sort"));
	{
		Entry token = text("column.primary_sort_token", 16,
		                   [](void *r) { return &W(r).table_data.column.primary_sort_token; }, Presence::NonEmpty,
		                   Ref::None, sort_token_choices());
		token.custom_set = &sort_token_set;
		out.push_back(token);
	}
	out.push_back(sort_key<&mnu::TableColumn::secondary_sort>("column.secondary_sort"));
	out.push_back(sort_key<&mnu::TableColumn::tertiary_sort>("column.tertiary_sort"));
	out.push_back(number("min_item_height", [](void *r) { return &W(r).table_data.min_item_height; },
	                     [](void *r) { return &W(r).table_data.has_min_item_height; }));
	out.push_back(number("fixed_header_height", [](void *r) { return &W(r).table_data.fixed_header_height; },
	                     [](void *r) { return &W(r).table_data.has_fixed_header_height; }));
	out.push_back(part_toggle<&mnu::Window::scrollbar, mnu::WindowType::Scroll>("scrollbar"));
	return out;
}

std::vector<Entry> part_entries() {
	std::vector<Entry> out;
	for (Entry &e : window_entries())
		if (std::strcmp(e.path, "type") != 0) out.push_back(std::move(e));
	return out;
}

mnu::Screen &SC(void *r) { return *static_cast<mnu::Screen *>(r); }
std::vector<Entry> screen_entries() {
	return {
	        text("name", 128, [](void *r) { return &SC(r).name; }),
	        number("music_var", [](void *r) { return &SC(r).music_var; }, [](void *r) { return &SC(r).has_music_var; }),
	};
}

mnu::Appearance &AP(void *r) { return *static_cast<mnu::Appearance *>(r); }
std::vector<Entry> appearance_entries() {
	return {
	        text("state", 16, [](void *r) { return &AP(r).state; }, Presence::NonEmpty, Ref::None,
	             {{"DEFAULT", 0}, {"DISABLED", 1}, {"MOUSEOVER", 2}, {"SELECTED", 3}}),
	        text("type", 16, [](void *r) { return &AP(r).type; }, Presence::NonEmpty, Ref::None,
	             {{"", 0}, {"IMAGE", 1}, {"COLOR", 2}, {"CUSTOM", 3}, {"OUTLINE", 4}, {"IMAGEROW", 5}}),
	        text("value", 128, [](void *r) { return &AP(r).value; }, Presence::Always, Ref::Dynamic),
	        number("map_state", [](void *r) { return &AP(r).map_state; }, [](void *r) { return &AP(r).has_map_state; }),
	        number("height", [](void *r) { return &AP(r).height; }, [](void *r) { return &AP(r).has_height; }),
	        material_flags(text("flags", 64, [](void *r) { return &AP(r).flags; })),
	};
}

mnu::Sound &SO(void *r) { return *static_cast<mnu::Sound *>(r); }
std::vector<Entry> sound_entries() {
	return {
	        text("state", 16, [](void *r) { return &SO(r).state; }, Presence::NonEmpty, Ref::None,
	             {{"MOUSEIN", 0}, {"MOUSEOUT", 1}, {"SELECTED", 2}}),
	        text("trigger", 32, [](void *r) { return &SO(r).trigger; }),
	        // The bank the text names, opened by that name [orig: SoundBank_CollectionAddOrRef @ 0x652b40 ->
	        // SoundBank_OpenFile @ 0x75caa0]; the parse ignores a bank that does not open (no sound plays for
	        // it).
	        text("file", 128, [](void *r) { return &SO(r).file; }, Presence::Always, Ref::Sound),
	};
}

mnu::Action &AC(void *r) { return *static_cast<mnu::Action *>(r); }
const std::vector<Choice> &field_attr_choices() {
	static const std::vector<Choice> c = {{"", 0}, {"FIELD", 1}, {"SOURCE", 2}, {"NAME", 3}};
	return c;
}
bool field_attr_set(void *r, const Value &value, std::string &error) {
	return name_token(field_attr_choices(), value, AC(r).field_attr, error,
	                  "The slot is written as FIELD, SOURCE or NAME (none writes FIELD).");
}
std::vector<Entry> action_entries() {
	// A token's code is its index + 1; the empty choice is code 0.
	const auto codes = [](const char *const *tokens, std::vector<Choice> choices) {
		for (int i = 0; tokens[i]; ++i) choices.push_back({tokens[i], i + 1});
		return choices;
	};
	std::vector<Entry> out;
	out.push_back(text("type", 32, [](void *r) { return &AC(r).type; }, Presence::NonEmpty, Ref::None,
	                   codes(mnu::kActionTypes, {})));
	out.push_back(text("state", 16, [](void *r) { return &AC(r).state; }, Presence::NonEmpty, Ref::None,
	                   codes(mnu::kActionStates, {{"", 0}})));
	out.push_back(text("file", 128, [](void *r) { return &AC(r).file; }, Presence::NonEmpty, Ref::Menu));
	out.push_back(text("field", 128, [](void *r) { return &AC(r).field; }, Presence::NonEmpty, Ref::Dynamic));
	{
		Entry slot = text("field_attr", 16, [](void *r) { return &AC(r).field_attr; }, Presence::NonEmpty, Ref::None,
		                  field_attr_choices());
		slot.custom_set = &field_attr_set;
		out.push_back(slot);
	}
	out.push_back(number("target_form", [](void *r) { return &AC(r).target_form; },
	                     [](void *r) { return &AC(r).has_target_form; }));
	out.push_back(flag("toggle", [](void *r) { return &AC(r).toggle; }));
	out.push_back(text("test", 8, [](void *r) { return &AC(r).test; }, Presence::NonEmpty, Ref::None,
	                   {{"", 0}, {"LT", 1}, {"LE", 2}, {"EQ", 3}, {"GE", 4}, {"GT", 5}}));
	out.push_back(text("target", 128, [](void *r) { return &AC(r).target; }, Presence::Always, Ref::Dynamic));
	out.push_back(flag("external_browser", [](void *r) { return &AC(r).external_browser; }));
	return out;
}

mnu::Hotkey &HK(void *r) { return *static_cast<mnu::Hotkey *>(r); }
std::vector<Entry> hotkey_entries() {
	return {
	        text("value", 32, [](void *r) { return &HK(r).value; }, Presence::Always),
	        flag("virtual", [](void *r) { return &HK(r).virtual_key; }),
	};
}

// A DATASOURCE names the credits file the marquee loads by that name [orig: CMarqueeWnd_ParseXMLDefinition
// @ 0x65ceb0 -> CMarqueeWnd_LoadCreditsFromIni @ 0x65c5a0 -> ConfigFile_LoadGlobal @ 0x760ad0]; one that
// does not load adds no line.
std::vector<Entry> datasource_entries() {
	return {
	        text("value", 128, [](void *r) { return static_cast<std::string *>(r); }, Presence::Always, Ref::Credits),
	};
}

mnu::Item &IT(void *r) { return *static_cast<mnu::Item *>(r); }
std::vector<Entry> item_entries() {
	return {
	        text("type", 16, [](void *r) { return &IT(r).type; }, Presence::NonEmpty, Ref::None,
	             {{"", 0}, {"ID", 1}, {"IMAGE", 2}, {"COLOR", 3}, {"BITMAP", 4}}),
	        text("value", 64, [](void *r) { return &IT(r).value; }),
	        text("text", 1024, [](void *r) { return &IT(r).text; }, Presence::Always, Ref::Dynamic),
	        text("justify", 64, [](void *r) { return &IT(r).justify; }, Presence::NonEmpty, Ref::None, justify_choices()),
	        text("vjustify", 64, [](void *r) { return &IT(r).vjustify; }, Presence::NonEmpty, Ref::None,
	             vjustify_choices()),
	        flag("pairs_list", [](void *r) { return &IT(r).pairs_list; }),
	        number("column", [](void *r) { return &IT(r).column; }, [](void *r) { return &IT(r).has_column; }),
	};
}

mnu::TableHeader &HD(void *r) { return *static_cast<mnu::TableHeader *>(r); }
std::vector<Entry> header_entries() {
	return {
	        text("justify", 64, [](void *r) { return &HD(r).justify; }, Presence::NonEmpty, Ref::None, justify_choices()),
	        text("vjustify", 64, [](void *r) { return &HD(r).vjustify; }, Presence::NonEmpty, Ref::None,
	             vjustify_choices()),
	        number("column", [](void *r) { return &HD(r).column; }, [](void *r) { return &HD(r).has_column; }),
	        text("sort", 8, [](void *r) { return &HD(r).sort; }),
	        number("width", [](void *r) { return &HD(r).width; }, [](void *r) { return &HD(r).has_width; }),
	        text("type", 8, [](void *r) { return &HD(r).type; }, Presence::NonEmpty, Ref::None, id_choices()),
	        text("text", 1024, [](void *r) { return &HD(r).text; }, Presence::Always, Ref::Dynamic),
	};
}

// A BODY's draw kind is the first authored of CUSTOM_DRAW / BITMAP_DRAW / BITMAP_TEXT (the writer puts it
// first); the kind and the three flags stay in step: a kind sets its flag, a flag set with no kind becomes
// the kind, and a flag cleared under the kind hands it to the next one still set.
mnu::TableBody &BD(void *r) { return *static_cast<mnu::TableBody *>(r); }
const char *const kDrawKinds[] = {"CUSTOM_DRAW", "BITMAP_DRAW", "BITMAP_TEXT"};
bool *draw_flag(mnu::TableBody &b, int k) { return k == 0 ? &b.custom_draw : k == 1 ? &b.bitmap_draw : &b.bitmap_text; }
void draw_kind_after_clear(mnu::TableBody &b) {
	for (int k = 0; k < 3; ++k)
		if (iequals(b.display, kDrawKinds[k]) && *draw_flag(b, k)) return;
	b.display.clear();
	for (int k = 0; k < 3; ++k)
		if (*draw_flag(b, k)) {
			b.display = kDrawKinds[k];
			return;
		}
}
bool display_set(void *r, const Value &value, std::string &error) {
	mnu::TableBody &b = BD(r);
	const std::string &token = std::get<std::string>(value);
	if (token.empty()) {
		for (int k = 0; k < 3; ++k) *draw_flag(b, k) = false;
		b.display.clear();
		return true;
	}
	for (int k = 0; k < 3; ++k) {
		if (!iequals(token, kDrawKinds[k])) continue;
		*draw_flag(b, k) = true;
		b.display = kDrawKinds[k];
		return true;
	}
	error = "The draw kind is CUSTOM_DRAW, BITMAP_DRAW, BITMAP_TEXT or none.";
	return false;
}
template <int K> bool draw_flag_set(void *r, const Value &value, std::string &) {
	mnu::TableBody &b = BD(r);
	*draw_flag(b, K) = std::get<int64_t>(value) != 0;
	if (*draw_flag(b, K) && b.display.empty()) b.display = kDrawKinds[K];
	else draw_kind_after_clear(b);
	return true;
}
template <int K> Entry draw_flag_entry(const char *path) {
	Entry e = flag(path, [](void *r) { return draw_flag(BD(r), K); });
	e.custom_set = &draw_flag_set<K>;
	return e;
}
std::vector<Entry> body_entries() {
	std::vector<Entry> out;
	out.push_back(text("justify", 64, [](void *r) { return &BD(r).justify; }, Presence::NonEmpty, Ref::None,
	                   justify_choices()));
	out.push_back(text("vjustify", 64, [](void *r) { return &BD(r).vjustify; }, Presence::NonEmpty, Ref::None,
	                   vjustify_choices()));
	out.push_back(number("column", [](void *r) { return &BD(r).column; }, [](void *r) { return &BD(r).has_column; }));
	Entry display = text("display", 16, [](void *r) { return &BD(r).display; }, Presence::NonEmpty, Ref::None,
	                     {{"", 0}, {"CUSTOM_DRAW", 1}, {"BITMAP_DRAW", 2}, {"BITMAP_TEXT", 3}});
	display.custom_set = &display_set;
	out.push_back(display);
	out.push_back(draw_flag_entry<0>("custom_draw"));
	out.push_back(draw_flag_entry<1>("bitmap_draw"));
	out.push_back(draw_flag_entry<2>("bitmap_text"));
	out.push_back(text("bitmap_flags", 64, [](void *r) { return &BD(r).bitmap_flags; }));
	out.push_back(flag("scale_bitmap", [](void *r) { return &BD(r).scale_bitmap; }));
	return out;
}

mnu::TableSubst &SU(void *r) { return *static_cast<mnu::TableSubst *>(r); }
std::vector<Entry> subst_entries() {
	return {
	        number("column", [](void *r) { return &SU(r).column; }, [](void *r) { return &SU(r).has_column; }),
	        text("value", 64, [](void *r) { return &SU(r).value; }),
	        flag("is_url", [](void *r) { return &SU(r).is_url; }),
	        flag("is_file", [](void *r) { return &SU(r).is_file; }),
	        text("file", 128, [](void *r) { return &SU(r).file; }, Presence::Always, Ref::Dynamic),
	};
}

// A tag or an attribute name the writer can put in an element: not empty, no whitespace and none of the
// characters that end a name or open a value.
bool plain_name(const std::string &name) {
	if (name.empty()) return false;
	for (const char c : name)
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '<' || c == '>' || c == '/' || c == '"' ||
		    c == '\'' || c == '=' || c == '&')
			return false;
	return true;
}
mnu::Element &EL(void *r) { return *static_cast<mnu::Element *>(r); }
// The tag as the reader keeps it: upper case (read_extra), so a save reads back as written.
bool tag_set(void *r, const Value &value, std::string &error) {
	const std::string &tag = std::get<std::string>(value);
	if (!plain_name(tag)) {
		error = "A tag is a plain name: no spaces and none of < > / \" ' = &.";
		return false;
	}
	EL(r).tag = strutil::to_upper(tag);
	return true;
}
std::vector<Entry> element_entries() {
	std::vector<Entry> out;
	Entry tag = text("tag", 64, [](void *r) { return &EL(r).tag; }, Presence::Always);
	tag.custom_set = &tag_set;
	out.push_back(tag);
	out.push_back(text("text", 1024, [](void *r) { return &EL(r).text; }, Presence::Always));
	return out;
}

mnu::ElementAttribute &AT(void *r) { return *static_cast<mnu::ElementAttribute *>(r); }
bool attribute_name_set(void *r, const Value &value, std::string &error) {
	const std::string &name = std::get<std::string>(value);
	if (!plain_name(name)) {
		error = "An attribute name is a plain name: no spaces and none of < > / \" ' = &.";
		return false;
	}
	AT(r).name = name;
	return true;
}
std::vector<Entry> attribute_entries() {
	std::vector<Entry> out;
	Entry name = text("name", 64, [](void *r) { return &AT(r).name; }, Presence::Always);
	name.custom_set = &attribute_name_set;
	out.push_back(name);
	Entry value = text("value", 128, [](void *r) { return &AT(r).value; }, Presence::Bit);
	value.bit = [](void *r) { return &AT(r).has_value; };
	out.push_back(value);
	return out;
}

constexpr size_t kShapeCount = size_t(MenuShape::Attribute) + 1;

std::vector<Entry> entries_of(MenuShape shape) {
	switch (shape) {
	case MenuShape::Screen: return screen_entries();
	case MenuShape::Window: return window_entries();
	case MenuShape::Part: return part_entries();
	case MenuShape::Appearance: return appearance_entries();
	case MenuShape::Sound: return sound_entries();
	case MenuShape::Action: return action_entries();
	case MenuShape::Hotkey: return hotkey_entries();
	case MenuShape::Datasource: return datasource_entries();
	case MenuShape::Item: return item_entries();
	case MenuShape::Row: return {};
	case MenuShape::Header: return header_entries();
	case MenuShape::Body: return body_entries();
	case MenuShape::Subst: return subst_entries();
	case MenuShape::Element: return element_entries();
	case MenuShape::Attribute: return attribute_entries();
	}
	return {};
}

// Every shape's entries, made once for the process: a field's value functions point at its entry and,
// for a member of a block, at the block's (author_block).
const std::array<std::vector<Entry>, kShapeCount> &all_entries() {
	static const std::array<std::vector<Entry>, kShapeCount> entries = [] {
		std::array<std::vector<Entry>, kShapeCount> out;
		for (size_t s = 0; s < kShapeCount; ++s) out[s] = entries_of(MenuShape(s));
		return out;
	}();
	return entries;
}

const Entry *entry_in(const std::vector<Entry> &entries, const char *path) {
	for (const Entry &e : entries)
		if (std::strcmp(path, e.path) == 0) return &e;
	return nullptr;
}

bool entry_get(const Entry &e, void *record, Value &out) {
	if (e.custom_get) return e.custom_get(record, out);
	if (e.text) {
		out = *e.text(record);
		return true;
	}
	if (e.number) {
		out = int64_t(*e.number(record));
		return true;
	}
	if (e.flag) {
		out = int64_t(*e.flag(record) ? 1 : 0);
		return true;
	}
	if (e.bit) {
		out = int64_t(*e.bit(record) ? 1 : 0);
		return true;
	}
	return false;
}

bool entry_own_present(const Entry &e, void *record) {
	if (e.custom_present) return e.custom_present(record);
	switch (e.presence) {
	case Presence::Always: return true;
	case Presence::Bit:
	case Presence::Block: return e.bit && *e.bit(record);
	case Presence::NonEmpty: return e.text ? !e.text(record)->empty() : true;
	}
	return true;
}

// The enclosing block's field authored (a STRING, an ITEMS, a part).
void author_block(const Entry *block, void *record) {
	if (!block) return;
	std::string ignored;
	if (block->custom_set) {
		if (!block->custom_present || !block->custom_present(record)) block->custom_set(record, Value(int64_t(1)), ignored);
	} else if (block->bit) {
		*block->bit(record) = true;
	}
}

// A value within the field's type and width. A Set of the value the field already reads changes nothing
// (an unauthored field stays unauthored, so a Set of every field to its own value leaves the bytes as
// they are); any other value authors the field's bit and the block that encloses it. False, with
// `error`, when the value does not fit.
bool entry_set(const Entry &e, const Entry *block, void *record, const Value &value, std::string &error) {
	if (e.type == Type::Text) {
		const auto *text = std::get_if<std::string>(&value);
		if (!text) {
			error = "This field takes text.";
			return false;
		}
		if (text->size() >= e.width) {
			error = "The text is too long.";
			return false;
		}
	} else {
		const auto *number = std::get_if<int64_t>(&value);
		if (!number) {
			error = e.type == Type::Flag ? "A flag is yes (1) or no (0)." : "This field takes a whole number.";
			return false;
		}
		if (e.type == Type::Integer && (*number < INT_MIN || *number > INT_MAX)) {
			error = "The number is out of range.";
			return false;
		}
	}
	const Value normalized = e.type == Type::Flag ? Value(int64_t(std::get<int64_t>(value) != 0 ? 1 : 0)) : value;
	Value current;
	if (entry_get(e, record, current) && current == normalized) return true; // nothing changes
	if (e.custom_set) {
		if (!e.custom_set(record, normalized, error)) return false;
	} else if (e.text) {
		*e.text(record) = std::get<std::string>(normalized);
	} else if (e.number) {
		*e.number(record) = int(std::get<int64_t>(normalized));
	} else if (e.flag) {
		*e.flag(record) = std::get<int64_t>(normalized) != 0;
	}
	if (e.bit) *e.bit(record) = e.presence == Presence::Block ? std::get<int64_t>(normalized) != 0 : true;
	author_block(block, record);
	return true;
}

// --- what a field names, by its record ----------------------------------------------------------------

ReferenceKind reference_of(Ref reference) {
	switch (reference) {
	case Ref::Font: return ReferenceKind::Font;
	case Ref::MenuTexture: return ReferenceKind::MenuTexture;
	case Ref::StyleVar: return ReferenceKind::StyleVar;
	case Ref::TextTable: return ReferenceKind::TextTable;
	case Ref::TextId: return ReferenceKind::TextId;
	case Ref::Menu: return ReferenceKind::Menu;
	case Ref::Sound: return ReferenceKind::SoundBank;
	case Ref::Credits: return ReferenceKind::Credits;
	case Ref::Screen: return ReferenceKind::MenuScreen;
	case Ref::Window: return ReferenceKind::MenuWindow;
	default: return ReferenceKind::None;
	}
}

// What a field whose sibling decides it names on this record: an APPEARANCE's value is a texture for
// IMAGE / IMAGEROW, a style colour for COLOR / OUTLINE; an ITEM's text a string id for ID, a texture for
// IMAGE / BITMAP, a colour for COLOR; a HEADER's text and the STRING's and TOGGLE_STRING's value a string
// id for ID; a SUBST's text a texture when it is a FILE and not a URL; an ACTION's target a screen for
// SCREEN, a window for WINDOW, TAB, GLB_FILTER and GLB_FILTER_NUM, and its FIELD / SOURCE / NAME slot a
// window for URL. None when it names nothing here.
ReferenceKind reference_on(MenuShape shape, const char *path, void *record) {
	const auto is = [](const std::string &token, const char *what) { return iequals(token, what); };
	switch (shape) {
	case MenuShape::Appearance: {
		const mnu::Appearance &a = AP(record);
		if (is(a.type, "IMAGE") || is(a.type, "IMAGEROW")) return ReferenceKind::MenuTexture;
		if (is(a.type, "COLOR") || is(a.type, "OUTLINE")) return ReferenceKind::StyleVar;
		return ReferenceKind::None;
	}
	case MenuShape::Item: {
		const mnu::Item &i = IT(record);
		if (is(i.type, "ID")) return ReferenceKind::TextId;
		if (is(i.type, "IMAGE") || is(i.type, "BITMAP")) return ReferenceKind::MenuTexture;
		if (is(i.type, "COLOR")) return ReferenceKind::StyleVar;
		return ReferenceKind::None;
	}
	case MenuShape::Header: return is(HD(record).type, "ID") ? ReferenceKind::TextId : ReferenceKind::None;
	case MenuShape::Action: {
		// The verb decides what the text and the slot name [orig: CUIWidget_HandleScriptedAction @
		// 0x6497f0]: SCREEN selects the screen of that name in the file it loads (@ 0x649894,
		// CUIScene_SelectNodeByName @ 0x63b6b0); WINDOW finds the window of that name on the acting
		// window's own screen (@ 0x6498c8, UI_FindScreenControl @ 0x63ae80); TAB moves the focus to the
		// control of that name (@ 0x649c30) and GLB_FILTER / GLB_FILTER_NUM send to it [orig:
		// CEditWnd_HandleInputEvent @ 0x661510], each on the screen showing, the acting window's while its
		// keys reach it; URL reads the text of the control the slot names (@ 0x649a1c). Every other
		// verb's text is no name the file defines.
		const mnu::Action &a = AC(record);
		if (std::strcmp(path, "target") == 0) {
			if (is(a.type, "SCREEN")) return ReferenceKind::MenuScreen;
			if (is(a.type, "WINDOW") || is(a.type, "TAB") || is(a.type, "GLB_FILTER") || is(a.type, "GLB_FILTER_NUM"))
				return ReferenceKind::MenuWindow;
			return ReferenceKind::None;
		}
		if (std::strcmp(path, "field") == 0) return is(a.type, "URL") ? ReferenceKind::MenuWindow : ReferenceKind::None;
		return ReferenceKind::None;
	}
	case MenuShape::Subst: {
		const mnu::TableSubst &s = SU(record);
		return s.is_file && !s.is_url ? ReferenceKind::MenuTexture : ReferenceKind::None;
	}
	case MenuShape::Window:
	case MenuShape::Part: {
		const mnu::Window &w = W(record);
		if (std::strcmp(path, "string.value") == 0)
			return is(w.string_data.type, "ID") ? ReferenceKind::TextId : ReferenceKind::None;
		if (std::strcmp(path, "toggle_string.value") == 0)
			return is(w.toggle_string.type, "ID") ? ReferenceKind::TextId : ReferenceKind::None;
		return ReferenceKind::None;
	}
	default: return ReferenceKind::None;
	}
}

// --- what the editor shows ---------------------------------------------------------------------------

// The readable names the inspector shows for the element paths (the path itself is the tooltip). A
// window's and a part's fields share one table.
struct PathLabel {
	const char *path;
	const char *label;
};

const PathLabel kWindowLabels[] = {
	{"name", "Name"}, {"type", "Type"}, {"hidden", "Hidden"}, {"disable", "Disabled"}, {"checked", "Checked"},
	{"draw_frame", "Draw the frame"}, {"modal", "Modal"}, {"readonly", "Read only"},
	{"as_button", "Acts as a button"}, {"number", "Numbers only"}, {"global_var", "Global variable"},
	{"password", "Password"}, {"group", "Group"}, {"form", "Form"}, {"minval", "Minimum"}, {"maxval", "Maximum"},
	{"maxchar", "Maximum characters"}, {"position.left", "Left"}, {"position.top", "Top"},
	{"position.right", "Right"}, {"position.bottom", "Bottom"}, {"frame.stencil", "Stencil"},
	{"frame.stencil_size", "Stencil size"}, {"frame.insetx", "Horizontal inset"}, {"frame.insety", "Vertical inset"},
	{"frame.brush", "Brush"}, {"frame.monogram", "Monogram"}, {"scroll_extent", "Scroll size"},
	{"scroll_extent_width", "Scroll size is a width"}, {"orientation", "Orientation"},
	{"text_rsrc", "String table"}, {"private_data", "Private data"}, {"cursor.file", "Pointer image"},
	{"cursor.flags", "Pointer flags"}, {"font.name", "Font"}, {"font.default_fg", "Text colour"},
	{"font.default_bg", "Background colour"}, {"font.mouseover_fg", "Mouse-over text colour"},
	{"font.mouseover_bg", "Mouse-over background"}, {"font.selected_fg", "Selected text colour"},
	{"font.selected_bg", "Selected background"}, {"font.disabled_fg", "Disabled text colour"},
	{"font.disabled_bg", "Disabled background"}, {"string", "Text"}, {"string.type", "Text is"},
	{"string.justify", "Horizontal alignment"}, {"string.vjustify", "Vertical alignment"},
	{"string.edge", "Edge padding"}, {"string.wrap", "Wrap lines"}, {"string.value", "Text"},
	{"toggle_string", "Toggle text"}, {"toggle_string.type", "Text is"}, {"toggle_string.value", "Text"},
	{"items", "Items"}, {"items.multiselect", "Multiple selection"}, {"items.justify", "Horizontal alignment"},
	{"items.vjustify", "Vertical alignment"}, {"list_box", "List box"},
	{"list_box.sb_edge_pad", "Scrollbar edge padding"}, {"spinup", "Spin up"}, {"spindown", "Spin down"},
	{"column.count", "Columns"}, {"column.spacing", "Column spacing"}, {"column.primary_sort", "Primary sort column"},
	{"column.primary_sort_token", "Primary sort written as"}, {"column.secondary_sort", "Secondary sort column"},
	{"column.tertiary_sort", "Tertiary sort column"}, {"min_item_height", "Minimum row height"},
	{"fixed_header_height", "Header height"}, {"scrollbar", "Scrollbar"},
};
// The groups a window's dotted paths form.
const PathLabel kWindowSections[] = {
	{"position", "Position"}, {"frame", "Frame"}, {"cursor", "Pointer"}, {"font", "Font and colours"},
	{"string", "Text"}, {"toggle_string", "Toggle text"}, {"items", "Items"}, {"list_box", "List box"},
	{"spinup", "Spin up"}, {"spindown", "Spin down"}, {"column", "Table columns"}, {"scrollbar", "Scrollbar"},
};
const PathLabel kScreenLabels[] = {{"name", "Name"}, {"music_var", "Music"}};
const PathLabel kAppearanceLabels[] = {{"state", "State"}, {"type", "Kind"}, {"value", "Image or colour"},
                                       {"map_state", "Sprite row"}, {"height", "Frame height"}, {"flags", "Texture flags"}};
const PathLabel kSoundLabels[] = {{"state", "When"}, {"trigger", "Sound"}, {"file", "Sound bank"}};
const PathLabel kActionLabels[] = {{"type", "Action"}, {"state", "Window state"}, {"file", "Menu file"},
                                   {"field", "Source control"}, {"field_attr", "Source written as"},
                                   {"target_form", "Target form"}, {"toggle", "Toggle"}, {"test", "Test"},
                                   {"target", "Target"}, {"external_browser", "Open in the web browser"}};
const PathLabel kHotkeyLabels[] = {{"value", "Key"}, {"virtual", "Virtual key"}};
const PathLabel kDatasourceLabels[] = {{"value", "File"}};
const PathLabel kItemLabels[] = {{"type", "Kind"}, {"value", "Value"}, {"text", "Text"},
                                 {"justify", "Horizontal alignment"}, {"vjustify", "Vertical alignment"},
                                 {"pairs_list", "Pairs list"}, {"column", "Column"}};
const PathLabel kHeaderLabels[] = {{"justify", "Horizontal alignment"}, {"vjustify", "Vertical alignment"},
                                   {"column", "Column"}, {"sort", "Sort"}, {"width", "Width"}, {"type", "Text is"},
                                   {"text", "Text"}};
const PathLabel kBodyLabels[] = {{"justify", "Horizontal alignment"}, {"vjustify", "Vertical alignment"},
                                 {"column", "Column"}, {"display", "Draw"}, {"custom_draw", "Custom draw"},
                                 {"bitmap_draw", "Bitmap"}, {"bitmap_text", "Bitmap and text"},
                                 {"bitmap_flags", "Bitmap flags"}, {"scale_bitmap", "Scale the bitmap"}};
const PathLabel kSubstLabels[] = {{"column", "Column"}, {"value", "Value"}, {"is_url", "Is a web address"},
                                  {"is_file", "Is a file"}, {"file", "Substitute"}};
const PathLabel kElementLabels[] = {{"tag", "Tag"}, {"text", "Text"}};
const PathLabel kAttributeLabels[] = {{"name", "Name"}, {"value", "Value"}};

template <size_t N> const char *find_label(const PathLabel (&table)[N], const std::string &path) {
	for (const PathLabel &entry : table)
		if (path == entry.path) return entry.label;
	return "";
}

bool is_window_shape(MenuShape shape) { return shape == MenuShape::Window || shape == MenuShape::Part; }

const char *field_label(MenuShape shape, const std::string &path) {
	switch (shape) {
	case MenuShape::Screen: return find_label(kScreenLabels, path);
	case MenuShape::Window:
	case MenuShape::Part: return find_label(kWindowLabels, path);
	case MenuShape::Appearance: return find_label(kAppearanceLabels, path);
	case MenuShape::Sound: return find_label(kSoundLabels, path);
	case MenuShape::Action: return find_label(kActionLabels, path);
	case MenuShape::Hotkey: return find_label(kHotkeyLabels, path);
	case MenuShape::Datasource: return find_label(kDatasourceLabels, path);
	case MenuShape::Item: return find_label(kItemLabels, path);
	case MenuShape::Row: return "";
	case MenuShape::Header: return find_label(kHeaderLabels, path);
	case MenuShape::Body: return find_label(kBodyLabels, path);
	case MenuShape::Subst: return find_label(kSubstLabels, path);
	case MenuShape::Element: return find_label(kElementLabels, path);
	case MenuShape::Attribute: return find_label(kAttributeLabels, path);
	}
	return "";
}

// The group heading of a window field: its path's first step's ("" for one outside any group; a block's
// toggle is named as its group).
const char *section_label(MenuShape shape, const std::string &path) {
	return is_window_shape(shape) ? find_label(kWindowSections, path.substr(0, path.find('.'))) : "";
}

// Whether a text field holds prose that may run over several lines.
bool multiline(MenuShape shape, const std::string &path) {
	if (is_window_shape(shape)) return path == "string.value" || path == "toggle_string.value" || path == "private_data";
	return (shape == MenuShape::Item || shape == MenuShape::Header || shape == MenuShape::Element) && path == "text";
}

// The readable name of a choice: the token's meaning, the token itself the tooltip. The verbs follow
// docs/mnu/menu-re.md "Authored Actions"; a token with no plainer name keeps its own ("" = the token).
struct ChoiceLabel {
	const char *field; // a field's path, or the last step of one ("justify")
	const char *token;
	const char *label;
};
const ChoiceLabel kWindowChoices[] = {
	{"type", "window", "Window (generic)"}, {"type", "static", "Static"}, {"type", "button", "Button"},
	{"type", "edit", "Text box"}, {"type", "multiline_edit", "Multi-line text box"}, {"type", "list", "List"},
	{"type", "checkbox", "Check box"}, {"type", "radio", "Radio button"}, {"type", "combobox", "Drop-down list"},
	{"type", "scroll", "Scrollbar"}, {"type", "table", "Table"}, {"type", "spinlist", "Spin list"},
	{"type", "marquee_wnd", "Marquee"}, {"type", "glb_table", "Server table"},
	{"type", "radioedit", "Radio button with a text box"}, {"type", "lan_list", "LAN game list"},
	{"type", "gopher", "Gopher browser"},
	{"orientation", "", "Not set"}, {"orientation", "HORIZONTAL", "Horizontal"}, {"orientation", "VERTICAL", "Vertical"},
	{"type", "ID", "A string id"}, {"type", "", "Plain text"},
	{"column.primary_sort_token", "", "Not set (PRIMARY_SORT)"}, {"cursor.flags", "", "Not written"},
};
const ChoiceLabel kAlignLabels[] = {
	{"justify", "", "Not set"}, {"justify", "LEFT", "Left"}, {"justify", "CENTER", "Centre"},
	{"justify", "RIGHT", "Right"}, {"vjustify", "", "Not set"}, {"vjustify", "TOP", "Top"},
	{"vjustify", "CENTER", "Middle"}, {"vjustify", "BOTTOM", "Bottom"},
};
const ChoiceLabel kAppearanceChoices[] = {
	{"state", "DEFAULT", "Normal"}, {"state", "DISABLED", "Disabled"}, {"state", "MOUSEOVER", "Mouse over"},
	{"state", "SELECTED", "Selected"}, {"type", "", "None (marks the state)"}, {"type", "IMAGE", "Image"},
	{"type", "COLOR", "Colour"}, {"type", "CUSTOM", "Custom"}, {"type", "OUTLINE", "Outline"},
	{"type", "IMAGEROW", "Image row"}, {"flags", "", "Not written"},
};
const ChoiceLabel kSoundChoices[] = {
	{"state", "MOUSEIN", "Mouse enters"}, {"state", "MOUSEOUT", "Mouse leaves"}, {"state", "SELECTED", "Selected"},
};
const ChoiceLabel kActionChoices[] = {
	{"type", "SCREEN", "Go to a screen"}, {"type", "WINDOW", "Show, hide, enable or disable a window"},
	{"type", "URL", "Open a web page"}, {"type", "FORM_POST", "Post the login form"},
	{"type", "GLB_LOAD", "Server list: load"}, {"type", "GLB_LOADANDPING", "Server list: load and ping"},
	{"type", "GLB_FILTER", "Server list: filter by text"}, {"type", "GLB_FILTER_NUM", "Server list: filter by number"},
	{"type", "GLB_PING", "Server list: ping"}, {"type", "GLB_JOIN", "Server list: join"},
	{"type", "TAB", "Tab to a control"}, {"type", "POP_SCREEN", "Go back"},
	{"type", "APPMSG", "Application message"}, {"type", "LAN_SEARCH", "LAN: search"},
	{"type", "LAN_JOIN", "LAN: join"}, {"type", "MNX", "MNX service"},
	{"state", "", "Not set"}, {"state", "HIDE", "Hide"}, {"state", "SHOW", "Show"}, {"state", "ENABLE", "Enable"},
	{"state", "DISABLE", "Disable"}, {"test", "", "Not set (less than)"}, {"test", "LT", "Less than"},
	{"test", "LE", "At most"}, {"test", "EQ", "Equal to"}, {"test", "GE", "At least"}, {"test", "GT", "Greater than"},
	{"field_attr", "", "Not set (FIELD)"},
};
const ChoiceLabel kItemChoices[] = {
	{"type", "", "Plain text"}, {"type", "ID", "A string id"}, {"type", "IMAGE", "Image"},
	{"type", "COLOR", "Colour"}, {"type", "BITMAP", "Bitmap"},
};
const ChoiceLabel kHeaderChoices[] = {{"type", "", "Plain text"}, {"type", "ID", "A string id"}};
const ChoiceLabel kBodyChoices[] = {
	{"display", "", "Not set"}, {"display", "CUSTOM_DRAW", "Custom draw"}, {"display", "BITMAP_DRAW", "Bitmap"},
	{"display", "BITMAP_TEXT", "Bitmap and text"},
};

template <size_t N> const char *find_choice(const ChoiceLabel (&table)[N], const std::string &field, const char *token) {
	for (const ChoiceLabel &entry : table)
		if (field == entry.field && std::strcmp(token, entry.token) == 0) return entry.label;
	return "";
}

const char *choice_label(MenuShape shape, const std::string &path, const char *token) {
	const std::string step = path.substr(path.rfind('.') == std::string::npos ? 0 : path.rfind('.') + 1);
	if (step == "justify" || step == "vjustify") return find_choice(kAlignLabels, step, token);
	switch (shape) {
	case MenuShape::Window:
	case MenuShape::Part:
		// STRING's and TOGGLE_STRING's TYPE: plain text or a string id, beside the window's own.
		return find_choice(kWindowChoices, path == "string.type" || path == "toggle_string.type" ? "type" : path, token);
	case MenuShape::Appearance: return find_choice(kAppearanceChoices, path, token);
	case MenuShape::Sound: return find_choice(kSoundChoices, path, token);
	case MenuShape::Action: return find_choice(kActionChoices, path, token);
	case MenuShape::Item: return find_choice(kItemChoices, path, token);
	case MenuShape::Header: return find_choice(kHeaderChoices, path, token);
	case MenuShape::Body: return find_choice(kBodyChoices, path, token);
	default: return "";
	}
}

const std::vector<FieldChoice> &yes_no() {
	static const std::vector<FieldChoice> choices = {{"no", 0}, {"yes", 1}};
	return choices;
}

// One entry as a labelled field: a flag is a yes / no integer, a Bit field is optional, a field whose
// reference a sibling decides references nothing until its record decides it; each with the name, the
// group and the choice names the editor shows.
LabelledField labelled(MenuShape shape, const std::vector<Entry> &entries, const Entry &e) {
	LabelledField out;
	FieldSchema &schema = out.schema;
	schema.id = e.path;
	schema.type = e.type == Type::Text ? FieldType::Text : FieldType::Integer;
	schema.width = e.width;
	schema.reference = reference_of(e.reference);
	if (e.type == Type::Flag) schema.choices = yes_no();
	for (const Choice &choice : e.choices) schema.choices.push_back({choice.name, choice.value, choice_label(shape, e.path, choice.name)});
	schema.open_choices = e.open;
	// A style colour's text is the hex AARRGGBB word the parse reads with wcstoul (a %VAR% the stylesheet
	// resolves first) [orig: CRT_wcstoxl @ 0x76e93b through the APPEARANCE COLOR / OUTLINE arm @ 0x648562,
	// the FONT colours @ 0x648d14..0x648e64 and the spin ITEM @ 0x64bd10]: every field naming a style
	// variable holds one.
	schema.color = schema.reference == ReferenceKind::StyleVar ? FieldColor::HexArgb : FieldColor::None;
	schema.optional = e.presence == Presence::Bit;
	// A screen's and a window's NAME is what the by-name lookups find them by (MnuDocument::lookup_names);
	// a part's NAME no lookup reads (MnuDocument::refine_field).
	if (std::strcmp(e.path, "name") == 0) {
		if (shape == MenuShape::Screen) schema.defines = ReferenceKind::MenuScreen;
		if (is_window_shape(shape)) schema.defines = ReferenceKind::MenuWindow;
	}
	schema.label = field_label(shape, e.path);
	schema.section = section_label(shape, e.path);
	schema.multiline = e.type == Type::Text && multiline(shape, e.path);

	const Entry *entry = &e;
	const Entry *block = *e.block ? entry_in(entries, e.block) : nullptr;
	out.value.get = [entry](const RecordHandle &record, Value &value) { return entry_get(*entry, record.data, value); };
	out.value.set = [entry, block](const RecordHandle &record, const Value &value, std::string &error) {
		return entry_set(*entry, block, record.data, value, error);
	};
	// A block's toggle always reads: its value is whether the block is written. Any other field is
	// written while its own presence says so and the block that encloses it is.
	if (e.presence != Presence::Block && (e.presence != Presence::Always || e.custom_present || block))
		out.value.present = [entry, block](const RecordHandle &record) {
			return entry_own_present(*entry, record.data) && (!block || entry_own_present(*block, record.data));
		};
	// An optional (Bit) field left out of the file or written again with the value it reads (writing one
	// authors its enclosing block).
	if (e.presence == Presence::Bit && e.bit)
		out.value.set_present = [entry, block](const RecordHandle &record, bool present, std::string &) {
			*entry->bit(record.data) = present;
			if (present) author_block(block, record.data);
			return true;
		};
	if (e.reference == Ref::Dynamic) {
		const char *path = e.path;
		out.reference = [shape, path](const RecordHandle &record) { return reference_on(shape, path, record.data); };
	}
	return out;
}

// --- the lists ---------------------------------------------------------------------------------------

// The defaults a new record takes: each survives the writer's skips and reads back as written. A window
// of type STATIC at 0,0,100,20 with a typeless DEFAULT appearance, an APPEARANCE with STATE DEFAULT, a
// SOUND on MOUSEIN with the MOUSE_OVER trigger, an ACTION of type POP_SCREEN (the one verb that takes no
// operand [orig: CUIWidget_HandleScriptedAction @ 0x6497f0, code 12]), a HEADER and a BODY with their
// COLUMN index written (the new column's), a SUBST with its COLUMN written, a window's PLAYERLIST
// attribute, an element's NAME attribute, an extra element TARGET.
void make_default(mnu::Window &w, size_t) {
	w.type = mnu::WindowType::Static;
	w.position = {0, 0, 100, 20, true, true, true, true};
	mnu::Appearance appearance;
	appearance.state = "default";
	w.appearances.push_back(appearance);
}
void make_default(mnu::Appearance &a, size_t) { a.state = "default"; }
void make_default(mnu::Sound &s, size_t) {
	s.state = "mousein";
	s.trigger = "MOUSE_OVER";
}
void make_default(mnu::Action &a, size_t) { a.type = "POP_SCREEN"; }
void make_default(mnu::Element &e, size_t) { e.tag = "TARGET"; }
void make_default(mnu::TableHeader &h, size_t index) {
	h.has_column = true;
	h.column = int(index);
}
void make_default(mnu::TableBody &b, size_t index) {
	b.has_column = true;
	b.column = int(index);
}
void make_default(mnu::TableSubst &s, size_t) { s.has_column = true; }
template <class T> void make_default(T &, size_t) {}

// A list of the owner's: a std::vector of its records, each new one taking its list's default, an ITEMS
// row authoring the ITEMS block it lies in.
template <class Owner, class Record>
ListOps vector_ops(NodeKind kind, const char *label, const char *record_label, std::vector<Record> &(*list)(Owner &),
                   void (*fresh)(Record &, size_t) = nullptr, bool items = false) {
	ListOps ops;
	ops.size = [list](const RecordHandle &owner) { return list(owner.as<Owner>()).size(); };
	ops.at = [list, kind](const RecordHandle &owner, size_t index) {
		std::vector<Record> &records = list(owner.as<Owner>());
		return index < records.size() ? RecordHandle{kind, &records[index]} : RecordHandle{};
	};
	ops.insert = [list, kind, label, record_label, fresh, items](const RecordHandle &owner, size_t index,
	                                                              const DetachedRecord *record, std::string &error) {
		if (record && (!record->data || record->kind != kind)) {
			error = std::string("The ") + label + " take " + record_label + " records only.";
			return false;
		}
		std::vector<Record> &records = list(owner.as<Owner>());
		index = std::min(index, records.size());
		if (record) {
			records.insert(records.begin() + std::ptrdiff_t(index), *static_cast<const Record *>(record->data.get()));
		} else {
			Record value{};
			if (fresh) fresh(value, index);
			records.insert(records.begin() + std::ptrdiff_t(index), std::move(value));
		}
		if constexpr (std::is_same<Owner, mnu::Window>::value)
			if (items) owner.as<mnu::Window>().items.present = true;
		return true;
	};
	ops.erase = [list](const RecordHandle &owner, size_t index) {
		std::vector<Record> &records = list(owner.as<Owner>());
		if (index >= records.size()) return false;
		records.erase(records.begin() + std::ptrdiff_t(index));
		return true;
	};
	ops.copy = [list, kind](const RecordHandle &owner, size_t index) {
		DetachedRecord out;
		std::vector<Record> &records = list(owner.as<Owner>());
		if (index >= records.size()) return out;
		out.kind = kind;
		out.data = std::make_shared<Record>(records[index]);
		return out;
	};
	if (items)
		ops.present = [](const RecordHandle &owner) { return owner.as<mnu::Window>().items.present; };
	return ops;
}

// A part's list: the one window the part holds, written or left out. A part with no window holds none; a
// new one is written with a typeless DEFAULT appearance, a moved one keeps whether it was written.
template <mnu::WindowPart mnu::Window::*Member, mnu::WindowType Type>
ListOps part_ops(NodeKind kind, const char *record_label) {
	ListOps ops;
	ops.size = [](const RecordHandle &owner) -> size_t { return (owner.as<mnu::Window>().*Member).latent() ? 1 : 0; };
	ops.at = [kind](const RecordHandle &owner, size_t index) {
		mnu::Window *held = (owner.as<mnu::Window>().*Member).latent();
		return index == 0 && held ? RecordHandle{kind, held} : RecordHandle{};
	};
	ops.insert = [kind, record_label](const RecordHandle &owner, size_t, const DetachedRecord *record,
	                                  std::string &error) {
		mnu::WindowPart &part = owner.as<mnu::Window>().*Member;
		if (record && (!record->data || record->kind != kind)) {
			error = std::string("The ") + record_label + " take " + record_label + " records only.";
			return false;
		}
		if (part.latent()) {
			error = std::string("A window holds one ") + record_label + " at most.";
			return false;
		}
		if (record) {
			part.author(Type) = *static_cast<const mnu::Window *>(record->data.get());
			if (!record->shown) part.hide();
		} else {
			default_part(part, Type);
		}
		return true;
	};
	ops.erase = [](const RecordHandle &owner, size_t index) {
		mnu::WindowPart &part = owner.as<mnu::Window>().*Member;
		if (index != 0 || !part.latent()) return false;
		part.clear();
		return true;
	};
	ops.copy = [kind](const RecordHandle &owner, size_t index) {
		DetachedRecord out;
		const mnu::WindowPart &part = owner.as<mnu::Window>().*Member;
		if (index != 0 || !part.latent()) return out;
		out.kind = kind;
		out.data = std::make_shared<mnu::Window>(*part.latent());
		out.shown = part.present();
		return out;
	};
	ops.present = [](const RecordHandle &owner) { return (owner.as<mnu::Window>().*Member).present(); };
	return ops;
}

// The kinds: Screen 0, Window 1, then one kind per list, named by the list's element path (a list path
// two owners share, "attribute" and "element", is one kind), a table row's cells ("item") last.
struct KindRow {
	const char *token;
	const char *label;
	MenuShape shape;
};
constexpr KindRow kKinds[] = {
	{"screen", "Screen", MenuShape::Screen},
	{"window", "Window", MenuShape::Window},
	{"attribute", "Attribute", MenuShape::Attribute},
	{"hotkey", "Hotkey", MenuShape::Hotkey},
	{"action", "Action", MenuShape::Action},
	{"appearance", "Appearance", MenuShape::Appearance},
	{"shuttle", "Shuttle", MenuShape::Appearance},
	{"scrollup", "Scroll up", MenuShape::Appearance},
	{"scrolldown", "Scroll down", MenuShape::Appearance},
	{"datasource", "Data source", MenuShape::Datasource},
	{"sound", "Sound", MenuShape::Sound},
	{"items.appearance", "Item appearance", MenuShape::Appearance},
	{"items.item", "Item", MenuShape::Item},
	{"items.row", "Row", MenuShape::Row},
	{"list_box", "List box", MenuShape::Part},
	{"spinup", "Spin up", MenuShape::Part},
	{"spindown", "Spin down", MenuShape::Part},
	{"column.header", "Header", MenuShape::Header},
	{"column.body", "Body", MenuShape::Body},
	{"column.subst", "Substitution", MenuShape::Subst},
	{"scrollbar", "Scrollbar", MenuShape::Part},
	{"element", "Element", MenuShape::Element},
	{"item", "Cell", MenuShape::Item},
};
constexpr size_t kKindCount = std::size(kKinds);

constexpr bool same_text(const char *a, const char *b) {
	while (*a && *a == *b) ++a, ++b;
	return *a == *b;
}
constexpr NodeKind kind_index(const char *token) {
	for (size_t i = 0; i < kKindCount; ++i)
		if (same_text(kKinds[i].token, token)) return NodeKind(i);
	return -1;
}
constexpr bool kinds_well_formed() {
	if (kKinds[0].shape != MenuShape::Screen || kKinds[1].shape != MenuShape::Window) return false;
	for (size_t i = 0; i < kKindCount; ++i)
		for (size_t j = i + 1; j < kKindCount; ++j)
			if (same_text(kKinds[i].token, kKinds[j].token)) return false;
	return true;
}
static_assert(size_t(MenuKind::Screen) == 0 && size_t(MenuKind::Window) == 1, "a screen and a window come first");
static_assert(kinds_well_formed(), "the menu's kinds: Screen, Window, then each list's records, each token its own");

constexpr NodeKind kWindowKind = node_kind(MenuKind::Window);

// A window's lists (a part's too) in the order the writer emits them (write_window), the child windows
// last, so a pre-order walk of the records meets them in file order.
std::vector<TableList> window_lists() {
	using Window = mnu::Window;
	const auto spec = [](const char *token, const char *label, const char *name_field, size_t max = 0) {
		Document::CollectionSpec out;
		out.kind = kind_index(token);
		out.label = label;
		out.name_field = name_field;
		out.max = max;
		return out;
	};
	std::vector<TableList> out;
	out.push_back({spec("attribute", "Attributes", "name"),
	               vector_ops<Window, mnu::ElementAttribute>(
	                       kind_index("attribute"), "Attributes", "Attribute",
	                       [](Window &w) -> std::vector<mnu::ElementAttribute> & { return w.extra_attributes; },
	                       // A window keeps only PLAYERLIST and SERVERLIST (GLB_TABLE).
	                       [](mnu::ElementAttribute &a, size_t) { a.name = "PLAYERLIST"; })});
	out.push_back({spec("hotkey", "Hotkeys", ""),
	               vector_ops<Window, mnu::Hotkey>(kind_index("hotkey"), "Hotkeys", "Hotkey",
	                                               [](Window &w) -> std::vector<mnu::Hotkey> & { return w.hotkeys; })});
	out.push_back({spec("action", "Actions", ""),
	               vector_ops<Window, mnu::Action>(kind_index("action"), "Actions", "Action",
	                                               [](Window &w) -> std::vector<mnu::Action> & { return w.actions; },
	                                               make_default)});
	out.push_back({spec("appearance", "Appearances", ""),
	               vector_ops<Window, mnu::Appearance>(
	                       kind_index("appearance"), "Appearances", "Appearance",
	                       [](Window &w) -> std::vector<mnu::Appearance> & { return w.appearances; }, make_default)});
	out.push_back({spec("shuttle", "Shuttle", ""),
	               vector_ops<Window, mnu::Appearance>(
	                       kind_index("shuttle"), "Shuttle", "Shuttle",
	                       [](Window &w) -> std::vector<mnu::Appearance> & { return w.shuttle; }, make_default)});
	out.push_back({spec("scrollup", "Scroll up", ""),
	               vector_ops<Window, mnu::Appearance>(
	                       kind_index("scrollup"), "Scroll up", "Scroll up",
	                       [](Window &w) -> std::vector<mnu::Appearance> & { return w.scrollup; }, make_default)});
	out.push_back({spec("scrolldown", "Scroll down", ""),
	               vector_ops<Window, mnu::Appearance>(
	                       kind_index("scrolldown"), "Scroll down", "Scroll down",
	                       [](Window &w) -> std::vector<mnu::Appearance> & { return w.scrolldown; }, make_default)});
	out.push_back({spec("datasource", "Data sources", ""),
	               vector_ops<Window, std::string>(kind_index("datasource"), "Data sources", "Data source",
	                                               [](Window &w) -> std::vector<std::string> & { return w.datasources; })});
	out.push_back({spec("sound", "Sounds", ""),
	               vector_ops<Window, mnu::Sound>(kind_index("sound"), "Sounds", "Sound",
	                                              [](Window &w) -> std::vector<mnu::Sound> & { return w.sounds; },
	                                              make_default)});
	out.push_back({spec("items.appearance", "Item appearances", ""),
	               vector_ops<Window, mnu::Appearance>(
	                       kind_index("items.appearance"), "Item appearances", "Item appearance",
	                       [](Window &w) -> std::vector<mnu::Appearance> & { return w.items.appearances; },
	                       make_default, true)});
	out.push_back({spec("items.item", "Items", ""),
	               vector_ops<Window, mnu::Item>(kind_index("items.item"), "Items", "Item",
	                                             [](Window &w) -> std::vector<mnu::Item> & { return w.items.items; },
	                                             nullptr, true)});
	out.push_back({spec("items.row", "Rows", ""),
	               vector_ops<Window, mnu::TableRow>(kind_index("items.row"), "Rows", "Row",
	                                                 [](Window &w) -> std::vector<mnu::TableRow> & { return w.items.rows; },
	                                                 nullptr, true)});
	out.push_back({spec("list_box", "List box", "name", 1),
	               part_ops<&Window::list_box, mnu::WindowType::List>(kind_index("list_box"), "List box")});
	out.push_back({spec("spinup", "Spin up", "name", 1),
	               part_ops<&Window::spinup, mnu::WindowType::Button>(kind_index("spinup"), "Spin up")});
	out.push_back({spec("spindown", "Spin down", "name", 1),
	               part_ops<&Window::spindown, mnu::WindowType::Button>(kind_index("spindown"), "Spin down")});
	out.push_back({spec("column.header", "Headers", ""),
	               vector_ops<Window, mnu::TableHeader>(
	                       kind_index("column.header"), "Headers", "Header",
	                       [](Window &w) -> std::vector<mnu::TableHeader> & { return w.table_data.column.headers; },
	                       make_default)});
	out.push_back({spec("column.body", "Bodies", ""),
	               vector_ops<Window, mnu::TableBody>(
	                       kind_index("column.body"), "Bodies", "Body",
	                       [](Window &w) -> std::vector<mnu::TableBody> & { return w.table_data.column.bodies; },
	                       make_default)});
	out.push_back({spec("column.subst", "Substitutions", ""),
	               vector_ops<Window, mnu::TableSubst>(
	                       kind_index("column.subst"), "Substitutions", "Substitution",
	                       [](Window &w) -> std::vector<mnu::TableSubst> & { return w.table_data.column.substitutions; },
	                       make_default)});
	out.push_back({spec("scrollbar", "Scrollbar", "name", 1),
	               part_ops<&Window::scrollbar, mnu::WindowType::Scroll>(kind_index("scrollbar"), "Scrollbar")});
	out.push_back({spec("element", "Elements", "tag"),
	               vector_ops<Window, mnu::Element>(kind_index("element"), "Elements", "Element",
	                                                [](Window &w) -> std::vector<mnu::Element> & { return w.extras; },
	                                                make_default)});
	out.push_back({spec("window", "Windows", "name"),
	               vector_ops<Window, mnu::Window>(kWindowKind, "Windows", "Window",
	                                               [](Window &w) -> std::vector<mnu::Window> & { return w.children; },
	                                               make_default)});
	return out;
}

RecordTable make_table() {
	std::vector<TableKind> kinds;
	for (size_t k = 0; k < kKindCount; ++k) {
		const KindRow &row = kKinds[k];
		const bool screen = k == size_t(MenuKind::Screen);
		TableKind kind(RecordKindRow{NodeKind(k), row.token, row.label, screen ? "Add screen" : "", screen});
		const std::vector<Entry> &entries = all_entries()[size_t(row.shape)];
		for (const Entry &e : entries) kind.field(labelled(row.shape, entries, e));
		Document::CollectionSpec windows;
		windows.kind = kWindowKind;
		windows.label = "Windows";
		windows.name_field = "name";
		switch (row.shape) {
		case MenuShape::Screen:
			kind.list({windows, vector_ops<mnu::Screen, mnu::Window>(
			                            kWindowKind, "Windows", "Window",
			                            [](mnu::Screen &s) -> std::vector<mnu::Window> & { return s.roots; }, make_default)});
			break;
		case MenuShape::Window:
		case MenuShape::Part:
			for (TableList &list : window_lists()) kind.list(std::move(list));
			break;
		case MenuShape::Row: {
			Document::CollectionSpec cells;
			cells.kind = kind_index("item");
			cells.label = "Cells";
			kind.list({cells, vector_ops<mnu::TableRow, mnu::Item>(
			                          kind_index("item"), "Cells", "Cell",
			                          [](mnu::TableRow &r) -> std::vector<mnu::Item> & { return r.cells; })});
			break;
		}
		case MenuShape::Element: {
			Document::CollectionSpec attributes, elements;
			attributes.kind = kind_index("attribute");
			attributes.label = "Attributes";
			attributes.name_field = "name";
			elements.kind = kind_index("element");
			elements.label = "Elements";
			elements.name_field = "tag";
			// An element takes any attribute name.
			kind.list({attributes, vector_ops<mnu::Element, mnu::ElementAttribute>(
			                               kind_index("attribute"), "Attributes", "Attribute",
			                               [](mnu::Element &e) -> std::vector<mnu::ElementAttribute> & { return e.attributes; },
			                               [](mnu::ElementAttribute &a, size_t) { a.name = "NAME"; })});
			kind.list({elements, vector_ops<mnu::Element, mnu::Element>(
			                             kind_index("element"), "Elements", "Element",
			                             [](mnu::Element &e) -> std::vector<mnu::Element> & { return e.children; },
			                             make_default)});
			break;
		}
		default: break;
		}
		kinds.push_back(std::move(kind));
	}
	return RecordTable(std::move(kinds));
}

} // namespace

const RecordTable &menu_table() {
	static const RecordTable table = make_table();
	return table;
}

MenuShape menu_shape(NodeKind kind) {
	return kind >= 0 && size_t(kind) < kKindCount ? kKinds[size_t(kind)].shape : MenuShape::Row;
}

bool is_window_kind(NodeKind kind) { return kind >= 0 && size_t(kind) < kKindCount && is_window_shape(menu_shape(kind)); }

NodeKind menu_kind(const std::string &token) { return kind_index(token.c_str()); }

size_t menu_window_list(const std::string &path) {
	const std::vector<TableList> &lists = menu_table().kind(kWindowKind)->lists();
	for (size_t i = 0; i < lists.size(); ++i)
		if (path == menu_table().kind(lists[i].spec.kind)->row().token) return i;
	return SIZE_MAX;
}

size_t menu_children_list() {
	static const size_t index = menu_window_list("window");
	return index;
}

DetachedRecord menu_window_record(const mnu::Window &window) {
	DetachedRecord out;
	out.kind = kWindowKind;
	out.data = std::make_shared<mnu::Window>(window);
	return out;
}

} // namespace opennova::editor
