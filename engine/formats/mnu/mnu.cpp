// MNU menu file parser implementation: the typed layer over mnu_xml's retail
// reader; the writer is mnu_write.cpp. docs/mnu/menu-re.md ("The reader", "The writer") has the
// rules with their witnesses.
#include <formats/mnu/mnu.h>

#include <formats/mns/mns.h>
#include <formats/mnu/mnu_layout.h>
#include <formats/mnu/mnu_schema.h>
#include <formats/mnu/mnu_xml.h>
#include <formats/rtxt/rtxt.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <set>
#include <sstream>

#include <base/io/cp1252.h>
#include <base/io/strutil.h>

namespace opennova::mnu {

namespace {

using mnu_xml::Attribute;
using mnu_xml::Node;
using mnu_xml::Text;
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

// --- the source decode [orig: XML_ParseWithBOMDetection @ 0x76a690;
// NapiXML_ParseMultiByte @ 0x76a4f0] ------------------------------------------

// A first byte of 0xFE or 0xFF reads the rest as little-endian UTF-16 from byte 2
// (so a big-endian file reads byte-swapped and loads nothing); EF BB BF reads UTF-8
// (MultiByteToWideChar(CP_UTF8): a bad sequence is U+FFFD); anything else is the
// system code page, 1252 here.
void decode_bytes(const uint8_t *data, size_t size, SourceEncoding &encoding,
                   Text &text) {
  text.clear();
  if (size >= 1 && (data[0] == 0xFE || data[0] == 0xFF)) {
    encoding = SourceEncoding::Utf16LE;
    for (size_t i = 2; i < size; i += 2) {
      uint32_t unit = data[i];
      if (i + 1 < size) unit |= uint32_t(data[i + 1]) << 8;
      if (unit >= 0xD800 && unit <= 0xDBFF && i + 3 < size) {
        const uint32_t low = uint32_t(data[i + 2]) | (uint32_t(data[i + 3]) << 8);
        if (low >= 0xDC00 && low <= 0xDFFF) {
          text.push_back(char32_t(0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00)));
          i += 2;
          continue;
        }
      }
      text.push_back(unit >= 0xD800 && unit <= 0xDFFF ? char32_t(0xFFFD) : char32_t(unit));
    }
    return;
  }
  if (size >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
    encoding = SourceEncoding::Utf8Bom;
    for (size_t i = 3; i < size;) {
      const uint8_t lead = data[i];
      uint32_t cp = 0;
      size_t count = 0;
      if (lead < 0x80) { cp = lead; count = 1; }
      else if (lead >= 0xC2 && lead <= 0xDF) { cp = lead & 0x1Fu; count = 2; }
      else if ((lead & 0xF0) == 0xE0) { cp = lead & 0x0Fu; count = 3; }
      else if (lead >= 0xF0 && lead <= 0xF4) { cp = lead & 0x07u; count = 4; }
      bool ok = count != 0 && i + count <= size;
      for (size_t k = 1; ok && k < count; ++k) {
        if ((data[i + k] & 0xC0) != 0x80) ok = false;
        else cp = (cp << 6) | (data[i + k] & 0x3Fu);
      }
      if (ok && ((count == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) ||
                 (count == 4 && (cp < 0x10000 || cp > 0x10FFFF))))
        ok = false;
      if (!ok) {
        text.push_back(char32_t(0xFFFD));
        ++i;
        continue;
      }
      text.push_back(char32_t(cp));
      i += count;
    }
    return;
  }
  encoding = SourceEncoding::CodePage;
  for (size_t i = 0; i < size; ++i) text.push_back(cp1252_decode_byte(data[i]));
}

// --- the typed read ---------------------------------------------------------

bool in(const Text &token, const char *const *tokens) {
  for (size_t i = 0; tokens[i]; ++i)
    if (mnu_xml::iequals(token, tokens[i])) return true;
  return false;
}

bool tag_is(const Node &node, const char *tag) { return mnu_xml::iequals(node.tag, tag); }

// The elements the base parse reads [orig: CUIElement_ParseXMLDefinition @ 0x648120].
const char *const kBaseTags[] = {"APPEARANCE", "FRAME", "SOUND", "POSITION", "FONT", "TEXT_RSRC",
                                 "ACTION", "CURSOR", "HOTKEY", "PRIVATE_DATA", "WINDOW", nullptr};
// The elements a SCROLL's own parse reads after the base [orig:
// CUIScrollWidget_ParseExtendedXMLDef @ 0x64c6d0].
const char *const kScrollTags[] = {"ORIENTATION", "HEIGHT", "WIDTH", "SHUTTLE", "SCROLLUP", "SCROLLLEFT",
                                   "SCROLLDOWN", "SCROLLRIGHT", nullptr};
// The elements a LIST's own parse reads [orig: CListWnd_ParseXMLDefinition @ 0x645770] and
// a TABLE's [orig: CTableWnd_ParseXMLContentDefinition @ 0x6427d0].
const char *const kListTags[] = {"ITEMS", "MIN_ITEM_HEIGHT", "SCROLLBAR", nullptr};
const char *const kTableTags[] = {"ITEMS", "COLUMN", "MIN_ITEM_HEIGHT", "FIXED_HEADER_HEIGHT", "SCROLLBAR",
                                  nullptr};

bool tag_in(const Node &node, const char *const *tags) { return in(node.tag, tags); }

// Whether a type's own parse reads a child element (grill set A3's table): the
// window-level elements only some types read. Everything else the base parse reads for
// every type. An empty value faults only where retail reads it.
bool type_reads(WindowType t, const Node &child) {
  using T = WindowType;
  const bool list = t == T::List || t == T::LanList;
  // STRING: every class whose parse chains to the STATIC one [orig:
  // CUIButtonWidget_ParseXMLAttributes @ 0x657c30].
  if (tag_is(child, "STRING"))
    return t == T::Static || t == T::Button || t == T::Edit || t == T::MultilineEdit || t == T::Radio ||
           t == T::CheckBox || t == T::SpinList || list || t == T::Table || t == T::Combo || t == T::RadioEdit;
  // TOGGLE_STRING: the BUTTON chain [orig: CButtonWnd_ParseTooltipXML @ 0x658170].
  if (tag_is(child, "TOGGLE_STRING"))
    return t == T::Button || t == T::Radio || t == T::CheckBox || t == T::SpinList || list || t == T::Table ||
           t == T::RadioEdit;
  // GROUP: the R-chain [orig: CRadioWnd_ParseXMLDefinition @ 0x656c40, the compare
  // @ 0x656c87; chained from CListWnd_ParseXMLDefinition @ 0x6457a0 and
  // CTableWnd_ParseXMLContentDefinition @ 0x642801; RADIOEDIT's embedded radio
  // @ 0x65d201].
  if (tag_is(child, "GROUP")) return t == T::Radio || list || t == T::Table || t == T::RadioEdit;
  // [orig: CListWnd_ParseXMLDefinition @ 0x64600d; CTableWnd_ParseXMLContentDefinition
  // @ 0x643a9f, FIXED_HEADER_HEIGHT @ 0x643ace]
  if (tag_is(child, "MIN_ITEM_HEIGHT")) return list || t == T::Table;
  if (tag_is(child, "FIXED_HEADER_HEIGHT")) return t == T::Table;
  if (tag_is(child, "ITEMS")) return list || t == T::SpinList || t == T::Table;
  if (tag_is(child, "SPINUP") || tag_is(child, "SPINDOWN")) return t == T::SpinList;
  if (tag_is(child, "LIST_BOX")) return t == T::Combo;
  if (tag_is(child, "SCROLLBAR")) return list || t == T::Table || t == T::MultilineEdit;
  if (tag_is(child, "COLUMN")) return t == T::Table;
  if (tag_in(child, kScrollTags)) return t == T::Scroll;
  return true;
}

// The elements the LAN_LIST, GLB_TABLE and GOPHER parses read, each by its schema path
// (schema_reads) on `type` [orig: CLanListWnd_ParseXMLDefinition @ 0x65b9d0;
// CLanGameBrowser_ParseExtendedXMLDefinition @ 0x65dac0; CGopherWnd_ParseXMLDefinition @
// 0x65b130]; null for any other element.
const char *typed_part_path(const Node &child, WindowType type) {
  static const struct {
    const char *tag;
    const char *path;
  } kParts[] = {
      {"JOIN_BUTTON", "join_button"},
      {"SEARCH_BUTTON", "search_button"},
      {"GLB_TABLE", "glb_table"},
      {"GLB_INFO_BUTTON", "glb_info_button"},
      {"GLB_INFO_PLAYER_DETAILS", "glb_info_player_details"},
      {"GLB_INFO_SERVER_DETAILS", "glb_info_server_details"},
      {"GLB_NW_INFO", "glb_nw_info"},
      {"FILTERS", "gopher_categories"},
      {"GOPHER_BACK", "gopher_back"},
  };
  for (const auto &part : kParts)
    if (tag_is(child, part.tag)) return part.path;
  if (tag_is(child, "TARGET")) return type == WindowType::Gopher ? "gopher_targets" : "glb_target";
  return nullptr;
}

// The three names CWnd_ParseVirtualKeyNameW @ 0x6467f0 knows (13, 27, 32).
const char *const kVirtualKeyNames[] = {"VK_RETURN", "VK_ESCAPE", "VK_SPACE"};

// The edit attributes (MAXCHAR, MAXVAL, MINVAL) [orig: CEditWnd_ParseXMLProperties
// @ 0x661d10]: EDIT, MULTILINE_EDIT and the RADIOEDIT edit part.
bool reads_edit_attributes(WindowType t) {
  return t == WindowType::Edit || t == WindowType::MultilineEdit || t == WindowType::RadioEdit;
}

// The per-window read state: the singleton elements seen so far (a later one
// replaces or merges into the earlier) and each part's own state.
struct ReadScope {
  std::map<std::string, const Node *> seen;
  std::map<const WindowPart *, ReadScope> parts;
};

class TreeReader {
public:
  explicit TreeReader(SourceEncoding encoding) : encoding_(encoding) {}

  void read_document(const mnu_xml::Document &xml, Document &out);
  void sweep(const mnu_xml::Document &xml, std::vector<ParseNote> &notes) const;

private:
  // The model's representation of retail's wide text: the code page's bytes
  // (WideCharToMultiByte(CP_ACP) on 1252, every consumer's narrowing: an unmappable
  // character is '?') or UTF-8 for a Unicode source.
  std::string narrow(const Text &text) const;
  std::string text_of(const Node &node) const {
    node.read = true;
    return narrow(node.text);
  }

