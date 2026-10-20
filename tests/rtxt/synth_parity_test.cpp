// Grills engine/formats/rtxt over the minted synthetic string tables
// (fixtures/rtxt, tests/fixtures/minimal_rtxt_gen.cpp): a multi-section game
// table with position hints, a menu table, a mission sidecar with cp1252 text
// and odd-length padding, the one-entry case, and the shapes the shipped bins
// take beyond those (synth_variants.bin: sections of no string first, between
// and last, CR LF and tab texts, a key twice in one section, an 11853-byte
// text, repeated and empty texts), which the retired rtxt_jo_install_sweep
// held every RTXT bin of the JO:CA install to.
//
// Each fixture must (a) obey the raw format invariants and (b) survive
// parse -> write byte-for-byte.
#include "rtxt_real_util.h"

#include "../common/file_io.h"

int main() {
  const char *names[] = {
      "synth_game.bin", "synth_menu.bin", "synth_mission.bin", "synth_tiny.bin",
      "synth_variants.bin", "synth_variants_aligned.bin",
  };

  int failures = 0;
  for (const char *name : names) {
    std::string path = std::string(RTXT_FIXTURE_DIR) + "/" + name;
    std::vector<uint8_t> bytes;
    if (!test_io::read_file(path, bytes)) {
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
  std::printf("OK: %zu synthetic string tables byte-stable through parse/write\n",
              sizeof(names) / sizeof(names[0]));
  return 0;
}
