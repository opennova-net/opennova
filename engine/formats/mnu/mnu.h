// MNU menu file parser (NovaLogic's XML-like UI markup format).
// Used for game menus, options screens, and other 2D UI layouts.
// This library parses .mnu files into a platform-agnostic AST.
//
// The reader follows retail's (docs/mnu/menu-re.md, "The reader"): the XML layer is
// mnu_xml's structural translation of NapiXML_ParseElementTree @ 0x769d70, and this
// layer reads the tree the way the element parses do [orig:
// CUIScene_ParseNodeAttributes @ 0x639630; CUIScene_CreateWidgetByType @ 0x64f630;
// CUIElement_ParseXMLDefinition @ 0x648120 and the per-type parses it chains to].
// What retail does not read is reported as a ParseNote and left out of the model;
// the writer produces from the model what retail's reader reads back into it.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace opennova::mnu {

// Presence contract: every `has_*` bit and container `present` bit is the
// authoritative authored state. Writers ignore retained latent values when the
// corresponding bit is false, allowing editors to disable a field/block without
// destroying a value that may be restored later. Code-created documents must
// set the bit explicitly; typed mutators do so when they author content.

// Window/widget types: the tokens the original factory matches [orig:
// CUIScene_CreateWidgetByType @ 0x64f630], full-word, ignoring case. Any other
// token (or "window") builds a generic CWnd: Window.
enum class WindowType {
  Window,        // generic CWnd ("window" or any token the factory does not match)
  Static,        // STATIC
  Button,        // BUTTON
  Edit,          // EDIT
  MultilineEdit, // MULTILINE_EDIT
  List,          // LIST
  CheckBox,      // CHECKBOX
  Radio,         // RADIO
  Combo,         // COMBOBOX
  Scroll,        // SCROLL
  Table,         // TABLE
  SpinList,      // SPINLIST
  Marquee,       // MARQUEE_WND
  GlbTable,      // GLB_TABLE - NovaWorld server-browser table
  RadioEdit,     // RADIOEDIT - radio button with an attached edit field
  LanList,       // LAN_LIST - LAN session list
  Gopher,        // GOPHER - in-game gopher/news browser
};
inline constexpr int kWindowTypeCount = static_cast<int>(WindowType::Gopher) + 1;

// The factory's match of a TYPE token (case-insensitive); Window for anything else.
WindowType parse_window_type(const std::string &type_str);

// The factory token of a type ("window" for the generic one).
const char *window_type_name(WindowType type);

// Position rectangle with optional fields.
struct Position {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;
  bool has_left = false;
  bool has_top = false;
  bool has_right = false;
  bool has_bottom = false;

  // Computed width/height (only valid if both bounds are set).
  int width() const { return has_right && has_left ? right - left : 0; }
  int height() const { return has_bottom && has_top ? bottom - top : 0; }
};

// One APPEARANCE row (also SHUTTLE / SCROLLUP / SCROLLDOWN and the ITEMS rows).
// Every attribute holds the token retail reads: STATE and TYPE the one its walk
// keeps, spelled as authored.
struct Appearance {
  std::string state;     // DEFAULT / DISABLED / MOUSEOVER / SELECTED
  std::string type;      // IMAGE / COLOR / CUSTOM / OUTLINE (IMAGEROW in a TABLE's ITEMS)
  std::string value;     // the element text: a texture name or a color
  bool has_map_state = false;
  int map_state = -1;    // Sprite sheet row index (-1 = not a sprite sheet)
  bool has_height = false;
  int height = 0;        // Height of each frame in sprite sheet
  std::string flags;     // FLAGS (texture flags, e.g. STANDARD_TRANSPARENT); empty = none
};

// Sound trigger definition.
struct Sound {
  std::string state;   // MOUSEIN / MOUSEOUT / SELECTED
  std::string trigger; // "MOUSE_OVER", "CLICK_SELECT"
  std::string file;    // the element text: a sound name (e.g., "menu.lwf")
};

