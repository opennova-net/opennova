// foliage::Runtime::invalidate_cells (ADR 0046 S25: an editor's sculpt or paint): the cached detail cells and model
// tiles whose instances read the changed region dropped, reported evicted with the next frame and generated again
// when visible (a new revision); every other cell kept (a hit, its revision unchanged); an empty rect drops nothing;
// reset forgets what was dropped.
#include <formats/foliage/runtime.h>

#include <array>
#include <cstdio>
#include <cstdint>
#include <vector>

using namespace opennova;
using namespace opennova::foliage;

namespace {

int g_fail = 0;

void check(bool condition, const char *message) {
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message);
		g_fail = 1;
	}
}

WorldSamplers flat_world(float height) {
	WorldSamplers world;
	world.detail_foliage_mask_at = [](int32_t, int32_t) { return 0x1u; };
	world.model_foliage_mask_at = [](int32_t, int32_t) { return 0x1u; };
	world.height_at = [height](float, float) { return height; };
	world.path_blocked = [](float, float, float) { return false; };
	return world;
}

// A detail cell's key: its X-min and Godot-plane Z-min, 16 units on.
uint32_t detail_key(int32_t x, int32_t z) {
	return (static_cast<uint32_t>(x & 0x7FFF) << 16) | static_cast<uint32_t>(z & 0x7FFF);
}

FrameRequest request_of(const std::vector<uint32_t> &keys, bool silhouette) {
	FrameRequest request;
	request.slots[0].enabled = true;
	request.slots[0].model_radius = 2.0f;
	for (const uint32_t key : keys) request.detail_cells.push_back({key, 10.0f});
	if (silhouette) request.silhouette_anchors.push_back({{32.0f, 48.0f}, 60.0f, 60.0f});
	return request;
}

uint64_t revision_of(const std::vector<CacheIdentity> &identities, uint32_t key) {
	for (const CacheIdentity &identity : identities)
		if (identity.key == key) return identity.revision;
	return 0;
}

void test_detail_cells() {
	Runtime runtime;
	float height = 4.0f;
	const WorldSamplers world = flat_world(height);
	const uint32_t near_key = detail_key(48, 16);  // [48, 64) x [16, 32)
	const uint32_t far_key = detail_key(128, 128); // [128, 144) x [128, 144)
	const FrameRequest request = request_of({near_key, far_key}, false);
	FrameOutput first = runtime.render_frame(request, world);
	const uint64_t near_revision = revision_of(first.detail_generated, near_key);
	const uint64_t far_revision = revision_of(first.detail_generated, far_key);
	check(near_revision != 0 && far_revision != 0, "both cells generated");

	// A rect over the near cell alone (its bilinear tap one texel on reaches x 64).
	check(runtime.invalidate_cells(60, 20, 62, 22) == 1, "one cell dropped");
	check(runtime.invalidate_cells(0, 0, 0, 10) == 0, "an empty rect drops nothing");
	FrameOutput second = runtime.render_frame(request, world);
	check(revision_of(second.detail_evicted, near_key) == near_revision, "the dropped cell reported evicted");
	check(revision_of(second.detail_evicted, far_key) == 0, "the far cell kept");
	const uint64_t regenerated = revision_of(second.detail_generated, near_key);
	check(regenerated != 0 && regenerated != near_revision, "the dropped cell generated again, a new revision");
	check(revision_of(second.detail_generated, far_key) == 0 && runtime.get_stats().detail.hits == 1,
	      "the far cell a hit");
	// A rect just past the near cell's bilinear reach drops nothing where its geometry reaches no further.
	check(runtime.invalidate_cells(66, 16, 70, 32) == 0, "a rect past the cell's reach drops nothing");
	FrameOutput third = runtime.render_frame(request, world);
	check(third.detail_evicted.empty() && third.detail_generated.empty(), "nothing generated again");
	// The same rect where the slot's expanded geometry reaches 2 units past an instance (the embedder's detail
	// reach: its vertices' planar extent): the cell's reach is 3 and the rect meets it.
	std::array<int32_t, FOLIAGE_MAX_DEFS> reach{};
	reach[0] = 2;
	check(runtime.invalidate_cells(66, 16, 70, 32, reach) == 1, "the geometry's reach drops the cell");
	check(runtime.invalidate_cells(68, 16, 70, 32, reach) == 0, "a rect past the geometry's reach drops nothing");
	FrameOutput regrown = runtime.render_frame(request, world);
	check(revision_of(regrown.detail_evicted, near_key) != 0, "reported evicted");
	// Dropped then reset: nothing reported after the reset.
	check(runtime.invalidate_cells(0, 0, 1024, 1024) == 2, "the whole plane drops both");
	runtime.reset();
	FrameOutput fourth = runtime.render_frame(request, world);
	check(fourth.detail_evicted.empty(), "a reset forgets the dropped cells");
}

void test_model_tiles() {
	Runtime runtime;
	float height = 4.0f;
	const WorldSamplers world = flat_world(height);
	const FrameRequest request = request_of({}, true);
	FrameOutput first = runtime.render_frame(request, world);
	check(!first.model_generated.empty(), "the anchor's tiles generated");
	// The anchor at (32, mission y -48): its four tiles about it; a rect there drops them.
	const size_t dropped = runtime.invalidate_cells(24, 40, 40, 56);
	check(dropped >= 1 && dropped <= first.model_generated.size(), "the tiles over the rect dropped");
	FrameOutput second = runtime.render_frame(request, world);
	check(second.model_evicted.size() == dropped, "each dropped tile reported evicted");
	size_t again = 0;
	for (const CacheIdentity &identity : second.model_generated)
		if (revision_of(second.model_evicted, identity.key) != 0) ++again;
	check(again == dropped, "each generated again when visible");
}

} // namespace

int main() {
	test_detail_cells();
	test_model_tiles();
	if (g_fail == 0) std::printf("foliage_runtime_invalidate: all tests passed\n");
	return g_fail;
}
