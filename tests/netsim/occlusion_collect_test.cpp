// One view's entity collect (runtime/inmatch/occlusion_collect.h) over a bare
// MissionKernel whose registry rows and ClientState are authored directly (no
// boot, no socket, no Godot): which rows the walk visits, the render gates'
// drops, a decoded row's registry twin, the wire latches' lifetime and the
// MODEL foliage tier's anchors.
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/occlusion_collect.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/replication/client_state.h>
#include <runtime/world/collision.h> // kStaticProxCountBound, kItemDefTypeBuilding
#include <runtime/world/entity.h>
#include <runtime/world/occlusion.h>
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

// Presentation frame: the eye at mission (0, 0, 2) looking down mission +x,
// every component of the triad set (the spec's defaults look down -z).
w::OcclusionFrameCamera camera_down_x(float water_z_units = -1000.0f) {
	w::OcclusionViewSpec view;
	view.eye[0] = 0.0f;
	view.eye[1] = 2.0f;
	view.eye[2] = 0.0f;
	const float forward[3] = {1.0f, 0.0f, 0.0f};
	const float right[3] = {0.0f, 0.0f, 1.0f};
	const float up[3] = {0.0f, 1.0f, 0.0f};
	for (int i = 0; i < 3; ++i) {
		view.forward[i] = forward[i];
		view.right[i] = right[i];
		view.up[i] = up[i];
	}
	view.fov_y_deg = 60.0f;
	view.aspect = 16.0f / 9.0f;
	view.viewport_width = 1920.0f;
	view.fog_dist_units = 1000.0f;
	view.water_z_units = water_z_units;
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

// Which gate drops a row: its Flags words, its blink quad, its render and
// collision models, and the sphere leg (a held latch, no draw) at its own place
// and on the camera's axis at the same depth and height.
void dump_gates(const char *what, opennova::mission::MissionKernel &kernel,
		const w::OcclusionFrameCamera &cam, const w::Entity &e) {
	uint8_t held = 5;
	const int32_t at[3] = {e.position.x.q16, e.position.y.q16, e.position.z.q16};
	const int32_t axis[3] = {e.position.x.q16, 0, e.position.z.q16};
	const bool at_place = kernel.occlusion.sphere_render_visible(kernel.collision, cam, at, 0,
			held, 0);
	const bool on_axis = kernel.occlusion.sphere_render_visible(kernel.collision, cam, axis, 0,
			held, 0);
	std::printf("%s: flags 0x%x engine 0x%x blink 0x%x 0x%x 0x%x 0x%x render_model %d "
				"collision_model %d sphere at its place %d on the axis %d
",
			what, e.flags, e.engine_flags, e.blink_hits[0], e.blink_hits[1], e.blink_hits[2],
			e.blink_hits[3], kernel.occlusion.has_render_model(e.handle) ? 1 : 0,
			kernel.collision.model_for(kernel.world, e.handle) != nullptr ? 1 : 0,
			at_place ? 1 : 0, on_axis ? 1 : 0);
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

	// A joiner walks the decoded row whose wire handle equals its native L's
	// packed handle like any other (H and L are separate domains; the self row
	// is the self handle's): a model-less row there is culled, where the L
	// comparison skipped it (D-NET-698) [orig: Terrain_CollectVisibleEntities_0
	// @0x5C6F20, every drawn entity built from the wire].
	add(local.handle.packed, 0x1234, opennova::EntityClass::Player, 15);
	im::collect_view_verdicts(im::PresentRowsContext{kernel, &runtime, true}, latches, scratch, cam,
			culled_bms, culled_wire, anchors);
	expect(contains(culled_wire, static_cast<int32_t>(local.handle.packed)),
			"a joiner's wire walk takes the row at L's packed handle");
}

// The water mirror's collect (collect_mirror_view) runs the scene collect's walk over
// the mirror's camera under its Flags match: at or above the water a placed person and
// a bare wire row without the Reflective bit drop ahead of their latches while a
// vehicle's latch ticks; below the water the walk is unfiltered, and the crouched
// person it collects anchors the MODEL tier the reflected subscene's BySide waves
// update. [orig: Terrain_CollectVisibleEntitiesForReflection @ 0x5c90a0 - the calls
//  @ 0x5c913d / @ 0x5c9145 / @ 0x5c914e, the match passed @ 0x5c913b / @ 0x5c9142 /
//  @ 0x5c914a; Water_RenderReflectedWorldScene @ 0x5c8510 - the BySide waves
//  @ 0x5c8590 / @ 0x5c8599, their Foliage_UpdateModelTiles calls @ 0x5c7edb..0x5c7f11]
void test_the_mirror_walk() {
	auto kernel_heap = std::make_unique<opennova::mission::MissionKernel>();
	opennova::mission::MissionKernel &kernel = *kernel_heap;
	kernel.world.registry.configure_pool(0, 16);
	kernel.world.registry.configure_pool(1, 16);
	w::Entity &person = row(kernel, 0, 1, w::EntityKind::Organic, 101, 20.0f, 0.0f);
	person.net_stance_bits = 0x2u;
	w::Entity &car = row(kernel, 1, 1, w::EntityKind::Item, 102, 20.0f, 1.0f);
	car.item_type = 1; // a vehicle def: its init's Reflective bit
	opennova::renderer::ObjectProjectionSphere unstamped;
	unstamped.valid = true;
	kernel.occlusion.assign_render_model(car.handle, unstamped);
	im::ClientRuntime runtime("OcclusionCollectMirror");
	opennova::replication::ClientEntityState &bare = runtime.state().upsert(0x1003);
	bare.type_id = 0x1234;
	bare.cls = opennova::EntityClass::NoNetworkCallback;
	bare.x = 15 << 16;
	bare.z = 2 << 16;

	im::WireOcclusionLatchMap latches;
	std::vector<w::EntityHandle> scratch;
	im::MirrorViewLists lists;
	const im::PresentRowsContext context{kernel, &runtime, false};

	// The eye above the water.
	im::collect_mirror_view(context, latches, scratch, camera_down_x(), lists);
	expect(contains(lists.culled_bms, 101) && person.occlusion_latch == 5,
			"above the water the person drops ahead of its latch");
	expect(!contains(lists.culled_bms, 102) && car.occlusion_latch == 4,
			"the vehicle is collected and its latch ticks");
	expect(contains(lists.culled_wire, 0x1003) && latches.count(0x1003) == 0,
			"the bare row without the Reflective bit drops ahead of its latch");
	expect(lists.anchors.empty(), "above the water no person anchors the mirror's MODEL tier");
	expect(kernel.occlusion.selected_view() == w::OcclusionView::kMain,
			"the scene's view stays selected");

	// The eye below the water: unfiltered.
	im::collect_mirror_view(context, latches, scratch, camera_down_x(10.0f), lists);
	expect(!contains(lists.culled_bms, 101) && person.occlusion_latch == 4,
			"below the water the person is collected and its latch ticks");
	expect(lists.anchors.size() == 1 && has_anchor(lists.anchors, 20.0f, 0.0f, 2.0f),
			"below the water the crouched person anchors the mirror's MODEL tier");
	expect(!contains(lists.culled_bms, 102) && car.occlusion_latch == 3,
			"the vehicle again");
	expect(latches.count(0x1003) == 1 && contains(lists.culled_wire, 0x1003),
			"the bare row reaches its latch, then drops for its missing model");
}

// The walk order: the statics (the pool-2 rows that are not Building-type defs, in
// slot order), then pool 0, then pool 1, each pool in slot order with the placed rows,
// the host's own runtime spawns (their wire rows) and the local player each at its own
// slot. Five rows that each re-arm in one collect take the shared PRNG stream's five
// draws in that order. [orig: Terrain_CollectVisibleEntities @ 0x5c9160 - _0 @ 0x5c9199,
// Terrain_CollectVisibleEntitiesForTerrain pool 0 @ 0x5c91a1, pool 1 @ 0x5c91aa;
// Entity_BuildProximityLists_Pool2 @ 0x4b9430's second pass @ 0x4b94e0..0x4b9568;
// ForTerrain's slot walk @ 0x5c8caa..0x5c8ced; NapiNPClientMsg_0x00C @ 0x42e730 - the
// slot @ 0x42e7bf..0x42e7dd; the re-arm `(PRNG_Next16_C() & 7) + 16` @ 0x5c7162 /
// @ 0x5c8ea9 / @ 0x5c8fd6, PRNG_Next16_C @ 0x6131b0]
void test_the_walk_order() {
	auto kernel_heap = std::make_unique<opennova::mission::MissionKernel>();
	opennova::mission::MissionKernel &kernel = *kernel_heap;
	kernel.world.registry.configure_pool(0, 16);
	kernel.world.registry.configure_pool(1, 16);
	kernel.world.registry.configure_pool(2, 16);
	opennova::renderer::ObjectProjectionSphere unstamped;
	unstamped.valid = true;
	// Spawned in an order that is none of the walks': pool 1, pool 0, pool 2.
	w::Entity &vehicle = row(kernel, 1, 1, w::EntityKind::Item, 302, 20.0f, 3.0f);
	kernel.occlusion.assign_render_model(vehicle.handle, unstamped);
	w::Entity &local = row(kernel, 0, 6, w::EntityKind::Organic, 0, 20.0f, 2.0f);
	kernel.world.cached.local_player = local.handle;
	w::Entity &spawned = row(kernel, 0, 4, w::EntityKind::Organic, 0, 20.0f, 1.0f);
	spawned.item_id = 0x0055;
	spawned.spawn_origin = w::kSpawnOriginNone;
	w::Entity &person = row(kernel, 0, 2, w::EntityKind::Organic, 301, 20.0f, 0.0f);
	// A decoration: a pool-2 row whose def is not Building-type, entity-collected.
	w::Entity &decoration = row(kernel, 2, 3, w::EntityKind::Building, 303, 20.0f, -1.0f);
	decoration.has_item_def = true;
	decoration.item_type_index = 1;
	decoration.item_type = 6;
	kernel.occlusion.assign_render_model(decoration.handle, unstamped);
	w::Entity *const order[5] = {&decoration, &person, &spawned, &local, &vehicle};
	const char *const names[5] = {"decoration", "person", "spawned", "local", "vehicle"};

	im::ClientRuntime runtime("OcclusionCollectOrder");
	opennova::replication::ClientEntityState &twin_row = runtime.state().upsert(spawned.handle.packed);
	twin_row.type_id = 0x0055;
	twin_row.cls = opennova::EntityClass::Infantry;

	const w::OcclusionFrameCamera cam = camera_down_x();
	kernel.occlusion.build_frame(kernel.world, kernel.collision, cam);
	im::WireOcclusionLatchMap latches;
	std::vector<w::EntityHandle> scratch;
	std::vector<int32_t> culled_bms;
	std::vector<int32_t> culled_wire;
	std::vector<w::Vec3> anchors;
	const auto dump = [&](const char *when) {
		std::printf("walk order %s: culled_bms", when);
		for (const int32_t id : culled_bms) std::printf(" %d", id);
		std::printf(", culled_wire");
		for (const int32_t h : culled_wire) std::printf(" 0x%04x", h);
		std::printf(", latches");
		for (int i = 0; i < 5; ++i)
			std::printf(" %s=%d", names[i], static_cast<int>(order[i]->occlusion_latch));
		std::printf("\n");
	};
	// The fixture first, apart from the order: with every latch held, each of the
	// five rows passes the gates ahead of the latch (no draw).
	for (w::Entity *e : order) e->occlusion_latch = 5;
	im::collect_view_verdicts(im::PresentRowsContext{kernel, &runtime, false}, latches, scratch, cam,
			culled_bms, culled_wire, anchors);
	if (!expect(culled_bms.empty() && culled_wire.empty(), "every row passes the gates")) {
		dump("(held latches)");
		dump_gates("walk order decoration", kernel, cam, decoration);
	}
	for (w::Entity *e : order) e->occlusion_latch = 0;

	uint32_t stream = 0x13579BDFu;
	kernel.occlusion.bind_focal_wind_random(&stream);
	// PRNG_Next16_C's step, the oracle for each draw; this seed's five draws differ in
	// their low three bits, so each latch names its draw.
	const auto next16 = [](uint32_t &s) {
		const auto rol = [](uint32_t v, int k) { return (v << k) | (v >> (32 - k)); };
		const uint32_t rotated = rol(s + rol(s, 11), 4);
		s = rotated ^ 1u;
		return static_cast<uint16_t>((rotated & 0xFFFFu) ^ 1u);
	};
	uint32_t expected_stream = stream;
	uint8_t expected[5];
	for (uint8_t &latch : expected) latch = static_cast<uint8_t>((next16(expected_stream) & 7) + 16);
	for (int i = 0; i < 5; ++i)
		for (int j = i + 1; j < 5; ++j)
			expect(expected[i] != expected[j], "the seed's five draws are distinct");

	im::collect_view_verdicts(im::PresentRowsContext{kernel, &runtime, false}, latches, scratch, cam,
			culled_bms, culled_wire, anchors);
	bool ok = expect(culled_bms.empty() && culled_wire.empty(), "every row is collected");
	ok = expect(stream == expected_stream, "five re-arms, one draw each") && ok;
	ok = expect(decoration.occlusion_latch == expected[0], "the static takes the first draw") && ok;
	ok = expect(person.occlusion_latch == expected[1], "pool 0 slot 2's placed person the second") &&
			ok;
	ok = expect(spawned.occlusion_latch == expected[2], "pool 0 slot 4's runtime spawn the third") &&
			ok;
	ok = expect(local.occlusion_latch == expected[3], "pool 0 slot 6's local player the fourth") &&
			ok;
	ok = expect(vehicle.occlusion_latch == expected[4], "pool 1's vehicle the last") && ok;
	if (!ok) {
		dump("(re-armed)");
		std::printf("walk order: expected");
		for (int i = 0; i < 5; ++i) std::printf(" %d", static_cast<int>(expected[i]));
		std::printf(", stream 0x%08x (expected 0x%08x)\n", stream, expected_stream);
	}
}

// The statics' walk is the static prox list's tail, and the list counts at most
// 1199 rows, Building-type defs first: with 1198 buildings listed, the first
// decoration after them is collected and the next one is never walked (no verdict,
// its latch untouched); a row with no def or no item type is never listed.
// [orig: Entity_BuildProximityLists_Pool2 @ 0x4b9430 - pass 1's tests @ 0x4b945d /
//  @ 0x4b9466 and bound @ 0x4b94b7 / @ 0x4b94cb, pass 2's @ 0x4b94f1 / @ 0x4b94fa
//  and @ 0x4b954b / @ 0x4b955f; Terrain_CollectVisibleEntities_0 @ 0x5c6f20, the
//  walk to g_StaticProxCount @ 0x5c6f48..0x5c721e]
void test_the_statics_bound() {
	auto kernel_heap = std::make_unique<opennova::mission::MissionKernel>();
	opennova::mission::MissionKernel &kernel = *kernel_heap;
	constexpr int kBuildings = w::kStaticProxCountBound - 1;
	kernel.world.registry.configure_pool(2, kBuildings + 3);
	opennova::renderer::ObjectProjectionSphere unstamped;
	unstamped.valid = true;
	// A def-less decoration ahead of every listed row: never listed.
	w::Entity &unlisted = row(kernel, 2, 0, w::EntityKind::Item, 400, 20.0f, 0.0f);
	kernel.occlusion.assign_render_model(unlisted.handle, unstamped);
	for (int slot = 1; slot <= kBuildings; ++slot) {
		w::Entity &b = row(kernel, 2, slot, w::EntityKind::Building, 0, -20.0f, 0.0f);
		b.has_item_def = true;
		b.item_type_index = 2;
		b.item_type = w::kItemDefTypeBuilding;
	}
	const auto decoration = [&](int slot, int32_t bms_id) -> w::Entity & {
		w::Entity &d = row(kernel, 2, slot, w::EntityKind::Building, bms_id, 20.0f, 1.0f);
		d.has_item_def = true;
		d.item_type_index = 1;
		d.item_type = 6;
		d.occlusion_latch = 0;
		kernel.occlusion.assign_render_model(d.handle, unstamped);
		return d;
	};
	w::Entity &counted = decoration(kBuildings + 1, 401);
	w::Entity &past = decoration(kBuildings + 2, 402);
	unlisted.occlusion_latch = 0;

	const w::OcclusionFrameCamera cam = camera_down_x();
	kernel.occlusion.build_frame(kernel.world, kernel.collision, cam);
	im::WireOcclusionLatchMap latches;
	std::vector<w::EntityHandle> scratch;
	std::vector<int32_t> culled_bms;
	std::vector<int32_t> culled_wire;
	std::vector<w::Vec3> anchors;
	im::collect_view_verdicts(im::PresentRowsContext{kernel, nullptr, false}, latches, scratch, cam,
			culled_bms, culled_wire, anchors);
	expect(culled_bms.empty(), "no static is culled");
	expect(counted.occlusion_latch >= 16 && counted.occlusion_latch <= 23,
			"the 1199th listed row is walked and its latch re-arms");
	expect(past.occlusion_latch == 0, "the row past the bound is never walked");
	expect(unlisted.occlusion_latch == 0, "a def-less row is never listed");
}

// A listed decoration to the right of the camera's axis (a negative mission y,
// the camera looking down mission +x) is collected: inside the frustum, its
// latch held, nothing ahead of the latch drops it.
// [orig: Terrain_CollectVisibleEntities_0 @ 0x5c6f20 - the gates ahead of the latch
//  @ 0x5c6fd1..0x5c7112, the latch @ 0x5c7125..0x5c7162]
void test_a_decoration_right_of_the_axis_is_collected() {
	auto kernel_heap = std::make_unique<opennova::mission::MissionKernel>();
	opennova::mission::MissionKernel &kernel = *kernel_heap;
	kernel.world.registry.configure_pool(2, 16);
	opennova::renderer::ObjectProjectionSphere unstamped;
	unstamped.valid = true;
	w::Entity &decoration = row(kernel, 2, 3, w::EntityKind::Building, 303, 20.0f, -1.0f);
	decoration.has_item_def = true;
	decoration.item_type_index = 1;
	decoration.item_type = 6;
	kernel.occlusion.assign_render_model(decoration.handle, unstamped);
	const w::OcclusionFrameCamera cam = camera_down_x();
	kernel.occlusion.build_frame(kernel.world, kernel.collision, cam);
	im::WireOcclusionLatchMap latches;
	std::vector<w::EntityHandle> scratch;
	std::vector<int32_t> culled_bms;
	std::vector<int32_t> culled_wire;
	std::vector<w::Vec3> anchors;
	im::collect_view_verdicts(im::PresentRowsContext{kernel, nullptr, false}, latches, scratch, cam,
			culled_bms, culled_wire, anchors);
	if (!expect(culled_bms.empty() && decoration.occlusion_latch == 4,
				"a decoration right of the axis is collected and its latch ticks"))
		dump_gates("right of the axis", kernel, cam, decoration);
}

} // namespace

int main() {
	test_the_walk();
	test_the_mirror_walk();
	test_the_walk_order();
	test_the_statics_bound();
	test_a_decoration_right_of_the_axis_is_collected();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("occlusion_collect: ok\n");
	return 0;
}
