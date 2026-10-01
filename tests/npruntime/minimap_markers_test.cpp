// The retained minimap marker rows (npruntime/minimap_markers.h): bank order,
// the policy resolve against the local entity, and the locally deployed
// player's restored regular row — appended only when no decoded regular row
// already covers its wire handle. [orig: Render_MinimapSlotBlip @0x5BE240;
// the regular TSDicon submit @0x597F73]
#include <cstdint>
#include <cstdio>
#include <unordered_map>
#include <vector>

#include <runtime/replication/client_state.h>
#include <runtime/inmatch/minimap_markers.h>
#include <runtime/hud/hud_minimap.h>
#include <runtime/world/entity.h>
#include <runtime/world/world.h>

using namespace opennova;

static int failures = 0;
#define CHECK(c)                                                                       \
    do {                                                                               \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

struct LocalWorld {
    world::World w;
    world::EntityHandle local;
    LocalWorld() {
        w.registry.configure_pool(0, 8);
        world::Entity seed;
        seed.kind = world::EntityKind::Organic;
        seed.item_id = 0x14B9;
        seed.net_id = 1;
        seed.position = {10.0f, 20.0f, 3.0f};
        seed.alive = true;
        seed.team = 1;
        seed.zone_number = 4;
        local = w.registry.spawn(0, seed);
        w.cached.local_player = local;
    }
    uint16_t wire_handle() const { return static_cast<uint16_t>(local.packed); }
};

replication::ClientMinimapOverlaySlot slot(uint16_t handle, uint8_t param, bool known) {
    replication::ClientMinimapOverlaySlot s;
    s.active = true;
    s.handle = handle;
    s.param = param;
    s.argb = 0xFF112233u;
    s.flags = 0x10;
    s.source = 2;
    s.x = 65536;
    s.y = 2 * 65536;
    s.z = 3 * 65536;
    s.heading_bam = 0x10000000;
    s.remaining_ticks = 31;
    s.entity_known = known;
    return s;
}

void test_banks_walk_in_order_and_the_local_row_is_restored() {
    LocalWorld lw;
    replication::ClientMinimapState map;
    map.transient[0] = slot(500, 2, false);
    map.persistent[1] = slot(501, 4, true);
    map.special[0] = slot(502, 24, false);
    inmatch::MinimapMarkerInputs in;
    in.map = &map;
    in.world = &lw.w;
    in.local_marker_handle = lw.wire_handle();
    in.local_heading_bam = 0x20000000;
    std::vector<hud::HudMinimapMarker> rows;
    inmatch::build_minimap_markers(in, rows);
    CHECK(rows.size() == 4);
    CHECK(rows[0].bank == static_cast<uint8_t>(hud::HudMinimapBank::kTransient));
    CHECK(rows[0].handle == 500);
    CHECK(rows[0].icon == 2);
    CHECK(rows[0].color == 0xFF112233u);
    CHECK(rows[0].remaining_ticks == 31);
    CHECK(rows[0].entity_known == 0);
    CHECK(rows[0].half_x_q16 == 0 && rows[0].half_y_q16 == 0); // no entity: unresolved fallback
    CHECK(rows[1].bank == static_cast<uint8_t>(hud::HudMinimapBank::kPersistent));
    CHECK(rows[1].handle == 501);
    CHECK(rows[1].entity_known == 1);
    CHECK(rows[2].bank == static_cast<uint8_t>(hud::HudMinimapBank::kSpecial));
    CHECK(rows[2].handle == 502);
    // The restored local row: regular persistent bank, cell 3, the team color,
    // zone as source, zero lifetime, resolved.
    const hud::HudMinimapMarker &me = rows[3];
    CHECK(me.bank == static_cast<uint8_t>(hud::HudMinimapBank::kPersistent));
    CHECK(me.handle == lw.wire_handle());
    CHECK(me.icon == 3);
    CHECK(me.flags == 0x10);
    CHECK(me.source == 4);
    CHECK(me.remaining_ticks == 0);
    CHECK(me.entity_known == 1);
    CHECK(me.heading_bam == 0x20000000);
    CHECK(me.x == 10 * 65536 && me.y == 20 * 65536 && me.z == 3 * 65536);
    CHECK(me.color == replication::minimap_team_argb(1));
    CHECK(me.medic == 0);
}

void test_a_decoded_regular_row_for_the_local_handle_suppresses_the_restore() {
    LocalWorld lw;
    replication::ClientMinimapState map;
    map.persistent[0] = slot(lw.wire_handle(), 3, true);
    inmatch::MinimapMarkerInputs in;
    in.map = &map;
    in.world = &lw.w;
    in.local_marker_handle = lw.wire_handle();
    std::vector<hud::HudMinimapMarker> rows;
    inmatch::build_minimap_markers(in, rows);
    CHECK(rows.size() == 1);
    CHECK(rows[0].handle == lw.wire_handle());
    // The decoded row resolves its policy against the live local entity.
    CHECK(rows[0].entity_known == 1);
    // An unresolved (entity_known false) regular row does NOT cover the handle.
    map.persistent[0].entity_known = false;
    inmatch::build_minimap_markers(in, rows);
    CHECK(rows.size() == 2);
    CHECK(rows[1].icon == 3);
    // A special-bank row never covers it either.
    map.persistent[0].active = false;
    map.special[0] = slot(lw.wire_handle(), 24, true);
    inmatch::build_minimap_markers(in, rows);
    CHECK(rows.size() == 2);
    CHECK(rows[0].bank == static_cast<uint8_t>(hud::HudMinimapBank::kSpecial));
    CHECK(rows[1].icon == 3);
}

void test_no_session_and_no_world() {
    LocalWorld lw;
    inmatch::MinimapMarkerInputs in;
    in.world = &lw.w;
    in.local_marker_handle = lw.wire_handle();
    std::vector<hud::HudMinimapMarker> rows;
    inmatch::build_minimap_markers(in, rows); // no session: only the local row
    CHECK(rows.size() == 1);
    CHECK(rows[0].icon == 3);
    in.local_marker_handle = world::EntityHandle::kInvalid; // no wire identity: nothing
    inmatch::build_minimap_markers(in, rows);
    CHECK(rows.empty());
    replication::ClientMinimapState map;
    map.transient[0] = slot(500, 2, true);
    in.map = &map;
    in.world = nullptr; // no world: the map rows with unresolved policies, no local row
    in.local_marker_handle = 7;
    inmatch::build_minimap_markers(in, rows);
    CHECK(rows.size() == 1);
    CHECK(rows[0].handle == 500);
    CHECK(rows[0].half_x_q16 == 0 && rows[0].half_y_q16 == 0);
}

// v6: a vehicle bay's row carries the attrib2 bay bit, its spawn families
// (ItemDef+0xAD8) and the live altitude the logo walk lifts.
// [orig: HUD_DrawVehicleBayLogos @0x5a2c00 -- def+0x58 bit 0 @0x5a2c9e,
//  def+0xAD8 @0x5a2d6f, entity+0xC @0x5a2d56]
void test_a_vehicle_bay_row_carries_the_logo_facts() {
    LocalWorld lw;
    world::Entity bay;
    bay.kind = world::EntityKind::Item;
    bay.item_id = 0x1500;
    bay.net_id = 2;
    bay.position = {30.0f, 40.0f, 5.5f};
    bay.alive = true;
    bay.team = 1;
    bay.has_item_def = true;
    bay.item_attrib2 = 1u;
    bay.vehicle_bay_flags = 2u;
    const world::EntityHandle bay_handle = lw.w.registry.spawn(0, bay);
    world::Entity plain = bay;
    plain.net_id = 3;
    plain.item_attrib2 = 0u;
    plain.vehicle_bay_flags = 0u;
    const world::EntityHandle plain_handle = lw.w.registry.spawn(0, plain);
    replication::ClientMinimapState map;
    map.persistent[0] = slot(static_cast<uint16_t>(bay_handle.packed), 19, true);
    map.persistent[1] = slot(static_cast<uint16_t>(plain_handle.packed), 10, true);
    inmatch::MinimapMarkerInputs in;
    in.map = &map;
    in.world = &lw.w;
    in.local_marker_handle = lw.wire_handle();
    std::vector<hud::HudMinimapMarker> rows;
    inmatch::build_minimap_markers(in, rows);
    CHECK(rows.size() == 3);
    CHECK((rows[0].entity_bits & hud::kMarkerEntityVehicleBay) != 0);
    CHECK(rows[0].bay_groups == 2);
    CHECK(rows[0].entity_z == static_cast<int32_t>(5.5 * 65536.0));
    CHECK((rows[1].entity_bits & hud::kMarkerEntityVehicleBay) == 0);
    CHECK(rows[1].bay_groups == 0);
}

// v7: a row whose handle carries a zone-timer entry (either S2C 0x6F or 0x53
// landed) takes the entry's image; the others, and every row without a
// session list, stay unknown.
// [orig: Render_CapturePointLabels @0x5a2840 --
//  CProximityList_FindEntryById(g_ZoneTimerList, entity) @0x5a292c]
void test_a_zone_row_carries_its_timer_entry() {
    LocalWorld lw;
    replication::ClientMinimapState map;
    map.transient[0] = slot(600, 12, true);
    map.persistent[0] = slot(601, 12, true);
    std::unordered_map<uint16_t, inmatch::ClientRuntime::ZoneState> timers;
    inmatch::ClientRuntime::ZoneState::Entry &entry = timers[600].entry;
    entry.mode_a = 1;
    entry.mode_b = 2;
    entry.value_current = 40;
    entry.value_limit = 62;
    entry.value_rate = -3;
    entry.value_active = true;
    inmatch::MinimapMarkerInputs in;
    in.map = &map;
    in.world = &lw.w;
    in.zone_timers = &timers;
    std::vector<hud::HudMinimapMarker> rows;
    inmatch::build_minimap_markers(in, rows);
    CHECK(rows.size() == 2);
    CHECK(rows[0].handle == 600 && rows[0].timer_known == 1 && rows[0].timer_active == 1);
    CHECK(rows[0].timer_team == 1 && rows[0].timer_bar_team == 2);
    CHECK(rows[0].timer_value == 40 && rows[0].timer_limit == 62 && rows[0].timer_rate == -3);
    CHECK(rows[1].handle == 601 && rows[1].timer_known == 0);
    in.zone_timers = nullptr;
    inmatch::build_minimap_markers(in, rows);
    CHECK(rows[0].timer_known == 0);
}

} // namespace

int main() {
    test_a_zone_row_carries_its_timer_entry();
    test_a_vehicle_bay_row_carries_the_logo_facts();
    test_banks_walk_in_order_and_the_local_row_is_restored();
    test_a_decoded_regular_row_for_the_local_handle_suppresses_the_restore();
    test_no_session_and_no_world();
    if (failures == 0) std::printf("minimap_markers_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
