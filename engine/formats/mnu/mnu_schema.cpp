// The mnu format's rules a property table reads (mnu_schema.h): the material flag tokens a FLAGS
// text names, and which window type reads what.
#include <formats/mnu/mnu_schema.h>

#include <cstring>

#include <base/io/strutil.h>

namespace opennova::mnu {

using strutil::iequals;

const std::vector<SchemaChoice> &ui_material_flag_choices() {
  // g_UIMaterialFlagNames @ 0x84a5d0, in table order (rows of wchar name[64] + DWORD flags).
  static const std::vector<SchemaChoice> choices = {
      {"STANDARD", 0x300600}, {"STANDARD_TRANSPARENT", 0x300651}, {"AFUNC_BITMASK", 0xF},
      {"ASRC_BITMASK", 0xF0}, {"COLOR_BITMASK", 0xF00}, {"AFUNC_NONE", 0x0}, {"AFUNC_BLEND", 0x1},
      {"AFUNC_SRCCOPY", 0x0}, {"AFUNC_ADD", 0x2}, {"AFUNC_SHADE", 0x3}, {"AFUNC_GLOW", 0x4},
      {"AFUNC_ALPHALIGHT", 0x5}, {"AFUNC_MULTIPLY", 0x6}, {"AFUNC_DEST", 0x7}, {"AFUNC_MULTIPLYDBL", 0x8},
      {"AFUNC_SQR", 0x9}, {"AFUNC_SQRADD", 0xA}, {"ASRC_NONE", 0x0}, {"ASRC_CONSTANT", 0x10},
      {"ASRC_ITERATED", 0x20}, {"ASRC_TEXTURE", 0x30}, {"ASRC_TEXTURExCONSTANT", 0x40},
      {"ASRC_TEXTURExITERATED", 0x50}, {"COLOR_NONE", 0x0}, {"COLOR_CONSTANT", 0x100},
      {"COLOR_ITERATED", 0x200}, {"COLOR_ITERATEDxCONSTANT", 0x300}, {"COLOR_TEXTURE", 0x400},
      {"COLOR_TEXTURExCONSTANT", 0x500}, {"COLOR_TEXTURExITERATED", 0x600}, {"COLOR_TEXTURERAMP", 0x700},
      {"COLOR_TEXTUREdpCONSTANT", 0x800}, {"COLOR_TEXTUREdpITERATED", 0x900}, {"COLOR2_SQRD", 0x1000},
      {"APPLYSPECULAR", 0x10000}, {"APPLYFOG", 0x20000}, {"ALPHATEST", 0x40000}, {"NATIVE2X", 0x80000},
      {"NOWRITEDEPTH", 0x100000}, {"NOCHECKDEPTH", 0x200000}, {"GMAT_OVR_CLAMPUV1", 0x1000000},
      {"GMAT_OVR_CLAMPUV2", 0x2000000}, {"TINTASTEXT", 0x80000000LL},
  };
  return choices;
}

// --- which type reads what -----------------------------------------------------------

namespace {

using Types = uint32_t;
constexpr Types type_bit(WindowType t) { return Types(1) << unsigned(t); }
constexpr Types kAllTypes = (Types(1) << unsigned(kWindowTypeCount)) - 1;
using T = WindowType;
// The parse chains (grill set A3, docs/mnu/menu-re.md "Which type reads what"): every
// type's parse runs the base parse first [orig: CUIElement_ParseXMLDefinition @
// 0x648120]; the S-chain reads STRING [orig: CUIButtonWidget_ParseXMLAttributes @
// 0x657c30], the B-chain TOGGLE_STRING [orig: CButtonWnd_ParseTooltipXML @ 0x658170], the
// R-chain CHECKED and GROUP [orig: CRadioWnd_ParseXMLDefinition @ 0x656c40]; RADIOEDIT
// feeds its elements to an embedded edit and an embedded radio [orig:
// CRadioEditWnd_ParseXMLDefinition @ 0x65d1c0].
constexpr Types kS = type_bit(T::Static) | type_bit(T::Button) | type_bit(T::Edit) | type_bit(T::MultilineEdit) |
                     type_bit(T::Radio) | type_bit(T::CheckBox) | type_bit(T::SpinList) | type_bit(T::List) |
                     type_bit(T::LanList) | type_bit(T::Table) | type_bit(T::Combo) | type_bit(T::RadioEdit);
constexpr Types kB = type_bit(T::Button) | type_bit(T::Radio) | type_bit(T::CheckBox) | type_bit(T::SpinList) |
                     type_bit(T::List) | type_bit(T::LanList) | type_bit(T::Table) | type_bit(T::RadioEdit);
constexpr Types kR = type_bit(T::Radio) | type_bit(T::List) | type_bit(T::LanList) | type_bit(T::Table) |
                     type_bit(T::RadioEdit);
constexpr Types kEditAttributes = type_bit(T::Edit) | type_bit(T::MultilineEdit) | type_bit(T::RadioEdit);
constexpr Types kLists = type_bit(T::List) | type_bit(T::LanList);

struct ReadRule {
  const char *path;
  Types types;
};
// A path with no rule (nor a rule for a path it extends) is the base parse's: every type.
const ReadRule kReadRules[] = {
    {"string", kS},
    {"toggle_string", kB},
    {"checked", kR | type_bit(T::CheckBox)},  // [orig: @ 0x656c62, CCheckWnd_ParseXMLDefinition @ 0x64adce]
    {"group", kR},                           // the element [orig: @ 0x656c87]
    {"as_button", type_bit(T::CheckBox)},    // [orig: @ 0x64adb2]
    // [orig: CEditWnd_ParseXMLProperties @ 0x661d10]
    {"password", kEditAttributes}, {"number", kEditAttributes}, {"maxchar", kEditAttributes},
    {"readonly", kEditAttributes}, {"maxval", kEditAttributes}, {"minval", kEditAttributes},
    {"datasource", type_bit(T::Marquee)},    // [orig: CMarqueeWnd_ParseXMLDefinition @ 0x65cee5]
    // [orig: CUIScrollWidget_ParseExtendedXMLDef @ 0x64c6d0]
    {"orientation", type_bit(T::Scroll)}, {"scroll_extent", type_bit(T::Scroll)},
    {"scroll_extent_width", type_bit(T::Scroll)}, {"shuttle", type_bit(T::Scroll)},
    {"scrollup", type_bit(T::Scroll)}, {"scrolldown", type_bit(T::Scroll)},
    // The ITEMS shapes: the LIST form [orig: CListWnd_ParseXMLDefinition @ 0x6457b6: JUSTIFY,
    // VJUSTIFY, MULTISELECT; ITEM and APPEARANCE rows; ITEM JUSTIFY, VJUSTIFY, PAIRS_LIST,
    // TYPE, VALUE], the SPINLIST form [orig: CUISpinList_ParseXMLDefinition @ 0x64bd57:
    // JUSTIFY, VJUSTIFY; ITEM rows only, TYPE ID / IMAGE / COLOR], the TABLE form [orig:
    // CTableWnd_ParseXMLContentDefinition @ 0x642816: MULTISELECT; ROW > ITEM cells with
    // COLUMN, JUSTIFY, VJUSTIFY, VALUE, TYPE; APPEARANCE rows]. COMBOBOX reads ITEMS only in
    // its LIST_BOX, GLB_TABLE only inside its own elements.
    {"items", kLists | type_bit(T::SpinList) | type_bit(T::Table)},
    {"items.multiselect", kLists | type_bit(T::Table)},
    {"items.justify", kLists | type_bit(T::SpinList)},
    {"items.vjustify", kLists | type_bit(T::SpinList)},
    {"items.appearance", kLists | type_bit(T::Table)},
    {"items.item", kLists | type_bit(T::SpinList)},
    {"items.item.pairs_list", kLists},
    {"items.item.column", 0},
    {"items.row", type_bit(T::Table)},
    {"items.row.item.pairs_list", 0},
    {"list_box", type_bit(T::Combo)},        // [orig: CComboWnd_ParseXMLDefinition @ 0x65c0f2]
    {"spinup", type_bit(T::SpinList)},       // [orig: @ 0x64c2fd]
    {"spindown", type_bit(T::SpinList)},     // [orig: @ 0x64c31a]
    {"column", type_bit(T::Table)},          // [orig: @ 0x6430f6]
    // [orig: @ 0x64603d (LIST), 0x643afd (TABLE), 0x6608a4 (MULTILINE_EDIT)]
    {"scrollbar", kLists | type_bit(T::Table) | type_bit(T::MultilineEdit)},
    {"min_item_height", kLists | type_bit(T::Table)},  // [orig: @ 0x64600d, 0x643a9f]
    {"fixed_header_height", type_bit(T::Table)},       // [orig: @ 0x643ace]
    // [orig: CLanListWnd_ParseXMLDefinition @ 0x65b9d0; CLanGameBrowser_ParseExtendedXMLDefinition
    // @ 0x65dac0; CGopherWnd_ParseXMLDefinition @ 0x65b130]
    {"element", type_bit(T::LanList) | type_bit(T::GlbTable) | type_bit(T::Gopher)},
    {"attribute", type_bit(T::GlbTable)},
};

}  // namespace

SchemaApplies schema_reads(WindowType type, const std::string &path) {
  // The path, then each shorter path it extends ("items.item.value", "items.item", "items").
  size_t length = path.size();
  for (;;) {
    for (const ReadRule &rule : kReadRules)
      if (std::strlen(rule.path) == length && path.compare(0, length, rule.path) == 0)
        return (rule.types & type_bit(type)) ? SchemaApplies::Reads : SchemaApplies::Ignored;
    const size_t dot = path.rfind('.', length ? length - 1 : 0);
    if (dot == std::string::npos || dot >= length) break;
    length = dot;
  }
  return (kAllTypes & type_bit(type)) ? SchemaApplies::Reads : SchemaApplies::Ignored;
}

SchemaApplies schema_action_reads(const Action &action, const std::string &field) {
  // [orig: CUIElement_ParseXMLDefinition @ 0x648ee2 ACTION arm; CUIWidget_HandleScriptedAction
  // @ 0x6497f0]: sixteen verbs; any other token (or none) is code 0, ignored on every path.
  if (field == "type") return SchemaApplies::Reads;
  if (!known_token(action.type, kActionTypes)) return SchemaApplies::Ignored;
  auto only = [&](std::initializer_list<const char *> readers) {
    for (const char *verb : readers)
      if (iequals(action.type, verb)) return SchemaApplies::Reads;
    return SchemaApplies::Ignored;
  };
  if (field == "file") return only({"SCREEN"});                       // kept for SCREEN only
  if (field == "field" || field == "field_attr")                       // kept for these four
    return only({"URL", "GLB_FILTER", "GLB_FILTER_NUM", "GLB_JOIN"});
  if (field == "state" || field == "toggle") return only({"WINDOW"});  // the WINDOW state switch @ 0x6498f7
  if (field == "external_browser") return only({"URL"});
  // TARGET_FORM is stored for FORM_POST; where the post reads it is not witnessed
  // (UI_BuildURLAndSubmitRequest @ 0x63e3f0 takes only the scene).
  if (field == "target_form") return iequals(action.type, "FORM_POST") ? SchemaApplies::Unverified : SchemaApplies::Ignored;
  return SchemaApplies::Reads; // TEST (stored for every verb) and the target text
}

SchemaApplies schema_element_reads(WindowType type, const std::string &tag) {
  struct TagRule {
    const char *tag;
    Types types;
  };
  static const TagRule rules[] = {
      {"JOIN_BUTTON", type_bit(T::LanList)},     // [orig: @ 0x65b9e4]
      {"SEARCH_BUTTON", type_bit(T::LanList)},   // [orig: @ 0x65ba00]
      {"GLB_TABLE", type_bit(T::GlbTable)},      // [orig: @ 0x65dae8 .. 0x65e012]
      {"GLB_INFO_BUTTON", type_bit(T::GlbTable)},
      {"GLB_INFO_PLAYER_DETAILS", type_bit(T::GlbTable)},
      {"GLB_INFO_SERVER_DETAILS", type_bit(T::GlbTable)},
      {"GLB_NW_INFO", type_bit(T::GlbTable)},
      {"TARGET", type_bit(T::GlbTable) | type_bit(T::Gopher)},
      {"FILTERS", type_bit(T::Gopher)},          // [orig: @ 0x65b167 .. 0x65b2af]
      {"GOPHER_BACK", type_bit(T::Gopher)},
  };
  for (const TagRule &rule : rules)
    if (iequals(tag, rule.tag)) return (rule.types & type_bit(type)) ? SchemaApplies::Reads : SchemaApplies::Ignored;
  return SchemaApplies::Ignored;
}

SchemaApplies schema_applies_both(SchemaApplies a, SchemaApplies b) {
  if (a == SchemaApplies::Ignored || b == SchemaApplies::Ignored) return SchemaApplies::Ignored;
  if (a == SchemaApplies::Unverified || b == SchemaApplies::Unverified) return SchemaApplies::Unverified;
  return SchemaApplies::Reads;
}

}  // namespace opennova::mnu
