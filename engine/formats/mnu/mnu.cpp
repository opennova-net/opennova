// MNU menu file parser implementation: the typed layer over mnu_xml's retail
// reader, which names each record's element for the text layout (mnu_text_layout.h);
// the writer is mnu_write.cpp. docs/mnu/menu-re.md ("The reader", "The writer") has the
// rules with their witnesses.
#include <formats/mnu/mnu.h>

#include <formats/mns/mns.h>
#include <formats/mnu/mnu_text_layout.h>
#include <formats/mnu/mnu_write.h>
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
  TreeReader(SourceEncoding encoding, TextLayoutCapture &capture) : encoding_(encoding), capture_(capture) {}

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
  void read_part(const Node &node, WindowPart &part, WindowType type, ReadScope &owner, const char *key);
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
  // `by`: the element whose parse stopped (the text layout keeps the rest while it stands as read).
  void unread_rest(const Node &parent, size_t from, const char *const *tags, const std::string &why, const Node &by);

  SourceEncoding encoding_;
  // Told which element each record and singleton was read from, and which elements are read
  // for nothing only while another stands (the text layout's Tied tokens).
  TextLayoutCapture &capture_;
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
  capture_.tie(*it->second, node);
  it->second = &node;
  return false;
}

// Marks the elements of `tags` from `from` on as not read, with `why`.
void TreeReader::unread_rest(const Node &parent, size_t from, const char *const *tags, const std::string &why,
                             const Node &by) {
  for (size_t i = from; i < parent.children.size(); ++i) {
    const Node &child = *parent.children[i];
    if (tags && !tag_in(child, tags)) continue;
    child.read = false;
    because(&child, why);
    capture_.tie(child, by);
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
    screen.source = capture_.record(*root);
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
      capture_.keep(child, "NAME");
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
      capture_.keep(child, "MUSICVAR");
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
  out.source = capture_.record(node);
  ReadScope frame;
  read_window_body(node, out, frame);
  return true;
}

// A part's own embedded widget parses the element's children with its own chain
// (the attributes are the element's); an empty one hands the parse no node, which
// faults. A repeated part element parses into the same widget.
void TreeReader::read_part(const Node &node, WindowPart &part, WindowType type, ReadScope &owner, const char *key) {
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
  capture_.keep(node, key);
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
        capture_.tie(child, node);
        unread_rest(node, i + 1, kBaseTags,
                    "Not read: an earlier APPEARANCE or SOUND stopped this window's parse in retail.", node);
        continue;
      }
      child.read = true;
      appearance.source = capture_.record(child);
      w.appearances.push_back(std::move(appearance));
    } else if (tag_is(child, "SOUND")) {
      Sound sound;
      if (!read_sound(child, sound)) {
        stopped = true;
        windows.clear();
        because(&child, "A SOUND with no STATE retail knows (MOUSEIN, MOUSEOUT, SELECTED) or no TRIGGER stops "
                        "this window's parse in retail: it, the elements after it and the child windows are "
                        "left out.");
        capture_.tie(child, node);
        unread_rest(node, i + 1, kBaseTags,
                    "Not read: an earlier APPEARANCE or SOUND stopped this window's parse in retail.", node);
        continue;
      }
      child.read = true;
      sound.source = capture_.record(child);
      w.sounds.push_back(std::move(sound));
    } else if (tag_is(child, "FRAME")) {
      const bool first_one = singleton(frame, child, "FRAME", true);
      capture_.keep(child, "FRAME");
      read_frame(child, w.frame);
      if (!first_one) child.read = false;
    } else if (tag_is(child, "POSITION")) {
      const bool first_one = singleton(frame, child, "POSITION", true);
      capture_.keep(child, "POSITION");
      read_position(child, w.position);
      if (!first_one) child.read = false;
    } else if (tag_is(child, "FONT")) {
      const bool first_one = singleton(frame, child, "FONT", true);
      capture_.keep(child, "FONT");
      read_font(child, w.font);
      if (!first_one) child.read = false;
    } else if (tag_is(child, "TEXT_RSRC")) {
      singleton(frame, child, "TEXT_RSRC", false);
      capture_.keep(child, "TEXT_RSRC");
      w.text_rsrc = text_of(child);
      w.has_text_rsrc = true;
    } else if (tag_is(child, "ACTION")) {
      Action action;
      read_action(child, action);
      action.source = capture_.record(child);
      w.actions.push_back(std::move(action));
    } else if (tag_is(child, "CURSOR")) {
      const bool first_one = singleton(frame, child, "CURSOR", true);
      capture_.keep(child, "CURSOR");
      read_cursor(child, w.cursor);
      if (!first_one) child.read = false;
    } else if (tag_is(child, "HOTKEY")) {
      // VIRTUAL by presence; the text is the key name or its first character, untrimmed.
      Hotkey hotkey;
      hotkey.virtual_key = flag(child, "VIRTUAL");
      hotkey.value = text_of(child);
      hotkey.source = capture_.record(child);
      w.hotkeys.push_back(std::move(hotkey));
    } else if (tag_is(child, "PRIVATE_DATA")) {
      singleton(frame, child, "PRIVATE_DATA", false);
      capture_.keep(child, "PRIVATE_DATA");
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
        capture_.tie(*child, node);
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
      capture_.keep(child, "STRING");
      read_string(child, w.string_data, !first_one);
    } else if (tag_is(child, "TOGGLE_STRING")) {
      singleton(frame, child, "TOGGLE_STRING", false);
      capture_.keep(child, "TOGGLE_STRING");
      w.toggle_string.present = true;
      w.toggle_string.type = keyword(child, "TYPE", kStringTypes);
      w.toggle_string.value = text_of(child);
    } else if (tag_is(child, "GROUP")) {
      singleton(frame, child, "GROUP", false);
      capture_.keep(child, "GROUP");
      w.group = number_text(child);
      w.has_group = true;
    } else if (tag_is(child, "ITEMS")) {
      const bool first_one = singleton(frame, child, "ITEMS", true);
      capture_.keep(child, "ITEMS");
      if (!read_items(child, w.items, list_rule, table_rule)) {
        own_stopped = true;
        unread_rest(node, i + 1, list_rule ? kListTags : kTableTags,
                    "Not read: an earlier ITEMS APPEARANCE stopped this widget's list parse in retail.", child);
      }
      if (!first_one) child.read = false;
    } else if (tag_is(child, "SPINUP")) {
      read_part(child, w.spinup, WindowType::Button, frame, "SPINUP");
    } else if (tag_is(child, "SPINDOWN")) {
      read_part(child, w.spindown, WindowType::Button, frame, "SPINDOWN");
    } else if (tag_is(child, "LIST_BOX")) {
      // [orig: CComboWnd_ParseXMLDefinition @ 0x65c0d0] sb_edge_pad is the combo's;
      // every other attribute and the content are the embedded list's.
      if (number_attr(child, "sb_edge_pad", number)) {
        w.sb_edge_pad = number;
        w.has_sb_edge_pad = true;
      }
      read_part(child, w.list_box, WindowType::List, frame, "LIST_BOX");
    } else if (tag_is(child, "SCROLLBAR")) {
      read_part(child, w.scrollbar, WindowType::Scroll, frame, "SCROLLBAR");
    } else if (tag_is(child, "MIN_ITEM_HEIGHT")) {
      singleton(frame, child, "MIN_ITEM_HEIGHT", false);
      capture_.keep(child, "MIN_ITEM_HEIGHT");
      w.table_data.min_item_height = number_text(child);
      w.table_data.has_min_item_height = true;
    } else if (tag_is(child, "FIXED_HEADER_HEIGHT")) {
      singleton(frame, child, "FIXED_HEADER_HEIGHT", false);
      capture_.keep(child, "FIXED_HEADER_HEIGHT");
      w.table_data.fixed_header_height = number_text(child);
      w.table_data.has_fixed_header_height = true;
    } else if (tag_is(child, "COLUMN")) {
      if (frame.seen.count("COLUMN")) {
        because(&child, "A repeated COLUMN: retail's result depends on the COUNTs (docs/mnu/menu-re.md); "
                        "only the first COLUMN is kept.");
        capture_.tie(child, *frame.seen["COLUMN"]);
        continue;
      }
      frame.seen["COLUMN"] = &child;
      capture_.keep(child, "COLUMN");
      read_column(child, w.table_data.column);
    } else if (tag_is(child, "ORIENTATION")) {
      // Only a HORIZONTAL text sets anything and nothing resets it [orig: @ 0x64c745 /
      // 0x64c755]: the model keeps the first HORIZONTAL one, else the first.
      const auto seen = frame.seen.find("ORIENTATION");
      if (seen == frame.seen.end()) {
        frame.seen["ORIENTATION"] = &child;
        capture_.keep(child, "ORIENTATION");
        w.orientation = text_of(child);
      } else if (mnu_xml::iequals(child.text, kHorizontalOrientation) &&
                 !mnu_xml::iequals(seen->second->text, kHorizontalOrientation)) {
        seen->second->read = false;
        because(seen->second, "A later HORIZONTAL ORIENTATION decides (retail never resets it to vertical); "
                              "this one is left out.");
        capture_.tie(*seen->second, child);
        seen->second = &child;
        capture_.keep(child, "ORIENTATION");
        w.orientation = text_of(child);
      } else {
        because(&child, "A repeated ORIENTATION changes nothing in retail (only a HORIZONTAL one sets anything, "
                        "and nothing resets it); it is left out.");
        capture_.tie(child, *seen->second);
      }
    } else if (tag_is(child, "HEIGHT") || tag_is(child, "WIDTH")) {
      // One slot: the last authored of either [orig: @ 0x64c766 / 0x64c77e].
      singleton(frame, child, "HEIGHT", false);
      capture_.keep(child, "SCROLL_EXTENT");
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
        capture_.tie(child, node);
        unread_rest(node, i + 1, kScrollTags,
                    "Not read: an earlier scroll part stopped the scroll parse in retail.", node);
        continue;
      }
      // (Not a SCROLL: retail reads none of these; the row is kept as authored.)
      child.read = true;
      row.source = capture_.record(child);
      if (tag_is(child, "SHUTTLE")) w.shuttle.push_back(std::move(row));
      else if (tag_is(child, "SCROLLUP") || tag_is(child, "SCROLLLEFT")) w.scrollup.push_back(std::move(row));
      else w.scrolldown.push_back(std::move(row));
    } else if (tag_is(child, "DATASOURCE")) {
      // Every one loads and appends its credits [orig: @ 0x65ceeb -> CMarqueeWnd_LoadCreditsFromIni].
      capture_.keep(child, "DATASOURCE#" + std::to_string(w.datasources.size()));
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
      capture_.keep(c, "LEFT");
    } else if (tag_is(c, "RIGHT")) {
      pos.right = number_text(c);
      pos.has_right = true;
      capture_.keep(c, "RIGHT");
    } else if (tag_is(c, "TOP") || tag_is(c, "ULY")) {
      pos.top = number_text(c);
      pos.has_top = true;
      capture_.keep(c, "TOP");
    } else if (tag_is(c, "BOTTOM")) {
      pos.bottom = number_text(c);
      pos.has_bottom = true;
      capture_.keep(c, "BOTTOM");
    } else if (tag_is(c, "WIDTH")) {
      pos.right = pos.left + number_text(c);
      pos.has_right = true;
      capture_.keep(c, "RIGHT");
    } else if (tag_is(c, "HEIGHT")) {
      pos.bottom = pos.top + number_text(c);
      pos.has_bottom = true;
      capture_.keep(c, "BOTTOM");
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
    if (slot) {
      *slot = text_of(c);
      capture_.keep(c, strutil::to_upper(mnu_xml::ascii(c.tag)));
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
      capture_.keep(c, "STENCIL");
    } else if (tag_is(c, "BRUSH")) {
      frame.brush = text_of(c);
      capture_.keep(c, "BRUSH");
    } else if (tag_is(c, "MONOGRAM")) {
      frame.monogram = text_of(c);
      capture_.keep(c, "MONOGRAM");
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
    if (tag_is(c, "FILE")) {
      cursor.file = text_of(c);
      capture_.keep(c, "FILE");
    } else if (tag_is(c, "FLAGS")) {
      cursor.flags = text_of(c);
      capture_.keep(c, "FLAGS");
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
      item.source = capture_.record(c);
      items.items.push_back(std::move(item));
    } else if (tag_is(c, "ROW")) {
      Reads reads(*this, table_rule);
      c.read = true;
      TableRow row;
      row.source = capture_.record(c);
      for (const auto &cell_ptr : c.children) {
        if (!tag_is(*cell_ptr, "ITEM")) continue;
        Item cell;
        read_item(*cell_ptr, cell, true);
        cell.source = capture_.record(*cell_ptr);
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
        capture_.tie(c, node);
        unread_rest(node, i + 1, nullptr, "Not read: an earlier ITEMS APPEARANCE ended the list parse.", node);
        return false;
      }
      // (Another type reads no ITEMS APPEARANCE: the row is kept as authored.)
      c.read = true;
      appearance.source = capture_.record(c);
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
      header.source = capture_.record(c);
      column.headers.push_back(std::move(header));
    } else if (tag_is(c, "BODY")) {
      c.read = true;
      TableBody body;
      read_index(body.has_column, body.column);
      body.justify = keyword(c, "JUSTIFY", kJustify);
      body.vjustify = keyword(c, "VJUSTIFY", kVJustify);
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
      body.bitmap_flags = string_attr(c, "BITMAP_FLAGS");
      body.source = capture_.record(c);
      column.bodies.push_back(std::move(body));
    } else if (tag_is(c, "SUBST")) {
      c.read = true;
      TableSubst subst;
      read_index(subst.has_column, subst.column);
      subst.value = string_attr(c, "VALUE");
      subst.is_url = flag(c, "URL");
      subst.is_file = flag(c, "FILE");
      subst.file = text_of(c);
      subst.source = capture_.record(c);
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
  element.source = capture_.record(node);
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
           std::vector<ParseNote> *notes, ParseLayout layout) {
  return parse(reinterpret_cast<const uint8_t *>(content.data()),
               content.size(), out, error, notes, layout);
}

bool parse(const uint8_t *data, size_t size, Document &out, std::string &error,
           std::vector<ParseNote> *notes, ParseLayout layout) {
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
  TextLayoutCapture capture(out.source_encoding, layout == ParseLayout::Text);
  TreeReader reader(out.source_encoding, capture);
  reader.read_document(xml, out);
  if (notes) {
    // The reader's own notes are all retail hangs or overruns.
    for (const mnu_xml::Note &n : xml.notes) notes->push_back({n.line, std::string(), n.message, true});
    reader.sweep(xml, *notes);
  }
  // The file's look, when asked for, modeled against the writer's words for the records as read;
  // the bytes the loader skips before the text (the byte order mark) are the layout's.
  if (!capture.on()) return true;
  const size_t skipped = out.source_encoding == SourceEncoding::Utf16LE ? std::min<size_t>(size, 2)
                         : out.source_encoding == SourceEncoding::Utf8Bom ? 3
                                                                          : 0;
  out.text_layout = capture.finish(text, xml, written_screens(out),
                                   std::string(reinterpret_cast<const char *>(data), skipped));
  return true;
}

bool parse_file(const std::string &path, Document &out, std::string &error,
                std::vector<ParseNote> *notes, ParseLayout layout) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    error = "Failed to open file: " + path;
    return false;
  }

  std::stringstream buffer;
  buffer << file.rdbuf();
  std::string content = buffer.str();

  return parse(content, out, error, notes, layout);
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

}  // namespace opennova::mnu
