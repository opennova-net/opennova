// entity_reset_to_spawn_state — the gate-clear + position-backup half of the host
// player-spawn machine [orig: Entity_ResetToSpawnState @0x4B9610 / net-re §5.2b step 5].
#include <runtime/world/entity.h>
#include <runtime/world/entity_spawn.h>

#include <cstdio>

using namespace opennova::world;

static int failures = 0;
#define CHECK(c) \
    do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

int main() {
    // The movement gate (entity+36 bit 1) is cleared; the other Flags bits are preserved.
    {
        Entity e;
        e.flags = 0xF;                  // bits 0..3 set, including the gate bit 1
        entity_reset_to_spawn_state(e);
        CHECK((e.flags & 2u) == 0u);    // gate cleared
        CHECK(e.flags == 0xDu);         // only bit 1 cleared: 0xF & ~2 == 0xD
    }
    // The live Position is backed up into the spawn-point fields, and is itself untouched.
    {
        Entity e;
        e.position = {100.0f, 200.0f, 300.0f};
        e.spawn_position = {};
        entity_reset_to_spawn_state(e);
        CHECK(e.spawn_position.x == 100.0f);
        CHECK(e.spawn_position.y == 200.0f);
        CHECK(e.spawn_position.z == 300.0f);
        CHECK(e.position.x == 100.0f);  // live position unchanged
        CHECK(e.position.z == 300.0f);
    }
    // An already-clear gate stays clear (the clear is a single AND, idempotent on the bit).
    {
        Entity e;
        e.flags = 0x4;                  // gate bit already clear
        entity_reset_to_spawn_state(e);
        CHECK(e.flags == 0x4u);
    }
    // The classes whose definition callback is the organic init: the event-callback table's org0
    // and org1 rows, by a whole-tag stricmp [orig: g_EntityClassEventCallbackTable @0x813018 /
    // @0x813030].
    {
        CHECK(organic_init_class("org0") && organic_init_class("ORG1"));
        CHECK(!organic_init_class("org2") && !organic_init_class("org") && !organic_init_class("org0 ") &&
              !organic_init_class("plyr") && !organic_init_class("") && !organic_init_class(nullptr));
    }
    // A placed record's init facts: an `aidata` definition takes the waypoint as its route and
    // channel and folds Guarding (2) to Flags 0x40; without the AI slot, neither [orig:
    // Entity_SpawnFromBMSRecord @0x40ED4E, @0x40ED9F].
    {
        const OrganicSpawnFacts routed = organic_spawn_facts_from_record(true, 126, 0x2u | 0x4000u);
        CHECK(routed.route && routed.route_channel == 126 && routed.flags == kEntityFlagMounted);
        CHECK(!routed.rotor_wash && !routed.parented && routed.parent_phrase_set == 0);
        const OrganicSpawnFacts idle = organic_spawn_facts_from_record(true, 0, 0);
        CHECK(!idle.route && idle.route_channel == 0 && idle.flags == 0);
        const OrganicSpawnFacts no_slot = organic_spawn_facts_from_record(false, 126, 0x2u);
        CHECK(!no_slot.route && no_slot.route_channel == 0 && no_slot.flags == 0);
    }
    // The warmup settles a body standing under one unit [orig: @0x4B8BE7 cmp 10000h].
    CHECK(kOrganicWarmupSettleQ16 == 0x10000);
    std::printf("entity_spawn: %s\n", failures == 0 ? "OK" : "FAILED");
    return failures == 0 ? 0 : 1;
}
