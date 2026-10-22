// The zone HUD's client feeds (parity lane LH): the regular overlay slot the
// zone HUD reads a zone's capture bits through, and the A&S radio context that
// reads the same slot.
// [orig: MapOverlay_FindRegularSlotByEntity @0x5BE870; HUD_DrawZoneStatusPanel
//  @0x5A2480; Entity_FindNearestProximityEntity @0x5380C0]
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/loopback_channel.h>
#include <runtime/replication/client_state.h>
#include <runtime/world/entity.h>
#include <runtime/world/world.h>

#include <net/npwire/ingame_message_id.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

using namespace opennova;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

// The S2C 0x6F body: [u16 handle][u8 mode][i32 value][i32 limit][i16 rate][u8][u8],
// value and limit in 16.16 seconds [orig: NapiNPClientMsg_ZoneTimerValue @0x428D60].
std::vector<uint8_t> zone_timer_value_body(uint16_t handle, uint8_t mode, int32_t value_s,
		int32_t limit_s, int16_t rate, uint8_t owner = 0, uint8_t other = 0) {
	std::vector<uint8_t> body;
	const auto u16 = [&](uint16_t v) {
		body.push_back(static_cast<uint8_t>(v));
		body.push_back(static_cast<uint8_t>(v >> 8));
	};
	const auto u32 = [&](uint32_t v) {
		for (int i = 0; i < 4; ++i) body.push_back(static_cast<uint8_t>(v >> (8 * i)));
	};
	u16(handle);
	body.push_back(mode);
	u32(static_cast<uint32_t>(value_s));
	u32(static_cast<uint32_t>(limit_s));
	u16(static_cast<uint16_t>(rate));
	body.push_back(owner);
	body.push_back(other);
	return body;
}

void bank(replication::ClientMinimapOverlaySlot &slot, uint16_t handle, uint8_t flags,
		uint8_t source) {
	slot.active = true;
	slot.handle = handle;
	slot.flags = flags;
	slot.source = source;
}

// The walk is the 1160 slots in address order (transient, persistent, special),
// skipping a free handle and a special (flags 0x40) slot, whatever the bank
// [orig: MapOverlay_FindRegularSlotByEntity @0x5BE870 -- `cmp cx, 0FFFFh`
//  @0x5BE883, `test byte [eax+3], 40h` @0x5BE88A, stride 0x20 @0x5BE8B5].
void regular_slot_cases() {
	auto map = std::make_unique<replication::ClientMinimapState>();
	constexpr uint16_t kZone = 0x2003;
	CHECK(replication::find_regular_overlay_slot(*map, kZone) == nullptr);
	// A special slot (0x40) never answers, whatever its source.
	bank(map->special[0], kZone, 0x40, 0xC0);
	CHECK(replication::find_regular_overlay_slot(*map, kZone) == nullptr);
	// A SpawnPoint row rides the persistent bank (flags 0x10); its source byte
	// carries the capture bits.
	bank(map->persistent[4], kZone, 0x10, 0x82);
	const replication::ClientMinimapOverlaySlot *slot =
			replication::find_regular_overlay_slot(*map, kZone);
	CHECK(slot == &map->persistent[4] && slot->source == 0x82);
	// A transient slot comes first in address order.
	bank(map->transient[7], kZone, 0x00, 0x01);
	CHECK(replication::find_regular_overlay_slot(*map, kZone) == &map->transient[7]);
	// A special-bank slot without the 0x40 bit is a regular slot to the walk.
	map->transient[7] = replication::ClientMinimapOverlaySlot{};
	map->persistent[4] = replication::ClientMinimapOverlaySlot{};
	bank(map->special[9], kZone, 0x00, 0x40);
	CHECK(replication::find_regular_overlay_slot(*map, kZone) == &map->special[9]);
	// A cleared slot (handle -1) and another handle never answer.
	map->special[9].handle = 0xFFFF;
	bank(map->persistent[0], 0x2004, 0x10, 0x80);
	CHECK(replication::find_regular_overlay_slot(*map, kZone) == nullptr);
}

// The A&S radio context's nearest capture entry: a numbered SpawnPoint zone
// counts only through its regular slot's source capture bits, and a zone's row
// rides the persistent bank [orig: Entity_FindNearestProximityEntity @0x5380C0 --
//  MapOverlay_FindRegularSlotByEntity (the call @0x538197), `test [edx+54h], 40000h`
//  @0x5381A8, `cmp [ecx+21Ah], 0` @0x5381B1, `test byte [eax+4], 0C0h` @0x5381BA;
//  Server_BuildOverlayStateForPlayer's flags 0x10 on a SpawnPoint row].
void radio_zone_slot_cases() {
	replication::LoopbackChannel loop;
	auto runtime = std::make_unique<inmatch::ClientRuntime>(loop);
	auto world = std::make_unique<world::World>();
	world->registry.configure_pool(0, 4);
	world->registry.configure_pool(2, 4);
	world::Entity zone;
	zone.has_item_def = true;
	zone.item_attrib = 0x40000u; // SpawnPoint
	zone.zone_number = 2;
	zone.zone_radius = 10;
	const world::EntityHandle zone_h = world->registry.spawn(2, zone);
	world::Entity speaker;
	const world::EntityHandle speaker_h = world->registry.spawn(0, speaker);
	// An active value entry short of its limit: 0.5 s of 1 s.
	loop.host_send(s2c::ZONE_TIMER_VALUE,
			zone_timer_value_body(zone_h.packed, 1, 0x8000, 0x10000, 1));
	runtime->Client_ProcessNetworkFrame();
	CHECK(runtime->zone_states().count(zone_h.packed) == 1);
	const world::Entity *talker = world->registry.get(speaker_h);
	CHECK(talker != nullptr);
	if (talker == nullptr) return;
	replication::ClientMinimapState &map = runtime->state().minimap;
	// No slot: not visible.
	CHECK(!inmatch::in_active_radio_zone(*world, *talker, *runtime));
	// The persistent row without capture bits: not visible.
	bank(map.persistent[0], zone_h.packed, 0x10, 0x02);
	CHECK(!inmatch::in_active_radio_zone(*world, *talker, *runtime));
	// The recipient's capture bit set: the speaker stands in the zone.
	map.persistent[0].source = 0x82;
	CHECK(inmatch::in_active_radio_zone(*world, *talker, *runtime));
	// The same bits on a special slot do not count.
	map.persistent[0] = replication::ClientMinimapOverlaySlot{};
	bank(map.special[0], zone_h.packed, 0x40, 0xC2);
	CHECK(!inmatch::in_active_radio_zone(*world, *talker, *runtime));
}

} // namespace

int main() {
	regular_slot_cases();
	radio_zone_slot_cases();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("zone_hud_feed_test OK\n");
	return 0;
}
