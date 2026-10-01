// The mnu format's rules a property table reads (formats/mnu/mnu_schema, ADR 0046 A16 and S9h):
// which window type reads what (grill set A3 and the ITEMS arms; docs/mnu/menu-re.md), what an
// ACTION's verb reads, which extra elements a type reads, the stricter of two answers, the material
// flag tokens a FLAGS text names, and since S13 D10 what a field names, what a new record is, a
// window's TYPE token and whose TEXT_RSRC a part's string ids fall back to. The property table those rules serve (every member the
// reader fills by its element path, the lists, the defaults) is the editor's menu table since ADR 0046
// S13 D10 (tests/editor/menu_table_test.cpp).
#include <cstdlib>
#include <iostream>
#include <string>

#include <formats/mnu/mnu.h>
#include <formats/mnu/mnu_schema.h>

namespace {

namespace mnu = opennova::mnu;

#define CHECK(cond, msg)                                                   \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::cerr << "FAIL: " << msg << " at line " << __LINE__ << "\n";     \
      return false;                                                        \
    }                                                                      \
  } while (0)

// Which type reads what (grill set A3 and the ITEMS arms; docs/mnu/menu-re.md).
bool test_applicability() {
  using mnu::SchemaApplies;
  using T = mnu::WindowType;
  CHECK(mnu::schema_reads(T::Window, "string.value") == SchemaApplies::Ignored, "no STRING on a generic window");
  CHECK(mnu::schema_reads(T::Static, "string.value") == SchemaApplies::Reads, "STATIC reads STRING");
  CHECK(mnu::schema_reads(T::Combo, "toggle_string") == SchemaApplies::Ignored, "COMBOBOX chains to STATIC");
  CHECK(mnu::schema_reads(T::Radio, "group") == SchemaApplies::Reads &&
            mnu::schema_reads(T::Combo, "group") == SchemaApplies::Ignored,
        "GROUP on the R-chain");
  CHECK(mnu::schema_reads(T::Combo, "maxchar") == SchemaApplies::Ignored &&
            mnu::schema_reads(T::RadioEdit, "maxchar") == SchemaApplies::Reads,
        "the edit attributes");
  CHECK(mnu::schema_reads(T::Marquee, "datasource") == SchemaApplies::Reads &&
            mnu::schema_reads(T::Static, "datasource") == SchemaApplies::Ignored,
        "DATASOURCE on MARQUEE_WND only");
  CHECK(mnu::schema_reads(T::Table, "items.item") == SchemaApplies::Ignored &&
            mnu::schema_reads(T::Table, "items.row.item.value") == SchemaApplies::Reads,
        "a TABLE reads ROW cells, not ITEMs");
  CHECK(mnu::schema_reads(T::SpinList, "items.appearance") == SchemaApplies::Ignored &&
            mnu::schema_reads(T::SpinList, "items.item.text") == SchemaApplies::Reads &&
            mnu::schema_reads(T::SpinList, "items.item.pairs_list") == SchemaApplies::Ignored,
        "the SPINLIST form");
  CHECK(mnu::schema_reads(T::List, "items.item.column") == SchemaApplies::Ignored, "no COLUMN on a LIST ITEM");
  CHECK(mnu::schema_reads(T::Combo, "list_box") == SchemaApplies::Reads &&
            mnu::schema_reads(T::List, "list_box") == SchemaApplies::Ignored,
        "LIST_BOX on COMBOBOX");
  CHECK(mnu::schema_reads(T::MultilineEdit, "scrollbar") == SchemaApplies::Reads &&
            mnu::schema_reads(T::Edit, "scrollbar") == SchemaApplies::Ignored,
        "SCROLLBAR");
  CHECK(mnu::schema_reads(T::Button, "position.left") == SchemaApplies::Reads &&
            mnu::schema_reads(T::Gopher, "text_rsrc") == SchemaApplies::Reads,
        "the base parse: every type");
  CHECK(mnu::schema_element_reads(T::LanList, "JOIN_BUTTON") == SchemaApplies::Reads &&
            mnu::schema_element_reads(T::GlbTable, "JOIN_BUTTON") == SchemaApplies::Ignored &&
            mnu::schema_element_reads(T::Gopher, "target") == SchemaApplies::Reads,
        "the extras by tag");
  mnu::Action action;
  action.type = "window";
  CHECK(mnu::schema_action_reads(action, "state") == SchemaApplies::Reads &&
            mnu::schema_action_reads(action, "file") == SchemaApplies::Ignored,
        "WINDOW reads STATE, not FILE");
  action.type = "FORM_POST";
  CHECK(mnu::schema_action_reads(action, "target_form") == SchemaApplies::Unverified, "TARGET_FORM's use is open");
  action.type = "NOPE";
  CHECK(mnu::schema_action_reads(action, "target") == SchemaApplies::Ignored &&
            mnu::schema_action_reads(action, "type") == SchemaApplies::Reads,
        "code 0 is ignored");
  CHECK(mnu::schema_applies_both(SchemaApplies::Reads, SchemaApplies::Unverified) == SchemaApplies::Unverified &&
            mnu::schema_applies_both(SchemaApplies::Unverified, SchemaApplies::Ignored) == SchemaApplies::Ignored,
        "the stricter answer");
  return true;
}


