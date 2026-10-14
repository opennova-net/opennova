// Generator + guard for fixtures/dfx2_wire/s2c_0a_coop_objective.bin and
// fixtures/dfx2_wire/c2s_00_join_lan.bin: two Delta Force: Xtreme 2 in-match bodies written by our
// own encoders from the values a retail DFX2 LAN pair put on the wire (the 2026-10-14 onHook
// capture of a co-op round on CP_01_CP and its joiner's JOIN, decoded; no capture is committed).
// tests/novaworld/nw_dfx2_frame_test.cpp pins them against those decoded values.
//
//   s2c_0a_coop_objective.bin  an S2C 0x0A of a DFX2 co-op host, phase 3: the four objective
//                              values, then the co-op checkpoint 0xFFFF (DFX2-1), the recipient's
//                              tail and its own player's compact record
//   c2s_00_join_lan.bin        a DFX2 LAN joiner's C2S 0x00 JOIN: its CDKIID (the wire key of the
//                              serial the capture's joiner presented, under the DFX2 wire chain),
//                              then VERSIONCRCSTRING "0" (DFX2-13)
//
// [orig: NetPacket_WritePlayerState @0x4fcd84..0x4fcd9b (dfx2.exe); NapiNP_WriteClientAuthPayload
//  @0x42c25b..0x42c344 (dfx2.exe)]
//
// Default: rebuild in memory and byte-compare the committed files. `--write` (re)writes them.
#include "common/test_paths.h"
#include "common/file_io.h"

#include "fixtures/dfx2_wire_values.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

bool expect(bool cond, const char *msg) {
	if (!cond) std::fprintf(stderr, "FAIL: %s\n", msg);
	return cond;
}

bool check_or_write(const std::string &path, const std::vector<uint8_t> &bytes, bool write_mode) {
	if (write_mode) {
		std::ofstream o(path, std::ios::binary);
		o.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		std::printf("wrote %s (%zu bytes)\n", path.c_str(), bytes.size());
		return true;
	}
	std::vector<uint8_t> committed;
	if (!expect(test_io::read_file(path, committed), ("committed file missing; run with --write: " + path).c_str()))
		return false;
	return expect(committed == bytes,
	              ("differs from the generator output; regenerate with --write: " + path).c_str());
}

} // namespace

int main(int argc, char **argv) {
	bool write_mode = false;
	for (int i = 1; i < argc; ++i)
		if (std::strcmp(argv[i], "--write") == 0) write_mode = true;
	const std::string root = std::string(test_paths_repo_root(__FILE__)) + "/fixtures/dfx2_wire/";
	if (!check_or_write(root + "s2c_0a_coop_objective.bin", dfx2_wire::coop_objective_frame(), write_mode))
		return 1;
	if (!check_or_write(root + "c2s_00_join_lan.bin", dfx2_wire::lan_join_body(), write_mode)) return 1;
	if (!write_mode) std::printf("OK: fixtures/dfx2_wire/*.bin byte-reproducible\n");
	return 0;
}
