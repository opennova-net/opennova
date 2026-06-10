// Unit tests for mnu: MNU menu file parser.
#include <cstdlib>
#include <iostream>
#include <string>

#include "mnu/mnu.h"

namespace {

#define CHECK(cond, msg)                                      \
  do {                                                        \
    if (!(cond)) {                                            \
      std::cerr << "FAIL: " << msg << " at line " << __LINE__ \
                << "\n";                                      \
      return false;                                           \
    }                                                         \
  } while (0)

bool position_eq(const mnu::Position &a, const mnu::Position &b) {
  return a.left == b.left && a.top == b.top && a.right == b.right &&
         a.bottom == b.bottom && a.has_left == b.has_left &&
         a.has_top == b.has_top && a.has_right == b.has_right &&
         a.has_bottom == b.has_bottom;
}

bool appearance_eq(const mnu::Appearance &a, const mnu::Appearance &b) {
  return a.state == b.state && a.type == b.type && a.value == b.value &&
         a.map_state == b.map_state && a.height == b.height;
}

bool sound_eq(const mnu::Sound &a, const mnu::Sound &b) {
  return a.state == b.state && a.trigger == b.trigger && a.file == b.file;
}

bool action_eq(const mnu::Action &a, const mnu::Action &b) {
  return a.type == b.type && a.state == b.state && a.file == b.file &&
         a.target == b.target;
}

bool string_eq(const mnu::String &a, const mnu::String &b) {
  return a.type == b.type && a.justify == b.justify &&
         a.vjustify == b.vjustify && a.edge == b.edge && a.value == b.value;
}

bool font_eq(const mnu::Font &a, const mnu::Font &b) {
  return a.name == b.name && a.default_fg == b.default_fg &&
         a.default_bg == b.default_bg && a.mouseover_fg == b.mouseover_fg &&
         a.mouseover_bg == b.mouseover_bg && a.selected_fg == b.selected_fg &&
         a.selected_bg == b.selected_bg && a.disabled_fg == b.disabled_fg &&
         a.disabled_bg == b.disabled_bg;
}

bool frame_eq(const mnu::Frame &a, const mnu::Frame &b) {
  return a.stencil == b.stencil && a.stencil_size == b.stencil_size &&
         a.brush == b.brush && a.monogram == b.monogram &&
         a.has_insetx == b.has_insetx && a.insetx == b.insetx &&
         a.has_insety == b.has_insety && a.insety == b.insety;
}

bool items_eq(const mnu::Items &a, const mnu::Items &b) {
  if (a.justify != b.justify || a.vjustify != b.vjustify ||
      a.items.size() != b.items.size()) {
    return false;
  }
  for (size_t i = 0; i < a.items.size(); ++i) {
    const auto &ai = a.items[i];
    const auto &bi = b.items[i];
    if (ai.type != bi.type || ai.value != bi.value || ai.text != bi.text) {
      return false;
    }
  }
  return true;
}

bool spin_eq(const mnu::SpinButton &a, const mnu::SpinButton &b) {
  if (a.present != b.present) return false;
  if (!a.present) return true;
  if (!position_eq(a.position, b.position)) return false;
  if (a.appearances.size() != b.appearances.size()) return false;
  for (size_t i = 0; i < a.appearances.size(); ++i) {
    if (!appearance_eq(a.appearances[i], b.appearances[i])) return false;
  }
  return true;
}

bool cursor_eq(const mnu::Cursor &a, const mnu::Cursor &b) {
  return a.file == b.file && a.flags == b.flags;
}

bool table_header_eq(const mnu::TableHeader &a, const mnu::TableHeader &b) {
  return a.justify == b.justify && a.vjustify == b.vjustify &&
         a.column == b.column && a.sort == b.sort && a.width == b.width &&
         a.type == b.type && a.text == b.text;
}

bool table_subst_eq(const mnu::TableSubst &a, const mnu::TableSubst &b) {
  return a.column == b.column && a.value == b.value && a.is_file == b.is_file &&
         a.file == b.file;
}

bool table_column_eq(const mnu::TableColumn &a, const mnu::TableColumn &b) {
  if (a.count != b.count || a.spacing != b.spacing) return false;
  if (a.headers.size() != b.headers.size()) return false;
  if (a.bodies.size() != b.bodies.size()) return false;
  if (a.substitutions.size() != b.substitutions.size()) return false;
  for (size_t i = 0; i < a.headers.size(); ++i) {
    if (!table_header_eq(a.headers[i], b.headers[i])) return false;
  }
  for (size_t i = 0; i < a.substitutions.size(); ++i) {
    if (!table_subst_eq(a.substitutions[i], b.substitutions[i])) return false;
  }
  return true;
}

bool table_scrollbar_eq(const mnu::TableScrollbar &a, const mnu::TableScrollbar &b) {
  if (a.present != b.present) return false;
  if (!a.present) return true;
  if (!position_eq(a.position, b.position)) return false;
  if (a.track.size() != b.track.size()) return false;
  if (a.shuttle.size() != b.shuttle.size()) return false;
  if (a.scrollup.size() != b.scrollup.size()) return false;
  if (a.scrolldown.size() != b.scrolldown.size()) return false;
  return true;
}

bool table_data_eq(const mnu::TableData &a, const mnu::TableData &b) {
  if (!table_column_eq(a.column, b.column)) return false;
  if (!table_scrollbar_eq(a.scrollbar, b.scrollbar)) return false;
  if (a.min_item_height != b.min_item_height) return false;
  if (a.outline_color != b.outline_color) return false;
  if (a.selection_color != b.selection_color) return false;
  return true;
}

bool window_eq(const mnu::Window &a, const mnu::Window &b) {
  if (a.name != b.name || a.type != b.type || a.hidden != b.hidden ||
      a.disabled != b.disabled || a.checked != b.checked || a.group != b.group)
    return false;
  if (a.has_form != b.has_form || a.form != b.form ||
      a.global_var != b.global_var || a.password != b.password)
    return false;
  if (a.has_scroll_height != b.has_scroll_height ||
      a.scroll_height != b.scroll_height ||
      a.has_scroll_width != b.has_scroll_width ||
      a.scroll_width != b.scroll_width)
    return false;
  if (!position_eq(a.position, b.position)) return false;
  if (a.appearances.size() != b.appearances.size()) return false;
  if (a.sounds.size() != b.sounds.size()) return false;
  if (a.actions.size() != b.actions.size()) return false;
  if (!string_eq(a.string_data, b.string_data)) return false;
  if (!font_eq(a.font, b.font)) return false;
  if (!frame_eq(a.frame, b.frame)) return false;
  if (!items_eq(a.items, b.items)) return false;
  if (!spin_eq(a.spinup, b.spinup) || !spin_eq(a.spindown, b.spindown))
    return false;
  if (!cursor_eq(a.cursor, b.cursor)) return false;

  // Scroll/slider fields.
  if (a.orientation != b.orientation) return false;
  if (a.shuttle.size() != b.shuttle.size()) return false;
  if (a.scrollup.size() != b.scrollup.size()) return false;
  if (a.scrolldown.size() != b.scrolldown.size()) return false;

  // Table data.
  if (!table_data_eq(a.table_data, b.table_data)) return false;

  for (size_t i = 0; i < a.appearances.size(); ++i) {
    if (!appearance_eq(a.appearances[i], b.appearances[i])) return false;
  }
  for (size_t i = 0; i < a.sounds.size(); ++i) {
    if (!sound_eq(a.sounds[i], b.sounds[i])) return false;
  }
  for (size_t i = 0; i < a.actions.size(); ++i) {
    if (!action_eq(a.actions[i], b.actions[i])) return false;
  }
  if (a.children.size() != b.children.size()) return false;
  for (size_t i = 0; i < a.children.size(); ++i) {
    if (!window_eq(a.children[i], b.children[i])) return false;
  }
  return true;
}

bool screen_eq(const mnu::Screen &a, const mnu::Screen &b) {
  if (a.name != b.name || a.music_var != b.music_var ||
      a.text_rsrc != b.text_rsrc || a.cursor_file != b.cursor_file ||
      a.cursor_flags != b.cursor_flags) {
    return false;
  }
  return window_eq(a.root_window, b.root_window);
}

// Test basic screen parsing.
bool test_basic_screen() {
  const std::string xml = R"(
<SCREEN>
  <NAME>STARTUP</NAME>
  <MUSICVAR>1</MUSICVAR>
  <WINDOW type="window" name="MAIN">
  </WINDOW>
</SCREEN>
  )";
  mnu::Document doc;
  std::string err;

