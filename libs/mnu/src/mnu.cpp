// MNU menu file parser implementation.
// Converts generic XML nodes from mnu_xml into typed MNU structures.
#include "mnu/mnu.h"

#include "mnu_xml/mnu_xml.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace mnu {

namespace {

// Parse window type from string (case-insensitive).
// [orig: CUIScene_CreateWidgetByType @ 0x64f630]  Original does a full-word wide
// CRT_wcsicmp on the TYPE attribute; tokens are STATIC/BUTTON/SCROLL/EDIT/
// MULTILINE_EDIT/RADIO/LIST/SPINLIST/CHECKBOX/TABLE/COMBOBOX/MARQUEE_WND/
// GLB_TABLE/RADIOEDIT/LAN_LIST/GOPHER; any unrecognized type falls back to a
// generic CWnd. The raw authored token is preserved on Window::type_token so an
// unmodelled type survives round-trip verbatim. window_type_name() emits the
// canonical spellings so from-scratch documents stay original-valid.
WindowType parse_type_string(const std::string &s) {
  std::string lower = mnu_xml::to_lower(s);
  if (lower == "window") return WindowType::Window;
  if (lower == "static") return WindowType::Static;
  if (lower == "button") return WindowType::Button;
  if (lower == "edit") return WindowType::Edit;
  if (lower == "multiline_edit" || lower == "multilineedit")
    return WindowType::MultilineEdit;
  if (lower == "list") return WindowType::List;
  if (lower == "checkbox" || lower == "check") return WindowType::CheckBox;
  if (lower == "radio") return WindowType::Radio;
  if (lower == "combo" || lower == "combobox") return WindowType::Combo;
  if (lower == "scroll") return WindowType::Scroll;
  if (lower == "table") return WindowType::Table;
  if (lower == "spinlist" || lower == "spin") return WindowType::SpinList;
  if (lower == "multi") return WindowType::Multi;
  if (lower == "map") return WindowType::Map;
  if (lower == "globe") return WindowType::Globe;
  if (lower == "label") return WindowType::Label;
  if (lower == "goto") return WindowType::Goto;
  if (lower == "marquee_wnd" || lower == "marquee") return WindowType::Marquee;
  if (lower == "glb_table") return WindowType::GlbTable;
  if (lower == "radioedit") return WindowType::RadioEdit;
  if (lower == "lan_list") return WindowType::LanList;
  if (lower == "gopher") return WindowType::Gopher;
  return WindowType::Unknown;
}

// Parse integer with fallback.
int parse_int(const std::string &s, int fallback = 0) {
  if (s.empty()) return fallback;
  try {
    return std::stoi(s);
  } catch (...) {
    return fallback;
  }
}

// Get text content from a child element by tag name.
// MNU format uses <TAG>value</TAG> pattern for many values.
std::string child_text(const mnu_xml::Node *node, const std::string &tag) {
  if (!node) return "";
  const mnu_xml::Node *child = node->find_child(tag);
  return child ? child->get_direct_text() : "";
}

// Get attribute OR child element text (MNU format can use either).
std::string attr_or_child(const mnu_xml::Node *node, const std::string &name) {
  if (!node) return "";
  std::string val = node->attr(name);
  if (val.empty()) {
    val = child_text(node, name);
  }
  return val;
}

// Parse POSITION element.
// MNU uses child elements: <LEFT>0</LEFT><TOP>75</TOP>...
Position parse_position(const mnu_xml::Node *pos_node) {
  Position pos;
  if (!pos_node) return pos;

  // Try child elements first (MNU format), then attributes as fallback.
  std::string left = attr_or_child(pos_node, "left");
  std::string top = attr_or_child(pos_node, "top");
  std::string right = attr_or_child(pos_node, "right");
  std::string bottom = attr_or_child(pos_node, "bottom");

  if (!left.empty()) {
    pos.left = parse_int(left);
    pos.has_left = true;
  }
  if (!top.empty()) {
    pos.top = parse_int(top);
    pos.has_top = true;
  }
  if (!right.empty()) {
    pos.right = parse_int(right);
    pos.has_right = true;
  }
  if (!bottom.empty()) {
    pos.bottom = parse_int(bottom);
    pos.has_bottom = true;
  }

  // Aliases the original POSITION handler also accepts: ULX/ULY = left/top;
  // WIDTH/HEIGHT derive right/bottom from origin + extent. Shipped fixtures use
  // LEFT/TOP/RIGHT/BOTTOM; these keep a menu authored the other way intact.
  if (!pos.has_left) {
    std::string ulx = attr_or_child(pos_node, "ulx");
    if (!ulx.empty()) { pos.left = parse_int(ulx); pos.has_left = true; }
  }
  if (!pos.has_top) {
    std::string uly = attr_or_child(pos_node, "uly");
    if (!uly.empty()) { pos.top = parse_int(uly); pos.has_top = true; }
  }
  if (!pos.has_right) {
    std::string w = attr_or_child(pos_node, "width");
    if (!w.empty()) { pos.right = pos.left + parse_int(w); pos.has_right = true; }
  }
  if (!pos.has_bottom) {
    std::string h = attr_or_child(pos_node, "height");
    if (!h.empty()) { pos.bottom = pos.top + parse_int(h); pos.has_bottom = true; }
  }

  return pos;
}

// Parse APPEARANCE element.
// MNU format: <APPEARANCE type="image" state="default">Main_hdr.tga</APPEARANCE>
// Value comes from text content.
Appearance parse_appearance(const mnu_xml::Node *app_node) {
  Appearance app;
  if (!app_node) return app;

  app.state = app_node->attr("state");
  app.type = app_node->attr("type");

  // Value can be in attribute OR text content.
  app.value = app_node->attr("value");
  if (app.value.empty()) {
    app.value = app_node->get_direct_text();
  }

  std::string map_state = app_node->attr("map_state");
  if (!map_state.empty()) {
    app.map_state = parse_int(map_state, -1);
  }

  std::string height = app_node->attr("height");
  if (!height.empty()) {
    app.height = parse_int(height);
  }

  return app;
}

// Parse SOUND element.
// MNU format: <SOUND state="mousein" trigger="MOUSE_OVER">menu.lwf</SOUND>
// File comes from text content.
Sound parse_sound(const mnu_xml::Node *sound_node) {
  Sound snd;
  if (!sound_node) return snd;

  snd.state = sound_node->attr("state");
  snd.trigger = sound_node->attr("trigger");

  // File can be in attribute OR text content.
  snd.file = sound_node->attr("file");
  if (snd.file.empty()) {
    snd.file = sound_node->get_direct_text();
  }

  return snd;
}

// Parse ACTION element.
// MNU format: <ACTION type="screen" file="sp.mnu">SINGLE_PLAYER</ACTION>
// Target comes from text content.
Action parse_action(const mnu_xml::Node *action_node) {
  Action act;
  if (!action_node) return act;

  act.type = action_node->attr("type");
  act.state = action_node->attr("state");
  act.file = action_node->attr("file");
  act.external_browser = action_node->attr_bool("external_browser");

  // Target can be in attribute OR text content.
  act.target = action_node->attr("target");
  if (act.target.empty()) {
    act.target = action_node->attr("screen");
  }
  if (act.target.empty()) {
    act.target = action_node->attr("window");
  }
  if (act.target.empty()) {
    act.target = action_node->get_direct_text();
  }

  return act;
}

// Parse STRING element.
String parse_string(const mnu_xml::Node *str_node) {
  String str;
  if (!str_node) return str;

  str.type = str_node->attr("type");
  str.justify = str_node->attr("justify");
  str.vjustify = str_node->attr("vjustify");
  str.edge = parse_int(str_node->attr("edge"));
  str.value = str_node->attr("id");

  // If no ID attribute, check for text content or value attribute.
  if (str.value.empty()) {
    str.value = str_node->attr("value");
  }
  if (str.value.empty()) {
    str.value = str_node->get_direct_text();
  }

  return str;
}

// Parse FONT element.
// MNU format uses child elements: <NAME>Gunpl27b.fnt</NAME><DEFAULT_FG>FFFFFF</DEFAULT_FG>...
Font parse_font(const mnu_xml::Node *font_node) {
  Font font;
  if (!font_node) return font;

  // Name can be attribute or child element.
  font.name = attr_or_child(font_node, "name");

  // Colors as separate child elements (MNU format).
  font.default_fg = child_text(font_node, "default_fg");
  font.default_bg = child_text(font_node, "default_bg");
  font.mouseover_fg = child_text(font_node, "mouseover_fg");
  font.mouseover_bg = child_text(font_node, "mouseover_bg");
  font.selected_fg = child_text(font_node, "selected_fg");
  font.selected_bg = child_text(font_node, "selected_bg");
  font.disabled_fg = child_text(font_node, "disabled_fg");
  font.disabled_bg = child_text(font_node, "disabled_bg");

  // Also support nested elements with fg/bg attributes (alternative format).
  for (const auto &child : font_node->children) {
    if (!child->is_element()) continue;

    std::string tag = mnu_xml::to_lower(child->tag);
    std::string fg = child->attr("foreground");
    std::string bg = child->attr("background");

    if (fg.empty()) fg = child->attr("fg");
    if (bg.empty()) bg = child->attr("bg");

    if (tag == "default") {
      if (!fg.empty()) font.default_fg = fg;
      if (!bg.empty()) font.default_bg = bg;
    } else if (tag == "mouseover" || tag == "mouse_over") {
      if (!fg.empty()) font.mouseover_fg = fg;
      if (!bg.empty()) font.mouseover_bg = bg;
    } else if (tag == "selected") {
      if (!fg.empty()) font.selected_fg = fg;
      if (!bg.empty()) font.selected_bg = bg;
    } else if (tag == "disabled") {
      if (!fg.empty()) font.disabled_fg = fg;
      if (!bg.empty()) font.disabled_bg = bg;
    }
  }

  return font;
}

// Parse FRAME element.
// FRAME can have child elements: STENCIL, BRUSH, MONOGRAM
// Example: <FRAME><STENCIL size="32">border.tga</STENCIL><BRUSH>tile.tga</BRUSH></FRAME>
Frame parse_frame(const mnu_xml::Node *frame_node) {
  Frame frame;
  if (!frame_node) return frame;

  // Check for child elements first (the actual format in MNU files).
  for (const auto &child : frame_node->children) {
    std::string tag = mnu_xml::to_lower(child->tag);
    if (tag == "stencil") {
      frame.stencil = child->get_direct_text();
      frame.stencil_size = parse_int(child->attr("size"));
      if (child->has_attr("insetx")) {
        frame.insetx = parse_int(child->attr("insetx"));
        frame.has_insetx = true;
      }
      if (child->has_attr("insety")) {
        frame.insety = parse_int(child->attr("insety"));
        frame.has_insety = true;
      }
    } else if (tag == "brush") {
      frame.brush = child->get_direct_text();
    } else if (tag == "monogram") {
      frame.monogram = child->get_direct_text();
    }
  }

  // Fallback to attributes (for potential alternate format).
  if (frame.stencil.empty()) frame.stencil = frame_node->attr("stencil");
  if (frame.stencil_size == 0) frame.stencil_size = parse_int(frame_node->attr("stencil_size"));
  if (frame.brush.empty()) frame.brush = frame_node->attr("brush");
  if (frame.monogram.empty()) frame.monogram = frame_node->attr("monogram");

  return frame;
}

// Parse ITEM element.
Item parse_item(const mnu_xml::Node *item_node) {
  Item item;
  if (!item_node) return item;

  item.type = item_node->attr("type");
  item.value = item_node->attr("value");
  item.text = item_node->attr("text");

  // If no text attribute, check for ID or content.
  if (item.text.empty()) {
    item.text = item_node->attr("id");
  }
  if (item.text.empty()) {
    item.text = item_node->get_direct_text();
  }

  return item;
}

// Parse ITEMS element.
Items parse_items(const mnu_xml::Node *items_node) {
  Items items;
  if (!items_node) return items;

  items.justify = items_node->attr("justify");
  items.vjustify = items_node->attr("vjustify");

  for (const auto &child : items_node->children) {
    if (!child->is_element()) continue;
    if (mnu_xml::iequals(child->tag, "item")) {
      items.items.push_back(parse_item(child.get()));
    } else if (mnu_xml::iequals(child->tag, "appearance")) {
      // Extract selection color from appearance (state="selected", type="color")
      Appearance app = parse_appearance(child.get());
      if (mnu_xml::iequals(app.state, "selected") && mnu_xml::iequals(app.type, "color")) {
        items.selection_color = app.value;
      }
    }
  }

  return items;
}

// Parse LIST_BOX SCROLLBAR element.
ListBoxScrollbar parse_listbox_scrollbar(const mnu_xml::Node *node) {
  ListBoxScrollbar scrollbar;
  if (!node) return scrollbar;

  scrollbar.present = true;

  for (const auto &child : node->children) {
    if (!child->is_element()) continue;
    std::string tag = mnu_xml::to_lower(child->tag);

    if (tag == "position") {
      scrollbar.position = parse_position(child.get());
    } else if (tag == "appearance") {
      scrollbar.track.push_back(parse_appearance(child.get()));
    } else if (tag == "shuttle") {
      scrollbar.shuttle.push_back(parse_appearance(child.get()));
    } else if (tag == "scrollup") {
      scrollbar.scrollup.push_back(parse_appearance(child.get()));
    } else if (tag == "scrolldown") {
      scrollbar.scrolldown.push_back(parse_appearance(child.get()));
    } else if (tag == "sound") {
      scrollbar.sounds.push_back(parse_sound(child.get()));
    }
  }

  return scrollbar;
}

// Parse LIST_BOX element.
// [orig: CListWnd_ParseXMLDefinition @ 0x645770]
ListBox parse_listbox(const mnu_xml::Node *listbox_node) {
  ListBox listbox;
  if (!listbox_node) return listbox;

  listbox.present = true;

  // SB_EDGE_PAD is an attribute on the LIST_BOX element itself (not a child).
  if (listbox_node->has_attr("sb_edge_pad")) {
    listbox.sb_edge_pad = parse_int(listbox_node->attr("sb_edge_pad"));
    listbox.has_sb_edge_pad = true;
  }

  for (const auto &child : listbox_node->children) {
    if (!child->is_element()) continue;
    std::string tag = mnu_xml::to_lower(child->tag);

    if (tag == "position") {
      listbox.position = parse_position(child.get());
    } else if (tag == "appearance") {
      listbox.appearances.push_back(parse_appearance(child.get()));
    } else if (tag == "items") {
      listbox.items = parse_items(child.get());
    } else if (tag == "min_item_height") {
      // The value is a child text node, so child->text (the element's own text)
      // is empty here; read it via get_direct_text() like every other site. Using
      // child->text silently dropped it (latent until a real combobox LIST_BOX
      // with MIN_ITEM_HEIGHT exercised it; see ADR 0002).
      listbox.min_item_height = parse_int(child->get_direct_text());
    } else if (tag == "scrollbar") {
      listbox.scrollbar = parse_listbox_scrollbar(child.get());
    }
  }

  return listbox;
}

// Parse CURSOR element.
// MNU format: <CURSOR><FILE>newarow1.tga</FILE><FLAGS>STANDARD_TRANSPARENT</FLAGS></CURSOR>
Cursor parse_cursor(const mnu_xml::Node *cursor_node) {
  Cursor cursor;
  if (!cursor_node) return cursor;

  // File and flags can be attributes or child elements.
  cursor.file = attr_or_child(cursor_node, "file");
  cursor.flags = attr_or_child(cursor_node, "flags");

  return cursor;
}

// Parse spin button (SPINUP/SPINDOWN).
SpinButton parse_spinbutton(const mnu_xml::Node *spin_node) {
  SpinButton spin;
  if (!spin_node) return spin;

  spin.present = true;

  // Parse position.
  auto pos_node = spin_node->find_child("position");
  if (pos_node) {
    spin.position = parse_position(pos_node);
  }

  // Parse appearances.
  for (const auto &child : spin_node->children) {
    if (child->is_element() && mnu_xml::iequals(child->tag, "appearance")) {
      spin.appearances.push_back(parse_appearance(child.get()));
    }
  }

  return spin;
}

// Parse table HEADER element.
// [orig: CTableWnd_ParseXMLContentDefinition @ 0x6427d0]. type="id" headers
// resolve their text via CUIStringTable_LookupString @ 0x6434df; the attribute
// is parsed below (the refs extractor's string_id emission depends on it).
TableHeader parse_table_header(const mnu_xml::Node *node) {
  TableHeader header;
  if (!node) return header;

  header.justify = node->attr("justify");
  header.vjustify = node->attr("vjustify");
  header.column = parse_int(node->attr("column"));
  header.sort = node->attr("sort");
  header.width = parse_int(node->attr("width"));
  header.type = node->attr("type");
  header.text = node->get_direct_text();

  return header;
}

// Parse table BODY element.
TableBody parse_table_body(const mnu_xml::Node *node) {
  TableBody body;
  if (!node) return body;

  body.justify = node->attr("justify");
  body.vjustify = node->attr("vjustify");
  body.column = parse_int(node->attr("column"));

  // Check for BITMAP_DRAW flag (bare attribute or attribute with value).
  body.bitmap_draw = node->has_attr("bitmap_draw");
  body.bitmap_flags = node->attr("bitmap_flags");
  body.scale_bitmap = node->has_attr("scale_bitmap");
  body.custom_draw = node->has_attr("custom_draw");

  return body;
}

// Parse table SUBST element.
TableSubst parse_table_subst(const mnu_xml::Node *node) {
  TableSubst subst;
  if (!node) return subst;

  subst.column = parse_int(node->attr("column"));
  subst.value = node->attr("value");
  subst.is_file = node->has_attr("file");
  // File path is in text content.
  subst.file = node->get_direct_text();

  return subst;
}

// Parse table COLUMN element.
// [orig: CTableWnd_ParseXMLContentDefinition @ 0x6427d0]
TableColumn parse_table_column(const mnu_xml::Node *node) {
  TableColumn col;
  if (!node) return col;

  col.count = parse_int(node->attr("count"));
  col.spacing = parse_int(node->attr("spacing"));

  for (const auto &child : node->children) {
    if (!child->is_element()) continue;
    std::string tag = mnu_xml::to_lower(child->tag);

    if (tag == "header") {
      col.headers.push_back(parse_table_header(child.get()));
    } else if (tag == "body") {
      col.bodies.push_back(parse_table_body(child.get()));
    } else if (tag == "subst") {
      col.substitutions.push_back(parse_table_subst(child.get()));
    }
  }

  return col;
}

// Parse table SCROLLBAR element.
TableScrollbar parse_table_scrollbar(const mnu_xml::Node *node) {
  TableScrollbar scrollbar;
  if (!node) return scrollbar;

  scrollbar.present = true;

  for (const auto &child : node->children) {
    if (!child->is_element()) continue;
    std::string tag = mnu_xml::to_lower(child->tag);

    if (tag == "position") {
      scrollbar.position = parse_position(child.get());
    } else if (tag == "appearance") {
      scrollbar.track.push_back(parse_appearance(child.get()));
    } else if (tag == "shuttle") {
      scrollbar.shuttle.push_back(parse_appearance(child.get()));
    } else if (tag == "scrollup") {
      scrollbar.scrollup.push_back(parse_appearance(child.get()));
    } else if (tag == "scrolldown") {
      scrollbar.scrolldown.push_back(parse_appearance(child.get()));
    } else if (tag == "sound") {
      scrollbar.sounds.push_back(parse_sound(child.get()));
    }
  }

  return scrollbar;
}

// Forward declaration.
Window parse_window(const mnu_xml::Node *window_node);

// Parse WINDOW element recursively.
// [orig: CUIElement_ParseXMLDefinition @ 0x648120 (base attrs); edit attrs in
//  parse_edit_widget_xml_properties @ 0x661d10; checkbox attrs @ 0x64ad90]
// The original splits attributes across a base parser + per-widget-class overrides;
// this reimpl flattens them onto every window (harmless superset). DROPPED by reimpl:
// FORM (int -> widget+0x124 @0x6482a6), GLOBAL_VAR (@0x648323), PASSWORD (edit, @0x661d3b).
// See notes/mnu/divergence-backlog.md.
Window parse_window(const mnu_xml::Node *window_node) {
  Window win;
  if (!window_node) return win;

  // Debug: print window being parsed.
  // fprintf(stderr, "[MNU Parse] Window: %s\n", window_node->attr("name").c_str());

  // Parse attributes.
  win.name = window_node->attr("name");

  std::string type_str = window_node->attr("type");
  if (!type_str.empty()) {
    win.type = parse_type_string(type_str);
    win.type_token = type_str;
  }

  win.hidden = window_node->attr_bool("hidden");
  // MNU uses both "DISABLE" and "disabled" as bare attributes.
  win.disabled = window_node->attr_bool("disabled") ||
                 window_node->attr_bool("disable");
  win.checked = window_node->attr_bool("checked");
  win.draw_frame = window_node->attr_bool("draw_frame");
  win.modal = window_node->attr_bool("modal");
  win.readonly = window_node->attr_bool("readonly");
  win.as_button = window_node->attr_bool("as_button");
  win.group = parse_int(window_node->attr("group"));

  // Numeric edit-field constraints (preserved for round-trip; see ADR 0002).
  win.number = window_node->attr_bool("number");
  if (window_node->has_attr("minval")) {
    win.minval = parse_int(window_node->attr("minval"));
    win.has_minval = true;
  }
  if (window_node->has_attr("maxval")) {
    win.maxval = parse_int(window_node->attr("maxval"));
    win.has_maxval = true;
  }
  if (window_node->has_attr("maxchar")) {
    win.maxchar = parse_int(window_node->attr("maxchar"));
    win.has_maxchar = true;
  }

  // Additional attributes the original parses (preserved for round-trip).
  if (window_node->has_attr("form")) {
    win.form = parse_int(window_node->attr("form"));
    win.has_form = true;
  }
  win.global_var = window_node->attr_bool("global_var");
  win.password = window_node->attr_bool("password");

  // Parse child elements.
  for (const auto &child : window_node->children) {
    if (!child->is_element()) continue;

    std::string tag = mnu_xml::to_lower(child->tag);

    if (tag == "position") {
      win.position = parse_position(child.get());
    } else if (tag == "appearance") {
      win.appearances.push_back(parse_appearance(child.get()));
    } else if (tag == "sound") {
      win.sounds.push_back(parse_sound(child.get()));
    } else if (tag == "action") {
      win.actions.push_back(parse_action(child.get()));
    } else if (tag == "string") {
      win.string_data = parse_string(child.get());
    } else if (tag == "font") {
      win.font = parse_font(child.get());
    } else if (tag == "frame") {
      win.frame = parse_frame(child.get());
    } else if (tag == "items") {
      win.items = parse_items(child.get());
    } else if (tag == "list_box" || tag == "listbox") {
      // LIST_BOX contains styling and items for dropdown popups.
      win.list_box = parse_listbox(child.get());
      // Also copy items to win.items for convenience.
      if (!win.list_box.items.items.empty()) {
        win.items = win.list_box.items;
      }
    } else if (tag == "spinup") {
      win.spinup = parse_spinbutton(child.get());
    } else if (tag == "spindown") {
      win.spindown = parse_spinbutton(child.get());
    } else if (tag == "cursor") {
      win.cursor = parse_cursor(child.get());
    } else if (tag == "group") {
      // Radio button group ID (element form).
      win.group = parse_int(child->get_text());
    } else if (tag == "orientation") {
      // Scroll/slider orientation.
      win.orientation = child->get_direct_text();
    } else if (tag == "height") {
      // Window-level scroll-bar thickness (horizontal scroll); sibling of POSITION.
      win.scroll_height = parse_int(child->get_direct_text());
      win.has_scroll_height = true;
    } else if (tag == "width") {
      // Window-level scroll-bar thickness (vertical scroll); sibling of POSITION.
      win.scroll_width = parse_int(child->get_direct_text());
      win.has_scroll_width = true;
    } else if (tag == "shuttle") {
      // Slider grabber appearance (same format as appearance).
      win.shuttle.push_back(parse_appearance(child.get()));
    } else if (tag == "scrollup") {
      // Left/up arrow button appearance.
      win.scrollup.push_back(parse_appearance(child.get()));
    } else if (tag == "scrolldown") {
      // Right/down arrow button appearance.
      win.scrolldown.push_back(parse_appearance(child.get()));
    } else if (tag == "column") {
      // Table column definition.
      win.table_data.column = parse_table_column(child.get());
    } else if (tag == "scrollbar") {
      // Table scrollbar definition.
      win.table_data.scrollbar = parse_table_scrollbar(child.get());
    } else if (tag == "min_item_height") {
      // Table minimum row height.
      win.table_data.min_item_height = parse_int(child->get_direct_text());
    } else if (tag == "datasource") {
      // Data source file (for marquee_wnd credits, etc.)
      win.datasource = child->get_direct_text();
    } else if (tag == "hotkey") {
      // Hotkey binding (e.g., VK_ESCAPE, VK_RETURN)
      win.hotkey = child->get_direct_text();
      win.hotkey_virtual = child->has_attr("virtual");
    } else if (tag == "window") {
      // Nested child window.
      win.children.push_back(parse_window(child.get()));
    }
  }

  // For Table type, extract outline/selection colors and multiselect from ITEMS.
  if (win.type == WindowType::Table) {
    for (const auto &child : window_node->children) {
      if (!child->is_element()) continue;
      if (mnu_xml::iequals(child->tag, "items")) {
        // Check for MULTISELECT attribute (bare attribute).
        win.table_data.multiselect = child->has_attr("multiselect");

        for (const auto &item_child : child->children) {
          if (!item_child->is_element()) continue;
          if (mnu_xml::iequals(item_child->tag, "appearance")) {
            Appearance app = parse_appearance(item_child.get());
            if (mnu_xml::iequals(app.state, "default") &&
                mnu_xml::iequals(app.type, "outline")) {
              win.table_data.outline_color = app.value;
            } else if (mnu_xml::iequals(app.state, "selected") &&
                       mnu_xml::iequals(app.type, "color")) {
              win.table_data.selection_color = app.value;
            }
          }
        }
        break;
      }
    }
  }

  return win;
}

// Parse SCREEN element.
// MNU format: <SCREEN><NAME>STARTUP</NAME><MUSICVAR>1</MUSICVAR>...
// [orig: parse_scene_node_attributes @ 0x639630; SCREEN node callback @ 0x63b800]
// SCREEN is the only document root the original handles (no <sc> script tag exists);
// the reimpl's optional <MNU>/<MENU> wrapper is a harmless superset.
Screen parse_screen(const mnu_xml::Node *screen_node) {
  Screen screen;
  if (!screen_node) return screen;

  // Name, music_var, text_rsrc can be attributes or child elements.
  screen.name = attr_or_child(screen_node, "name");
  screen.music_var = parse_int(attr_or_child(screen_node, "musicvar"));
  if (screen.music_var == 0) {
    screen.music_var = parse_int(attr_or_child(screen_node, "music_var"));
  }

  // Parse child elements.
  for (const auto &child : screen_node->children) {
    if (!child->is_element()) continue;

    std::string tag = mnu_xml::to_lower(child->tag);

    if (tag == "cursor") {
      Cursor c = parse_cursor(child.get());
      screen.cursor_file = c.file;
      screen.cursor_flags = c.flags;
    } else if (tag == "text_rsrc") {
      screen.text_rsrc = child->get_direct_text();
    } else if (tag == "window") {
      // Root window of the screen - also check for text_rsrc and cursor in window.
      screen.root_window = parse_window(child.get());

      // The cursor and text_rsrc might be inside the root window element.
      if (screen.cursor_file.empty()) {
        auto win_cursor = child->find_child("cursor");
        if (win_cursor) {
          Cursor c = parse_cursor(win_cursor);
          screen.cursor_file = c.file;
          screen.cursor_flags = c.flags;
        }
      }
      if (screen.text_rsrc.empty()) {
        screen.text_rsrc = child_text(child.get(), "text_rsrc");
      }
    }
  }

  return screen;
}

}  // namespace

