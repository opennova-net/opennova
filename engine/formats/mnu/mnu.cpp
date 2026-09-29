// MNU menu file parser implementation: the typed layer over mnu_xml's retail
// reader, and the writer. docs/mnu/menu-re.md ("The reader", "The writer") has the
// rules with their witnesses.
#include <formats/mnu/mnu.h>

#include <formats/mns/mns.h>
#include <formats/mnu/mnu_xml.h>
#include <formats/rtxt/rtxt.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
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

// The recognized tokens of each keyword attribute (the element parses' compares).
const char *const kAppearanceStates[] = {"DEFAULT", "DISABLED", "MOUSEOVER", "SELECTED", nullptr};
const char *const kAppearanceTypes[] = {"IMAGE", "COLOR", "CUSTOM", "OUTLINE", nullptr};
const char *const kTableAppearanceTypes[] = {"IMAGE", "IMAGEROW", "COLOR", "CUSTOM", "OUTLINE", nullptr};
const char *const kSoundStates[] = {"MOUSEIN", "MOUSEOUT", "SELECTED", nullptr};
const char *const kActionTests[] = {"LT", "LE", "EQ", "GE", "GT", nullptr};
const char *const kJustify[] = {"LEFT", "CENTER", "RIGHT", nullptr};
const char *const kVJustify[] = {"TOP", "CENTER", "BOTTOM", nullptr};
const char *const kStringTypes[] = {"ID", nullptr};
const char *const kItemTypes[] = {"ID", "IMAGE", "COLOR", "BITMAP", nullptr};

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
  // A keyword attribute's token (effective); empty when absent.
  std::string keyword(const Node &node, const char *name, const char *const *tokens);
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
  // `cell`: a TABLE ROW cell, the only ITEM whose COLUMN retail reads.
  void read_item(const Node &node, Item &item, bool cell);
  // False when a row retail's LIST / TABLE parse stops at was read (`stops` is the
  // window's type rule).
  bool read_items(const Node &node, Items &items, bool list_rule, bool table_rule);
  void read_column(const Node &node, TableColumn &column);
  Element read_extra(const Node &node);
  void unread_rest(const Node &parent, size_t from, const char *const *tags, const std::string &why);

  SourceEncoding encoding_;
  std::map<const void *, std::string> reasons_;
  std::set<const void *> fatal_;
  bool reads_ = true; // retail reads what is being read now
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
  token(node, effective(node, name, tokens), out);
  return out;
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
  out.type = parse_window_type(token_text);
  out.type_token = token_text;
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

