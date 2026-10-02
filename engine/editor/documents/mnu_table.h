#pragma once

#include <cstddef>
#include <string>

#include <editor/model/table_shape.h>
#include <formats/mnu/mnu.h>
#include <formats/mnu/mnu_schema.h>

namespace opennova::editor {

// The menu's table (ADR 0046 S9h; S13 D10 made it rows of the one table shape, model/table_shape.h,
// from the format's property table it was): every member the reader fills, named once by its element
// path ("name", "position.left", "string.value", "font.default_fg"), with how the writer decides it is
// written, and the ordered lists each record holds, in the order the writer emits them, the child
// windows last (so a walk meets them in file order). What the format witnesses stays the format's
// (formats/mnu/mnu_schema.h), which the table maps: which window type reads what, what a field names
// on its record, what a new record is written as, a window's TYPE token, whose TEXT_RSRC a part's
// string ids fall back to.
//
// The kinds: Screen, Window, then one kind per list, named by the list's element path ("attribute",
// "action", "items.item", "list_box", "column.header"; a path two owners share, "attribute" and
// "element", is one kind), a table row's cells last ("item"). A part (LIST_BOX, SPINUP, SPINDOWN,
// SCROLLBAR) is a window an owner parses from one child element: a window's fields but its TYPE,
// which the owner decides, in a list that holds one window at most. Each kind is a record of one of
// the format's shapes (mnu::SchemaShape): Screen mnu::Screen; Window and Part mnu::Window; Appearance
// (the appearance, shuttle, scroll and item-appearance kinds) mnu::Appearance; Sound, Action, Hotkey;
// Datasource a std::string; Item (an ITEMS row and a table row's cell) mnu::Item; Row mnu::TableRow;
// Header, Body, Subst the table's columns; Element and Attribute an extra element and its attribute.

enum class MenuKind : NodeKind { Screen = 0, Window = 1 };
constexpr NodeKind node_kind(MenuKind kind) { return static_cast<NodeKind>(kind); }

const RecordTable &menu_table();
mnu::SchemaShape menu_shape(NodeKind kind);
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

// A record of a screen (never the screen itself) as the format's paths read it, from the records it
// lies in: the nearest window or part at or above it (a window's own), the element path from that
// window to the record's list ("" for a window or a part), whether the lists above it are read, the
// root window it hangs under, and the window whose TEXT_RSRC its string ids fall back to (the root,
// or for a part parsed before it is attached the part itself: menu::window_text_rsrc). A screen (and a
// record given without the records it lies in that is no window) has no window: what it holds is
// read.
struct MenuContext {
	const mnu::Window *window = nullptr;
	std::string prefix;
	mnu::SchemaApplies applies = mnu::SchemaApplies::Reads;
	const mnu::Window *root = nullptr;
	const mnu::Window *text_fallback = nullptr;
};
MenuContext menu_context(const RecordHandle &record, const RecordOwners &owners);
// Whether the game reads list `list` of a window (or a part, or a record under one) where it sits.
mnu::SchemaApplies menu_list_reads(const RecordHandle &owner, const RecordOwners &owners, size_t list);
// The format's answer as the editor's.
Applicability menu_applicability(mnu::SchemaApplies applies);
// The colour a field's text holds by what it names (the format's rule: a style colour's hex word).
FieldColor menu_reference_colour(ReferenceKind reference);

} // namespace opennova::editor
