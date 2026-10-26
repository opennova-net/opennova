// The menu model's API: the window types, the UI material flags, the row types, the window parts
// and the by-name lookups, the hotkey strip and the model's equality. The reader is mnu_read.cpp,
// the writer mnu_write.cpp. docs/mnu/menu-re.md has the rules with their witnesses.
#include <formats/mnu/mnu.h>

#include <formats/mnu/mnu_layout.h>
#include <formats/rtxt/rtxt.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <base/io/strutil.h>

namespace opennova::mnu {

namespace {

using opennova::strutil::iequals;

// The factory's tokens, in its compare order [orig: CUIScene_CreateWidgetByType
// @ 0x64f630].
struct TypeToken {
  const char *token;
  WindowType type;
};
const TypeToken kTypeTokens[] = {
    {"STATIC", WindowType::Static},     {"BUTTON", WindowType::Button},
    {"SCROLL", WindowType::Scroll},     {"EDIT", WindowType::Edit},
    {"MULTILINE_EDIT", WindowType::MultilineEdit},
    {"RADIO", WindowType::Radio},       {"LIST", WindowType::List},
    {"SPINLIST", WindowType::SpinList}, {"CHECKBOX", WindowType::CheckBox},
    {"TABLE", WindowType::Table},       {"GLB_TABLE", WindowType::GlbTable},
    {"RADIOEDIT", WindowType::RadioEdit},
    {"MARQUEE_WND", WindowType::Marquee}, {"COMBOBOX", WindowType::Combo},
    {"LAN_LIST", WindowType::LanList},  {"GOPHER", WindowType::Gopher},
};

}  // namespace

// --- public API --------------------------------------------------------------

const std::vector<UiMaterialFlag> &ui_material_flags() {
  // g_UIMaterialFlagNames @ 0x84a5d0, in table order (rows of wchar name[64] + DWORD flags).
  static const std::vector<UiMaterialFlag> table = {
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
      {"GMAT_OVR_CLAMPUV2", 0x2000000}, {"TINTASTEXT", 0x80000000u},
  };
  return table;
}

uint32_t ui_material_flags_of(const std::string &text) {
  // [orig: UI_ParseMaterialFlagNamesW @ 0x646cc0] wcstok on " ,|+"; each token the first row of its name, without case.
  uint32_t flags = 0;
  size_t at = 0;
  while (at < text.size()) {
    const size_t start = text.find_first_not_of(" ,|+", at);
    if (start == std::string::npos) break;
    const size_t end = text.find_first_of(" ,|+", start);
    const std::string token = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
    for (const UiMaterialFlag &row : ui_material_flags())
      if (iequals(token, row.name)) {
        flags |= row.bits;
        break;
      }
    at = end == std::string::npos ? text.size() : end;
  }
  return flags;
}

std::string ui_material_flags_text(uint32_t flags) {
  const std::vector<UiMaterialFlag> &table = ui_material_flags();
  if (flags == 0) return "AFUNC_NONE";
  // The widest rows first (the most bits; the table's order among rows of as many), each one whose
  // bits are all the word's and that adds some.
  std::vector<const UiMaterialFlag *> rows;
  for (const UiMaterialFlag &row : table)
    if (row.bits != 0) rows.push_back(&row);
  std::stable_sort(rows.begin(), rows.end(), [](const UiMaterialFlag *a, const UiMaterialFlag *b) {
    auto bits = [](uint32_t v) {
      int n = 0;
      for (; v; v &= v - 1) ++n;
      return n;
    };
    return bits(a->bits) > bits(b->bits);
  });
  std::string out;
  uint32_t covered = 0;
  for (const UiMaterialFlag *row : rows) {
    if ((row->bits & ~flags) != 0 || (row->bits & ~covered) == 0) continue;
    if (!out.empty()) out += '|';
    out += row->name;
    covered |= row->bits;
  }
  return covered == flags ? out : std::string();
}

const char *const *item_types_for(WindowType type, bool cell) {
  if (cell || type == WindowType::Table) return kCellItemTypes;
  if (type == WindowType::SpinList) return kSpinListItemTypes;
  return kListItemTypes;
}

bool appearance_color_type(const std::string &type) { return iequals(type, "COLOR") || iequals(type, "OUTLINE"); }

bool item_color_type(const std::string &type) { return iequals(type, "COLOR"); }

void set_appearance_type(Appearance &row, const std::string &type) {
  const bool was = appearance_color_type(row.type), is = appearance_color_type(type);
  if (was && !is) {
    row.value = color_word(row.color);
    row.color = Color();
  } else if (!was && is) {
    row.color = color_of(row.value);
    row.value.clear();
  }
  row.type = type;
}

void set_item_type(Item &item, const std::string &type) {
  const bool was = item_color_type(item.type), is = item_color_type(type);
  if (was && !is) {
    item.text = color_word(item.color);
    item.color = Color();
  } else if (!was && is) {
    item.color = color_of(item.text);
    item.text.clear();
  }
  item.type = type;
}

bool known_token(const std::string &token, const char *const *tokens) {
  for (size_t i = 0; tokens[i]; ++i)
    if (iequals(token, tokens[i])) return true;
  return false;
}

WindowType parse_window_type(const std::string &type_str) {
  for (const TypeToken &t : kTypeTokens)
    if (iequals(type_str, t.token)) return t.type;
  return WindowType::Window;
}

const char *window_type_name(WindowType type) {
  switch (type) {
    case WindowType::Window: return "window";
    case WindowType::Static: return "static";
    case WindowType::Button: return "button";
    case WindowType::Edit: return "edit";
    case WindowType::MultilineEdit: return "multiline_edit";
    case WindowType::List: return "list";
    case WindowType::CheckBox: return "checkbox";
    case WindowType::Radio: return "radio";
    case WindowType::Combo: return "combobox";
    case WindowType::Scroll: return "scroll";
    case WindowType::Table: return "table";
    case WindowType::SpinList: return "spinlist";
    case WindowType::Marquee: return "marquee_wnd";
    case WindowType::GlbTable: return "glb_table";
    case WindowType::RadioEdit: return "radioedit";
    case WindowType::LanList: return "lan_list";
    case WindowType::Gopher: return "gopher";
  }
  return "window";
}

WindowPart::WindowPart() = default;
WindowPart::WindowPart(const WindowPart &other)
    : window_(other.window_ ? std::make_unique<Window>(*other.window_) : nullptr), shown_(other.shown_) {}
WindowPart::WindowPart(WindowPart &&other) noexcept = default;
WindowPart &WindowPart::operator=(const WindowPart &other) {
  if (this != &other) {
    window_ = other.window_ ? std::make_unique<Window>(*other.window_) : nullptr;
    shown_ = other.shown_;
  }
  return *this;
}
WindowPart &WindowPart::operator=(WindowPart &&other) noexcept = default;
WindowPart::~WindowPart() = default;

Window &WindowPart::author(WindowType type) {
  if (!window_) {
    window_ = std::make_unique<Window>();
    window_->type = type;
  }
  shown_ = true;
  return *window_;
}

void WindowPart::clear() {
  window_.reset();
  shown_ = false;
}

std::vector<TableHeaderSetup> table_header_setup(const TableColumn &column) {
  std::vector<TableHeaderSetup> out;
  out.reserve(column.headers.size());
  const int columns = column.has_count && column.count >= 1 ? column.count : 1;
  // [orig: CTableWnd_ParseXMLContentDefinition @ 0x6427d0] index 0, width 100 and the
  // text compare at the COLUMN element; SORT's first character: '1' numeric, 'A' or 'a'
  // text, anything else keeps the carried value.
  TableHeaderSetup carried;
  carried.column = 0;
  for (const TableHeader &h : column.headers) {
    if (h.has_column) carried.column = h.column;
    if (h.has_width) carried.width = h.width;
    if (!h.sort.empty()) {
      if (h.sort[0] == '1') carried.numeric_sort = true;
      else if (h.sort[0] == 'A' || h.sort[0] == 'a') carried.numeric_sort = false;
    }
    carried.set_up = carried.column >= 0 && carried.column < columns;
    out.push_back(carried);
  }
  return out;
}

const Screen *Document::find_screen(const std::string &name) const {
  for (auto it = screens.rbegin(); it != screens.rend(); ++it)
    if (iequals(it->name, name)) return &*it;
  return nullptr;
}

const Screen *Document::first_screen() const {
  return screens.empty() ? nullptr : &screens[0];
}

std::vector<PartWindow> part_windows(const Window &window) {
  std::vector<PartWindow> out;
  const auto add = [&out](const WindowPart &part, const char *name) {
    if (part.present()) out.push_back({part.get(), name});
  };
  switch (window.type) {
  case WindowType::Combo: add(window.list_box, "LISTBOX_WND"); break;
  case WindowType::SpinList:
    add(window.spinup, "SPINLISTWND_UP");
    add(window.spindown, "SPINLISTWND_DOWN");
    break;
  case WindowType::List: add(window.scrollbar, "LISTWND_SCROLL"); break;
  case WindowType::Table: add(window.scrollbar, "TABLEWND_SCROLL"); break;
  case WindowType::MultilineEdit: add(window.scrollbar, "MEDITWND_SCROLL"); break;
  default: break;
  }
  return out;
}

namespace {

// The walk under a window whose NAME, as the search compares it, is `own`: the part's fixed one
// for a part [orig: CWnd_FindChildByName @ 0x646850]. A window of no NAME ends the walk under it; each
// window, then its child windows' walks, then its parts'. An explicit stack, never a recursion a level
// (a menu nests its windows as deep as the game's stack takes them).
const Window *find_window_named(const Window &window, const std::string &own, const std::string &name) {
  if (name.empty()) return nullptr;
  std::vector<std::pair<const Window *, std::string_view>> walk{{&window, own}};
  while (!walk.empty()) {
    const auto [w, own_name] = walk.back();
    walk.pop_back();
    if (own_name.empty()) continue;
    if (iequals(name, own_name)) return w;
    const std::vector<PartWindow> parts = part_windows(*w);
    for (size_t i = parts.size(); i-- > 0;) walk.push_back({parts[i].window, parts[i].name});
    for (size_t i = w->children.size(); i-- > 0;) walk.push_back({&w->children[i], w->children[i].name});
  }
  return nullptr;
}

} // namespace

const Window *find_window(const Window &window, const std::string &name) {
  return find_window_named(window, window.name, name);
}

const Window *find_window(const Screen &screen, const std::string &name) {
  for (const Window &root : screen.roots)
    if (const Window *found = find_window(root, name)) return found;
  return nullptr;
}

std::string strip_hotkey_marker(const std::string &text,
                                std::string *out_hotkey,
                                int *out_hotkey_pos) {
  if (out_hotkey) out_hotkey->clear();
  if (out_hotkey_pos) *out_hotkey_pos = -1;

  // CButtonWnd_SetLabel removes only the FIRST marker via strstr — CASE
  // SENSITIVE, so "{HOT}" stays literal — records its byte offset, and
  // registers the byte that follows it; later markers remain literal. The
  // shared strip is rtxt's (docs/interface/rtxt-strings-re.md)
  // [orig: CButtonWnd_SetLabel @ 0x6572F0 — strstr @0x657451].
  int marker = -1;
  std::string result = rtxt::strip_hotkey(text, marker);
  if (marker < 0) return result;
  if (out_hotkey_pos) *out_hotkey_pos = marker;
  if (out_hotkey && static_cast<size_t>(marker) < result.size()) {
    *out_hotkey = std::string(1, result[static_cast<size_t>(marker)]);
  }
  return result;
}

// --- the model's equality ---------------------------------------------------------------------

namespace {

// Field-by-field equality over the model; `where` gathers the path to the first difference.
class Same {
public:
  explicit Same(std::string *where) : where_(where) {}