// Action definition [orig: CUIElement_ParseXMLDefinition @ 0x648120, the ACTION arm].
struct Action {
  std::string type;   // one of the sixteen codes; anything else is code 0 (ignored)
  std::string state;  // HIDE / SHOW / ENABLE / DISABLE (WINDOW)
  std::string file;   // FILE (SCREEN)
  // FIELD, SOURCE and NAME write one slot; the first authored wins. `field_attr` is
  // the spelling it was authored with (empty writes FIELD).
  std::string field;
  std::string field_attr;
  bool has_target_form = false;
  int target_form = 0;
  bool toggle = false;           // TOGGLE, presence
  std::string test;              // LT/LE/EQ/GE/GT (LT when absent)
  std::string target;            // the element text, untrimmed
  bool external_browser = false; // EXTERNAL_BROWSER, presence
};

// The sixteen ACTION TYPE tokens in the parse's compare order, nullptr-ended: a token's
// code is its index + 1; any other token (or none) is code 0, the jump table's default,
// which every path ignores; and the STATEs a WINDOW action's switch acts on (any other does
// nothing) [orig: CUIElement_ParseXMLDefinition @ 0x648ee2 ACTION arm;
// CUIWidget_HandleScriptedAction @ 0x6497f0, its WINDOW STATE switch @ 0x6498f7].
inline constexpr const char *kActionTypes[] = {
    "SCREEN", "WINDOW", "URL", "FORM_POST", "GLB_LOAD", "GLB_LOADANDPING", "GLB_FILTER", "GLB_FILTER_NUM",
    "GLB_PING", "GLB_JOIN", "TAB", "POP_SCREEN", "APPMSG", "LAN_SEARCH", "LAN_JOIN", "MNX", nullptr};
inline constexpr const char *kActionStates[] = {"HIDE", "SHOW", "ENABLE", "DISABLE", nullptr};

// The tokens each keyword attribute's parse recognizes, nullptr-ended, in the order the
// reader compares them; any other token is left out of the model (a ParseNote says so).
// An APPEARANCE's STATE and TYPE [orig: @ 0x648226], a TABLE's ITEMS rows also knowing
// IMAGEROW [orig: @ 0x642816]; a SOUND's STATE, its sound state the index + 1
// (runtime/menu/menu_sound.h) [orig: @ 0x648909]; an ACTION's TEST, and the three
// attribute names that write its one FIELD slot, the first authored kept [orig: @ 0x648ee2];
// the JUSTIFY and VJUSTIFY of a STRING, an ITEMS, an ITEM, a HEADER and a BODY; the TYPE of
// a STRING, a TOGGLE_STRING and a HEADER [orig: CUIButtonWidget_ParseXMLAttributes
// @ 0x657c30] and of an ITEM [orig: @ 0x6457b6, 0x64bd57, 0x642816].
inline constexpr const char *kAppearanceStates[] = {"DEFAULT", "DISABLED", "MOUSEOVER", "SELECTED", nullptr};
inline constexpr const char *kAppearanceTypes[] = {"IMAGE", "COLOR", "CUSTOM", "OUTLINE", nullptr};
inline constexpr const char *kTableAppearanceTypes[] = {"IMAGE", "IMAGEROW", "COLOR", "CUSTOM", "OUTLINE", nullptr};
inline constexpr const char *kSoundStates[] = {"MOUSEIN", "MOUSEOUT", "SELECTED", nullptr};
inline constexpr const char *kActionTests[] = {"LT", "LE", "EQ", "GE", "GT", nullptr};
inline constexpr const char *kActionFieldAttributes[] = {"FIELD", "SOURCE", "NAME", nullptr};
inline constexpr const char *kJustify[] = {"LEFT", "CENTER", "RIGHT", nullptr};
inline constexpr const char *kVJustify[] = {"TOP", "CENTER", "BOTTOM", nullptr};
inline constexpr const char *kStringTypes[] = {"ID", nullptr};
inline constexpr const char *kItemTypes[] = {"ID", "IMAGE", "COLOR", "BITMAP", nullptr};
// A HEADER's primary sort key, one slot under either name [orig: @ 0x6431f2 / 0x643228];
// the writer puts the first down when the model holds no spelling
// (TableColumn::primary_sort_token).
inline constexpr const char *kPrimarySortTokens[] = {"PRIMARY_SORT", "DEFAULT_SORT", nullptr};
// A BODY's draw kinds (TableBody::display), in the order the writer puts them down.
inline constexpr const char *kBodyDisplays[] = {"CUSTOM_DRAW", "BITMAP_DRAW", "BITMAP_TEXT", nullptr};
// The one ORIENTATION text that sets anything: the whole text HORIZONTAL sets the flag and
// nothing clears it, so any other text reads as vertical [orig: @ 0x64c745 / 0x64c755].
inline constexpr const char *kHorizontalOrientation = "HORIZONTAL";

