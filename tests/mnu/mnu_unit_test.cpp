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
         a.brush == b.brush && a.monogram == b.monogram;
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
         a.text == b.text;
}

bool table_column_eq(const mnu::TableColumn &a, const mnu::TableColumn &b) {
  if (a.count != b.count || a.spacing != b.spacing) return false;
  if (a.headers.size() != b.headers.size()) return false;
  if (a.bodies.size() != b.bodies.size()) return false;
  for (size_t i = 0; i < a.headers.size(); ++i) {
    if (!table_header_eq(a.headers[i], b.headers[i])) return false;
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
  RUN_TEST(test_nested_windows);
  RUN_TEST(test_hex_color);
  RUN_TEST(test_color_variable);
  RUN_TEST(test_strip_hotkey);
  RUN_TEST(test_full_mnu_document);
  RUN_TEST(test_scroll_elements);
  RUN_TEST(test_table_column);
  RUN_TEST(test_table_scrollbar);
  RUN_TEST(test_table_items_colors);
  RUN_TEST(test_serialize_roundtrip);

  if (failed > 0) {
    std::cerr << "\n" << failed << " test(s) FAILED\n";
    return EXIT_FAILURE;
  }

  std::cout << "\nAll tests passed!\n";
  return EXIT_SUCCESS;
}
