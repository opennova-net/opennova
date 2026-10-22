// The zone HUD's client feeds (parity lane LH): the regular overlay slot the
// zone HUD reads a zone's capture bits through, the A&S radio context that
// reads the same slot, the zone status panel's rows and the takeover dialog's
// nearest entry.
// [orig: MapOverlay_FindRegularSlotByEntity @0x5BE870; HUD_DrawZoneStatusPanel
//  @0x5A2480; HUD_DrawZoneMarker @0x5986F0; Entity_FindNearestProximityEntity
//  @0x5380C0; Entity_FindNearestInProximityList @0x537F80]
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/loopback_channel.h>
#include <runtime/inmatch/role_feeds.h>
#include <runtime/inmatch/takeover_status_feed.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/replication/client_state.h>
#include <runtime/world/spawn_select.h>
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

// The S2C 0x53 body: [u16 handle][u8 modeA][u8 modeB][u16 start][u16 end][u8 rate],
// seconds [orig: NapiNPClientMsg_ZoneTimerWindow @0x428AE0].
std::vector<uint8_t> zone_timer_window_body(uint16_t handle, uint8_t mode_a, uint8_t mode_b,
		uint16_t start_s, uint16_t end_s, uint8_t rate) {
	return { static_cast<uint8_t>(handle), static_cast<uint8_t>(handle >> 8), mode_a, mode_b,
		static_cast<uint8_t>(start_s), static_cast<uint8_t>(start_s >> 8),
		static_cast<uint8_t>(end_s), static_cast<uint8_t>(end_s >> 8), rate };
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
	auto game = std::make_unique<world::World>();
	game->registry.configure_pool(0, 4);
	game->registry.configure_pool(2, 4);
	world::Entity zone;
	zone.has_item_def = true;
	zone.item_attrib = 0x40000u; // SpawnPoint
	zone.zone_number = 2;
	zone.zone_radius = 10;
	const world::EntityHandle zone_h = game->registry.spawn(2, zone);
	world::Entity speaker;
	const world::EntityHandle speaker_h = game->registry.spawn(0, speaker);
	// An active value entry short of its limit: 0.5 s of 1 s.
	loop.host_send(s2c::ZONE_TIMER_VALUE,
			zone_timer_value_body(zone_h.packed, 1, 0x8000, 0x10000, 1));
	runtime->Client_ProcessNetworkFrame();
	CHECK(runtime->zone_states().count(zone_h.packed) == 1);
	const world::Entity *talker = game->registry.get(speaker_h);
	CHECK(talker != nullptr);
	if (talker == nullptr) return;
	replication::ClientMinimapState &map = runtime->state().minimap;
	// No slot: not visible.
	CHECK(!inmatch::in_active_radio_zone(*game, *talker, *runtime));
	// The persistent row without capture bits: not visible.
	bank(map.persistent[0], zone_h.packed, 0x10, 0x02);
	CHECK(!inmatch::in_active_radio_zone(*game, *talker, *runtime));
	// The recipient's capture bit set: the speaker stands in the zone.
	map.persistent[0].source = 0x82;
	CHECK(inmatch::in_active_radio_zone(*game, *talker, *runtime));
	// The same bits on a special slot do not count.
	map.persistent[0] = replication::ClientMinimapOverlaySlot{};
	bank(map.special[0], zone_h.packed, 0x40, 0xC2);
	CHECK(!inmatch::in_active_radio_zone(*game, *talker, *runtime));
}

// The zone status panel's rows on a client: the capture bits from the zone's
// persistent row's source byte, the zone byte the panel groups by, and the
// Team byte as this client holds it (a joiner's replica row once a team store
// lands) [orig: HUD_DrawZoneStatusPanel @0x5A2517..0x5A2565 (`mov dl, [esi+4]`
//  @0x5A2562), `mov al, [eax+21Ah]` @0x5A24CD; HUD_DrawZoneMarker
//  `mov al, [ebp+162h]; movsx eax, al` @0x598734..0x59873A].
void panel_row_cases() {
	replication::LoopbackChannel loop;
	auto runtime = std::make_unique<inmatch::ClientRuntime>(loop);
	auto kernel = std::make_unique<mission::MissionKernel>();
	world::World &w = kernel->world;
	w.registry.configure_pool(0, 4);
	w.registry.configure_pool(2, 4);
	world::Entity me;
	me.kind = world::EntityKind::Organic;
	me.team = 1;
	w.cached.local_player = w.registry.spawn(0, me);
	world::Entity zone;
	zone.kind = world::EntityKind::Item;
	zone.has_item_def = true;
	zone.item_attrib = 0x40000u;
	zone.is_spawn_point = true;
	zone.alive = true;
	zone.team = 1;
	zone.zone_number = 2;
	zone.zone_radius = 10;
	const world::EntityHandle zone_h = w.registry.spawn(2, zone);
	world::SpawnZoneRegistry reg;
	reg.entries = { zone_h };
	loop.host_send(s2c::ZONE_TIMER_VALUE,
			zone_timer_value_body(zone_h.packed, 1, 0x8000, 0x10000, -1, 2, 1));
	runtime->Client_ProcessNetworkFrame();
	bank(runtime->state().minimap.persistent[0], zone_h.packed, 0x10, 0x82);
	inmatch::RoleView view;
	view.kernel = kernel.get();
	view.runtime = runtime.get();
	std::vector<hud::HudLfpZone> rows;
	CHECK(inmatch::collect_lfp_zones(view, reg, 1, rows) && rows.size() == 1);
	if (rows.size() == 1) {
		CHECK(rows[0].capture_flags == 0x82);
		CHECK(rows[0].zone_byte == 2 && rows[0].team == 1);
		CHECK(rows[0].timer_present && rows[0].timer_team == 1 && rows[0].rate == -1);
		CHECK(rows[0].count_owner == 2 && rows[0].count_other == 1);
	}
	// A joiner reads the team its replica row holds once a team store landed.
	replication::ClientEntityState row;
	row.handle = zone_h.packed;
	row.team = 2;
	row.team_known = true;
	runtime->state().entities.push_back(row);
	view.joiner = true;
	CHECK(inmatch::collect_lfp_zones(view, reg, 1, rows) && rows.size() == 1 &&
			rows[0].team == 2 && rows[0].zone_byte == 2);
	// The zone's row as a special slot carries no capture bits to the panel.
	runtime->state().minimap.persistent[0] = replication::ClientMinimapOverlaySlot{};
	bank(runtime->state().minimap.special[0], zone_h.packed, 0x40, 0x82);
	CHECK(inmatch::collect_lfp_zones(view, reg, 1, rows) && rows.size() == 1 &&
			rows[0].capture_flags == 0);
}