// Public API implementations.

WindowType parse_window_type(const std::string &type_str) {
  return parse_type_string(type_str);
}

const char *window_type_name(WindowType type) {
  // [orig: tokens matched by CUIScene_CreateWidgetByType @ 0x64f630]  These canonical
  // spellings (e.g. Combo->"combobox", Marquee->"marquee_wnd") are exactly the wide
  // literals the original factory full-word-matches, so round-trip stays engine-valid.
  // Output lowercase type names to match original NovaLogic MNU format
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
    case WindowType::Multi: return "multi";
    case WindowType::Map: return "map";
    case WindowType::Globe: return "globe";
    case WindowType::Label: return "label";
    case WindowType::Goto: return "goto";
    case WindowType::Marquee: return "marquee_wnd";
    case WindowType::GlbTable: return "glb_table";
    case WindowType::RadioEdit: return "radioedit";
    case WindowType::LanList: return "lan_list";
    case WindowType::Gopher: return "gopher";
    case WindowType::Unknown: return "unknown";
  }
  return "unknown";
}

const Screen *Document::find_screen(const std::string &name) const {
  for (const auto &screen : screens) {
    if (mnu_xml::iequals(screen.name, name)) {
      return &screen;
    }
  }
  return nullptr;
}

const Screen *Document::first_screen() const {
  return screens.empty() ? nullptr : &screens[0];
}

