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

  // Wrong magic is rejected.
  std::vector<uint8_t> wrong_magic = bytes;
  wrong_magic[0] = 'X';
  if (!expect(!parse(wrong_magic.data(), wrong_magic.size(), bad, error),
              "a buffer with the wrong magic should fail")) return 1;

  std::printf("OK: rtxt handles empty tables and rejects malformed headers\n");
  return 0;
}