// The base parse [orig: CUIElement_ParseXMLDefinition @ 0x648120] then every
// type's own parse, flattened: the model keeps what any type reads.
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
  w.checked = flag(node, "CHECKED") || w.checked;
  w.as_button = flag(node, "AS_BUTTON") || w.as_button;
  w.password = flag(node, "PASSWORD") || w.password;  // [orig: PASSWORD compare @ 0x661d3b]
  w.number = flag(node, "NUMBER") || w.number;
  w.readonly = flag(node, "READONLY") || w.readonly;
  {
    Reads edit(*this, reads_edit_attributes(w.type));
    if (number_attr(node, "MAXCHAR", number)) { w.maxchar = number; w.has_maxchar = true; }
    if (number_attr(node, "MAXVAL", number)) { w.maxval = number; w.has_maxval = true; }
    if (number_attr(node, "MINVAL", number)) { w.minval = number; w.has_minval = true; }
  }
  for (const Attribute &a : node.attributes) {
    if (in(a.name, kExtraAttributes)) {
      a.read = true;
      w.extra_attributes.push_back({mnu_xml::ascii(a.name), std::string(), false});
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
      // VIRTUAL by presence; the text is the key name or its first character, untrimmed.
      Hotkey hotkey;
      hotkey.virtual_key = flag(child, "VIRTUAL");
      hotkey.value = text_of(child);
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
  const bool scroll_rule = w.type == WindowType::Scroll;
  const bool list_rule = w.type == WindowType::List || w.type == WindowType::LanList;
  const bool table_rule = w.type == WindowType::Table;
  bool scroll_stopped = false, own_stopped = false;
  for (size_t i = 0; i < kids.size(); ++i) {
    const Node &child = *kids[i];
    if (tag_in(child, kBaseTags)) continue;
    if ((scroll_stopped && tag_in(child, kScrollTags)) ||
        (own_stopped && tag_in(child, list_rule ? kListTags : kTableTags)))
      continue;
    Reads reads(*this, type_reads(w.type, child));
    if (tag_is(child, "STRING")) {
      const bool first_one = singleton(frame, child, "STRING", true);
      read_string(child, w.string_data, !first_one);
    } else if (tag_is(child, "TOGGLE_STRING")) {
      singleton(frame, child, "TOGGLE_STRING", false);
      w.toggle_string.present = true;
      w.toggle_string.type = keyword(child, "TYPE", kStringTypes);
      w.toggle_string.value = text_of(child);
    } else if (tag_is(child, "GROUP")) {
      singleton(frame, child, "GROUP", false);
      w.group = number_text(child);
      w.has_group = true;
    } else if (tag_is(child, "ITEMS")) {
      const bool first_one = singleton(frame, child, "ITEMS", true);
      if (!read_items(child, w.items, list_rule, table_rule)) {
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
      if (seen == frame.seen.end()) {
        frame.seen["ORIENTATION"] = &child;
        w.orientation = text_of(child);
      } else if (mnu_xml::iequals(child.text, "HORIZONTAL") && !mnu_xml::iequals(seen->second->text, "HORIZONTAL")) {
        seen->second->read = false;
        because(seen->second, "A later HORIZONTAL ORIENTATION decides (retail never resets it to vertical); "
                              "this one is left out.");
        seen->second = &child;
        w.orientation = text_of(child);
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
      if (!ok && scroll_rule) {
        scroll_stopped = true;
        because(&child, mnu_xml::ascii(child.tag) + " with no STATE retail knows stops the scroll parse in "
                                                   "retail: it and the scroll elements after it are left out.");
        unread_rest(node, i + 1, kScrollTags,
                    "Not read: an earlier scroll part stopped the scroll parse in retail.");
        continue;
      }
      // (Not a SCROLL: retail reads none of these; the row is kept as authored.)
      child.read = true;
      if (tag_is(child, "SHUTTLE")) w.shuttle.push_back(std::move(row));
      else if (tag_is(child, "SCROLLUP") || tag_is(child, "SCROLLLEFT")) w.scrollup.push_back(std::move(row));
      else w.scrolldown.push_back(std::move(row));
    } else if (tag_is(child, "DATASOURCE")) {
      // Every one loads and appends its credits [orig: @ 0x65ceeb -> CMarqueeWnd_LoadCreditsFromIni].
      w.datasources.push_back(text_of(child));
    } else if (tag_in(child, kExtraTags)) {
      w.extras.push_back(read_extra(child));
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
  out.flags = string_attr(node, "FLAGS");
  out.value = narrow(node.text);
  return !node.attributes.empty() && known_token(out.state, kAppearanceStates);
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
  for (const auto &child_ptr : node.children) {
    const Node &c = *child_ptr;
    std::string *slot = nullptr;
    if (tag_is(c, "NAME")) slot = &font.name;
    else if (tag_is(c, "DEFAULT_FG")) slot = &font.default_fg;
    else if (tag_is(c, "DEFAULT_BG")) slot = &font.default_bg;
    else if (tag_is(c, "DISABLED_FG")) slot = &font.disabled_fg;
    else if (tag_is(c, "DISABLED_BG")) slot = &font.disabled_bg;
    else if (tag_is(c, "SELECTED_FG")) slot = &font.selected_fg;
    else if (tag_is(c, "SELECTED_BG")) slot = &font.selected_bg;
    else if (tag_is(c, "MOUSEOVER_FG")) slot = &font.mouseover_fg;
    else if (tag_is(c, "MOUSEOVER_BG")) slot = &font.mouseover_bg;
    if (slot) *slot = text_of(c);
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
    const bool shares = mnu_xml::iequals(a.name, "FIELD") || mnu_xml::iequals(a.name, "SOURCE") ||
                        mnu_xml::iequals(a.name, "NAME");
    if (!shares) continue;
    if (!slot) { slot = &a; continue; }
    Text tok;
    if (!a.token(tok)) empty_value(node, a); // the walk copies every one of them
    else because(&a, "FIELD, SOURCE and NAME write one slot and retail keeps the first authored; " +
                         mnu_xml::ascii(a.name) + " is left out.");
  }
  if (slot && token(node, slot, out.field)) out.field_attr = strutil::to_upper(mnu_xml::ascii(slot->name));
  out.state = keyword(node, "STATE", kActionStates);
  int number = 0;
  if (number_attr(node, "TARGET_FORM", number)) { out.target_form = number; out.has_target_form = true; }
  out.external_browser = flag(node, "EXTERNAL_BROWSER");
  out.toggle = flag(node, "TOGGLE");
  out.test = keyword(node, "TEST", kActionTests);
  for (const Attribute &a : node.attributes)
    if (mnu_xml::iequals(a.name, "TARGET") || mnu_xml::iequals(a.name, "SCREEN") ||
        mnu_xml::iequals(a.name, "WINDOW"))
      because(&a, "ACTION reads its target from the element text; " + mnu_xml::ascii(a.name) +
                      " is left out.");
  out.target = text_of(node);
}

// [orig: @ 0x6494f0] FILE and FLAGS children; the last of each wins.
void TreeReader::read_cursor(const Node &node, Cursor &cursor) {
  node.read = true;
  for (const Attribute &a : node.attributes)
    because(&a, "CURSOR attributes are not read (FILE and FLAGS are child elements); " +
                    mnu_xml::ascii(a.name) + " is left out.");
  for (const auto &child_ptr : node.children) {
    const Node &c = *child_ptr;
    if (tag_is(c, "FILE")) cursor.file = text_of(c);
    else if (tag_is(c, "FLAGS")) cursor.flags = text_of(c);
  }
}

// [orig: CUIButtonWidget_ParseXMLAttributes @ 0x657c30] Each STRING replaces the
// text and resets TYPE; JUSTIFY, VJUSTIFY, EDGE and WRAP set only what it authors.
void TreeReader::read_string(const Node &node, String &s, bool repeated) {
  node.read = true;
  s.present = true;
  s.value = text_of(node);
  s.type = keyword(node, "TYPE", kStringTypes);
  if (node.attr("JUSTIFY")) s.justify = keyword(node, "JUSTIFY", kJustify);
  if (node.attr("VJUSTIFY")) s.vjustify = keyword(node, "VJUSTIFY", kVJustify);
  int number = 0;
  if (number_attr(node, "EDGE", number)) { s.edge = number; s.has_edge = true; }
  if (flag(node, "WRAP")) s.wrap = true;
  if (repeated) node.read = false; // the merge note covers it
}

void TreeReader::read_item(const Node &node, Item &item, bool cell) {
  node.read = true;
  item.type = keyword(node, "TYPE", kItemTypes);
  item.value = string_attr(node, "VALUE");
  item.justify = keyword(node, "JUSTIFY", kJustify);
  item.vjustify = keyword(node, "VJUSTIFY", kVJustify);
  item.pairs_list = flag(node, "PAIRS_LIST");
  int number = 0;
  Reads column(*this, cell);
  if (number_attr(node, "COLUMN", number)) { item.column = number; item.has_column = true; }
  item.text = text_of(node);
}

// ITEMS: the LIST form (JUSTIFY, VJUSTIFY, MULTISELECT; ITEM and APPEARANCE rows),
// the SPINLIST form, and the TABLE form (MULTISELECT; ROW > ITEM cells;
// APPEARANCE with IMAGEROW) [orig: @ 0x6457b6, 0x64bd57, 0x642816]. An APPEARANCE
// row with no known STATE ends a LIST's or a TABLE's own parse.
bool TreeReader::read_items(const Node &node, Items &items, bool list_rule, bool table_rule) {
  node.read = true;
  items.present = true;
  if (node.attr("JUSTIFY")) items.justify = keyword(node, "JUSTIFY", kJustify);
  if (node.attr("VJUSTIFY")) items.vjustify = keyword(node, "VJUSTIFY", kVJustify);
  if (flag(node, "MULTISELECT")) items.multiselect = true;
  for (size_t i = 0; i < node.children.size(); ++i) {
    const Node &c = *node.children[i];
    // Each form reads its own rows (docs/mnu/menu-re.md, the ITEMS forms): an ITEM
    // directly under ITEMS but on a TABLE, whose ITEMs are ROW cells, the only ones
    // with a COLUMN; APPEARANCE rows on a LIST and a TABLE, not a SPINLIST [orig:
    // CListWnd_ParseXMLDefinition @ 0x645770; CUISpinList_ParseXMLDefinition @ 0x64bd10;
    // CTableWnd_ParseXMLContentDefinition @ 0x6427d0].
    if (tag_is(c, "ITEM")) {
      Reads reads(*this, !table_rule);
      Item item;
      read_item(c, item, false);
      items.items.push_back(std::move(item));
    } else if (tag_is(c, "ROW")) {
      Reads reads(*this, table_rule);
      c.read = true;
      TableRow row;
      for (const auto &cell_ptr : c.children) {
        if (!tag_is(*cell_ptr, "ITEM")) continue;
        Item cell;
        read_item(*cell_ptr, cell, true);
        row.cells.push_back(std::move(cell));
      }
      items.rows.push_back(std::move(row));
    } else if (tag_is(c, "APPEARANCE")) {
      Reads reads(*this, list_rule || table_rule);
      Appearance appearance;
      const bool ok = read_appearance(c, appearance, table_rule ? kTableAppearanceTypes : kAppearanceTypes);
      if (!ok && (list_rule || table_rule)) {
        because(&c, "An ITEMS APPEARANCE with no STATE retail knows ends this widget's list parse in retail: "
                    "it and what follows are left out.");
        unread_rest(node, i + 1, nullptr, "Not read: an earlier ITEMS APPEARANCE ended the list parse.");
        return false;
      }
      // (Another type reads no ITEMS APPEARANCE: the row is kept as authored.)
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
        const int slot = mnu_xml::iequals(a.name, "DEFAULT_SORT") || mnu_xml::iequals(a.name, "PRIMARY_SORT") ? 0
                         : mnu_xml::iequals(a.name, "SECONDARY_SORT")                                         ? 1
                         : mnu_xml::iequals(a.name, "TERTIARY_SORT")                                          ? 2
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
          column.primary_sort_token = strutil::to_upper(mnu_xml::ascii(a.name));
        } else {
          (slot == 1 ? column.secondary_sort : column.tertiary_sort) = walk_index;
        }
      }
      read_index(header.has_column, header.column);
      header.justify = keyword(c, "JUSTIFY", kJustify);
      header.vjustify = keyword(c, "VJUSTIFY", kVJustify);
      header.sort = string_attr(c, "SORT");
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
      body.justify = keyword(c, "JUSTIFY", kJustify);
      body.vjustify = keyword(c, "VJUSTIFY", kVJustify);
      // The draw kind: the walk's last write, the first authored of the three.
      for (const Attribute &a : c.attributes) {
        const bool custom = mnu_xml::iequals(a.name, "CUSTOM_DRAW");
        const bool bitmap = mnu_xml::iequals(a.name, "BITMAP_DRAW");
        const bool bitmap_text = mnu_xml::iequals(a.name, "BITMAP_TEXT");
        if (!custom && !bitmap && !bitmap_text) continue;
        a.read = true;
        if (body.display.empty()) body.display = custom ? "CUSTOM_DRAW" : bitmap ? "BITMAP_DRAW" : "BITMAP_TEXT";
        body.custom_draw = body.custom_draw || custom;
        body.bitmap_draw = body.bitmap_draw || bitmap;
        body.bitmap_text = body.bitmap_text || bitmap_text;
      }
      body.scale_bitmap = flag(c, "SCALE_BITMAP");
      body.bitmap_flags = string_attr(c, "BITMAP_FLAGS");
      column.bodies.push_back(std::move(body));
    } else if (tag_is(c, "SUBST")) {
      c.read = true;
      TableSubst subst;
      read_index(subst.has_column, subst.column);
      subst.value = string_attr(c, "VALUE");
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

Element TreeReader::read_extra(const Node &node) {
  node.read = true;
  Element element;
  element.tag = strutil::to_upper(mnu_xml::ascii(node.tag));
  element.text = narrow(node.text);
  for (const Attribute &a : node.attributes) {
    a.read = true;
    if (a.name.empty() && !a.has_value) continue;
    ElementAttribute attr;
    attr.name = narrow(a.name);
    Text tok;
    attr.has_value = a.has_value;
    if (a.token(tok)) attr.value = narrow(tok);
    element.attributes.push_back(std::move(attr));
  }
  for (const auto &child : node.children) element.children.push_back(read_extra(*child));
  return element;
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

namespace {

// Where the writer puts a table sort key so that retail's HEADER walk sets it to the
// model's index: on the first HEADER whose index is that value, before its COLUMN
// (`own`), else after the COLUMN of the first HEADER the running index reaches it
// before (the HEADERs are written first in the COLUMN element, so the index starts at
// 0). `placed` is false when no HEADER reaches the value.
struct SortKeyPlace {
  bool slot = false;   // the key is set
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

class Writer {
public:
  Writer(bool pretty, int indent_size) : pretty_(pretty), indent_(indent_size) {}
  std::string out;

  void screen(const Screen &s);

private:
  void line(int depth, const std::string &text) {
    if (pretty_) out.append(static_cast<size_t>(depth * indent_), ' ');
    out += text;
    if (pretty_) out.push_back('\n');
  }
  // An element with text only, never self-closed.
  void leaf(int depth, const std::string &tag, const std::string &attrs, const std::string &text) {
    line(depth, "<" + tag + attrs + ">" + escape_text(text) + "</" + tag + ">");
  }
  static std::string attr(const char *name, const std::string &value) {
    if (value.empty()) return "";
    return std::string(" ") + name + "=\"" + value + "\"";
  }
  static std::string attr_int(const char *name, bool has, int value) {
    return has ? std::string(" ") + name + "=\"" + std::to_string(value) + "\"" : std::string();
  }
  static std::string bare(const char *name, bool on) { return on ? std::string(" ") + name : std::string(); }

  void window(const Window &w, int depth, const char *tag, bool part, const std::string &extra = std::string());

public:
  void window_elements(const Window &w, int depth);

private:
  void position(const Position &pos, int depth);
  void appearance(const Appearance &a, const char *tag, int depth);
  void items(const Items &items, int depth);
  void item(const Item &item, int depth);
  void column(const TableColumn &column, int depth);
  void element(const Element &e, int depth);

  bool pretty_;
  int indent_;
};

void Writer::position(const Position &pos, int depth) {
  if (!pos.has_left && !pos.has_top && !pos.has_right && !pos.has_bottom) return;
  line(depth, "<POSITION>");
  if (pos.has_left) leaf(depth + 1, "LEFT", "", std::to_string(pos.left));
  if (pos.has_top) leaf(depth + 1, "TOP", "", std::to_string(pos.top));
  if (pos.has_right) leaf(depth + 1, "RIGHT", "", std::to_string(pos.right));
  if (pos.has_bottom) leaf(depth + 1, "BOTTOM", "", std::to_string(pos.bottom));
  line(depth, "</POSITION>");
}

bool empty_row(const Appearance &a) {
  return a.state.empty() && a.type.empty() && a.value.empty() && !a.has_map_state && !a.has_height && a.flags.empty();
}

void Writer::appearance(const Appearance &a, const char *tag, int depth) {
  if (empty_row(a)) return; // nothing to write (an element with no attributes stops the parse)
  std::string attrs = attr("type", a.type) + attr("state", a.state);
  attrs += attr_int("map_state", a.has_map_state, a.map_state);
  attrs += attr_int("height", a.has_height, a.height);
  attrs += attr("flags", a.flags);
  leaf(depth, tag, attrs, a.value);
}

void Writer::item(const Item &i, int depth) {
  std::string attrs = attr("type", i.type) + attr("value", i.value) + attr("justify", i.justify) +
                      attr("vjustify", i.vjustify) + bare("PAIRS_LIST", i.pairs_list) +
                      attr_int("column", i.has_column, i.column);
  leaf(depth, "ITEM", attrs, i.text);
}

void Writer::items(const Items &items, int depth) {
  if (!items.present) return;
  line(depth, "<ITEMS" + attr("justify", items.justify) + attr("vjustify", items.vjustify) +
                  bare("MULTISELECT", items.multiselect) + ">");
  for (const Appearance &a : items.appearances) appearance(a, "APPEARANCE", depth + 1);
  for (const Item &i : items.items) item(i, depth + 1);
  for (const TableRow &row : items.rows) {
    line(depth + 1, "<ROW>");
    for (const Item &cell : row.cells) item(cell, depth + 2);
    line(depth + 1, "</ROW>");
  }
  line(depth, "</ITEMS>");
}

void Writer::column(const TableColumn &c, int depth) {
  const std::vector<SortKeyPlace> places = sort_key_places(c);
  const bool keys = places[0].slot || places[1].slot || places[2].slot;
  if (!c.has_count && !c.has_spacing && c.headers.empty() && c.bodies.empty() && c.substitutions.empty() && !keys)
    return;
  line(depth, "<COLUMN" + attr_int("count", c.has_count, c.count) + attr_int("spacing", c.has_spacing, c.spacing) +
                  ">");
  const char *const key_tokens[] = {c.primary_sort_token.empty() ? "PRIMARY_SORT" : c.primary_sort_token.c_str(),
                                    "SECONDARY_SORT", "TERTIARY_SORT"};
  for (size_t i = 0; i < c.headers.size(); ++i) {
    const TableHeader &h = c.headers[i];
    // A sort key written before COLUMN takes this HEADER's index, one written after it
    // the index before (the walk runs last authored first).
    std::string before, after;
    for (int k = 0; k < 3; ++k)
      if (places[k].slot && places[k].placed && places[k].header == i)
        (places[k].own ? before : after) += std::string(" ") + key_tokens[k];
    std::string attrs = attr("justify", h.justify) + attr("vjustify", h.vjustify) + before +
                        attr_int("column", h.has_column, h.column) + after + attr("sort", h.sort) +
                        attr_int("width", h.has_width, h.width) + attr("type", h.type);
    leaf(depth + 1, "HEADER", attrs, h.text);
  }
  for (const TableBody &b : c.bodies) {
    std::string attrs = attr("justify", b.justify) + attr("vjustify", b.vjustify) +
                        attr_int("column", b.has_column, b.column);
    // The draw kind retail keeps is the first authored of the three: written first.
    const char *kinds[] = {"CUSTOM_DRAW", "BITMAP_DRAW", "BITMAP_TEXT"};
    const bool on[] = {b.custom_draw, b.bitmap_draw, b.bitmap_text};
    for (int k = 0; k < 3; ++k)
      if (on[k] && iequals(b.display, kinds[k])) attrs += std::string(" ") + kinds[k];
    for (int k = 0; k < 3; ++k)
      if (on[k] && !iequals(b.display, kinds[k])) attrs += std::string(" ") + kinds[k];
    attrs += attr("BITMAP_FLAGS", b.bitmap_flags) + bare("SCALE_BITMAP", b.scale_bitmap);
    line(depth + 1, "<BODY" + attrs + "></BODY>");
  }
  for (const TableSubst &s : c.substitutions)
    leaf(depth + 1, "SUBST",
         attr_int("column", s.has_column, s.column) + attr("value", s.value) + bare("URL", s.is_url) +
             bare("FILE", s.is_file),
         s.file);
  line(depth, "</COLUMN>");
}

bool is_space_text(const std::string &text) {
  return std::all_of(text.begin(), text.end(), [](char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
  });
}

void Writer::element(const Element &e, int depth) {
  std::string attrs;
  for (const ElementAttribute &a : e.attributes)
    attrs += a.has_value ? " " + a.name + "=\"" + a.value + "\"" : " " + a.name;
  if (e.children.empty()) {
    leaf(depth, e.tag, attrs, e.text);
    return;
  }
  if (!is_space_text(e.text)) {
    // Text beside child elements: written compact, so no layout whitespace joins it.
    Writer inner(false, 0);
    for (const Element &child : e.children) inner.element(child, 0);
    line(depth, "<" + e.tag + attrs + ">" + escape_text(e.text) + inner.out + "</" + e.tag + ">");
    return;
  }
  line(depth, "<" + e.tag + attrs + ">");
  for (const Element &child : e.children) element(child, depth + 1);
  line(depth, "</" + e.tag + ">");
}

void Writer::window(const Window &w, int depth, const char *tag, bool part, const std::string &extra) {
  std::string attrs = extra;
  if (!part)
    attrs += attr("type", w.type_token.empty() ? window_type_name(w.type) : w.type_token);
  attrs += attr("name", w.name);
  attrs += bare("DRAW_FRAME", w.draw_frame) + bare("HIDDEN", w.hidden) + bare("MODAL", w.modal) +
           bare("READONLY", w.readonly) + bare("DISABLE", w.disabled) + bare("CHECKED", w.checked) +
           bare("AS_BUTTON", w.as_button) + bare("NUMBER", w.number);
  attrs += attr_int("MINVAL", w.has_minval, w.minval) + attr_int("MAXVAL", w.has_maxval, w.maxval) +
           attr_int("MAXCHAR", w.has_maxchar, w.maxchar);
  attrs += bare("GLOBAL_VAR", w.global_var) + bare("PASSWORD", w.password) +
           attr_int("FORM", w.has_form, w.form);
  for (const ElementAttribute &a : w.extra_attributes)
    attrs += a.has_value ? " " + a.name + "=\"" + a.value + "\"" : " " + a.name;
  line(depth, "<" + std::string(tag) + attrs + ">");
  window_elements(w, depth + 1);
  line(depth, "</" + std::string(tag) + ">");
}

void Writer::window_elements(const Window &w, int depth) {
  if (w.has_group) leaf(depth, "GROUP", "", std::to_string(w.group));
  for (const Hotkey &h : w.hotkeys) leaf(depth, "HOTKEY", bare("VIRTUAL", h.virtual_key), h.value);
  for (const Action &a : w.actions) {
    std::string attrs = attr("type", a.type) + attr("state", a.state) + attr("file", a.file);
    if (!a.field.empty())
      attrs += " " + (a.field_attr.empty() ? std::string("FIELD") : a.field_attr) + "=\"" + a.field + "\"";
    attrs += attr_int("target_form", a.has_target_form, a.target_form) + bare("TOGGLE", a.toggle) +
             attr("test", a.test) + bare("EXTERNAL_BROWSER", a.external_browser);
    leaf(depth, "ACTION", attrs, a.target);
  }
  const Frame &f = w.frame;
  const bool has_stencil = !f.stencil.empty() || f.has_stencil_size || f.has_insetx || f.has_insety;
  if (has_stencil || !f.brush.empty() || !f.monogram.empty()) {
    line(depth, "<FRAME>");
    if (has_stencil)
      leaf(depth + 1, "STENCIL",
           attr_int("size", f.has_stencil_size, f.stencil_size) + attr_int("insetx", f.has_insetx, f.insetx) +
               attr_int("insety", f.has_insety, f.insety),
           f.stencil);
    if (!f.brush.empty()) leaf(depth + 1, "BRUSH", "", f.brush);
    if (!f.monogram.empty()) leaf(depth + 1, "MONOGRAM", "", f.monogram);
    line(depth, "</FRAME>");
  }
  position(w.position, depth);
  if (w.has_scroll_extent)
    leaf(depth, w.scroll_extent_is_width ? "WIDTH" : "HEIGHT", "", std::to_string(w.scroll_extent));
  if (!w.orientation.empty()) leaf(depth, "ORIENTATION", "", w.orientation);
  for (const Appearance &a : w.appearances) appearance(a, "APPEARANCE", depth);
  for (const Appearance &a : w.shuttle) appearance(a, "SHUTTLE", depth);
  for (const Appearance &a : w.scrollup) appearance(a, "SCROLLUP", depth);
  for (const Appearance &a : w.scrolldown) appearance(a, "SCROLLDOWN", depth);
  if (w.has_text_rsrc) leaf(depth, "TEXT_RSRC", "", w.text_rsrc);
  for (const std::string &source : w.datasources) leaf(depth, "DATASOURCE", "", source);
  if (!w.private_data.empty()) leaf(depth, "PRIVATE_DATA", "", w.private_data);
  if (!w.cursor.file.empty() || !w.cursor.flags.empty()) {
    line(depth, "<CURSOR>");
    if (!w.cursor.file.empty()) leaf(depth + 1, "FILE", "", w.cursor.file);
    if (!w.cursor.flags.empty()) leaf(depth + 1, "FLAGS", "", w.cursor.flags);
    line(depth, "</CURSOR>");
  }
  const Font &font = w.font;
  if (!font.empty()) {
    line(depth, "<FONT>");
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
      if (!slot.second->empty()) leaf(depth + 1, slot.first, "", *slot.second);
    line(depth, "</FONT>");
  }
  const String &s = w.string_data;
  if (s.present)
    leaf(depth, "STRING",
         attr("type", s.type) + attr("justify", s.justify) + attr("vjustify", s.vjustify) +
             attr_int("edge", s.has_edge, s.edge) + bare("WRAP", s.wrap),
         s.value);
  if (w.toggle_string.present) leaf(depth, "TOGGLE_STRING", attr("type", w.toggle_string.type), w.toggle_string.value);
  for (const Sound &snd : w.sounds)
    leaf(depth, "SOUND", attr("state", snd.state) + attr("trigger", snd.trigger), snd.file);
  items(w.items, depth);
  // The combo's sb_edge_pad rides on the LIST_BOX element with the list's own attributes.
  if (w.list_box)
    window(*w.list_box, depth, "LIST_BOX", true, attr_int("sb_edge_pad", w.has_sb_edge_pad, w.sb_edge_pad));
  if (w.spinup) window(*w.spinup, depth, "SPINUP", true);
  if (w.spindown) window(*w.spindown, depth, "SPINDOWN", true);
  column(w.table_data.column, depth);
  if (w.table_data.has_min_item_height) leaf(depth, "MIN_ITEM_HEIGHT", "", std::to_string(w.table_data.min_item_height));
  if (w.table_data.has_fixed_header_height)
    leaf(depth, "FIXED_HEADER_HEIGHT", "", std::to_string(w.table_data.fixed_header_height));
  if (w.scrollbar) window(*w.scrollbar, depth, "SCROLLBAR", true);
  for (const Element &e : w.extras) element(e, depth);
  for (const Window &child : w.children) window(child, depth, "WINDOW", false);
}

void Writer::screen(const Screen &s) {
  line(0, "<SCREEN>");
  if (!s.name.empty()) leaf(1, "NAME", "", s.name);
  if (s.has_music_var) leaf(1, "MUSICVAR", "", std::to_string(s.music_var));
  for (const Window &root : s.roots) window(root, 1, "WINDOW", false);
  line(0, "</SCREEN>");
}

// --- write issues ------------------------------------------------------------

bool has_quote(const std::string &value) {
  return value.find('"') != std::string::npos || value.find('\'') != std::string::npos;
}

bool known(const std::string &value, const char *const *tokens) { return known_token(value, tokens); }

// Whether the writer puts any child element in a window (retail creates no WINDOW
// without one; a part without one crashes it).
bool writes_elements(const Window &w) {
  Writer probe(false, 0);
  probe.window_elements(w, 0);
  return !probe.out.empty();
}

// Each issue names the record that holds the value by the property table's list paths
// and the field by its path (formats/mnu/mnu_schema.h), so the editor shows it on the
// field that causes it.
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

}  // namespace

std::vector<WriteIssue> write_issues(const Document &doc) {
  std::vector<WriteIssue> issues;
  IssueCheck check(issues);
  for (size_t i = 0; i < doc.screens.size(); ++i) check.screen(doc.screens[i], i);
  return issues;
}

std::string serialize(const Document &doc, bool pretty, int indent_size) {
  Writer writer(pretty, indent_size);
  for (const auto &screen : doc.screens) writer.screen(screen);
  return writer.out;
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

}  // namespace opennova::mnu