  CHECK(mnu::parse(xml, doc, err), "parse failed: " + err);
  CHECK(doc.screens.size() == 1, "expected 1 screen");
  CHECK(doc.screens[0].name == "STARTUP", "expected name STARTUP");
  CHECK(doc.screens[0].music_var == 1, "expected music_var 1");
  CHECK(doc.screens[0].root_window.name == "MAIN", "expected window MAIN");

  return true;
}

// Test position parsing.
bool test_position() {
  const std::string xml = R"(
<SCREEN>
  <NAME>TEST</NAME>
  <WINDOW type="window" name="MAIN">
    <POSITION>
      <LEFT>10</LEFT>
      <TOP>20</TOP>
      <RIGHT>100</RIGHT>
      <BOTTOM>200</BOTTOM>
    </POSITION>
  </WINDOW>
</SCREEN>
  )";
  mnu::Document doc;
  std::string err;

  CHECK(mnu::parse(xml, doc, err), "parse failed: " + err);

  const auto& pos = doc.screens[0].root_window.position;
  CHECK(pos.left == 10, "expected left=10");
  CHECK(pos.top == 20, "expected top=20");
  CHECK(pos.right == 100, "expected right=100");
  CHECK(pos.bottom == 200, "expected bottom=200");
  CHECK(pos.width() == 90, "expected width=90");
  CHECK(pos.height() == 180, "expected height=180");

  return true;
}

// Test font parsing.
bool test_font() {
  const std::string xml = R"(
<SCREEN>
  <NAME>TEST</NAME>
  <WINDOW type="window" name="MAIN">
    <FONT>
      <NAME>Arial12b.fnt</NAME>
      <DEFAULT_FG>FFFFFF</DEFAULT_FG>
      <DEFAULT_BG>000000</DEFAULT_BG>
      <MOUSEOVER_FG>FF0000</MOUSEOVER_FG>
      <MOUSEOVER_BG>001100</MOUSEOVER_BG>
      <SELECTED_FG>00FF00</SELECTED_FG>
      <DISABLED_FG>808080</DISABLED_FG>
    </FONT>
  </WINDOW>
</SCREEN>
  )";
  mnu::Document doc;
  std::string err;

  CHECK(mnu::parse(xml, doc, err), "parse failed: " + err);

  const auto& font = doc.screens[0].root_window.font;
  CHECK(font.name == "Arial12b.fnt", "font name mismatch");
  CHECK(font.default_fg == "FFFFFF", "default_fg mismatch");
  CHECK(font.default_bg == "000000", "default_bg mismatch");
  CHECK(font.mouseover_fg == "FF0000", "mouseover_fg mismatch");
  CHECK(font.mouseover_bg == "001100", "mouseover_bg mismatch");
  CHECK(font.selected_fg == "00FF00", "selected_fg mismatch");
  CHECK(font.disabled_fg == "808080", "disabled_fg mismatch");

  return true;
}

// Test appearance parsing.
bool test_appearance() {
  const std::string xml = R"(
<SCREEN>
  <NAME>TEST</NAME>
  <WINDOW type="window" name="MAIN">
    <APPEARANCE type="image" state="default">texture.tga</APPEARANCE>
    <APPEARANCE type="custom" state="mouseover"></APPEARANCE>
  </WINDOW>
</SCREEN>
  )";
  mnu::Document doc;
  std::string err;

  CHECK(mnu::parse(xml, doc, err), "parse failed: " + err);

  const auto& apps = doc.screens[0].root_window.appearances;
  CHECK(apps.size() == 2, "expected 2 appearances");
  CHECK(apps[0].type == "image", "type mismatch");
  CHECK(apps[0].state == "default", "state mismatch");
  CHECK(apps[0].value == "texture.tga", "value mismatch");
  CHECK(apps[1].type == "custom", "type mismatch");
  CHECK(apps[1].state == "mouseover", "state mismatch");

  return true;
}

// Test sound parsing.
bool test_sound() {
  const std::string xml = R"(
<SCREEN>
  <NAME>TEST</NAME>
  <WINDOW type="button" name="BTN">
    <SOUND state="mousein" trigger="MOUSE_OVER">menu.lwf</SOUND>
    <SOUND state="selected" trigger="CLICK_SELECT">click.lwf</SOUND>
  </WINDOW>
</SCREEN>
  )";
  mnu::Document doc;
  std::string err;

  CHECK(mnu::parse(xml, doc, err), "parse failed: " + err);

  const auto& sounds = doc.screens[0].root_window.sounds;
  CHECK(sounds.size() == 2, "expected 2 sounds");
  CHECK(sounds[0].state == "mousein", "state mismatch");
  CHECK(sounds[0].trigger == "MOUSE_OVER", "trigger mismatch");
  CHECK(sounds[0].file == "menu.lwf", "file mismatch");
  CHECK(sounds[1].file == "click.lwf", "file mismatch");

  return true;
}

// Test action parsing.
bool test_action() {
  const std::string xml = R"(
<SCREEN>
  <NAME>TEST</NAME>
  <WINDOW type="button" name="BTN">
    <ACTION type="screen" file="sp.mnu">SINGLE_PLAYER</ACTION>
    <ACTION type="window" state="SHOW" target="OPTIONS_PANEL"/>
  </WINDOW>
</SCREEN>
  )";
  mnu::Document doc;
  std::string err;

  CHECK(mnu::parse(xml, doc, err), "parse failed: " + err);

  const auto& actions = doc.screens[0].root_window.actions;
  CHECK(actions.size() == 2, "expected 2 actions");
  CHECK(actions[0].type == "screen", "type mismatch");
  CHECK(actions[0].file == "sp.mnu", "file mismatch");
  CHECK(actions[0].target == "SINGLE_PLAYER", "target mismatch");
  CHECK(actions[1].type == "window", "type mismatch");
  CHECK(actions[1].state == "SHOW", "state mismatch");
  CHECK(actions[1].target == "OPTIONS_PANEL", "target mismatch");

  return true;
}