bool parse(const std::string &content, Document &out, std::string &error,
           const ParseOptions &options) {
  return parse(reinterpret_cast<const uint8_t *>(content.data()),
               content.size(), out, error, options);
}

bool parse(const uint8_t *data, size_t size, Document &out, std::string &error,
           const ParseOptions &options) {
  out.screens.clear();

  // Parse as generic XML first.
  mnu_xml::Document xml_doc;
  mnu_xml::ParseOptions xml_opts;
  xml_opts.normalize_whitespace = true;
  xml_opts.trim_text = true;

  if (!mnu_xml::parse(data, size, xml_doc, error, xml_opts)) {
    return false;
  }

  // Convert XML tree to MNU structures.
  for (const auto &root : xml_doc.roots) {
    if (!root->is_element()) continue;

    std::string tag = mnu_xml::to_lower(root->tag);

    if (tag == "screen") {
      out.screens.push_back(parse_screen(root.get()));
    } else if (tag == "mnu" || tag == "menu") {
      // Container element, process children.
      for (const auto &child : root->children) {
        if (child->is_element() && mnu_xml::iequals(child->tag, "screen")) {
          out.screens.push_back(parse_screen(child.get()));
        }
      }
    }
  }

  return true;
}

bool parse_file(const std::string &path, Document &out, std::string &error,
                const ParseOptions &options) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    error = "Failed to open file: " + path;
    return false;
  }

  std::stringstream buffer;
  buffer << file.rdbuf();
  std::string content = buffer.str();

  return parse(content, out, error, options);
}

