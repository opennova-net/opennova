// Grills engine/formats/rtxt over the minted synthetic string tables
// (fixtures/rtxt, tests/fixtures/minimal_rtxt_gen.cpp): a multi-section game
// table with position hints, a menu table, a mission sidecar with cp1252 text
// and odd-length padding, and the one-entry case. The same two checks over the
// shipped retail bins (gametext, menutxt, the mission sidecars, gameerr,
// vmacros, keyhelp) are the gated install sweep (rtxt_jo_install_sweep).
//
// Each fixture must (a) obey the raw format invariants and (b) survive
// parse -> write byte-for-byte.
#include "rtxt_real_util.h"

int main() {
  const char *names[] = {
      "synth_game.bin", "synth_menu.bin", "synth_mission.bin", "synth_tiny.bin",
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
    std::fprintf(stderr, "FAIL: %d synthetic-fixture check(s) failed\n", failures);
    return 1;
  }
  std::printf("OK: 4 synthetic string tables byte-stable through parse/write\n");
  return 0;
}