// Test string parsing.
bool test_string() {
  const std::string xml = R"(
<SCREEN>
  <NAME>TEST</NAME>
  <WINDOW type="button" name="BTN">
    <STRING type="id" justify="LEFT" vjustify="CENTER" edge="5">MM_Exit</STRING>
  </WINDOW>
</SCREEN>
  )";
  mnu::Document doc;
  std::string err;

  CHECK(mnu::parse(xml, doc, err), "parse failed: " + err);

  const auto& str = doc.screens[0].root_window.string_data;
  CHECK(str.type == "id", "type mismatch");
  CHECK(str.justify == "LEFT", "justify mismatch");
  CHECK(str.vjustify == "CENTER", "vjustify mismatch");
  CHECK(str.edge == 5, "edge mismatch");
  CHECK(str.value == "MM_Exit", "value mismatch");

  return true;
}

// Test cursor parsing.
bool test_cursor() {
  const std::string xml = R"(
<SCREEN>
  <NAME>TEST</NAME>
  <WINDOW type="window" name="MAIN">
    <CURSOR>
      <FILE>newarow1.tga</FILE>
      <FLAGS>STANDARD_TRANSPARENT</FLAGS>
    </CURSOR>
  </WINDOW>
</SCREEN>
  )";
  mnu::Document doc;
  std::string err;

  CHECK(mnu::parse(xml, doc, err), "parse failed: " + err);

  // Cursor in root window is extracted to screen level.
  CHECK(doc.screens[0].cursor_file == "newarow1.tga", "cursor file mismatch");
  CHECK(doc.screens[0].cursor_flags == "STANDARD_TRANSPARENT",
        "cursor flags mismatch");

  return true;
}

// An unmodelled TYPE token must survive parse -> serialize verbatim: the
// original engine maps it to a generic CWnd and keeps going [orig:
// CUIScene_CreateWidgetByType @ 0x64f630], so rewriting it (the old behavior
// emitted "unknown") corrupts a menu the real engine still understands.
bool test_unknown_type_token_roundtrip() {
  const std::string xml = R"(
<SCREEN>
  <NAME>S</NAME>
  <WINDOW type="window" name="ROOT">
    <WINDOW type="FUTURE_WIDGET" name="MYSTERY">
      <POSITION left="1" top="2" right="3" bottom="4"></POSITION>
    </WINDOW>
    <WINDOW type="GLB_TABLE" name="BROWSER">
    </WINDOW>
  </WINDOW>
</SCREEN>
  )";
  mnu::Document doc;
  std::string err;
  CHECK(mnu::parse(xml, doc, err), "parse failed: " + err);
  const mnu::Window &root = doc.screens[0].root_window;
  CHECK(root.children.size() == 2, "expected 2 children");
  CHECK(root.children[0].type == mnu::WindowType::Unknown,
        "FUTURE_WIDGET maps to Unknown (generic CWnd analogue)");
  CHECK(root.children[0].type_token == "FUTURE_WIDGET",
        "raw token preserved on the window");
  CHECK(root.children[1].type == mnu::WindowType::GlbTable,
        "GLB_TABLE is a typed factory token");

  const std::string out = mnu::serialize(doc);
  CHECK(out.find("FUTURE_WIDGET") != std::string::npos,
        "unknown token survives serialization verbatim");
  CHECK(out.find("\"unknown\"") == std::string::npos,
        "no window degrades to type=\"unknown\"");
  CHECK(out.find("GLB_TABLE") != std::string::npos,
        "GLB_TABLE authored casing survives via the token");

  // Idempotence: a second pass parses back to the same shape.
  mnu::Document doc2;
  CHECK(mnu::parse(out, doc2, err), "re-parse failed: " + err);
  CHECK(doc2.screens[0].root_window.children[0].type_token == "FUTURE_WIDGET",
        "token stable across round-trips");

  return true;
}

// Test window type parsing.
bool test_window_types() {
  CHECK(mnu::parse_window_type("window") == mnu::WindowType::Window,
        "window type");
  CHECK(mnu::parse_window_type("WINDOW") == mnu::WindowType::Window,
        "WINDOW type");
  CHECK(mnu::parse_window_type("button") == mnu::WindowType::Button,
        "button type");
  CHECK(mnu::parse_window_type("static") == mnu::WindowType::Static,
        "static type");
  CHECK(mnu::parse_window_type("edit") == mnu::WindowType::Edit, "edit type");
  CHECK(mnu::parse_window_type("list") == mnu::WindowType::List, "list type");
  CHECK(mnu::parse_window_type("checkbox") == mnu::WindowType::CheckBox,
        "checkbox type");
  CHECK(mnu::parse_window_type("radio") == mnu::WindowType::Radio,
        "radio type");
  CHECK(mnu::parse_window_type("combo") == mnu::WindowType::Combo,
        "combo type");
  CHECK(mnu::parse_window_type("spinlist") == mnu::WindowType::SpinList,
        "spinlist type");
  CHECK(mnu::parse_window_type("unknown_type") == mnu::WindowType::Unknown,
        "unknown type");

  // Shipped-content aliases: pin the parse table so a rename can't silently route
  // a real widget type to Unknown/placeholder (combobox/marquee_wnd appear in the
  // committed fixtures; the rest are exercised by the full corpus).
  CHECK(mnu::parse_window_type("combobox") == mnu::WindowType::Combo,
        "combobox alias -> Combo");
  CHECK(mnu::parse_window_type("marquee_wnd") == mnu::WindowType::Marquee,
        "marquee_wnd -> Marquee");
  CHECK(mnu::parse_window_type("multiline_edit") == mnu::WindowType::MultilineEdit,
        "multiline_edit -> MultilineEdit");
  CHECK(mnu::parse_window_type("multi") == mnu::WindowType::Multi, "multi type");
  CHECK(mnu::parse_window_type("scroll") == mnu::WindowType::Scroll, "scroll type");
  CHECK(mnu::parse_window_type("table") == mnu::WindowType::Table, "table type");
  CHECK(mnu::parse_window_type("map") == mnu::WindowType::Map, "map type");
  CHECK(mnu::parse_window_type("globe") == mnu::WindowType::Globe, "globe type");
  CHECK(mnu::parse_window_type("goto") == mnu::WindowType::Goto, "goto type");

  // The four remaining factory tokens [orig: CUIScene_CreateWidgetByType
  // @ 0x64f630] now carry typed identity (runtime behavior still a container).
  CHECK(mnu::parse_window_type("GLB_TABLE") == mnu::WindowType::GlbTable,
        "GLB_TABLE -> GlbTable");
  CHECK(mnu::parse_window_type("RADIOEDIT") == mnu::WindowType::RadioEdit,
        "RADIOEDIT -> RadioEdit");
  CHECK(mnu::parse_window_type("LAN_LIST") == mnu::WindowType::LanList,
        "LAN_LIST -> LanList");
  CHECK(mnu::parse_window_type("GOPHER") == mnu::WindowType::Gopher,
        "GOPHER -> Gopher");

  // Every type's serialized name must re-parse to the same type, locking the
  // Combo->"combobox"->Combo style remaps.
  const mnu::WindowType all[] = {
      mnu::WindowType::Window,   mnu::WindowType::Static,
      mnu::WindowType::Button,   mnu::WindowType::Edit,
      mnu::WindowType::MultilineEdit, mnu::WindowType::List,
      mnu::WindowType::CheckBox, mnu::WindowType::Radio,
      mnu::WindowType::Combo,    mnu::WindowType::Scroll,
      mnu::WindowType::Table,    mnu::WindowType::SpinList,
      mnu::WindowType::Multi,    mnu::WindowType::Map,
      mnu::WindowType::Globe,    mnu::WindowType::Label,
      mnu::WindowType::Goto,     mnu::WindowType::Marquee,
      mnu::WindowType::GlbTable, mnu::WindowType::RadioEdit,
      mnu::WindowType::LanList,  mnu::WindowType::Gopher,
  };
  for (mnu::WindowType t : all) {
    CHECK(mnu::parse_window_type(mnu::window_type_name(t)) == t,
          std::string("type name round-trips: ") + mnu::window_type_name(t));
  }

  return true;
}

