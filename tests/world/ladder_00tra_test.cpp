// D-COL-5 ladder climb on the retail data: 00TRa's ladder-climbing tutorial
// section drives the climb motor end to end on the engine's own collision
// world.
//
//   * sweep the collision debug view around the spawn for CL/type-4 volumes
//     (the ladder is authored purely as a .3di BVOL — no items.def attribute)
//     and rank them by lean: a true vertical rung ladder, not a gangway;
//   * from ABOVE: hover just under the top looking down — the from-above entry
//     arm has no facing requirement (heightDiff < 0 short-circuits the 60 deg
//     gate; the pitch sign must agree) — the latch is the witnessed gravity
//     skip (an idle latched body holds Z exactly) AND the climb clip family;
//   * HOLD FORWARD after a look up: the climb block stamps climb_up and the
//     clip's vertical root lane rises the body [orig: @0x4b7484-0x4b750a];
//   * release: climb_idle holds height (gravity skipped while latched
//     [orig: @0x4b7acd]); a natural top-out hands over to the exit leg;
//   * from BELOW: enter at the base like a player walks up to a ladder (facing
//     within 60 deg + pitch UP [orig: @0x4b32a5-0x4b32c0]) and climb the whole
//     span without a drop (the "pushed off halfway up" report).
//   The dismount family is pinned by the `collision`/`infantry` ctests.
// Gated on OPENNOVA_JO_DIR (a retail JO install carrying 00TRa.bms).
#include "common/retail_mission_files.h"
#include "common/retail_paths.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace {

using namespace opennova;
namespace w = opennova::world;

int failures = 0;
bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	++failures;
	return false;
}

constexpr float kSweepStep = 100.0f;
constexpr float kSweepRadius = 400.0f;
constexpr float kPi = 3.14159265358979323846f;

// One CL volume in mission space: the corner centroid, base/top, the lean
// (horizontal distance between the top-face and bottom-face corner centroids
// — ~0 for a true vertical rung ladder, large for a climbable staircase).
struct Ladder {
	w::Vec3 center;
	float base = 0.0f;
	float top = 0.0f;
	float lean = 0.0f;
	w::Vec3 top_xy; // the top-face column
	uint16_t owner = 0xFFFF;
};

std::vector<Ladder> cl_volumes(testrig::RetailMissionRig &rig) {
	std::vector<Ladder> found;
	for (const w::CollisionWorld::DebugInstance &inst :
			rig.collision_instances(rig.player_position(), 150.0f, 128)) {
		for (const w::CollisionWorld::DebugVolume &vd : inst.volumes) {
			if (vd.type != w::bvol_type::kLadderCL) continue;
			Ladder l;
			float base = 1e30f, top = -1e30f;
			w::Vec3 c{};
			for (int k = 0; k < 8; ++k) {
				const float x = vd.corners[k][0] / 65536.0f, y = vd.corners[k][1] / 65536.0f,
							z = vd.corners[k][2] / 65536.0f;
				c.x += x; c.y += y; c.z += z;
				base = std::min(base, z);
				top = std::max(top, z);
			}
			c.x /= 8.0f; c.y /= 8.0f; c.z /= 8.0f;
			const float mid = (base + top) * 0.5f;
			w::Vec3 lo{}, hi{};
			int nlo = 0, nhi = 0;
			for (int k = 0; k < 8; ++k) {
				const float x = vd.corners[k][0] / 65536.0f, y = vd.corners[k][1] / 65536.0f,
							z = vd.corners[k][2] / 65536.0f;
				if (z <= mid) { lo.x += x; lo.y += y; ++nlo; } else { hi.x += x; hi.y += y; ++nhi; }
			}
			l.center = c;
			l.base = base;
			l.top = top;
			l.lean = 1e30f;
			l.top_xy = w::Vec3{c.x, c.y, 0.0f};
			if (nlo > 0 && nhi > 0) {
				lo.x /= nlo; lo.y /= nlo; hi.x /= nhi; hi.y /= nhi;
				l.lean = std::sqrt((hi.x - lo.x) * (hi.x - lo.x) + (hi.y - lo.y) * (hi.y - lo.y));
				l.top_xy = w::Vec3{hi.x, hi.y, 0.0f};
			}
			l.owner = inst.handle.packed;
			found.push_back(l);
		}
	}
	return found;
}

bool climbing(testrig::RetailMissionRig &rig) {
	return rig.player_anim_key().find("climb") != std::string::npos;
}

void look_up(testrig::RetailMissionRig &rig) {
	for (int i = 0; i < 8; ++i) {
		rig.look(0.0f, -600.0f);
		rig.tick();
	}
}

} // namespace