// Whether `token` is one of the nullptr-ended `tokens`, ignoring case (the parses' compare).
bool known_token(const std::string &token, const char *const *tokens);

// STRING [orig: CUIButtonWidget_ParseXMLAttributes @ 0x657c30].
struct String {
  bool present = false;
  std::string type;     // "ID" (a string-table key) or literal
  std::string justify;  // "LEFT", "CENTER", "RIGHT"
  std::string vjustify; // "TOP", "CENTER", "BOTTOM"
  bool has_edge = false;
  int edge = 0;         // Padding from edge in pixels
  bool wrap = false;    // WRAP, presence
  std::string value;    // the element text: a string ID or literal text
};

// TOGGLE_STRING [orig: CButtonWnd_ParseTooltipXML @ 0x658170]: the button's second
// label, a string-table key when TYPE="ID".
struct ToggleString {
  bool present = false;
  std::string type;
  std::string value;
};

// Font specification with colors for different states.
struct Font {
  std::string name;          // Font filename (e.g., "Gunpl22b.fnt")
  std::string default_fg;    // Default foreground color (hex)
  std::string default_bg;    // Default background color (hex)
  std::string mouseover_fg;  // Hover foreground color
  std::string mouseover_bg;  // Hover background color
  std::string selected_fg;   // Selected/pressed foreground color
  std::string selected_bg;   // Selected/pressed background color
  std::string disabled_fg;   // Disabled foreground color
  std::string disabled_bg;   // Disabled background color

  // True when no child element is authored.
  bool empty() const {
    return name.empty() && default_fg.empty() && default_bg.empty() &&
           mouseover_fg.empty() && mouseover_bg.empty() && selected_fg.empty() &&
           selected_bg.empty() && disabled_fg.empty() && disabled_bg.empty();
  }
};

// One ITEM: a list / spin list row, or a TABLE cell (in a ROW).
struct Item {
  std::string type;     // ID / IMAGE / COLOR (spin list) / BITMAP (table cell)
  std::string value;    // VALUE
  std::string text;     // the element text: display text, a key or a texture name
  std::string justify;  // JUSTIFY
  std::string vjustify; // VJUSTIFY
  bool pairs_list = false; // PAIRS_LIST, presence (LIST)
  bool has_column = false; // COLUMN (a TABLE cell)
  int column = 0;
};

// A TABLE's ITEMS > ROW: one row of cells [orig: CTableWnd_ParseXMLContentDefinition
// @ 0x6427d0].
struct TableRow {
  std::vector<Item> cells;
};

// Items container for list-like widgets.
struct Items {
  bool present = false;
  bool multiselect = false;  // MULTISELECT, presence
  std::string justify;       // "LEFT", "CENTER", "RIGHT"
  std::string vjustify;      // "TOP", "CENTER", "BOTTOM"
  std::vector<Appearance> appearances;  // ordered, duplicates kept
  std::vector<Item> items;
  std::vector<TableRow> rows;           // TABLE only
};

