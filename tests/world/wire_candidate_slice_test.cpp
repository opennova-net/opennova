// D-COL-20: a joiner's wire-keyed candidate slices follow retail's client build over its
// wire-built pools. A pool-0 row sources a slice behind its in-use word alone, so a hidden body
// (Flags bit 0), which joins no person table, still has one. A pool-1 row the source gates skip
// (Flags bit 0) keeps the slice it has, across the builds that skip it. A spawn into a handle
// clears the slice with the rest of the record, and a row that leaves takes its slice with it.
// [orig: Entity_BuildProximityListsFromPools @0x4B8EB0 - the fill @0x4B8ED0, the pool-0 test
//  @0x4B8EF9..0x4B8F00, the pool-1 gates @0x4B9127..0x4B9147; Entity_BuildProximityLists_Pool01
//  @0x4B9340 - the person table's bit-0 skip @0x4B93E4; NapiNPClientMsg_0x00C @0x42E730 - the
//  memset @0x42E7E9..0x42E7F1; NapiNPClientMsg_0x00D @0x432D23]

#include <runtime/world/collision.h>
#include <runtime/world/entity_registry.h>
#include <runtime/world/world.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include "../common/title_fixture.h"

using namespace opennova::world;

static int failures = 0;
#define CHECK(c) \
    do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

namespace {

int32_t fx(double v) { return static_cast<int32_t>(v * 65536.0); }

// A registry pool-1 item the dynamic table holds: every decoded source near it walks it.
Entity item_seed(float x) {
    Entity seed;
    seed.kind = EntityKind::Item;
    seed.has_item_def = true;
    seed.item_type_index = 7;
    seed.item_type = 6;
    seed.position = {x, 0.0f, 0.0f};
    seed.bound_radius = 1.0f;
    seed.alive = true;
    return seed;
}

// A decoded pool-1 mover the source gates take, 0.5 u off the item.
WireDynamicCollisionProxy mover(uint16_t handle, uint32_t lifetime) {
    WireDynamicCollisionProxy proxy;
    proxy.wire_handle = handle;
    proxy.lifetime = lifetime;
    proxy.position_q16 = FixedVec3{fx(0.5), 0, 0};
    proxy.bound_radius_q16 = fx(1.0);
    proxy.candidate_source_eligible = true;
    return proxy;
}

WireSliceRow skipped_row(uint16_t handle, uint32_t lifetime) {
    WireSliceRow row;
    row.wire_handle = handle;
    row.lifetime = lifetime;
    return row;
}

struct Rig {
    std::unique_ptr<World> wp = std::make_unique<World>();
    bool wp_jo = test_titles::jo_world(*wp);
    World &world = *wp;
    CollisionWorld collision;
    Rig() {
        world.registry.configure_pool(1, 8);
        world.registry.spawn(1, item_seed(0.0f));
    }
};

// A hidden pool-0 body joins no person table, yet sources a slice at its position.
void test_a_hidden_pool0_row_sources_a_slice() {
    Rig rig;
    WireSliceRow hidden = skipped_row(0x0007, 1);
    hidden.pool0_source = true;
    hidden.position_q16 = FixedVec3{fx(0.5), 0, 0};
    hidden.bound_radius_q16 = fx(1.0);
    WireSliceRow far_away = hidden;
    far_away.wire_handle = 0x0009;
    far_away.position_q16 = FixedVec3{fx(500.0), 0, 0};
    rig.collision.replace_wire_collision_proxies({}, {}, 0xFFFF, {far_away, hidden});
    rig.collision.build_initial_tables(rig.world);
    CHECK(rig.collision.wire_candidate_count(0x0007) == 1);
    CHECK(rig.collision.wire_person_proxy(0x0007) == nullptr);
    CHECK(rig.collision.wire_candidate_count(0x0009) == 0);
    // A pool-1 row the tables pass over sources none.
    Rig other;
    WireSliceRow carried = hidden;
    carried.wire_handle = 0x1007;
    carried.pool0_source = false;
    other.collision.replace_wire_collision_proxies({}, {}, 0xFFFF, {carried});
    other.collision.build_initial_tables(other.world);
    CHECK(other.collision.wire_candidate_count(0x1007) == 0);
}

// A pool-1 mover that becomes carried (Flags bit 0: no proxy, a skipped row) keeps its slice
// between the builds and across a build that skips it.
void test_a_skipped_pool1_row_keeps_its_slice() {
    Rig rig;
    rig.collision.replace_wire_collision_proxies({}, {mover(0x1007, 3)});
    rig.collision.build_initial_tables(rig.world);
    CHECK(rig.collision.wire_candidate_count(0x1007) == 1);
    rig.collision.replace_wire_collision_proxies({}, {}, 0xFFFF, {skipped_row(0x1007, 3)});
    CHECK(rig.collision.wire_candidate_count(0x1007) == 1);
    rig.collision.rebuild_candidate_slices(rig.world);
    CHECK(rig.collision.wire_candidate_count(0x1007) == 1);
}

// A spawn into the handle (a new lifetime) holds no slice until a build gives it one, and a row
// that left holds none.
void test_a_new_lifetime_or_a_gone_row_holds_none() {
    Rig rig;
    rig.collision.replace_wire_collision_proxies({}, {mover(0x1007, 3)});
    rig.collision.build_initial_tables(rig.world);
    CHECK(rig.collision.wire_candidate_count(0x1007) == 1);
    rig.collision.replace_wire_collision_proxies({}, {}, 0xFFFF, {skipped_row(0x1007, 4)});
    CHECK(rig.collision.wire_candidate_count(0x1007) == 0);
    rig.collision.rebuild_candidate_slices(rig.world);
    CHECK(rig.collision.wire_candidate_count(0x1007) == 0);
    rig.collision.replace_wire_collision_proxies({}, {mover(0x1007, 4)});
    rig.collision.rebuild_candidate_slices(rig.world);
    CHECK(rig.collision.wire_candidate_count(0x1007) == 1);
    rig.collision.replace_wire_collision_proxies({}, {});
    CHECK(rig.collision.wire_candidate_count(0x1007) == 0);
}

} // namespace

int main() {
    test_a_hidden_pool0_row_sources_a_slice();
    test_a_skipped_pool1_row_keeps_its_slice();
    test_a_new_lifetime_or_a_gone_row_holds_none();
    if (failures == 0) std::puts("wire_candidate_slice: OK");
    return failures == 0 ? 0 : 1;
}