// The material flag tokens, in the table's order [orig: g_UIMaterialFlagNames @ 0x84a5d0, 43 rows].
bool test_material_flags() {
  const auto &flags = mnu::ui_material_flag_choices();
  CHECK(flags.size() == 43, "43 rows");
  CHECK(std::string(flags.front().name) == "STANDARD" && flags.front().value == 0x300600, "STANDARD first");
  CHECK(std::string(flags.back().name) == "TINTASTEXT" && flags.back().value == 0x80000000LL, "TINTASTEXT last");
  return true;
}

// What a field names, by its own row or by its record's siblings (docs/mnu/menu-re.md "Names and the
// lookups"; CUIWidget_HandleScriptedAction @ 0x6497f0 for an ACTION's verb), and the colour word a
// style colour holds.
bool test_references() {
  using mnu::SchemaReference;
  using S = mnu::SchemaShape;
  CHECK(mnu::schema_field_reference(S::Window, "font.name") == SchemaReference::Font &&
            mnu::schema_field_reference(S::Part, "cursor.file") == SchemaReference::MenuTexture &&
            mnu::schema_field_reference(S::Window, "text_rsrc") == SchemaReference::TextTable,
        "a window's fixed references, a part's its window's");
  CHECK(mnu::schema_field_reference(S::Sound, "file") == SchemaReference::Sound &&
            mnu::schema_field_reference(S::Datasource, "value") == SchemaReference::Credits &&
            mnu::schema_field_reference(S::Action, "file") == SchemaReference::Menu,
        "a SOUND's bank, a DATASOURCE's credits, an ACTION's FILE");
  CHECK(mnu::schema_field_reference(S::Appearance, "value") == SchemaReference::None &&
            mnu::schema_reference_varies(S::Appearance, "value") && !mnu::schema_reference_varies(S::Window, "font.name"),
        "an APPEARANCE's value varies, a FONT's name does not");
  mnu::Appearance appearance;
  appearance.type = "imagerow";
  CHECK(mnu::schema_reference(S::Appearance, "value", &appearance) == SchemaReference::MenuTexture, "IMAGEROW");
  appearance.type = "OUTLINE";
  CHECK(mnu::schema_reference(S::Appearance, "value", &appearance) == SchemaReference::StyleVar, "OUTLINE");
  appearance.type = "CUSTOM";
  CHECK(mnu::schema_reference(S::Appearance, "value", &appearance) == SchemaReference::None, "CUSTOM");
  mnu::Item item;
  item.type = "id";
  CHECK(mnu::schema_reference(S::Item, "text", &item) == SchemaReference::TextId, "ITEM ID");
  item.type = "BITMAP";
  CHECK(mnu::schema_reference(S::Item, "text", &item) == SchemaReference::MenuTexture, "ITEM BITMAP");
  item.type = "COLOR";
  CHECK(mnu::schema_reference(S::Item, "text", &item) == SchemaReference::StyleVar, "ITEM COLOR");
  mnu::TableSubst subst;
  subst.is_file = true;
  CHECK(mnu::schema_reference(S::Subst, "file", &subst) == SchemaReference::MenuTexture, "SUBST FILE");
  subst.is_url = true;
  CHECK(mnu::schema_reference(S::Subst, "file", &subst) == SchemaReference::None, "a URL is fetched");
  mnu::Action action;
  action.type = "screen";
  CHECK(mnu::schema_reference(S::Action, "target", &action) == SchemaReference::Screen, "SCREEN");
  action.type = "GLB_FILTER_NUM";
  CHECK(mnu::schema_reference(S::Action, "target", &action) == SchemaReference::Window, "GLB_FILTER_NUM");
  action.type = "URL";
  CHECK(mnu::schema_reference(S::Action, "target", &action) == SchemaReference::None &&
            mnu::schema_reference(S::Action, "field", &action) == SchemaReference::Window,
        "URL's slot");
  mnu::Window window;
  window.string_data.type = "Id";
  CHECK(mnu::schema_reference(S::Window, "string.value", &window) == SchemaReference::TextId &&
            mnu::schema_reference(S::Part, "toggle_string.value", &window) == SchemaReference::None,
        "STRING ID");
  CHECK(mnu::schema_hex_colour(SchemaReference::StyleVar) && !mnu::schema_hex_colour(SchemaReference::MenuTexture),
        "a style colour holds the hex word");
  return true;
}