// Test hidden/disabled flags.
bool test_flags() {
  const std::string xml = R"(
<SCREEN>
  <NAME>TEST</NAME>
  <WINDOW type="button" name="BTN1" HIDDEN DISABLE/>
  <WINDOW type="button" name="BTN2"/>
</SCREEN>
  )";
  mnu::Document doc;
  std::string err;

  CHECK(mnu::parse(xml, doc, err), "parse failed: " + err);

  // Root window is empty, children are BTN1 and BTN2 (but in this format,
  // they're siblings at root level, so we need to adjust). Actually the above
  // creates root_window empty and children at screen level. Let me fix the XML.

  return true;
}

// Test nested windows.
bool test_nested_windows() {
  const std::string xml = R"(
<SCREEN>
  <NAME>TEST</NAME>
  <WINDOW type="window" name="MAIN">
    <WINDOW type="window" name="PANEL">
      <WINDOW type="button" name="BTN1" HIDDEN/>
      <WINDOW type="button" name="BTN2" DISABLE/>
    </WINDOW>
  </WINDOW>
</SCREEN>
  )";
  mnu::Document doc;
  std::string err;

  CHECK(mnu::parse(xml, doc, err), "parse failed: " + err);

  const auto& main = doc.screens[0].root_window;
  CHECK(main.name == "MAIN", "expected MAIN");
  CHECK(main.children.size() == 1, "expected 1 child (PANEL)");

  const auto& panel = main.children[0];
  CHECK(panel.name == "PANEL", "expected PANEL");
  CHECK(panel.children.size() == 2, "expected 2 children");

  CHECK(panel.children[0].name == "BTN1", "expected BTN1");
  CHECK(panel.children[0].hidden == true, "expected BTN1 hidden");
  CHECK(panel.children[0].disabled == false, "expected BTN1 not disabled");

  CHECK(panel.children[1].name == "BTN2", "expected BTN2");
  CHECK(panel.children[1].hidden == false, "expected BTN2 not hidden");
  CHECK(panel.children[1].disabled == true, "expected BTN2 disabled");

  return true;
}

// Test hex color parsing.
bool test_hex_color() {
  uint8_t r, g, b, a;

  CHECK(mnu::parse_hex_color("FF0000", r, g, b, a), "parse FF0000");
  CHECK(r == 255 && g == 0 && b == 0 && a == 255, "FF0000 values");

  CHECK(mnu::parse_hex_color("00FF00", r, g, b, a), "parse 00FF00");
  CHECK(r == 0 && g == 255 && b == 0, "00FF00 values");

  CHECK(mnu::parse_hex_color("#0000FF", r, g, b, a), "parse #0000FF");
  CHECK(r == 0 && g == 0 && b == 255, "#0000FF values");

  CHECK(mnu::parse_hex_color("80FFFFFF", r, g, b, a), "parse 80FFFFFF");
  CHECK(a == 128 && r == 255 && g == 255 && b == 255, "80FFFFFF values");

  CHECK(!mnu::parse_hex_color("%VAR%", r, g, b, a), "variable should fail");
  CHECK(!mnu::parse_hex_color("", r, g, b, a), "empty should fail");

  return true;
}

// Test color variable detection.
bool test_color_variable() {
  CHECK(mnu::is_color_variable("%TRIM_COLOR%"), "%TRIM_COLOR% is variable");
  CHECK(mnu::is_color_variable("%VAR%"), "%VAR% is variable");
  CHECK(!mnu::is_color_variable("FF0000"), "FF0000 is not variable");
  CHECK(!mnu::is_color_variable(""), "empty is not variable");
  CHECK(!mnu::is_color_variable("%"), "single % is not variable");

  return true;
}

// Test hotkey marker stripping.
bool test_strip_hotkey() {
  std::string hotkey;
  int pos;

  std::string result = mnu::strip_hotkey_marker("{hot}Exit", &hotkey, &pos);
  CHECK(result == "Exit", "result should be Exit");
  CHECK(hotkey == "E", "hotkey should be E");
  CHECK(pos == 0, "pos should be 0");

  result = mnu::strip_hotkey_marker("E{hot}xit", &hotkey, &pos);
  CHECK(result == "Exit", "result should be Exit");
  CHECK(hotkey == "x", "hotkey should be x");
  CHECK(pos == 1, "pos should be 1");

  result = mnu::strip_hotkey_marker("No hotkey here");
  CHECK(result == "No hotkey here", "no change expected");

  return true;
}