std::string fix_malformed_xml(const std::string &content) {
  // The mnu_xml parser already handles most malformed cases (unquoted
  // attributes, bare booleans). This function is provided for compatibility
  // but may not need to do anything.
  return content;
}

std::string strip_hotkey_marker(const std::string &text,
                                std::string *out_hotkey,
                                int *out_hotkey_pos) {
  std::string result;
  result.reserve(text.size());

  size_t i = 0;
  while (i < text.size()) {
    if (i + 4 < text.size() && text[i] == '{' &&
        (text[i + 1] == 'h' || text[i + 1] == 'H') &&
        (text[i + 2] == 'o' || text[i + 2] == 'O') &&
        (text[i + 3] == 't' || text[i + 3] == 'T') && text[i + 4] == '}') {
      // Found {hot} marker.
      if (out_hotkey_pos) {
        *out_hotkey_pos = static_cast<int>(result.size());
      }
      i += 5;  // Skip "{hot}".

      // The next character is the hotkey.
      if (i < text.size() && out_hotkey) {
        *out_hotkey = std::string(1, text[i]);
      }
    }
    if (i < text.size()) {
      result += text[i++];
    }
  }

  return result;
}

bool parse_hex_color(const std::string &hex, uint8_t &r, uint8_t &g, uint8_t &b,
                     uint8_t &a) {
  if (hex.empty()) return false;

  // Check for variable reference.
  if (is_color_variable(hex)) return false;

  std::string s = hex;
  // Remove leading '#'.
  if (!s.empty() && s[0] == '#') {
    s = s.substr(1);
  }

  // Default alpha to fully opaque.
  a = 255;

  if (s.size() == 6) {
    // RRGGBB.
    r = static_cast<uint8_t>(std::strtol(s.substr(0, 2).c_str(), nullptr, 16));
    g = static_cast<uint8_t>(std::strtol(s.substr(2, 2).c_str(), nullptr, 16));
    b = static_cast<uint8_t>(std::strtol(s.substr(4, 2).c_str(), nullptr, 16));
    return true;
  } else if (s.size() == 8) {
    // AARRGGBB.
    a = static_cast<uint8_t>(std::strtol(s.substr(0, 2).c_str(), nullptr, 16));
    r = static_cast<uint8_t>(std::strtol(s.substr(2, 2).c_str(), nullptr, 16));
    g = static_cast<uint8_t>(std::strtol(s.substr(4, 2).c_str(), nullptr, 16));
    b = static_cast<uint8_t>(std::strtol(s.substr(6, 2).c_str(), nullptr, 16));
    return true;
  }

  return false;
}

bool is_color_variable(const std::string &color) {
  return color.size() >= 2 && color.front() == '%' && color.back() == '%';
}