// Frame/border definition.
struct Frame {
  static constexpr int kDefaultInsetX = 12;
  static constexpr int kDefaultInsetY = 8;

  std::string stencil;   // Border texture (9-slice style)
  bool has_stencil_size = false;
  int stencil_size = 0;  // Border thickness in px (STENCIL size=, orig elem+0x284)
  std::string brush;     // Tiling background texture
  std::string monogram;  // Watermark/logo overlay
  // Retail constructs every window with INSETX=12 and INSETY=8, then lets
  // authored STENCIL attributes override those fields (elem+0x288/+0x28C).
  // has_* remains separate so omitted defaults and explicit values round-trip.
  bool has_insetx = false;
  int insetx = kDefaultInsetX;
  bool has_insety = false;
  int insety = kDefaultInsetY;
};

// Cursor definition.
struct Cursor {
  std::string file;  // Cursor image file (e.g., "newarow1.tga")
  std::string flags; // Cursor flags (e.g., "STANDARD_TRANSPARENT")
};

// Table column header definition. The COLUMN index is explicit in the model: a
// HEADER, BODY or SUBST that authors none takes the running index of its COLUMN
// element, which the reader writes in (so a save in any order reads the same).
// WIDTH and SORT are as authored: one the HEADER leaves out carries over from the
// HEADER before it (table_header_setup resolves them).
struct TableHeader {
  std::string justify;   // "LEFT", "CENTER", "RIGHT"
  std::string vjustify;  // "TOP", "CENTER", "BOTTOM"
  bool has_column = false;
  int column = 0;        // Column index
  std::string sort;      // SORT ('1' numeric, 'A'/'a' alphabetic; other values keep the carried one)
  bool has_width = false;
  int width = 0;         // WIDTH: the column width in pixels
  std::string type;      // "id" = text is a string-table key (CUIStringTable_LookupString)
  std::string text;      // Header text, or the string ID when type=="id"
};

// Table column body definition.
struct TableBody {
  std::string justify;   // "LEFT", "CENTER", "RIGHT"
  std::string vjustify;  // "TOP", "CENTER", "BOTTOM"
  bool has_column = false;
  int column = 0;        // Column index
  bool bitmap_draw = false;      // BITMAP_DRAW - render images in this column
  bool bitmap_text = false;      // BITMAP_TEXT
  bool custom_draw = false;      // CUSTOM_DRAW - shell-drawn cell
  // The cell's draw kind is the first authored of CUSTOM_DRAW / BITMAP_DRAW /
  // BITMAP_TEXT (retail's walk keeps it); written first. Empty: none authored.
  std::string display;
  std::string bitmap_flags;      // BITMAP_FLAGS (e.g., "STANDARD_TRANSPARENT")
  bool scale_bitmap = false;     // SCALE_BITMAP flag
};

// Value substitution for table cells (renders image based on value).
struct TableSubst {
  bool has_column = false;
  int column = 0;        // Column index
  std::string value;     // Value to match
  bool is_file = false;  // FILE attribute present (image substitution)
  bool is_url = false;   // URL attribute present (the image is fetched)
  std::string file;      // the element text: the image name or address
};

// Table column definition.
struct TableColumn {
  bool has_count = false;
  int count = 0;         // Number of columns (fewer than 1 is refused: the table keeps 1)
  bool has_spacing = false;
  int spacing = 0;       // Spacing between columns
  std::vector<TableHeader> headers;
  std::vector<TableBody> bodies;
  std::vector<TableSubst> substitutions;  // SUBST elements
  // The table's sort keys [orig: CTableWnd_ParseXMLContentDefinition @ 0x6427d0, the
  // HEADER walk's stores @ 0x643240 / 0x64325f / 0x64327e / 0x64329d]: a HEADER's
  // DEFAULT_SORT / PRIMARY_SORT (one slot), SECONDARY_SORT or TERTIARY_SORT sets the
  // key to the column index the attribute walk stands at when it meets the key (the
  // walk runs last authored first, so a key authored after the HEADER's own COLUMN
  // takes the index before it); the last write wins. -1: no HEADER sets it. The
  // writer places each on a HEADER that reads back the same index.
  int primary_sort = -1;
  std::string primary_sort_token;  // DEFAULT_SORT or PRIMARY_SORT, as authored
  int secondary_sort = -1;
  int tertiary_sort = -1;
};

