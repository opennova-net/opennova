// Byte-exact serializer guarantee: write is a faithful function of File state,
// so write -> parse -> write reproduces the exact same bytes. This underpins the
// editor's promise that an unedited game .bin saves back byte-for-byte.
#include "rtxt_test_util.h"

using namespace opennova::rtxt;
using rtxt_test::expect;

int main() {
  File source = rtxt_test::make_sample();

  std::vector<uint8_t> first;
  std::string error;
  if (!expect(write(source, first, error), "first write should succeed")) return 1;

  File reparsed;
  if (!expect(parse(first.data(), first.size(), reparsed, error), error.c_str())) return 1;

  std::vector<uint8_t> second;
  if (!expect(write(reparsed, second, error), "second write should succeed")) return 1;

  if (!expect(first.size() == second.size(), "re-serialized byte length should match")) return 1;
  for (size_t i = 0; i < first.size(); ++i) {
    if (first[i] != second[i]) {
      std::fprintf(stderr, "FAIL: byte %zu differs (0x%02X != 0x%02X)\n", i, first[i], second[i]);
      return 1;
    }
  }

  // Header sanity: magic and entry count land where the format dictates.
  if (!expect(first[0] == 'R' && first[1] == 'T' && first[2] == 'X' && first[3] == 'T',
              "magic should be ASCII 'RTXT'")) return 1;
  if (!expect(first[12] == 4 && first[13] == 0 && first[14] == 0 && first[15] == 0,
              "entry_count field should encode 4 entries")) return 1;

  std::printf("OK: rtxt write is byte-stable across a parse round-trip\n");
  return 0;
}
