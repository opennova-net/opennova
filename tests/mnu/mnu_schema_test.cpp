// The mnu format's rules a property table reads (formats/mnu/mnu_schema, ADR 0046 A16 and S9h):
// which window type reads what (grill set A3 and the ITEMS arms; docs/mnu/menu-re.md), what an
// ACTION's verb reads, which extra elements a type reads, the stricter of two answers, and the
// material flag tokens a FLAGS text names. The property table those rules serve (every member the
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
  if (failed > 0) {
    std::cerr << "\n" << failed << " test(s) FAILED\n";
    return EXIT_FAILURE;
  }
  std::cout << "\nAll tests passed!\n";
  return EXIT_SUCCESS;
}