// What retail's HEADER walk hands init_table_row for one HEADER [orig:
// CTableWnd_ParseXMLContentDefinition @ 0x6427d0 -> init_table_row @ 0x63f9c0]: the
// column index, the width (100 at the COLUMN element, then carried from HEADER to
// HEADER until one authors WIDTH) and the column's sort-compare flag (+112: 0 at the
// COLUMN element, carried the same way; SORT '1' sets it, 'A' or 'a' clears it): set,
// the rows compare numerically, else by text [orig: CTableWnd_CompareRows @ 0x63e9c0
// reads it as the compare mode; the direction is the column's +120, which
// init_table_row sets to 1]. `set_up` is false for an index retail's init_table_row
// refuses (below 0, or not below the table's column count: the authored COUNT when it
// is 1 or more, else 1).
struct TableHeaderSetup {
  int column = 0;
  int width = 100;
  bool numeric_sort = false;
  bool set_up = true;
};
std::vector<TableHeaderSetup> table_header_setup(const TableColumn &column);

// Table-specific data.
struct TableData {
  TableColumn column;
  bool has_min_item_height = false;
  int min_item_height = 0;      // MIN_ITEM_HEIGHT (LIST and TABLE)
  bool has_fixed_header_height = false;
  int fixed_header_height = 0;  // FIXED_HEADER_HEIGHT (TABLE)
};

// One authored accelerator. A Window may carry several HOTKEY rows; order is
// significant because the runtime chooses the first matching visible Widget.
struct Hotkey {
  std::string value;        // the element text, untrimmed
  bool virtual_key = false; // VIRTUAL, presence
};

// An element a type-specific parse reads that the model does not type (the
// LAN_LIST buttons, the GLB_TABLE parts, the GOPHER targets and filters), kept as
// read so it writes back the same.
struct ElementAttribute {
  std::string name;
  std::string value;      // the token retail reads
  bool has_value = false; // false: a bare attribute
};
struct Element {
  std::string tag;
  std::vector<ElementAttribute> attributes;
  std::string text;
  std::vector<Element> children;
};

// The elements only the GLB_TABLE, GOPHER and LAN_LIST parses read, which a window keeps
// at its top level as read [orig: CLanGameBrowser_ParseExtendedXMLDefinition @ 0x65dac0;
// CGopherWnd_ParseXMLDefinition @ 0x65b130; CLanListWnd_ParseXMLDefinition @ 0x65b9d0],
// and the attributes beside its own it keeps, read by presence with no value [orig:
// CLanGameBrowser_ParseExtendedXMLDefinition @ 0x65dac0]; each nullptr-ended.
inline constexpr const char *kExtraTags[] = {"JOIN_BUTTON", "SEARCH_BUTTON", "GLB_TABLE", "GLB_INFO_BUTTON",
                                             "GLB_INFO_PLAYER_DETAILS", "GLB_INFO_SERVER_DETAILS", "GLB_NW_INFO",
                                             "TARGET", "FILTERS", "GOPHER_BACK", nullptr};
inline constexpr const char *kExtraAttributes[] = {"PLAYERLIST", "SERVERLIST", nullptr};

struct Window;

