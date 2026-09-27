// Retained retail minimap banks: 0x40 routing/refresh/clear/expiry and 0x6B
// linked markers over the one ClientReplicaPipeline reducer.
// [orig: MapOverlay_UpdateOrCreateSlot @0x5BEA60; Minimap_UpdateOverlayEntity
// @0x5BEC10; MapOverlay_UpdateTimers @0x5BFCE0]

#include <runtime/replication/client_replica_pipeline.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/loopback_channel.h>
#include <runtime/inmatch/session.h>
#include <runtime/world/local_player_view.h>
#include <net/npwire/ingame_message_id.h>

#include <cstdio>
#include <vector>

namespace ns = opennova::replication;

namespace {

int failures = 0;

#define CHECK(c, m) do { if (!(c)) { std::fprintf(stderr, "FAIL: %s\n", m); ++failures; } } while (0)

std::vector<uint8_t> zone(uint16_t handle, uint8_t icon, uint8_t color,
		uint8_t flags, uint8_t source = 0) {
	return {1, static_cast<uint8_t>(handle), static_cast<uint8_t>(handle >> 8),
			icon, color, flags, source};
}

// One 12-byte 0x6B record: handle, s16 x/y/z whole units, u16 lifetime
// seconds, type, height. [orig: NapiNPClientMsg_0x06B @0x425520 marshalling]
std::vector<uint8_t> linked_record(uint16_t handle, int16_t x, int16_t y,
		int16_t z, uint16_t seconds, uint8_t type, uint8_t height) {
	std::vector<uint8_t> body{1};
	auto put16 = [&body](uint16_t v) {
		body.push_back(static_cast<uint8_t>(v));
		body.push_back(static_cast<uint8_t>(v >> 8));
	};
	put16(handle);
	put16(static_cast<uint16_t>(x));
	put16(static_cast<uint16_t>(y));
	put16(static_cast<uint16_t>(z));
	put16(seconds);
	body.push_back(type);
	body.push_back(height);
	return body;
}

const ns::ClientMinimapOverlaySlot *find_slot(
		const ns::ClientMinimapState &map, uint16_t handle,
		bool live_only = true) {
	for (const auto &bank : {&map.transient, &map.persistent}) {
		for (const auto &slot : *bank)
			if (slot.active && slot.handle == handle) return &slot;
	}
	for (const auto &slot : map.special) {
		if (!slot.active || slot.handle != handle) continue;
		if (live_only && slot.remaining_ticks == 0) continue;
		return &slot;
	}
	return nullptr;
}

} // namespace