  void because(const void *what, const std::string &why) { reasons_[what] = why; }
  // A note on input retail crashes or hangs on, where retail reads it (`reads_`).
  void fatal_because(const void *what, const std::string &why) {
    reasons_[what] = why;
    if (reads_) fatal_.insert(what);
  }
  // A fatal note on a read element the model keeps, where retail reads it: retail crashes on what the
  // element (or its window, through it) holds (docs/mnu/menu-re.md, "Crash and hang cases").
  void crash_note(const Node &node, const std::string &why) {
    if (!reads_) return;
    text_notes_[&node] = why;
    fatal_.insert(&node);
  }
  // An attribute with no token (an empty or bare value): retail's comparison or
  // conversion faults on it where its parse reads it.
  void empty_value(const Node &node, const Attribute &attr);
  // While one lives, `reads_` also requires `reads` (the element being read is one the
  // window's type reads).
  class Reads {
  public:
    Reads(TreeReader &reader, bool reads) : reader_(reader), saved_(reader.reads_) { reader.reads_ = saved_ && reads; }
    ~Reads() { reader_.reads_ = saved_; }
    Reads(const Reads &) = delete;
    Reads &operator=(const Reads &) = delete;

  private:
    TreeReader &reader_;
    bool saved_;
  };
  // The token of an attribute (marked read); false, with a note, when retail's
  // wcstok finds none (an empty or bare value retail faults on).
  bool token(const Node &node, const Attribute *attr, std::string &out);
  // The first authored attribute of a name (a whole-list walk keeps it); the others
  // are noted as repeats.
  const Attribute *first(const Node &node, const char *name);
  // The first authored attribute whose token is one of `tokens` (a walk that only
  // overwrites on a match keeps it), else the first authored.
  const Attribute *effective(const Node &node, const char *name, const char *const *tokens);
  // A presence flag: any attribute of the name.
  bool flag(const Node &node, const char *name);
  // A keyword attribute's token (effective), as its table spells it; empty when absent, and for a
  // token the table lacks (the code no token sets; noted, left out).
  std::string keyword(const Node &node, const char *name, const char *const *tokens);
  // A JUSTIFY / VJUSTIFY: each one's keyword sets the justification and another token's wcstol the
  // x or y offset, two fields, so the first authored of each kind is kept (`keyword` as its table
  // spells it, `offset` the number's decimal), each set only where one is authored.
  void justification(const Node &node, const char *name, const char *const *tokens, std::string &keyword,
                     std::string &offset);
  // The first authored attribute of a name whose value has a token, the others noted: a walk that
  // skips a value of no token (`if (token)`) and overwrites, the list last authored first [orig:
  // CGopherWnd_ParseCategoryXML @ 0x65a5ba, @ 0x65a7da]. An empty one is noted, not fatal.
  std::string first_token(const Node &node, const char *name);
  // A VALUE the parse reads with wcstol: the number's decimal; empty when absent (and, `zero_none`,
  // for 0, the parse's default).
  std::string number_value(const Node &node, const char *name, bool zero_none);
  // A FLAGS word [orig: sub_646CC0 @ 0x646cc0]; kDefaultUiMaterialFlags when absent.
  uint32_t flags_attr(const Node &node, const char *name);
  std::string string_attr(const Node &node, const char *name);
  bool number_attr(const Node &node, const char *name, int &out);
  int number_text(const Node &node) {
    node.read = true;
    variable_number(node, nullptr, node.text);
    return static_cast<int>(mnu_xml::wcstol(node.text, 10));
  }
  // A number (the element's text, or `attr`'s token) that holds a %NAME%: a note on it,
  // fatal where retail reads it; the model keeps the number read here. True when it does.
  bool variable_number(const Node &node, const Attribute *attr, const Text &text);
  // A colour element's text (the node's), as the model holds it (color_of over the wide text the
  // parse reads), with a note on a text the parse reads otherwise than whole.
  Color color(const Node &node);
  // A singleton element: the earlier one of the tag (if any) is noted as replaced
  // (merge == false) or merged; true when this is the first.
  bool singleton(ReadScope &frame, const Node &node, const char *tag, bool merge);

  bool read_screen(const Node &node, Screen &out);
  bool read_created_window(const Node &node, Window &out);
  void read_window_body(const Node &node, Window &w, ReadScope &frame);
  void read_part(const Node &node, WindowPart &part, WindowType type, ReadScope &owner);
  bool read_appearance(const Node &node, Appearance &out, const char *const *types);
  bool read_sound(const Node &node, Sound &out);
  void read_position(const Node &node, Position &pos);
  void read_font(const Node &node, Font &font);
  void read_frame(const Node &node, mnu::Frame &frame);
  void read_action(const Node &node, Action &out);
  void read_cursor(const Node &node, Cursor &cursor);
  void read_string(const Node &node, String &s, bool repeated);
  // `cell`: a TABLE ROW cell, the only ITEM whose COLUMN retail reads; `type` the window's.
  void read_item(const Node &node, Item &item, bool cell, WindowType type);
  // False when a row retail's LIST / TABLE parse stops at was read. Each row and attribute
  // the window's `type` reads (schema_reads "items.*"); the others are left out.
  bool read_items(const Node &node, Items &items, WindowType type);
  // Each attribute of `name` on `node`: one the window's type does not read, left out.
  void not_read(const Node &node, const char *name, WindowType type);
  void read_column(const Node &node, TableColumn &column);
  void read_glb_fields(const Node &node, GlbFields &fields);
  bool read_gopher_category(const Node &node, GopherCategory &category, ReadScope &owner);
  void unread_rest(const Node &parent, size_t from, const char *const *tags, const std::string &why);