// A window an owner parses from one child element with its own embedded widget:
// SPINUP / SPINDOWN (a BUTTON), LIST_BOX (a LIST), SCROLLBAR (a SCROLL). Absent until
// authored; copies deep. Its presence bit follows the contract above: hide() leaves
// the part out of the file and keeps its window (latent()), author() writes it again.
class WindowPart {
public:
  WindowPart();
  WindowPart(const WindowPart &other);
  WindowPart(WindowPart &&other) noexcept;
  WindowPart &operator=(const WindowPart &other);
  WindowPart &operator=(WindowPart &&other) noexcept;
  ~WindowPart();

  bool present() const { return window_ != nullptr && shown_; }
  explicit operator bool() const { return present(); }
  const Window *get() const { return present() ? window_.get() : nullptr; }
  Window *get() { return present() ? window_.get() : nullptr; }
  const Window *operator->() const { return window_.get(); }
  Window *operator->() { return window_.get(); }
  const Window &operator*() const { return *window_; }
  Window &operator*() { return *window_; }
  // The part's window whether it is written or left out (null when none was authored).
  const Window *latent() const { return window_.get(); }
  Window *latent() { return window_.get(); }
  // The part, written, created (with `type`) when absent; a left-out part's window
  // is written again as it was.
  Window &author(WindowType type);
  // Left out of the file, its window kept.
  void hide() { shown_ = false; }
  void clear();

private:
  std::unique_ptr<Window> window_;
  bool shown_ = false;
};

// Window/widget node in the UI tree.
struct Window {
  std::string name;
  WindowType type = WindowType::Window;
  // The TYPE token as authored (its first token), written in preference to the
  // canonical name so a token the factory does not match round-trips. Empty for
  // windows created in code and for parts.
  std::string type_token;
  // Flags are presence-only [orig: @ 0x648120 attribute walk]: HIDDEN="0" hides.
  bool hidden = false;
  bool disabled = false;    // DISABLE
  bool checked = false;     // CHECKED (RADIO, CHECKBOX)
  bool draw_frame = false;  // DRAW_FRAME: gates ALL frame drawing (own <FRAME> or
                            // inherited); a window with a <FRAME> but no DRAW_FRAME only
                            // hands textures to framed children [orig: CStaticWnd_Render
                            // @ 0x657b10 -> field +0x134 guards CUIElement_DrawFrame]
  bool modal = false;       // MODAL - dialog window
  bool readonly = false;    // READONLY - for multiline_edit
  bool as_button = false;   // AS_BUTTON - render a checkbox as a toggle button
  bool has_group = false;
  int group = 0;            // <GROUP>n</GROUP> (RADIO); the attribute form is not read

  // Numeric edit-field constraints (EDIT) [orig: CEditWnd_ParseXMLProperties
  // @ 0x661d10]. NUMBER is a bare flag.
  bool number = false;      // NUMBER: numeric-only input
  bool has_minval = false;
  int minval = 0;           // MINVAL
  bool has_maxval = false;
  int maxval = 0;           // MAXVAL
  bool has_maxchar = false;
  int maxchar = 0;          // MAXCHAR

  bool has_form = false;
  int form = 0;             // FORM (int -> widget+0x124): form/grouping index
  bool global_var = false;  // GLOBAL_VAR flag
  bool password = false;    // PASSWORD flag (edit widgets: masked input)

  // SCROLL [orig: CUIScrollWidget_ParseExtendedXMLDef @ 0x64c6d0]: ORIENTATION's
  // text ("HORIZONTAL" whole, else vertical: an ORIENTATION only ever sets horizontal,
  // @ 0x64c755, so any HORIZONTAL one decides) and the one along-axis part extent a
  // window-level <HEIGHT> or <WIDTH> sets (the last authored; its spelling kept).
  std::string orientation;
  bool has_scroll_extent = false;
  int scroll_extent = 0;
  bool scroll_extent_is_width = false;

