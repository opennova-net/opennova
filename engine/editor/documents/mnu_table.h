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
// The kinds: Screen, Window, then one kind per list, named by the list's element path ("action",
// "items.item", "list_box", "column.header", "gopher_categories"), a table row's cells last ("item"). A
// part (LIST_BOX, SPINUP, SPINDOWN, SCROLLBAR; a LAN_LIST's, a GLB_TABLE's and a GOPHER's own elements;
// a gopher category's LABEL) is a window an owner parses from one child element: a window's fields but
// its TYPE, which the owner decides, in a list that holds one window at most. Each kind is a record of one of
// the format's shapes (mnu::SchemaShape): Screen mnu::Screen; Window and Part mnu::Window; Appearance
// (the appearance, shuttle, scroll and item-appearance kinds) mnu::Appearance; Sound, Action, Hotkey;
// Datasource a std::string; Item (an ITEMS row and a table row's cell) mnu::Item; Row mnu::TableRow;
// Header, Body, Subst the table's columns; Category a gopher's FILTERS CATEGORY (mnu::GopherCategory), Entry
// its ITEM or INFO row, FieldSlot a GLB server details FIELD (the gopher's TARGETs Datasource strings).

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

// A window's TYPE set (ADR 0046 S27): the window becomes what the game reads of it after the change. The factory
// builds the widget its TYPE names and that widget's parse chain reads the window's elements [orig:
// CUIScene_CreateWidgetByType @ 0x64f630; the chains, mnu::schema_reads], and the writer puts down what the old type
// reads (formats/mnu mnu_write), so what the new type reads of the window is what both types read: a field, a list
// or a part both read keeps (a latent value too, mnu.h's presence contract); the rest goes, a field as the zeroed
// record holds it, a list's records with their identities (`ids`, the window's own). An ITEM's TYPE and an ITEMS
// APPEARANCE's keep a token both types' forms read [orig: CListWnd_ParseXMLDefinition @ 0x645770;
// CUISpinList_ParseXMLDefinition @ 0x64bd10; CTableWnd_ParseXMLContentDefinition @ 0x6427d0]; a TARGET a GLB_TABLE
// and a GOPHER read into members of their own goes over as the other's arm reads it (its new records' identities
// 0, which the batch numbers). The child windows,
// which the base parse creates under every type [orig: CUIElement_ParseXMLDefinition @ 0x649772..0x649789], and the
// parts kept are their own types' and stay as they are.
void menu_set_window_type(const RecordHandle &window, RecordIds &ids, mnu::WindowType type);

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
// The TYPE tokens a row's or an item's parse knows where the record sits (nullptr-ended, none for any
// other record): an ITEM's its form's [orig: CListWnd_ParseXMLDefinition @ 0x645770, the compare @
// 0x645b0f..0x645b26: a LIST's ID alone; CUISpinList_ParseXMLDefinition @ 0x64bd10: ID, IMAGE, COLOR;
// CTableWnd_ParseXMLContentDefinition @ 0x6427d0: a cell's ID, BITMAP] (mnu::item_types_for), an ITEMS
// APPEARANCE's a TABLE's IMAGEROW too [orig: @ 0x642816], any other APPEARANCE row's the window's
// (mnu::kAppearanceTypes). The reader holds any other token as none; so does the game.
const char *const *menu_type_tokens(const RecordHandle &record, const RecordOwners &owners);
// The colour a field's text holds by what it names (the format's rule: a style colour's hex word).
FieldColor menu_reference_colour(ReferenceKind reference);

} // namespace opennova::editor