  SourceEncoding encoding_;
  std::map<const void *, std::string> reasons_;
  std::set<const void *> fatal_;
  // A read element's own note: a colour of which retail reads part of the text, or what retail
  // crashes on (crash_note).
  std::map<const Node *, std::string> text_notes_;
  // Each FONT colour's element by its window's element and the colour's place in the FONT, so a later
  // one (of any FONT of that window) takes the earlier's note with it.
  std::map<std::pair<const Node *, size_t>, const Node *> font_colour_nodes_;
  bool reads_ = true; // retail reads what is being read now
  // A GLB_TABLE's browser reads its tables' HEADER FIELDs (read_column) [orig: @ 0x65dac0].
  bool glb_header_fields_ = false;
};

std::string TreeReader::narrow(const Text &text) const {
  std::string out;
  out.reserve(text.size());
  for (char32_t c : text) {
    if (encoding_ == SourceEncoding::CodePage) {
      uint8_t byte = 0;
      out.push_back(cp1252_encode_codepoint(c, byte) ? static_cast<char>(byte) : '?');
    } else {
      utf8_append(out, c);
    }
  }
  return out;
}

void TreeReader::empty_value(const Node &node, const Attribute &attr) {
  attr.read = false;
  const std::string what = "An empty " + mnu_xml::ascii(attr.name) + " on " + mnu_xml::ascii(node.tag);
  if (reads_)
    fatal_because(&attr, what + ": retail's parse faults reading it; it is left out.");
  else
    because(&attr, what + ": retail does not read it here; it is left out.");
}

bool TreeReader::token(const Node &node, const Attribute *attr, std::string &out) {
  out.clear();
  if (!attr) return false;
  attr->read = true;
  Text tok;
  if (!attr->token(tok)) {
    empty_value(node, *attr);
    return false;
  }
  out = narrow(tok);
  return true;
}

// The walk reads every attribute of the name, so an empty one faults even where
// another is the one kept.
const Attribute *TreeReader::first(const Node &node, const char *name) {
  const Attribute *chosen = node.attr(name);
  for (const Attribute &a : node.attributes) {
    if (&a == chosen || !mnu_xml::iequals(a.name, name)) continue;
    Text tok;
    if (!a.token(tok)) empty_value(node, a);
    else because(&a, std::string("A repeated ") + name + ": retail keeps the first; this one is left out.");
  }
  return chosen;
}

const Attribute *TreeReader::effective(const Node &node, const char *name, const char *const *tokens) {
  const Attribute *chosen = nullptr;
  for (const Attribute &a : node.attributes) {
    if (!mnu_xml::iequals(a.name, name)) continue;
    Text tok;
    if (a.token(tok) && in(tok, tokens)) { chosen = &a; break; }
  }
  if (!chosen) chosen = node.attr(name);
  for (const Attribute &a : node.attributes) {
    if (&a == chosen || !mnu_xml::iequals(a.name, name)) continue;
    Text tok;
    if (!a.token(tok)) empty_value(node, a);
    else because(&a, std::string("A repeated ") + name + ": retail keeps the first it knows; this one is left out.");
  }
  return chosen;
}

bool TreeReader::flag(const Node &node, const char *name) {
  bool any = false;
  for (const Attribute &a : node.attributes)
    if (mnu_xml::iequals(a.name, name)) { a.read = true; any = true; }
  return any;
}

std::string TreeReader::keyword(const Node &node, const char *name, const char *const *tokens) {
  std::string out;
  const Attribute *attr = effective(node, name, tokens);
  if (!token(node, attr, out)) return std::string();
  // A token the parse knows, as its table spells it: the compare is without case, so the
  // file's spelling is none of the model's.
  for (const char *const *t = tokens; *t; ++t)
    if (iequals(out, *t)) return *t;
  // Another token sets no code: the parse reads it as none.
  attr->read = false;
  because(attr, std::string(name) + " \"" + out + "\" is no token the parse knows: it sets nothing, as no " + name +
                    " does; it is left out.");
  return std::string();
}

void TreeReader::justification(const Node &node, const char *name, const char *const *tokens, std::string &keyword,
                               std::string &offset) {
  // The walk compares every one of the name, so an empty one faults (CRT_wcsicmp of no token).
  const Attribute *kept_keyword = nullptr;
  const Attribute *kept_offset = nullptr;
  for (const Attribute &a : node.attributes) {
    if (!mnu_xml::iequals(a.name, name)) continue;
    Text tok;
    if (!a.token(tok)) {
      empty_value(node, a);
      continue;
    }
    const bool is_keyword = in(tok, tokens);
    const Attribute *&kept = is_keyword ? kept_keyword : kept_offset;
    if (kept) {
      because(&a, std::string("A repeated ") + name + (is_keyword ? " keyword" : " offset") +
                      ": retail keeps the first; this one is left out.");
      continue;
    }
    kept = &a;
  }
  std::string text;
  if (kept_keyword && token(node, kept_keyword, text))
    for (const char *const *t = tokens; *t; ++t)
      if (iequals(text, *t)) keyword = *t;
  if (kept_offset) {
    kept_offset->read = true;
    Text tok;
    kept_offset->token(tok);
    variable_number(node, kept_offset, tok);
    offset = std::to_string(static_cast<int>(mnu_xml::wcstol(tok, 10)));
  }
}

std::string TreeReader::first_token(const Node &node, const char *name) {
  const Attribute *kept = nullptr;
  for (const Attribute &a : node.attributes) {
    if (!mnu_xml::iequals(a.name, name)) continue;
    Text tok;
    if (!a.token(tok)) {
      because(&a, std::string("An empty ") + name + " on " + mnu_xml::ascii(node.tag) +
                      ": retail skips a value of no token; it is left out.");
      continue;
    }
    if (kept) {
      because(&a, std::string("A repeated ") + name + ": retail keeps the first; this one is left out.");
      continue;
    }
    kept = &a;
  }
  std::string out;
  token(node, kept, out);
  return out;
}

std::string TreeReader::number_value(const Node &node, const char *name, bool zero_none) {
  const Attribute *attr = first(node, name);
  std::string ignored;
  if (!token(node, attr, ignored)) return std::string();
  Text tok;
  attr->token(tok);
  variable_number(node, attr, tok);
  const int value = static_cast<int>(mnu_xml::wcstol(tok, 10));
  if (zero_none && value == 0) {
    // The parse's default: the model holds none, and the writer leaves it out.
    attr->read = false;
    because(attr, std::string(name) + " reads 0, the parse's default, as none does; it is left out.");
    return std::string();
  }
  return std::to_string(value);
}

uint32_t TreeReader::flags_attr(const Node &node, const char *name) {
  std::string text;
  const Attribute *attr = first(node, name);
  if (!token(node, attr, text)) return kDefaultUiMaterialFlags;
  const uint32_t word = ui_material_flags_of(text);
  if (word == kDefaultUiMaterialFlags) {
    attr->read = false;
    because(attr, std::string(name) + " reads the default word (STANDARD_TRANSPARENT), as none does; it is left out.");
  }
  return word;
}

void TreeReader::not_read(const Node &node, const char *name, WindowType type) {
  for (const Attribute &a : node.attributes)
    if (mnu_xml::iequals(a.name, name))
      because(&a, std::string("A ") + strutil::to_upper(window_type_name(type)) + " reads no " + name +
                      " (its type's parse does not); it is left out.");
}

std::string TreeReader::string_attr(const Node &node, const char *name) {
  std::string out;
  token(node, first(node, name), out);
  return out;
}

bool TreeReader::number_attr(const Node &node, const char *name, int &out) {
  const Attribute *attr = first(node, name);
  std::string ignored;
  if (!token(node, attr, ignored)) return false;
  Text tok;
  attr->token(tok);
  variable_number(node, attr, tok);
  out = static_cast<int>(mnu_xml::wcstol(tok, 10));
  return true;
}

// The game expands every %NAME% of a menu before it reads the menu [orig:
// UIScene_LoadAndParseContent @ 0x63c830, NapiXML_ExpandVariablesInText @ 0x63c980 before
// the parse @ 0x63c9a4], so a number holding one is read from the variable's value (a
// POSITION edge's wcstol @ 0x648b47). The model holds only the number read here, which a
// save would write in place of the variable: where retail reads it, the menu cannot be
// kept as it stands (D-MNU-1). A repeated attribute first() leaves out is overwritten by
// the first authored in the same walk (FORM's store @ 0x6482d8), so it blocks nothing,
// except a HEADER COLUMN a sort key took (read_column).
bool TreeReader::variable_number(const Node &node, const Attribute *attr, const Text &text) {
  const std::string value = narrow(text);
  if (!mns::holds_variable_reference(value)) return false;
  std::string what = attr ? mnu_xml::ascii(attr->name) + " on " : std::string();
  what += "<" + mnu_xml::ascii(node.tag) + "> holds a stylesheet variable where a number goes ('" + value + "')";
  const void *item = attr ? static_cast<const void *>(attr) : &node;
  if (attr) attr->read = false;
  else node.read = false;
  const std::string read_here = std::to_string(mnu_xml::wcstol(text, 10));
  if (reads_)
    fatal_because(item, what + ": retail reads the variable's value as the number, which the menu model "
                               "cannot hold (it reads " + read_here + " here).");
  else
    because(item, what + ": retail does not read it here (the menu model reads " + read_here + ").");
  return true;
}

// A colour's text [orig: CRT_wcstoxl @ 0x76e93b through the FONT arm @ 0x648d14..0x648e64, the APPEARANCE
// COLOR / OUTLINE arm @ 0x648562 and CUISpinList_ParseXMLDefinition @ 0x64bd10]: the model holds one %NAME% as
// the reference and any other text as the word wcstoul reads of it (color_of). A %NAME% inside a longer text is
// read from the variable's value in retail (the expansion runs over the whole menu first [orig:
// NapiXML_ExpandVariablesInText @ 0x63a000]), which the model cannot hold: noted, fatal where retail reads it,
// as a number's (D-MNU-1). Characters the read stops at (a '#', a word past its digits, no digit at all) are
// noted as not read: the model holds the word read.
Color TreeReader::color(const Node &node) {
  const std::string text = narrow(node.text);
  const Color read = color_of(node.text, text);
  if (!read.var.empty()) return read;
  const std::string word = color_text(read.argb);
  const std::string tag = "<" + mnu_xml::ascii(node.tag) + ">";
  if (mns::holds_variable_reference(text)) {
    text_notes_[&node] = tag + " holds a stylesheet variable inside a colour ('" + text +
                         "'): retail reads the variable's value there, which the menu model cannot hold (it reads " +
                         word + " here).";
    if (reads_) fatal_.insert(&node);
  } else if (!color_reads_whole(node.text)) {
    text_notes_[&node] = tag + "'s colour '" + text + "' reads as " + word +
                         ": retail's read stops at the first character that is no hex digit, and the rest is "
                         "left out.";
  }
  return read;
}

bool TreeReader::singleton(ReadScope &frame, const Node &node, const char *tag, bool merge) {
  auto it = frame.seen.find(tag);
  if (it == frame.seen.end()) {
    frame.seen[tag] = &node;
    return true;
  }
  if (merge) {
    // Retail reads both into the same fields; the model holds one element.
    because(&node, std::string("A repeated ") + tag + ": retail reads it into the same fields as the first; "
                   "it is saved as one " + tag + ".");
    return false;
  }
  // The later value is the one retail keeps, so a variable in this one's text is lost to
  // nothing (variable_number); what its attributes crash on stays fatal.
  it->second->read = false;
  fatal_.erase(it->second);
  because(it->second, std::string("A later ") + tag + " replaces this one (retail keeps the last); it is left out.");
  it->second = &node;
  return false;
}

// Marks the elements of `tags` from `from` on as not read, with `why`.
void TreeReader::unread_rest(const Node &parent, size_t from, const char *const *tags, const std::string &why) {
  for (size_t i = from; i < parent.children.size(); ++i) {
    const Node &child = *parent.children[i];
    if (tags && !tag_in(child, tags)) continue;
    child.read = false;
    because(&child, why);
  }
}

// [orig: parse_script_block @ 0x63b800 -> CUIScene_ParseNodeAttributes @ 0x639630]
void TreeReader::read_document(const mnu_xml::Document &xml, Document &out) {
  for (const auto &root : xml.roots) {
    if (!tag_is(*root, "SCREEN")) {
      because(root.get(), "Only SCREEN elements are read at the top level; <" + mnu_xml::ascii(root->tag) +
                              "> and everything in it are left out.");
      continue;
    }
    Screen screen;
    read_screen(*root, screen);
    out.screens.push_back(std::move(screen));
  }
}

// A SCREEN reads its NAME, MUSICVAR and WINDOW children and nothing else: every
// root WINDOW is created with parent 0 and kept in document order; the last NAME
// and MUSICVAR win.
bool TreeReader::read_screen(const Node &node, Screen &out) {
  node.read = true;
  for (const Attribute &a : node.attributes)
    because(&a, "SCREEN attributes are not read (NAME and MUSICVAR are child elements); " +
                    mnu_xml::ascii(a.name) + " is left out.");
  ReadScope frame;
  for (const auto &child_ptr : node.children) {
    const Node &child = *child_ptr;
    if (tag_is(child, "NAME")) {
      singleton(frame, child, "NAME", false);
      out.name = text_of(child);
    } else if (tag_is(child, "MUSICVAR")) {
      // wcstok(text, L"\"") then wcstol: an empty one faults.
      Text tok;
      mnu_xml::Attribute probe;
      probe.value = child.text;
      probe.has_value = true;
      if (!probe.token(tok)) {
        fatal_because(&child, "An empty MUSICVAR: retail's parse faults reading it; it is left out.");
        continue;
      }
      singleton(frame, child, "MUSICVAR", false);
      child.read = true;
      variable_number(child, nullptr, tok);
      out.music_var = static_cast<int>(mnu_xml::wcstol(tok, 10));
      out.has_music_var = true;
    } else if (tag_is(child, "WINDOW")) {
      Window window;
      if (read_created_window(child, window)) out.roots.push_back(std::move(window));
    } else if (tag_is(child, "TEXT_RSRC") || tag_is(child, "CURSOR")) {
      because(&child, "A SCREEN reads only NAME, MUSICVAR and WINDOW: " + mnu_xml::ascii(child.tag) +
                          " belongs in a root WINDOW; this one is left out.");
    }
  }
  // No NAME leaves the screen's name null, which each created root window copies [orig:
  // CUIScene_ParseNodeAttributes @ 0x639630, the name cleared @ 0x639642; the copy 0x646c10 called
  // @ 0x639791]; an empty NAME element's text is "" [orig: NapiXML_ParseElementTree @ 0x769d70, the close
  // tag's empty text].
  if (!frame.seen.count("NAME") && !out.roots.empty())
    crash_note(node, "A SCREEN with no NAME crashes retail as it creates the screen's first window (the window "
                     "copies the null name).");
  return true;
}

// [orig: CUIScene_CreateWidgetByType @ 0x64f630] The factory takes the WINDOW's
// first child element and the TYPE its attribute walk meets first (the last
// authored); no child element or no TYPE creates nothing.
bool TreeReader::read_created_window(const Node &node, Window &out) {
  if (node.children.empty()) {
    because(&node, "A WINDOW with no child element is not created by retail; it is left out.");
    return false;
  }
  const Attribute *type = node.last_attr("TYPE");
  if (!type) {
    because(&node, "A WINDOW with no TYPE is not created by retail; it and everything in it are left out.");
    return false;
  }
  for (const Attribute &a : node.attributes)
    if (&a != type && mnu_xml::iequals(a.name, "TYPE"))
      because(&a, "A repeated TYPE: the widget factory takes the last; this one is left out.");
  std::string token_text;
  if (!token(node, type, token_text)) {
    fatal_because(&node, "A WINDOW with an empty TYPE: retail's factory faults on it; it is left out.");
    return false;
  }
  // The factory's match of the token (a generic window for a token it does not match); the writer
  // puts the type's own name down.
  out.type = parse_window_type(token_text);
  ReadScope frame;
  read_window_body(node, out, frame);
  return true;
}

// A part's own embedded widget parses the element's children with its own chain
// (the attributes are the element's); an empty one hands the parse no node, which
// faults. A repeated part element parses into the same widget.
void TreeReader::read_part(const Node &node, WindowPart &part, WindowType type, ReadScope &owner) {
  if (node.children.empty()) {
    if (reads_)
      fatal_because(&node, "An empty " + mnu_xml::ascii(node.tag) +
                               " crashes retail (its widget parses no element); it is left out.");
    else
      because(&node, "An empty " + mnu_xml::ascii(node.tag) + ": retail does not read it here; it is left out.");
    return;
  }
  const bool first_time = !part.present();
  Window &w = part.author(type);
  if (!first_time) {
    because(&node, "A repeated " + mnu_xml::ascii(node.tag) + ": retail parses it into the same widget; "
                       "it is saved as one.");
  }
  for (const Attribute &a : node.attributes)
    if (mnu_xml::iequals(a.name, "TYPE"))
      because(&a, "A " + mnu_xml::ascii(node.tag) + " is always its owner's own widget; TYPE is left out.");
  read_window_body(node, w, owner.parts[&part]);
  if (!first_time) node.read = false; // the note covers the element; its content is read
}

// The base parse [orig: CUIElement_ParseXMLDefinition @ 0x648120] then the window's
// type's own parse: the model keeps what its type reads, an element or an attribute only
// another type's parse reads left out (schema_reads, grill set A3).
void TreeReader::read_window_body(const Node &node, Window &w, ReadScope &frame) {
  node.read = true;
  // The base attribute walk: NAME and FORM (first authored), the presence flags.
  if (const Attribute *name = first(node, "NAME")) {
    std::string value;
    if (token(node, name, value)) w.name = value;
  }
  int number = 0;
  // [orig: CUIElement_ParseXMLDefinition FORM compare @ 0x6482a6, GLOBAL_VAR @ 0x648323]
  if (number_attr(node, "FORM", number)) { w.form = number; w.has_form = true; }
  w.hidden = flag(node, "HIDDEN") || w.hidden;
  w.disabled = flag(node, "DISABLE") || w.disabled;
  w.global_var = flag(node, "GLOBAL_VAR") || w.global_var;
  w.draw_frame = flag(node, "DRAW_FRAME") || w.draw_frame;
  w.modal = flag(node, "MODAL") || w.modal;
  // The per-type attribute walks [orig: CRadioWnd_ParseXMLDefinition @ 0x656c40;
  // CCheckWnd_ParseXMLDefinition @ 0x64ad90; CEditWnd_ParseXMLProperties
  // @ 0x661d10; CLanGameBrowser_ParseExtendedXMLDefinition @ 0x65dac0].
  const auto own = [&w](const char *path) { return schema_reads(w.type, path) == SchemaApplies::Reads; };
  if (own("checked")) w.checked = flag(node, "CHECKED") || w.checked;
  else not_read(node, "CHECKED", w.type);
  if (own("as_button")) w.as_button = flag(node, "AS_BUTTON") || w.as_button;
  else not_read(node, "AS_BUTTON", w.type);
  if (reads_edit_attributes(w.type)) {
    w.password = flag(node, "PASSWORD") || w.password;  // [orig: PASSWORD compare @ 0x661d3b]
    w.number = flag(node, "NUMBER") || w.number;
    w.readonly = flag(node, "READONLY") || w.readonly;
    if (number_attr(node, "MAXCHAR", number)) { w.maxchar = number; w.has_maxchar = true; }
    if (number_attr(node, "MAXVAL", number)) { w.maxval = number; w.has_maxval = true; }
    if (number_attr(node, "MINVAL", number)) { w.minval = number; w.has_minval = true; }
  } else {
    for (const char *name : {"PASSWORD", "NUMBER", "READONLY", "MAXCHAR", "MAXVAL", "MINVAL"}) not_read(node, name, w.type);
  }
  // GLOBAL_VAR reads the scene, still unset while the parse runs, on an edit window and on a CHECKED
  // checkbox [orig: CEditWnd_ParseXMLProperties @ 0x661e5f; CCheckWnd_ParseXMLDefinition @ 0x64ae05].
  if (w.global_var && (reads_edit_attributes(w.type) || (w.type == WindowType::CheckBox && w.checked)))
    crash_note(node, reads_edit_attributes(w.type)
                         ? "GLOBAL_VAR on an edit window faults when retail parses it (the scene is not set yet)."
                         : "GLOBAL_VAR on a CHECKED checkbox faults when retail parses it (the scene is not set yet).");
  // PLAYERLIST sets the browser's list mode, SERVERLIST clears it, by presence, over the attribute
  // list last authored first: the first authored of the two decides [orig:
  // CLanGameBrowser_ParseExtendedXMLDefinition @ 0x65dadf..0x65db16, self[3658]].
  const Attribute *list_mode = nullptr;
  for (const Attribute &a : node.attributes) {
    if (mnu_xml::iequals(a.name, "PLAYERLIST") || mnu_xml::iequals(a.name, "SERVERLIST")) {
      if (!own("glb_playerlist")) {
        not_read(node, mnu_xml::ascii(a.name).c_str(), w.type);
        continue;
      }
      if (list_mode) {
        because(&a, "A later PLAYERLIST or SERVERLIST: retail's mode is the first authored's; this one is left out.");
        continue;
      }
      a.read = true;
      list_mode = &a;
      w.glb_playerlist = mnu_xml::iequals(a.name, "PLAYERLIST");
    } else if (mnu_xml::iequals(a.name, "GROUP")) {
      because(&a, "GROUP is read only as a child element (<GROUP>n</GROUP>); the attribute is left out.");
    } else if (mnu_xml::iequals(a.name, "DISABLED")) {
      because(&a, "DISABLED is not a WINDOW attribute (retail reads DISABLE); it is left out.");
    }
  }

  // The base element loop: an APPEARANCE or SOUND it cannot use returns E_FAIL,
  // which leaves the rest of the loop and the child WINDOWs unread; the caller
  // ignores the result, so the type's own parse still runs.
  bool stopped = false;
  std::vector<const Node *> windows;
  const auto &kids = node.children;
  for (size_t i = 0; i < kids.size(); ++i) {
    const Node &child = *kids[i];
    if (!tag_in(child, kBaseTags)) continue;
    if (stopped) continue;
    if (tag_is(child, "APPEARANCE")) {
      Appearance appearance;
      if (!read_appearance(child, appearance, kAppearanceTypes)) {
        stopped = true;
        windows.clear();
        because(&child, "An APPEARANCE with no attributes or no STATE retail knows (DEFAULT, DISABLED, "
                        "MOUSEOVER, SELECTED) stops this window's parse in retail: it, the elements after it "
                        "and the child windows are left out.");
        unread_rest(node, i + 1, kBaseTags,
                    "Not read: an earlier APPEARANCE or SOUND stopped this window's parse in retail.");
        continue;
      }
      child.read = true;
      w.appearances.push_back(std::move(appearance));
    } else if (tag_is(child, "SOUND")) {
      Sound sound;
      if (!read_sound(child, sound)) {
        stopped = true;
        windows.clear();
        because(&child, "A SOUND with no STATE retail knows (MOUSEIN, MOUSEOUT, SELECTED) or no TRIGGER stops "
                        "this window's parse in retail: it, the elements after it and the child windows are "
                        "left out.");
        unread_rest(node, i + 1, kBaseTags,
                    "Not read: an earlier APPEARANCE or SOUND stopped this window's parse in retail.");
        continue;
      }
      child.read = true;
      w.sounds.push_back(std::move(sound));
    } else if (tag_is(child, "FRAME")) {
      const bool first_one = singleton(frame, child, "FRAME", true);
      read_frame(child, w.frame);
      if (!first_one) child.read = false;
    } else if (tag_is(child, "POSITION")) {
      const bool first_one = singleton(frame, child, "POSITION", true);
      read_position(child, w.position);
      if (!first_one) child.read = false;
    } else if (tag_is(child, "FONT")) {
      const bool first_one = singleton(frame, child, "FONT", true);
      read_font(child, w.font);
      if (!first_one) child.read = false;
    } else if (tag_is(child, "TEXT_RSRC")) {
      singleton(frame, child, "TEXT_RSRC", false);
      w.text_rsrc = text_of(child);
      w.has_text_rsrc = true;
    } else if (tag_is(child, "ACTION")) {
      Action action;
      read_action(child, action);
      w.actions.push_back(std::move(action));
    } else if (tag_is(child, "CURSOR")) {
      const bool first_one = singleton(frame, child, "CURSOR", true);
      read_cursor(child, w.cursor);
      if (!first_one) child.read = false;
    } else if (tag_is(child, "HOTKEY")) {
      // VIRTUAL by presence: the text one of the three names CWnd_ParseVirtualKeyNameW @ 0x6467f0
      // knows, untrimmed, without case; else the key is the text's first character. A key of 0
      // registers nothing [orig: CWnd_RegisterHotkeysRecursive @ 0x649d90].
      Hotkey hotkey;
      hotkey.virtual_key = flag(child, "VIRTUAL");
      const std::string text = text_of(child);
      if (hotkey.virtual_key) {
        for (const char *name : kVirtualKeyNames)
          if (iequals(text, name)) hotkey.value = name;
      } else if (!child.text.empty()) {
        hotkey.value = narrow(Text(1, child.text[0]));
      }
      if (hotkey.value.empty()) {
        child.read = false;
        because(&child, hotkey.virtual_key ? "A VIRTUAL HOTKEY of a name the game does not know (VK_RETURN, VK_ESCAPE, "
                                             "VK_SPACE) is key 0, which registers nothing; it is left out."
                                           : "An empty HOTKEY is key 0, which registers nothing; it is left out.");
        continue;
      }
      w.hotkeys.push_back(std::move(hotkey));
    } else if (tag_is(child, "PRIVATE_DATA")) {
      singleton(frame, child, "PRIVATE_DATA", false);
      w.private_data = text_of(child);
    } else if (tag_is(child, "WINDOW")) {
      windows.push_back(&child);
    }
  }
  // The child WINDOWs, created after the loop [orig: @ 0x649772]; none after a stop.
  if (stopped)
    for (const auto &child : kids)
      if (tag_is(*child, "WINDOW")) {
        child->read = false;
        because(child.get(), "Not created: an APPEARANCE or SOUND stopped this window's parse in retail.");
      }
  for (const Node *child : windows) {
    Window window;
    if (read_created_window(*child, window)) w.children.push_back(std::move(window));
  }

  // The type's own elements, in document order.
  const bool list_rule = w.type == WindowType::List || w.type == WindowType::LanList;
  const bool table_rule = w.type == WindowType::Table;
  bool scroll_stopped = false, own_stopped = false;
  for (size_t i = 0; i < kids.size(); ++i) {
    const Node &child = *kids[i];
    if (tag_in(child, kBaseTags)) continue;
    if ((scroll_stopped && tag_in(child, kScrollTags)) ||
        (own_stopped && tag_in(child, list_rule ? kListTags : kTableTags)))
      continue;
    // An element only another type's parse reads: left out (the window's type never reads it).
    const char *typed = typed_part_path(child, w.type);
    const bool read_here = typed ? schema_reads(w.type, typed) == SchemaApplies::Reads
                           : tag_is(child, "DATASOURCE") ? w.type == WindowType::Marquee
                                                         : type_reads(w.type, child);
    if (!read_here) {
      if (typed || tag_is(child, "DATASOURCE") || tag_in(child, kScrollTags) ||
          tag_in(child, kTableTags) || tag_is(child, "STRING") || tag_is(child, "TOGGLE_STRING") ||
          tag_is(child, "GROUP") || tag_is(child, "SPINUP") || tag_is(child, "SPINDOWN") || tag_is(child, "LIST_BOX"))
        because(&child, "A " + strutil::to_upper(window_type_name(w.type)) + " reads no " +
                            mnu_xml::ascii(child.tag) + " (its type's parse does not); it is left out.");
      continue;
    }
    if (tag_is(child, "STRING")) {
      const bool first_one = singleton(frame, child, "STRING", true);
      read_string(child, w.string_data, !first_one);
    } else if (tag_is(child, "TOGGLE_STRING")) {
      singleton(frame, child, "TOGGLE_STRING", false);
      w.toggle_string.present = true;
      w.toggle_string.type = keyword(child, "TYPE", kStringTypes); // ID alone [orig: @ 0x658170]
      w.toggle_string.value = text_of(child);
    } else if (tag_is(child, "GROUP")) {
      singleton(frame, child, "GROUP", false);
      w.group = number_text(child);
      w.has_group = true;
    } else if (tag_is(child, "ITEMS")) {
      const bool first_one = singleton(frame, child, "ITEMS", true);
      if (!read_items(child, w.items, w.type)) {
        own_stopped = true;
        unread_rest(node, i + 1, list_rule ? kListTags : kTableTags,
                    "Not read: an earlier ITEMS APPEARANCE stopped this widget's list parse in retail.");
      }
      if (!first_one) child.read = false;
    } else if (tag_is(child, "SPINUP")) {
      read_part(child, w.spinup, WindowType::Button, frame);
    } else if (tag_is(child, "SPINDOWN")) {
      read_part(child, w.spindown, WindowType::Button, frame);
    } else if (tag_is(child, "LIST_BOX")) {
      // [orig: CComboWnd_ParseXMLDefinition @ 0x65c0d0] sb_edge_pad is the combo's;
      // every other attribute and the content are the embedded list's.
      if (number_attr(child, "sb_edge_pad", number)) {
        w.sb_edge_pad = number;
        w.has_sb_edge_pad = true;
      }
      read_part(child, w.list_box, WindowType::List, frame);
    } else if (tag_is(child, "SCROLLBAR")) {
      read_part(child, w.scrollbar, WindowType::Scroll, frame);
    } else if (tag_is(child, "MIN_ITEM_HEIGHT")) {
      singleton(frame, child, "MIN_ITEM_HEIGHT", false);
      w.table_data.min_item_height = number_text(child);
      w.table_data.has_min_item_height = true;
      // [orig: CTableWnd_RecalcLayout @ 0x63f1a0, idiv @ 0x63f26b: 0 stays 0, only below 0 is replaced
      //  @ 0x63f1ee]
      if (child.read && w.table_data.min_item_height == 0 && w.type == WindowType::Table)
        crash_note(child, "MIN_ITEM_HEIGHT 0 divides by zero when retail lays the table out.");
    } else if (tag_is(child, "FIXED_HEADER_HEIGHT")) {
      singleton(frame, child, "FIXED_HEADER_HEIGHT", false);
      w.table_data.fixed_header_height = number_text(child);
      w.table_data.has_fixed_header_height = true;
    } else if (tag_is(child, "COLUMN")) {
      if (frame.seen.count("COLUMN")) {
        because(&child, "A repeated COLUMN: retail's result depends on the COUNTs (docs/mnu/menu-re.md); "
                        "only the first COLUMN is kept.");
        continue;
      }
      frame.seen["COLUMN"] = &child;
      read_column(child, w.table_data.column);
    } else if (tag_is(child, "ORIENTATION")) {
      // Only a HORIZONTAL text sets anything and nothing resets it [orig: @ 0x64c745 /
      // 0x64c755]: the model keeps the first HORIZONTAL one, else the first.
      const auto seen = frame.seen.find("ORIENTATION");
      // The model holds what the text decides: HORIZONTAL, or nothing (vertical).
      const auto decided = [this](const Node &n) {
        return iequals(text_of(n), kHorizontalOrientation) ? std::string(kHorizontalOrientation) : std::string();
      };
      if (seen == frame.seen.end()) {
        frame.seen["ORIENTATION"] = &child;
        w.orientation = decided(child);
      } else if (mnu_xml::iequals(child.text, kHorizontalOrientation) &&
                 !mnu_xml::iequals(seen->second->text, kHorizontalOrientation)) {
        seen->second->read = false;
        because(seen->second, "A later HORIZONTAL ORIENTATION decides (retail never resets it to vertical); "
                              "this one is left out.");
        seen->second = &child;
        w.orientation = decided(child);
      } else {
        because(&child, "A repeated ORIENTATION changes nothing in retail (only a HORIZONTAL one sets anything, "
                        "and nothing resets it); it is left out.");
      }
    } else if (tag_is(child, "HEIGHT") || tag_is(child, "WIDTH")) {
      // One slot: the last authored of either [orig: @ 0x64c766 / 0x64c77e].
      singleton(frame, child, "HEIGHT", false);
      w.scroll_extent = number_text(child);
      w.has_scroll_extent = true;
      w.scroll_extent_is_width = tag_is(child, "WIDTH");
    } else if (tag_in(child, kScrollTags)) {
      Appearance row;
      const bool ok = read_appearance(child, row, kAppearanceTypes);
      if (!ok) {
        scroll_stopped = true;
        because(&child, mnu_xml::ascii(child.tag) + " with no STATE retail knows stops the scroll parse in "
                                                   "retail: it and the scroll elements after it are left out.");
        unread_rest(node, i + 1, kScrollTags,
                    "Not read: an earlier scroll part stopped the scroll parse in retail.");
        continue;
      }
      child.read = true;
      if (tag_is(child, "SHUTTLE")) w.shuttle.push_back(std::move(row));
      else if (tag_is(child, "SCROLLUP") || tag_is(child, "SCROLLLEFT")) w.scrollup.push_back(std::move(row));
      else w.scrolldown.push_back(std::move(row));
    } else if (tag_is(child, "DATASOURCE")) {
      // Every one loads and appends its credits [orig: @ 0x65ceeb -> CMarqueeWnd_LoadCreditsFromIni].
      w.datasources.push_back(text_of(child));
    } else if (tag_is(child, "JOIN_BUTTON")) {
      read_part(child, w.join_button, WindowType::Button, frame);
    } else if (tag_is(child, "SEARCH_BUTTON")) {
      read_part(child, w.search_button, WindowType::Button, frame);
    } else if (tag_is(child, "GLB_TABLE") || tag_is(child, "GLB_INFO_PLAYER_DETAILS")) {
      // An embedded table parses the element's children; the browser then reads its HEADERs'
      // FIELDs [orig: @ 0x65dac0; CTableWnd at +184 / +1422 of CLanWnd_Construct @ 0x65ec30].
      const bool saved = glb_header_fields_;
      glb_header_fields_ = true;
      read_part(child, tag_is(child, "GLB_TABLE") ? w.glb_table : w.glb_info_player_details, WindowType::Table,
                frame);
      glb_header_fields_ = saved;
    } else if (tag_is(child, "GLB_INFO_BUTTON")) {
      read_part(child, w.glb_info_button, WindowType::Button, frame);
    } else if (tag_is(child, "GLB_INFO_SERVER_DETAILS")) {
      // A multiline edit parses the children; the browser reads the first FIELDS child's slots.
      read_part(child, w.glb_info_server_details, WindowType::MultilineEdit, frame);
      for (const auto &grand : child.children)
        if (tag_is(*grand, "FIELDS")) {
          read_glb_fields(*grand, w.glb_server_fields);
          break;
        }
    } else if (tag_is(child, "GLB_NW_INFO")) {
      read_part(child, w.glb_nw_info, WindowType::Static, frame);
    } else if (tag_is(child, "TARGET")) {
      if (w.type == WindowType::GlbTable) {
        // The last TARGET's text is the browser's [orig: @ 0x65dac0, self[3668]].
        if (const auto seen = frame.seen.find("TARGET"); seen != frame.seen.end()) {
          seen->second->read = false;
          because(seen->second, "A later TARGET replaces this one (the browser keeps the last); it is left out.");
        }
        frame.seen["TARGET"] = &child;
        w.glb_target = text_of(child);
        w.has_glb_target = true;
      } else {
        // Every one is a target in order [orig: CGopherWnd_ParseXMLDefinition @ 0x65b130].
        w.gopher_targets.push_back(text_of(child));
      }
    } else if (tag_is(child, "FILTERS")) {
      child.read = true;
      for (const auto &grand : child.children) {
        if (!tag_is(*grand, "CATEGORY")) continue;
        GopherCategory category;
        if (read_gopher_category(*grand, category, frame)) w.gopher_categories.push_back(std::move(category));
      }
    } else if (tag_is(child, "GOPHER_BACK")) {
      read_part(child, w.gopher_back, WindowType::Button, frame);
    }
  }
}

// [orig: @ 0x648226] An APPEARANCE needs an attribute and a STATE the parse knows;
// TYPE, MAP_STATE, HEIGHT and FLAGS are optional. The text is the texture or color.
// Every field is read; false when the row is one the parse stops at. The caller
// marks the element read when it keeps the row.
bool TreeReader::read_appearance(const Node &node, Appearance &out, const char *const *types) {
  out.state = keyword(node, "STATE", kAppearanceStates);
  out.type = keyword(node, "TYPE", types);
  int number = 0;
  if (number_attr(node, "MAP_STATE", number)) { out.map_state = number; out.has_map_state = true; }
  if (number_attr(node, "HEIGHT", number)) { out.height = number; out.has_height = true; }
  out.flags = flags_attr(node, "FLAGS");
  // A row of no attribute or of a STATE the parse does not know stops it before its TYPE switch [orig: E_FAIL @
  // 0x648514, the switch @ 0x64853f]: its text is never read. Else the text is a colour for COLOR / OUTLINE
  // (wcstoul @ 0x64864b, @ 0x648562), any other TYPE's texture name.
  const bool kept = !node.attributes.empty() && known_token(out.state, kAppearanceStates);
  if (!appearance_color_type(out.type)) out.value = narrow(node.text);
  else if (kept) out.color = color(node);
  return kept;
}

// [orig: @ 0x648909] STATE and TRIGGER are both required; LOOP is compared and
// has no effect.
bool TreeReader::read_sound(const Node &node, Sound &out) {
  out.state = keyword(node, "STATE", kSoundStates);
  out.trigger = string_attr(node, "TRIGGER");
  for (const Attribute &a : node.attributes)
    if (mnu_xml::iequals(a.name, "LOOP"))
      because(&a, "SOUND LOOP has no effect in retail (the parse compares it and moves on); it is left out.");
  out.file = narrow(node.text);
  return !node.attributes.empty() && known_token(out.state, kSoundStates) && !out.trigger.empty();
}

// [orig: @ 0x648ae6] The edges are child elements read in order with wcstol on the
// text: LEFT/ULX set the left, TOP/ULY the top, WIDTH and HEIGHT add to the left and
// top as they stand. The model writes the same edges back as LEFT/TOP/RIGHT/BOTTOM.
void TreeReader::read_position(const Node &node, Position &pos) {
  node.read = true;
  for (const Attribute &a : node.attributes)
    because(&a, "POSITION attributes are not read (the edges are child elements); " + mnu_xml::ascii(a.name) +
                    " is left out.");
  for (const auto &child_ptr : node.children) {
    const Node &c = *child_ptr;
    if (tag_is(c, "LEFT") || tag_is(c, "ULX")) {
      pos.left = number_text(c);
      pos.has_left = true;
    } else if (tag_is(c, "RIGHT")) {
      pos.right = number_text(c);
      pos.has_right = true;
    } else if (tag_is(c, "TOP") || tag_is(c, "ULY")) {
      pos.top = number_text(c);
      pos.has_top = true;
    } else if (tag_is(c, "BOTTOM")) {
      pos.bottom = number_text(c);
      pos.has_bottom = true;
    } else if (tag_is(c, "WIDTH")) {
      pos.right = pos.left + number_text(c);
      pos.has_right = true;
    } else if (tag_is(c, "HEIGHT")) {
      pos.bottom = pos.top + number_text(c);
      pos.has_bottom = true;
    }
  }
}

// [orig: @ 0x648c75] NAME and the eight colors are child elements; the last of
// each wins.
void TreeReader::read_font(const Node &node, Font &font) {
  node.read = true;
  for (const Attribute &a : node.attributes)
    because(&a, "FONT attributes are not read (NAME and the colors are child elements); " +
                    mnu_xml::ascii(a.name) + " is left out.");
  static const std::pair<const char *, Color Font::*> kColours[] = {
      {"DEFAULT_FG", &Font::default_fg},   {"DEFAULT_BG", &Font::default_bg},   {"DISABLED_FG", &Font::disabled_fg},
      {"DISABLED_BG", &Font::disabled_bg}, {"SELECTED_FG", &Font::selected_fg}, {"SELECTED_BG", &Font::selected_bg},
      {"MOUSEOVER_FG", &Font::mouseover_fg}, {"MOUSEOVER_BG", &Font::mouseover_bg}};
  for (const auto &child_ptr : node.children) {
    const Node &c = *child_ptr;
    if (tag_is(c, "NAME")) {
      font.name = text_of(c);
      continue;
    }
    for (size_t k = 0; k < std::size(kColours); ++k) {
      if (!tag_is(c, kColours[k].first)) continue;
      // A later element of a colour replaces the earlier (of this FONT or an earlier one of the window), and its
      // note.
      const std::pair<const Node *, size_t> key{node.parent, k};
      if (const Node *earlier = font_colour_nodes_[key]) {
        text_notes_.erase(earlier);
        fatal_.erase(earlier);
      }
      font_colour_nodes_[key] = &c;
      c.read = true;
      font.*kColours[k].second = color(c);
    }
  }
}

// [orig: @ 0x64865d] STENCIL (SIZE, INSETX, INSETY), BRUSH and MONOGRAM children.
void TreeReader::read_frame(const Node &node, mnu::Frame &frame) {
  node.read = true;
  for (const Attribute &a : node.attributes)
    because(&a, "FRAME attributes are not read (STENCIL, BRUSH and MONOGRAM are child elements); " +
                    mnu_xml::ascii(a.name) + " is left out.");
  for (const auto &child_ptr : node.children) {
    const Node &c = *child_ptr;
    if (tag_is(c, "STENCIL")) {
      int number = 0;
      if (number_attr(c, "SIZE", number)) { frame.stencil_size = number; frame.has_stencil_size = true; }
      if (number_attr(c, "INSETX", number)) { frame.insetx = number; frame.has_insetx = true; }
      if (number_attr(c, "INSETY", number)) { frame.insety = number; frame.has_insety = true; }
      frame.stencil = text_of(c);
    } else if (tag_is(c, "BRUSH")) {
      frame.brush = text_of(c);
    } else if (tag_is(c, "MONOGRAM")) {
      frame.monogram = text_of(c);
    }
  }
}

// [orig: @ 0x648ee2] The target is the element text; FIELD, SOURCE and NAME share
// one slot; TEST defaults to LT.
void TreeReader::read_action(const Node &node, Action &out) {
  node.read = true;
  out.type = keyword(node, "TYPE", kActionTypes);
  out.file = string_attr(node, "FILE");
  const Attribute *slot = nullptr;
  for (const Attribute &a : node.attributes) {
    if (!in(a.name, kActionFieldAttributes)) continue;
    if (!slot) { slot = &a; continue; }
    Text tok;
    if (!a.token(tok)) empty_value(node, a); // the walk copies every one of them
    else because(&a, "FIELD, SOURCE and NAME write one slot and retail keeps the first authored; " +
                         mnu_xml::ascii(a.name) + " is left out.");
  }
  if (slot) token(node, slot, out.field);
  out.state = keyword(node, "STATE", kActionStates);
  int number = 0;
  if (number_attr(node, "TARGET_FORM", number)) { out.target_form = number; out.has_target_form = true; }
  out.external_browser = flag(node, "EXTERNAL_BROWSER");
  out.toggle = flag(node, "TOGGLE");
  out.test = keyword(node, "TEST", kActionTests);
  if (out.test == kActionTests[0]) { // LT is the parse's 0, as no TEST
    out.test.clear();
    if (const Attribute *test = effective(node, "TEST", kActionTests)) {
      test->read = false;
      because(test, "TEST LT is the parse's 0, as none does; it is left out.");
    }
  }
  for (const Attribute &a : node.attributes)
    if (mnu_xml::iequals(a.name, "TARGET") || mnu_xml::iequals(a.name, "SCREEN") ||
        mnu_xml::iequals(a.name, "WINDOW"))
      because(&a, "ACTION reads its target from the element text; " + mnu_xml::ascii(a.name) +
                      " is left out.");
  out.target = text_of(node);
  // A SCREEN action's FILE is the file activation reads, null with none [orig: PFF_FindEntry @ 0x7685d0, the
  // strncpy @ 0x7685f3]; an empty FILE is empty_value's.
  if (iequals(out.type, "SCREEN") && !node.attr("FILE"))
    crash_note(node, "A SCREEN action with no FILE crashes retail when it is activated (it reads a null file "
                     "name).");
}

// [orig: @ 0x6494f0] FILE and FLAGS children; the last of each wins.
void TreeReader::read_cursor(const Node &node, Cursor &cursor) {
  node.read = true;
  for (const Attribute &a : node.attributes)
    because(&a, "CURSOR attributes are not read (FILE and FLAGS are child elements); " +
                    mnu_xml::ascii(a.name) + " is left out.");
  for (const auto &child_ptr : node.children) {
    const Node &c = *child_ptr;
    if (tag_is(c, "FILE")) {
      cursor.file = text_of(c);
    } else if (tag_is(c, "FLAGS")) {
      cursor.flags = ui_material_flags_of(text_of(c)); // [orig: sub_646CC0 @ 0x649586]
      if (cursor.flags == kDefaultUiMaterialFlags) {
        c.read = false;
        because(&c, "FLAGS reads the default word (STANDARD_TRANSPARENT), as none does; it is left out.");
      }
    }
  }
}

// [orig: CUIButtonWidget_ParseXMLAttributes @ 0x657c30] Each STRING replaces the
// text and resets TYPE; JUSTIFY, VJUSTIFY, EDGE and WRAP set only what it authors.
void TreeReader::read_string(const Node &node, String &s, bool repeated) {
  node.read = true;
  s.present = true;
  s.value = text_of(node);
  s.type = keyword(node, "TYPE", kStringTypes);
  // A keyword sets the justification, another token's wcstol the offset [orig: @ 0x657d62..0x657eba].
  justification(node, "JUSTIFY", kJustify, s.justify, s.justify_offset);
  justification(node, "VJUSTIFY", kVJustify, s.vjustify, s.vjustify_offset);
  int number = 0;
  if (number_attr(node, "EDGE", number)) { s.edge = number; s.has_edge = true; }
  if (flag(node, "WRAP")) s.wrap = true;
  if (repeated) node.read = false; // the merge note covers it
}

void TreeReader::read_item(const Node &node, Item &item, bool cell, WindowType type) {
  node.read = true;
  item.type = keyword(node, "TYPE", item_types_for(type, cell));
  item.value = number_value(node, "VALUE", true);
  justification(node, "JUSTIFY", kJustify, item.justify, item.justify_offset);
  justification(node, "VJUSTIFY", kVJustify, item.vjustify, item.vjustify_offset);
  // PAIRS_LIST a LIST's ITEM alone reads, COLUMN a TABLE cell alone (schema_reads
  // "items.item.pairs_list", "items.item.column", "items.row.item.pairs_list").
  if (!cell && schema_reads(type, "items.item.pairs_list") == SchemaApplies::Reads)
    item.pairs_list = flag(node, "PAIRS_LIST");
  else
    not_read(node, "PAIRS_LIST", type);
  int number = 0;
  if (cell) {
    if (number_attr(node, "COLUMN", number)) { item.column = number; item.has_column = true; }
  } else {
    not_read(node, "COLUMN", type);
  }
  // A spin list's COLOR item's text is a colour (wcstoul base 16 @ 0x64bd10), any other its text.
  if (item_color_type(item.type)) item.color = color(node);
  else item.text = text_of(node);
}

// ITEMS: the LIST form (JUSTIFY, VJUSTIFY, MULTISELECT; ITEM and APPEARANCE rows),
// the SPINLIST form, and the TABLE form (MULTISELECT; ROW > ITEM cells;
// APPEARANCE with IMAGEROW) [orig: @ 0x6457b6, 0x64bd57, 0x642816]. An APPEARANCE
// row with no known STATE ends a LIST's or a TABLE's own parse.
bool TreeReader::read_items(const Node &node, Items &items, WindowType type) {
  const bool list_rule = type == WindowType::List || type == WindowType::LanList;
  const bool table_rule = type == WindowType::Table;
  const auto reads = [type](const char *path) { return schema_reads(type, path) == SchemaApplies::Reads; };
  node.read = true;
  items.present = true;
  if (reads("items.justify")) {
    justification(node, "JUSTIFY", kJustify, items.justify, items.justify_offset);
    justification(node, "VJUSTIFY", kVJustify, items.vjustify, items.vjustify_offset);
  } else {
    not_read(node, "JUSTIFY", type);
    not_read(node, "VJUSTIFY", type);
  }
  if (reads("items.multiselect")) {
    if (flag(node, "MULTISELECT")) items.multiselect = true;
  } else {
    not_read(node, "MULTISELECT", type);
  }
  const std::string kind = strutil::to_upper(window_type_name(type));
  for (size_t i = 0; i < node.children.size(); ++i) {
    const Node &c = *node.children[i];
    // Each form reads its own rows (docs/mnu/menu-re.md, the ITEMS forms): an ITEM
    // directly under ITEMS but on a TABLE, whose ITEMs are ROW cells, the only ones
    // with a COLUMN; APPEARANCE rows on a LIST and a TABLE, not a SPINLIST [orig:
    // CListWnd_ParseXMLDefinition @ 0x645770; CUISpinList_ParseXMLDefinition @ 0x64bd10;
    // CTableWnd_ParseXMLContentDefinition @ 0x6427d0].
    if (tag_is(c, "ITEM")) {
      if (!reads("items.item")) {
        because(&c, "A " + kind + "'s ITEMS reads no ITEM row here" +
                        (table_rule ? " (a TABLE's cells are ITEMs in a ROW)" : "") + "; it is left out.");
        continue;
      }
      Item item;
      read_item(c, item, false, type);
      items.items.push_back(std::move(item));
    } else if (tag_is(c, "ROW")) {
      if (!reads("items.row")) {
        because(&c, "A " + kind + "'s ITEMS reads no ROW (a TABLE's alone does); it is left out.");
        continue;
      }
      c.read = true;
      TableRow row;
      for (const auto &cell_ptr : c.children) {
        if (!tag_is(*cell_ptr, "ITEM")) continue;
        Item cell;
        read_item(*cell_ptr, cell, true, type);
        row.cells.push_back(std::move(cell));
      }
      items.rows.push_back(std::move(row));
    } else if (tag_is(c, "APPEARANCE")) {
      if (!reads("items.appearance")) {
        because(&c, "A " + kind + "'s ITEMS reads no APPEARANCE (a LIST's and a TABLE's do); it is left out.");
        continue;
      }
      Appearance appearance;
      const bool ok = read_appearance(c, appearance, table_rule ? kTableAppearanceTypes : kAppearanceTypes);
      if (!ok) {
        because(&c, "An ITEMS APPEARANCE with no STATE retail knows ends this widget's list parse in retail: "
                    "it and what follows are left out.");
        unread_rest(node, i + 1, nullptr, "Not read: an earlier ITEMS APPEARANCE ended the list parse.");
        return false;
      }
      c.read = true;
      items.appearances.push_back(std::move(appearance));
    }
  }
  return true;
}

// [orig: CTableWnd_ParseXMLContentDefinition @ 0x6430f6] COUNT and SPACING, then
// HEADER / BODY / SUBST rows. The column index is one running value over the rows
// (0 at the COLUMN); a row with no COLUMN takes it, and the model writes it in.
void TreeReader::read_column(const Node &node, TableColumn &column) {
  node.read = true;
  int number = 0;
  // Each COUNT of 1 or more resizes the column array; fewer is refused and the table
  // keeps its one column [orig: resize_column_count @ 0x63f6c0]. The walk (last
  // authored first) leaves the first authored COUNT of 1 or more.
  const Attribute *count = nullptr;
  for (const Attribute &a : node.attributes) {
    Text tok;
    if (mnu_xml::iequals(a.name, "COUNT") && a.token(tok) && mnu_xml::wcstol(tok, 10) >= 1) {
      count = &a;
      break;
    }
  }
  if (!count) count = node.attr("COUNT");
  for (const Attribute &a : node.attributes) {
    if (&a == count || !mnu_xml::iequals(a.name, "COUNT")) continue;
    Text tok;
    if (!a.token(tok)) empty_value(node, a);
    else if (!variable_number(node, &a, tok))
      because(&a, "A repeated COUNT: retail keeps the first of 1 or more; this one is left out.");
  }
  std::string count_text;
  if (count && token(node, count, count_text)) {
    Text tok;
    count->token(tok);
    variable_number(node, count, tok);
    column.count = static_cast<int>(mnu_xml::wcstol(tok, 10));
    column.has_count = true;
  }
  const int columns = column.has_count && column.count >= 1 ? column.count : 1;
  if (number_attr(node, "SPACING", number)) { column.spacing = number; column.has_spacing = true; }
  int running = 0;
  const Attribute *sort_keys[3] = {nullptr, nullptr, nullptr}; // where each table sort key was set
  // The COLUMN (and its HEADER) whose value each sort key took, when one of its HEADER's
  // own COLUMNs set it: a repeated one too, which the model does not keep.
  const Attribute *sort_columns[3] = {nullptr, nullptr, nullptr};
  const Node *sort_headers[3] = {nullptr, nullptr, nullptr};
  for (const auto &child_ptr : node.children) {
    const Node &c = *child_ptr;
    auto read_index = [&](bool &has, int &value) {
      if (number_attr(c, "COLUMN", number)) running = number;
      has = true;
      value = running;
    };
    if (tag_is(c, "HEADER")) {
      c.read = true;
      TableHeader header;
      // The sort keys take the running index as the attribute walk (last authored
      // first) stands when it meets them; the walk reloads it for every attribute and
      // a COLUMN moves it [orig: @ 0x6431f2 / 0x643228; the stores @ 0x643240 ..
      // 0x64329d]. The last write wins.
      int walk_index = running;
      const Attribute *walk_column = nullptr;
      for (auto it = c.attributes.rbegin(); it != c.attributes.rend(); ++it) {
        const Attribute &a = *it;
        Text tok;
        if (mnu_xml::iequals(a.name, "COLUMN") && a.token(tok)) {
          walk_index = static_cast<int>(mnu_xml::wcstol(tok, 10));
          walk_column = &a;
        }
        const int slot = in(a.name, kPrimarySortTokens)                ? 0
                         : mnu_xml::iequals(a.name, "SECONDARY_SORT") ? 1
                         : mnu_xml::iequals(a.name, "TERTIARY_SORT")  ? 2
                                                                      : -1;
        if (slot < 0) continue;
        a.read = true;
        if (sort_keys[slot]) {
          sort_keys[slot]->read = false;
          because(sort_keys[slot], "A later sort key of the same slot replaces this one (retail keeps the last "
                                   "write); it is left out.");
        }
        sort_keys[slot] = &a;
        sort_columns[slot] = walk_column;
        sort_headers[slot] = &c;
        if (slot == 0) {
          column.primary_sort = walk_index;
        } else {
          (slot == 1 ? column.secondary_sort : column.tertiary_sort) = walk_index;
        }
      }
      read_index(header.has_column, header.column);
      header.justify = keyword(c, "JUSTIFY", kJustify);
      header.vjustify = keyword(c, "VJUSTIFY", kVJustify);
      // SORT by its first character: '1' numeric, 'A' or 'a' alphabetic, another nothing [orig:
      // @ 0x6432ca..0x6432dd].
      {
        const Attribute *sort = first(c, "SORT");
        std::string text;
        if (token(c, sort, text)) {
          if (text[0] == '1') header.sort = "1";
          else if (text[0] == 'A' || text[0] == 'a') header.sort = "A";
          else {
            sort->read = false;
            because(sort, "A SORT whose first character is not 1, A or a sets nothing; it is left out.");
          }
        }
      }
      // The browser stores a HEADER's FIELD at its column index alone where 0 <= index < the table's
      // column count [orig: CLanGameBrowser_ParseExtendedXMLDefinition @ 0x65dc50, @ 0x65de8c].
      if (glb_header_fields_) {
        if (header.column >= 0 && header.column < columns) {
          header.field = string_attr(c, "FIELD");
        } else {
          for (const Attribute &a : c.attributes)
            if (mnu_xml::iequals(a.name, "FIELD"))
              because(&a, "The browser reads no FIELD of a HEADER whose COLUMN is below 0 or not below the "
                          "table's column count; it is left out.");
        }
      }
      if (number_attr(c, "WIDTH", number)) { header.width = number; header.has_width = true; }
      // A type="id" HEADER's text is a string id, looked up as the table parses
      // [orig: CUIStringTable_LookupString call @ 0x6434df].
      header.type = keyword(c, "TYPE", kStringTypes);
      header.text = text_of(c);
      // [orig: init_table_row @ 0x63f9c0] sets up only 0 <= index < the column count.
      if (header.column < 0 || header.column >= columns) {
        because(&c, "A HEADER whose COLUMN is below 0 or not below the table's column count (its COUNT, or 1 "
                    "without a COUNT of 1 or more) is not set up by retail.");
        c.read = false;
      }
      column.headers.push_back(std::move(header));
    } else if (tag_is(c, "BODY")) {
      c.read = true;
      TableBody body;
      read_index(body.has_column, body.column);
      justification(c, "JUSTIFY", kJustify, body.justify, body.justify_offset);
      justification(c, "VJUSTIFY", kVJustify, body.vjustify, body.vjustify_offset);
      // The draw kind: the walk's last write, the first authored of the three.
      for (const Attribute &a : c.attributes) {
        const bool custom = mnu_xml::iequals(a.name, kBodyDisplays[0]);
        const bool bitmap = mnu_xml::iequals(a.name, kBodyDisplays[1]);
        const bool bitmap_text = mnu_xml::iequals(a.name, kBodyDisplays[2]);
        if (!custom && !bitmap && !bitmap_text) continue;
        a.read = true;
        if (body.display.empty()) body.display = kBodyDisplays[custom ? 0 : bitmap ? 1 : 2];
        body.custom_draw = body.custom_draw || custom;
        body.bitmap_draw = body.bitmap_draw || bitmap;
        body.bitmap_text = body.bitmap_text || bitmap_text;
      }
      body.scale_bitmap = flag(c, "SCALE_BITMAP");
      body.bitmap_flags = flags_attr(c, "BITMAP_FLAGS");
      column.bodies.push_back(std::move(body));
    } else if (tag_is(c, "SUBST")) {
      c.read = true;
      TableSubst subst;
      read_index(subst.has_column, subst.column);
      subst.value = string_attr(c, "VALUE"); // the token, matched as text [orig: @ 0x6438f1..0x643918]
      subst.is_url = flag(c, "URL");
      subst.is_file = flag(c, "FILE");
      subst.file = text_of(c);
      column.substitutions.push_back(std::move(subst));
    }
  }
  // The walk converts every COLUMN it meets and a sort key keeps the value as it stands
  // [orig: the wcstol @ 0x64321e; the stores @ 0x643240 .. 0x64329d], so a repeated COLUMN
  // (noted, left out) is read when a kept sort key took its value.
  for (int slot = 0; slot < 3; ++slot) {
    if (!sort_columns[slot]) continue;
    Text tok;
    sort_columns[slot]->token(tok);
    variable_number(*sort_headers[slot], sort_columns[slot], tok);
  }
}

// [orig: CLanGameBrowser_ParseXMLFieldDefinitions @ 0x65d850] count (an overwriting walk over the
// attribute list, last authored first, so the first authored; an empty one faults in wcstol), then
// each FORMAT (the last; its \r and \n pairs read as CR and LF) and FIELD (its which, the first
// authored, one past the count reading 0; the last FIELD of a slot kept). A FIELD with no count
// writes through no array, and one at the count past its array: each faults.
void TreeReader::read_glb_fields(const Node &node, GlbFields &fields) {
  node.read = true;
  fields.present = true;
  if (number_attr(node, "count", fields.count)) fields.has_count = true;
  for (const auto &child_ptr : node.children) {
    const Node &c = *child_ptr;
    if (tag_is(c, "FORMAT")) {
      std::string text = text_of(c);
      std::string read;
      for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < text.size() && (text[i + 1] == 'r' || text[i + 1] == 'n')) {
          read += text[i + 1] == 'r' ? '\r' : '\n';
          ++i;
        } else {
          read += text[i];
        }
      }
      fields.format = read;
      fields.has_format = true;
    } else if (tag_is(c, "FIELD")) {
      c.read = true;
      int which = 0;
      number_attr(c, "which", which);
      if (static_cast<unsigned>(which) > static_cast<unsigned>(fields.count)) which = 0;
      if (!fields.has_count || which == fields.count) {
        fatal_because(&c, fields.has_count ? "A FIELD whose which is the count writes past the browser's slots in "
                                             "retail; it is left out."
                                           : "A FIELD before any count writes through no slots in retail; it is left out.");
        c.read = false;
        continue;
      }
      const std::string text = text_of(c);
      auto slot = std::find_if(fields.fields.begin(), fields.fields.end(),
                               [which](const GlbFieldSlot &f) { return f.which == which; });
      if (slot != fields.fields.end()) slot->text = text;
      else fields.fields.push_back({which, text});
    }
  }
}