  bool document(const Document &a, const Document &b) {
    if (!same(a.source_encoding == b.source_encoding, "source_encoding")) return false;
    return list(a.screens, b.screens, "screen", [this](const Screen &x, const Screen &y) { return screen(x, y); });
  }

private:
  std::string *where_;
  std::vector<std::string> path_;

  bool same(bool equal, const std::string &field) {
    if (!equal && where_) {
      std::string at;
      for (const std::string &step : path_) at += step + "/";
      *where_ = at + field;
    }
    return equal;
  }
  template <typename T, typename F>
  bool list(const std::vector<T> &a, const std::vector<T> &b, const char *name, F each) {
    if (!same(a.size() == b.size(), std::string(name) + " count")) return false;
    for (size_t i = 0; i < a.size(); ++i) {
      path_.push_back(std::string(name) + ":" + std::to_string(i));
      const bool ok = each(a[i], b[i]);
      path_.pop_back();
      if (!ok) return false;
    }
    return true;
  }
  bool screen(const Screen &a, const Screen &b) {
    return same(a.name == b.name, "name") && same(a.has_music_var == b.has_music_var, "has_music_var") &&
           same(a.music_var == b.music_var, "music_var") &&
           list(a.roots, b.roots, "window", [this](const Window &x, const Window &y) { return window(x, y); });
  }
  bool appearance(const Appearance &a, const Appearance &b) {
    // The row's text as written (appearance_text): a colour row's word or reference, any other's name; the slot the
    // TYPE does not write is latent (a code-built row's colour reads back authored).
    return same(a.state == b.state, "state") && same(a.type == b.type, "type") &&
           same(appearance_text(a) == appearance_text(b), "value") &&
           same(a.has_map_state == b.has_map_state && a.map_state == b.map_state, "map_state") &&
           same(a.has_height == b.has_height && a.height == b.height, "height") && same(a.flags == b.flags, "flags");
  }
  bool appearances(const std::vector<Appearance> &a, const std::vector<Appearance> &b, const char *name) {
    return list(a, b, name, [this](const Appearance &x, const Appearance &y) { return appearance(x, y); });
  }
  bool item(const Item &a, const Item &b) {
    return same(a.type == b.type, "type") && same(a.value == b.value, "value") &&
           same(item_text(a) == item_text(b), "text") &&
           same(a.justify == b.justify, "justify") && same(a.vjustify == b.vjustify, "vjustify") &&
           same(a.justify_offset == b.justify_offset, "justify_offset") &&
           same(a.vjustify_offset == b.vjustify_offset, "vjustify_offset") &&
           same(a.pairs_list == b.pairs_list, "pairs_list") &&
           same(a.has_column == b.has_column && a.column == b.column, "column");
  }
  bool items(const Items &a, const Items &b) {
    path_.push_back("items");
    const bool ok =
        same(a.present == b.present, "present") && same(a.multiselect == b.multiselect, "multiselect") &&
        same(a.justify == b.justify, "justify") && same(a.vjustify == b.vjustify, "vjustify") &&
        same(a.justify_offset == b.justify_offset, "justify_offset") &&
        same(a.vjustify_offset == b.vjustify_offset, "vjustify_offset") &&
        appearances(a.appearances, b.appearances, "appearance") &&
        list(a.items, b.items, "item", [this](const Item &x, const Item &y) { return item(x, y); }) &&
        list(a.rows, b.rows, "row", [this](const TableRow &x, const TableRow &y) {
          return list(x.cells, y.cells, "item", [this](const Item &p, const Item &q) { return item(p, q); });
        });
    path_.pop_back();
    return ok;
  }
  bool column(const TableColumn &a, const TableColumn &b) {
    path_.push_back("column");
    const bool ok =
        same(a.has_count == b.has_count && a.count == b.count, "count") &&
        same(a.has_spacing == b.has_spacing && a.spacing == b.spacing, "spacing") &&
        list(a.headers, b.headers, "header",
             [this](const TableHeader &x, const TableHeader &y) {
               return same(x.justify == y.justify, "justify") && same(x.vjustify == y.vjustify, "vjustify") &&
                      same(x.has_column == y.has_column && x.column == y.column, "column") &&
                      same(x.sort == y.sort, "sort") && same(x.has_width == y.has_width && x.width == y.width, "width") &&
                      same(x.type == y.type, "type") && same(x.text == y.text, "text") &&
                      same(x.field == y.field, "field");
             }) &&
        list(a.bodies, b.bodies, "body",
             [this](const TableBody &x, const TableBody &y) {
               return same(x.justify == y.justify, "justify") && same(x.vjustify == y.vjustify, "vjustify") &&
                      same(x.justify_offset == y.justify_offset, "justify_offset") &&
                      same(x.vjustify_offset == y.vjustify_offset, "vjustify_offset") &&
                      same(x.has_column == y.has_column && x.column == y.column, "column") &&
                      same(x.bitmap_draw == y.bitmap_draw, "bitmap_draw") &&
                      same(x.bitmap_text == y.bitmap_text, "bitmap_text") &&
                      same(x.custom_draw == y.custom_draw, "custom_draw") && same(x.display == y.display, "display") &&
                      same(x.bitmap_flags == y.bitmap_flags, "bitmap_flags") &&
                      same(x.scale_bitmap == y.scale_bitmap, "scale_bitmap");
             }) &&
        list(a.substitutions, b.substitutions, "subst",
             [this](const TableSubst &x, const TableSubst &y) {
               return same(x.has_column == y.has_column && x.column == y.column, "column") &&
                      same(x.value == y.value, "value") && same(x.is_file == y.is_file, "file") &&
                      same(x.is_url == y.is_url, "url") && same(x.file == y.file, "text");
             }) &&
        same(a.primary_sort == b.primary_sort, "primary_sort") &&
        same(a.secondary_sort == b.secondary_sort, "secondary_sort") &&
        same(a.tertiary_sort == b.tertiary_sort, "tertiary_sort");
    path_.pop_back();
    return ok;
  }
  bool glb_fields(const GlbFields &a, const GlbFields &b) {
    return same(a.present == b.present, "glb_server_fields") &&
           same(a.has_count == b.has_count && a.count == b.count, "glb_server_fields.count") &&
           same(a.has_format == b.has_format && a.format == b.format, "glb_server_fields.format") &&
           list(a.fields, b.fields, "glb_server_fields.field", [this](const GlbFieldSlot &x, const GlbFieldSlot &y) {
             return same(x.which == y.which, "which") && same(x.text == y.text, "text");
           });
  }
  bool category(const GopherCategory &a, const GopherCategory &b) {
    return same(a.has_level == b.has_level && a.level == b.level, "level") && part(a.label, b.label, "label") &&
           list(a.entries, b.entries, "entry", [this](const GopherEntry &x, const GopherEntry &y) {
             return same(x.info == y.info, "info") && same(x.name == y.name, "name") &&
                    same(x.target == y.target, "target") && same(x.filter == y.filter, "filter") &&
                    same(x.table == y.table, "table") && same(x.text == y.text, "text");
           });
  }
  // A part's presence; its window is compared after its owner's own fields (window).
  bool part(const WindowPart &a, const WindowPart &b, const char *name) {
    return same(a.present() == b.present(), std::string(name) + " present");
  }
  // Two windows and everything under them: each pair's own fields, then its parts' windows (a
  // CATEGORY's LABEL with the parts, in the fields' order) and its child windows', the path to the
  // first difference kept. An explicit stack, never a recursion a level (a menu nests its windows as
  // deep as the game's stack takes them).
  bool window(const Window &a, const Window &b) {
    struct Pair {
      const Window *a;
      const Window *b;
      size_t depth; // path_'s size before its steps
      std::string step, label_step;
    };
    const size_t base = path_.size();
    std::vector<Pair> walk{{&a, &b, base, std::string(), std::string()}};
    std::vector<Pair> under;
    while (!walk.empty()) {
      Pair pair = std::move(walk.back());
      walk.pop_back();
      path_.resize(pair.depth);
      if (!pair.step.empty()) path_.push_back(pair.step);
      if (!pair.label_step.empty()) path_.push_back(pair.label_step);
      const Window &x = *pair.a, &y = *pair.b;
      if (!window_fields(x, y)) {
        path_.resize(base);
        return false;
      }
      const size_t depth = path_.size();
      under.clear();
      const auto part_pair = [&](const WindowPart &p, const WindowPart &q, const char *name) {
        if (p.present()) under.push_back({p.get(), q.get(), depth, name, std::string()});
      };
      part_pair(x.list_box, y.list_box, "list_box");
      part_pair(x.spinup, y.spinup, "spinup");
      part_pair(x.spindown, y.spindown, "spindown");
      part_pair(x.scrollbar, y.scrollbar, "scrollbar");
      part_pair(x.join_button, y.join_button, "join_button");
      part_pair(x.search_button, y.search_button, "search_button");
      part_pair(x.glb_table, y.glb_table, "glb_table");
      part_pair(x.glb_info_button, y.glb_info_button, "glb_info_button");
      part_pair(x.glb_info_player_details, y.glb_info_player_details, "glb_info_player_details");
      part_pair(x.glb_info_server_details, y.glb_info_server_details, "glb_info_server_details");
      part_pair(x.glb_nw_info, y.glb_nw_info, "glb_nw_info");
      for (size_t i = 0; i < x.gopher_categories.size(); ++i)
        if (x.gopher_categories[i].label.present())
          under.push_back({x.gopher_categories[i].label.get(), y.gopher_categories[i].label.get(), depth,
                           "gopher_category:" + std::to_string(i), "label"});
      part_pair(x.gopher_back, y.gopher_back, "gopher_back");
      for (size_t i = 0; i < x.children.size(); ++i)
        under.push_back({&x.children[i], &y.children[i], depth, "window:" + std::to_string(i), std::string()});
      for (size_t i = under.size(); i-- > 0;) walk.push_back(std::move(under[i]));
    }
    path_.resize(base);
    return true;
  }
  // A window's own fields, its parts' presence and its child windows' count.
  bool window_fields(const Window &a, const Window &b) {
    const Font &f = a.font, &g = b.font;
    const Frame &r = a.frame, &q = b.frame;
    return same(a.name == b.name, "name") && same(a.type == b.type, "type") && same(a.hidden == b.hidden, "hidden") &&
           same(a.disabled == b.disabled, "disabled") && same(a.checked == b.checked, "checked") &&
           same(a.draw_frame == b.draw_frame, "draw_frame") && same(a.modal == b.modal, "modal") &&
           same(a.readonly == b.readonly, "readonly") && same(a.as_button == b.as_button, "as_button") &&
           same(a.has_group == b.has_group && a.group == b.group, "group") && same(a.number == b.number, "number") &&
           same(a.has_minval == b.has_minval && a.minval == b.minval, "minval") &&
           same(a.has_maxval == b.has_maxval && a.maxval == b.maxval, "maxval") &&
           same(a.has_maxchar == b.has_maxchar && a.maxchar == b.maxchar, "maxchar") &&
           same(a.has_form == b.has_form && a.form == b.form, "form") && same(a.global_var == b.global_var, "global_var") &&
           same(a.password == b.password, "password") && same(a.orientation == b.orientation, "orientation") &&
           same(a.has_scroll_extent == b.has_scroll_extent && a.scroll_extent == b.scroll_extent &&
                    a.scroll_extent_is_width == b.scroll_extent_is_width,
                "scroll_extent") &&
           same(a.position.has_left == b.position.has_left && a.position.left == b.position.left &&
                    a.position.has_top == b.position.has_top && a.position.top == b.position.top &&
                    a.position.has_right == b.position.has_right && a.position.right == b.position.right &&
                    a.position.has_bottom == b.position.has_bottom && a.position.bottom == b.position.bottom,
                "position") &&
           appearances(a.appearances, b.appearances, "appearance") &&
           list(a.sounds, b.sounds, "sound",
                [this](const Sound &x, const Sound &y) {
                  return same(x.state == y.state, "state") && same(x.trigger == y.trigger, "trigger") &&
                         same(x.file == y.file, "file");
                }) &&
           list(a.actions, b.actions, "action",
                [this](const Action &x, const Action &y) {
                  return same(x.type == y.type, "type") && same(x.state == y.state, "state") &&
                         same(x.file == y.file, "file") && same(x.field == y.field, "field") &&
                         same(x.has_target_form == y.has_target_form && x.target_form == y.target_form, "target_form") &&
                         same(x.toggle == y.toggle, "toggle") && same(x.test == y.test, "test") &&
                         same(x.target == y.target, "target") && same(x.external_browser == y.external_browser, "external_browser");
                }) &&
           same(a.string_data.present == b.string_data.present && a.string_data.type == b.string_data.type &&
                    a.string_data.justify == b.string_data.justify && a.string_data.vjustify == b.string_data.vjustify &&
                    a.string_data.justify_offset == b.string_data.justify_offset &&
                    a.string_data.vjustify_offset == b.string_data.vjustify_offset &&
                    a.string_data.has_edge == b.string_data.has_edge && a.string_data.edge == b.string_data.edge &&
                    a.string_data.wrap == b.string_data.wrap && a.string_data.value == b.string_data.value,
                "string") &&
           same(a.toggle_string.present == b.toggle_string.present && a.toggle_string.type == b.toggle_string.type &&
                    a.toggle_string.value == b.toggle_string.value,
                "toggle_string") &&
           same(f.name == g.name && f.default_fg == g.default_fg && f.default_bg == g.default_bg &&
                    f.mouseover_fg == g.mouseover_fg && f.mouseover_bg == g.mouseover_bg &&
                    f.selected_fg == g.selected_fg && f.selected_bg == g.selected_bg && f.disabled_fg == g.disabled_fg &&
                    f.disabled_bg == g.disabled_bg,
                "font") &&
           same(r.stencil == q.stencil && r.has_stencil_size == q.has_stencil_size && r.stencil_size == q.stencil_size &&
                    r.brush == q.brush && r.monogram == q.monogram && r.has_insetx == q.has_insetx &&
                    r.insetx == q.insetx && r.has_insety == q.has_insety && r.insety == q.insety,
                "frame") &&
           items(a.items, b.items) && part(a.list_box, b.list_box, "list_box") &&
           same(a.has_sb_edge_pad == b.has_sb_edge_pad && a.sb_edge_pad == b.sb_edge_pad, "sb_edge_pad") &&
           part(a.spinup, b.spinup, "spinup") && part(a.spindown, b.spindown, "spindown") &&
           part(a.scrollbar, b.scrollbar, "scrollbar") &&
           same(a.cursor.file == b.cursor.file && a.cursor.flags == b.cursor.flags, "cursor") &&
           same(a.has_text_rsrc == b.has_text_rsrc && a.text_rsrc == b.text_rsrc, "text_rsrc") &&
           same(a.private_data == b.private_data, "private_data") && same(a.datasources == b.datasources, "datasource") &&
           list(a.hotkeys, b.hotkeys, "hotkey",
                [this](const Hotkey &x, const Hotkey &y) {
                  return same(x.value == y.value, "value") && same(x.virtual_key == y.virtual_key, "virtual");
                }) &&
           appearances(a.shuttle, b.shuttle, "shuttle") && appearances(a.scrollup, b.scrollup, "scrollup") &&
           appearances(a.scrolldown, b.scrolldown, "scrolldown") && column(a.table_data.column, b.table_data.column) &&
           same(a.table_data.has_min_item_height == b.table_data.has_min_item_height &&
                    a.table_data.min_item_height == b.table_data.min_item_height,
                "min_item_height") &&
           same(a.table_data.has_fixed_header_height == b.table_data.has_fixed_header_height &&
                    a.table_data.fixed_header_height == b.table_data.fixed_header_height,
                "fixed_header_height") &&
           part(a.join_button, b.join_button, "join_button") && part(a.search_button, b.search_button, "search_button") &&
           same(a.glb_playerlist == b.glb_playerlist, "glb_playerlist") && part(a.glb_table, b.glb_table, "glb_table") &&
           part(a.glb_info_button, b.glb_info_button, "glb_info_button") &&
           part(a.glb_info_player_details, b.glb_info_player_details, "glb_info_player_details") &&
           part(a.glb_info_server_details, b.glb_info_server_details, "glb_info_server_details") &&
           glb_fields(a.glb_server_fields, b.glb_server_fields) && part(a.glb_nw_info, b.glb_nw_info, "glb_nw_info") &&
           same(a.has_glb_target == b.has_glb_target && a.glb_target == b.glb_target, "glb_target") &&
           same(a.gopher_targets == b.gopher_targets, "gopher_targets") &&
           list(a.gopher_categories, b.gopher_categories, "gopher_category",
                [this](const GopherCategory &x, const GopherCategory &y) { return category(x, y); }) &&
           part(a.gopher_back, b.gopher_back, "gopher_back") &&
           same(a.children.size() == b.children.size(), "window count");
  }
};

}  // namespace

bool equal(const Document &a, const Document &b, std::string *where) { return Same(where).document(a, b); }

}  // namespace opennova::mnu