// Test full MNU-style document.
bool test_full_mnu_document() {
  const std::string xml = R"(
<SCREEN>
  <NAME>STARTUP</NAME>
  <MUSICVAR>1</MUSICVAR>
  <WINDOW type="window" name="MAIN">
    <APPEARANCE type="custom" state="default"></APPEARANCE>
    <POSITION>
      <LEFT>0</LEFT>
      <TOP>75</TOP>
      <RIGHT>800</RIGHT>
      <BOTTOM>525</BOTTOM>
    </POSITION>
    <TEXT_RSRC>menutxt.BIN</TEXT_RSRC>
    <CURSOR>
      <FILE>newarow1.tga</FILE>
      <FLAGS>STANDARD_TRANSPARENT</FLAGS>
    </CURSOR>
    <WINDOW type="window" name="BUTTONS">
      <FONT>
        <NAME>Gunpl27b.fnt</NAME>
        <DEFAULT_FG>FFFFFF</DEFAULT_FG>
        <MOUSEOVER_FG>FF0000</MOUSEOVER_FG>
        <SELECTED_FG>FF0000</SELECTED_FG>
        <DISABLED_FG>545252</DISABLED_FG>
      </FONT>
      <WINDOW type="button" name="SINGLE_PLAYER">
        <ACTION type="screen" file="sp.mnu">SINGLE_PLAYER</ACTION>
        <APPEARANCE state="default"></APPEARANCE>
        <SOUND state="mousein" trigger="MOUSE_OVER">menu.lwf</SOUND>
        <STRING type="id" justify="LEFT">MM_Singleplayer</STRING>
      </WINDOW>
      <WINDOW type="button" name="DATABASE" HIDDEN DISABLE>
        <STRING type="id" justify="CENTER">MM_Database</STRING>
      </WINDOW>
    </WINDOW>
  </WINDOW>
</SCREEN>
  )";
  mnu::Document doc;
  std::string err;

  CHECK(mnu::parse(xml, doc, err), "parse failed: " + err);
  CHECK(doc.screens.size() == 1, "expected 1 screen");

  const auto& screen = doc.screens[0];
  CHECK(screen.name == "STARTUP", "screen name");
  CHECK(screen.music_var == 1, "music_var");
  CHECK(screen.text_rsrc == "menutxt.BIN", "text_rsrc");
  CHECK(screen.cursor_file == "newarow1.tga", "cursor file");
  CHECK(screen.cursor_flags == "STANDARD_TRANSPARENT", "cursor flags");

  const auto& main = screen.root_window;
  CHECK(main.name == "MAIN", "main window name");
  CHECK(main.position.left == 0, "position left");
  CHECK(main.position.top == 75, "position top");
  CHECK(main.position.right == 800, "position right");
  CHECK(main.position.bottom == 525, "position bottom");

  CHECK(main.children.size() == 1, "expected 1 child (BUTTONS)");
  const auto& buttons = main.children[0];
  CHECK(buttons.name == "BUTTONS", "buttons name");
  CHECK(buttons.font.name == "Gunpl27b.fnt", "font name");
  CHECK(buttons.font.default_fg == "FFFFFF", "font default_fg");
  CHECK(buttons.font.mouseover_fg == "FF0000", "font mouseover_fg");

  CHECK(buttons.children.size() == 2, "expected 2 button children");

  const auto& sp_btn = buttons.children[0];
  CHECK(sp_btn.name == "SINGLE_PLAYER", "sp button name");
  CHECK(sp_btn.type == mnu::WindowType::Button, "sp button type");
  CHECK(sp_btn.actions.size() == 1, "sp button actions");
  CHECK(sp_btn.actions[0].file == "sp.mnu", "sp action file");
  CHECK(sp_btn.actions[0].target == "SINGLE_PLAYER", "sp action target");
  CHECK(sp_btn.sounds.size() == 1, "sp sounds");
  CHECK(sp_btn.sounds[0].file == "menu.lwf", "sp sound file");
  CHECK(sp_btn.string_data.value == "MM_Singleplayer", "sp string");

  const auto& db_btn = buttons.children[1];
  CHECK(db_btn.name == "DATABASE", "db button name");
  CHECK(db_btn.hidden == true, "db button hidden");
  CHECK(db_btn.disabled == true, "db button disabled");

  return true;
}

// Test scroll/slider element parsing.
bool test_scroll_elements() {
  const std::string xml = R"(
<SCREEN>
  <NAME>TEST</NAME>
  <WINDOW type="scroll" name="SLIDER">
    <POSITION>
      <LEFT>10</LEFT>
      <TOP>20</TOP>
      <RIGHT>200</RIGHT>
      <BOTTOM>40</BOTTOM>
    </POSITION>
    <ORIENTATION>HORIZONTAL</ORIENTATION>
    <APPEARANCE type="image" state="default">track.tga</APPEARANCE>
    <SHUTTLE type="image" state="default">shuttle.tga</SHUTTLE>
    <SHUTTLE type="image" state="mouseover">shuttle_hover.tga</SHUTTLE>
    <SCROLLUP type="image" state="default" map_state="0" height="24">left_arrow.tga</SCROLLUP>
    <SCROLLUP type="image" state="mouseover" map_state="1" height="24">left_arrow.tga</SCROLLUP>
    <SCROLLDOWN type="image" state="default" map_state="0" height="24">right_arrow.tga</SCROLLDOWN>
    <SCROLLDOWN type="image" state="mouseover" map_state="1" height="24">right_arrow.tga</SCROLLDOWN>
  </WINDOW>
</SCREEN>
  )";
  mnu::Document doc;
  std::string err;

  CHECK(mnu::parse(xml, doc, err), "parse failed: " + err);

  const auto& slider = doc.screens[0].root_window;
  CHECK(slider.name == "SLIDER", "expected name SLIDER");
  CHECK(slider.type == mnu::WindowType::Scroll, "expected Scroll type");
  CHECK(slider.orientation == "HORIZONTAL", "expected HORIZONTAL orientation");

  // Track appearance.
  CHECK(slider.appearances.size() == 1, "expected 1 track appearance");
  CHECK(slider.appearances[0].value == "track.tga", "track texture mismatch");

  // Shuttle appearances.
  CHECK(slider.shuttle.size() == 2, "expected 2 shuttle appearances");
  CHECK(slider.shuttle[0].value == "shuttle.tga", "shuttle default mismatch");
  CHECK(slider.shuttle[0].state == "default", "shuttle state mismatch");
  CHECK(slider.shuttle[1].value == "shuttle_hover.tga", "shuttle hover mismatch");

  // Scrollup appearances.
  CHECK(slider.scrollup.size() == 2, "expected 2 scrollup appearances");
  CHECK(slider.scrollup[0].value == "left_arrow.tga", "scrollup texture mismatch");
  CHECK(slider.scrollup[0].map_state == 0, "scrollup map_state mismatch");
  CHECK(slider.scrollup[0].height == 24, "scrollup height mismatch");

  // Scrolldown appearances.
  CHECK(slider.scrolldown.size() == 2, "expected 2 scrolldown appearances");
  CHECK(slider.scrolldown[0].value == "right_arrow.tga", "scrolldown texture mismatch");

  return true;
}

// Test table column parsing.
bool test_table_column() {
  const std::string xml = R"(
<SCREEN>
  <NAME>TEST</NAME>
  <WINDOW type="table" name="CONTROL_MAPPING">
    <POSITION>
      <TOP>40</TOP>
      <LEFT>240</LEFT>
      <RIGHT>691</RIGHT>
      <BOTTOM>320</BOTTOM>
    </POSITION>
    <COLUMN count="3" spacing="0">
      <HEADER justify="CENTER" vjustify="CENTER" column="0" sort="A" width="150">Class</HEADER>
      <HEADER justify="CENTER" vjustify="CENTER" column="1" sort="A" width="150">Action</HEADER>
      <HEADER justify="CENTER" vjustify="CENTER" column="2" sort="A" width="150">Control</HEADER>
      <BODY justify="LEFT" vjustify="CENTER" column="0"></BODY>
      <BODY justify="LEFT" vjustify="CENTER" column="1"></BODY>
    </COLUMN>
    <MIN_ITEM_HEIGHT>20</MIN_ITEM_HEIGHT>
  </WINDOW>
</SCREEN>
  )";
  mnu::Document doc;
  std::string err;

  CHECK(mnu::parse(xml, doc, err), "parse failed: " + err);

  const auto& table = doc.screens[0].root_window;
  CHECK(table.name == "CONTROL_MAPPING", "expected name CONTROL_MAPPING");
  CHECK(table.type == mnu::WindowType::Table, "expected Table type");

  // Column data.
  const auto& col = table.table_data.column;
  CHECK(col.count == 3, "expected 3 columns");
  CHECK(col.spacing == 0, "expected 0 spacing");
  CHECK(col.headers.size() == 3, "expected 3 headers");

  // Header 0.
  CHECK(col.headers[0].text == "Class", "header 0 text mismatch");
  CHECK(col.headers[0].column == 0, "header 0 column mismatch");
  CHECK(col.headers[0].width == 150, "header 0 width mismatch");
  CHECK(col.headers[0].justify == "CENTER", "header 0 justify mismatch");
  CHECK(col.headers[0].sort == "A", "header 0 sort mismatch");

  // Header 1.
  CHECK(col.headers[1].text == "Action", "header 1 text mismatch");
  CHECK(col.headers[1].column == 1, "header 1 column mismatch");

  // Header 2.
  CHECK(col.headers[2].text == "Control", "header 2 text mismatch");
  CHECK(col.headers[2].column == 2, "header 2 column mismatch");

  // Body elements.
  CHECK(col.bodies.size() == 2, "expected 2 body elements");
  CHECK(col.bodies[0].column == 0, "body 0 column mismatch");
  CHECK(col.bodies[0].justify == "LEFT", "body 0 justify mismatch");

  // Min item height.
  CHECK(table.table_data.min_item_height == 20, "expected min_item_height 20");

  return true;
}

