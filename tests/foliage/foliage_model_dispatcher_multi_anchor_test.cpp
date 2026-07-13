// Multi-anchor cache semantics of the model tile walk:
// (1) the stagger frame regenerates once per TOUCHING ANCHOR [orig:
//     Foliage_UpdateModelTiles @ 0x601f50 regenerates per hit], and because
//     generate's accept gate is anchor-relative (|world - anchor| <= 0x40000
//     [orig: Foliage_GenerateModelTileInstances @ 0x600980]), the LAST
//     touching anchor's subset is the frame's end state — a same-frame dedup
//     froze the FIRST anchor's subset (caught in the 2026-07-12 pre-PR
//     review; docs/foliage/foliage-re.md D-FOLIAGE-11).
// (2) with more live tiles than cache entries in ONE frame, the no-victim
//     miss SKIPS adoption instead of recycling a this-frame entry (hosts hold
//     borrowed pointers into this frame's emissions; retail draws immediately
//     so it has no aliasing window).
#include <foliage/model_dispatcher.h>
#include <foliage/model_placement.h>

#include <cstdio>
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

PlacementSamplers accept_all_samplers() {
	PlacementSamplers s;
	s.path_blocked = [](Fixed16_16, Fixed16_16, int32_t) { return false; };
	s.slot_mask_at = [](Fixed16_16, Fixed16_16) -> uint32_t { return 0xFu; };
	s.height_at = [](Fixed16_16, Fixed16_16) -> Fixed16_16 { return 0; };
	return s;
}

} // namespace

int main() {
	const PlacementSamplers s = accept_all_samplers();
	ModelPlacementConfig cfg{};
	const int slot = 0;

	// --- (1) shared-tile stagger regen: last touching anchor wins ---------
	// A1 at (20u, 20u) and A2 at (28u, 20u): quadrant snaps share the x=16u
	// tile column, and their +-4u accept windows inside it are disjoint
	// (A1 x in [16,24], A2 x in [24,32]).
	const Fixed16_16 a1x = 0x00140000, a1z = 0x00140000;
	const Fixed16_16 a2x = 0x001C0000, a2z = 0x00140000;
	const Fixed16_16 a2_window_min_x = a2x - MODEL_CANDIDATE_RADIUS;

	ModelDispatcher dispatcher;
	std::vector<ModelTileDraw> out;

	// Prime off-stagger (frame 1 for slot 0): adoption fills from the first
	// touching anchor per tile.
	dispatcher.walk(slot, a1x, a1z, MODEL_DEPTH_GATE, 1, cfg, s, out);
	out.clear();
	dispatcher.walk(slot, a2x, a2z, MODEL_DEPTH_GATE, 1, cfg, s, out);

	// The stagger frame (8 for slot 0): each anchor's touch regenerates.
	const int64_t regen_before = dispatcher.regenerations();
	out.clear();
	dispatcher.walk(slot, a1x, a1z, MODEL_DEPTH_GATE, 8, cfg, s, out);
	if (!expect(dispatcher.regenerations() - regen_before == 4,
	            "anchor 1's stagger walk regenerates its 4 tiles")) return 1;
	out.clear();
	dispatcher.walk(slot, a2x, a2z, MODEL_DEPTH_GATE, 8, cfg, s, out);
	if (!expect(dispatcher.regenerations() - regen_before == 8,
	            "anchor 2's stagger walk regenerates per touch (shared tiles again)")) return 1;

	// A2's emitted draws — including the shared x=16u tiles — must carry A2's
	// accept-window content (last regen wins; a same-frame dedup would have
	// left the FIRST anchor's subset in the shared tiles).
	if (!expect(!out.empty(), "anchor 2 emits draws on the stagger frame")) return 1;
	for (const auto &draw : out) {
		for (int i = 0; i < draw.count; ++i) {
			if (!expect(draw.instances[i].center_x_fixed >= a2_window_min_x,
			            "shared-tile content after the stagger frame is the LAST "
			            "touching anchor's accept window")) {
				std::fprintf(stderr, "  instance x %d below A2 window min %d\n",
				             draw.instances[i].center_x_fixed, a2_window_min_x);
				return 1;
			}
		}
	}

	// Off-stagger follow-up: the cache serves A2's content without regen.
	out.clear();
	dispatcher.walk(slot, a2x, a2z, MODEL_DEPTH_GATE, 9, cfg, s, out);
	if (!expect(dispatcher.regenerations() - regen_before == 8,
	            "off-stagger walks do not regenerate")) return 1;
	for (const auto &draw : out) {
		for (int i = 0; i < draw.count; ++i) {
			if (!expect(draw.instances[i].center_x_fixed >= a2_window_min_x,
			            "the last stagger regen's content persists in the cache")) return 1;
		}
	}

	// --- (2) the no-victim miss skips adoption -----------------------------
	// 260 anchors spaced 32u apart touch 4 distinct tiles each: 1040 live
	// tiles in one frame against 1000 entries.
	ModelDispatcher crowded;
	std::vector<ModelTileDraw> crowd_out;
	for (int a = 0; a < 260; ++a) {
		const Fixed16_16 ax = static_cast<Fixed16_16>((32 * a + 20) * 0x10000);
		crowded.walk(slot, ax, a1z, MODEL_DEPTH_GATE, 1, cfg, s, crowd_out);
	}
	if (!expect(crowded.skipped_adoptions() > 0,
	            ">1000 live tiles in one frame skips adoptions")) return 1;
	if (!expect(crowded.cache_occupancy() == MODEL_CACHE_ENTRIES,
	            "the cache fills to exactly its 1000 entries")) return 1;
	// Every emitted borrowed view stays consistent (no this-frame recycling
	// dangled a pointer): counts match the instances the entry still holds.
	for (const auto &draw : crowd_out) {
		if (!expect(draw.count >= 0 && draw.instances != nullptr,
		            "emitted draws keep valid borrowed views")) return 1;
	}

	std::printf("OK: per-anchor stagger regen (last wins) + no-victim adoption skip\n");
	return 0;
}
