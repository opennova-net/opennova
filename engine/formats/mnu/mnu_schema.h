// The mnu format's witnessed rules a property table reads (ADR 0046 d9, A16; S13 D10): which window
// type reads what, by the element path a member is named by ("string.value", "items.item",
// "column.header.text"), what an ACTION's verb reads, which extra elements a type reads at its top
// level, what a field names outside its record (by the record's own siblings where they decide it),
// what a new record of each list is written as, how a window's TYPE token builds its window, whose
// TEXT_RSRC a part's string ids fall back to, and the material flag tokens a FLAGS text names. The
// property table itself (each record's members by their element paths, how the writer decides each is
// written, the lists each record holds) is the editor's menu table, rows of its one table shape
// (engine/editor/documents/mnu_table), which maps these answers; what stays here is the reader's and
// the writer's witnessed rules, which name no editor type, so the format and the runtime read them too.
//
// Which window type reads what (schema_reads) is retail's per-type parse chain, witnessed in full
// (docs/mnu/menu-re.md, "Which type reads what"); only what that record leaves open is Unverified.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <formats/mnu/mnu.h>

namespace opennova::mnu {

struct SchemaChoice {
  const char *name = "";
  int64_t value = 0;
};

// The material flag tokens a FLAGS text names, each with the word it ORs in: the parse
// splits the text on " ,|+" and matches each token, case aside, against this table; a
// token it lacks adds nothing [orig: sub_646CC0 @ 0x646cc0 over g_UIMaterialFlagNames
// @ 0x84a5d0, 43 rows; read for an APPEARANCE's FLAGS @ 0x6484fd and a CURSOR's
// @ 0x649586 in CUIElement_ParseXMLDefinition @ 0x648120].
const std::vector<SchemaChoice> &ui_material_flag_choices();

// --- the records the rules speak of --------------------------------------------------------
// Each a native struct of mnu.h: a screen (Screen), a window (Window), a part (a Window an owner
// parses from one child element, LIST_BOX, SPINUP, SPINDOWN or SCROLLBAR: a window's fields but its
// TYPE, which the owner decides), an APPEARANCE (Appearance; the shuttle's, the scroll arrows' and the
// ITEMS' too), a SOUND, an ACTION, a HOTKEY, a DATASOURCE (its text), an ITEM (an ITEMS row's and a
// table row's cell), a table ROW, a HEADER, a BODY, a SUBST, an extra element (Element) and its
// attribute (ElementAttribute).
enum class SchemaShape {
  Screen, Window, Part, Appearance, Sound, Action, Hotkey, Datasource, Item, Row, Header, Body, Subst,
  Element, Attribute,
};
inline constexpr size_t kSchemaShapeCount = size_t(SchemaShape::Attribute) + 1;

// --- what a field names ----------------------------------------------------------------------
// What a field names outside its record (docs/mnu/menu-re.md, "Names and the lookups"): a font by
// name, a texture the menu loader picks by the name's extension, a style variable (a style colour's
// %NAME%, schema_hex_colour), a string table, a string id, a menu file, a sound bank by its file name,
// a marquee's credits file by its file name, a screen by NAME in the menu file an ACTION's FILE names,
// a window by NAME on the acting window's screen.
enum class SchemaReference {
  None, Font, MenuTexture, StyleVar, TextTable, TextId, Menu, Sound, Credits, Screen, Window,
};
// What the field `path` of a record of `shape` names whatever its record holds: None for a field that
// names nothing and for one whose record's siblings decide it (schema_reference_varies).
SchemaReference schema_field_reference(SchemaShape shape, const std::string &path);
// Whether a sibling of the field decides what it names (an APPEARANCE's value by its TYPE).
bool schema_reference_varies(SchemaShape shape, const std::string &path);
// What the field names on this record (`record` the shape's native struct): its own reference, or
// what its siblings decide here.
SchemaReference schema_reference(SchemaShape shape, const std::string &path, const void *record);
// Whether a field naming `reference` holds a colour word: a style colour's text is the hex AARRGGBB
// word the parse reads with wcstoul (a %VAR% the stylesheet resolves first).
bool schema_hex_colour(SchemaReference reference);

// --- what a new record is ----------------------------------------------------------------------
// The defaults a list's new record takes, each of which the writer writes and the reader reads back as
// written: a window of type STATIC at 0,0,100,20 with a typeless DEFAULT appearance, an APPEARANCE with
// STATE DEFAULT, a SOUND on MOUSEIN with the MOUSE_OVER trigger, an ACTION of type POP_SCREEN, an extra
// element TARGET, a HEADER and a BODY with their COLUMN written (the new column's index), a SUBST with
// its COLUMN written, a window's attribute PLAYERLIST and an element's attribute NAME.
void schema_default(Window &window);
void schema_default(Appearance &appearance);
void schema_default(Sound &sound);
void schema_default(Action &action);
void schema_default(Element &element);
void schema_default(TableHeader &header, size_t column);
void schema_default(TableBody &body, size_t column);
void schema_default(TableSubst &subst);
void schema_default_window_attribute(ElementAttribute &attribute);
void schema_default_element_attribute(ElementAttribute &attribute);
// A part authored with its window of `type`, which, new, takes a typeless DEFAULT appearance.
Window &schema_default_part(WindowPart &part, WindowType type);

// --- a window's TYPE ---------------------------------------------------------------------------
// The TYPE as the file writes it (the token as typed, else the type's own name), and a TYPE set: the
// type the factory builds for the token and the token kept as typed.
std::string schema_type_token(const Window &window);
void schema_set_type_token(Window &window, const std::string &token);

// --- a part's string ids ------------------------------------------------------------------------
// Whether a part's string ids fall back to its root window's TEXT_RSRC (`part_path` the part's element
// path: "list_box", "spinup", "spindown", "scrollbar"), or are its own alone.
bool schema_part_reads_root_text(const std::string &part_path);

// --- which type reads what -------------------------------------------------------------
enum class SchemaApplies { Reads, Ignored, Unverified };
// Whether a window of `type` reads a field or a list, by its element path from the window
// ("string.value", "items.item", "items.item.pairs_list", "column.header.text"): the
// per-type parse chains (grill set A3; docs/mnu/menu-re.md "Which type reads what").
SchemaApplies schema_reads(WindowType type, const std::string &path);
// Whether an ACTION's verb reads one of its fields [orig: CUIElement_ParseXMLDefinition
// @ 0x648120 ACTION arm; CUIWidget_HandleScriptedAction @ 0x6497f0].
SchemaApplies schema_action_reads(const Action &action, const std::string &field);
// Whether a window of `type` reads an extra element of `tag` at its top level.
SchemaApplies schema_element_reads(WindowType type, const std::string &tag);
// The stricter of two answers (Ignored, then Unverified, then Reads).
SchemaApplies schema_applies_both(SchemaApplies a, SchemaApplies b);

}  // namespace opennova::mnu
