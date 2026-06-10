// Grills libs/rtxt against committed retail Joint Operations string tables:
// the five global tables Game_InitSubsystems @ 0x4A6CD0 loads (gameerr, gametext,
// vmacros, keyhelp) plus menutxt (menu path) and two mission bins (ash_g3d covers
// the tiny case, 00tra covers cp1252 text and odd-length padding).
//
// Each fixture must (a) obey the raw format invariants and (b) survive
// parse -> write byte-for-byte.
#include "rtxt_real_util.h"

int main() {
  const char *names[] = {
      "gameerr.bin", "gametext.bin", "vmacros.bin", "keyhelp.bin",
      "menutxt.bin", "ash_g3d.bin",  "00tra.bin",
  };

  int failures = 0;
  for (const char *name : names) {
    std::string path = std::string(RTXT_FIXTURE_DIR) + "/" + name;
    std::vector<uint8_t> bytes;
    if (!rtxt_real::load_file(path, bytes)) {
      std::fprintf(stderr, "FAIL: cannot read fixture %s\n", path.c_str());
      ++failures;
      continue;
    }
    if (!rtxt_real::check_format_invariants(name, bytes)) ++failures;
    if (!rtxt_real::check_parity(name, bytes)) ++failures;
  }

  if (failures) {
    std::fprintf(stderr, "FAIL: %d real-fixture check(s) failed\n", failures);
    return 1;
  }
  std::printf("OK: 7 retail string tables byte-stable through parse/write\n");
  return 0;
}
