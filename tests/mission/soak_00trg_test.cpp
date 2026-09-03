// S3/S4 native soak (ADR 0028) on the retail data: 00TRg boots with the sim's
// own asset root (the native SimPoseProvider + the native mounted
// resolver are AUTHORITATIVE) plus the seat-spec table, then runs an extended
// live-shaped load: thousands of 62.5 Hz ticks with the mission's AI thinking,
// walking, and command-mounted on the 50cals (bms 37 -> SSN 65, 51 -> 64), a
// spawned local player with an installed weapon, and periodic full-registry
// hitbox sweeps. Every tick resolves the mounted UseGun frames natively and
// every sweep drives the native collision pose path; the run must keep
// producing live hitboxes. The invariants are ENFORCED: hitboxes appear and
// never regress to zero mid-run, the mounted resolver is actually queried (a
// degenerate soak once passed on silent declines), and NEITHER native path
// declines a single production query across the whole run.
// Gated on OPENNOVA_JO_ASSETS (an extracted retail tree carrying 00TRg.bms).
// `--rounds N` (default 300; one round = 62 ticks + one sweep).
#include "common/retail_mission_files.h"
#include "common/retail_paths.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

using namespace opennova;

int failures = 0;
bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	++failures;
	return false;
}

} // namespace

int main(int argc, char **argv) {
	int rounds = 300;
	for (int i = 1; i + 1 < argc; ++i)
		if (std::strcmp(argv[i], "--rounds") == 0) rounds = std::max(1, std::atoi(argv[i + 1]));
	RETAIL_REQUIRE_OR_SKIP(assets, retail::assets(),
			"OPENNOVA_JO_ASSETS (an extracted retail tree carrying 00TRg.bms)");
	testrig::RetailMissionRig rig;
	std::string error;
	if (!rig.open(assets, "00TRg.bms", error)) return retail::skip(error.c_str());
	testrig::BootOptions options;
	if (!expect(rig.boot(options, error), "00TRg boots")) {
		std::fprintf(stderr, "  %s\n", error.c_str());
		return 1;
	}
	if (!expect(rig.has_local_player(), "the host's own player spawned")) return 1;
	if (!expect(rig.install_weapon("WPN_M4AUTO"), "WPN_M4AUTO installs")) return 1;
	std::printf("soak: 00TRg up: attached=%d seat specs=%zu rounds=%d (%d ticks)\n", rig.collision_attached,
			rig.seat_specs.size(), rounds, rounds * 62);

	int max_hitboxes = 0;
	int min_hitboxes = 0x7fffffff;
	bool seen = false;
	for (int r = 0; r < rounds; ++r) {
		rig.tick(62);
		const int count = static_cast<int>(rig.hitboxes(rig.player_position(), 80.0f, 96, 24000).size());
		max_hitboxes = std::max(max_hitboxes, count);
		if (seen) {
			// Zero rounds COUNT once live hitboxes exist — a mid-soak regression
			// to zero must fail, not slip past a skipping tracker.
			min_hitboxes = std::min(min_hitboxes, count);
			if (count <= 0) {
				std::fprintf(stderr, "FAIL: hitboxes regressed to zero at round %d/%d\n", r + 1, rounds);
				return 1;
			}
		} else if (count > 0) {
			seen = true;
			min_hitboxes = count;
		}
		if ((r + 1) % 60 == 0 || r == rounds - 1)
			std::printf("soak: round %d/%d hitbox_entities=%d\n", r + 1, rounds, count);
	}
	expect(max_hitboxes > 0, "native collision produced hitboxes across the soak");
	std::printf("soak: native pose stats: collision queries=%d declines=%d mounted queries=%d declines=%d evaluations=%d cache_hits=%d graphic sources=%zu\n",
			rig.collision_queries, rig.collision_declines, rig.mounted_queries, rig.mounted_declines,
			rig.mounted_evaluations, rig.mounted_cache_hits, rig.mounted_graphics.size());
	expect(rig.collision_queries > 0 && rig.collision_declines == 0,
			"native collision never declined a production query");
	expect(rig.mounted_queries > 0, "the soak drove mounted-pose queries (the 50cal gunners resolved)");
	expect(rig.mounted_declines == 0, "the native mounted resolver never declined");
	expect(!rig.mounted_graphics.empty(), "native mounted model sources are installed");
	if (failures == 0)
		std::printf("soak_00trg: native soak, hitbox_entities min=%d max=%d over %d ticks, mounted_queries=%d, 0 declines\n",
				min_hitboxes, max_hitboxes, rounds * 62, rig.mounted_queries);
	return failures == 0 ? 0 : 1;
}
