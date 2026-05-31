// MNU menu file parser (NovaLogic's XML-like UI markup format).
// Used for game menus, options screens, and other 2D UI layouts.
// This library parses .mnu files into a platform-agnostic AST.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mnu {

// Window/widget types in MNU files.
enum class WindowType {
  Window,        // Generic container
  Static,        // Static text/image (non-interactive)
  Button,        // Clickable button
  Edit,          // Single-line text input
  MultilineEdit, // Multi-line text input
  List,          // Item list
  CheckBox,      // Checkbox (toggle)
  Radio,         // Radio button (mutually exclusive)
  Combo,         // Dropdown/combobox
  Scroll,        // Scroll container
  Table,         // Table view
  SpinList,      // Spinner (up/down buttons)
  Multi,         // Multi-select list
  Map,           // Map view
  Globe,         // Globe view
  Label,         // Text label
  Goto,          // Navigation marker
  Marquee,       // Scrolling text/credits window (marquee_wnd)
  Unknown        // Unrecognized type
};

// Convert string to WindowType (case-insensitive).
WindowType parse_window_type(const std::string &type_str);

// Convert WindowType to string.
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

// Visual appearance for a specific state.
struct Appearance {
  std::string state;     // "default", "mouseover", "selected", "disabled"
  std::string type;      // "image", "custom", "outline", etc.
  std::string value;     // Texture filename or color value
  int map_state = -1;    // Sprite sheet row index (-1 = not a sprite sheet)
  int height = 0;        // Height of each frame in sprite sheet
};

// Sound trigger definition.
struct Sound {
  std::string state;   // "mousein", "selected", etc.
  std::string trigger; // "MOUSE_OVER", "CLICK_SELECT"
  std::string file;    // Sound filename (e.g., "menu.lwf")
};

// Action definition (navigation, show/hide).
struct Action {
  std::string type;   // "screen", "window", "POP_SCREEN"
  std::string state;  // "SHOW", "HIDE" (for window type)
  std::string file;   // Target .mnu file (for screen type)
  std::string target; // Screen name or window name
};

// Text/string definition.
struct String {
  std::string type;     // "id" (lookup) or literal
  std::string justify;  // "LEFT", "CENTER", "RIGHT"
  std::string vjustify; // "TOP", "CENTER", "BOTTOM"
  int edge = 0;         // Padding from edge in pixels
  std::string value;    // String ID or literal text
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

  // Check if font has any data set.
  bool empty() const { return name.empty() && default_fg.empty(); }
};

// Single item in a list/spinlist/combo.
struct Item {
  std::string type;  // "color", "image", "ID"
  std::string value; // Numeric value
  std::string text;  // Display text or texture filename
};

// Items container for list-like widgets.
struct Items {
  std::string justify;  // "LEFT", "CENTER", "RIGHT"
  std::string vjustify; // "TOP", "CENTER", "BOTTOM"
  std::string selection_color;  // Selected item highlight color
  std::vector<Item> items;
};

// ListBox scrollbar definition (for dropdown popups).
struct ListBoxScrollbar {
  bool present = false;
  Position position;
  std::vector<Appearance> track;       // Scrollbar track/background
  std::vector<Appearance> shuttle;     // Grabber appearances
  std::vector<Appearance> scrollup;    // Up arrow appearances
  std::vector<Appearance> scrolldown;  // Down arrow appearances
  std::vector<Sound> sounds;           // Scrollbar sounds (CLICK_VALUE, etc.)
};

// ListBox styling for dropdown popups.
struct ListBox {
  bool present = false;
  Position position;
  std::vector<Appearance> appearances;  // Background/outline colors
  Items items;  // Items with selection color
  int min_item_height = 0;  // Minimum height per item
  ListBoxScrollbar scrollbar;  // Custom scrollbar
};

// Frame/border definition.
struct Frame {
  std::string stencil;   // Border texture (9-slice style)
  int stencil_size = 0;  // Border thickness in pixels
  std::string brush;     // Tiling background texture
  std::string monogram;  // Watermark/logo overlay
};

// Spin button (up/down arrows) for SpinList.
struct SpinButton {
  bool present = false;
  Position position;
  std::vector<Appearance> appearances;
};

// Cursor definition.
struct Cursor {
  std::string file;  // Cursor image file (e.g., "newarow1.tga")
  std::string flags; // Cursor flags (e.g., "STANDARD_TRANSPARENT")
};

// Table column header definition.
struct TableHeader {
  std::string justify;   // "LEFT", "CENTER", "RIGHT"
  std::string vjustify;  // "TOP", "CENTER", "BOTTOM"
  int column = 0;        // Column index
  std::string sort;      // Sort order ("A" for ascending)
  int width = 0;         // Column width in pixels
  std::string text;      // Header text
};

// Table column body definition.
struct TableBody {
  std::string justify;   // "LEFT", "CENTER", "RIGHT"
  std::string vjustify;  // "TOP", "CENTER", "BOTTOM"
  int column = 0;        // Column index
  bool bitmap_draw = false;      // BITMAP_DRAW flag - render images in this column
  std::string bitmap_flags;      // BITMAP_FLAGS (e.g., "STANDARD_TRANSPARENT")
  bool scale_bitmap = false;     // SCALE_BITMAP flag
};

