#pragma once

#include <cstddef>
#include <string>

#include <editor/model/table_shape.h>
#include <formats/mnu/mnu.h>

namespace opennova::editor {

// The menu's table (ADR 0046 S9h; S13 D10 made it rows of the one table shape, model/table_shape.h,
// from the format's property table it was): every member the reader fills, named once by its element
// path ("name", "position.left", "string.value", "font.default_fg"), with how the writer decides it is
// written and what it refers to, and the ordered lists each record holds, in the order the writer
// emits them, the child windows last (so a walk meets them in file order). Which window type reads
// what is the format's witnessed rule (formats/mnu/mnu_schema.h), which the menu document applies
// where a record sits.
//
// The kinds: Screen, Window, then one kind per list, named by the list's element path ("attribute",
// "action", "items.item", "list_box", "column.header"; a path two owners share, "attribute" and
// "element", is one kind), a table row's cells last ("item"). A part (LIST_BOX, SPINUP, SPINDOWN,
// SCROLLBAR) is a window an owner parses from one child element: a window's fields but its TYPE,
// which the owner decides, in a list that holds one window at most.

enum class MenuKind : NodeKind { Screen = 0, Window = 1 };
constexpr NodeKind node_kind(MenuKind kind) { return static_cast<NodeKind>(kind); }

// The native record a menu kind's handle holds: Screen mnu::Screen; Window and Part mnu::Window;
// Appearance (the appearance, shuttle, scroll and item-appearance kinds) mnu::Appearance; Sound,
// Action, Hotkey; Datasource a std::string; Item (an ITEMS row and a table row's cell) mnu::Item; Row
// mnu::TableRow; Header, Body, Subst the table's columns; Element and Attribute an extra element and
// its attribute.
enum class MenuShape {
	Screen, Window, Part, Appearance, Sound, Action, Hotkey, Datasource, Item, Row, Header, Body, Subst,
	Element, Attribute,
};

const RecordTable &menu_table();
MenuShape menu_shape(NodeKind kind);
// A window or a part: a record whose fields and lists are a window's.
bool is_window_kind(NodeKind kind);
// The kind of a list's records by its element path ("action", "items.item"); -1 when none.
NodeKind menu_kind(const std::string &token);
// A window's (or a part's) list by its element path: its place among the window's lists, or npos.
size_t menu_window_list(const std::string &path);
// The window's list of child windows.
size_t menu_children_list();

// A window held apart, as a window's or a screen's list of windows takes it (a clipboard window).
DetachedRecord menu_window_record(const mnu::Window &window);

} // namespace opennova::editor