  Position position;
  std::vector<Appearance> appearances;
  std::vector<Sound> sounds;
  std::vector<Action> actions;  // document order (the runtime walks them in reverse)
  String string_data;
  ToggleString toggle_string;
  Font font;
  Frame frame;
  Items items;
  WindowPart list_box;          // LIST_BOX (COMBOBOX): the dropdown list
  bool has_sb_edge_pad = false; // LIST_BOX's sb_edge_pad attribute, read by the combo
  int sb_edge_pad = 0;
  WindowPart spinup;            // SPINUP / SPINDOWN (SPINLIST): the arrow buttons
  WindowPart spindown;
  WindowPart scrollbar;         // SCROLLBAR (LIST, TABLE, MULTILINE_EDIT)
  Cursor cursor;
  // TEXT_RSRC: this window's string table. An authored empty one is still a table (a
  // name no file answers: the window's keys show raw) [orig:
  // CUIElement_ParseXMLDefinition @ 0x648eb8 stores a copy of any TEXT_RSRC text].
  bool has_text_rsrc = false;
  std::string text_rsrc;
  std::string private_data;     // PRIVATE_DATA (+0x128)
  // Every DATASOURCE in document order (MARQUEE_WND): each loads and appends its
  // credits [orig: CMarqueeWnd_ParseXMLDefinition @ 0x65ceb0 calls
  // CMarqueeWnd_LoadCreditsFromIni @ 0x65c5a0 per element].
  std::vector<std::string> datasources;

  // Ordered bindings (e.g., VK_ESCAPE, "=", "-").
  std::vector<Hotkey> hotkeys;

  // Scroll/slider parts: SCROLLLEFT reads as SCROLLUP, SCROLLRIGHT as SCROLLDOWN.
  std::vector<Appearance> shuttle;          // Slider grabber appearances
  std::vector<Appearance> scrollup;         // Left/up arrow appearances
  std::vector<Appearance> scrolldown;       // Right/down arrow appearances

  // Table-specific data.
  TableData table_data;

  // What only the GLB_TABLE, GOPHER and LAN_LIST parses read, kept as read.
  std::vector<Element> extras;
  std::vector<ElementAttribute> extra_attributes; // PLAYERLIST / SERVERLIST

  // Child windows (nested hierarchy).
  std::vector<Window> children;
};

// Screen definition (top-level container) [orig: CUIScene_ParseNodeAttributes
// @ 0x639630]: a SCREEN reads NAME, MUSICVAR and its WINDOWs, nothing else.
struct Screen {
  std::string name;
  bool has_music_var = false;
  int music_var = 0;           // Music track index
  std::vector<Window> roots;   // every root WINDOW, in document order
};

// The source encoding is document metadata, not opaque source passthrough
// [orig: XML_ParseWithBOMDetection @ 0x76a690]: CodePage is no byte order mark
// (the system code page, 1252; the model holds its bytes), Utf8Bom and Utf16LE are
// read as Unicode (the model holds UTF-8). A UTF-16 big-endian file is read as
// little-endian, as retail does, and loads nothing.
enum class SourceEncoding : uint8_t {
  CodePage,
  Utf8Bom,
  Utf16LE,
};

// The wide text the loader hands retail's reader, and the encoding it read it as
// (the SourceEncoding rules above).
void decode_source(const uint8_t *data, size_t size, SourceEncoding &encoding,
                   std::u32string &text);

// Parsed MNU document containing one or more screens.
struct Document {
  std::vector<Screen> screens;
  SourceEncoding source_encoding = SourceEncoding::CodePage;

  // The screen a by-name lookup finds (case-insensitive): the LAST of that name,
  // as retail's newest-first walk does [orig: CUIScene_SelectNodeByName @ 0x63b6b0].
  const Screen *find_screen(const std::string &name) const;

  // Get the first/default screen. Returns nullptr if document is empty.
  const Screen *first_screen() const;
};