int main() {
	RETAIL_REQUIRE_OR_SKIP(install, retail::install(),
			"OPENNOVA_JO_DIR (a retail JO install carrying 00TRa.bms)");
	testrig::RetailMissionRig rig;
	std::string error;
	if (!rig.open(install, "00TRa.bms", error)) return retail::skip(error.c_str());
	testrig::BootOptions options;
	if (!expect(rig.boot(options, error), "00TRa boots")) {
		std::fprintf(stderr, "  %s\n", error.c_str());
		return 1;
	}
	if (!expect(rig.has_local_player(), "the host's own player spawned")) return 1;
	if (!expect(rig.collision_attached > 0, "the collision instances attached")) return 1;
	rig.tick(62);

	// --- Sweep for CL volumes: the debug view is player-anchored (150 u), so
	// hop a teleport grid around the spawn and accumulate every CL in range.
	const w::Vec3 spawn = rig.player_position();
	std::map<std::string, Ladder> by_key;
	std::vector<w::Vec3> points{w::Vec3{spawn.x, spawn.y, 0.0f}};
	for (float r = kSweepStep; r <= kSweepRadius; r += kSweepStep) {
		const int n = static_cast<int>(std::ceil(2.0f * kPi * r / kSweepStep));
		for (int i = 0; i < n; ++i) {
			const float a = 2.0f * kPi * float(i) / float(n);
			points.push_back(w::Vec3{spawn.x + r * std::cos(a), spawn.y + r * std::sin(a), 0.0f});
		}
	}
	for (const w::Vec3 &p : points) {
		rig.teleport_local_player(w::Vec3{p.x, p.y, 60.0f}, 0.0, 0.0);
		rig.tick(testrig::ticks_for_seconds(0.12));
		for (const Ladder &l : cl_volumes(rig)) {
			char key[64];
			std::snprintf(key, sizeof(key), "%d_%d_%d", int(std::lround(l.center.x * 4.0f)),
					int(std::lround(l.center.y * 4.0f)), int(std::lround(l.center.z * 4.0f)));
			by_key.emplace(key, l);
		}
	}
	std::vector<Ladder> ladders;
	for (const auto &kv : by_key) ladders.push_back(kv.second);
	if (!expect(!ladders.empty(), "a CL/type-4 volume lies within 400 u of the spawn")) return 1;
	std::sort(ladders.begin(), ladders.end(), [](const Ladder &a, const Ladder &b) {
		if (std::fabs(a.lean - b.lean) > 0.25f) return a.lean < b.lean;
		return (a.top - a.base) > (b.top - b.base);
	});
	for (const Ladder &l : ladders)
		std::printf("ladder: CL candidate center (%.2f, %.2f, %.2f) h %.1f lean %.2f owner=%u\n",
				l.center.x, l.center.y, l.center.z, l.top - l.base, l.lean, unsigned(l.owner));
	const Ladder ladder = ladders.front();
	const float base = ladder.base, top = ladder.top;
	const w::Vec3 face = ladder.top_xy;
	std::printf("ladder: chosen (%.2f, %.2f, %.2f) base %.1f top %.1f lean %.2f, %zu in field\n",
			ladder.center.x, ladder.center.y, ladder.center.z, base, top, ladder.lean, ladders.size());

	// --- From above: drop in just under the top, looking down. Hover points: the
	// prism column, then a ring of 0.45 u offsets (a thin rung-ladder prism
	// embeds against its wall; the open-air side stays in front of the rungs).
	std::vector<w::Vec3> hovers{w::Vec3{face.x, face.y, 0.0f}};
	for (int k = 0; k < 8; ++k) {
		const float a = 2.0f * kPi * float(k) / 8.0f;
		hovers.push_back(w::Vec3{face.x + 0.45f * std::cos(a), face.y + 0.45f * std::sin(a), 0.0f});
	}
	// The entry probe teleports the body to just under the top and lets it drop
	// past the 8.5 u column on every no-latch hover; with retail's fall-damage
	// tolerance (fallmps 13) the third such drop kills it and the probe reads a
	// death anim. That is a rig artifact, not the entry gate: disable fall damage
	// through the WAC-writable named value for the probe.
	rig.world.wac_values.fallmps = 0;
	bool latched = false;
	float hold_z = 0.0f;
	for (const w::Vec3 &h : hovers) {
		rig.teleport_local_player(w::Vec3{h.x, h.y, top - 0.3f}, 0.0, -30.0);
		rig.tick(testrig::ticks_for_seconds(0.8));
		const float z0 = rig.player_position().z;
		rig.tick(testrig::ticks_for_seconds(0.5));
		const float z1 = rig.player_position().z;
		const std::string key = rig.player_anim_key();
		if (std::fabs(z1 - z0) < 0.05f && z1 > base && key.find("climb") != std::string::npos) {
			latched = true;
			hold_z = z1;
			const w::Vec3 lp = rig.player_position();
			std::printf("ladder: LATCHED hover (%.2f, %.2f) holds z %.2f as %s (volume %.1f..%.1f); body %.2fu off the column\n",
					h.x, h.y, z1, key.c_str(), base, top, testrig::planar_distance(lp, face));
			break;
		}
		std::printf("ladder: hover (%.2f, %.2f) no latch (z %.2f -> %.2f, anim %s)\n", h.x, h.y, z0, z1, key.c_str());
	}
	if (!expect(latched, "an entry latched — the CL entry gate + the gravity skip")) return 1;

	// --- Climb: look up (the forward fan picks climb_up by the look-pitch sign),
	// hold forward, and the climb clip's vertical lane must rise the body.
	look_up(rig);
	rig.input.forward = true;
	float max_z = hold_z;
	for (int i = 0; i < 30; ++i) {
		rig.tick(testrig::ticks_for_seconds(0.1));
		max_z = std::max(max_z, rig.player_position().z);
	}
	rig.input.forward = false;
	if (!expect(max_z > hold_z + 0.4f, "forward + look-up climbs the body (climb_up root motion)")) {
		rig.input.forward = true;
		for (int i = 0; i < 6; ++i) {
			rig.tick(testrig::ticks_for_seconds(0.2));
			const w::Vec3 lp = rig.player_position();
			std::printf("ladder: STALL anim=%s pos (%.2f, %.2f, %.2f)\n", rig.player_anim_key().c_str(), lp.x, lp.y, lp.z);
		}
		rig.input.forward = false;
	} else {
		std::printf("ladder: CLIMB OK %.2f -> %.2f\n", hold_z, max_z);
	}

	// --- Hold: release the stick — climb_idle keeps the height while still
	// inside the span (a natural top-out above the volume falls instead).
	rig.tick(testrig::ticks_for_seconds(0.5));
	const float settle = rig.player_position().z;
	if (settle < top - 0.5f) {
		rig.tick(testrig::ticks_for_seconds(0.6));
		const float settle2 = rig.player_position().z;
		expect(std::fabs(settle2 - settle) <= 0.3f, "height is held after the climb (climb_idle, no gravity)");
		std::printf("ladder: HOLD z %.2f -> %.2f\n", settle, settle2);
	} else {
		std::printf("ladder: topped out above the volume (z %.2f) — the exit leg took over\n", settle);
	}

	// --- From below: enter at the base like a player walks up to a ladder and
	// climb the whole span; a drop before the top is the reported defect.
	bool below_latched = false;
	for (int k = 0; k < 8 && !below_latched; ++k) {
		const float a = 2.0f * kPi * float(k) / 8.0f;
		const float hx = face.x + 0.45f * std::cos(a), hy = face.y + 0.45f * std::sin(a);
		for (int yaw = 0; yaw < 360 && !below_latched; yaw += 45) {
			rig.teleport_local_player(w::Vec3{hx, hy, base + 0.6f}, double(yaw), 30.0);
			rig.tick(testrig::ticks_for_seconds(0.6));
			if (climbing(rig)) {
				below_latched = true;
				const w::Vec3 lp = rig.player_position();
				std::printf("ladder: BOTTOM LATCH offset k=%d yaw %d -> %s at (%.2f, %.2f, %.2f)\n", k, yaw,
						rig.player_anim_key().c_str(), lp.x, lp.y, lp.z);
			}
		}
	}
	if (expect(below_latched, "the from-below entry latches (facing within 60 deg + pitch up)")) {
		look_up(rig);
		rig.input.forward = true;
		float z_max = rig.player_position().z;
		bool dropped = false;
		for (int i = 0; i < 60; ++i) {
			rig.tick(testrig::ticks_for_seconds(0.12));
			const w::Vec3 lp = rig.player_position();
			const std::string key = rig.player_anim_key();
			z_max = std::max(z_max, lp.z);
			// Cresting hands over to the exit leg (the clip family leaves
			// climb); that is the top-out, not a drop.
			if (lp.z >= top - 0.2f) break;
			if (key.find("climb") == std::string::npos || lp.z < z_max - 0.4f) {
				dropped = true;
				std::printf("ladder: DROP at z %.2f (reached %.2f of top %.1f) anim %s drift %.2f\n", lp.z, z_max,
						top, key.c_str(), testrig::planar_distance(lp, face));
				break;
			}
		}
		rig.input.forward = false;
		expect(!dropped, "the full span climbs from the base without a drop");
		if (!dropped) std::printf("ladder: BOTTOM full span climbed %.2f -> %.2f\n", base + 0.6f, z_max);
	}

	if (failures == 0) std::printf("ladder_00tra: CL entry + gravity-off hold + climb_up + the full span on 00TRa\n");
	return failures == 0 ? 0 : 1;
}