// Value substitution for table cells (renders image based on value).
struct TableSubst {
  int column = 0;        // Column index
  std::string value;     // Value to match
  bool is_file = false;  // FILE attribute present (image substitution)
  std::string file;      // Image filename to display
};

// Table column definition.
struct TableColumn {
  int count = 0;         // Number of columns
  int spacing = 0;       // Spacing between columns
  std::vector<TableHeader> headers;
  std::vector<TableBody> bodies;
  std::vector<TableSubst> substitutions;  // SUBST elements
};

// Table scrollbar definition (embedded in Table).
struct TableScrollbar {
  bool present = false;
  Position position;
  std::vector<Appearance> track;       // Scrollbar track/background
  std::vector<Appearance> shuttle;     // Grabber appearances
  std::vector<Appearance> scrollup;    // Up/left arrow appearances
  std::vector<Appearance> scrolldown;  // Down/right arrow appearances
  std::vector<Sound> sounds;
};

// Table-specific data.
struct TableData {
  TableColumn column;
  TableScrollbar scrollbar;
  int min_item_height = 0;
  std::string outline_color;    // From ITEMS default outline
  std::string selection_color;  // From ITEMS selected color
  bool multiselect = false;     // MULTISELECT attribute on ITEMS
};

// Window/widget node in the UI tree.
struct Window {
  std::string name;
  WindowType type = WindowType::Window;
  bool hidden = false;
  bool disabled = false;
  bool checked = false;     // For radio/checkbox initial state
  bool draw_frame = false;  // Whether to draw inherited frame
  bool modal = false;       // MODAL - dialog window
  bool readonly = false;    // READONLY - for multiline_edit
  int group = 0;            // Radio button group ID

  Position position;
  std::vector<Appearance> appearances;
  std::vector<Sound> sounds;
  std::vector<Action> actions;
  String string_data;
  Font font;
  Frame frame;
  Items items;
  ListBox list_box;  // Dropdown popup styling
  SpinButton spinup;
  SpinButton spindown;
  Cursor cursor;
  std::string text_rsrc;  // String resource file (only on root window)
  std::string datasource; // Data source file (for marquee_wnd, etc.)

  // Hotkey binding (e.g., VK_ESCAPE, VK_RETURN).
  std::string hotkey;
  bool hotkey_virtual = false;  // VIRTUAL attribute on HOTKEY element

  // Scroll/slider specific fields.
  std::string orientation;                  // "HORIZONTAL" or "VERTICAL"
  std::vector<Appearance> shuttle;          // Slider grabber appearances
  std::vector<Appearance> scrollup;         // Left/up arrow appearances
  std::vector<Appearance> scrolldown;       // Right/down arrow appearances

  // Table-specific data.
  TableData table_data;

  // Child windows (nested hierarchy).
  std::vector<Window> children;
};

// Screen definition (top-level container).
struct Screen {
  std::string name;
  int music_var = 0;       // Music track index
  std::string text_rsrc;   // String resource file (e.g., "menutxt.BIN")
  std::string cursor_file; // Default cursor file
  std::string cursor_flags;
  Window root_window;      // Root window hierarchy
};

// Parsed MNU document containing one or more screens.
struct Document {
  std::vector<Screen> screens;

  // Find a screen by name (case-insensitive). Returns nullptr if not found.
  const Screen *find_screen(const std::string &name) const;

  // Get the first/default screen. Returns nullptr if document is empty.
  const Screen *first_screen() const;
};

// Parse options.
struct ParseOptions {
  bool fix_malformed_xml = true; // Fix common XML errors (bare attributes, etc.)
};

// Parse MNU content from a string buffer.
// Returns true on success, false on error with description in `error`.
bool parse(const std::string &content, Document &out, std::string &error,
           const ParseOptions &options = {});

// Parse MNU content from a byte buffer.
bool parse(const uint8_t *data, size_t size, Document &out, std::string &error,
           const ParseOptions &options = {});

// Parse MNU file from disk.
bool parse_file(const std::string &path, Document &out, std::string &error,
                const ParseOptions &options = {});

// Preprocess MNU content to fix common XML errors.
// Called automatically by parse() if fix_malformed_xml is true.
std::string fix_malformed_xml(const std::string &content);

// Strip {hot} marker from text for display.
// Returns the display text with {hot} removed.
// Optionally returns the hotkey character and its position.
std::string strip_hotkey_marker(const std::string &text,
                                std::string *out_hotkey = nullptr,
                                int *out_hotkey_pos = nullptr);

// Parse a hex color string to RGB components (0-255).
// Handles formats: "RRGGBB" or "AARRGGBB" or "#RRGGBB".
// Returns false if the string is a variable reference (%VAR%) or invalid.
bool parse_hex_color(const std::string &hex, uint8_t &r, uint8_t &g, uint8_t &b,
                     uint8_t &a);

// Check if a color string is a variable reference (e.g., "%TRIM_COLOR%").
bool is_color_variable(const std::string &color);

// Serialize MNU document back to XML-like text.
// When pretty is true, output is indented with indent_size spaces.
std::string serialize(const Document &doc, bool pretty = true,
                      int indent_size = 2);

// Serialize MNU document to a file on disk. Returns false on failure and sets
// error.
bool serialize_file(const Document &doc, const std::string &path,
                    std::string &error, bool pretty = true,
                    int indent_size = 2);

}  // namespace mnu