// [orig: CGopherWnd_ParseCategoryXML @ 0x65a540] LEVEL on the CATEGORY; its children: LABEL (the
// category static parses the element's children), ITEM (a button: NAME, TARGET, FILTER, TABLE, the
// text) and INFO (a static: NAME, FILTER, TABLE, the text); each attribute the first authored with a
// token (first_token). An empty CATEGORY hands the parse no node, which faults.
bool TreeReader::read_gopher_category(const Node &node, GopherCategory &category, ReadScope &owner) {
  if (node.children.empty()) {
    fatal_because(&node, "An empty CATEGORY crashes retail (its parse reads no element); it is left out.");
    return false;
  }
  node.read = true;
  category.level = first_token(node, "LEVEL");
  category.has_level = !category.level.empty();
  for (const auto &child_ptr : node.children) {
    const Node &c = *child_ptr;
    if (tag_is(c, "LABEL")) {
      read_part(c, category.label, WindowType::Static, owner);
    } else if (tag_is(c, "ITEM") || tag_is(c, "INFO")) {
      c.read = true;
      GopherEntry entry;
      entry.info = tag_is(c, "INFO");
      entry.name = first_token(c, "NAME");
      if (!entry.info) entry.target = first_token(c, "TARGET");
      entry.filter = first_token(c, "FILTER");
      entry.table = first_token(c, "TABLE");
      entry.text = text_of(c);
      category.entries.push_back(std::move(entry));
    }
  }
  return true;
}

