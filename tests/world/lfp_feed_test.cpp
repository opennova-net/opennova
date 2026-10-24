// The AAS zone status panel feed: list order, the timer join, the viewer's
// distance/cylinder tests, and the contest counts.
// [orig: HUD_DrawZoneStatusPanel @0x5a2480; HUD_DrawZoneMarker @0x5986f0]
#include <runtime/world/entity.h>
#include <runtime/world/lfp_feed.h>
#include <runtime/world/spawn_select.h>
#include <runtime/world/world.h>

#include <cstdio>

using namespace opennova::world;

static int failures = 0;
#define CHECK(c) \
    do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

namespace {

EntityHandle spawn_zone(World &w, uint8_t zone_no, uint8_t team, Vec3 pos,
                        uint16_t radius, float bound_radius) {
    Entity e;
    e.kind = EntityKind::Item;
    e.item_id = 1359;
    e.has_item_def = true;
    e.position = pos;
    e.team = team;
    e.zone_number = zone_no;
    e.zone_radius = radius;
    e.bound_radius = bound_radius;
    e.is_capture_trigger = true;
    e.is_spawn_point = true;
    e.alive = true;
    return w.registry.spawn(1, e);
}

} // namespace

int main() {
    World w;
    w.registry.configure_pool(0, 16);
    w.registry.configure_pool(1, 16);
    w.registry.configure_pool(2, 16);
    // Two zones: A (team 1) near the viewer, B (team 2) 500 u away.
    const EntityHandle za = spawn_zone(w, 1, 1, {50.0f, 0.0f, 10.0f}, 70, 4.0f);
    const EntityHandle zb = spawn_zone(w, 2, 2, {500.0f, 0.0f, 10.0f}, 70, 4.0f);
    Entity viewer;
    viewer.kind = EntityKind::Organic;
    viewer.position = {0.0f, 0.0f, 10.0f};
    viewer.team = 1;
    viewer.alive = true;
    const EntityHandle vh = w.registry.spawn(0, viewer);

    const SpawnZoneRegistry reg = w.zones.build_spawn_zone_list();
    CHECK(reg.entries.size() == 2);

    // The timer lookup: A has an entry with contest bytes, B none.
    LfpZoneTimerLookup timer = [&](EntityHandle h, LfpZoneTimer &out) {
        if (h != za) return false;
        out.team = 1;
        out.value = 30;
        out.control = 60;
        out.limit = 60;
        out.rate = -2;
        out.active = true;
        out.count_owner = 3;
        out.count_other = 1;
        return true;
    };
    LfpCaptureFlagsLookup flags = [&](EntityHandle h) -> uint8_t {
        return h == za ? 0x40 : 0;
    };

    std::vector<opennova::hud::HudLfpZone> rows;
    build_lfp_zones(w, reg, *w.registry.get(vh), 1, timer, flags, {}, rows);
    CHECK(rows.size() == 2);
    if (rows.size() == 2) {
        // List order = registry order; letters follow the index.
        CHECK(rows[0].letter_index == 0 && rows[1].letter_index == 1);
        CHECK(rows[0].team == 1 && rows[1].team == 2);
        // The zone byte rides beside the team: the panel groups by it
        // [orig: HUD_DrawZoneStatusPanel `mov al, [eax+21Ah]` @0x5a24cd].
        CHECK(rows[0].zone_byte == 1 && rows[1].zone_byte == 2);
        // The timer join.
        CHECK(rows[0].timer_present && rows[0].rate == -2 && rows[0].control == 60);
        CHECK(rows[0].count_owner == 3 && rows[0].count_other == 1);
        CHECK(rows[0].capture_flags == 0x40);
        CHECK(!rows[1].timer_present && rows[1].capture_flags == 0);
        // In the cylinder: 50 u away with a 70 u radius, level -> inside;
        // 500 u away -> outside.
        CHECK(rows[0].in_cylinder);
        CHECK(!rows[1].in_cylinder);
        // The distance reads to the objective's EDGE (bound radius 4):
        // 50 - 4 = 46 m, 500 - 4 = 496 m.
        CHECK(rows[0].distance_m == 46);
        CHECK(rows[1].distance_m == 496);
    }
    // Standing inside the zone's bound radius floors the distance at zero.
    w.registry.get(vh)->position = {48.0f, 0.0f, 10.0f};
    build_lfp_zones(w, reg, *w.registry.get(vh), 1, timer, flags, {}, rows);
    CHECK(rows.size() == 2 && rows[0].distance_m == 0);
    // A height difference past half the radius leaves the cylinder.
    w.registry.get(vh)->position = {50.0f, 0.0f, 10.0f + 36.0f};
    build_lfp_zones(w, reg, *w.registry.get(vh), 1, timer, flags, {}, rows);
    CHECK(rows.size() == 2 && !rows[0].in_cylinder);
    // The Z test reads the Position words' delta (D-NET-507) [orig:
    // HUD_DrawZoneMarker @0x5986F0 (the Z test @0x5987D1..0x598810)]. The viewer stands on zone A's
    // X / Y words at Z word 19088745 (291.27 u, whose float view rounds down
    // to 19088744), the zone at 16794984 (exact as a float): the words lie
    // 2293761 apart, one past half the 70 u radius (2293760), so the viewer
    // is out, where the float views' delta reads the half exactly and let it
    // in. Integer words only, so every x87 length agrees (Q3-11).
    {
        Entity *v = w.registry.get(vh);
        Entity *a = w.registry.get(za);
        v->position.x.q16 = a->position.x.q16;
        v->position.y.q16 = a->position.y.q16;
        v->position.z.q16 = 19088745;
        a->position.z.q16 = 16794984;
        build_lfp_zones(w, reg, *v, 1, timer, flags, {}, rows);
        CHECK(rows.size() == 2 && !rows[0].in_cylinder);
        // One word nearer is inside: the half radius itself passes.
        v->position.z.q16 = 19088744;
        build_lfp_zones(w, reg, *v, 1, timer, flags, {}, rows);
        CHECK(rows.size() == 2 && rows[0].in_cylinder);
    }
    // The team lookup: the marker's Team byte as the client holds it (a
    // joiner's replica row), over the entity's own byte [orig:
    // HUD_DrawZoneMarker `mov al, [ebp+162h]; movsx eax, al` @0x598734..0x59873a].
    {
        const LfpZoneTeamLookup held = [&](const Entity &zone) {
            return zone.handle == zb ? 0 : static_cast<int>(zone.team);
        };
        build_lfp_zones(w, reg, *w.registry.get(vh), 1, timer, flags, held, rows);
        CHECK(rows.size() == 2 && rows[0].team == 1 && rows[1].team == 0 &&
              rows[1].zone_byte == 2);
    }

    if (failures != 0) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("lfp_feed_test OK\n");
    return 0;
}