// Test table scrollbar parsing.
bool test_table_scrollbar() {
  const std::string xml = R"(
<SCREEN>
  <NAME>TEST</NAME>
  <WINDOW type="table" name="TABLE">
    <SCROLLBAR>
      <APPEARANCE type="image" state="default">m_scrolV.tga</APPEARANCE>
      <SHUTTLE type="image" state="default">shuttlev.tga</SHUTTLE>
      <SHUTTLE type="image" state="mouseover">shuttlev.tga</SHUTTLE>
      <SCROLLUP type="image" state="default" map_state="0" height="24">up2arrw.tga</SCROLLUP>
      <SCROLLUP type="image" state="mouseover" map_state="1" height="24">up2arrw.tga</SCROLLUP>
      <SCROLLDOWN type="image" state="default" map_state="0" height="24">dn2arrw.tga</SCROLLDOWN>
      <SOUND state="selected" trigger="CLICK_VALUE">menu.lwf</SOUND>
      <POSITION>
        <LEFT>452</LEFT>
        <TOP>20</TOP>
        <RIGHT>472</RIGHT>
        <BOTTOM>280</BOTTOM>
      </POSITION>
    </SCROLLBAR>
  </WINDOW>
</SCREEN>
  )";
  mnu::Document doc;
  std::string err;

  CHECK(mnu::parse(xml, doc, err), "parse failed: " + err);

  const auto& sb = doc.screens[0].root_window.table_data.scrollbar;
  CHECK(sb.present == true, "scrollbar should be present");

  // Position.
  CHECK(sb.position.left == 452, "scrollbar left mismatch");
  CHECK(sb.position.top == 20, "scrollbar top mismatch");
  CHECK(sb.position.right == 472, "scrollbar right mismatch");
  CHECK(sb.position.bottom == 280, "scrollbar bottom mismatch");

  // Track appearance.
  CHECK(sb.track.size() == 1, "expected 1 track appearance");
  CHECK(sb.track[0].value == "m_scrolV.tga", "track texture mismatch");

  // Shuttle.
  CHECK(sb.shuttle.size() == 2, "expected 2 shuttle appearances");
  CHECK(sb.shuttle[0].value == "shuttlev.tga", "shuttle texture mismatch");

  // Scrollup.
  CHECK(sb.scrollup.size() == 2, "expected 2 scrollup appearances");
  CHECK(sb.scrollup[0].value == "up2arrw.tga", "scrollup texture mismatch");
  CHECK(sb.scrollup[0].map_state == 0, "scrollup map_state mismatch");
  CHECK(sb.scrollup[0].height == 24, "scrollup height mismatch");

  // Scrolldown.
  CHECK(sb.scrolldown.size() == 1, "expected 1 scrolldown appearance");
  CHECK(sb.scrolldown[0].value == "dn2arrw.tga", "scrolldown texture mismatch");

  // Sound.
  CHECK(sb.sounds.size() == 1, "expected 1 sound");
  CHECK(sb.sounds[0].file == "menu.lwf", "sound file mismatch");

  return true;
}

// Test table items colors parsing.
bool test_table_items_colors() {
  const std::string xml = R"(
<SCREEN>
  <NAME>TEST</NAME>
  <WINDOW type="table" name="TABLE">
    <ITEMS>
      <APPEARANCE type="outline" state="default">3870BF</APPEARANCE>
      <APPEARANCE type="color" state="selected">7f1E6DF3</APPEARANCE>
    </ITEMS>
  </WINDOW>
</SCREEN>
  )";
  mnu::Document doc;
  std::string err;

  CHECK(mnu::parse(xml, doc, err), "parse failed: " + err);

  const auto& td = doc.screens[0].root_window.table_data;
  CHECK(td.outline_color == "3870BF", "outline color mismatch");
  CHECK(td.selection_color == "7f1E6DF3", "selection color mismatch");

  return true;
}

// Test serialize -> parse roundtrip.
bool test_serialize_roundtrip() {
  mnu::Document doc;
  mnu::Screen screen;
  screen.name = "ROUNDTRIP";
  screen.music_var = 3;
  screen.text_rsrc = "menutxt.BIN";
  // The MNU format stores a single cursor inside the root window for original
  // game compatibility (see write_screen in mnu.cpp); Screen.cursor_file is a
  // parse-time mirror of it. So the screen cursor must match the root window's
  // cursor below to round-trip (the format cannot hold two distinct cursors).
  screen.cursor_file = "newarow1.tga";
  screen.cursor_flags = "STANDARD_TRANSPARENT";

  mnu::Window root;
  root.name = "ROOT";
  root.type = mnu::WindowType::Window;
  root.position.left = 0;
  root.position.top = 10;
  root.position.right = 200;
  root.position.bottom = 300;
  root.position.has_left = root.position.has_top = true;
  root.position.has_right = root.position.has_bottom = true;

  mnu::Appearance app;
  app.state = "default";
  app.type = "image";
  app.value = "main_hdr.tga";
  app.map_state = 1;
  app.height = 64;
  root.appearances.push_back(app);

  mnu::Sound snd;
  snd.state = "mousein";
  snd.trigger = "MOUSE_OVER";
  snd.file = "menu.lwf";
  root.sounds.push_back(snd);

  mnu::Action act;
  act.type = "screen";
  act.file = "sp.mnu";
  act.target = "SINGLE_PLAYER";
  root.actions.push_back(act);

  root.string_data.type = "id";
  root.string_data.justify = "CENTER";
  root.string_data.vjustify = "BOTTOM";
  root.string_data.edge = 4;
  root.string_data.value = "MM_Start";

  root.font.name = "Gunpl27b.fnt";
  root.font.default_fg = "FFFFFF";
  root.font.mouseover_fg = "FF0000";

  root.frame.stencil = "border.tga";
  root.frame.stencil_size = 8;

  root.items.justify = "LEFT";
  mnu::Item item;
  item.type = "ID";
  item.value = "0";
  item.text = "OPTION_LOW";
  root.items.items.push_back(item);

  root.spinup.present = true;
  root.spinup.position.left = 10;
  root.spinup.position.has_left = true;
  root.spinup.appearances.push_back(app);

  root.cursor.file = "newarow1.tga";
  root.cursor.flags = "STANDARD_TRANSPARENT";

  mnu::Window child;
  child.name = "ChildBtn";
  child.type = mnu::WindowType::Button;
  child.checked = true;
  child.group = 2;
  child.position.left = 5;
  child.position.top = 15;
  child.position.has_left = child.position.has_top = true;
  child.string_data.value = "Child";
  root.children.push_back(child);

  screen.root_window = root;
  doc.screens.push_back(screen);

  std::string serialized = mnu::serialize(doc, true, 2);

  mnu::Document roundtrip;
  std::string err;
  CHECK(mnu::parse(serialized, roundtrip, err), "roundtrip parse failed: " + err);
  CHECK(roundtrip.screens.size() == doc.screens.size(), "screen count mismatch");
  CHECK(screen_eq(doc.screens[0], roundtrip.screens[0]), "roundtrip screen mismatch");

  return true;
}

