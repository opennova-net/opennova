// The mnu format's rules a property table reads (ADR 0046 d9, A16): which window type reads what, by
// the element path a member is named by ("string.value", "items.item", "column.header.text"), what an
// ACTION's verb reads, which extra elements a type reads at its top level, and the material flag
// tokens a FLAGS text names. The property table itself (each record's members by their element paths,
// how the writer decides each is written, the lists each record holds) is the editor's menu table,
// rows of its one table shape (engine/editor/documents/mnu_table, ADR 0046 S13 D10); what stays here
// is the reader's witnessed rules, which name no editor type.
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
