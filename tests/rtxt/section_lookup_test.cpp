// Section-scoped lookup parity with the engine
// [orig: TextResource_FindEntryBySectionAndKey @ 0x75D250 / TextResource_FindKeyInSection @ 0x75D1E0].
// Sections and keys match case-insensitively; the same key may live in several
// sections and resolve per-section; within a section the FIRST match wins; flat
// lookup walks the whole key blob in entry order, also first-wins
// [orig: TextResource_FindEntryByKey @ 0x75D450].
#include "rtxt_test_util.h"

using namespace opennova::rtxt;
using rtxt_test::expect;

int main() {
  File file;
  file.sections.push_back({"menu_main", 0});
  file.sections.push_back({"hud", 0});

  // Same key in both sections with different text (legitimate retail pattern), plus
  // an in-section duplicate whose later copy must be unreachable (first match wins).
  file.entries.push_back({"BTN_OK", "Main OK", {0, 0}, 0});
  file.entries.push_back({"SHARED", "main text", {0, 0}, 0});
  file.entries.push_back({"SHARED", "main text duplicate", {0, 0}, 0});
  file.entries.push_back({"shared", "hud text", {0, 0}, 1});
  file.entries.push_back({"HUD_AMMO", "Ammo", {0, 0}, 1});
  file.sections[0].string_count = 3;
  file.sections[1].string_count = 2;

  // Section-scoped hits, case-insensitive on both names.
  if (!expect(file.get_in_section("menu_main", "SHARED") == "main text",
              "section 0 lookup should return the first match")) return 1;
  if (!expect(file.get_in_section("HUD", "shared") == "hud text",
              "section 1 lookup should be scoped and case-insensitive")) return 1;
  if (!expect(file.get_in_section("hud", "BTN_OK").empty(),
              "keys outside the section should miss")) return 1;
  if (!expect(file.get_in_section("nosuch", "BTN_OK").empty(),
              "unknown section should miss")) return 1;

  const Entry *found = file.find_in_section("Menu_Main", "btn_ok");
  if (!expect(found != nullptr && found->text == "Main OK",
              "find_in_section should return the entry")) return 1;
  if (!expect(file.find_in_section("menu_main", "HUD_AMMO") == nullptr,
              "find_in_section must not cross section bounds")) return 1;

  // Flat lookup is first-wins in entry order (the map must not let later
  // duplicates shadow earlier ones).
  file.build_lookup();
  if (!expect(file.get("SHARED") == "main text",
              "flat lookup should return the first occurrence")) return 1;

  // Roundtrip: section-scoped behaviour survives write -> parse.
  std::vector<uint8_t> bytes;
  std::string error;
  if (!expect(write(file, bytes, error), error.c_str())) return 1;
  File reparsed;
  if (!expect(parse(bytes.data(), bytes.size(), reparsed, error), error.c_str())) return 1;
  if (!expect(reparsed.get_in_section("hud", "SHARED") == "hud text",
              "section lookup should survive a roundtrip")) return 1;

  // is_grouped/normalize_grouping: moving an entry's section without reordering
  // breaks the invariant the engine depends on; normalize restores it.
  if (!expect(file.is_grouped(), "sample should start grouped")) return 1;
  file.entries[0].section_index = 1;  // "BTN_OK" now claims hud but sits before menu_main rows
  if (!expect(!file.is_grouped(), "interleaved section indices should be detected")) return 1;
  file.normalize_grouping();
  if (!expect(file.is_grouped(), "normalize_grouping should restore grouping")) return 1;
  if (!expect(file.sections[0].string_count == 2 && file.sections[1].string_count == 3,
              "normalize_grouping should recompute section counts")) return 1;
  if (!expect(file.get_in_section("hud", "BTN_OK") == "Main OK",
              "moved entry should resolve in its new section")) return 1;

  std::printf("OK: section-scoped lookup matches engine semantics\n");
  return 0;
}
