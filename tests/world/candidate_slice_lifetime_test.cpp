// D-COL-19: a slot's candidate slice (+0x1BC / +0x1C0) lives in the entity's
// record. A spawn into the slot clears it with the rest of the record, so the
// new lifetime reads no slice until a build gives it one, between two builds as
// at one. A clone copies its template's record and walks the template's slice.
// A restart's reload re-spawns every row, so a row the build then skips has no
// slice.
// [orig: Entity_SpawnFromBMSRecord - the 0x2B4-byte memset @0x40EA27;
//  Entity_CloneFromTemplateByType @0x4398A0 - the copies @0x4398DC / @0x43997D;
//  Entity_InitArcMovement - the 0x310-byte copy @0x449BCC;
//  Entity_BuildProximityListsFromPools @0x4B8EB0 - the pool-1 gates
//  @0x4B9127..0x4B9147]

#include <runtime/world/collision.h>
#include <runtime/world/entity_registry.h>
#include <runtime/world/world.h>

#include <cstdio>
#include <memory>

using namespace opennova::world;

static int failures = 0;
#define CHECK(c) \
    do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

namespace {

// A pool-1 source the slice build takes (an item with a def, Flags bit 0
// clear), with a bound radius, so the pool-1 table holds it as a candidate.
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

void build_slices(CollisionWorld &collision, World &world) {
    for (int i = 0; i < 17; ++i) collision.build_tick_tables(world);
}

void test_a_new_lifetime_reads_no_slice_between_builds() {
    auto wp = std::make_unique<World>();
    World &world = *wp;
    world.registry.configure_pool(1, 8);
    CollisionWorld collision;
    const EntityHandle a = world.registry.spawn(1, item_seed(0.0f));
    const EntityHandle b = world.registry.spawn(1, item_seed(1.0f));
    build_slices(collision, world);
    CHECK(collision.candidate_count(a) == 1);
    CHECK(collision.candidate_count(b) == 1);
    // A new row into b's slot, no build since: it reads none.
    CHECK(world.registry.despawn(EntityLifetime{b, world.registry.get(b)->registry_spawn_id}));
    CHECK(world.registry.spawn_at(b, item_seed(1.0f)) == b);
    int32_t count = -1;
    CHECK(collision.candidate_slice(b, count) == nullptr && count == 0);
    CHECK(collision.candidate_count(b) == 0);
    CHECK(collision.candidate_count(a) == 1); // a's lifetime keeps its own
}

void test_a_clone_walks_its_templates_slice() {
    auto wp = std::make_unique<World>();
    World &world = *wp;
    world.registry.configure_pool(1, 8);
    CollisionWorld collision;
    const EntityHandle a = world.registry.spawn(1, item_seed(0.0f));
    const EntityHandle b = world.registry.spawn(1, item_seed(1.0f));
    build_slices(collision, world);
    int32_t source_count = 0;
    const EntityHandle *source_slice = collision.candidate_slice(a, source_count);
    CHECK(source_count == 1 && source_slice != nullptr && source_slice[0] == b);
    Entity copy = *world.registry.get(a);
    const EntityHandle clone = world.registry.spawn(1, copy);
    CHECK(clone.valid() && clone != a);
    CHECK(collision.candidate_count(clone) == 0); // no slice of its own yet
    collision.inherit_candidate_slice(a, *world.registry.get(clone));
    int32_t clone_count = 0;
    const EntityHandle *clone_slice = collision.candidate_slice(clone, clone_count);
    CHECK(clone_count == 1 && clone_slice == source_slice);
    // A template with no slice gives none.
    Entity bare = item_seed(500.0f);
    const EntityHandle lone = world.registry.spawn(1, bare);
    collision.inherit_candidate_slice(lone, *world.registry.get(clone));
    CHECK(collision.candidate_count(clone) == 0);
}

void test_a_restart_drops_a_skipped_rows_slice() {
    auto wp = std::make_unique<World>();
    World &world = *wp;
    world.registry.configure_pool(1, 8);
    CollisionWorld collision;
    world.collision = &collision;
    const EntityHandle a = world.registry.spawn(1, item_seed(0.0f));
    world.registry.spawn(1, item_seed(1.0f));
    build_slices(collision, world);
    CHECK(collision.candidate_count(a) == 1);
    // Carried: the build's Flags bit 0 gate skips it, so it keeps its slice.
    world.registry.get(a)->flags |= kEntityFlagCarried;
    build_slices(collision, world);
    CHECK(collision.candidate_count(a) == 1);
    const auto baseline = std::make_unique<World::Snapshot>(world.snapshot());
    world.restore(*baseline);
    CHECK((world.registry.get(a)->flags & kEntityFlagCarried) != 0);
    CHECK(collision.candidate_count(a) == 0);
}

} // namespace

int main() {
    test_a_new_lifetime_reads_no_slice_between_builds();
    test_a_clone_walks_its_templates_slice();
    test_a_restart_drops_a_skipped_rows_slice();
    if (failures == 0) std::puts("candidate_slice_lifetime: OK");
    return failures == 0 ? 0 : 1;
}