// The window a by-name lookup finds under `window` [orig: CWnd_FindChildByName @ 0x646850]:
// an empty name, or a window with no NAME, finds nothing (its children unsearched); a match
// without case is the window itself; else each child in order, recursively. Only
// `children` are walked: a part (list_box, spinup, spindown, scrollbar) and the windows it
// holds are not reached.
const Window *find_window(const Window &window, const std::string &name);
// The same over a screen's root windows in document order: the first root that finds one
// [orig: UI_FindScreenControl @ 0x63ae80, the walk over the section's roots].
const Window *find_window(const Screen &screen, const std::string &name);

// What the reader left out: an element or attribute retail's parse does not read
// (or that a later one replaces), a window retail does not create, a retail crash
// or hang it stopped at, or a number holding a %NAME% (the model holds only the
// number it reads). `key` is the element path (the mnu_coverage form,
// "/SCREEN[0]/WINDOW[MAIN]/STRING@TRIGGER"); the note covers everything under it.
// `fatal`: retail crashes or hangs on this input (docs/mnu/menu-re.md, "Crash and
// hang cases"), or reads a number from a stylesheet variable where the model holds
// another, so the file as it stands cannot ship.
struct ParseNote {
  size_t line = 0;
  std::string key;
  std::string message;
  bool fatal = false;
  // The screen or window of the model the note is in, as the issue check places a record
  // ("0/window:1/window:0": the screen's index, then each window's among the windows its
  // owner holds, as far down as the note's WINDOW elements were created); "" when it is in
  // none (a note of the reader itself, or outside every SCREEN). Unlike `key`, it tells two
  // windows of one NAME apart.
  std::string locator;
};

// Parse MNU content from a string buffer.
// Returns true on success, false on error with description in `error`.
bool parse(const std::string &content, Document &out, std::string &error,
           std::vector<ParseNote> *notes = nullptr);

// Parse MNU content from a byte buffer.
bool parse(const uint8_t *data, size_t size, Document &out, std::string &error,
           std::vector<ParseNote> *notes = nullptr);

// Parse MNU file from disk.
bool parse_file(const std::string &path, Document &out, std::string &error,
                std::vector<ParseNote> *notes = nullptr);

// Strip the first {hot} marker from text for display; later markers remain
// literal. Optionally returns the following hotkey byte and the marker's byte
// position (-1 when absent). A trailing marker has a position but no hotkey.
std::string strip_hotkey_marker(const std::string &text,
                                std::string *out_hotkey = nullptr,
                                int *out_hotkey_pos = nullptr);

// A value the writer cannot put in a file retail reads back the same (a quote in an
// attribute value) or that retail faults on (docs/mnu/menu-re.md, "Crash and hang
// cases"): what, where, and the retail consequence.
struct WriteIssue {
  std::string screen;   // the screen's name
  std::string window;   // the nearest window's name ("" for the screen itself)
  // The record that holds the value, by its lists' element paths (the element names
  // down from the owner, lowercase, joined by '.'; the editor's menu table,
  // editor/documents/mnu_table, names each list and field so): the screen's index,
  // then each list's path and index, "0/window:0/window:2/action:1".
  std::string locator;
  std::string field;    // the field's element path on that record ("" = the record itself)
  std::string message;
};

// Every WriteIssue of a document; serialization refuses while there is one.
std::vector<WriteIssue> write_issues(const Document &doc);

// Element text as the writer puts it: '<' and '&' as the entities retail's reader decodes
// back, everything else as it is.
std::string escape_text(const std::string &text);

// Serialize MNU document back to XML-like text (the model's own bytes).
// When pretty is true, output is indented with indent_size spaces.
std::string serialize(const Document &doc, bool pretty = true,
                      int indent_size = 2);

// Serialize using Document::source_encoding. False, with `error` (the first write
// issue), when the document has a write issue.
bool serialize_bytes(const Document &doc, std::vector<uint8_t> &out,
                     std::string &error, bool pretty = true,
                     int indent_size = 2);

}  // namespace opennova::mnu
