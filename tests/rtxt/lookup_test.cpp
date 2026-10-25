// Case-insensitive key lookup via File::get / File::has.
#include "rtxt_test_util.h"

using namespace opennova::rtxt;
using rtxt_test::expect;

int main() {
  File f = rtxt_test::make_sample();

  if (!expect(f.has("BTN_QUIT"), "exact-case key should be found")) return 1;
  if (!expect(f.has("btn_quit"), "lower-case key should match case-insensitively")) return 1;
  if (!expect(f.has("Btn_Quit"), "mixed-case key should match case-insensitively")) return 1;
  if (!expect(!f.has("MISSING_KEY"), "absent key should not be found")) return 1;

  if (!expect(f.get("BTN_NEW_GAME") == "{hot}New Game", "get should return raw text incl. marker")) return 1;
  if (!expect(f.get("hud_ammo") == "", "empty-text entry should return empty string")) return 1;
  if (!expect(f.get("NOPE") == "", "absent key should return empty string")) return 1;

  // Section grouping.
  std::vector<const Entry *> main = f.get_section_entries(0);
  if (!expect(main.size() == 2, "section 0 should contain two entries")) return 1;
  if (!expect(f.get_section_entries(2).size() == 1, "section 2 should contain one entry")) return 1;

  // Lookup also works before build_lookup() via the linear fallback.
  File fresh;
  fresh.entries = f.entries;
  if (!expect(fresh.get("btn_quit") == "Quit", "linear fallback should find the key before build_lookup")) return 1;

  // The section lookup searches the override table first, unless it is the
  // table asked (D-NET-675) [orig: TextResource_FindEntryBySectionAndKey
  // @0x75D250, the override test @0x75D27B].
  File over;
  over.sections = {{"menu_main", 1}};
  over.entries = {{"BTN_QUIT", "Leave", {0, 0}, 0}};
  over.build_lookup();
  const Entry *hit = find_with_override(&over, f, "menu_main", "btn_quit");
  if (!expect(hit != nullptr && hit->text == "Leave", "the override table answers first")) return 1;
  hit = find_with_override(&over, f, "menu_main", "BTN_NEW_GAME");
  if (!expect(hit != nullptr && hit->text == "{hot}New Game", "a key the override lacks reads the table")) return 1;
  hit = find_with_override(nullptr, f, "menu_main", "BTN_QUIT");
  if (!expect(hit != nullptr && hit->text == "Quit", "no override table reads the table")) return 1;
  if (!expect(find_with_override(&over, f, "hud", "MISSING") == nullptr, "a miss in both is none")) return 1;
  hit = find_with_override(&over, over, "menu_main", "BTN_QUIT");
  if (!expect(hit != nullptr && hit->text == "Leave", "the override asked for itself is searched once")) return 1;

  std::printf("OK: rtxt lookup is case-insensitive with a linear fallback\n");
  return 0;
}