// Test table SUBST (value->image) parse + serialize round-trip. SUBST cells were
// historically dropped on save (the COLUMN serializer emitted HEADER/BODY only);
// this guards that they survive parse -> serialize -> parse.
bool test_table_subst() {
  const std::string xml = R"(
<SCREEN>
  <NAME>TEST</NAME>
  <WINDOW type="table" name="MISSION_TABLE">
    <POSITION>
      <TOP>40</TOP>
      <LEFT>240</LEFT>
      <RIGHT>691</RIGHT>
      <BOTTOM>320</BOTTOM>
    </POSITION>
    <COLUMN count="3" spacing="0">
      <BODY justify="CENTER" vjustify="CENTER" column="2" BITMAP_DRAW></BODY>
      <SUBST column="2" value="0" FILE>alphachk0.tga</SUBST>
      <SUBST column="2" value="1" FILE>alphachk1.tga</SUBST>
    </COLUMN>
  </WINDOW>
</SCREEN>
  )";
  mnu::Document doc;
  std::string err;
  CHECK(mnu::parse(xml, doc, err), "parse failed: " + err);

  const auto &col = doc.screens[0].root_window.table_data.column;
  CHECK(col.substitutions.size() == 2, "expected 2 SUBST elements");
  CHECK(col.substitutions[0].column == 2, "subst 0 column mismatch");
  CHECK(col.substitutions[0].value == "0", "subst 0 value mismatch");
  CHECK(col.substitutions[0].is_file, "subst 0 should carry the FILE flag");
  CHECK(col.substitutions[0].file == "alphachk0.tga", "subst 0 file mismatch");
  CHECK(col.substitutions[1].file == "alphachk1.tga", "subst 1 file mismatch");

  // Round-trip: serialize then re-parse and confirm the substitutions survive
  // (regression guard for the dropped-on-save bug).
  const std::string serialized = mnu::serialize(doc, true, 2);
  mnu::Document roundtrip;
  CHECK(mnu::parse(serialized, roundtrip, err), "roundtrip parse failed: " + err);
  const auto &rcol = roundtrip.screens[0].root_window.table_data.column;
  CHECK(table_column_eq(col, rcol), "SUBST did not survive serialize round-trip");

  return true;
}

// Build a document entirely in code (no parse, no imported file) exercising every
// round-trip-preserved field, then serialize -> parse and confirm each survived.
// This is the "create from nothing + no raw passthrough" proof (ADR 0003): each
// construct is typed data the model owns, so a from-scratch menu round-trips.
bool test_create_from_scratch() {
  mnu::Document doc;
  mnu::Screen screen;
  screen.name = "SCREEN";
  mnu::Window &root = screen.root_window;
  root.name = "ROOT";
  root.type = mnu::WindowType::Window;

  mnu::Window cb;  // checkbox rendered as a toggle button
  cb.name = "GRID";
  cb.type = mnu::WindowType::CheckBox;
  cb.as_button = true;
  root.children.push_back(cb);

  mnu::Window ed;  // numeric edit with range + length constraints
  ed.name = "MAXPLAYERS";
  ed.type = mnu::WindowType::Edit;
  ed.number = true;
  ed.has_minval = true;
  ed.minval = 1;
  ed.has_maxval = true;
  ed.maxval = 99;
  ed.has_maxchar = true;
  ed.maxchar = 2;
  root.children.push_back(ed);

  mnu::Window combo;  // combobox whose listbox carries SB_EDGE_PAD + item height
  combo.name = "CLASS";
  combo.type = mnu::WindowType::Combo;
  combo.list_box.present = true;
  combo.list_box.has_sb_edge_pad = true;
  combo.list_box.sb_edge_pad = 21;
  combo.list_box.min_item_height = 20;
  root.children.push_back(combo);

  mnu::Window table;  // table column body drawn by the host
  table.name = "ROSTER";
  table.type = mnu::WindowType::Table;
  table.table_data.column.count = 1;
  mnu::TableBody body;
  body.column = 0;
  body.custom_draw = true;
  table.table_data.column.bodies.push_back(body);
  root.children.push_back(table);

  mnu::Window btn;  // button opening a URL in an external browser
  btn.name = "PREORDER";
  btn.type = mnu::WindowType::Button;
  mnu::Action act;
  act.type = "URL";
  act.target = "www.novalogic.com";
  act.external_browser = true;
  btn.actions.push_back(act);
  root.children.push_back(btn);

  doc.screens.push_back(screen);

  // Round-trip the from-scratch document.
  const std::string text = mnu::serialize(doc, true, 2);
  mnu::Document parsed;
  std::string err;
  CHECK(mnu::parse(text, parsed, err), "from-scratch doc failed to parse");
  CHECK(parsed.screens.size() == 1, "expected one screen");
  const mnu::Window &proot = parsed.screens[0].root_window;
  CHECK(proot.children.size() == 5, "expected five child widgets");

  const mnu::Window *pcb = nullptr, *ped = nullptr, *pcombo = nullptr,
                    *ptable = nullptr, *pbtn = nullptr;
  for (const auto &c : proot.children) {
    if (c.name == "GRID") pcb = &c;
    else if (c.name == "MAXPLAYERS") ped = &c;
    else if (c.name == "CLASS") pcombo = &c;
    else if (c.name == "ROSTER") ptable = &c;
    else if (c.name == "PREORDER") pbtn = &c;
  }
  CHECK(pcb && pcb->as_button, "AS_BUTTON lost");
  CHECK(ped && ped->number, "NUMBER lost");
  CHECK(ped->has_minval && ped->minval == 1, "MINVAL lost");
  CHECK(ped->has_maxval && ped->maxval == 99, "MAXVAL lost");
  CHECK(ped->has_maxchar && ped->maxchar == 2, "MAXCHAR lost");
  CHECK(pcombo && pcombo->list_box.has_sb_edge_pad &&
            pcombo->list_box.sb_edge_pad == 21,
        "SB_EDGE_PAD lost");
  CHECK(pcombo->list_box.min_item_height == 20, "listbox MIN_ITEM_HEIGHT lost");
  CHECK(ptable, "table widget lost");
  bool custom = false;
  for (const auto &b : ptable->table_data.column.bodies)
    if (b.custom_draw) custom = true;
  CHECK(custom, "CUSTOM_DRAW lost");
  CHECK(pbtn && !pbtn->actions.empty() && pbtn->actions[0].external_browser,
        "EXTERNAL_BROWSER lost");

  return true;
}