// The model record a node is in (ParseNote::locator): its top-level SCREEN by its place
// among them, then each WINDOW on the way down the reader created (the ones it read, in
// document order, as the model keeps them) by its place among its owner's created WINDOWs,
// stopping at the first element that is neither (a part's own elements are not windows).
std::string note_locator(const Node &node) {
  std::vector<const Node *> chain;
  for (const Node *n = &node; n; n = n->parent) chain.push_back(n);
  std::reverse(chain.begin(), chain.end());
  if (chain.front()->screen_ordinal < 0) return std::string();
  std::string locator = std::to_string(chain.front()->screen_ordinal);
  for (size_t i = 1; i < chain.size(); ++i) {
    const Node &window = *chain[i];
    if (!tag_is(window, "WINDOW") || !window.read) break;
    size_t index = 0;
    for (const auto &sibling : chain[i - 1]->children) {
      if (sibling.get() == &window) break;
      if (tag_is(*sibling, "WINDOW") && sibling->read) ++index;
    }
    locator += "/window:" + std::to_string(index);
  }
  return locator;
}

// A note covers everything under it; beneath it only a fatal note still comes out (a
// repeated element merged into the first, or a replaced one, is content retail parses).
void TreeReader::sweep(const mnu_xml::Document &xml, std::vector<ParseNote> &notes) const {
  std::function<void(const Node &, bool)> visit = [&](const Node &node, bool covered) {
    const auto text_note = text_notes_.find(&node);
    if (node.read && text_note != text_notes_.end())
      notes.push_back({node.line, mnu_xml::path_key(node), text_note->second, fatal_.count(&node) != 0,
                       note_locator(node)});
    if (!node.read && (!covered || fatal_.count(&node))) {
      const auto reason = reasons_.find(&node);
      notes.push_back({node.line, mnu_xml::path_key(node),
                       reason != reasons_.end()
                           ? reason->second
                           : "<" + mnu_xml::ascii(node.tag) + "> is not read here by retail's parse; it is left out.",
                       fatal_.count(&node) != 0, note_locator(node)});
    }
    covered = covered || !node.read;
    for (const Attribute &a : node.attributes) {
      if (a.read || (a.name.empty() && !a.has_value) || (covered && !fatal_.count(&a))) continue;
      const auto reason = reasons_.find(&a);
      notes.push_back({node.line, mnu_xml::path_key(node, &a),
                       reason != reasons_.end()
                           ? reason->second
                           : mnu_xml::ascii(a.name) + " on <" + mnu_xml::ascii(node.tag) +
                                 "> is not read by retail's parse; it is left out.",
                       fatal_.count(&a) != 0, note_locator(node)});
    }
    for (const auto &child : node.children) visit(*child, covered);
  };
  for (const auto &root : xml.roots) visit(*root, false);
}

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
  // [orig: sub_646CC0 @ 0x646cc0] wcstok on " ,|+"; each token the first row of its name, without case.
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