// The takeover dialog's nearest entry: filter 7 over the zone-timer image --
// a running entry, the player in the zone's cylinder, the zone's regular slot
// with its capture bits when the zone is a numbered SpawnPoint -- the nearest
// by the 2-D length [orig: Entity_FindNearestInProximityList @0x537F80, called
// with `push 7` @0x59B658 from HUD_DrawTakeoverStatus @0x59B630].
void takeover_feed_cases() {
	replication::LoopbackChannel loop;
	auto runtime = std::make_unique<inmatch::ClientRuntime>(loop);
	auto kernel = std::make_unique<mission::MissionKernel>();
	world::World &w = kernel->world;
	w.registry.configure_pool(0, 4);
	w.registry.configure_pool(2, 4);
	world::Entity me;
	me.kind = world::EntityKind::Organic;
	me.team = 1;
	w.cached.local_player = w.registry.spawn(0, me);
	world::Entity zone;
	zone.kind = world::EntityKind::Item;
	zone.has_item_def = true;
	zone.item_attrib = 0x40000u;
	zone.zone_radius = 10;
	zone.alive = true;
	// A numbered zone 5 units out, on the value channel.
	zone.zone_number = 2;
	zone.position = { 5.0f, 0.0f, 0.0f };
	const world::EntityHandle numbered = w.registry.spawn(2, zone);
	// An unnumbered building PSP 3 units out, its window running.
	zone.zone_number = 0;
	zone.item_type = 5;
	zone.position = { 3.0f, 0.0f, 0.0f };
	const world::EntityHandle psp = w.registry.spawn(2, zone);
	world::SpawnZoneRegistry reg;
	reg.entries = { numbered, psp };
	loop.host_send(s2c::ZONE_TIMER_VALUE,
			zone_timer_value_body(numbered.packed, 1, 0x8000, 0x10000, 2, 3, 1));
	loop.host_send(s2c::ZONE_TIMER_WINDOW, zone_timer_window_body(psp.packed, 1, 2, 4, 10, 1));
	runtime->Client_ProcessNetworkFrame();
	replication::ClientMinimapState &map = runtime->state().minimap;
	bank(map.persistent[0], numbered.packed, 0x10, 0x82);
	bank(map.persistent[1], psp.packed, 0x10, 0x00);
	inmatch::RoleView view;
	view.kernel = kernel.get();
	view.runtime = runtime.get();
	hud::HudTakeoverFacts facts;
	// The PSP is nearer: no capture bit is asked of an unnumbered zone.
	CHECK(inmatch::collect_takeover_status(view, reg, facts) && facts.present);
	CHECK(facts.zone_byte == 0 && facts.zone_index == 1 && facts.def_type == 5 &&
			facts.spawn_point_def && facts.window_active && !facts.value_active);
	CHECK(facts.distance_q16 == (3 << 16) && facts.team_a == 1 && facts.team_b == 2);
	// Without its map row the PSP drops out; the numbered zone answers through
	// its capture bits, with the contest bytes its 0x6F carried.
	map.persistent[1] = replication::ClientMinimapOverlaySlot{};
	CHECK(inmatch::collect_takeover_status(view, reg, facts) && facts.zone_byte == 2 &&
			facts.zone_index == 0 && facts.value_active && facts.value_rate == 2 &&
			facts.count_owner == 3 && facts.count_other == 1);
	// No capture bit on the numbered zone's row: nothing qualifies.
	map.persistent[0].source = 0x02;
	CHECK(!inmatch::collect_takeover_status(view, reg, facts) && !facts.present);
	map.persistent[0].source = 0x82;
	// Outside the cylinder's height (half the radius): nothing.
	w.registry.get(w.cached.local_player)->position = { 5.0f, 0.0f, 6.0f };
	CHECK(!inmatch::collect_takeover_status(view, reg, facts));
	w.registry.get(w.cached.local_player)->position = { 5.0f, 0.0f, 4.0f };
	CHECK(inmatch::collect_takeover_status(view, reg, facts) && facts.zone_byte == 2);
	// A value at its limit is not running: 1 s of a 1 s limit.
	loop.host_send(s2c::ZONE_TIMER_VALUE,
			zone_timer_value_body(numbered.packed, 1, 0x10000, 0x10000, 0, 0, 0));
	runtime->Client_ProcessNetworkFrame();
	map.persistent[0] = replication::ClientMinimapOverlaySlot{};
	bank(map.persistent[0], numbered.packed, 0x10, 0x82);
	CHECK(!inmatch::collect_takeover_status(view, reg, facts));
}

} // namespace

int main() {
	regular_slot_cases();
	radio_zone_slot_cases();
	panel_row_cases();
	takeover_feed_cases();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("zone_hud_feed_test OK\n");
	return 0;
}