int main() {
	ns::ClientReplicaPipeline view;
	auto &entity = view.state().upsert(0x2001);
	entity.x = 10 << 16;
	entity.y = -4 << 16;
	entity.z = 3 << 16;
	entity.heading_bam = 0x20000000;
	entity.cls = opennova::EntityClass::Vehicle;
	entity.team = 1;
	entity.team_known = true;

	view.apply(opennova::s2c::CAPTURE_ZONE_STATE,
			zone(entity.handle, 10, 0x0A, 0));
	const auto *slot = find_slot(view.state().minimap, entity.handle);
	CHECK(slot != nullptr, "0x40 creates a retained marker");
	if (slot != nullptr) {
		CHECK(slot->x == entity.x && slot->heading_bam == entity.heading_bam,
				"0x40 marker carries the decoded entity pose");
		CHECK(slot->entity_known, "0x40 marker knows its live entity");
		CHECK(slot->argb == 0xFF304080u, "color 0x0A resolves retail blue");
	}
	entity.x = 20 << 16;
	view.tick_minimap_overlays();
	CHECK(find_slot(view.state().minimap, entity.handle)->x == (20 << 16),
			"regular marker follows the live entity like retail's draw-time "
			"pool read");
	// A refresh updates the FOUND slot in place: the bank — and with it the
	// aging class — is fixed at first allocation, so a 0x10-flagged refresh
	// of a transient marker stays transient and still ages out.
	// [orig: MapOverlay_UpdateOrCreateSlot @0x5beb60..0x5beb7f]
	view.apply(opennova::s2c::CAPTURE_ZONE_STATE,
			zone(entity.handle, 3, 0x09, 0x10));
	{
		bool in_transient = false;
		for (const auto &s : view.state().minimap.transient)
			in_transient |= s.active && s.handle == entity.handle &&
					s.param == 3 && s.x == entity.x;
		bool in_persistent = false;
		for (const auto &s : view.state().minimap.persistent)
			in_persistent |= s.active && s.handle == entity.handle;
		CHECK(in_transient && !in_persistent,
				"a flags refresh updates the slot in place, never migrating banks");
	}
	for (int i = 0; i < ns::kMinimapOverlayLifetimeTicks + 2; ++i)
		view.tick_minimap_overlays();
	CHECK(find_slot(view.state().minimap, entity.handle) == nullptr,
			"the refreshed slot keeps its transient aging class and expires");
	// A marker ALLOCATED with 0x10 lands in the persistent bank and never
	// ages. [orig: MapOverlay_AllocSlot @0x5be98e..0x5be998; timers skip slot_data]
	view.apply(opennova::s2c::CAPTURE_ZONE_STATE,
			zone(entity.handle, 3, 0x09, 0x10));
	for (int i = 0; i < ns::kMinimapOverlayLifetimeTicks + 2; ++i)
		view.tick_minimap_overlays();
	CHECK(find_slot(view.state().minimap, entity.handle) != nullptr,
			"persistent 0x10 marker does not expire");
	view.apply(opennova::s2c::CAPTURE_ZONE_STATE,
			zone(entity.handle, 0, 0, 0x20));
	CHECK(find_slot(view.state().minimap, entity.handle) == nullptr,
			"0x20 frees the found slot (lifetime 0, handle -1)");

	// The full 32-entry color table is live: index 8 is the witnessed
	// 0xFF907000, and the alpha-0 tail entries reject the marker.
	// [orig: g_MinimapOverlayColorTable @0x840A10; alpha gate @0x5beb3e]
	view.apply(opennova::s2c::CAPTURE_ZONE_STATE,
			zone(entity.handle, 12, 8, 0x40));
	slot = find_slot(view.state().minimap, entity.handle);
	CHECK(slot != nullptr && slot->argb == 0xFF907000u,
			"color index 8 resolves the witnessed table entry");
	for (int i = 0; i < ns::kMinimapOverlayLifetimeTicks; ++i)
		view.tick_minimap_overlays();
	CHECK(find_slot(view.state().minimap, entity.handle) == nullptr,
			"special unlinked marker expires after 1984 ticks");
	// The alpha-0 reject lives ONLY on the 33..42 alias branch: a DIRECT
	// index < 0x20 takes the table entry straight (the draw path forces
	// alpha 0xFF), while an alias resolving to an alpha-0 entry rejects.
	// [orig: direct @0x5beb16..0x5beb1b (no test); alias gate @0x5beb3e]
	view.apply(opennova::s2c::CAPTURE_ZONE_STATE,
			zone(entity.handle, 12, 25, 0x40));
	slot = find_slot(view.state().minimap, entity.handle);
	CHECK(slot != nullptr && slot->argb == 0x00000000u,
			"a direct alpha-0 table index still creates the slot");
	view.apply(opennova::s2c::CAPTURE_ZONE_STATE,
			zone(entity.handle, 0, 0, 0x20));
	view.apply(opennova::s2c::CAPTURE_ZONE_STATE,
			zone(entity.handle, 12, 42, 0x40));
	CHECK(find_slot(view.state().minimap, entity.handle) == nullptr,
			"an alias index onto an alpha-0 entry rejects the marker");

	// Unknown-but-valid pool handles are retained with a zero pose, matching
	// retail's unconditional pool-slot read. [orig: @0x5beac0]
	view.apply(opennova::s2c::CAPTURE_ZONE_STATE, zone(0x2002, 10, 0x0C, 0));
	slot = find_slot(view.state().minimap, 0x2002);
	CHECK(slot != nullptr && !slot->entity_known && slot->x == 0,
			"unknown entity keeps the marker with entity_known false");

	// 0x6B consumes the full wire record: pose from the wire, lifetime in
	// seconds x62, type 1 -> icon 253, flags 0xC4, height -> slot z.
	view.apply(opennova::s2c::MINIMAP_OVERLAY,
			linked_record(entity.handle, 100, -50, 7, 2, 1, 40));
	slot = find_slot(view.state().minimap, entity.handle);
	CHECK(slot != nullptr && slot->param == 253,
			"0x6B type 1 creates the 253 pulse marker");
	if (slot != nullptr) {
		CHECK(slot->flags == 0xC4u, "0x6B stamps the witnessed 0xC4 flags");
		CHECK(slot->x == (100 << 16) && slot->y == (-50 << 16),
				"0x6B marker pose comes from the wire record");
		CHECK(slot->z == (40 << 16), "0x6B height byte is the ring radius");
		CHECK(slot->argb == 0xFF304080u, "team 1 resolves table entry 10");
	}
	bool linked_active = false;
	for (const auto &link : view.state().minimap.linked)
		linked_active |= link.active && link.handle == entity.handle &&
				link.remaining_ticks == 2u * 62u;
	CHECK(linked_active, "0x6B link lifetime is wire seconds x62");
	const auto &point = view.state().minimap.linked[0];
	CHECK(point.type == 1 && point.x == 100 * 65536 && point.y == -50 * 65536 &&
					point.radius_q16 == 40 * 65536,
			"designation query retains link geometry independently");
	{
		opennova::replication::LoopbackChannel channel;
		opennova::inmatch::ClientRuntime runtime(channel);
		runtime.state().upsert(0x2001).team = 1;
		auto receive = [&](uint8_t type, uint16_t seconds) {
			channel.host_send(opennova::s2c::MINIMAP_OVERLAY,
					linked_record(0x2001, 100, -50, 7, seconds, type, 40), false);
			runtime.Client_ProcessNetworkFrame(0);
			return opennova::inmatch::Role::view_session_inputs_for(&runtime, false, false);
		};
		const auto active = receive(1, 2);
		CHECK(active.hud_designations.size() == 1, "a live type-1 point reaches the HUD feed");
		if (!active.hud_designations.empty()) {
			const auto &p = active.hud_designations[0];
			CHECK(p.team == 1 && p.x == 100 * 65536 && p.y == -50 * 65536 &&
							p.radius_q16 == 40 * 65536,
					"the HUD retains received team/point/radius");
		}
		CHECK(receive(3, 2).hud_designations.empty(), "a person marker is not a designation");
		CHECK(receive(1, 0).hud_designations.empty(), "an expired designation leaves the HUD feed");
	}
	entity.x = 30 << 16;
	view.tick_minimap_overlays();
	slot = find_slot(view.state().minimap, entity.handle);
	CHECK(slot != nullptr && slot->x == (100 << 16),
			"linked marker keeps the wire pose (the link only re-handles)");
	for (int i = 0; i < 2 * 62 + 1; ++i) view.tick_minimap_overlays();
	CHECK(find_slot(view.state().minimap, entity.handle) == nullptr,
			"link expiry clears its special slot");

	// A 0x6B type-3 (person) record picks icon 24.
	view.apply(opennova::s2c::MINIMAP_OVERLAY,
			linked_record(entity.handle, 1, 1, 0, 60, 3, 0));
	slot = find_slot(view.state().minimap, entity.handle);
	CHECK(slot != nullptr && slot->param == 24, "0x6B type 3 -> icon 24");

	// 0x6B for a range-valid handle with NO decoded entity: retail resolves
	// the fixed pool slot regardless, so the marker retains from the wire
	// pose with the neutral team color, and the link lives purely on its own
	// lifetime — the timer walk never consults the entity.
	// [orig: NapiNPClientMsg_0x06B @0x425573 gate; team read @0x5becb8;
	//  MapOverlay_UpdateTimers @0x5bfd3a..0x5bfe21]
	view.apply(opennova::s2c::MINIMAP_OVERLAY,
			linked_record(0x2044, 5, 6, 0, 3, 1, 12));
	slot = find_slot(view.state().minimap, 0x2044);
	CHECK(slot != nullptr && slot->param == 253 && !slot->entity_known,
			"0x6B retains a marker for a not-yet-decoded entity");
	if (slot != nullptr) {
		CHECK(slot->argb == 0xFF208020u,
				"an undecoded slot takes the neutral team color");
		CHECK(slot->x == (5 << 16) && slot->y == (6 << 16),
				"the undecoded marker pose still rides the wire record");
	}
	for (int i = 0; i < 3 * 62 - 1; ++i) view.tick_minimap_overlays();
	CHECK(find_slot(view.state().minimap, 0x2044) != nullptr,
			"the link survives without a decoded entity until it lapses");
	for (int i = 0; i < 2; ++i) view.tick_minimap_overlays();
	CHECK(find_slot(view.state().minimap, 0x2044) == nullptr,
			"link lifetime expiry still frees the slot");

	// A 0x40 special badge and a 0x6B pulse for the SAME handle coexist: the
	// link keys strictly off its STORED slot, and a fresh link allocates a
	// second special slot rather than stamping the badge (the allocator's
	// free test skips active slots).
	// [orig: link[6] @0x5bece4; MapOverlay_AllocSlot free test @0x5be9b0]
	view.state().upsert(0x2005).team = 2;
	view.apply(opennova::s2c::CAPTURE_ZONE_STATE, zone(0x2005, 12, 8, 0x40));
	view.apply(opennova::s2c::MINIMAP_OVERLAY,
			linked_record(0x2005, 9, 9, 0, 5, 1, 16));
	{
		int badge = 0, pulse = 0;
		for (const auto &s : view.state().minimap.special) {
			if (!s.active || s.handle != 0x2005 || s.remaining_ticks == 0)
				continue;
			if (s.param == 12) ++badge;
			if (s.param == 253) ++pulse;
		}
		CHECK(badge == 1 && pulse == 1,
				"a 0x40 badge and a 0x6B pulse hold separate special slots");
	}

	// A 0x20 clear writes ONLY lifetime + handle on the first found slot and
	// never touches the link table; the surviving link re-arms the slot on
	// the next timer tick and the marker RESURRECTS (the special draw walk
	// gates on lifetime alone — no handle test).
	// [orig: @0x5beb4b..0x5beb56; re-arm @0x5bfd61; draw gate @0x5be794]
	view.apply(opennova::s2c::MINIMAP_OVERLAY,
			linked_record(0x2006, 4, 4, 0, 30, 1, 8));
	const auto *probe = find_slot(view.state().minimap, 0x2006);
	CHECK(probe != nullptr, "the resurrect probe pulse is up");
	view.apply(opennova::s2c::CAPTURE_ZONE_STATE, zone(0x2006, 0, 0, 0x20));
	CHECK(probe != nullptr && probe->remaining_ticks == 0 &&
			probe->handle == 0xFFFF && probe->x == (4 << 16),
			"the clear zeroes lifetime + handle and keeps the pose fields");
	view.tick_minimap_overlays();
	CHECK(probe != nullptr && probe->remaining_ticks > 0,
			"the surviving 0x6B link re-arms its slot — the marker resurrects");
	CHECK(probe != nullptr && probe->handle == 0x2006,
			"the link walk restores the slot's handle each tick "
			"[orig: @0x5bfd95..0x5bfdc8]");
	// With the handle restored, a later 0x40 record for that handle updates
	// THIS slot in place instead of allocating a twin.
	view.apply(opennova::s2c::CAPTURE_ZONE_STATE, zone(0x2006, 12, 8, 0x40));
	{
		int slots_for_handle = 0;
		for (const auto &s2 : view.state().minimap.special)
			if (s2.active && s2.handle == 0x2006) ++slots_for_handle;
		CHECK(slots_for_handle == 1,
				"a repeat 0x40 after the clear finds the restored slot (one slot)");
	}

	// Handle gates include the witnessed per-pool capacity (pool 2 holds
	// 1200 slots): index 0xFFE drops in BOTH appliers before any write.
	// [orig: @0x5beade (0x40); the 0x6B decoder gate @0x425573..0x42559d;
	//  capacities EntityPool_Allocate @0x442168]
	view.apply(opennova::s2c::CAPTURE_ZONE_STATE, zone(0x2FFE, 10, 0x0A, 0));
	CHECK(find_slot(view.state().minimap, 0x2FFE) == nullptr,
			"an out-of-capacity 0x40 handle is dropped");
	view.apply(opennova::s2c::MINIMAP_OVERLAY,
			linked_record(0x2FFE, 1, 1, 0, 5, 1, 4));
	CHECK(find_slot(view.state().minimap, 0x2FFE) == nullptr,
			"an out-of-capacity 0x6B handle is dropped");

	const auto before = view.state().minimap.revision;
	const auto malformed_before = view.malformed_bodies();
	view.apply(opennova::s2c::CAPTURE_ZONE_STATE, {1, 1});
	CHECK(view.state().minimap.revision == before,
			"malformed 0x40 is atomic and leaves retained state unchanged");
	CHECK(view.malformed_bodies() == malformed_before + 1,
			"malformed known-tag bodies count apart from unknown tags");

	if (failures != 0) return 1;
	std::printf("client_minimap_overlay_test OK\n");
	return 0;
}