void decode_source(const uint8_t *data, size_t size, SourceEncoding &encoding, std::u32string &text) {
  encoding = SourceEncoding::CodePage;
  text.clear();
  if (data && size != 0) decode_bytes(data, size, encoding, text);
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
// for a part [orig: CWnd_FindChildByName @ 0x646850].
const Window *find_window_named(const Window &window, const std::string &own, const std::string &name) {
  if (name.empty() || own.empty()) return nullptr;
  if (iequals(name, own)) return &window;
  for (const Window &child : window.children)
    if (const Window *found = find_window_named(child, child.name, name)) return found;
  for (const PartWindow &part : part_windows(window))
    if (const Window *found = find_window_named(*part.window, part.name, name)) return found;
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

bool parse(const std::string &content, Document &out, std::string &error,
           std::vector<ParseNote> *notes) {
  return parse(reinterpret_cast<const uint8_t *>(content.data()),
               content.size(), out, error, notes);
}

bool parse(const uint8_t *data, size_t size, Document &out, std::string &error,
           std::vector<ParseNote> *notes) {
  out = Document{};
  if (notes) notes->clear();
  if (!data && size != 0) {
    error = "MNU input buffer is null";
    return false;
  }
  Text text;
  if (size != 0) decode_bytes(data, size, out.source_encoding, text);
  mnu_xml::Document xml;
  if (!mnu_xml::parse(text, xml, error)) {
    out.screens.clear();
    return false;
  }
  TreeReader reader(out.source_encoding);
  reader.read_document(xml, out);
  if (notes) {
    // The reader's own notes are all retail hangs or overruns.
    for (const mnu_xml::Note &n : xml.notes) notes->push_back({n.line, std::string(), n.message, true});
    reader.sweep(xml, *notes);
  }
  return true;
}

bool parse_file(const std::string &path, Document &out, std::string &error,
                std::vector<ParseNote> *notes) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    error = "Failed to open file: " + path;
    return false;
  }

  std::stringstream buffer;
  buffer << file.rdbuf();
  std::string content = buffer.str();

  return parse(content, out, error, notes);
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
  bool part(const WindowPart &a, const WindowPart &b, const char *name) {
    if (!same(a.present() == b.present(), std::string(name) + " present")) return false;
    if (!a.present()) return true;
    path_.push_back(name);
    const bool ok = window(*a, *b);
    path_.pop_back();
    return ok;
  }
  bool window(const Window &a, const Window &b) {
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
           list(a.children, b.children, "window", [this](const Window &x, const Window &y) { return window(x, y); });
  }
};

}  // namespace

bool equal(const Document &a, const Document &b, std::string *where) { return Same(where).document(a, b); }

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

}  // namespace opennova::mnu
