// Edge cases: an empty table (no entries, no sections) and header validation.
#include "rtxt_test_util.h"

using namespace opennova::rtxt;
using rtxt_test::expect;

int main() {
  File empty;
  std::vector<uint8_t> bytes;
  std::string error;
  if (!expect(write(empty, bytes, error), "writing an empty table should succeed")) return 1;
  if (!expect(bytes.size() >= 16, "even an empty table has the 16-byte header + section count")) return 1;

  File loaded;
  if (!expect(parse(bytes.data(), bytes.size(), loaded, error), error.c_str())) return 1;
  if (!expect(loaded.entries.empty(), "empty table should parse with no entries")) return 1;
  if (!expect(loaded.sections.empty(), "empty table should parse with no sections")) return 1;

  // Too-small buffer is rejected.
  uint8_t tiny[4] = {'R', 'T', 'X', 'T'};
  File bad;
  if (!expect(!parse(tiny, sizeof(tiny), bad, error), "a buffer smaller than the header should fail")) return 1;

  // The magic is never read, as the engine's loaders never read it (D-RTXT-4)
  // [orig: TextResource_LoadFile @ 0x75D0F0 -> TextResource_FixupPointers
  // @ 0x75D050]: a table under another first dword reads as the same table.
  File one;
  one.sections = {{"menu", 1}};
  one.entries = {{"BTN_OK", "OK", {0, 0}, 0}};
  std::vector<uint8_t> one_bytes;
  if (!expect(write(one, one_bytes, error), "writing a one-entry table should succeed")) return 1;
  std::vector<uint8_t> wrong_magic = one_bytes;
  wrong_magic[0] = 'X';
  wrong_magic[3] = 0;
  File read_back;
  if (!expect(parse(wrong_magic.data(), wrong_magic.size(), read_back, error), error.c_str()))
    return 1;
  read_back.build_lookup();
  const Entry *ok = read_back.find_in_section("menu", "BTN_OK");
  if (!expect(read_back.entries.size() == 1 && ok != nullptr && ok->text == "OK",
              "a table under another magic reads as the same table")) return 1;
  std::vector<uint8_t> rewritten;
  if (!expect(write(read_back, rewritten, error) && rewritten == one_bytes,
              "written back, it carries the RTXT magic again")) return 1;

  std::printf("OK: rtxt handles empty tables, rejects a short header, never reads the magic\n");
  return 0;
}
