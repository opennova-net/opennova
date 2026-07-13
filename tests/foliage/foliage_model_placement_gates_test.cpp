#include <foliage/model_placement.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace opennova;
using namespace opennova::foliage;

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

} // namespace

int main() {
	const uint32_t key = pack_model_tile_key(0x00100000, 0x00200000);
	const Fixed16_16 anchor_x = 0x00180000;
	const Fixed16_16 anchor_z = 0x00280000;

	// --- Gate 1: per-axis Chebyshev range around the anchor ------------------
	{
		ModelPlacementConfig cfg{};
		PlacementSamplers s;
		s.path_blocked = [](Fixed16_16, Fixed16_16, int32_t) { return false; };
		s.slot_mask_at = [](Fixed16_16, Fixed16_16) -> uint32_t { return 0xFu; };
		s.height_at = [](Fixed16_16, Fixed16_16) -> Fixed16_16 { return 0; };

		const auto in_range = generate_model_tile_instances(0, key, anchor_x, anchor_z,
		                                                    MODEL_CANDIDATE_RADIUS, cfg, s);
		if (!expect(in_range.count > 0, "tile-center anchor accepts candidates at the retail radius")) return 1;
		for (int i = 0; i < in_range.count; ++i) {
			const auto &inst = in_range.instances[i];
			const int64_t dx = static_cast<int64_t>(inst.center_x_fixed) - anchor_x;
			const int64_t dz = static_cast<int64_t>(inst.center_z_fixed) - anchor_z;
			if (!expect(std::llabs(dx) <= MODEL_CANDIDATE_RADIUS &&
			                std::llabs(dz) <= MODEL_CANDIDATE_RADIUS,
			            "every accepted center is inside the +-4u Chebyshev box")) {
				return 1;
			}
		}

		// An anchor a full tile away leaves the box empty.
		const auto far_anchor = generate_model_tile_instances(0, key,
		                                                      anchor_x + 0x200000, anchor_z,
		                                                      MODEL_CANDIDATE_RADIUS, cfg, s);
		if (!expect(far_anchor.count == 0, "an anchor 32u away accepts nothing at the +-4u radius")) return 1;

		// The gate is per-axis (Chebyshev), not Euclidean: an anchor at the
		// tile corner still accepts candidates whose Euclidean distance
		// exceeds the radius but whose per-axis deltas fit.
		const auto huge = generate_model_tile_instances(0, key, anchor_x, anchor_z,
		                                                0x40000000, cfg, s);
		bool found_diag = false;
		for (int i = 0; i < in_range.count && !found_diag; ++i) {
			const auto &inst = in_range.instances[i];
			const double dx = (inst.center_x_fixed - anchor_x) / 65536.0;
			const double dz = (inst.center_z_fixed - anchor_z) / 65536.0;
			if (dx * dx + dz * dz > 16.0) {  // beyond 4u Euclidean, inside 4u Chebyshev
				found_diag = true;
			}
		}
		if (!expect(found_diag, "the range gate is Chebyshev: diagonal candidates beyond 4u Euclidean pass")) return 1;
		(void)huge;
	}

	// --- Gate 2: path spacing, skipped on FORCE_ON ---------------------------
	{
		ModelPlacementConfig cfg{};
		std::vector<Fixed16_16> path_x, path_z;
		std::vector<int32_t> path_range;
		PlacementSamplers s;
		s.path_blocked = [&](Fixed16_16 wx, Fixed16_16 wz, int32_t range) {
			path_x.push_back(wx);
			path_z.push_back(wz);
			path_range.push_back(range);
			return true;  // always blocked
		};
		s.slot_mask_at = [](Fixed16_16, Fixed16_16) -> uint32_t { return 0xFu; };
		s.height_at = [](Fixed16_16, Fixed16_16) -> Fixed16_16 { return 0; };

		const auto blocked = generate_model_tile_instances(0, key, anchor_x, anchor_z,
		                                                   0x40000000, cfg, s);
		if (!expect(blocked.count == 0, "an always-blocked path rejects every candidate")) return 1;
		if (!expect(!path_range.empty() && path_range[0] == MODEL_PATH_SPACING,
		            "the path gate passes the witnessed 0x20000 spacing")) return 1;
		// The path sampler receives the candidate's own (x, z) — no boundary
		// negation (placement.h) — and the stream is deterministic per key.
		bool coords_stable = true;
		size_t idx = 0;
		// Re-run permissively to collect the candidate centers in order.
		PlacementSamplers probe = s;
		std::vector<Fixed16_16> cand_x, cand_z;
		probe.path_blocked = [&](Fixed16_16 wx, Fixed16_16 wz, int32_t) {
			cand_x.push_back(wx);
			cand_z.push_back(wz);
			return false;
		};
		(void)generate_model_tile_instances(0, key, anchor_x, anchor_z, 0x40000000, cfg, probe);
		for (idx = 0; idx < path_x.size() && idx < cand_x.size(); ++idx) {
			if (path_x[idx] != cand_x[idx] || path_z[idx] != cand_z[idx]) {
				coords_stable = false;
				break;
			}
		}
		if (!expect(coords_stable && !cand_x.empty(),
		            "path gate coordinates are stable across runs")) return 1;

		// FORCE_ON skips the path gate entirely.
		ModelPlacementConfig forced{};
		forced.attrib_flags[0] = FOLIAGE_ATTRIB_FORCE_ON;
		size_t calls_before = path_x.size();
		const auto forced_result = generate_model_tile_instances(0, key, anchor_x, anchor_z,
		                                                         0x40000000, forced, s);
		if (!expect(forced_result.count == MODEL_TILE_CAP,
		            "FORCE_ON skips the path gate (capped acceptance)")) return 1;
		if (!expect(path_x.size() == calls_before,
		            "FORCE_ON never calls the path sampler")) return 1;
	}

	// --- Gate 3: foliage-map slot mask ---------------------------------------
	{
		ModelPlacementConfig cfg{};
		PlacementSamplers s;
		s.path_blocked = [](Fixed16_16, Fixed16_16, int32_t) { return false; };
		s.height_at = [](Fixed16_16, Fixed16_16) -> Fixed16_16 { return 0; };

		// Mask carries only slot 2's bit: slot 0 places nothing, slot 2 places.
		s.slot_mask_at = [](Fixed16_16, Fixed16_16) -> uint32_t { return 1u << 2; };
		const auto wrong_slot = generate_model_tile_instances(0, key, anchor_x, anchor_z,
		                                                      0x40000000, cfg, s);
		if (!expect(wrong_slot.count == 0, "a mask without the slot bit rejects everything")) return 1;
		const auto right_slot = generate_model_tile_instances(2, key, anchor_x, anchor_z,
		                                                      0x40000000, cfg, s);
		if (!expect(right_slot.count == MODEL_TILE_CAP, "the matching slot bit accepts")) return 1;

		// Mask gate runs on the un-negated (x, z) pair: it receives the same
		// coordinates the candidate centers are emitted at.
		std::vector<Fixed16_16> mask_x, mask_z;
		PlacementSamplers rec = s;
		rec.slot_mask_at = [&](Fixed16_16 wx, Fixed16_16 wz) -> uint32_t {
			mask_x.push_back(wx);
			mask_z.push_back(wz);
			return 0xFu;
		};
		const auto all = generate_model_tile_instances(0, key, anchor_x, anchor_z,
		                                               0x40000000, cfg, rec);
		bool coords_match = all.count == MODEL_TILE_CAP && mask_x.size() >= static_cast<size_t>(all.count);
		for (int i = 0; coords_match && i < all.count; ++i) {
			if (mask_x[i] != all.instances[i].center_x_fixed ||
			    mask_z[i] != all.instances[i].center_z_fixed) {
				coords_match = false;
			}
		}
		if (!expect(coords_match, "the mask gate samples at the emitted candidate centers (x, z)")) return 1;
	}

	std::printf("OK: model placement gates (Chebyshev range, forceon path skip, slot mask)\n");
	return 0;
}
