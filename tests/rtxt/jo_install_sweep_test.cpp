// Full-install RTXT sweep: mounts a real Joint Operations install through the
// engine-faithful VFS and parity-checks every RTXT-magic .bin it can see
// (98 files in retail JO:CA — the global tables plus all per-mission text bins).
//
// Gated on OPENNOVA_JO_DIR so CI and fixture-only runs skip it:
//   OPENNOVA_JO_DIR="C:\...\Joint Operations Combined Arms" ctest -R rtxt_jo_install
#include <vfs/vfs.h>

#include <cstdlib>

#include "rtxt_real_util.h"

int main() {
  const char *dir = std::getenv("OPENNOVA_JO_DIR");
  if (!dir || !*dir) {
    std::printf("SKIP: set OPENNOVA_JO_DIR to a JO install to run the full RTXT sweep\n");
    return 0;
  }

  opennova::Vfs vfs;
  if (!vfs.mount_game(dir, std::string(), opennova::VfsMountMode::Packed)) {
    std::fprintf(stderr, "FAIL: mount_game(%s): %s\n", dir, vfs.last_error().c_str());
    return 1;
  }

  int checked = 0;
  int failures = 0;
  for (const auto &loc : vfs.list_files()) {
    const std::string &name = loc.logical_name;
    if (name.size() < 4) continue;
    std::string ext = name.substr(name.size() - 4);
    for (auto &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext != ".bin") continue;

    std::vector<uint8_t> bytes;
    if (!vfs.read_file_raw(name, bytes) || bytes.size() < 16) continue;
    if (rtxt_real::read_u32(bytes, 0) != 0x54585452u) continue;  // not RTXT (e.g. SCR0 music bins)

    ++checked;
    if (!rtxt_real::check_format_invariants(name, bytes)) ++failures;
    if (!rtxt_real::check_parity(name, bytes)) ++failures;
  }

  if (failures) {
    std::fprintf(stderr, "FAIL: %d of %d RTXT bins failed the sweep\n", failures, checked);
    return 1;
  }
  std::printf("OK: %d RTXT bins from the install byte-stable through parse/write\n", checked);
  return 0;
}
