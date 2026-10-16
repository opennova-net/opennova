// One view's entity collect (runtime/inmatch/occlusion_collect.h) over a bare
// MissionKernel whose registry rows and ClientState are authored directly (no
// boot, no socket, no Godot): which rows the walk visits, the render gates'
// drops, a decoded row's registry twin, the wire latches' lifetime and the
// MODEL foliage tier's anchors.
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/occlusion_collect.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/replication/client_state.h>
#include <runtime/world/entity.h>
#include <runtime/world/occlusion_camera.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <vector>

namespace w = opennova::world;
namespace im = opennova::inmatch;

namespace {

int failures = 0;

bool expect(bool ok, const char *what) {
	if (!ok) {
		std::printf("FAIL: %s\n", what);
		++failures;
	}
	return ok;
}

bool contains(const std::vector<int32_t> &v, int32_t x) {
	return std::find(v.begin(), v.end(), x) != v.end();
}

bool has_anchor(const std::vector<w::Vec3> &v, float x, float y, float z) {
	for (const w::Vec3 &a : v)
		if (a.x == x && a.y == y && a.z == z) return true;
	return false;
}

// Presentation frame: the eye at mission (0, 0, 2) looking down mission +x.
w::OcclusionFrameCamera camera_down_x() {
	w::OcclusionViewSpec view;
	view.eye[0] = 0.0f;
	view.eye[1] = 2.0f;
	view.eye[2] = 0.0f;
	view.forward[0] = 1.0f;
	view.right[2] = 1.0f;
	view.up[1] = 1.0f;
	view.fov_y_deg = 60.0f;
	view.aspect = 16.0f / 9.0f;
	view.viewport_width = 1920.0f;
	view.fog_dist_units = 1000.0f;
	view.water_z_units = -1000.0f;
	w::OcclusionFrameCamera cam;
	w::occlusion_camera_from_view(view, cam);
	return cam;
}

// One row at a fixed slot. A held latch makes the gate's verdict the view
// test alone (no three-ray probe).
w::Entity &row(opennova::mission::MissionKernel &kernel, int pool, int slot, w::EntityKind kind,
		int32_t bms_id, float x, float y) {
	w::Entity seed;
	seed.kind = kind;
	seed.alive = true;
	seed.bms_id = bms_id;
	seed.position = {x, y, 2.0f};
	seed.bound_radius = 1.0f;
	seed.occlusion_latch = 5;
	const w::EntityHandle h = kernel.world.registry.spawn_at(w::EntityHandle::make(pool, slot), seed);
	return *kernel.world.registry.get(h);
}

void test_the_walk() {
	auto kernel_heap = std::make_unique<opennova::mission::MissionKernel>();
	opennova::mission::MissionKernel &kernel = *kernel_heap;
	kernel.world.registry.configure_pool(0, 16);
	kernel.world.registry.configure_pool(1, 16);
	const w::OcclusionFrameCamera cam = camera_down_x();

	// The placed rows: a crouched organic ahead (collected, anchored), one
	// behind the eye (culled), a hidden one (culled at the flags gate), one
	// standing on another entity (collected, no anchor), an item with no
	// render model (never collected), a marker and an identity-less organic
	// (neither visited).
	w::Entity &ahead = row(kernel, 0, 1, w::EntityKind::Organic, 101, 20.0f, 0.0f);
	ahead.net_stance_bits = 0x2u;
	row(kernel, 0, 2, w::EntityKind::Organic, 102, -20.0f, 0.0f);
	row(kernel, 0, 3, w::EntityKind::Organic, 103, 20.0f, 1.0f).flags |= 1u;
	w::Entity &riding = row(kernel, 0, 4, w::EntityKind::Organic, 104, 20.0f, -1.0f);
	riding.net_stance_bits = 0x1u;
	riding.ground_target = ahead.handle;
	w::Entity &item = row(kernel, 1, 1, w::EntityKind::Item, 105, 15.0f, 0.0f);
	item.item_id = 0x0321;
	row(kernel, 1, 2, w::EntityKind::Marker, 106, 15.0f, 0.0f);
	row(kernel, 0, 5, w::EntityKind::Organic, 0, 15.0f, 0.0f);
	// The local player: no placed identity, collected like any person.
	w::Entity &local = row(kernel, 0, 6, w::EntityKind::Organic, 0, 10.0f, 2.0f);
	local.net_stance_bits = 0x1u;
	kernel.world.cached.local_player = local.handle;
	// The host's own runtime spawns (no identity, no spawn origin): an organic
	// twin ahead and an item twin with no render model.
	w::Entity &spawned = row(kernel, 0, 8, w::EntityKind::Organic, 0, 30.0f, 0.0f);
	spawned.item_id = 0x0055;
	spawned.spawn_origin = w::kSpawnOriginNone;
	spawned.net_stance_bits = 0x2u;
	w::Entity &gun = row(kernel, 1, 7, w::EntityKind::Item, 0, 15.0f, 0.0f);
	gun.item_id = 0x0777;
	gun.spawn_origin = w::kSpawnOriginNone;

	kernel.occlusion.build_frame(kernel.world, kernel.collision, cam);
	im::WireOcclusionLatchMap latches;
	std::vector<w::EntityHandle> scratch;
	std::vector<int32_t> culled_bms;
	std::vector<int32_t> culled_wire;
	std::vector<w::Vec3> anchors;

	// No runtime: the placed rows and the local player alone.
	latches[0x1FFF] = 9;
	im::collect_view_verdicts(im::PresentRowsContext{kernel, nullptr, false}, latches, scratch, cam,
			culled_bms, culled_wire, anchors);
	std::vector<int32_t> sorted = culled_bms;
	std::sort(sorted.begin(), sorted.end());
	expect(sorted == std::vector<int32_t>({102, 103, 105}),
			"the placed walk drops the behind, hidden and model-less rows only");
	expect(culled_wire.empty() && latches.count(0x1FFF) == 1,
			"no runtime: no wire rows, the latches untouched");
	expect(anchors.size() == 2 && has_anchor(anchors, 20.0f, 0.0f, 2.0f) &&
					has_anchor(anchors, 10.0f, 2.0f, 2.0f),
			"the crouched placed organic and the local player anchor the MODEL tier");

	// The decoded rows.
	im::ClientRuntime runtime("OcclusionCollect");
	opennova::replication::ClientState &state = runtime.state();
	auto add = [&](uint16_t handle, uint16_t type_id, opennova::EntityClass cls, int32_t x)
			-> opennova::replication::ClientEntityState & {
		opennova::replication::ClientEntityState &es = state.upsert(handle);
		es.type_id = type_id;
		es.cls = cls;
		es.x = x << 16;
		es.y = 0;
		es.z = 2 << 16;
		return es;
	};
	add(0x1003, 0x1234, opennova::EntityClass::NoNetworkCallback, 15);  // bare, no model: culled
	add(0x1004, 0x1234, opennova::EntityClass::NoNetworkCallback, 15).state_flags_known = true;
	state.upsert(0x1004).state_flags = 0x01u; // hidden: never collected
	add(0x1005, 0, opennova::EntityClass::NoNetworkCallback, 15);       // no type: skipped
	add(item.handle.packed, 0x0321, opennova::EntityClass::NoNetworkCallback, 15); // placed twin: skipped
	// The twin's live pose, not the row's spawn image.
	add(spawned.handle.packed, 0x0055, opennova::EntityClass::Infantry, -40);
	add(gun.handle.packed, 0x0777, opennova::EntityClass::NoNetworkCallback, 15); // twin, no model: culled
	im::collect_view_verdicts(im::PresentRowsContext{kernel, &runtime, false}, latches, scratch, cam,
			culled_bms, culled_wire, anchors);
	sorted = culled_wire;
	std::sort(sorted.begin(), sorted.end());
	expect(sorted == std::vector<int32_t>({0x1003, static_cast<int32_t>(gun.handle.packed)}),
			"the wire walk drops the bare model-less row and the model-less twin only");
	expect(has_anchor(anchors, 30.0f, 0.0f, 2.0f) && anchors.size() == 3,
			"a visible twin anchors at its own live position");
	expect(latches.count(0x1FFF) == 0 && latches.count(0x1003) == 1 &&
					latches.count(0x1004) == 0,
			"a gone row's latch is dropped; a hidden row's never starts");
}

} // namespace

int main() {
	test_the_walk();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("occlusion_collect: ok\n");
	return 0;
}