namespace {

// Escape the four entities the engine can DECODE [orig: XML_ParseCharEntity
// @ 0x769cc0 table @ 0x85a628]. Notably NOT &apos;: the engine has no apos
// entry, so emitting it would round-trip a literal ' into a broken "&apos;".
// Attributes are written with double-quote delimiters, so a raw ' is always
// safe; only " needs escaping inside an attribute value.
std::string escape_xml(const std::string &input, bool escape_quotes = false) {
  std::string out;
  out.reserve(input.size());
  for (char c : input) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"':
        if (escape_quotes) {
          out += "&quot;";
        } else {
          out.push_back(c);
        }
        break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

void append_indent(std::string &out, int depth, bool pretty, int indent_size) {
  if (!pretty) return;
  out.append(static_cast<size_t>(depth * indent_size), ' ');
}

void append_line(std::string &out, int depth, const std::string &text,
                 bool pretty, int indent_size) {
  append_indent(out, depth, pretty, indent_size);
  out += text;
  if (pretty) out.push_back('\n');
}

std::string window_type_attr(WindowType type) {
  std::string name = window_type_name(type);
  for (auto &c : name) c = static_cast<char>(std::tolower(c));
  return name;
}

std::string attr_pair(const std::string &name, const std::string &value) {
  if (value.empty()) return "";
  return " " + name + "=\"" + escape_xml(value, true) + "\"";
}

void write_position(const Position &pos, std::string &out, int depth,
                    bool pretty, int indent_size) {
  if (!pos.has_left && !pos.has_top && !pos.has_right && !pos.has_bottom)
    return;
  append_line(out, depth, "<POSITION>", pretty, indent_size);
  if (pos.has_left) {
    append_line(out, depth + 1, "<LEFT>" + std::to_string(pos.left) + "</LEFT>",
                pretty, indent_size);
  }
  if (pos.has_top) {
    append_line(out, depth + 1, "<TOP>" + std::to_string(pos.top) + "</TOP>",
                pretty, indent_size);
  }
  if (pos.has_right) {
    append_line(out, depth + 1,
                "<RIGHT>" + std::to_string(pos.right) + "</RIGHT>", pretty,
                indent_size);
  }
  if (pos.has_bottom) {
    append_line(out, depth + 1,
                "<BOTTOM>" + std::to_string(pos.bottom) + "</BOTTOM>", pretty,
                indent_size);
  }
  append_line(out, depth, "</POSITION>", pretty, indent_size);
}

void write_appearance(const Appearance &app, std::string &out, int depth,
                      bool pretty, int indent_size) {
  if (app.state.empty() && app.type.empty() && app.value.empty() &&
      app.map_state < 0 && app.height == 0) {
    return;
  }
  std::string attrs;
  // Original format has type before state
  attrs += attr_pair("type", app.type);
  attrs += attr_pair("state", app.state);
  if (app.map_state >= 0) {
    attrs += " map_state=\"" + std::to_string(app.map_state) + "\"";
  }
  if (app.height > 0) {
    attrs += " height=\"" + std::to_string(app.height) + "\"";
  }

  append_line(out, depth,
              "<APPEARANCE" + attrs + ">" + escape_xml(app.value) +
                  "</APPEARANCE>",
              pretty, indent_size);
}

void write_sound(const Sound &snd, std::string &out, int depth, bool pretty,
                 int indent_size) {
  if (snd.state.empty() && snd.trigger.empty() && snd.file.empty()) return;
  std::string attrs;
  attrs += attr_pair("state", snd.state);
  attrs += attr_pair("trigger", snd.trigger);
  append_line(out, depth,
              "<SOUND" + attrs + ">" + escape_xml(snd.file) + "</SOUND>",
              pretty, indent_size);
}

void write_action(const Action &act, std::string &out, int depth, bool pretty,
                  int indent_size) {
  if (act.type.empty() && act.state.empty() && act.file.empty() &&
      act.target.empty()) {
    return;
  }
  std::string attrs;
  attrs += attr_pair("type", act.type);
  // Uppercase state values like HIDE/SHOW to match original format
  std::string state_upper = act.state;
  for (auto &c : state_upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  attrs += attr_pair("state", state_upper);
  attrs += attr_pair("file", act.file);
  if (act.external_browser) attrs += " EXTERNAL_BROWSER";
  // Don't write target as attribute - it goes in element content

  append_line(out, depth,
              "<ACTION" + attrs + ">" + escape_xml(act.target) + "</ACTION>",
              pretty, indent_size);
}

void write_string(const String &str, std::string &out, int depth, bool pretty,
                  int indent_size) {
  if (str.value.empty() && str.type.empty() && str.justify.empty() &&
      str.vjustify.empty() && str.edge == 0) {
    return;
  }
  std::string attrs;
  attrs += attr_pair("type", str.type);
  attrs += attr_pair("justify", str.justify);
  attrs += attr_pair("vjustify", str.vjustify);
  if (str.edge != 0) {
    attrs += " edge=\"" + std::to_string(str.edge) + "\"";
  }

  append_line(out, depth,
              "<STRING" + attrs + ">" + escape_xml(str.value) + "</STRING>",
              pretty, indent_size);
}

void write_font(const Font &font, std::string &out, int depth, bool pretty,
                int indent_size) {
  if (font.empty() && font.default_bg.empty() && font.mouseover_fg.empty() &&
      font.mouseover_bg.empty() && font.selected_fg.empty() &&
      font.selected_bg.empty() && font.disabled_fg.empty() &&
      font.disabled_bg.empty()) {
    return;
  }

  append_line(out, depth, "<FONT>", pretty, indent_size);
  if (!font.name.empty()) {
    append_line(out, depth + 1,
                "<NAME>" + escape_xml(font.name) + "</NAME>", pretty,
                indent_size);
  }
  if (!font.default_fg.empty()) {
    append_line(out, depth + 1,
                "<DEFAULT_FG>" + escape_xml(font.default_fg) +
                    "</DEFAULT_FG>",
                pretty, indent_size);
  }
  if (!font.default_bg.empty()) {
    append_line(out, depth + 1,
                "<DEFAULT_BG>" + escape_xml(font.default_bg) +
                    "</DEFAULT_BG>",
                pretty, indent_size);
  }
  if (!font.mouseover_fg.empty()) {
    append_line(out, depth + 1,
                "<MOUSEOVER_FG>" + escape_xml(font.mouseover_fg) +
                    "</MOUSEOVER_FG>",
                pretty, indent_size);
  }
  if (!font.mouseover_bg.empty()) {
    append_line(out, depth + 1,
                "<MOUSEOVER_BG>" + escape_xml(font.mouseover_bg) +
                    "</MOUSEOVER_BG>",
                pretty, indent_size);
  }
  if (!font.selected_fg.empty()) {
    append_line(out, depth + 1,
                "<SELECTED_FG>" + escape_xml(font.selected_fg) +
                    "</SELECTED_FG>",
                pretty, indent_size);
  }
  if (!font.selected_bg.empty()) {
    append_line(out, depth + 1,
                "<SELECTED_BG>" + escape_xml(font.selected_bg) +
                    "</SELECTED_BG>",
                pretty, indent_size);
  }
  if (!font.disabled_fg.empty()) {
    append_line(out, depth + 1,
                "<DISABLED_FG>" + escape_xml(font.disabled_fg) +
                    "</DISABLED_FG>",
                pretty, indent_size);
  }
  if (!font.disabled_bg.empty()) {
    append_line(out, depth + 1,
                "<DISABLED_BG>" + escape_xml(font.disabled_bg) +
                    "</DISABLED_BG>",
                pretty, indent_size);
  }
  append_line(out, depth, "</FONT>", pretty, indent_size);
}

void write_frame(const Frame &frame, std::string &out, int depth, bool pretty,
                 int indent_size) {
  if (frame.stencil.empty() && frame.stencil_size == 0 && frame.brush.empty() &&
      frame.monogram.empty()) {
    return;
  }
  append_line(out, depth, "<FRAME>", pretty, indent_size);
  if (!frame.stencil.empty()) {
    std::string size_attr = frame.stencil_size > 0
                                ? " size=\"" + std::to_string(frame.stencil_size) + "\""
                                : "";
    if (frame.has_insetx) size_attr += " insetx=\"" + std::to_string(frame.insetx) + "\"";
    if (frame.has_insety) size_attr += " insety=\"" + std::to_string(frame.insety) + "\"";
    append_line(out, depth + 1,
                "<STENCIL" + size_attr + ">" + escape_xml(frame.stencil) + "</STENCIL>",
                pretty, indent_size);
  }
  if (!frame.brush.empty()) {
    append_line(out, depth + 1,
                "<BRUSH>" + escape_xml(frame.brush) + "</BRUSH>",
                pretty, indent_size);
  }
  if (!frame.monogram.empty()) {
    append_line(out, depth + 1,
                "<MONOGRAM>" + escape_xml(frame.monogram) + "</MONOGRAM>",
                pretty, indent_size);
  }
  append_line(out, depth, "</FRAME>", pretty, indent_size);
}

void write_items(const Items &items, std::string &out, int depth, bool pretty,
                 int indent_size) {
  if (items.items.empty() && items.justify.empty() && items.vjustify.empty())
    return;
  std::string attrs;
  attrs += attr_pair("justify", items.justify);
  attrs += attr_pair("vjustify", items.vjustify);

  append_line(out, depth, "<ITEMS" + attrs + ">", pretty, indent_size);
  for (const auto &item : items.items) {
    std::string item_attrs;
    item_attrs += attr_pair("type", item.type);
    item_attrs += attr_pair("value", item.value);
    if (!item.text.empty()) {
      append_line(out, depth + 1,
                  "<ITEM" + item_attrs + ">" + escape_xml(item.text) +
                      "</ITEM>",
                  pretty, indent_size);
    } else {
      append_line(out, depth + 1, "<ITEM" + item_attrs + "/>", pretty,
                  indent_size);
    }
  }
  append_line(out, depth, "</ITEMS>", pretty, indent_size);
}

void write_spin(const SpinButton &spin, const char *tag, std::string &out,
                int depth, bool pretty, int indent_size) {
  if (!spin.present) return;
  append_line(out, depth, std::string("<") + tag + ">", pretty, indent_size);
  write_position(spin.position, out, depth + 1, pretty, indent_size);
  for (const auto &app : spin.appearances) {
    write_appearance(app, out, depth + 1, pretty, indent_size);
  }
  append_line(out, depth, std::string("</") + tag + ">", pretty, indent_size);
}

void write_listbox_scrollbar(const ListBoxScrollbar &sb, std::string &out,
                             int depth, bool pretty, int indent_size) {
  if (!sb.present) return;
  append_line(out, depth, "<SCROLLBAR>", pretty, indent_size);
  // Track appearances (background)
  for (const auto &app : sb.track) {
    write_appearance(app, out, depth + 1, pretty, indent_size);
  }
  // Shuttle appearances
  for (const auto &app : sb.shuttle) {
    Appearance shuttle_app = app;
    append_line(out, depth + 1,
                "<SHUTTLE" + attr_pair("type", shuttle_app.type) +
                    attr_pair("state", shuttle_app.state) + ">" +
                    escape_xml(shuttle_app.value) + "</SHUTTLE>",
                pretty, indent_size);
  }
  // Scroll up appearances
  for (const auto &app : sb.scrollup) {
    std::string attrs;
    attrs += attr_pair("type", app.type);
    attrs += attr_pair("state", app.state);
    if (app.map_state >= 0) attrs += attr_pair("map_state", std::to_string(app.map_state));
    if (app.height > 0) attrs += attr_pair("height", std::to_string(app.height));
    append_line(out, depth + 1,
                "<SCROLLUP" + attrs + ">" + escape_xml(app.value) + "</SCROLLUP>",
                pretty, indent_size);
  }
  // Scroll down appearances
  for (const auto &app : sb.scrolldown) {
    std::string attrs;
    attrs += attr_pair("type", app.type);
    attrs += attr_pair("state", app.state);
    if (app.map_state >= 0) attrs += attr_pair("map_state", std::to_string(app.map_state));
    if (app.height > 0) attrs += attr_pair("height", std::to_string(app.height));
    append_line(out, depth + 1,
                "<SCROLLDOWN" + attrs + ">" + escape_xml(app.value) + "</SCROLLDOWN>",
                pretty, indent_size);
  }
  // Sounds (CLICK_VALUE, etc.)
  for (const auto &s : sb.sounds) {
    write_sound(s, out, depth + 1, pretty, indent_size);
  }
  write_position(sb.position, out, depth + 1, pretty, indent_size);
  append_line(out, depth, "</SCROLLBAR>", pretty, indent_size);
}

void write_listbox(const ListBox &lb, std::string &out, int depth,
                   bool pretty, int indent_size) {
  if (!lb.present) return;
  std::string lb_attrs;
  if (lb.has_sb_edge_pad) {
    lb_attrs += " sb_edge_pad=\"" + std::to_string(lb.sb_edge_pad) + "\"";
  }
  append_line(out, depth, "<LIST_BOX" + lb_attrs + ">", pretty, indent_size);
  // Appearances (background/outline colors)
  for (const auto &app : lb.appearances) {
    write_appearance(app, out, depth + 1, pretty, indent_size);
  }
  // Position
  write_position(lb.position, out, depth + 1, pretty, indent_size);
  // Items (with selection color appearance)
  if (!lb.items.items.empty() || !lb.items.justify.empty() ||
      !lb.items.selection_color.empty()) {
    std::string attrs;
    attrs += attr_pair("justify", lb.items.justify);
    attrs += attr_pair("vjustify", lb.items.vjustify);
    append_line(out, depth + 1, "<ITEMS" + attrs + ">", pretty, indent_size);
    // Selection color as appearance
    if (!lb.items.selection_color.empty()) {
      append_line(out, depth + 2,
                  "<APPEARANCE type=\"color\" state=\"selected\">" +
                      escape_xml(lb.items.selection_color) + "</APPEARANCE>",
                  pretty, indent_size);
    }
    for (const auto &item : lb.items.items) {
      std::string item_attrs;
      item_attrs += attr_pair("type", item.type);
      item_attrs += attr_pair("value", item.value);
      append_line(out, depth + 2,
                  "<ITEM" + item_attrs + ">" + escape_xml(item.text) + "</ITEM>",
                  pretty, indent_size);
    }
    append_line(out, depth + 1, "</ITEMS>", pretty, indent_size);
  }
  // MIN_ITEM_HEIGHT
  if (lb.min_item_height > 0) {
    append_line(out, depth + 1,
                "<MIN_ITEM_HEIGHT>" + std::to_string(lb.min_item_height) +
                    "</MIN_ITEM_HEIGHT>",
                pretty, indent_size);
  }
  // Scrollbar
  write_listbox_scrollbar(lb.scrollbar, out, depth + 1, pretty, indent_size);
  append_line(out, depth, "</LIST_BOX>", pretty, indent_size);
}

void write_cursor(const Cursor &cursor, std::string &out, int depth,
                  bool pretty, int indent_size) {
  if (cursor.file.empty() && cursor.flags.empty()) return;
  append_line(out, depth, "<CURSOR>", pretty, indent_size);
  if (!cursor.file.empty()) {
    append_line(out, depth + 1,
                "<FILE>" + escape_xml(cursor.file) + "</FILE>", pretty,
                indent_size);
  }
  if (!cursor.flags.empty()) {
    append_line(out, depth + 1,
                "<FLAGS>" + escape_xml(cursor.flags) + "</FLAGS>", pretty,
                indent_size);
  }
  append_line(out, depth, "</CURSOR>", pretty, indent_size);
}

void write_window(const Window &win, std::string &out, int depth, bool pretty,
                  int indent_size) {
  std::string attrs;
  // Prefer the authored token: an unmodelled type (generic CWnd in the original
  // [orig: @ 0x64f630]) must not degrade to "unknown" on round-trip.
  attrs += attr_pair("type", win.type_token.empty() ? window_type_attr(win.type)
                                                    : win.type_token);
  if (!win.name.empty()) {
    attrs += attr_pair("name", win.name);
  }
  // Bare attributes in original format (uppercase)
  if (win.draw_frame) attrs += " DRAW_FRAME";
  if (win.hidden) attrs += " HIDDEN";
  if (win.modal) attrs += " MODAL";
  if (win.readonly) attrs += " READONLY";
  if (win.disabled) attrs += " disabled=\"true\"";
  // CHECKED is a bare attribute in original format
  if (win.checked) attrs += " CHECKED";
  if (win.as_button) attrs += " AS_BUTTON";
  // Numeric edit-field constraints (round-trip preservation; see ADR 0002).
  if (win.number) attrs += " NUMBER";
  if (win.has_minval) attrs += " MINVAL=\"" + std::to_string(win.minval) + "\"";
  if (win.has_maxval) attrs += " MAXVAL=\"" + std::to_string(win.maxval) + "\"";
  if (win.has_maxchar) attrs += " MAXCHAR=\"" + std::to_string(win.maxchar) + "\"";
  if (win.global_var) attrs += " GLOBAL_VAR";
  if (win.password) attrs += " PASSWORD";
  if (win.has_form) attrs += " FORM=\"" + std::to_string(win.form) + "\"";

  append_line(out, depth, "<WINDOW" + attrs + ">", pretty, indent_size);

  // GROUP as child element (not attribute, to match original format)
  if (win.group > 0) {
    append_line(out, depth + 1,
                "<GROUP>" + std::to_string(win.group) + "</GROUP>",
                pretty, indent_size);
  }

  // HOTKEY element (e.g., <HOTKEY VIRTUAL>VK_ESCAPE</HOTKEY>)
  if (!win.hotkey.empty()) {
    std::string hotkey_line = "<HOTKEY";
    if (win.hotkey_virtual) hotkey_line += " VIRTUAL";
    hotkey_line += ">" + escape_xml(win.hotkey) + "</HOTKEY>";
    append_line(out, depth + 1, hotkey_line, pretty, indent_size);
  }

  // Actions come before appearances for radio buttons
  for (const auto &act : win.actions) {
    write_action(act, out, depth + 1, pretty, indent_size);
  }

  // FRAME comes before APPEARANCE (like original files)
  write_frame(win.frame, out, depth + 1, pretty, indent_size);
  write_position(win.position, out, depth + 1, pretty, indent_size);
  // Scroll-bar thickness (window-level <HEIGHT>/<WIDTH>), sibling of POSITION.
  if (win.has_scroll_height) {
    append_line(out, depth + 1,
                "<HEIGHT>" + std::to_string(win.scroll_height) + "</HEIGHT>",
                pretty, indent_size);
  }
  if (win.has_scroll_width) {
    append_line(out, depth + 1,
                "<WIDTH>" + std::to_string(win.scroll_width) + "</WIDTH>",
                pretty, indent_size);
  }
  // ORIENTATION for scroll windows (comes after position)
  if (!win.orientation.empty()) {
    append_line(out, depth + 1,
                "<ORIENTATION>" + escape_xml(win.orientation) + "</ORIENTATION>",
                pretty, indent_size);
  }
  for (const auto &app : win.appearances) {
    write_appearance(app, out, depth + 1, pretty, indent_size);
  }
  // Shuttle (grabber) appearances for scroll windows
  for (const auto &app : win.shuttle) {
    std::string attrs = attr_pair("type", app.type) + attr_pair("state", app.state);
    if (app.map_state >= 0) attrs += attr_pair("map_state", std::to_string(app.map_state));
    if (app.height > 0) attrs += attr_pair("height", std::to_string(app.height));
    append_line(out, depth + 1,
                "<SHUTTLE" + attrs + ">" + escape_xml(app.value) + "</SHUTTLE>",
                pretty, indent_size);
  }
  // Scrollup (left/up arrow) for scroll windows
  for (const auto &app : win.scrollup) {
    std::string attrs = attr_pair("type", app.type) + attr_pair("state", app.state);
    if (app.map_state >= 0) attrs += attr_pair("map_state", std::to_string(app.map_state));
    if (app.height > 0) attrs += attr_pair("height", std::to_string(app.height));
    append_line(out, depth + 1,
                "<SCROLLUP" + attrs + ">" + escape_xml(app.value) + "</SCROLLUP>",
                pretty, indent_size);
  }
  // Scrolldown (right/down arrow) for scroll windows
  for (const auto &app : win.scrolldown) {
    std::string attrs = attr_pair("type", app.type) + attr_pair("state", app.state);
    if (app.map_state >= 0) attrs += attr_pair("map_state", std::to_string(app.map_state));
    if (app.height > 0) attrs += attr_pair("height", std::to_string(app.height));
    append_line(out, depth + 1,
                "<SCROLLDOWN" + attrs + ">" + escape_xml(app.value) + "</SCROLLDOWN>",
                pretty, indent_size);
  }
  // TEXT_RSRC goes inside root window (after position)
  if (!win.text_rsrc.empty()) {
    append_line(out, depth + 1,
                "<TEXT_RSRC>" + escape_xml(win.text_rsrc) + "</TEXT_RSRC>",
                pretty, indent_size);
  }
  // DATASOURCE for marquee_wnd and similar
  if (!win.datasource.empty()) {
    append_line(out, depth + 1,
                "<DATASOURCE>" + escape_xml(win.datasource) + "</DATASOURCE>",
                pretty, indent_size);
  }
  write_cursor(win.cursor, out, depth + 1, pretty, indent_size);
  write_font(win.font, out, depth + 1, pretty, indent_size);
  // STRING comes before SOUND in original format
  write_string(win.string_data, out, depth + 1, pretty, indent_size);
  for (const auto &snd : win.sounds) {
    write_sound(snd, out, depth + 1, pretty, indent_size);
  }
  // List and Table widgets serialize their ITEMS in the type-specific block
  // below. Skip the generic writer for them, otherwise they emit two <ITEMS>
  // elements (justify/rows here, selection/colors there); on re-parse the second
  // overwrites win.items (parse_window sets win.items = parse_items(...) per
  // ITEMS child) and the table-color extraction reads only the first <ITEMS> it
  // finds, so the split drops justify and the selection colors on save/reload.
  if (win.type != WindowType::List && win.type != WindowType::Table) {
    write_items(win.items, out, depth + 1, pretty, indent_size);
  }
  write_listbox(win.list_box, out, depth + 1, pretty, indent_size);
  write_spin(win.spinup, "SPINUP", out, depth + 1, pretty, indent_size);
  write_spin(win.spindown, "SPINDOWN", out, depth + 1, pretty, indent_size);

  // List-specific elements (ITEMS with justify, selection color, MIN_ITEM_HEIGHT, SCROLLBAR)
  if (win.type == WindowType::List) {
    const auto &td = win.table_data;

    // ITEMS element with justify/vjustify attributes
    std::string items_attrs;
    if (!win.items.justify.empty()) items_attrs += attr_pair("justify", win.items.justify);
    if (!win.items.vjustify.empty()) items_attrs += attr_pair("vjustify", win.items.vjustify);
    if (td.multiselect) items_attrs += " MULTISELECT";

    // Only output ITEMS if we have attributes, a selection color, or rows.
    bool has_selection = !td.selection_color.empty() || !win.items.selection_color.empty();
    bool has_rows = !win.items.items.empty();
    if (!items_attrs.empty() || has_selection || has_rows) {
      append_line(out, depth + 1, "<ITEMS" + items_attrs + ">", pretty, indent_size);
      // Selection color from table_data or items
      std::string sel_color = !td.selection_color.empty() ? td.selection_color : win.items.selection_color;
      if (!sel_color.empty()) {
        append_line(out, depth + 2,
                    "<APPEARANCE type=\"color\" state=\"selected\">" +
                        escape_xml(sel_color) + "</APPEARANCE>",
                    pretty, indent_size);
      }
      // Item rows (the row-bearing block for a List; the generic write_items above
      // is skipped for List types so this is the single source of its ITEMS).
      for (const auto &item : win.items.items) {
        std::string item_attrs;
        item_attrs += attr_pair("type", item.type);
        item_attrs += attr_pair("value", item.value);
        if (!item.text.empty()) {
          append_line(out, depth + 2,
                      "<ITEM" + item_attrs + ">" + escape_xml(item.text) +
                          "</ITEM>",
                      pretty, indent_size);
        } else {
          append_line(out, depth + 2, "<ITEM" + item_attrs + "/>", pretty,
                      indent_size);
        }
      }
      append_line(out, depth + 1, "</ITEMS>", pretty, indent_size);
    }

    // MIN_ITEM_HEIGHT
    if (td.min_item_height > 0) {
      append_line(out, depth + 1,
                  "<MIN_ITEM_HEIGHT>" + std::to_string(td.min_item_height) + "</MIN_ITEM_HEIGHT>",
                  pretty, indent_size);
    }

    // SCROLLBAR (same as Table)
    if (td.scrollbar.present) {
      const auto &sb = td.scrollbar;
      append_line(out, depth + 1, "<SCROLLBAR>", pretty, indent_size);

      // Track appearances
      for (const auto &app : sb.track) {
        write_appearance(app, out, depth + 2, pretty, indent_size);
      }

      // Shuttle appearances
      for (const auto &app : sb.shuttle) {
        std::string s_attrs = attr_pair("type", app.type) + attr_pair("state", app.state);
        if (app.map_state >= 0) s_attrs += attr_pair("map_state", std::to_string(app.map_state));
        if (app.height > 0) s_attrs += attr_pair("height", std::to_string(app.height));
        append_line(out, depth + 2,
                    "<SHUTTLE" + s_attrs + ">" + escape_xml(app.value) + "</SHUTTLE>",
                    pretty, indent_size);
      }

      // SCROLLUP appearances
      for (const auto &app : sb.scrollup) {
        std::string s_attrs = attr_pair("type", app.type) + attr_pair("state", app.state);
        if (app.map_state >= 0) s_attrs += attr_pair("map_state", std::to_string(app.map_state));
        if (app.height > 0) s_attrs += attr_pair("height", std::to_string(app.height));
        append_line(out, depth + 2,
                    "<SCROLLUP" + s_attrs + ">" + escape_xml(app.value) + "</SCROLLUP>",
                    pretty, indent_size);
      }

      // SCROLLDOWN appearances
      for (const auto &app : sb.scrolldown) {
        std::string s_attrs = attr_pair("type", app.type) + attr_pair("state", app.state);
        if (app.map_state >= 0) s_attrs += attr_pair("map_state", std::to_string(app.map_state));
        if (app.height > 0) s_attrs += attr_pair("height", std::to_string(app.height));
        append_line(out, depth + 2,
                    "<SCROLLDOWN" + s_attrs + ">" + escape_xml(app.value) + "</SCROLLDOWN>",
                    pretty, indent_size);
      }

      // Sounds
      for (const auto &sound : sb.sounds) {
        std::string sound_attrs;
        sound_attrs += attr_pair("state", sound.state);
        sound_attrs += attr_pair("trigger", sound.trigger);
        append_line(out, depth + 2,
                    "<SOUND" + sound_attrs + ">" + escape_xml(sound.file) + "</SOUND>",
                    pretty, indent_size);
      }

      // Position
      write_position(sb.position, out, depth + 2, pretty, indent_size);

      append_line(out, depth + 1, "</SCROLLBAR>", pretty, indent_size);
    }
  }

  // Table-specific elements (COLUMN, ITEMS, MIN_ITEM_HEIGHT, SCROLLBAR)
  if (win.type == WindowType::Table) {
    const auto &td = win.table_data;

    // COLUMN element. Emit whenever the column carries any structure so bodies and
    // value->image SUBST cells are never dropped on save (not only when a count or
    // header is present).
    if (td.column.count > 0 || !td.column.headers.empty() ||
        !td.column.bodies.empty() || !td.column.substitutions.empty()) {
      std::string col_attrs;
      col_attrs += attr_pair("count", std::to_string(td.column.count));
      col_attrs += attr_pair("spacing", std::to_string(td.column.spacing));
      append_line(out, depth + 1, "<COLUMN" + col_attrs + ">", pretty, indent_size);

      // HEADER elements
      for (const auto &h : td.column.headers) {
        std::string h_attrs;
        if (!h.justify.empty()) h_attrs += attr_pair("justify", h.justify);
        if (!h.vjustify.empty()) h_attrs += attr_pair("vjustify", h.vjustify);
        h_attrs += attr_pair("column", std::to_string(h.column));
        if (!h.sort.empty()) h_attrs += attr_pair("sort", h.sort);
        if (h.width > 0) h_attrs += attr_pair("width", std::to_string(h.width));
        if (!h.type.empty()) h_attrs += attr_pair("type", h.type);
        append_line(out, depth + 2, "<HEADER" + h_attrs + ">" + escape_xml(h.text) + "</HEADER>",
                    pretty, indent_size);
      }

      // BODY elements
      for (const auto &b : td.column.bodies) {
        std::string b_attrs;
        if (!b.justify.empty()) b_attrs += attr_pair("justify", b.justify);
        if (!b.vjustify.empty()) b_attrs += attr_pair("vjustify", b.vjustify);
        b_attrs += attr_pair("column", std::to_string(b.column));
        if (b.bitmap_draw) b_attrs += " BITMAP_DRAW";
        if (b.scale_bitmap) b_attrs += " SCALE_BITMAP";
        if (b.custom_draw) b_attrs += " CUSTOM_DRAW";
        if (!b.bitmap_flags.empty()) b_attrs += attr_pair("BITMAP_FLAGS", b.bitmap_flags);
        append_line(out, depth + 2, "<BODY" + b_attrs + " ></BODY>", pretty, indent_size);
      }

      // SUBST elements (value -> image substitution). Parsed into
      // td.column.substitutions and applied at runtime, so they must round-trip
      // on save. Mirror the original fixture form: <SUBST column="N" value="x"
      // FILE>img.tga</SUBST>, with the bare FILE flag emitted uppercase like BODY.
      for (const auto &s : td.column.substitutions) {
        std::string s_attrs = attr_pair("column", std::to_string(s.column));
        if (!s.value.empty()) s_attrs += attr_pair("value", s.value);
        if (s.is_file) s_attrs += " FILE";
        append_line(out, depth + 2,
                    "<SUBST" + s_attrs + ">" + escape_xml(s.file) + "</SUBST>",
                    pretty, indent_size);
      }

      append_line(out, depth + 1, "</COLUMN>", pretty, indent_size);
    }

    // ITEMS element (justify/vjustify + outline/selection colors + MULTISELECT).
    // One combined block: the generic write_items above is skipped for Table, so
    // this is the single source of the table's ITEMS, mirroring the List path. A
    // split (justify in an empty <ITEMS>, colors in a second) does not round-trip
    // -- parse_window keeps only the last ITEMS for win.items and reads colors
    // from the first, so each block would drop the other's data on save/reload.
    {
      std::string items_attrs;
      if (!win.items.justify.empty())
        items_attrs += attr_pair("justify", win.items.justify);
      if (!win.items.vjustify.empty())
        items_attrs += attr_pair("vjustify", win.items.vjustify);
      if (td.multiselect) items_attrs += " MULTISELECT";
      if (!items_attrs.empty() || !td.outline_color.empty() ||
          !td.selection_color.empty()) {
        append_line(out, depth + 1, "<ITEMS" + items_attrs + ">", pretty,
                    indent_size);
        if (!td.outline_color.empty()) {
          append_line(out, depth + 2,
                      "<APPEARANCE type=\"outline\" state=\"default\">" +
                          escape_xml(td.outline_color) + "</APPEARANCE>",
                      pretty, indent_size);
        }
        if (!td.selection_color.empty()) {
          append_line(out, depth + 2,
                      "<APPEARANCE type=\"color\" state=\"selected\">" +
                          escape_xml(td.selection_color) + "</APPEARANCE>",
                      pretty, indent_size);
        }
        append_line(out, depth + 1, "</ITEMS>", pretty, indent_size);
      }
    }

    // MIN_ITEM_HEIGHT
    if (td.min_item_height > 0) {
      append_line(out, depth + 1,
                  "<MIN_ITEM_HEIGHT>" + std::to_string(td.min_item_height) + "</MIN_ITEM_HEIGHT>",
                  pretty, indent_size);
    }

    // SCROLLBAR
    if (td.scrollbar.present) {
      const auto &sb = td.scrollbar;
      append_line(out, depth + 1, "<SCROLLBAR>", pretty, indent_size);

      // Track appearances
      for (const auto &app : sb.track) {
        write_appearance(app, out, depth + 2, pretty, indent_size);
      }

      // Shuttle appearances
      for (const auto &app : sb.shuttle) {
        std::string s_attrs = attr_pair("type", app.type) + attr_pair("state", app.state);
        if (app.map_state >= 0) s_attrs += attr_pair("map_state", std::to_string(app.map_state));
        if (app.height > 0) s_attrs += attr_pair("height", std::to_string(app.height));
        append_line(out, depth + 2,
                    "<SHUTTLE" + s_attrs + ">" + escape_xml(app.value) + "</SHUTTLE>",
                    pretty, indent_size);
      }

      // Scrollup appearances
      for (const auto &app : sb.scrollup) {
        std::string s_attrs = attr_pair("type", app.type) + attr_pair("state", app.state);
        if (app.map_state >= 0) s_attrs += attr_pair("map_state", std::to_string(app.map_state));
        if (app.height > 0) s_attrs += attr_pair("height", std::to_string(app.height));
        append_line(out, depth + 2,
                    "<SCROLLUP" + s_attrs + ">" + escape_xml(app.value) + "</SCROLLUP>",
                    pretty, indent_size);
      }

      // Scrolldown appearances
      for (const auto &app : sb.scrolldown) {
        std::string s_attrs = attr_pair("type", app.type) + attr_pair("state", app.state);
        if (app.map_state >= 0) s_attrs += attr_pair("map_state", std::to_string(app.map_state));
        if (app.height > 0) s_attrs += attr_pair("height", std::to_string(app.height));
        append_line(out, depth + 2,
                    "<SCROLLDOWN" + s_attrs + ">" + escape_xml(app.value) + "</SCROLLDOWN>",
                    pretty, indent_size);
      }

      // Sounds
      for (const auto &snd : sb.sounds) {
        write_sound(snd, out, depth + 2, pretty, indent_size);
      }

      // Position
      write_position(sb.position, out, depth + 2, pretty, indent_size);

      append_line(out, depth + 1, "</SCROLLBAR>", pretty, indent_size);
    }
  }

  // MIN_ITEM_HEIGHT for list-like widgets other than List/Table (e.g. combobox,
  // spinlist, multi): they parse a direct <MIN_ITEM_HEIGHT> child into table_data
  // but have no dedicated emit above, so it is lost on save without this (ADR 0002).
  if (win.type != WindowType::List && win.type != WindowType::Table &&
      win.table_data.min_item_height > 0) {
    append_line(out, depth + 1,
                "<MIN_ITEM_HEIGHT>" +
                    std::to_string(win.table_data.min_item_height) +
                    "</MIN_ITEM_HEIGHT>",
                pretty, indent_size);
  }

  for (const auto &child : win.children) {
    write_window(child, out, depth + 1, pretty, indent_size);
  }

  append_line(out, depth, "</WINDOW>", pretty, indent_size);
}

void write_screen(const Screen &screen, std::string &out, int depth,
                  bool pretty, int indent_size) {
  append_line(out, depth, "<SCREEN>", pretty, indent_size);
  if (!screen.name.empty()) {
    append_line(out, depth + 1,
                "<NAME>" + escape_xml(screen.name) + "</NAME>", pretty,
                indent_size);
  }
  if (screen.music_var != 0) {
    append_line(out, depth + 1,
                "<MUSICVAR>" + std::to_string(screen.music_var) +
                    "</MUSICVAR>",
                pretty, indent_size);
  }
  // TEXT_RSRC and CURSOR go inside the root window for original game compatibility
  Window root = screen.root_window;
  if (!screen.text_rsrc.empty() && root.text_rsrc.empty()) {
    root.text_rsrc = screen.text_rsrc;
  }
  if ((!screen.cursor_file.empty() || !screen.cursor_flags.empty()) &&
      root.cursor.file.empty() && root.cursor.flags.empty()) {
    root.cursor.file = screen.cursor_file;
    root.cursor.flags = screen.cursor_flags;
  }

  write_window(root, out, depth + 1, pretty, indent_size);

  append_line(out, depth, "</SCREEN>", pretty, indent_size);
}

}  // namespace

std::string serialize(const Document &doc, bool pretty, int indent_size) {
  std::string out;
  for (const auto &screen : doc.screens) {
    write_screen(screen, out, 0, pretty, indent_size);
  }
  return out;
}

bool serialize_file(const Document &doc, const std::string &path,
                    std::string &error, bool pretty, int indent_size) {
  std::ofstream file(path, std::ios::binary);
  if (!file) {
    error = "Failed to open file for writing: " + path;
    return false;
  }
  std::string data = serialize(doc, pretty, indent_size);
  file.write(data.data(), static_cast<std::streamsize>(data.size()));
  if (!file) {
    error = "Failed to write file: " + path;
    return false;
  }
  return true;
}

}  // namespace mnu
