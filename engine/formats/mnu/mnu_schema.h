// The mnu property table (ADR 0046 d9, A16): every member the reader fills, named once
// by its element path, with how the writer decides it is written and what it refers to,
// and the ordered lists each record holds, in the order the writer emits them. The
// editor's menu document projects it onto the neutral editing core; nothing here names an
// editor type (formats/def/def_schema.h is the precedent).
//
// Which window type reads what (schema_reads) is retail's per-type parse chain, witnessed
// in full (docs/mnu/menu-re.md, "Which type reads what"); only what that record leaves
// open is Unverified.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include <formats/mnu/mnu.h>

namespace opennova::mnu {

// The records the table describes. A Part is a window an owner parses from one child
// element (LIST_BOX, SPINUP, SPINDOWN, SCROLLBAR): a window's fields but its TYPE, which
// the owner decides.
enum class SchemaShape {
  Screen, Window, Part, Appearance, Sound, Action, Hotkey, Datasource, Item, Row, Header, Body, Subst,
  Element, Attribute,
};

enum class SchemaType {
  Integer,
  Flag, // yes (1) / no (0)
  Text,
};

// How the writer decides a field is written.
enum class SchemaPresence {
  Always,   // always written (a flag: written when yes; element text: with its element)
  Bit,      // a has_* bit: optional; Clear unsets the bit and keeps the value
  NonEmpty, // written when not empty
  Block,    // the field is a block's own present bit (STRING, TOGGLE_STRING, ITEMS, a part):
            // 0 leaves the block out and keeps its content (mnu.h's presence contract)
};

// What a field names outside its record. Sound: a .lwf bank by file name; Credits: a
// marquee's credits file by file name; Screen: a screen by NAME in the menu file the
// ACTION's FILE names; Window: a window by NAME on the screen the ACTION's window is on.
// Dynamic: a sibling field decides (schema_reference resolves it per record).
enum class SchemaReference {
  None, Font, MenuTexture, StyleVar, TextTable, TextId, Menu, Sound, Credits, Screen, Window, Dynamic,
};

using SchemaValue = std::variant<int64_t, double, std::string>;

struct SchemaChoice {
  const char *name = "";
  int64_t value = 0;
};

struct SchemaField {
  const char *path = "";  // the element path: "name", "position.left", "string.value", "font.default_fg"
  SchemaType type = SchemaType::Text;
  size_t width = 0;       // a text's capacity in bytes, the terminator included (0 for numbers)
  SchemaPresence presence = SchemaPresence::Always;
  SchemaReference reference = SchemaReference::None;
  std::vector<SchemaChoice> choices; // the tokens the parse compares (a text field takes any text)
  const char *block = "";  // the enclosing block's field ("string", "items", ...; "" = none)
  // The choices are the tokens the parse knows, the text any list of them or other words
  // (the material FLAGS: ui_material_flag_choices).
  bool open = false;
};

// The material flag tokens a FLAGS text names, each with the word it ORs in: the parse
// splits the text on " ,|+" and matches each token, case aside, against this table; a
// token it lacks adds nothing [orig: sub_646CC0 @ 0x646cc0 over g_UIMaterialFlagNames
// @ 0x84a5d0, 43 rows; read for an APPEARANCE's FLAGS @ 0x6484fd and a CURSOR's
// @ 0x649586 in CUIElement_ParseXMLDefinition @ 0x648120].
const std::vector<SchemaChoice> &ui_material_flag_choices();

// An ordered list a record holds.
struct SchemaList {
  const char *path = "";         // the list's element path, its token: "action", "items.item", "list_box", "window"
  const char *label = "";        // "Actions"
  const char *record_label = ""; // "Action"
  SchemaShape shape = SchemaShape::Window;
  const char *block = "";        // the owner's block field that gates it ("items"; "" = none)
  size_t max = 0;                // 0 = any number; 1 = a part
  const char *name_field = "";   // the field that names a record ("" = none)
};

// The fields of a shape, and the lists a shape holds (a Screen its root windows; a Window
// or a Part its lists in writer order, its child windows last; a Row its cells; an Element
// its attributes and child elements; the others none).
const std::vector<SchemaField> &schema_fields(SchemaShape shape);
const SchemaField *schema_field(SchemaShape shape, const std::string &path);
const std::vector<SchemaList> &schema_lists(SchemaShape owner);

// A record of the table: its shape and the native struct (Screen, Window for a Window or
// a Part, Appearance, Sound, Action, Hotkey, std::string for a Datasource, Item,
// TableRow, TableHeader, TableBody, TableSubst, Element, ElementAttribute).
struct SchemaRecord {
  SchemaShape shape = SchemaShape::Screen;
  void *data = nullptr;
  explicit operator bool() const { return data != nullptr; }
};
SchemaRecord schema_screen(Screen &screen);
SchemaRecord schema_window(Window &window);

bool schema_get(const SchemaRecord &record, const std::string &path, SchemaValue &out);
// Whether the writer writes the field: its own bit, a non-empty value, and every block
// that encloses it. A block's own toggle is always present (its value says whether the
// block is written).
bool schema_present(const SchemaRecord &record, const std::string &path);
// A value within the field's type and width. A Set of the value the field already reads
// changes nothing (an unauthored field stays unauthored, so a Set of every field to its
// own value leaves the bytes as they are); any other value authors the field's bit and
// the block that encloses it. False, with `error`, when the value does not fit.
bool schema_set(const SchemaRecord &record, const std::string &path, const SchemaValue &value, std::string &error);
// An optional (Bit) field left out of the file or written again with the value it reads
// (writing one authors its enclosing block). False for any other field.
bool schema_set_present(const SchemaRecord &record, const std::string &path, bool present, std::string &error);
// What the field names on this record: a Dynamic one resolved (an APPEARANCE's value is a
// texture for IMAGE / IMAGEROW, a style colour for COLOR / OUTLINE; an ITEM's text a string
// id for ID, a texture for IMAGE / BITMAP, a colour for COLOR; a HEADER's text and the
// STRING's and TOGGLE_STRING's value a string id for ID; a SUBST's text a texture when it
// is a FILE and not a URL; an ACTION's target a screen for SCREEN, a window for WINDOW,
// TAB, GLB_FILTER and GLB_FILTER_NUM, and its FIELD / SOURCE / NAME slot a window for URL),
// None when it names nothing here.
SchemaReference schema_reference(const SchemaRecord &record, const std::string &path);

// --- the lists ------------------------------------------------------------------------
size_t schema_list_size(const SchemaRecord &owner, size_t list);
// The record at an index (a part's window whether it is written or left out).
SchemaRecord schema_list_at(const SchemaRecord &owner, size_t list, size_t index);
// Whether the list's records are written: the owner's block is (ITEMS for items.*), and a
// part is written, not left out.
bool schema_list_present(const SchemaRecord &owner, size_t list);
// The typed-mutator rule (mnu.h): a record added to a list authors the block that gates
// it (ITEMS for items.*).
void schema_author_list(const SchemaRecord &owner, size_t list);

// A copy of one record held apart from its list (a record between its removal and its
// insertion elsewhere, a duplicate, a clipboard window).
struct SchemaDetached {
  SchemaShape shape = SchemaShape::Window;
  std::shared_ptr<void> data;
  bool shown = true; // a part: written (false: left out)
};
SchemaDetached schema_list_copy(const SchemaRecord &owner, size_t list, size_t index);
SchemaDetached schema_detach_window(const Window &window);
// Inserts `record` (a default one when null) at min(index, size); a part list holds one
// window at most. The default of each list survives the writer's skips and reads back as
// written: a window of type STATIC at 0,0,100,20 with a typeless DEFAULT appearance (the
// part's own type for a part), an APPEARANCE with STATE DEFAULT, a SOUND on MOUSEIN with
// the MOUSE_OVER trigger, an ACTION of type POP_SCREEN (the one verb that takes no operand
// [orig: CUIWidget_HandleScriptedAction @ 0x6497f0, code 12]), a HEADER and a BODY with
// their COLUMN index written (the new column's), a window's PLAYERLIST attribute, an extra
// element TARGET. The list's block is authored.
bool schema_list_insert(const SchemaRecord &owner, size_t list, size_t index, const SchemaDetached *record,
                        std::string &error);
bool schema_list_erase(const SchemaRecord &owner, size_t list, size_t index);

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