// What a new record is, a window's TYPE token, and whose TEXT_RSRC a part's string ids fall back to.
bool test_defaults() {
  mnu::Window window;
  mnu::schema_default(window);
  CHECK(window.type == mnu::WindowType::Static && window.position.has_left && window.position.right == 100 &&
            window.position.bottom == 20 && window.appearances.size() == 1 && window.appearances[0].state == "default" &&
            window.appearances[0].type.empty(),
        "a window: STATIC, 0,0,100,20, a typeless DEFAULT appearance");
  mnu::Action action;
  mnu::schema_default(action);
  CHECK(action.type == "POP_SCREEN", "an ACTION: POP_SCREEN, the verb with no operand");
  mnu::Sound sound;
  mnu::schema_default(sound);
  CHECK(sound.state == "mousein" && sound.trigger == "MOUSE_OVER", "a SOUND on MOUSEIN");
  mnu::TableHeader header;
  mnu::schema_default(header, 3);
  CHECK(header.has_column && header.column == 3, "a HEADER with its COLUMN");
  mnu::WindowPart part;
  const mnu::Window &authored = mnu::schema_default_part(part, mnu::WindowType::List);
  CHECK(authored.appearances.size() == 1 && authored.appearances[0].state == "default", "a part's DEFAULT appearance");
  mnu::Window typed;
  mnu::schema_set_type_token(typed, "Button");
  CHECK(typed.type == mnu::WindowType::Button && mnu::schema_type_token(typed) == "Button", "the token as typed");
  mnu::schema_set_type_token(typed, "NOT_A_TYPE");
  CHECK(typed.type == mnu::WindowType::Window && mnu::schema_type_token(typed) == "NOT_A_TYPE",
        "a token the factory does not match builds a generic window");
  CHECK(mnu::schema_part_reads_root_text("list_box") && !mnu::schema_part_reads_root_text("spinup") &&
            !mnu::schema_part_reads_root_text("scrollbar"),
        "a combo's LIST_BOX falls back to the root's TEXT_RSRC, a spin arrow and a scrollbar do not");
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
  RUN_TEST(test_applicability);
  RUN_TEST(test_material_flags);
  RUN_TEST(test_references);
  RUN_TEST(test_defaults);
  if (failed > 0) {
    std::cerr << "\n" << failed << " test(s) FAILED\n";
    return EXIT_FAILURE;
  }
  std::cout << "\nAll tests passed!\n";
  return EXIT_SUCCESS;
}
