// The map feed's entity facts and the non-bank legs' gather
// (inmatch/minimap_markers.h stamp_minimap_entity_facts,
// inmatch/minimap_overlays.h build_minimap_overlays): the v5 live-entity
// columns, the pool-3/pool-4 walks with the 2044 location ordinals, the
// tracked callout off the death-type-6 radio target, the loop-2 display
// names, and the WPNames / Overlays strings.
// [orig: HUD_DrawMapOverlay @0x5a7504 / @0x5a770b; Entity_SpawnFromBMSRecord
//  @0x40F180; HUD_SetTrackedEntityTarget @0x59D050; Entity_GetDisplayName
//  @0x59BF70]
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <net/npwire/entity_class.h>
#include <runtime/hud/hud_minimap.h>
#include <runtime/inmatch/minimap_markers.h>
#include <runtime/inmatch/minimap_overlays.h>
#include <runtime/replication/client_state.h>
#include <runtime/world/entity.h>
#include <runtime/world/world.h>

using namespace opennova;

static int failures = 0;
#define CHECK(c)                                                                       \
    do {                                                                               \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

struct MapWorld {
    world::World w;
    world::EntityHandle local;
    MapWorld() {
        for (int pool = 0; pool < 5; ++pool) w.registry.configure_pool(pool, 8);
        world::Entity me;
        me.kind = world::EntityKind::Organic;
        me.has_item_def = true;
        me.item_type = 3;
        me.item_type_index = 1;
        me.team = 1;
        me.position = {0.0f, 0.0f, 0.0f};
        local = w.registry.spawn(0, me);
        w.cached.local_player = local;
    }
};

void test_entity_facts_are_stamped() {
    MapWorld mw;
    world::Entity zone;
    zone.has_item_def = true;
    zone.item_type = 5;
    zone.item_type_index = 3;
    zone.item_attrib = 0x40000u;
    zone.team = 2;
    zone.zone_number = 3;
    zone.zone_radius = 70;
    zone.has_graphic_model = true;
    zone.bound_radius = 12.5f;
    zone.position = {100.0f, -40.0f, 5.0f};
    zone.bbox_center = {0.0f, 0.0f, 0.0f};
    const world::EntityHandle zh = mw.w.registry.spawn(2, zone);
    hud::HudMinimapMarker m;
    inmatch::stamp_minimap_entity_facts(m, *mw.w.registry.get(zh),
                                        mw.w.registry.get(mw.local), nullptr);
    CHECK(m.team == 2 && m.zone_number == 3 && m.zone_radius == 70 && m.def_type == 5);
    CHECK((m.entity_bits & hud::kMarkerEntityHasModel) != 0);
    CHECK((m.entity_bits & hud::kMarkerEntityZoneDef) != 0);
    CHECK((m.entity_bits & hud::kMarkerEntityHud) == 0);
    CHECK(m.entity_x == 100 * 65536 && m.entity_y == -40 * 65536);
    CHECK(m.anchor_x == m.entity_x && m.anchor_y == m.entity_y); // zero bbox centre
    CHECK(m.bound_radius_q16 == static_cast<int32_t>(12.5 * 65536));
    CHECK(m.zone_index == -1); // no zone list
    // A bbox centre rotates with the placement: the anchor keeps its length.
    mw.w.registry.get(zh)->bbox_center = {3.0f, 4.0f, 0.0f};
    inmatch::stamp_minimap_entity_facts(m, *mw.w.registry.get(zh),
                                        mw.w.registry.get(mw.local), nullptr);
    const double dx = (m.anchor_x - m.entity_x) / 65536.0;
    const double dy = (m.anchor_y - m.entity_y) / 65536.0;
    CHECK(std::fabs(std::hypot(dx, dy) - 5.0) < 0.01);
}

void test_overlays_gather() {
    MapWorld mw;
    const auto spawn_marker = [&](int pool, int32_t id, float x) {
        world::Entity e;
        e.has_item_def = true;
        e.item_id = id;
        e.item_type_index = 9;
        e.position = {x, 0.0f, 0.0f};
        e.bound_radius = 2.0f;
        return mw.w.registry.spawn(pool, e);
    };
    spawn_marker(3, 2044, 10.0f);
    spawn_marker(3, 6027, 20.0f);
    spawn_marker(3, 2044, 30.0f);
    const world::EntityHandle wp = spawn_marker(4, 6089, 40.0f);
    mw.w.registry.get(wp)->display_name = "Rally";
    // A same-team person in the transient bank, unnamed -> the fallback.
    world::Entity buddy;
    buddy.has_item_def = true;
    buddy.item_type = 3;
    buddy.item_type_index = 1;
    buddy.team = 1;
    const world::EntityHandle bh = mw.w.registry.spawn(0, buddy);

    replication::ClientState cs;
    cs.location_names = {"North Hill", "South Hill"};
    cs.spawn_hold_seconds = 7;
    cs.minimap.transient[0].active = true;
    cs.minimap.transient[0].handle = bh.packed;
    cs.tracked_target.handle = bh.packed;
    cs.tracked_target.ticks_remaining = 1860;
    cs.tracked_target.position[0] = 5 * 65536;
    cs.tracked_target.friendly = true;
    cs.tracked_target.color = 0xFF80A0FFu;
    cs.tracked_target.serial = 2;

    inmatch::MinimapOverlayInputs in;
    in.client = &cs;
    in.world = &mw.w;
    in.game_type = 0x10010;
    in.gametext = [](const char *section, const char *key, const char *) {
        return std::string(section) + "/" + key;
    };
    hud::HudMinimapOverlays out;
    inmatch::build_minimap_overlays(in, out);
    CHECK(out.game_type == 0x10010 && out.zone_timer == 7);
    CHECK(out.hud_present && out.hud_team == 1 && out.hud_handle == mw.local.packed);
    CHECK(out.location_names.size() == 2);
    CHECK(out.pool3.size() == 3);
    if (out.pool3.size() == 3) {
        CHECK(out.pool3[0].def_id == 2044 && out.pool3[0].location_index == 0);
        CHECK(out.pool3[1].def_id == 6027);
        CHECK(out.pool3[2].def_id == 2044 && out.pool3[2].location_index == 1);
        CHECK(out.pool3[0].radius_q16 == 2 * 65536);
    }
    CHECK(out.player_waypoints.size() == 1 && out.player_waypoints[0].name == "Rally");
    CHECK(out.objective_point_format == "Overlays/STROVER_OBJECTIVEPOINT_SHORT");
    CHECK(out.tracked.ticks == 1860 && out.tracked.serial == 2 && out.tracked.friendly);
    CHECK(out.tracked.live_known && out.tracked.snap_x == 5 * 65536);
    // ED0 is the colour stored at the set [orig: @0x59d0ef..0x59d0ff].
    CHECK(out.tracked.set_color == 0xFF80A0FFu);
    CHECK(out.names.size() == 1 && out.names[0].handle == bh.packed &&
          out.names[0].text.size() > 1 && out.names[0].text[0] == '^');
    // The rules word's 0x400 bit nulls every display name.
    in.rules_word = 0x400;
    inmatch::build_minimap_overlays(in, out);
    CHECK(out.names.empty());
}

// Bit 5 loop 1 over the S2C 0x4C table: every entry's live slot and the
// entity the slot drives; the own-slot test on the entry's entity; the name
// with the registry-tag wrap; the KOTH delta off the 0x16 team rows; and
// Entity_GetDisplayName's player arm through the same table.
// [orig: HUD_DrawEntityLabelsAndMarkers @0x5a4a54..0x5a4eb4; Minimap_DrawKothZoneRing
//  @0x5974E0; Entity_GetDisplayName @0x59C07A..0x59C0F9]
void test_player_slot_table_feeds_loop_one() {
    MapWorld mw;
    world::World &w = mw.w;
    w.registry.get(mw.local)->flags |= world::kEntityFlagPlayer;
    w.registry.get(mw.local)->display_name = "Me";
    world::Entity mate;
    mate.kind = world::EntityKind::Organic;
    mate.has_item_def = true;
    mate.item_type = 3;
    mate.item_type_index = 1;
    mate.team = 1;
    mate.flags = world::kEntityFlagPlayer | world::kEntityFlagDead;
    mate.position = {12.0f, 8.0f, 0.0f};
    mate.radio_request = 1;
    const world::EntityHandle mh = w.registry.spawn(0, mate);

    replication::ClientState cs;
    cs.roster[0].bound = true;
    cs.roster[0].name = "Me";
    cs.roster[0].entity_slot = static_cast<int16_t>(mw.local.slot());
    cs.roster[3].bound = true;
    cs.roster[3].name = "Mate";
    cs.roster[3].registry_clan = "TAG";
    cs.roster[3].entity_slot = static_cast<int16_t>(mh.slot());
    cs.roster[3].downed_revive_seconds = 40;
    cs.roster[3].medic_request_active = true;
    cs.roster[3].squad_color = 5;
    cs.visible_players.push_back({0, mw.local.packed});
    cs.visible_players.push_back({3, mh.packed});
    // An entry whose slot the roster no longer holds is skipped (slot+0x0D).
    cs.visible_players.push_back({9, 0x0007});
    cs.scoreboard.teams.resize(3);
    cs.scoreboard.teams[1].score1 = 12;
    cs.scoreboard.teams[2].score1 = -3;

    inmatch::MinimapOverlayInputs in;
    in.client = &cs;
    in.world = &w;
    hud::HudMinimapOverlays out;
    inmatch::build_minimap_overlays(in, out);
    CHECK(out.zone_score_delta == 15);
    CHECK(!out.own_revive_profile);
    CHECK(out.player_slots.size() == 2);
    if (out.player_slots.size() == 2) {
        const hud::HudMinimapPlayerSlot &own = out.player_slots[0];
        CHECK(own.active && own.own_slot && own.name_slot && own.name == "Me");
        CHECK(own.blip.handle == mw.local.packed && own.blip.entity_known);
        const hud::HudMinimapPlayerSlot &other = out.player_slots[1];
        CHECK(other.active && !other.own_slot);
        CHECK(other.revivable && other.medic_request && other.radio_request);
        CHECK(other.squad == 5);
        CHECK(other.name_slot && other.name == "Mate" && other.clan == "TAG");
        CHECK((other.blip.entity_bits & hud::kMarkerEntityDead) != 0);
        CHECK(other.blip.entity_x == 12 * 65536 && other.blip.team == 1);
    }
    // The own-slot test reads the table entry's entity, not the slot's.
    cs.visible_players[0].entity_handle = 0xFFFF;
    inmatch::build_minimap_overlays(in, out);
    CHECK(out.player_slots.size() == 2 && !out.player_slots[0].own_slot);

    // Entity_GetDisplayName: a player in the transient bank names its table
    // slot with " <ch>" tag "<co>"; a player no table slot drives is null.
    w.registry.get(mh)->flags &= ~world::kEntityFlagDead;
    cs.minimap.transient[0].active = true;
    cs.minimap.transient[0].handle = mh.packed;
    inmatch::build_minimap_overlays(in, out);
    CHECK(out.names.size() == 1 && out.names[0].text == "Mate <ch>TAG<co>");
    cs.visible_players.pop_back();
    cs.visible_players.pop_back();
    inmatch::build_minimap_overlays(in, out);
    CHECK(out.names.empty());
}

// A joiner's slot entity is a decoded row: the blip comes off the row.
void test_joiner_rows_feed_loop_one() {
    MapWorld mw;
    replication::ClientState cs;
    replication::ClientEntityState &row = cs.upsert(0x0004);
    row.type_id = 0x14B9;
    row.cls = EntityClass::Player;
    row.team = 1;
    row.x = 30 << 16;
    row.y = -2 << 16;
    row.heading_bam = 0x40000000;
    cs.roster[2].bound = true;
    cs.roster[2].name = "Row";
    cs.roster[2].entity_slot = 4;
    cs.visible_players.push_back({2, 0x0004});
    inmatch::MinimapOverlayInputs in;
    in.client = &cs;
    in.world = &mw.w;
    in.self_handle = 0x0009; // the joiner's own wire handle
    hud::HudMinimapOverlays out;
    inmatch::build_minimap_overlays(in, out);
    CHECK(out.player_slots.size() == 1);
    if (out.player_slots.size() == 1) {
        const hud::HudMinimapPlayerSlot &s = out.player_slots[0];
        CHECK(s.active && !s.own_slot && s.name == "Row");
        CHECK(s.blip.entity_known && s.blip.def_type == 3 && s.blip.rotate);
        CHECK(s.blip.entity_x == 30 * 65536 && s.blip.anchor_y == -2 * 65536);
        CHECK(s.blip.heading_bam == 0x40000000);
        CHECK(s.blip.half_x_q16 == 0x20000);
    }
}

} // namespace

int main() {
    test_entity_facts_are_stamped();
    test_overlays_gather();
    test_player_slot_table_feeds_loop_one();
    test_joiner_rows_feed_loop_one();
    if (failures != 0) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("minimap_overlays: all passed\n");
    return 0;
}
