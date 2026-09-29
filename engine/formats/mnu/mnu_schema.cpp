// The mnu property table: each record shape's fields with the slot the reader fills,
// the lists a record holds with the vector (or part) behind each, the defaults a new
// record takes, and which window type reads what.
#include <formats/mnu/mnu_schema.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <type_traits>
#include <utility>

#include <base/io/strutil.h>

namespace opennova::mnu {

namespace {

using strutil::iequals;

// --- the field table ---------------------------------------------------------------

// Where a field's value lives in its record. A plain field names one slot (a text, a
// number or a flag) and, for a Bit or a Block field, the presence bit; a field with its own
// rules (a window's TYPE, a part's toggle, a BODY's draw kind, a table sort key) reads and
// writes through its own functions.
struct Entry {
  SchemaField field;
  std::string *(*text)(void *) = nullptr;
  int *(*number)(void *) = nullptr;
  bool *(*flag)(void *) = nullptr;
  bool *(*bit)(void *) = nullptr;
  bool (*custom_get)(void *, SchemaValue &) = nullptr;
  bool (*custom_set)(void *, const SchemaValue &, std::string &) = nullptr;
  bool (*custom_present)(void *) = nullptr;
};

Window &W(void *r) { return *static_cast<Window *>(r); }

const std::vector<SchemaChoice> &justify_choices() {
  static const std::vector<SchemaChoice> c = {{"", 0}, {"LEFT", 1}, {"CENTER", 2}, {"RIGHT", 3}};
  return c;
}
const std::vector<SchemaChoice> &vjustify_choices() {
  static const std::vector<SchemaChoice> c = {{"", 0}, {"TOP", 1}, {"CENTER", 2}, {"BOTTOM", 3}};
  return c;
}
const std::vector<SchemaChoice> &id_choices() {
  static const std::vector<SchemaChoice> c = {{"", 0}, {"ID", 1}};
  return c;
}

Entry text(const char *path, size_t width, std::string *(*slot)(void *), SchemaPresence presence = SchemaPresence::NonEmpty,
           SchemaReference reference = SchemaReference::None, std::vector<SchemaChoice> choices = {},
           const char *block = "") {
  Entry e;
  e.field = {path, SchemaType::Text, width, presence, reference, std::move(choices), block};
  e.text = slot;
  return e;
}

// A FLAGS text: no text (the element not written: the writer leaves an empty FLAGS out),
// then the material flag tokens, any text taken (ui_material_flag_choices).
Entry material_flags(Entry e) {
  e.field.choices = {{"", 0}};
  const std::vector<SchemaChoice> &tokens = ui_material_flag_choices();
  e.field.choices.insert(e.field.choices.end(), tokens.begin(), tokens.end());
  e.field.open = true;
  return e;
}

Entry number(const char *path, int *(*slot)(void *), bool *(*bit)(void *), const char *block = "") {
  Entry e;
  e.field = {path, SchemaType::Integer, 0, bit ? SchemaPresence::Bit : SchemaPresence::Always, SchemaReference::None, {},
             block};
  e.number = slot;
  e.bit = bit;
  return e;
}

Entry flag(const char *path, bool *(*slot)(void *), const char *block = "") {
  Entry e;
  e.field = {path, SchemaType::Flag, 0, SchemaPresence::Always, SchemaReference::None, {}, block};
  e.flag = slot;
  return e;
}

Entry toggle(const char *path, bool *(*bit)(void *)) {
  Entry e;
  e.field = {path, SchemaType::Flag, 0, SchemaPresence::Block, SchemaReference::None, {}, ""};
  e.bit = bit;
  return e;
}

// A window's TYPE: the factory's match of the token, the token written as typed (a token
// it does not match builds a generic window [orig: CUIScene_CreateWidgetByType @ 0x64f630]).
bool type_get(void *r, SchemaValue &out) {
  const Window &w = W(r);
  out = w.type_token.empty() ? std::string(window_type_name(w.type)) : w.type_token;
  return true;
}
bool type_set(void *r, const SchemaValue &value, std::string &) {
  Window &w = W(r);
  const std::string &token = std::get<std::string>(value);
  w.type = parse_window_type(token);
  w.type_token = token;
  return true;
}

// A part's default: written, with a typeless DEFAULT appearance (a part with no element
// would crash retail).
Window &default_part(WindowPart &part, WindowType type) {
  Window &w = part.author(type);
  if (w.appearances.empty()) {
    Appearance appearance;
    appearance.state = "default";
    w.appearances.push_back(appearance);
  }
  return w;
}

// A part's toggle: 0 leaves the part out and keeps its window, 1 writes it again. A part
// that was never authored is added to its list (schema_list_insert), not toggled on: the
// toggle changes no list's length.
template <WindowPart Window::*Member>
bool part_get(void *r, SchemaValue &out) {
  out = int64_t((W(r).*Member).present() ? 1 : 0);
  return true;
}
template <WindowPart Window::*Member, WindowType Type>
bool part_set(void *r, const SchemaValue &value, std::string &error) {
  WindowPart &part = W(r).*Member;
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
template <WindowPart Window::*Member>
bool part_present(void *r) {
  return (W(r).*Member).present();
}
template <WindowPart Window::*Member, WindowType Type>
Entry part_toggle(const char *path) {
  Entry e;
  e.field = {path, SchemaType::Flag, 0, SchemaPresence::Block, SchemaReference::None, {}, ""};
  e.custom_get = &part_get<Member>;
  e.custom_set = &part_set<Member, Type>;
  e.custom_present = &part_present<Member>;
  return e;
}

// A token the writer puts down as an attribute's NAME (an ACTION's FIELD / SOURCE / NAME
// slot, the table's primary sort key): only one the reader matches, spelled as it keeps
// it; any other would write an attribute retail reads as something else, or faults on.
bool name_token(const std::vector<SchemaChoice> &choices, const SchemaValue &value, std::string &slot,
                std::string &error, const char *what) {
  const std::string &text = std::get<std::string>(value);
  for (const SchemaChoice &choice : choices)
    if (iequals(text, choice.name)) {
      slot = choice.name;
      return true;
    }
  error = what;
  return false;
}

const std::vector<SchemaChoice> &sort_token_choices() {
  static const std::vector<SchemaChoice> c = {{"", 0}, {"PRIMARY_SORT", 1}, {"DEFAULT_SORT", 2}};
  return c;
}
bool sort_token_set(void *r, const SchemaValue &value, std::string &error) {
  return name_token(sort_token_choices(), value, W(r).table_data.column.primary_sort_token, error,
                    "The primary sort key is written as PRIMARY_SORT or DEFAULT_SORT (none writes PRIMARY_SORT).");
}

// A table sort key: the column index the HEADER walk binds it to, -1 when no HEADER sets it.
template <int TableColumn::*Member>
bool sort_present(void *r) {
  return W(r).table_data.column.*Member >= 0;
}
template <int TableColumn::*Member>
Entry sort_key(const char *path) {
  Entry e;
  e.field = {path, SchemaType::Integer, 0, SchemaPresence::Always, SchemaReference::None, {}, ""};
  e.number = [](void *r) -> int * { return &(W(r).table_data.column.*Member); };
  e.custom_present = &sort_present<Member>;
  return e;
}

const std::vector<Entry> &window_entries() {
  static const std::vector<Entry> entries = [] {
    std::vector<Entry> out;
    out.push_back(text("name", 128, [](void *r) { return &W(r).name; }));
    {
      Entry type;
      type.field = {"type", SchemaType::Text, 32, SchemaPresence::Always, SchemaReference::None, {}, ""};
      for (int i = 0; i < kWindowTypeCount; ++i)
        type.field.choices.push_back({window_type_name(WindowType(i)), i});
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
    out.push_back(text("frame.stencil", 128, [](void *r) { return &W(r).frame.stencil; }, SchemaPresence::NonEmpty,
                       SchemaReference::MenuTexture));
    out.push_back(number("frame.stencil_size", [](void *r) { return &W(r).frame.stencil_size; },
                         [](void *r) { return &W(r).frame.has_stencil_size; }));
    out.push_back(number("frame.insetx", [](void *r) { return &W(r).frame.insetx; },
                         [](void *r) { return &W(r).frame.has_insetx; }));
    out.push_back(number("frame.insety", [](void *r) { return &W(r).frame.insety; },
                         [](void *r) { return &W(r).frame.has_insety; }));
    out.push_back(text("frame.brush", 128, [](void *r) { return &W(r).frame.brush; }, SchemaPresence::NonEmpty,
                       SchemaReference::MenuTexture));
    out.push_back(text("frame.monogram", 128, [](void *r) { return &W(r).frame.monogram; }, SchemaPresence::NonEmpty,
                       SchemaReference::MenuTexture));
    // SCROLL: the one along-axis extent a window-level HEIGHT or WIDTH sets, with its spelling.
    out.push_back(number("scroll_extent", [](void *r) { return &W(r).scroll_extent; },
                         [](void *r) { return &W(r).has_scroll_extent; }));
    out.push_back(flag("scroll_extent_width", [](void *r) { return &W(r).scroll_extent_is_width; }));
    out.push_back(text("orientation", 32, [](void *r) { return &W(r).orientation; }, SchemaPresence::NonEmpty,
                       SchemaReference::None, {{"", 0}, {"HORIZONTAL", 1}, {"VERTICAL", 2}}));
    {
      Entry rsrc = text("text_rsrc", 64, [](void *r) { return &W(r).text_rsrc; }, SchemaPresence::Bit,
                        SchemaReference::TextTable);
      rsrc.bit = [](void *r) { return &W(r).has_text_rsrc; };
      out.push_back(rsrc);
    }
    out.push_back(text("private_data", 1024, [](void *r) { return &W(r).private_data; }));
    out.push_back(text("cursor.file", 64, [](void *r) { return &W(r).cursor.file; }, SchemaPresence::NonEmpty,
                       SchemaReference::MenuTexture));
    out.push_back(material_flags(text("cursor.flags", 64, [](void *r) { return &W(r).cursor.flags; })));
    out.push_back(text("font.name", 64, [](void *r) { return &W(r).font.name; }, SchemaPresence::NonEmpty,
                       SchemaReference::Font));
    const SchemaReference color = SchemaReference::StyleVar;
    out.push_back(text("font.default_fg", 32, [](void *r) { return &W(r).font.default_fg; }, SchemaPresence::NonEmpty, color));
    out.push_back(text("font.default_bg", 32, [](void *r) { return &W(r).font.default_bg; }, SchemaPresence::NonEmpty, color));
    out.push_back(text("font.mouseover_fg", 32, [](void *r) { return &W(r).font.mouseover_fg; }, SchemaPresence::NonEmpty, color));
    out.push_back(text("font.mouseover_bg", 32, [](void *r) { return &W(r).font.mouseover_bg; }, SchemaPresence::NonEmpty, color));
    out.push_back(text("font.selected_fg", 32, [](void *r) { return &W(r).font.selected_fg; }, SchemaPresence::NonEmpty, color));
    out.push_back(text("font.selected_bg", 32, [](void *r) { return &W(r).font.selected_bg; }, SchemaPresence::NonEmpty, color));
    out.push_back(text("font.disabled_fg", 32, [](void *r) { return &W(r).font.disabled_fg; }, SchemaPresence::NonEmpty, color));
    out.push_back(text("font.disabled_bg", 32, [](void *r) { return &W(r).font.disabled_bg; }, SchemaPresence::NonEmpty, color));
    // STRING [orig: CUIButtonWidget_ParseXMLAttributes @ 0x657c30].
    out.push_back(toggle("string", [](void *r) { return &W(r).string_data.present; }));
    out.push_back(text("string.type", 16, [](void *r) { return &W(r).string_data.type; }, SchemaPresence::NonEmpty,
                       SchemaReference::None, id_choices(), "string"));
    out.push_back(text("string.justify", 64, [](void *r) { return &W(r).string_data.justify; }, SchemaPresence::NonEmpty,
                       SchemaReference::None, justify_choices(), "string"));
    out.push_back(text("string.vjustify", 64, [](void *r) { return &W(r).string_data.vjustify; },
                       SchemaPresence::NonEmpty, SchemaReference::None, vjustify_choices(), "string"));
    out.push_back(number("string.edge", [](void *r) { return &W(r).string_data.edge; },
                         [](void *r) { return &W(r).string_data.has_edge; }, "string"));
    out.push_back(flag("string.wrap", [](void *r) { return &W(r).string_data.wrap; }, "string"));
    out.push_back(text("string.value", 1024, [](void *r) { return &W(r).string_data.value; }, SchemaPresence::Always,
                       SchemaReference::Dynamic, {}, "string"));
    // TOGGLE_STRING [orig: CButtonWnd_ParseTooltipXML @ 0x658170].
    out.push_back(toggle("toggle_string", [](void *r) { return &W(r).toggle_string.present; }));
    out.push_back(text("toggle_string.type", 16, [](void *r) { return &W(r).toggle_string.type; },
                       SchemaPresence::NonEmpty, SchemaReference::None, id_choices(), "toggle_string"));
    out.push_back(text("toggle_string.value", 1024, [](void *r) { return &W(r).toggle_string.value; },
                       SchemaPresence::Always, SchemaReference::Dynamic, {}, "toggle_string"));
    out.push_back(toggle("items", [](void *r) { return &W(r).items.present; }));
    out.push_back(flag("items.multiselect", [](void *r) { return &W(r).items.multiselect; }, "items"));
    out.push_back(text("items.justify", 64, [](void *r) { return &W(r).items.justify; }, SchemaPresence::NonEmpty,
                       SchemaReference::None, justify_choices(), "items"));
    out.push_back(text("items.vjustify", 64, [](void *r) { return &W(r).items.vjustify; }, SchemaPresence::NonEmpty,
                       SchemaReference::None, vjustify_choices(), "items"));
    out.push_back(part_toggle<&Window::list_box, WindowType::List>("list_box"));
    out.push_back(number("list_box.sb_edge_pad", [](void *r) { return &W(r).sb_edge_pad; },
                         [](void *r) { return &W(r).has_sb_edge_pad; }, "list_box"));
    out.push_back(part_toggle<&Window::spinup, WindowType::Button>("spinup"));
    out.push_back(part_toggle<&Window::spindown, WindowType::Button>("spindown"));
    out.push_back(number("column.count", [](void *r) { return &W(r).table_data.column.count; },
                         [](void *r) { return &W(r).table_data.column.has_count; }));
    out.push_back(number("column.spacing", [](void *r) { return &W(r).table_data.column.spacing; },
                         [](void *r) { return &W(r).table_data.column.has_spacing; }));
    out.push_back(sort_key<&TableColumn::primary_sort>("column.primary_sort"));
    {
      Entry token = text("column.primary_sort_token", 16, [](void *r) { return &W(r).table_data.column.primary_sort_token; },
                         SchemaPresence::NonEmpty, SchemaReference::None, sort_token_choices());
      token.custom_set = &sort_token_set;
      out.push_back(token);
    }
    out.push_back(sort_key<&TableColumn::secondary_sort>("column.secondary_sort"));
    out.push_back(sort_key<&TableColumn::tertiary_sort>("column.tertiary_sort"));
    out.push_back(number("min_item_height", [](void *r) { return &W(r).table_data.min_item_height; },
                         [](void *r) { return &W(r).table_data.has_min_item_height; }));
    out.push_back(number("fixed_header_height", [](void *r) { return &W(r).table_data.fixed_header_height; },
                         [](void *r) { return &W(r).table_data.has_fixed_header_height; }));
    out.push_back(part_toggle<&Window::scrollbar, WindowType::Scroll>("scrollbar"));
    return out;
  }();
  return entries;
}

const std::vector<Entry> &part_entries() {
  static const std::vector<Entry> entries = [] {
    std::vector<Entry> out;
    for (const Entry &e : window_entries())
      if (std::strcmp(e.field.path, "type") != 0) out.push_back(e);
    return out;
  }();
  return entries;
}

Screen &SC(void *r) { return *static_cast<Screen *>(r); }
const std::vector<Entry> &screen_entries() {
  static const std::vector<Entry> entries = {
      text("name", 128, [](void *r) { return &SC(r).name; }),
      number("music_var", [](void *r) { return &SC(r).music_var; }, [](void *r) { return &SC(r).has_music_var; }),
  };
  return entries;
}

Appearance &AP(void *r) { return *static_cast<Appearance *>(r); }
const std::vector<Entry> &appearance_entries() {
  static const std::vector<Entry> entries = {
      text("state", 16, [](void *r) { return &AP(r).state; }, SchemaPresence::NonEmpty, SchemaReference::None,
           {{"DEFAULT", 0}, {"DISABLED", 1}, {"MOUSEOVER", 2}, {"SELECTED", 3}}),
      text("type", 16, [](void *r) { return &AP(r).type; }, SchemaPresence::NonEmpty, SchemaReference::None,
           {{"", 0}, {"IMAGE", 1}, {"COLOR", 2}, {"CUSTOM", 3}, {"OUTLINE", 4}, {"IMAGEROW", 5}}),
      text("value", 128, [](void *r) { return &AP(r).value; }, SchemaPresence::Always, SchemaReference::Dynamic),
      number("map_state", [](void *r) { return &AP(r).map_state; }, [](void *r) { return &AP(r).has_map_state; }),
      number("height", [](void *r) { return &AP(r).height; }, [](void *r) { return &AP(r).has_height; }),
      material_flags(text("flags", 64, [](void *r) { return &AP(r).flags; })),
  };
  return entries;
}

Sound &SO(void *r) { return *static_cast<Sound *>(r); }
const std::vector<Entry> &sound_entries() {
  static const std::vector<Entry> entries = {
      text("state", 16, [](void *r) { return &SO(r).state; }, SchemaPresence::NonEmpty, SchemaReference::None,
           {{"MOUSEIN", 0}, {"MOUSEOUT", 1}, {"SELECTED", 2}}),
      text("trigger", 32, [](void *r) { return &SO(r).trigger; }),
      // The bank the text names, opened by that name [orig: SoundBank_CollectionAddOrRef
      // @ 0x652b40 -> SoundBank_OpenFile @ 0x75caa0]; the parse ignores a bank that does
      // not open (no sound plays for it).
      text("file", 128, [](void *r) { return &SO(r).file; }, SchemaPresence::Always, SchemaReference::Sound),
  };
  return entries;
}

Action &AC(void *r) { return *static_cast<Action *>(r); }
const std::vector<SchemaChoice> &field_attr_choices() {
  static const std::vector<SchemaChoice> c = {{"", 0}, {"FIELD", 1}, {"SOURCE", 2}, {"NAME", 3}};
  return c;
}
bool field_attr_set(void *r, const SchemaValue &value, std::string &error) {
  return name_token(field_attr_choices(), value, AC(r).field_attr, error,
                    "The slot is written as FIELD, SOURCE or NAME (none writes FIELD).");
}
const std::vector<Entry> &action_entries() {
  static const std::vector<Entry> entries = [] {
    // A token's code is its index + 1; the empty choice is code 0.
    const auto codes = [](const char *const *tokens, std::vector<SchemaChoice> choices) {
      for (int i = 0; tokens[i]; ++i) choices.push_back({tokens[i], i + 1});
      return choices;
    };
    std::vector<Entry> out;
    out.push_back(text("type", 32, [](void *r) { return &AC(r).type; }, SchemaPresence::NonEmpty,
                       SchemaReference::None, codes(kActionTypes, {})));
    out.push_back(text("state", 16, [](void *r) { return &AC(r).state; }, SchemaPresence::NonEmpty,
                       SchemaReference::None, codes(kActionStates, {{"", 0}})));
    out.push_back(text("file", 128, [](void *r) { return &AC(r).file; }, SchemaPresence::NonEmpty,
                       SchemaReference::Menu));
    out.push_back(text("field", 128, [](void *r) { return &AC(r).field; }, SchemaPresence::NonEmpty,
                       SchemaReference::Dynamic));
    {
      Entry slot = text("field_attr", 16, [](void *r) { return &AC(r).field_attr; }, SchemaPresence::NonEmpty,
                        SchemaReference::None, field_attr_choices());
      slot.custom_set = &field_attr_set;
      out.push_back(slot);
    }
    out.push_back(number("target_form", [](void *r) { return &AC(r).target_form; },
                         [](void *r) { return &AC(r).has_target_form; }));
    out.push_back(flag("toggle", [](void *r) { return &AC(r).toggle; }));
    out.push_back(text("test", 8, [](void *r) { return &AC(r).test; }, SchemaPresence::NonEmpty, SchemaReference::None,
                       {{"", 0}, {"LT", 1}, {"LE", 2}, {"EQ", 3}, {"GE", 4}, {"GT", 5}}));
    out.push_back(text("target", 128, [](void *r) { return &AC(r).target; }, SchemaPresence::Always,
                       SchemaReference::Dynamic));
    out.push_back(flag("external_browser", [](void *r) { return &AC(r).external_browser; }));
    return out;
  }();
  return entries;
}

Hotkey &HK(void *r) { return *static_cast<Hotkey *>(r); }
const std::vector<Entry> &hotkey_entries() {
  static const std::vector<Entry> entries = {
      text("value", 32, [](void *r) { return &HK(r).value; }, SchemaPresence::Always),
      flag("virtual", [](void *r) { return &HK(r).virtual_key; }),
  };
  return entries;
}

// A DATASOURCE names the credits file the marquee loads by that name [orig:
// CMarqueeWnd_ParseXMLDefinition @ 0x65ceb0 -> CMarqueeWnd_LoadCreditsFromIni @ 0x65c5a0 ->
// ConfigFile_LoadGlobal @ 0x760ad0]; one that does not load adds no line.
const std::vector<Entry> &datasource_entries() {
  static const std::vector<Entry> entries = {
      text("value", 128, [](void *r) { return static_cast<std::string *>(r); }, SchemaPresence::Always,
           SchemaReference::Credits),
  };
  return entries;
}

Item &IT(void *r) { return *static_cast<Item *>(r); }
const std::vector<Entry> &item_entries() {
  static const std::vector<Entry> entries = {
      text("type", 16, [](void *r) { return &IT(r).type; }, SchemaPresence::NonEmpty, SchemaReference::None,
           {{"", 0}, {"ID", 1}, {"IMAGE", 2}, {"COLOR", 3}, {"BITMAP", 4}}),
      text("value", 64, [](void *r) { return &IT(r).value; }),
      text("text", 1024, [](void *r) { return &IT(r).text; }, SchemaPresence::Always, SchemaReference::Dynamic),
      text("justify", 64, [](void *r) { return &IT(r).justify; }, SchemaPresence::NonEmpty, SchemaReference::None,
           justify_choices()),
      text("vjustify", 64, [](void *r) { return &IT(r).vjustify; }, SchemaPresence::NonEmpty, SchemaReference::None,
           vjustify_choices()),
      flag("pairs_list", [](void *r) { return &IT(r).pairs_list; }),
      number("column", [](void *r) { return &IT(r).column; }, [](void *r) { return &IT(r).has_column; }),
  };
  return entries;
}

TableHeader &HD(void *r) { return *static_cast<TableHeader *>(r); }
const std::vector<Entry> &header_entries() {
  static const std::vector<Entry> entries = {
      text("justify", 64, [](void *r) { return &HD(r).justify; }, SchemaPresence::NonEmpty, SchemaReference::None,
           justify_choices()),
      text("vjustify", 64, [](void *r) { return &HD(r).vjustify; }, SchemaPresence::NonEmpty, SchemaReference::None,
           vjustify_choices()),
      number("column", [](void *r) { return &HD(r).column; }, [](void *r) { return &HD(r).has_column; }),
      text("sort", 8, [](void *r) { return &HD(r).sort; }),
      number("width", [](void *r) { return &HD(r).width; }, [](void *r) { return &HD(r).has_width; }),
      text("type", 8, [](void *r) { return &HD(r).type; }, SchemaPresence::NonEmpty, SchemaReference::None,
           id_choices()),
      text("text", 1024, [](void *r) { return &HD(r).text; }, SchemaPresence::Always, SchemaReference::Dynamic),
  };
  return entries;
}

// A BODY's draw kind is the first authored of CUSTOM_DRAW / BITMAP_DRAW / BITMAP_TEXT
// (the writer puts it first); the kind and the three flags stay in step: a kind sets
// its flag, a flag set with no kind becomes the kind, and a flag cleared under the kind
// hands it to the next one still set.
TableBody &BD(void *r) { return *static_cast<TableBody *>(r); }
const char *const kDrawKinds[] = {"CUSTOM_DRAW", "BITMAP_DRAW", "BITMAP_TEXT"};
bool *draw_flag(TableBody &b, int k) { return k == 0 ? &b.custom_draw : k == 1 ? &b.bitmap_draw : &b.bitmap_text; }
void draw_kind_after_clear(TableBody &b) {
  for (int k = 0; k < 3; ++k)
    if (iequals(b.display, kDrawKinds[k]) && *draw_flag(b, k)) return;
  b.display.clear();
  for (int k = 0; k < 3; ++k)
    if (*draw_flag(b, k)) { b.display = kDrawKinds[k]; return; }
}
bool display_set(void *r, const SchemaValue &value, std::string &error) {
  TableBody &b = BD(r);
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
template <int K>
bool draw_flag_set(void *r, const SchemaValue &value, std::string &) {
  TableBody &b = BD(r);
  *draw_flag(b, K) = std::get<int64_t>(value) != 0;
  if (*draw_flag(b, K) && b.display.empty()) b.display = kDrawKinds[K];
  else draw_kind_after_clear(b);
  return true;
}
template <int K>
Entry draw_flag_entry(const char *path) {
  Entry e = flag(path, [](void *r) { return draw_flag(BD(r), K); });
  e.custom_set = &draw_flag_set<K>;
  return e;
}
const std::vector<Entry> &body_entries() {
  static const std::vector<Entry> entries = [] {
    std::vector<Entry> out;
    out.push_back(text("justify", 64, [](void *r) { return &BD(r).justify; }, SchemaPresence::NonEmpty,
                       SchemaReference::None, justify_choices()));
    out.push_back(text("vjustify", 64, [](void *r) { return &BD(r).vjustify; }, SchemaPresence::NonEmpty,
                       SchemaReference::None, vjustify_choices()));
    out.push_back(number("column", [](void *r) { return &BD(r).column; }, [](void *r) { return &BD(r).has_column; }));
    Entry display = text("display", 16, [](void *r) { return &BD(r).display; }, SchemaPresence::NonEmpty,
                         SchemaReference::None,
                         {{"", 0}, {"CUSTOM_DRAW", 1}, {"BITMAP_DRAW", 2}, {"BITMAP_TEXT", 3}});
    display.custom_set = &display_set;
    out.push_back(display);
    out.push_back(draw_flag_entry<0>("custom_draw"));
    out.push_back(draw_flag_entry<1>("bitmap_draw"));
    out.push_back(draw_flag_entry<2>("bitmap_text"));
    out.push_back(text("bitmap_flags", 64, [](void *r) { return &BD(r).bitmap_flags; }));
    out.push_back(flag("scale_bitmap", [](void *r) { return &BD(r).scale_bitmap; }));
    return out;
  }();
  return entries;
}

TableSubst &SU(void *r) { return *static_cast<TableSubst *>(r); }
const std::vector<Entry> &subst_entries() {
  static const std::vector<Entry> entries = {
      number("column", [](void *r) { return &SU(r).column; }, [](void *r) { return &SU(r).has_column; }),
      text("value", 64, [](void *r) { return &SU(r).value; }),
      flag("is_url", [](void *r) { return &SU(r).is_url; }),
      flag("is_file", [](void *r) { return &SU(r).is_file; }),
      text("file", 128, [](void *r) { return &SU(r).file; }, SchemaPresence::Always, SchemaReference::Dynamic),
  };
  return entries;
}

// A tag or an attribute name the writer can put in an element: not empty, no whitespace
// and none of the characters that end a name or open a value.
bool plain_name(const std::string &name) {
  if (name.empty()) return false;
  for (const char c : name)
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '<' || c == '>' || c == '/' || c == '"' ||
        c == '\'' || c == '=' || c == '&')
      return false;
  return true;
}
Element &EL(void *r) { return *static_cast<Element *>(r); }
// The tag as the reader keeps it: upper case (read_extra), so a save reads back as written.
bool tag_set(void *r, const SchemaValue &value, std::string &error) {
  const std::string &tag = std::get<std::string>(value);
  if (!plain_name(tag)) {
    error = "A tag is a plain name: no spaces and none of < > / \" ' = &.";
    return false;
  }
  EL(r).tag = strutil::to_upper(tag);
  return true;
}
const std::vector<Entry> &element_entries() {
  static const std::vector<Entry> entries = [] {
    std::vector<Entry> out;
    Entry tag = text("tag", 64, [](void *r) { return &EL(r).tag; }, SchemaPresence::Always);
    tag.custom_set = &tag_set;
    out.push_back(tag);
    out.push_back(text("text", 1024, [](void *r) { return &EL(r).text; }, SchemaPresence::Always));
    return out;
  }();
  return entries;
}

ElementAttribute &AT(void *r) { return *static_cast<ElementAttribute *>(r); }
bool attribute_name_set(void *r, const SchemaValue &value, std::string &error) {
  const std::string &name = std::get<std::string>(value);
  if (!plain_name(name)) {
    error = "An attribute name is a plain name: no spaces and none of < > / \" ' = &.";
    return false;
  }
  AT(r).name = name;
  return true;
}
const std::vector<Entry> &attribute_entries() {
  static const std::vector<Entry> entries = [] {
    std::vector<Entry> out;
    Entry name = text("name", 64, [](void *r) { return &AT(r).name; }, SchemaPresence::Always);
    name.custom_set = &attribute_name_set;
    out.push_back(name);
    Entry value = text("value", 128, [](void *r) { return &AT(r).value; }, SchemaPresence::Bit);
    value.bit = [](void *r) { return &AT(r).has_value; };
    out.push_back(value);
    return out;
  }();
  return entries;
}

const std::vector<Entry> &entries_of(SchemaShape shape) {
  static const std::vector<Entry> none;
  switch (shape) {
  case SchemaShape::Screen: return screen_entries();
  case SchemaShape::Window: return window_entries();
  case SchemaShape::Part: return part_entries();
  case SchemaShape::Appearance: return appearance_entries();
  case SchemaShape::Sound: return sound_entries();
  case SchemaShape::Action: return action_entries();
  case SchemaShape::Hotkey: return hotkey_entries();
  case SchemaShape::Datasource: return datasource_entries();
  case SchemaShape::Item: return item_entries();
  case SchemaShape::Row: return none;
  case SchemaShape::Header: return header_entries();
  case SchemaShape::Body: return body_entries();
  case SchemaShape::Subst: return subst_entries();
  case SchemaShape::Element: return element_entries();
  case SchemaShape::Attribute: return attribute_entries();
  }
  return none;
}

const Entry *entry_of(SchemaShape shape, const std::string &path) {
  for (const Entry &e : entries_of(shape))
    if (path == e.field.path) return &e;
  return nullptr;
}

bool entry_get(const Entry &e, void *record, SchemaValue &out) {
  if (e.custom_get) return e.custom_get(record, out);
  if (e.text) { out = *e.text(record); return true; }
  if (e.number) { out = int64_t(*e.number(record)); return true; }
  if (e.flag) { out = int64_t(*e.flag(record) ? 1 : 0); return true; }
  if (e.bit) { out = int64_t(*e.bit(record) ? 1 : 0); return true; }
  return false;
}

bool entry_own_present(const Entry &e, void *record) {
  if (e.custom_present) return e.custom_present(record);
  switch (e.field.presence) {
  case SchemaPresence::Always: return true;
  case SchemaPresence::Bit:
  case SchemaPresence::Block: return e.bit && *e.bit(record);
  case SchemaPresence::NonEmpty: return e.text ? !e.text(record)->empty() : true;
  }
  return true;
}

// The enclosing block's field authored (a STRING, an ITEMS, a part).
void author_block(SchemaShape shape, void *record, const char *block) {
  if (!*block) return;
  const Entry *e = entry_of(shape, block);
  if (!e) return;
  std::string ignored;
  if (e->custom_set) {
    if (!e->custom_present || !e->custom_present(record)) e->custom_set(record, SchemaValue(int64_t(1)), ignored);
  } else if (e->bit) {
    *e->bit(record) = true;
  }
}

// --- the lists -----------------------------------------------------------------------

const std::vector<SchemaList> &screen_lists() {
  static const std::vector<SchemaList> lists = {{"window", "Windows", "Window", SchemaShape::Window, "", 0, "name"}};
  return lists;
}

// In the order the writer emits them (write_window), the child windows last, so a
// pre-order walk of the records meets them in file order.
const std::vector<SchemaList> &window_lists() {
  static const std::vector<SchemaList> lists = {
      {"attribute", "Attributes", "Attribute", SchemaShape::Attribute, "", 0, "name"},
      {"hotkey", "Hotkeys", "Hotkey", SchemaShape::Hotkey, "", 0, ""},
      {"action", "Actions", "Action", SchemaShape::Action, "", 0, ""},
      {"appearance", "Appearances", "Appearance", SchemaShape::Appearance, "", 0, ""},
      {"shuttle", "Shuttle", "Shuttle", SchemaShape::Appearance, "", 0, ""},
      {"scrollup", "Scroll up", "Scroll up", SchemaShape::Appearance, "", 0, ""},
      {"scrolldown", "Scroll down", "Scroll down", SchemaShape::Appearance, "", 0, ""},
      {"datasource", "Data sources", "Data source", SchemaShape::Datasource, "", 0, ""},
      {"sound", "Sounds", "Sound", SchemaShape::Sound, "", 0, ""},
      {"items.appearance", "Item appearances", "Item appearance", SchemaShape::Appearance, "items", 0, ""},
      {"items.item", "Items", "Item", SchemaShape::Item, "items", 0, ""},
      {"items.row", "Rows", "Row", SchemaShape::Row, "items", 0, ""},
      {"list_box", "List box", "List box", SchemaShape::Part, "", 1, "name"},
      {"spinup", "Spin up", "Spin up", SchemaShape::Part, "", 1, "name"},
      {"spindown", "Spin down", "Spin down", SchemaShape::Part, "", 1, "name"},
      {"column.header", "Headers", "Header", SchemaShape::Header, "", 0, ""},
      {"column.body", "Bodies", "Body", SchemaShape::Body, "", 0, ""},
      {"column.subst", "Substitutions", "Substitution", SchemaShape::Subst, "", 0, ""},
      {"scrollbar", "Scrollbar", "Scrollbar", SchemaShape::Part, "", 1, "name"},
      {"element", "Elements", "Element", SchemaShape::Element, "", 0, "tag"},
      {"window", "Windows", "Window", SchemaShape::Window, "", 0, "name"},
  };
  return lists;
}
enum WindowList : size_t {
  kAttributes, kHotkeys, kActions, kAppearances, kShuttle, kScrollUp, kScrollDown, kDatasources, kSounds,
  kItemAppearances, kItemItems, kItemRows, kListBox, kSpinUp, kSpinDown, kHeaders, kBodies, kSubsts, kScrollbar,
  kExtras, kChildren, kWindowListCount,
};

const std::vector<SchemaList> &row_lists() {
  static const std::vector<SchemaList> lists = {{"item", "Cells", "Cell", SchemaShape::Item, "", 0, ""}};
  return lists;
}

const std::vector<SchemaList> &element_lists() {
  static const std::vector<SchemaList> lists = {
      {"attribute", "Attributes", "Attribute", SchemaShape::Attribute, "", 0, "name"},
      {"element", "Elements", "Element", SchemaShape::Element, "", 0, "tag"},
  };
  return lists;
}

// Hands `fn` the container behind one list: a std::vector of the list's records, or the
// WindowPart of a part list (with the part's type). False for a list the owner does not hold.
struct PartList {
  WindowPart *part;
  WindowType type;
};
template <class Fn> bool with_list(const SchemaRecord &owner, size_t list, Fn &&fn) {
  if (!owner) return false;
  switch (owner.shape) {
  case SchemaShape::Screen:
    if (list != 0) return false;
    fn(static_cast<Screen *>(owner.data)->roots);
    return true;
  case SchemaShape::Window:
  case SchemaShape::Part: {
    Window &w = W(owner.data);
    switch (list) {
    case kAttributes: fn(w.extra_attributes); return true;
    case kHotkeys: fn(w.hotkeys); return true;
    case kActions: fn(w.actions); return true;
    case kAppearances: fn(w.appearances); return true;
    case kShuttle: fn(w.shuttle); return true;
    case kScrollUp: fn(w.scrollup); return true;
    case kScrollDown: fn(w.scrolldown); return true;
    case kDatasources: fn(w.datasources); return true;
    case kSounds: fn(w.sounds); return true;
    case kItemAppearances: fn(w.items.appearances); return true;
    case kItemItems: fn(w.items.items); return true;
    case kItemRows: fn(w.items.rows); return true;
    case kListBox: { PartList p{&w.list_box, WindowType::List}; fn(p); return true; }
    case kSpinUp: { PartList p{&w.spinup, WindowType::Button}; fn(p); return true; }
    case kSpinDown: { PartList p{&w.spindown, WindowType::Button}; fn(p); return true; }
    case kHeaders: fn(w.table_data.column.headers); return true;
    case kBodies: fn(w.table_data.column.bodies); return true;
    case kSubsts: fn(w.table_data.column.substitutions); return true;
    case kScrollbar: { PartList p{&w.scrollbar, WindowType::Scroll}; fn(p); return true; }
    case kExtras: fn(w.extras); return true;
    case kChildren: fn(w.children); return true;
    default: return false;
    }
  }
  case SchemaShape::Row:
    if (list != 0) return false;
    fn(static_cast<TableRow *>(owner.data)->cells);
    return true;
  case SchemaShape::Element:
    if (list == 0) { fn(static_cast<Element *>(owner.data)->attributes); return true; }
    if (list == 1) { fn(static_cast<Element *>(owner.data)->children); return true; }
    return false;
  default: return false;
  }
}

template <class T> struct is_part : std::is_same<T, PartList> {};

// The defaults a new record takes (see schema_list_insert).
void make_default(Window &w) {
  w.type = WindowType::Static;
  w.position = {0, 0, 100, 20, true, true, true, true};
  Appearance appearance;
  appearance.state = "default";
  w.appearances.push_back(appearance);
}
void make_default(Appearance &a) { a.state = "default"; }
void make_default(Sound &s) {
  s.state = "mousein";
  s.trigger = "MOUSE_OVER";
}
void make_default(Action &a) { a.type = "POP_SCREEN"; }
void make_default(Element &e) { e.tag = "TARGET"; }
template <class T> void make_default(T &) {}

template <class T>
bool vector_insert(std::vector<T> &list, const SchemaRecord &owner, size_t index, const SchemaDetached *record) {
  index = std::min(index, list.size());
  if (record) {
    list.insert(list.begin() + static_cast<std::ptrdiff_t>(index), *static_cast<const T *>(record->data.get()));
    return true;
  }
  T value{};
  make_default(value);
  if constexpr (std::is_same<T, TableHeader>::value || std::is_same<T, TableBody>::value) {
    value.has_column = true;
    value.column = int(index);
  } else if constexpr (std::is_same<T, TableSubst>::value) {
    value.has_column = true;
  } else if constexpr (std::is_same<T, ElementAttribute>::value) {
    // A window keeps only PLAYERLIST and SERVERLIST (GLB_TABLE); an element takes any name.
    value.name = owner.shape == SchemaShape::Element ? "NAME" : "PLAYERLIST";
  }
  list.insert(list.begin() + static_cast<std::ptrdiff_t>(index), std::move(value));
  return true;
}

}  // namespace

// --- public API --------------------------------------------------------------------

const std::vector<SchemaChoice> &ui_material_flag_choices() {
  // g_UIMaterialFlagNames @ 0x84a5d0, in table order (rows of wchar name[64] + DWORD flags).
  static const std::vector<SchemaChoice> choices = {
      {"STANDARD", 0x300600}, {"STANDARD_TRANSPARENT", 0x300651}, {"AFUNC_BITMASK", 0xF},
      {"ASRC_BITMASK", 0xF0}, {"COLOR_BITMASK", 0xF00}, {"AFUNC_NONE", 0x0}, {"AFUNC_BLEND", 0x1},
      {"AFUNC_SRCCOPY", 0x0}, {"AFUNC_ADD", 0x2}, {"AFUNC_SHADE", 0x3}, {"AFUNC_GLOW", 0x4},
      {"AFUNC_ALPHALIGHT", 0x5}, {"AFUNC_MULTIPLY", 0x6}, {"AFUNC_DEST", 0x7}, {"AFUNC_MULTIPLYDBL", 0x8},
      {"AFUNC_SQR", 0x9}, {"AFUNC_SQRADD", 0xA}, {"ASRC_NONE", 0x0}, {"ASRC_CONSTANT", 0x10},
      {"ASRC_ITERATED", 0x20}, {"ASRC_TEXTURE", 0x30}, {"ASRC_TEXTURExCONSTANT", 0x40},
      {"ASRC_TEXTURExITERATED", 0x50}, {"COLOR_NONE", 0x0}, {"COLOR_CONSTANT", 0x100},
      {"COLOR_ITERATED", 0x200}, {"COLOR_ITERATEDxCONSTANT", 0x300}, {"COLOR_TEXTURE", 0x400},
      {"COLOR_TEXTURExCONSTANT", 0x500}, {"COLOR_TEXTURExITERATED", 0x600}, {"COLOR_TEXTURERAMP", 0x700},
      {"COLOR_TEXTUREdpCONSTANT", 0x800}, {"COLOR_TEXTUREdpITERATED", 0x900}, {"COLOR2_SQRD", 0x1000},
      {"APPLYSPECULAR", 0x10000}, {"APPLYFOG", 0x20000}, {"ALPHATEST", 0x40000}, {"NATIVE2X", 0x80000},
      {"NOWRITEDEPTH", 0x100000}, {"NOCHECKDEPTH", 0x200000}, {"GMAT_OVR_CLAMPUV1", 0x1000000},
      {"GMAT_OVR_CLAMPUV2", 0x2000000}, {"TINTASTEXT", 0x80000000LL},
  };
  return choices;
}

const std::vector<SchemaField> &schema_fields(SchemaShape shape) {
  static const std::vector<std::vector<SchemaField>> tables = [] {
    std::vector<std::vector<SchemaField>> out;
    for (int s = 0; s <= int(SchemaShape::Attribute); ++s) {
      std::vector<SchemaField> fields;
      for (const Entry &e : entries_of(SchemaShape(s))) fields.push_back(e.field);
      out.push_back(std::move(fields));
    }
    return out;
  }();
  return tables[size_t(shape)];
}

const SchemaField *schema_field(SchemaShape shape, const std::string &path) {
  const Entry *e = entry_of(shape, path);
  return e ? &e->field : nullptr;
}

const std::vector<SchemaList> &schema_lists(SchemaShape owner) {
  static const std::vector<SchemaList> none;
  switch (owner) {
  case SchemaShape::Screen: return screen_lists();
  case SchemaShape::Window:
  case SchemaShape::Part: return window_lists();
  case SchemaShape::Row: return row_lists();
  case SchemaShape::Element: return element_lists();
  default: return none;
  }
}

SchemaRecord schema_screen(Screen &screen) { return {SchemaShape::Screen, &screen}; }
SchemaRecord schema_window(Window &window) { return {SchemaShape::Window, &window}; }

bool schema_get(const SchemaRecord &record, const std::string &path, SchemaValue &out) {
  const Entry *e = record ? entry_of(record.shape, path) : nullptr;
  return e && entry_get(*e, record.data, out);
}

bool schema_present(const SchemaRecord &record, const std::string &path) {
  const Entry *e = record ? entry_of(record.shape, path) : nullptr;
  if (!e) return false;
  // A block's toggle always reads: its value is whether the block is written.
  if (e->field.presence == SchemaPresence::Block) return true;
  if (!entry_own_present(*e, record.data)) return false;
  if (!*e->field.block) return true;
  const Entry *block = entry_of(record.shape, e->field.block);
  return !block || entry_own_present(*block, record.data);
}

bool schema_set(const SchemaRecord &record, const std::string &path, const SchemaValue &value, std::string &error) {
  const Entry *e = record ? entry_of(record.shape, path) : nullptr;
  if (!e) { error = "Unknown field."; return false; }
  const SchemaField &f = e->field;
  if (f.type == SchemaType::Text) {
    const auto *text = std::get_if<std::string>(&value);
    if (!text) { error = "This field takes text."; return false; }
    if (text->size() >= f.width) { error = "The text is too long."; return false; }
  } else {
    const auto *number = std::get_if<int64_t>(&value);
    if (!number) { error = f.type == SchemaType::Flag ? "A flag is yes (1) or no (0)." : "This field takes a whole number."; return false; }
    if (f.type == SchemaType::Integer && (*number < INT_MIN || *number > INT_MAX)) { error = "The number is out of range."; return false; }
  }
  const SchemaValue normalized =
      f.type == SchemaType::Flag ? SchemaValue(int64_t(std::get<int64_t>(value) != 0 ? 1 : 0)) : value;
  SchemaValue current;
  if (entry_get(*e, record.data, current) && current == normalized) return true; // nothing changes
  if (e->custom_set) {
    if (!e->custom_set(record.data, normalized, error)) return false;
  } else if (e->text) {
    *e->text(record.data) = std::get<std::string>(normalized);
  } else if (e->number) {
    *e->number(record.data) = int(std::get<int64_t>(normalized));
  } else if (e->flag) {
    *e->flag(record.data) = std::get<int64_t>(normalized) != 0;
  }
  if (e->bit) *e->bit(record.data) = f.presence == SchemaPresence::Block ? std::get<int64_t>(normalized) != 0 : true;
  author_block(record.shape, record.data, f.block);
  return true;
}

bool schema_set_present(const SchemaRecord &record, const std::string &path, bool present, std::string &error) {
  const Entry *e = record ? entry_of(record.shape, path) : nullptr;
  if (!e) { error = "Unknown field."; return false; }
  if (e->field.presence != SchemaPresence::Bit || !e->bit) { error = "This field is always written."; return false; }
  *e->bit(record.data) = present;
  if (present) author_block(record.shape, record.data, e->field.block);
  return true;
}

SchemaReference schema_reference(const SchemaRecord &record, const std::string &path) {
  const Entry *e = record ? entry_of(record.shape, path) : nullptr;
  if (!e) return SchemaReference::None;
  if (e->field.reference != SchemaReference::Dynamic) return e->field.reference;
  auto is = [](const std::string &token, const char *what) { return iequals(token, what); };
  switch (record.shape) {
  case SchemaShape::Appearance: {
    const Appearance &a = AP(record.data);
    if (is(a.type, "IMAGE") || is(a.type, "IMAGEROW")) return SchemaReference::MenuTexture;
    if (is(a.type, "COLOR") || is(a.type, "OUTLINE")) return SchemaReference::StyleVar;
    return SchemaReference::None;
  }
  case SchemaShape::Item: {
    const Item &i = IT(record.data);
    if (is(i.type, "ID")) return SchemaReference::TextId;
    if (is(i.type, "IMAGE") || is(i.type, "BITMAP")) return SchemaReference::MenuTexture;
    if (is(i.type, "COLOR")) return SchemaReference::StyleVar;
    return SchemaReference::None;
  }
  case SchemaShape::Header: return is(HD(record.data).type, "ID") ? SchemaReference::TextId : SchemaReference::None;
  case SchemaShape::Action: {
    // The verb decides what the text and the slot name [orig: CUIWidget_HandleScriptedAction
    // @ 0x6497f0]: SCREEN selects the screen of that name in the file it loads (@ 0x649894,
    // CUIScene_SelectNodeByName @ 0x63b6b0); WINDOW finds the window of that name on the
    // acting window's own screen (@ 0x6498c8, UI_FindScreenControl @ 0x63ae80); TAB moves the
    // focus to the control of that name (@ 0x649c30) and GLB_FILTER / GLB_FILTER_NUM send to
    // it [orig: CEditWnd_HandleInputEvent @ 0x661510], each on the screen showing, the
    // acting window's while its keys reach it; URL reads the text of the control the slot
    // names (@ 0x649a1c). Every other verb's text is no name the file defines.
    const Action &a = AC(record.data);
    if (path == "target") {
      if (is(a.type, "SCREEN")) return SchemaReference::Screen;
      if (is(a.type, "WINDOW") || is(a.type, "TAB") || is(a.type, "GLB_FILTER") || is(a.type, "GLB_FILTER_NUM"))
        return SchemaReference::Window;
      return SchemaReference::None;
    }
    if (path == "field") return is(a.type, "URL") ? SchemaReference::Window : SchemaReference::None;
    return SchemaReference::None;
  }
  case SchemaShape::Subst: {
    const TableSubst &s = SU(record.data);
    return s.is_file && !s.is_url ? SchemaReference::MenuTexture : SchemaReference::None;
  }
  case SchemaShape::Window:
  case SchemaShape::Part: {
    const Window &w = W(record.data);
    if (path == "string.value") return is(w.string_data.type, "ID") ? SchemaReference::TextId : SchemaReference::None;
    if (path == "toggle_string.value")
      return is(w.toggle_string.type, "ID") ? SchemaReference::TextId : SchemaReference::None;
    return SchemaReference::None;
  }
  default: return SchemaReference::None;
  }
}

size_t schema_list_size(const SchemaRecord &owner, size_t list) {
  size_t size = 0;
  with_list(owner, list, [&](auto &container) {
    using C = std::decay_t<decltype(container)>;
    if constexpr (is_part<C>::value) size = container.part->latent() ? 1 : 0;
    else size = container.size();
  });
  return size;
}

SchemaRecord schema_list_at(const SchemaRecord &owner, size_t list, size_t index) {
  const std::vector<SchemaList> &lists = schema_lists(owner.shape);
  if (list >= lists.size()) return {};
  SchemaRecord out;
  with_list(owner, list, [&](auto &container) {
    using C = std::decay_t<decltype(container)>;
    if constexpr (is_part<C>::value) {
      if (index == 0 && container.part->latent()) out = {SchemaShape::Part, container.part->latent()};
    } else {
      if (index < container.size()) out = {lists[list].shape, &container[index]};
    }
  });
  return out;
}

bool schema_list_present(const SchemaRecord &owner, size_t list) {
  if (!owner || (owner.shape != SchemaShape::Window && owner.shape != SchemaShape::Part)) return true;
  const Window &w = W(owner.data);
  switch (list) {
  case kItemAppearances:
  case kItemItems:
  case kItemRows: return w.items.present;
  case kListBox: return w.list_box.present();
  case kSpinUp: return w.spinup.present();
  case kSpinDown: return w.spindown.present();
  case kScrollbar: return w.scrollbar.present();
  default: return true;
  }
}

void schema_author_list(const SchemaRecord &owner, size_t list) {
  if (!owner || (owner.shape != SchemaShape::Window && owner.shape != SchemaShape::Part)) return;
  if (list == kItemAppearances || list == kItemItems || list == kItemRows) W(owner.data).items.present = true;
}

SchemaDetached schema_list_copy(const SchemaRecord &owner, size_t list, size_t index) {
  SchemaDetached out;
  const std::vector<SchemaList> &lists = schema_lists(owner.shape);
  if (list >= lists.size()) return out;
  out.shape = lists[list].shape;
  with_list(owner, list, [&](auto &container) {
    using C = std::decay_t<decltype(container)>;
    if constexpr (is_part<C>::value) {
      if (index == 0 && container.part->latent()) {
        out.data = std::make_shared<Window>(*container.part->latent());
        out.shown = container.part->present();
      }
    } else {
      using T = typename C::value_type;
      if (index < container.size()) out.data = std::make_shared<T>(container[index]);
    }
  });
  return out;
}

SchemaDetached schema_detach_window(const Window &window) {
  SchemaDetached out;
  out.shape = SchemaShape::Window;
  out.data = std::make_shared<Window>(window);
  return out;
}

bool schema_list_insert(const SchemaRecord &owner, size_t list, size_t index, const SchemaDetached *record,
                        std::string &error) {
  const std::vector<SchemaList> &lists = schema_lists(owner.shape);
  if (list >= lists.size()) { error = "This record holds no such list."; return false; }
  if (record && (!record->data || record->shape != lists[list].shape)) {
    error = std::string("The ") + lists[list].label + " take " + lists[list].record_label + " records only.";
    return false;
  }
  bool done = false;
  with_list(owner, list, [&](auto &container) {
    using C = std::decay_t<decltype(container)>;
    if constexpr (is_part<C>::value) {
      if (container.part->latent()) {
        error = std::string("A window holds one ") + lists[list].record_label + " at most.";
        return;
      }
      if (record) {
        container.part->author(container.type) = *static_cast<const Window *>(record->data.get());
        if (!record->shown) container.part->hide();
      } else {
        default_part(*container.part, container.type);
      }
      done = true;
    } else {
      done = vector_insert(container, owner, index, record);
    }
  });
  if (done) schema_author_list(owner, list);
  return done;
}

bool schema_list_erase(const SchemaRecord &owner, size_t list, size_t index) {
  bool done = false;
  with_list(owner, list, [&](auto &container) {
    using C = std::decay_t<decltype(container)>;
    if constexpr (is_part<C>::value) {
      if (index == 0 && container.part->latent()) {
        container.part->clear();
        done = true;
      }
    } else if (index < container.size()) {
      container.erase(container.begin() + static_cast<std::ptrdiff_t>(index));
      done = true;
    }
  });
  return done;
}

// --- which type reads what -----------------------------------------------------------

namespace {

using Types = uint32_t;
constexpr Types type_bit(WindowType t) { return Types(1) << unsigned(t); }
constexpr Types kAllTypes = (Types(1) << unsigned(kWindowTypeCount)) - 1;
using T = WindowType;
// The parse chains (grill set A3, docs/mnu/menu-re.md "Which type reads what"): every
// type's parse runs the base parse first [orig: CUIElement_ParseXMLDefinition @
// 0x648120]; the S-chain reads STRING [orig: CUIButtonWidget_ParseXMLAttributes @
// 0x657c30], the B-chain TOGGLE_STRING [orig: CButtonWnd_ParseTooltipXML @ 0x658170], the
// R-chain CHECKED and GROUP [orig: CRadioWnd_ParseXMLDefinition @ 0x656c40]; RADIOEDIT
// feeds its elements to an embedded edit and an embedded radio [orig:
// CRadioEditWnd_ParseXMLDefinition @ 0x65d1c0].
constexpr Types kS = type_bit(T::Static) | type_bit(T::Button) | type_bit(T::Edit) | type_bit(T::MultilineEdit) |
                     type_bit(T::Radio) | type_bit(T::CheckBox) | type_bit(T::SpinList) | type_bit(T::List) |
                     type_bit(T::LanList) | type_bit(T::Table) | type_bit(T::Combo) | type_bit(T::RadioEdit);
constexpr Types kB = type_bit(T::Button) | type_bit(T::Radio) | type_bit(T::CheckBox) | type_bit(T::SpinList) |
                     type_bit(T::List) | type_bit(T::LanList) | type_bit(T::Table) | type_bit(T::RadioEdit);
constexpr Types kR = type_bit(T::Radio) | type_bit(T::List) | type_bit(T::LanList) | type_bit(T::Table) |
                     type_bit(T::RadioEdit);
constexpr Types kEditAttributes = type_bit(T::Edit) | type_bit(T::MultilineEdit) | type_bit(T::RadioEdit);
constexpr Types kLists = type_bit(T::List) | type_bit(T::LanList);

struct ReadRule {
  const char *path;
  Types types;
};
// A path with no rule (nor a rule for a path it extends) is the base parse's: every type.
const ReadRule kReadRules[] = {
    {"string", kS},
    {"toggle_string", kB},
    {"checked", kR | type_bit(T::CheckBox)},  // [orig: @ 0x656c62, CCheckWnd_ParseXMLDefinition @ 0x64adce]
    {"group", kR},                           // the element [orig: @ 0x656c87]
    {"as_button", type_bit(T::CheckBox)},    // [orig: @ 0x64adb2]
    // [orig: CEditWnd_ParseXMLProperties @ 0x661d10]
    {"password", kEditAttributes}, {"number", kEditAttributes}, {"maxchar", kEditAttributes},
    {"readonly", kEditAttributes}, {"maxval", kEditAttributes}, {"minval", kEditAttributes},
    {"datasource", type_bit(T::Marquee)},    // [orig: CMarqueeWnd_ParseXMLDefinition @ 0x65cee5]
    // [orig: CUIScrollWidget_ParseExtendedXMLDef @ 0x64c6d0]
    {"orientation", type_bit(T::Scroll)}, {"scroll_extent", type_bit(T::Scroll)},
    {"scroll_extent_width", type_bit(T::Scroll)}, {"shuttle", type_bit(T::Scroll)},
    {"scrollup", type_bit(T::Scroll)}, {"scrolldown", type_bit(T::Scroll)},
    // The ITEMS shapes: the LIST form [orig: CListWnd_ParseXMLDefinition @ 0x6457b6: JUSTIFY,
    // VJUSTIFY, MULTISELECT; ITEM and APPEARANCE rows; ITEM JUSTIFY, VJUSTIFY, PAIRS_LIST,
    // TYPE, VALUE], the SPINLIST form [orig: CUISpinList_ParseXMLDefinition @ 0x64bd57:
    // JUSTIFY, VJUSTIFY; ITEM rows only, TYPE ID / IMAGE / COLOR], the TABLE form [orig:
    // CTableWnd_ParseXMLContentDefinition @ 0x642816: MULTISELECT; ROW > ITEM cells with
    // COLUMN, JUSTIFY, VJUSTIFY, VALUE, TYPE; APPEARANCE rows]. COMBOBOX reads ITEMS only in
    // its LIST_BOX, GLB_TABLE only inside its own elements.
    {"items", kLists | type_bit(T::SpinList) | type_bit(T::Table)},
    {"items.multiselect", kLists | type_bit(T::Table)},
    {"items.justify", kLists | type_bit(T::SpinList)},
    {"items.vjustify", kLists | type_bit(T::SpinList)},
    {"items.appearance", kLists | type_bit(T::Table)},
    {"items.item", kLists | type_bit(T::SpinList)},
    {"items.item.pairs_list", kLists},
    {"items.item.column", 0},
    {"items.row", type_bit(T::Table)},
    {"items.row.item.pairs_list", 0},
    {"list_box", type_bit(T::Combo)},        // [orig: CComboWnd_ParseXMLDefinition @ 0x65c0f2]
    {"spinup", type_bit(T::SpinList)},       // [orig: @ 0x64c2fd]
    {"spindown", type_bit(T::SpinList)},     // [orig: @ 0x64c31a]
    {"column", type_bit(T::Table)},          // [orig: @ 0x6430f6]
    // [orig: @ 0x64603d (LIST), 0x643afd (TABLE), 0x6608a4 (MULTILINE_EDIT)]
    {"scrollbar", kLists | type_bit(T::Table) | type_bit(T::MultilineEdit)},
    {"min_item_height", kLists | type_bit(T::Table)},  // [orig: @ 0x64600d, 0x643a9f]
    {"fixed_header_height", type_bit(T::Table)},       // [orig: @ 0x643ace]
    // [orig: CLanListWnd_ParseXMLDefinition @ 0x65b9d0; CLanGameBrowser_ParseExtendedXMLDefinition
    // @ 0x65dac0; CGopherWnd_ParseXMLDefinition @ 0x65b130]
    {"element", type_bit(T::LanList) | type_bit(T::GlbTable) | type_bit(T::Gopher)},
    {"attribute", type_bit(T::GlbTable)},
};

}  // namespace

SchemaApplies schema_reads(WindowType type, const std::string &path) {
  // The path, then each shorter path it extends ("items.item.value", "items.item", "items").
  size_t length = path.size();
  for (;;) {
    for (const ReadRule &rule : kReadRules)
      if (std::strlen(rule.path) == length && path.compare(0, length, rule.path) == 0)
        return (rule.types & type_bit(type)) ? SchemaApplies::Reads : SchemaApplies::Ignored;
    const size_t dot = path.rfind('.', length ? length - 1 : 0);
    if (dot == std::string::npos || dot >= length) break;
    length = dot;
  }
  return (kAllTypes & type_bit(type)) ? SchemaApplies::Reads : SchemaApplies::Ignored;
}

SchemaApplies schema_action_reads(const Action &action, const std::string &field) {
  // [orig: CUIElement_ParseXMLDefinition @ 0x648ee2 ACTION arm; CUIWidget_HandleScriptedAction
  // @ 0x6497f0]: sixteen verbs; any other token (or none) is code 0, ignored on every path.
  if (field == "type") return SchemaApplies::Reads;
  if (!known_token(action.type, kActionTypes)) return SchemaApplies::Ignored;
  auto only = [&](std::initializer_list<const char *> readers) {
    for (const char *verb : readers)
      if (iequals(action.type, verb)) return SchemaApplies::Reads;
    return SchemaApplies::Ignored;
  };
  if (field == "file") return only({"SCREEN"});                       // kept for SCREEN only
  if (field == "field" || field == "field_attr")                       // kept for these four
    return only({"URL", "GLB_FILTER", "GLB_FILTER_NUM", "GLB_JOIN"});
  if (field == "state" || field == "toggle") return only({"WINDOW"});  // the WINDOW state switch @ 0x6498f7
  if (field == "external_browser") return only({"URL"});
  // TARGET_FORM is stored for FORM_POST; where the post reads it is not witnessed
  // (UI_BuildURLAndSubmitRequest @ 0x63e3f0 takes only the scene).
  if (field == "target_form") return iequals(action.type, "FORM_POST") ? SchemaApplies::Unverified : SchemaApplies::Ignored;
  return SchemaApplies::Reads; // TEST (stored for every verb) and the target text
}

SchemaApplies schema_element_reads(WindowType type, const std::string &tag) {
  struct TagRule {
    const char *tag;
    Types types;
  };
  static const TagRule rules[] = {
      {"JOIN_BUTTON", type_bit(T::LanList)},     // [orig: @ 0x65b9e4]
      {"SEARCH_BUTTON", type_bit(T::LanList)},   // [orig: @ 0x65ba00]
      {"GLB_TABLE", type_bit(T::GlbTable)},      // [orig: @ 0x65dae8 .. 0x65e012]
      {"GLB_INFO_BUTTON", type_bit(T::GlbTable)},
      {"GLB_INFO_PLAYER_DETAILS", type_bit(T::GlbTable)},
      {"GLB_INFO_SERVER_DETAILS", type_bit(T::GlbTable)},
      {"GLB_NW_INFO", type_bit(T::GlbTable)},
      {"TARGET", type_bit(T::GlbTable) | type_bit(T::Gopher)},
      {"FILTERS", type_bit(T::Gopher)},          // [orig: @ 0x65b167 .. 0x65b2af]
      {"GOPHER_BACK", type_bit(T::Gopher)},
  };
  for (const TagRule &rule : rules)
    if (iequals(tag, rule.tag)) return (rule.types & type_bit(type)) ? SchemaApplies::Reads : SchemaApplies::Ignored;
  return SchemaApplies::Ignored;
}

SchemaApplies schema_applies_both(SchemaApplies a, SchemaApplies b) {
  if (a == SchemaApplies::Ignored || b == SchemaApplies::Ignored) return SchemaApplies::Ignored;
  if (a == SchemaApplies::Unverified || b == SchemaApplies::Unverified) return SchemaApplies::Unverified;
  return SchemaApplies::Reads;
}

}  // namespace opennova::mnu