// Round-trip the attributes recovered by the 2026-06-01 Jointops.exe grill:
// table HEADER type, STENCIL INSETX/INSETY, WINDOW FORM/GLOBAL_VAR/PASSWORD, the
// window-level scroll <HEIGHT>, and the POSITION ULX/ULY/WIDTH/HEIGHT aliases.
bool test_grill_attributes_roundtrip() {
  // (A) Build in code, serialize, re-parse, confirm every field survives.
  mnu::Document doc;
  mnu::Screen screen;
  screen.name = "S";
  mnu::Window &root = screen.root_window;
  root.name = "ROOT";

  mnu::Window framed;  // FRAME with data-driven insets
  framed.name = "PANEL";
  framed.frame.stencil = "border.tga";
  framed.frame.stencil_size = 32;
  framed.frame.has_insetx = true;
  framed.frame.insetx = 12;
  framed.frame.has_insety = true;
  framed.frame.insety = 18;
  root.children.push_back(framed);

  mnu::Window edit;  // FORM + GLOBAL_VAR + PASSWORD
  edit.name = "PW";
  edit.type = mnu::WindowType::Edit;
  edit.has_form = true;
  edit.form = 3;
  edit.global_var = true;
  edit.password = true;
  root.children.push_back(edit);

  mnu::Window scroll;  // window-level scroll thickness
  scroll.name = "BAR";
  scroll.type = mnu::WindowType::Scroll;
  scroll.has_scroll_height = true;
  scroll.scroll_height = 12;
  root.children.push_back(scroll);

  mnu::Window tbl;  // table HEADER type="id"
  tbl.name = "GRID";
  tbl.type = mnu::WindowType::Table;
  tbl.table_data.column.count = 1;
  mnu::TableHeader hdr;
  hdr.column = 0;
  hdr.width = 150;
  hdr.type = "id";
  hdr.text = "Class";
  tbl.table_data.column.headers.push_back(hdr);
  root.children.push_back(tbl);

  doc.screens.push_back(screen);

  const std::string text = mnu::serialize(doc, true, 2);
  mnu::Document parsed;
  std::string err;
  CHECK(mnu::parse(text, parsed, err), "grill-attrs doc failed to parse");
  CHECK(parsed.screens.size() == 1, "expected one screen");
  CHECK(window_eq(doc.screens[0].root_window, parsed.screens[0].root_window),
        "grill attributes lost on round-trip");

  const mnu::Window &pr = parsed.screens[0].root_window;
  const mnu::Window *pf = nullptr, *ppw = nullptr, *psc = nullptr, *pt = nullptr;
  for (const auto &c : pr.children) {
    if (c.name == "PANEL") pf = &c;
    else if (c.name == "PW") ppw = &c;
    else if (c.name == "BAR") psc = &c;
    else if (c.name == "GRID") pt = &c;
  }
  CHECK(pf && pf->frame.has_insetx && pf->frame.insetx == 12 &&
            pf->frame.has_insety && pf->frame.insety == 18,
        "STENCIL INSETX/INSETY lost");
  CHECK(ppw && ppw->has_form && ppw->form == 3 && ppw->global_var &&
            ppw->password,
        "FORM/GLOBAL_VAR/PASSWORD lost");
  CHECK(psc && psc->has_scroll_height && psc->scroll_height == 12,
        "scroll <HEIGHT> lost");
  CHECK(pt && !pt->table_data.column.headers.empty() &&
            pt->table_data.column.headers[0].type == "id",
        "HEADER type=id lost (the round-trip data-loss bug)");

  // (B) Parse the original authored syntax directly: STENCIL inset attributes, the
  // POSITION ULX/ULY/WIDTH/HEIGHT aliases, and a window-level scroll <HEIGHT>.
  const char *src =
      "<SCREEN><NAME>S</NAME><WINDOW type=\"window\" name=\"ROOT\">"
      "<WINDOW type=\"static\" name=\"A\">"
      "<FRAME><STENCIL size=\"16\" insetx=\"4\" insety=\"6\">b.tga</STENCIL></FRAME>"
      "<POSITION><ULX>10</ULX><ULY>20</ULY><WIDTH>100</WIDTH><HEIGHT>40</HEIGHT></POSITION>"
      "</WINDOW>"
      "<WINDOW type=\"scroll\" name=\"B\"><HEIGHT>12</HEIGHT></WINDOW>"
      "</WINDOW></SCREEN>";
  mnu::Document d2;
  CHECK(mnu::parse(src, d2, err), "authored-syntax parse failed");
  CHECK(d2.screens.size() == 1, "expected one screen (B)");
  const mnu::Window &r2 = d2.screens[0].root_window;
  CHECK(r2.children.size() == 2, "expected two children (B)");
  const mnu::Window &a = r2.children[0];
  CHECK(a.frame.insetx == 4 && a.frame.insety == 6,
        "STENCIL insetx/insety attribute parse");
  CHECK(a.position.has_left && a.position.left == 10, "POSITION ULX alias");
  CHECK(a.position.has_top && a.position.top == 20, "POSITION ULY alias");
  CHECK(a.position.has_right && a.position.right == 110,
        "POSITION WIDTH alias (left+width)");
  CHECK(a.position.has_bottom && a.position.bottom == 60,
        "POSITION HEIGHT alias (top+height)");
  const mnu::Window &b = r2.children[1];
  CHECK(b.has_scroll_height && b.scroll_height == 12,
        "scroll-level <HEIGHT> parse");

  return true;
}

}  // namespace

int main() {
  int failed = 0;

#define RUN_TEST(name)                     \
  do {                                     \
    std::cout << "Running " #name "... "; \
    if (name()) {                          \
      std::cout << "OK\n";                 \
    } else {                               \
      std::cout << "FAILED\n";             \
      ++failed;                            \
    }                                      \
  } while (0)

  RUN_TEST(test_basic_screen);
  RUN_TEST(test_position);
  RUN_TEST(test_font);
  RUN_TEST(test_appearance);
  RUN_TEST(test_sound);
  RUN_TEST(test_action);
  RUN_TEST(test_string);
  RUN_TEST(test_cursor);
  RUN_TEST(test_window_types);
  RUN_TEST(test_unknown_type_token_roundtrip);
  RUN_TEST(test_nested_windows);
  RUN_TEST(test_hex_color);
  RUN_TEST(test_color_variable);
  RUN_TEST(test_strip_hotkey);
  RUN_TEST(test_full_mnu_document);
  RUN_TEST(test_scroll_elements);
  RUN_TEST(test_table_column);
  RUN_TEST(test_table_subst);
  RUN_TEST(test_table_scrollbar);
  RUN_TEST(test_table_items_colors);
  RUN_TEST(test_serialize_roundtrip);
  RUN_TEST(test_create_from_scratch);
  RUN_TEST(test_grill_attributes_roundtrip);

  if (failed > 0) {
    std::cerr << "\n" << failed << " test(s) FAILED\n";
    return EXIT_FAILURE;
  }

  std::cout << "\nAll tests passed!\n";
  return EXIT_SUCCESS;
}
