// Structural round-trip: a File survives write -> parse with all fields intact.
#include "rtxt_test_util.h"

using namespace opennova::rtxt;
using rtxt_test::expect;

int main() {
  File source = rtxt_test::make_sample();

  std::vector<uint8_t> bytes;
  std::string error;
  if (!expect(write(source, bytes, error), "write should succeed")) return 1;

  File loaded;
  if (!expect(parse(bytes.data(), bytes.size(), loaded, error), error.c_str())) return 1;

  if (!expect(loaded.entries.size() == source.entries.size(), "entry count should round-trip")) return 1;
  if (!expect(loaded.sections.size() == source.sections.size(), "section count should round-trip")) return 1;

  for (size_t i = 0; i < source.entries.size(); ++i) {
    const Entry &a = source.entries[i];
    const Entry &b = loaded.entries[i];
    if (!expect(a.key == b.key, "key should round-trip")) return 1;
    if (!expect(a.text == b.text, "text should round-trip")) return 1;
    if (!expect(a.position.x == b.position.x, "position x should round-trip")) return 1;
    if (!expect(a.position.y == b.position.y, "position y should round-trip")) return 1;
    if (!expect(a.section_index == b.section_index, "section_index should round-trip")) return 1;
  }
  for (size_t i = 0; i < source.sections.size(); ++i) {
    if (!expect(loaded.sections[i].name == source.sections[i].name, "section name should round-trip")) return 1;
    if (!expect(loaded.sections[i].string_count == source.sections[i].string_count,
                "section string_count should round-trip")) return 1;
  }

  // Negative positions must survive the int16 pack/unpack.
  if (!expect(loaded.entries[3].position.x == -32 && loaded.entries[3].position.y == -16,
              "negative position hints should round-trip through the packed field")) return 1;

  // A .rtxt is untrusted input. TextDataEndPosition (header offset 4) was only
  // bounds-checked upward, so an end offset pointing BACK into the entry table
  // was accepted and the section count was then read out of the entry table's
  // own bytes — structure parsed out of unrelated data instead of a clean
  // failure. One entry, end offset aimed at the table: must be rejected.
  {
    File one;
    one.sections = {{"menu_main", 1}};
    one.entries = {{"BTN_ONLY", "Only", {1, 2}, 0}};
    one.build_lookup();

    std::vector<uint8_t> raw;
    if (!expect(write(one, raw, error), "one-entry write should succeed")) return 1;

    File probe;
    if (!expect(parse(raw.data(), raw.size(), probe, error),
                "the unmodified one-entry file should parse")) return 1;

    // Point TextDataEndPosition at the first byte of the entry table.
    constexpr size_t kHeaderSize = 16;
    raw[4] = static_cast<uint8_t>(kHeaderSize);
    raw[5] = 0;
    raw[6] = 0;
    raw[7] = 0;

    File rejected;
    error.clear();
    if (!expect(!parse(raw.data(), raw.size(), rejected, error),
                "a backward TextDataEndPosition must be rejected")) return 1;
    if (!expect(!error.empty(), "the backward range must report a reason")) return 1;
    std::printf("  rejected backward text range: %s\n", error.c_str());
  }

  std::printf("OK: rtxt structural round-trip preserved entries, sections, and positions\n");
  return 0;
}
